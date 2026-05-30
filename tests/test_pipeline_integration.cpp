// tests/test_pipeline_integration.cpp
// Integration tests: Parquet → LiquidCacheStore → Velox Vector pipeline.
// Covers:
//   - PipelineCorrectness: Parquet→load→read_batch_velox→value verification
//   - ReadSplitVelox: offset/length→RG mapping correctness
//   - Projection: column projection correctness
//   - MultiFile: multi-file isolation
//   - CacheMiss: missing key returns nullptr
//   - FSSTPath: compressor_states path correctness
// Only compiled when LIQUID_ENABLE_VELOX is defined.
#include <gtest/gtest.h>

#include <arrow/api.h>
#if ARROW_VERSION_MAJOR >= 19
#include <arrow/compute/initialize.h>
#endif

#include <arrow/io/file.h>
#include <arrow/table.h>
#include <parquet/arrow/writer.h>
#include <parquet/arrow/reader.h>

#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <string>
#include <vector>

#include "velox/common/memory/Memory.h"
#include "velox/type/HugeInt.h"
#include "velox/type/Timestamp.h"
#include "velox/vector/FlatVector.h"
#include "velox/vector/ComplexVector.h"

#include "liquid_cache/liquid_cache_store.h"
#include "liquid_cache/liquid_array.h"

using namespace liquid_cache;
using namespace facebook::velox;

// Shared memory pool — use Velox MemoryManager (initialized in main() from
// test_velox_crossval.cpp, which shares this binary).
static memory::MemoryPool* get_test_pool() {
    static auto pool = memory::memoryManager()->addLeafPool("pipeline_test_pool");
    return pool.get();
}
static memory::MemoryPool* test_pool() { return get_test_pool(); }

// ═══════════════════════════════════════════════════════════════════════
// Helpers: Arrow → Velox type conversion
// Replicates arrow_type_to_velox / arrow_schema_to_velox_row_type from
// liquid_to_velox.cpp (anonymous namespace).
// ═══════════════════════════════════════════════════════════════════════

static TypePtr arrow_type_to_velox_type(const std::shared_ptr<arrow::DataType>& at) {
    switch (at->id()) {
        case arrow::Type::INT8:    return TINYINT();
        case arrow::Type::INT16:   return SMALLINT();
        case arrow::Type::INT32:   return INTEGER();
        case arrow::Type::INT64:   return BIGINT();
        case arrow::Type::UINT8:   return TINYINT();
        case arrow::Type::UINT16:  return SMALLINT();
        case arrow::Type::UINT32:  return INTEGER();
        case arrow::Type::UINT64:  return BIGINT();
        case arrow::Type::FLOAT:   return REAL();
        case arrow::Type::DOUBLE:  return DOUBLE();
        case arrow::Type::STRING:  return VARCHAR();
        case arrow::Type::LARGE_STRING: return VARCHAR();
        case arrow::Type::BINARY:  return VARBINARY();
        case arrow::Type::LARGE_BINARY: return VARBINARY();
        case arrow::Type::DATE32:  return INTEGER();
        case arrow::Type::DATE64:  return INTEGER();
        case arrow::Type::TIMESTAMP: return TIMESTAMP();
        case arrow::Type::DECIMAL128: {
            auto dt = std::static_pointer_cast<arrow::Decimal128Type>(at);
            return DECIMAL(static_cast<uint8_t>(dt->precision()),
                           static_cast<uint8_t>(dt->scale()));
        }
        case arrow::Type::DECIMAL256: {
            auto dt = std::static_pointer_cast<arrow::Decimal256Type>(at);
            return DECIMAL(static_cast<uint8_t>(dt->precision()),
                           static_cast<uint8_t>(dt->scale()));
        }
        default:
            throw std::runtime_error("Unsupported Arrow type: " + at->ToString());
    }
}

static RowTypePtr arrow_schema_to_velox_row_type(
        const std::shared_ptr<arrow::Schema>& schema) {
    std::vector<std::string> names;
    std::vector<TypePtr> types;
    for (int i = 0; i < schema->num_fields(); ++i) {
        names.push_back(schema->field(i)->name());
        types.push_back(arrow_type_to_velox_type(schema->field(i)->type()));
    }
    return ROW(std::move(names), std::move(types));
}

// ═══════════════════════════════════════════════════════════════════════
// Helpers: Parquet file creation
// ═══════════════════════════════════════════════════════════════════════

/// Write one or more Arrow arrays to a Parquet file with explicit row groups.
/// Returns the file_id (hash of path).
static uint64_t WriteParquetFile(
        const std::string& path,
        const std::shared_ptr<arrow::Schema>& schema,
        const std::vector<std::shared_ptr<arrow::Array>>& columns,
        int rows_per_rg) {
    int total_rows = static_cast<int>(columns[0]->length());
    int num_rgs = (total_rows + rows_per_rg - 1) / rows_per_rg;

    // Build table
    auto table = arrow::Table::Make(schema, columns);

    auto maybe_out = arrow::io::FileOutputStream::Open(path);
    if (!maybe_out.ok()) {
        throw std::runtime_error("Cannot open: " + path);
    }
    auto out = maybe_out.ValueOrDie();

    auto writer_props = parquet::WriterProperties::Builder()
        .max_row_group_length(rows_per_rg)->build();
    auto arrow_props = parquet::ArrowWriterProperties::Builder().build();

    auto maybe_writer = parquet::arrow::FileWriter::Open(
        *schema, arrow::default_memory_pool(),
        out, writer_props, arrow_props);
    if (!maybe_writer.ok()) {
        throw std::runtime_error("Cannot create writer: " + path);
    }
    auto writer = std::move(maybe_writer).ValueOrDie();

    for (int rg = 0; rg < num_rgs; ++rg) {
        int64_t start = static_cast<int64_t>(rg) * rows_per_rg;
        int64_t len = std::min(static_cast<int64_t>(rows_per_rg),
                               total_rows - start);
        auto chunk = table->Slice(start, len);
        ARROW_CHECK_OK(writer->NewRowGroup(len));
        for (int c = 0; c < chunk->num_columns(); ++c) {
            ARROW_CHECK_OK(writer->WriteColumnChunk(*chunk->column(c)->chunk(0)));
        }
    }
    ARROW_CHECK_OK(writer->Close());
    ARROW_CHECK_OK(out->Close());

    return std::hash<std::string>{}(path);
}

// ═══════════════════════════════════════════════════════════════════════
// Helpers: data generation
// ═══════════════════════════════════════════════════════════════════════

#define APPEND(builder, val) do { auto _s = (builder).Append(val); (void)_s; } while(0)
#define APPEND_NULL(builder) do { auto _s = (builder).AppendNull(); (void)_s; } while(0)

/// Generate an int32 column: 0, 1, 2, ..., n-1
static std::shared_ptr<arrow::Array> GenInt32(int n, int null_pct = 0) {
    arrow::Int32Builder b;
    for (int i = 0; i < n; ++i) {
        if (null_pct > 0 && i % (100 / null_pct) == 0) APPEND_NULL(b);
        else APPEND(b, i);
    }
    return b.Finish().ValueOrDie();
}

/// Generate an int64 column: 0, 1000, 2000, ...
static std::shared_ptr<arrow::Array> GenInt64(int n) {
    arrow::Int64Builder b;
    for (int i = 0; i < n; ++i) APPEND(b, static_cast<int64_t>(i) * 1000);
    return b.Finish().ValueOrDie();
}

/// Generate a float64 column
static std::shared_ptr<arrow::Array> GenFloat64(int n) {
    arrow::DoubleBuilder b;
    for (int i = 0; i < n; ++i) APPEND(b, static_cast<double>(i) * 1.5);
    return b.Finish().ValueOrDie();
}

/// Generate a float32 column
static std::shared_ptr<arrow::Array> GenFloat32(int n) {
    arrow::FloatBuilder b;
    for (int i = 0; i < n; ++i) APPEND(b, static_cast<float>(i) * 0.75f);
    return b.Finish().ValueOrDie();
}

/// Generate a string column
static std::shared_ptr<arrow::Array> GenString(int n, int null_pct = 0) {
    arrow::StringBuilder b;
    for (int i = 0; i < n; ++i) {
        if (null_pct > 0 && i % (100 / null_pct) == 0) APPEND_NULL(b);
        else APPEND(b, "str_" + std::to_string(i));
    }
    return b.Finish().ValueOrDie();
}

/// Generate a string column with varied strings (good for FSST testing)
static std::shared_ptr<arrow::Array> GenStringVaried(int n) {
    arrow::StringBuilder b;
    for (int i = 0; i < n; ++i) {
        std::string s = "column_value_row_" + std::to_string(i) + "_data";
        APPEND(b, s);
    }
    return b.Finish().ValueOrDie();
}

/// Generate an all-null column
static std::shared_ptr<arrow::Array> GenAllNull(int n, arrow::Type::type type_id) {
    switch (type_id) {
        case arrow::Type::INT32: return arrow::MakeArrayOfNull(arrow::int32(), n).ValueOrDie();
        case arrow::Type::FLOAT: return arrow::MakeArrayOfNull(arrow::float32(), n).ValueOrDie();
        default: return arrow::MakeArrayOfNull(arrow::int64(), n).ValueOrDie();
    }
}

// ═══════════════════════════════════════════════════════════════════════
// Helpers: Velox vector → Arrow array comparison
// ═══════════════════════════════════════════════════════════════════════

/// Read a specific (rg_idx, col_idx) from a Parquet file as Arrow array.
static std::shared_ptr<arrow::Array> ReadParquetColumn(
        const std::string& path, int rg_idx, int col_idx) {
    auto maybe_in = arrow::io::ReadableFile::Open(path);
    if (!maybe_in.ok()) return nullptr;
    auto infile = maybe_in.ValueOrDie();

    std::unique_ptr<parquet::arrow::FileReader> reader;
#if ARROW_VERSION_MAJOR >= 19
    auto open_res = parquet::arrow::OpenFile(infile, arrow::default_memory_pool());
    if (!open_res.ok()) return nullptr;
    reader = std::move(open_res).ValueOrDie();
#else
    auto st = parquet::arrow::OpenFile(infile, arrow::default_memory_pool(), &reader);
    if (!st.ok()) return nullptr;
#endif

    std::shared_ptr<arrow::RecordBatchReader> rg_reader;
#if ARROW_VERSION_MAJOR >= 19
    auto rb_res = reader->GetRecordBatchReader({rg_idx});
    if (!rb_res.ok()) return nullptr;
    rg_reader = std::move(rb_res).ValueOrDie();
#else
    auto rb_st = reader->GetRecordBatchReader({rg_idx}, &rg_reader);
    if (!rb_st.ok()) return nullptr;
#endif

    // Collect all batches for this RG and concat the target column
    std::vector<std::shared_ptr<arrow::Array>> pieces;
    while (true) {
        std::shared_ptr<arrow::RecordBatch> batch;
        auto st = rg_reader->ReadNext(&batch);
        if (!st.ok() || !batch) break;
        pieces.push_back(batch->column(col_idx));
    }
    if (pieces.empty()) return nullptr;
    if (pieces.size() == 1) return pieces[0];

    // Concatenate
    arrow::ChunkedArray chunked(pieces);
    auto concat_res = arrow::Concatenate(chunked.chunks());
    if (!concat_res.ok()) return nullptr;
    return concat_res.ValueOrDie();
}

/// Compare an Int32 Velox FlatVector with an Arrow Int32Array over [start, start+n).
static void AssertInt32Matches(const VectorPtr& vec,
                                const std::shared_ptr<arrow::Array>& arrow_arr,
                                int64_t start, int64_t n) {
    auto flat = vec->asFlatVector<int32_t>();
    auto typed = std::static_pointer_cast<arrow::Int32Array>(arrow_arr);
    for (int64_t i = 0; i < n; ++i) {
        int64_t arrow_idx = start + i;
        bool arrow_null = arrow_arr->IsNull(arrow_idx);
        bool velox_null = flat->isNullAt(i);
        ASSERT_EQ(arrow_null, velox_null)
            << "Null mismatch at offset " << i << " (arrow_idx=" << arrow_idx << ")";
        if (!arrow_null) {
            EXPECT_EQ(flat->valueAt(i), typed->Value(arrow_idx))
                << "Value mismatch at offset " << i;
        }
    }
}

/// Compare an Int64 Velox FlatVector with an Arrow Int64Array.
static void AssertInt64Matches(const VectorPtr& vec,
                                const std::shared_ptr<arrow::Array>& arrow_arr,
                                int64_t start, int64_t n) {
    auto flat = vec->asFlatVector<int64_t>();
    auto typed = std::static_pointer_cast<arrow::Int64Array>(arrow_arr);
    for (int64_t i = 0; i < n; ++i) {
        int64_t arrow_idx = start + i;
        if (arrow_arr->IsNull(arrow_idx)) {
            EXPECT_TRUE(flat->isNullAt(i)) << "Expected null at " << i;
        } else {
            EXPECT_FALSE(flat->isNullAt(i));
            EXPECT_EQ(flat->valueAt(i), typed->Value(arrow_idx))
                << "Value mismatch at offset " << i;
        }
    }
}

/// Compare a Float64 Velox FlatVector with an Arrow DoubleArray.
static void AssertFloat64Matches(const VectorPtr& vec,
                                  const std::shared_ptr<arrow::Array>& arrow_arr,
                                  int64_t start, int64_t n) {
    auto flat = vec->asFlatVector<double>();
    auto typed = std::static_pointer_cast<arrow::DoubleArray>(arrow_arr);
    for (int64_t i = 0; i < n; ++i) {
        int64_t arrow_idx = start + i;
        if (arrow_arr->IsNull(arrow_idx)) {
            EXPECT_TRUE(flat->isNullAt(i));
        } else {
            EXPECT_FALSE(flat->isNullAt(i));
            EXPECT_DOUBLE_EQ(flat->valueAt(i), typed->Value(arrow_idx))
                << "Value mismatch at offset " << i;
        }
    }
}

/// Compare a Float32 Velox FlatVector with an Arrow FloatArray.
static void AssertFloat32Matches(const VectorPtr& vec,
                                  const std::shared_ptr<arrow::Array>& arrow_arr,
                                  int64_t start, int64_t n) {
    auto flat = vec->asFlatVector<float>();
    auto typed = std::static_pointer_cast<arrow::FloatArray>(arrow_arr);
    for (int64_t i = 0; i < n; ++i) {
        int64_t arrow_idx = start + i;
        if (arrow_arr->IsNull(arrow_idx)) {
            EXPECT_TRUE(flat->isNullAt(i));
        } else {
            EXPECT_FALSE(flat->isNullAt(i));
            EXPECT_FLOAT_EQ(flat->valueAt(i), typed->Value(arrow_idx));
        }
    }
}

/// Compare a String Velox FlatVector<StringView> with an Arrow StringArray.
static void AssertStringMatches(const VectorPtr& vec,
                                 const std::shared_ptr<arrow::Array>& arrow_arr,
                                 int64_t start, int64_t n) {
    auto flat = vec->asFlatVector<StringView>();
    auto typed = std::static_pointer_cast<arrow::StringArray>(arrow_arr);
    for (int64_t i = 0; i < n; ++i) {
        int64_t arrow_idx = start + i;
        if (arrow_arr->IsNull(arrow_idx)) {
            EXPECT_TRUE(flat->isNullAt(i));
        } else {
            EXPECT_FALSE(flat->isNullAt(i));
            auto arrow_view = typed->GetView(arrow_idx);
            auto velox_val = flat->valueAt(i);
            EXPECT_EQ(std::string(velox_val.data(), velox_val.size()),
                      std::string(arrow_view.data(), arrow_view.size()))
                << "String mismatch at offset " << i;
        }
    }
}

/// Dispatch to the appropriate comparison function based on Arrow type.
static void AssertVectorMatchesArrowArray(
        const VectorPtr& vec,
        const std::shared_ptr<arrow::Array>& arrow_arr,
        int64_t start_row, int64_t num_rows) {
    ASSERT_NE(vec, nullptr);
    ASSERT_NE(arrow_arr, nullptr);
    EXPECT_EQ(vec->size(), num_rows);

    switch (arrow_arr->type_id()) {
        case arrow::Type::INT32:
            AssertInt32Matches(vec, arrow_arr, start_row, num_rows);
            break;
        case arrow::Type::INT64:
            AssertInt64Matches(vec, arrow_arr, start_row, num_rows);
            break;
        case arrow::Type::FLOAT:
            AssertFloat32Matches(vec, arrow_arr, start_row, num_rows);
            break;
        case arrow::Type::DOUBLE:
            AssertFloat64Matches(vec, arrow_arr, start_row, num_rows);
            break;
        case arrow::Type::STRING:
            AssertStringMatches(vec, arrow_arr, start_row, num_rows);
            break;
        default:
            // For other types, just verify we didn't get nullptr
            GTEST_SUCCEED() << "No detailed comparison for type "
                            << arrow_arr->type()->ToString();
            break;
    }
}

// ═══════════════════════════════════════════════════════════════════════
// Helper: load Parquet, read all RGs, compare with source
// ═══════════════════════════════════════════════════════════════════════

/// Load a Parquet file into store using the FSST overload, then verify
/// every row group / every column can be read back correctly.
/// Returns the row_type so caller can use it for read_split_velox too.
static RowTypePtr LoadAndVerifyAllBatches(
        LiquidCacheStore& store,
        const std::string& parquet_path,
        const std::shared_ptr<arrow::Schema>& schema,
        const std::vector<std::shared_ptr<arrow::Array>>& source_columns) {
    std::shared_ptr<arrow::Schema> loaded_schema;
    double transcode_sec = 0;
    auto rg_infos = store.load_from_parquet(
        {parquet_path}, loaded_schema, transcode_sec);

    auto rowType = arrow_schema_to_velox_row_type(
        loaded_schema ? loaded_schema : schema);

    int num_cols = static_cast<int>(source_columns.size());
    uint64_t file_id = std::hash<std::string>{}(parquet_path);
    int64_t rg_start_row = 0;

    for (const auto& info : rg_infos) {
        // Read all batches within this RG
        for (uint16_t b = 0; b < info.num_batches; ++b) {
            // Read all columns together
            auto vec = store.read_batch_velox(file_id, info.rg_id, b,
                                              rowType, test_pool());
            if (!vec) {
                ADD_FAILURE() << "read_batch_velox null for file=" << file_id
                              << " rg=" << info.rg_id << " batch=" << b;
                return rowType;
            }

            auto rowVec = std::dynamic_pointer_cast<RowVector>(vec);
            if (!rowVec) {
                ADD_FAILURE() << "dynamic_pointer_cast<RowVector> failed";
                return rowType;
            }

            // Compute row range: batch_id * batch_size within this RG,
            // plus the cumulative rows from previous RGs
            int64_t batch_start = rg_start_row + static_cast<int64_t>(b) * 8192;
            int64_t batch_rows = rowVec->size();

            for (int c = 0; c < num_cols; ++c) {
                auto child = rowVec->childAt(c);
                auto& src_arr = source_columns[c];
                AssertVectorMatchesArrowArray(child, src_arr, batch_start, batch_rows);
            }
        }
        rg_start_row += static_cast<int64_t>(info.total_rows);
    }

    return rowType;
}

// ═══════════════════════════════════════════════════════════════════════
// 1. PipelineCorrectness tests
// ═══════════════════════════════════════════════════════════════════════

TEST(PipelineCorrectness, SingleRG_AllTypes) {
    const int n = 200;
    auto schema = arrow::schema({
        arrow::field("c0", arrow::int32()),
        arrow::field("c1", arrow::int64()),
        arrow::field("c2", arrow::float32()),
        arrow::field("c3", arrow::float64()),
        arrow::field("c4", arrow::utf8()),
    });
    std::vector<std::shared_ptr<arrow::Array>> cols = {
        GenInt32(n), GenInt64(n), GenFloat32(n), GenFloat64(n), GenString(n)
    };

    std::string path = "/tmp/test_pipeline_single_rg.parquet";
    WriteParquetFile(path, schema, cols, n);  // 1 RG

    LiquidCacheStore store;
    auto rowType = LoadAndVerifyAllBatches(store, path, schema, cols);
    (void)rowType;
    std::remove(path.c_str());
}

TEST(PipelineCorrectness, MultiRG_IntTypes) {
    const int rows_per_rg = 100;
    const int num_rgs = 5;
    const int n = rows_per_rg * num_rgs;

    auto schema = arrow::schema({
        arrow::field("c0", arrow::int32()),
        arrow::field("c1", arrow::int64()),
    });
    std::vector<std::shared_ptr<arrow::Array>> cols = {
        GenInt32(n), GenInt64(n)
    };

    std::string path = "/tmp/test_pipeline_multi_rg_int.parquet";
    WriteParquetFile(path, schema, cols, rows_per_rg);

    LiquidCacheStore store;
    LoadAndVerifyAllBatches(store, path, schema, cols);
    std::remove(path.c_str());
}

TEST(PipelineCorrectness, MultiRG_String) {
    const int rows_per_rg = 80;
    const int num_rgs = 3;
    const int n = rows_per_rg * num_rgs;

    auto schema = arrow::schema({
        arrow::field("c0", arrow::int32()),
        arrow::field("c1", arrow::utf8()),
    });
    std::vector<std::shared_ptr<arrow::Array>> cols = {
        GenInt32(n), GenStringVaried(n)
    };

    std::string path = "/tmp/test_pipeline_multi_rg_str.parquet";
    WriteParquetFile(path, schema, cols, rows_per_rg);

    LiquidCacheStore store;
    LoadAndVerifyAllBatches(store, path, schema, cols);
    std::remove(path.c_str());
}

TEST(PipelineCorrectness, SingleRG_WithNulls) {
    const int n = 300;
    auto schema = arrow::schema({
        arrow::field("c0", arrow::int32()),
        arrow::field("c1", arrow::float64()),
        arrow::field("c2", arrow::utf8()),
    });
    std::vector<std::shared_ptr<arrow::Array>> cols = {
        GenInt32(n, 20),     // 20% nulls
        GenFloat64(n),       // no nulls
        GenString(n, 20),    // 20% nulls
    };

    std::string path = "/tmp/test_pipeline_nulls.parquet";
    WriteParquetFile(path, schema, cols, n);

    LiquidCacheStore store;
    LoadAndVerifyAllBatches(store, path, schema, cols);
    std::remove(path.c_str());
}

TEST(PipelineCorrectness, MultiRG_AllNullColumn) {
    const int rows_per_rg = 80;
    const int num_rgs = 3;
    const int n = rows_per_rg * num_rgs;

    auto schema = arrow::schema({
        arrow::field("c0", arrow::int32()),
        arrow::field("c1", arrow::int32()),  // all-null
    });
    std::vector<std::shared_ptr<arrow::Array>> cols = {
        GenInt32(n),
        GenAllNull(n, arrow::Type::INT32),
    };

    std::string path = "/tmp/test_pipeline_allnull.parquet";
    WriteParquetFile(path, schema, cols, rows_per_rg);

    LiquidCacheStore store;
    LoadAndVerifyAllBatches(store, path, schema, cols);
    std::remove(path.c_str());
}

TEST(PipelineCorrectness, LargeColumnCount) {
    // Regression test: previously read_batch_velox iterated rowType->size()
    // instead of projection.size(), causing all columns to be decoded even
    // when only a few were projected. With 20+ columns this was a 30x slowdown.
    const int n = 100;

    arrow::FieldVector fields;
    std::vector<std::shared_ptr<arrow::Array>> cols;
    for (int i = 0; i < 22; ++i) {
        fields.push_back(arrow::field("col_" + std::to_string(i), arrow::int32()));
        cols.push_back(GenInt32(n));
    }
    auto schema = arrow::schema(fields);

    std::string path = "/tmp/test_pipeline_wide.parquet";
    WriteParquetFile(path, schema, cols, n);

    LiquidCacheStore store;
    // Use projection to read only 2 columns
    std::shared_ptr<arrow::Schema> loaded_schema;
    double transcode_sec = 0;
    store.load_from_parquet({path}, loaded_schema, transcode_sec);
    auto rowType = arrow_schema_to_velox_row_type(
        loaded_schema ? loaded_schema : schema);

    uint64_t file_id = std::hash<std::string>{}(path);
    std::vector<int> projection = {0, 5};  // Only col_0 and col_5
    auto vec = store.read_batch_velox(file_id, 0, 0, rowType, test_pool(),
                                      projection);
    ASSERT_NE(vec, nullptr);
    auto rowVec = std::dynamic_pointer_cast<RowVector>(vec);
    ASSERT_NE(rowVec, nullptr);
    EXPECT_EQ(rowVec->childrenSize(), 2)
        << "Should only have 2 projected columns, not all 22";
    EXPECT_EQ(rowVec->size(), n);

    // Verify values: col_0 and col_5 should match source
    AssertInt32Matches(rowVec->childAt(0), cols[0], 0, n);
    AssertInt32Matches(rowVec->childAt(1), cols[5], 0, n);

    std::remove(path.c_str());
}

// ═══════════════════════════════════════════════════════════════════════
// 2. ReadSplitVelox tests
// ═══════════════════════════════════════════════════════════════════════

TEST(ReadSplitVelox, SingleRG_ExactMatch) {
    const int rows_per_rg = 100;
    const int num_rgs = 3;
    const int n = rows_per_rg * num_rgs;

    auto schema = arrow::schema({
        arrow::field("c0", arrow::int32()),
        arrow::field("c1", arrow::float64()),
    });
    std::vector<std::shared_ptr<arrow::Array>> cols = {
        GenInt32(n), GenFloat64(n)
    };

    std::string path = "/tmp/test_split_single_rg.parquet";
    WriteParquetFile(path, schema, cols, rows_per_rg);

    LiquidCacheStore store;
    auto rowType = LoadAndVerifyAllBatches(store, path, schema, cols);

    uint64_t file_id = std::hash<std::string>{}(path);

    // Read the Parquet footer to get the actual RG offsets
    auto maybe_in = arrow::io::ReadableFile::Open(path);
    ASSERT_TRUE(maybe_in.ok());
    std::unique_ptr<parquet::arrow::FileReader> reader;
    ASSERT_TRUE(parquet::arrow::OpenFile(
        maybe_in.ValueOrDie(), arrow::default_memory_pool(), &reader).ok());
    ASSERT_EQ(reader->num_row_groups(), num_rgs);

    auto meta = reader->parquet_reader()->metadata();

    // Collect RG offsets using same logic as load_from_parquet
    std::vector<uint64_t> rg_offsets;
    for (int i = 0; i < num_rgs; ++i) {
        auto rg_meta = meta->RowGroup(i);
        auto off = static_cast<uint64_t>(rg_meta->file_offset());
        if (off == 0 && rg_meta->num_columns() > 0)
            off = static_cast<uint64_t>(rg_meta->ColumnChunk(0)->file_offset());
        rg_offsets.push_back(off);
    }

    // Query exactly RG 0's byte range: from offset[0] to offset[1]
    uint64_t start = rg_offsets[0];
    uint64_t length = rg_offsets[1] - rg_offsets[0];

    auto vec = store.read_split_velox(file_id, start, length,
                                      rowType, test_pool());
    ASSERT_NE(vec, nullptr);
    auto rowVec = std::dynamic_pointer_cast<RowVector>(vec);
    ASSERT_NE(rowVec, nullptr);
    EXPECT_EQ(rowVec->size(), rows_per_rg)
        << "Should return exactly RG 0 = " << rows_per_rg << " rows";

    AssertInt32Matches(rowVec->childAt(0), cols[0], 0, rows_per_rg);
    AssertFloat64Matches(rowVec->childAt(1), cols[1], 0, rows_per_rg);

    std::remove(path.c_str());
}

TEST(ReadSplitVelox, MultiRG_PartialCoverage) {
    const int rows_per_rg = 80;
    const int num_rgs = 5;
    const int n = rows_per_rg * num_rgs;

    auto schema = arrow::schema({
        arrow::field("c0", arrow::int32()),
    });
    std::vector<std::shared_ptr<arrow::Array>> cols = {GenInt32(n)};

    std::string path = "/tmp/test_split_partial.parquet";
    WriteParquetFile(path, schema, cols, rows_per_rg);

    LiquidCacheStore store;
    auto rowType = LoadAndVerifyAllBatches(store, path, schema, cols);
    uint64_t file_id = std::hash<std::string>{}(path);

    // Get RG offsets
    auto maybe_in = arrow::io::ReadableFile::Open(path);
    ASSERT_TRUE(maybe_in.ok());
    std::unique_ptr<parquet::arrow::FileReader> reader;
    ASSERT_TRUE(parquet::arrow::OpenFile(
        maybe_in.ValueOrDie(), arrow::default_memory_pool(), &reader).ok());

    auto meta = reader->parquet_reader()->metadata();

    std::vector<uint64_t> rg_offsets;
    for (int i = 0; i < num_rgs; ++i) {
        auto rg_meta = meta->RowGroup(i);
        auto off = static_cast<uint64_t>(rg_meta->file_offset());
        if (off == 0 && rg_meta->num_columns() > 0)
            off = static_cast<uint64_t>(rg_meta->ColumnChunk(0)->file_offset());
        rg_offsets.push_back(off);
    }

    // Query RGs 1, 2, 3 (skip RG 0 and RG 4)
    // Range: [rg_offsets[1], rg_offsets[4])
    uint64_t start = rg_offsets[1];
    uint64_t length = rg_offsets[4] - rg_offsets[1];

    auto vec = store.read_split_velox(file_id, start, length,
                                      rowType, test_pool());
    ASSERT_NE(vec, nullptr);
    auto rowVec = std::dynamic_pointer_cast<RowVector>(vec);
    ASSERT_NE(rowVec, nullptr);

    // Should cover RGs 1,2,3 = rows 80..319
    EXPECT_EQ(rowVec->size(), rows_per_rg * 3);
    AssertInt32Matches(rowVec->childAt(0), cols[0], rows_per_rg, rows_per_rg * 3);

    std::remove(path.c_str());
}

TEST(ReadSplitVelox, AllRGs) {
    const int rows_per_rg = 100;
    const int num_rgs = 3;
    const int n = rows_per_rg * num_rgs;

    auto schema = arrow::schema({
        arrow::field("c0", arrow::float64()),
    });
    std::vector<std::shared_ptr<arrow::Array>> cols = {GenFloat64(n)};

    std::string path = "/tmp/test_split_all_rgs.parquet";
    WriteParquetFile(path, schema, cols, rows_per_rg);

    LiquidCacheStore store;
    auto rowType = LoadAndVerifyAllBatches(store, path, schema, cols);
    uint64_t file_id = std::hash<std::string>{}(path);

    // Query from offset=0 with a very large length
    auto vec = store.read_split_velox(file_id, 0,
                                      static_cast<uint64_t>(1) << 50,
                                      rowType, test_pool());
    ASSERT_NE(vec, nullptr);
    auto rowVec = std::dynamic_pointer_cast<RowVector>(vec);
    ASSERT_NE(rowVec, nullptr);
    EXPECT_EQ(rowVec->size(), n);

    std::remove(path.c_str());
}

TEST(ReadSplitVelox, NoMatch_OutOfRange) {
    const int rows_per_rg = 100;
    const int num_rgs = 2;
    const int n = rows_per_rg * num_rgs;

    auto schema = arrow::schema({
        arrow::field("c0", arrow::int32()),
    });
    std::vector<std::shared_ptr<arrow::Array>> cols = {GenInt32(n)};

    std::string path = "/tmp/test_split_nomatch.parquet";
    WriteParquetFile(path, schema, cols, rows_per_rg);

    LiquidCacheStore store;
    auto rowType = LoadAndVerifyAllBatches(store, path, schema, cols);
    uint64_t file_id = std::hash<std::string>{}(path);

    // Query an offset that doesn't match any RG
    auto vec = store.read_split_velox(file_id,
                                      static_cast<uint64_t>(1) << 40,  // huge offset
                                      100,
                                      rowType, test_pool());
    EXPECT_EQ(vec, nullptr) << "Out-of-range split should return nullptr";

    std::remove(path.c_str());
}

// ═══════════════════════════════════════════════════════════════════════
// 3. Projection tests
// ═══════════════════════════════════════════════════════════════════════

TEST(Projection, SubsetOfColumns) {
    const int n = 200;
    auto schema = arrow::schema({
        arrow::field("c0", arrow::int32()),
        arrow::field("c1", arrow::int64()),
        arrow::field("c2", arrow::float64()),
        arrow::field("c3", arrow::utf8()),
        arrow::field("c4", arrow::int32()),
    });
    std::vector<std::shared_ptr<arrow::Array>> cols = {
        GenInt32(n), GenInt64(n), GenFloat64(n), GenString(n),
        GenInt32(n)
    };

    std::string path = "/tmp/test_proj_subset.parquet";
    WriteParquetFile(path, schema, cols, n);

    LiquidCacheStore store;
    std::shared_ptr<arrow::Schema> loaded_schema;
    double transcode_sec = 0;
    store.load_from_parquet({path}, loaded_schema, transcode_sec);
    auto rowType = arrow_schema_to_velox_row_type(
        loaded_schema ? loaded_schema : schema);

    uint64_t file_id = std::hash<std::string>{}(path);

    // Project only columns {1, 3} (int64, string)
    std::vector<int> projection = {1, 3};
    auto vec = store.read_batch_velox(file_id, 0, 0, rowType, test_pool(),
                                      projection);
    ASSERT_NE(vec, nullptr);
    auto rowVec = std::dynamic_pointer_cast<RowVector>(vec);
    ASSERT_NE(rowVec, nullptr);

    // Should have exactly 2 children
    EXPECT_EQ(rowVec->childrenSize(), 2);
    EXPECT_EQ(rowVec->size(), n);

    // Child 0 should be col 1 (int64), Child 1 = col 3 (string)
    AssertInt64Matches(rowVec->childAt(0), cols[1], 0, n);
    AssertStringMatches(rowVec->childAt(1), cols[3], 0, n);

    std::remove(path.c_str());
}

TEST(Projection, SingleColumn) {
    const int n = 200;
    auto schema = arrow::schema({
        arrow::field("c0", arrow::int32()),
        arrow::field("c1", arrow::int64()),
        arrow::field("c2", arrow::float64()),
        arrow::field("c3", arrow::utf8()),
    });
    std::vector<std::shared_ptr<arrow::Array>> cols = {
        GenInt32(n), GenInt64(n), GenFloat64(n), GenString(n),
    };

    std::string path = "/tmp/test_proj_single.parquet";
    WriteParquetFile(path, schema, cols, n);

    LiquidCacheStore store;
    std::shared_ptr<arrow::Schema> loaded_schema;
    double transcode_sec = 0;
    store.load_from_parquet({path}, loaded_schema, transcode_sec);
    auto rowType = arrow_schema_to_velox_row_type(
        loaded_schema ? loaded_schema : schema);

    uint64_t file_id = std::hash<std::string>{}(path);

    std::vector<int> projection = {0};
    auto vec = store.read_batch_velox(file_id, 0, 0, rowType, test_pool(),
                                      projection);
    ASSERT_NE(vec, nullptr);
    auto rowVec = std::dynamic_pointer_cast<RowVector>(vec);
    ASSERT_NE(rowVec, nullptr);
    EXPECT_EQ(rowVec->childrenSize(), 1);
    AssertInt32Matches(rowVec->childAt(0), cols[0], 0, n);

    std::remove(path.c_str());
}

TEST(Projection, AllColumns_EmptyProjection) {
    const int n = 100;
    auto schema = arrow::schema({
        arrow::field("c0", arrow::int32()),
        arrow::field("c1", arrow::float64()),
    });
    std::vector<std::shared_ptr<arrow::Array>> cols = {
        GenInt32(n), GenFloat64(n),
    };

    std::string path = "/tmp/test_proj_all.parquet";
    WriteParquetFile(path, schema, cols, n);

    LiquidCacheStore store;
    std::shared_ptr<arrow::Schema> loaded_schema;
    double transcode_sec = 0;
    store.load_from_parquet({path}, loaded_schema, transcode_sec);
    auto rowType = arrow_schema_to_velox_row_type(
        loaded_schema ? loaded_schema : schema);

    uint64_t file_id = std::hash<std::string>{}(path);

    // Empty projection → all columns
    auto vec = store.read_batch_velox(file_id, 0, 0, rowType, test_pool());
    ASSERT_NE(vec, nullptr);
    auto rowVec = std::dynamic_pointer_cast<RowVector>(vec);
    ASSERT_NE(rowVec, nullptr);
    EXPECT_EQ(rowVec->childrenSize(), 2);
    AssertInt32Matches(rowVec->childAt(0), cols[0], 0, n);
    AssertFloat64Matches(rowVec->childAt(1), cols[1], 0, n);

    // Should match the result with explicit projection {0, 1}
    auto vec2 = store.read_batch_velox(file_id, 0, 0, rowType, test_pool(),
                                       {0, 1});
    ASSERT_NE(vec2, nullptr);
    auto rowVec2 = std::dynamic_pointer_cast<RowVector>(vec2);
    ASSERT_NE(rowVec2, nullptr);
    EXPECT_EQ(rowVec2->childrenSize(), 2);
    EXPECT_EQ(rowVec2->size(), rowVec->size());

    std::remove(path.c_str());
}

// ═══════════════════════════════════════════════════════════════════════
// 4. MultiFile tests
// ═══════════════════════════════════════════════════════════════════════

TEST(MultiFile, IndependentFiles) {
    const int n = 150;
    const int rows_per_rg = 75;  // 2 RGs per file

    // File A: int32
    auto schema_a = arrow::schema({
        arrow::field("val", arrow::int32()),
    });
    std::vector<std::shared_ptr<arrow::Array>> cols_a = {GenInt32(n)};
    std::string path_a = "/tmp/test_multifile_a.parquet";
    uint64_t fid_a = WriteParquetFile(path_a, schema_a, cols_a, rows_per_rg);

    // File B: float64 (different type, same col_id)
    auto schema_b = arrow::schema({
        arrow::field("val", arrow::float64()),
    });
    std::vector<std::shared_ptr<arrow::Array>> cols_b = {GenFloat64(n)};
    std::string path_b = "/tmp/test_multifile_b.parquet";
    uint64_t fid_b = WriteParquetFile(path_b, schema_b, cols_b, rows_per_rg);

    LiquidCacheStore store;
    std::shared_ptr<arrow::Schema> loaded_schema;
    double transcode_sec = 0;

    // Load both files
    store.load_from_parquet({path_a, path_b}, loaded_schema, transcode_sec);

    auto rowType_a = arrow_schema_to_velox_row_type(schema_a);
    auto rowType_b = arrow_schema_to_velox_row_type(schema_b);

    // Read file A
    for (uint16_t rg = 0; rg < 2; ++rg) {
        auto vec = store.read_batch_velox(fid_a, rg, 0, rowType_a, test_pool());
        ASSERT_NE(vec, nullptr) << "File A RG " << rg << " should exist";
        auto rowVec = std::dynamic_pointer_cast<RowVector>(vec);
        AssertInt32Matches(rowVec->childAt(0), cols_a[0],
                           static_cast<int64_t>(rg) * rows_per_rg, 75);
    }

    // Read file B
    for (uint16_t rg = 0; rg < 2; ++rg) {
        auto vec = store.read_batch_velox(fid_b, rg, 0, rowType_b, test_pool());
        ASSERT_NE(vec, nullptr) << "File B RG " << rg << " should exist";
        auto rowVec = std::dynamic_pointer_cast<RowVector>(vec);
        AssertFloat64Matches(rowVec->childAt(0), cols_b[0],
                             static_cast<int64_t>(rg) * rows_per_rg, 75);
    }

    // Verify isolation: file A values should not be affected
    {
        auto vec = store.read_batch_velox(fid_a, 0, 0, rowType_a, test_pool());
        ASSERT_NE(vec, nullptr);
        auto rowVec = std::dynamic_pointer_cast<RowVector>(vec);
        AssertInt32Matches(rowVec->childAt(0), cols_a[0], 0, 75);
    }

    std::remove(path_a.c_str());
    std::remove(path_b.c_str());
}

TEST(MultiFile, SameColumnIndex_DifferentData) {
    // Two files with the same column layout but different values
    const int n = 100;
    auto schema = arrow::schema({
        arrow::field("c0", arrow::int32()),
    });

    // File 1: ascending (0..99)
    auto cols1 = std::vector<std::shared_ptr<arrow::Array>>{GenInt32(n)};
    std::string path1 = "/tmp/test_multifile_same_1.parquet";
    uint64_t fid1 = WriteParquetFile(path1, schema, cols1, n);

    // File 2: values by 100 (0, 100, 200, ...)
    arrow::Int32Builder b2;
    for (int i = 0; i < n; ++i) APPEND(b2, i * 100);
    auto cols2 = std::vector<std::shared_ptr<arrow::Array>>{
        b2.Finish().ValueOrDie()};
    std::string path2 = "/tmp/test_multifile_same_2.parquet";
    uint64_t fid2 = WriteParquetFile(path2, schema, cols2, n);

    LiquidCacheStore store;
    std::shared_ptr<arrow::Schema> loaded_schema;
    double transcode_sec = 0;
    store.load_from_parquet({path1, path2}, loaded_schema, transcode_sec);

    auto rowType = arrow_schema_to_velox_row_type(schema);

    // File 1 should have values 0..99
    auto vec1 = store.read_batch_velox(fid1, 0, 0, rowType, test_pool());
    ASSERT_NE(vec1, nullptr);
    auto rv1 = std::dynamic_pointer_cast<RowVector>(vec1);
    AssertInt32Matches(rv1->childAt(0), cols1[0], 0, n);

    // File 2 should have values 0, 100, 200, ...
    auto vec2 = store.read_batch_velox(fid2, 0, 0, rowType, test_pool());
    ASSERT_NE(vec2, nullptr);
    auto rv2 = std::dynamic_pointer_cast<RowVector>(vec2);
    AssertInt32Matches(rv2->childAt(0), cols2[0], 0, n);

    // Re-check file 1 still correct (isolation)
    auto vec1b = store.read_batch_velox(fid1, 0, 0, rowType, test_pool());
    ASSERT_NE(vec1b, nullptr);
    auto rv1b = std::dynamic_pointer_cast<RowVector>(vec1b);
    auto flat1b = rv1b->childAt(0)->asFlatVector<int32_t>();
    EXPECT_EQ(flat1b->valueAt(0), 0);
    EXPECT_EQ(flat1b->valueAt(50), 50);
    EXPECT_EQ(flat1b->valueAt(99), 99);

    std::remove(path1.c_str());
    std::remove(path2.c_str());
}

// ═══════════════════════════════════════════════════════════════════════
// 5. CacheMiss tests
// ═══════════════════════════════════════════════════════════════════════

TEST(CacheMiss, MissingFile) {
    auto schema = arrow::schema({
        arrow::field("c0", arrow::int32()),
    });
    auto rowType = arrow_schema_to_velox_row_type(schema);

    LiquidCacheStore store;
    // Query a file_id that was never loaded (should not exist)
    uint64_t never_loaded = 0xDEADBEEF;
    auto vec = store.read_batch_velox(never_loaded, 0, 0, rowType, test_pool());
    EXPECT_EQ(vec, nullptr)
        << "Querying a never-loaded file should return nullptr";
}

TEST(CacheMiss, MissingRowGroup) {
    const int n = 150;
    auto schema = arrow::schema({
        arrow::field("c0", arrow::int32()),
    });
    std::vector<std::shared_ptr<arrow::Array>> cols = {GenInt32(n)};

    std::string path = "/tmp/test_miss_rg.parquet";
    WriteParquetFile(path, schema, cols, 50);  // 3 RGs
    uint64_t file_id = std::hash<std::string>{}(path);

    LiquidCacheStore store;
    std::shared_ptr<arrow::Schema> loaded_schema;
    double transcode_sec = 0;
    store.load_from_parquet({path}, loaded_schema, transcode_sec);
    auto rowType = arrow_schema_to_velox_row_type(
        loaded_schema ? loaded_schema : schema);

    // RG 0 should exist
    auto vec_ok = store.read_batch_velox(file_id, 0, 0, rowType, test_pool());
    ASSERT_NE(vec_ok, nullptr);

    // RG 99 should not exist
    auto vec_miss = store.read_batch_velox(file_id, 99, 0, rowType, test_pool());
    EXPECT_EQ(vec_miss, nullptr)
        << "Querying a non-existent row group should return nullptr";

    std::remove(path.c_str());
}

// ═══════════════════════════════════════════════════════════════════════
// 6. FSSTPath tests
// ═══════════════════════════════════════════════════════════════════════

TEST(FSSTPath, WithCompressorStates) {
    // Use the load_from_parquet overload that internally uses compressor_states
    const int n = 200;
    auto schema = arrow::schema({
        arrow::field("c0", arrow::int32()),
        arrow::field("c1", arrow::utf8()),
    });
    std::vector<std::shared_ptr<arrow::Array>> cols = {
        GenInt32(n), GenStringVaried(n)
    };

    std::string path = "/tmp/test_fsst_single.parquet";
    WriteParquetFile(path, schema, cols, n);

    LiquidCacheStore store;
    std::shared_ptr<arrow::Schema> loaded_schema;
    double transcode_sec = 0;

    // Use the FSST auto-reuse overload (no transcode_fn parameter)
    auto rg_infos = store.load_from_parquet(
        {path}, loaded_schema, transcode_sec);
    ASSERT_FALSE(rg_infos.empty());

    auto rowType = arrow_schema_to_velox_row_type(
        loaded_schema ? loaded_schema : schema);
    uint64_t file_id = std::hash<std::string>{}(path);

    for (const auto& info : rg_infos) {
        for (uint16_t b = 0; b < info.num_batches; ++b) {
            auto vec = store.read_batch_velox(file_id, info.rg_id, b,
                                              rowType, test_pool());
            ASSERT_NE(vec, nullptr);
            auto rowVec = std::dynamic_pointer_cast<RowVector>(vec);
            ASSERT_NE(rowVec, nullptr);

            int64_t batch_start = static_cast<int64_t>(b) * 8192;
            AssertInt32Matches(rowVec->childAt(0), cols[0],
                               batch_start, rowVec->size());
            AssertStringMatches(rowVec->childAt(1), cols[1],
                                batch_start, rowVec->size());
        }
    }

    std::remove(path.c_str());
}

TEST(FSSTPath, AutoReuse) {
    // Load two files with the same schema — compressor_states should
    // be auto-reused for the second file
    const int n = 100;
    auto schema = arrow::schema({
        arrow::field("c0", arrow::int32()),
        arrow::field("c1", arrow::utf8()),
    });

    // File 1
    std::vector<std::shared_ptr<arrow::Array>> cols1 = {
        GenInt32(n), GenStringVaried(n)
    };
    std::string path1 = "/tmp/test_fsst_reuse_1.parquet";
    WriteParquetFile(path1, schema, cols1, n);

    // File 2 — same schema, different data
    arrow::StringBuilder b_s;
    for (int i = 0; i < n; ++i) {
        APPEND(b_s, "reuse_test_row_" + std::to_string(i + 1000));
    }
    std::vector<std::shared_ptr<arrow::Array>> cols2 = {
        GenInt32(n), b_s.Finish().ValueOrDie()
    };
    std::string path2 = "/tmp/test_fsst_reuse_2.parquet";
    WriteParquetFile(path2, schema, cols2, n);

    // Load both files with compressor states (FSST reuse)
    LiquidCacheStore store;
    std::shared_ptr<arrow::Schema> loaded_schema;
    double transcode_sec = 0;
    auto rg_infos = store.load_from_parquet(
        {path1, path2}, loaded_schema, transcode_sec);

    auto rowType = arrow_schema_to_velox_row_type(
        loaded_schema ? loaded_schema : schema);

    uint64_t fid1 = std::hash<std::string>{}(path1);
    uint64_t fid2 = std::hash<std::string>{}(path2);

    // Verify file 1 data
    {
        auto vec = store.read_batch_velox(fid1, 0, 0, rowType, test_pool());
        ASSERT_NE(vec, nullptr);
        auto rowVec = std::dynamic_pointer_cast<RowVector>(vec);
        AssertStringMatches(rowVec->childAt(1), cols1[1], 0, n);
    }

    // Verify file 2 data
    {
        auto vec = store.read_batch_velox(fid2, 0, 0, rowType, test_pool());
        ASSERT_NE(vec, nullptr);
        auto rowVec = std::dynamic_pointer_cast<RowVector>(vec);
        AssertStringMatches(rowVec->childAt(1), cols2[1], 0, n);
    }

    std::remove(path1.c_str());
    std::remove(path2.c_str());
}

// ═══════════════════════════════════════════════════════════════════════
// No main() — main() is provided by test_velox_crossval.cpp,
// which initializes Velox MemoryManager and Arrow compute.
// ═══════════════════════════════════════════════════════════════════════
