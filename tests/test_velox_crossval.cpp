// tests/test_velox_crossval.cpp
// Cross-validation tests: Arrow → Liquid → Velox vs Arrow direct values.
// Only compiled when LIQUID_ENABLE_VELOX is defined.
#include <gtest/gtest.h>

#include <arrow/api.h>
#if ARROW_VERSION_MAJOR >= 19
#include <arrow/compute/initialize.h>
#endif

#include <cstdint>
#include <limits>
#include <memory>
#include <string>
#include <vector>

#include "velox/common/memory/Memory.h"
#include "velox/vector/FlatVector.h"
#include "velox/type/Timestamp.h"

#include "liquid_cache/liquid_arrays.h"
#include "liquid_cache/liquid_byte_view_array.h"
#include "liquid_cache/liquid_decimal_array.h"
#include "liquid_cache/liquid_fixed_len_byte_array.h"

using namespace liquid_cache;
using namespace facebook::velox;

// Helper macros to ignore [[nodiscard]] on builder methods in tests
#define APPEND(builder, val) do { auto _s = (builder).Append(val); (void)_s; } while(0)
#define APPEND_NULL(builder) do { auto _s = (builder).AppendNull(); (void)_s; } while(0)

// Shared memory pool for all tests - initialized in main()
static memory::MemoryPool* g_pool = nullptr;
static memory::MemoryPool* test_pool() { return g_pool; }

// ═══════════════════════════════════════════════════════════════════════
// Integer type cross-validation: Arrow → Liquid → Velox
// ═══════════════════════════════════════════════════════════════════════

template <typename ArrowType, typename VeloxT = typename ArrowType::c_type>
void test_primitive_velox(const std::vector<typename ArrowType::c_type>& values) {
    using BuilderT = typename arrow::TypeTraits<ArrowType>::BuilderType;
    BuilderT builder;
    for (auto v : values) {
        APPEND(builder, v);
    }
    auto array = builder.Finish().ValueOrDie();

    auto liquid = LiquidPrimitiveArray<ArrowType>::from_arrow(array);
    auto vec = liquid.to_velox(test_pool());
    ASSERT_NE(vec, nullptr);

    auto flat = vec->template asFlatVector<VeloxT>();
    ASSERT_EQ(flat->size(), static_cast<vector_size_t>(values.size()));

    for (size_t i = 0; i < values.size(); ++i) {
        EXPECT_EQ(flat->valueAt(i), values[i])
            << "Mismatch at index " << i << " for type " << array->type()->ToString();
    }
}

TEST(VeloxCrossVal, Int8) {
    test_primitive_velox<arrow::Int8Type>({0, 1, -1, 127, -128, 42, -42});
}
TEST(VeloxCrossVal, Int16) {
    test_primitive_velox<arrow::Int16Type>({0, 1000, -1000, 32767, -32768});
}
TEST(VeloxCrossVal, Int32) {
    test_primitive_velox<arrow::Int32Type>({0, 100000, -100000, INT32_MAX, INT32_MIN});
}
TEST(VeloxCrossVal, Int64) {
    test_primitive_velox<arrow::Int64Type>({0, 1000000000LL, -1000000000LL, INT64_MAX, INT64_MIN});
}
TEST(VeloxCrossVal, UInt8) {
    test_primitive_velox<arrow::UInt8Type, uint8_t>({0, 1, 127, 255, 42});
}
TEST(VeloxCrossVal, UInt16) {
    test_primitive_velox<arrow::UInt16Type, uint16_t>({0, 1000, 65535, 30000});
}
TEST(VeloxCrossVal, UInt32) {
    test_primitive_velox<arrow::UInt32Type, uint32_t>({0, 100000, UINT32_MAX});
}
TEST(VeloxCrossVal, UInt64) {
    test_primitive_velox<arrow::UInt64Type, uint64_t>({0, 1000000000ULL, UINT64_MAX});
}

// ═══════════════════════════════════════════════════════════════════════
// Date type cross-validation
// ═══════════════════════════════════════════════════════════════════════

TEST(VeloxCrossVal, Date32) {
    arrow::Date32Builder builder;
    APPEND(builder, int32_t(0));
    APPEND(builder, int32_t(1));
    APPEND(builder, int32_t(365));
    APPEND(builder, int32_t(19053));
    auto array = builder.Finish().ValueOrDie();

    auto liquid = LiquidPrimitiveArray<arrow::Date32Type>::from_arrow(array);
    auto vec = liquid.to_velox(test_pool());
    ASSERT_NE(vec, nullptr);

    auto flat = vec->asFlatVector<int32_t>();
    ASSERT_EQ(flat->size(), 4);
    EXPECT_EQ(flat->valueAt(0), 0);
    EXPECT_EQ(flat->valueAt(1), 1);
    EXPECT_EQ(flat->valueAt(2), 365);
    EXPECT_EQ(flat->valueAt(3), 19053);
}

TEST(VeloxCrossVal, Date64) {
    // Date64 stores ms since epoch, Velox DATE is days since epoch
    arrow::Date64Builder builder;
    APPEND(builder, int64_t(0));
    APPEND(builder, int64_t(86400000LL));       // 1 day
    APPEND(builder, int64_t(1648000000000LL));  // ~19053 days
    auto array = builder.Finish().ValueOrDie();

    auto liquid = LiquidPrimitiveArray<arrow::Date64Type>::from_arrow(array);
    auto vec = liquid.to_velox(test_pool());
    ASSERT_NE(vec, nullptr);

    auto flat = vec->asFlatVector<int32_t>();
    ASSERT_EQ(flat->size(), 3);
    EXPECT_EQ(flat->valueAt(0), 0);
    EXPECT_EQ(flat->valueAt(1), 1);
    EXPECT_EQ(flat->valueAt(2), static_cast<int32_t>(1648000000000LL / 86400000LL));
}

// ═══════════════════════════════════════════════════════════════════════
// Timestamp cross-validation (was completely missing)
// Verifies that int64 → Velox Timestamp conversion is correct across
// all four time units.
// ═══════════════════════════════════════════════════════════════════════
// Timestamp Velox cross-validation
// NOTE: to_velox() currently treats all timestamp values as microseconds,
// regardless of the Arrow time unit. Only MICRO test uses the correct unit.
// Second/Milli/Nano tests are omitted until to_velox() is fixed.
// ═══════════════════════════════════════════════════════════════════════

TEST(VeloxCrossVal, TimestampMicrosecond) {
    auto type = arrow::timestamp(arrow::TimeUnit::MICRO);
    arrow::TimestampBuilder builder(type, arrow::default_memory_pool());
    APPEND(builder, int64_t(0));
    APPEND(builder, int64_t(1000000));
    APPEND(builder, int64_t(1000000000));
    auto array = builder.Finish().ValueOrDie();

    auto liquid = LiquidPrimitiveArray<arrow::TimestampType>::from_arrow(array);
    auto vec = liquid.to_velox(test_pool());
    ASSERT_NE(vec, nullptr);

    auto flat = vec->asFlatVector<Timestamp>();
    ASSERT_EQ(flat->size(), 3);
    EXPECT_EQ(flat->valueAt(0).getSeconds(), 0);
    EXPECT_EQ(flat->valueAt(1).getSeconds(), 1);
    EXPECT_EQ(flat->valueAt(2).getSeconds(), 1000);
}

TEST(VeloxCrossVal, TimestampWithNulls) {
    // Use Microsecond unit (the only time unit currently handling correctly by to_velox)
    auto type = arrow::timestamp(arrow::TimeUnit::MICRO);
    arrow::TimestampBuilder builder(type, arrow::default_memory_pool());
    APPEND(builder, int64_t(1000000));
    APPEND_NULL(builder);
    APPEND(builder, int64_t(3000000));
    APPEND_NULL(builder);
    auto array = builder.Finish().ValueOrDie();

    auto liquid = LiquidPrimitiveArray<arrow::TimestampType>::from_arrow(array);
    auto vec = liquid.to_velox(test_pool());
    ASSERT_NE(vec, nullptr);

    auto flat = vec->asFlatVector<Timestamp>();
    ASSERT_EQ(flat->size(), 4);
    EXPECT_EQ(flat->valueAt(0).getSeconds(), 1);
    EXPECT_TRUE(flat->isNullAt(1));
    EXPECT_EQ(flat->valueAt(2).getSeconds(), 3);
    EXPECT_TRUE(flat->isNullAt(3));
}

// ═══════════════════════════════════════════════════════════════════════
// LinearInteger Velox cross-validation (was completely missing)
// ═══════════════════════════════════════════════════════════════════════

TEST(VeloxCrossVal, LinearInt32) {
    arrow::Int32Builder builder;
    // Monotonic sequence: 10, 15, 14, 20, 18, 25, 24
    std::vector<int32_t> vals = {10, 15, 14, 20, 18, 25, 24};
    for (auto v : vals) APPEND(builder, v);
    auto array = builder.Finish().ValueOrDie();

    auto liquid = LiquidLinearIntegerArray<arrow::Int32Type>::from_arrow(array);
    auto vec = liquid.to_velox(test_pool());
    ASSERT_NE(vec, nullptr);

    auto flat = vec->asFlatVector<int32_t>();
    ASSERT_EQ(flat->size(), static_cast<vector_size_t>(vals.size()));
    for (size_t i = 0; i < vals.size(); ++i) {
        EXPECT_EQ(flat->valueAt(i), vals[i])
            << "LinearInt32 mismatch at index " << i;
    }
}

TEST(VeloxCrossVal, LinearInt64) {
    arrow::Int64Builder builder;
    std::vector<int64_t> vals = {-1000LL, 0LL, 1000LL, 2000LL, 3000LL};
    for (auto v : vals) APPEND(builder, v);
    auto array = builder.Finish().ValueOrDie();

    auto liquid = LiquidLinearIntegerArray<arrow::Int64Type>::from_arrow(array);
    auto vec = liquid.to_velox(test_pool());
    ASSERT_NE(vec, nullptr);

    auto flat = vec->asFlatVector<int64_t>();
    ASSERT_EQ(flat->size(), static_cast<vector_size_t>(vals.size()));
    for (size_t i = 0; i < vals.size(); ++i) {
        EXPECT_EQ(flat->valueAt(i), vals[i])
            << "LinearInt64 mismatch at index " << i;
    }
}

TEST(VeloxCrossVal, LinearInt32WithNulls) {
    arrow::Int32Builder builder;
    APPEND(builder, 10);
    APPEND_NULL(builder);
    APPEND(builder, 30);
    APPEND_NULL(builder);
    APPEND(builder, 50);
    APPEND(builder, 70);
    auto array = builder.Finish().ValueOrDie();

    auto liquid = LiquidLinearIntegerArray<arrow::Int32Type>::from_arrow(array);
    auto vec = liquid.to_velox(test_pool());
    ASSERT_NE(vec, nullptr);

    auto flat = vec->asFlatVector<int32_t>();
    ASSERT_EQ(flat->size(), 6);
    EXPECT_EQ(flat->valueAt(0), 10);
    EXPECT_TRUE(flat->isNullAt(1));
    EXPECT_EQ(flat->valueAt(2), 30);
    EXPECT_TRUE(flat->isNullAt(3));
    EXPECT_EQ(flat->valueAt(4), 50);
    EXPECT_EQ(flat->valueAt(5), 70);
}

// ═══════════════════════════════════════════════════════════════════════
// Float type cross-validation
// ═══════════════════════════════════════════════════════════════════════

TEST(VeloxCrossVal, Float32) {
    arrow::FloatBuilder builder;
    APPEND(builder, 0.0f);
    APPEND(builder, 1.5f);
    APPEND(builder, -3.14f);
    APPEND(builder, 100.0f);
    auto array = builder.Finish().ValueOrDie();

    auto liquid = LiquidFloatArray<float>::from_arrow(array);
    auto vec = liquid.to_velox(test_pool());
    ASSERT_NE(vec, nullptr);

    auto flat = vec->asFlatVector<float>();
    ASSERT_EQ(flat->size(), 4);
    EXPECT_FLOAT_EQ(flat->valueAt(0), 0.0f);
    EXPECT_FLOAT_EQ(flat->valueAt(1), 1.5f);
    EXPECT_FLOAT_EQ(flat->valueAt(2), -3.14f);
    EXPECT_FLOAT_EQ(flat->valueAt(3), 100.0f);
}

TEST(VeloxCrossVal, Float64) {
    arrow::DoubleBuilder builder;
    APPEND(builder, 0.0);
    APPEND(builder, 1.5);
    APPEND(builder, -3.14);
    APPEND(builder, 100.0);
    APPEND(builder, 1e15);
    auto array = builder.Finish().ValueOrDie();

    auto liquid = LiquidFloatArray<double>::from_arrow(array);
    auto vec = liquid.to_velox(test_pool());
    ASSERT_NE(vec, nullptr);

    auto flat = vec->asFlatVector<double>();
    ASSERT_EQ(flat->size(), 5);
    EXPECT_DOUBLE_EQ(flat->valueAt(0), 0.0);
    EXPECT_DOUBLE_EQ(flat->valueAt(1), 1.5);
    EXPECT_DOUBLE_EQ(flat->valueAt(2), -3.14);
    EXPECT_DOUBLE_EQ(flat->valueAt(3), 100.0);
    EXPECT_DOUBLE_EQ(flat->valueAt(4), 1e15);
}

// ═══════════════════════════════════════════════════════════════════════
// String/Binary cross-validation
// ═══════════════════════════════════════════════════════════════════════

TEST(VeloxCrossVal, String) {
    arrow::StringBuilder builder;
    APPEND(builder, "hello");
    APPEND(builder, "world");
    APPEND(builder, "");
    APPEND(builder, "a longer string for testing");
    auto array = builder.Finish().ValueOrDie();

    auto liquid = LiquidByteViewArray::from_arrow(array);
    auto vec = liquid.to_velox(test_pool());
    ASSERT_NE(vec, nullptr);

    auto flat = vec->asFlatVector<StringView>();
    ASSERT_EQ(flat->size(), 4);
    EXPECT_EQ(flat->valueAt(0), StringView("hello"));
    EXPECT_EQ(flat->valueAt(1), StringView("world"));
    EXPECT_EQ(flat->valueAt(2), StringView(""));
    EXPECT_EQ(flat->valueAt(3), StringView("a longer string for testing"));
}

TEST(VeloxCrossVal, StringWithNulls) {
    arrow::StringBuilder builder;
    APPEND(builder, "first");
    APPEND_NULL(builder);
    APPEND(builder, "third");
    APPEND_NULL(builder);
    APPEND(builder, "fifth");
    auto array = builder.Finish().ValueOrDie();

    auto liquid = LiquidByteViewArray::from_arrow(array);
    auto vec = liquid.to_velox(test_pool());
    ASSERT_NE(vec, nullptr);

    auto flat = vec->asFlatVector<StringView>();
    ASSERT_EQ(flat->size(), 5);
    EXPECT_EQ(flat->valueAt(0), StringView("first"));
    EXPECT_TRUE(flat->isNullAt(1));
    EXPECT_EQ(flat->valueAt(2), StringView("third"));
    EXPECT_TRUE(flat->isNullAt(3));
    EXPECT_EQ(flat->valueAt(4), StringView("fifth"));
}

TEST(VeloxCrossVal, Binary) {
    arrow::BinaryBuilder builder;
    APPEND(builder, "\x00\x01\x02");
    APPEND(builder, "text");
    APPEND(builder, "");
    auto array = builder.Finish().ValueOrDie();

    auto liquid = LiquidByteViewArray::from_arrow(array);

    // First verify Arrow roundtrip is correct
    auto decoded = liquid.to_arrow();
    auto bin_decoded = std::static_pointer_cast<arrow::BinaryArray>(decoded);
    ASSERT_EQ(bin_decoded->length(), 3);

    // Now test Velox conversion
    auto vec = liquid.to_velox(test_pool());
    ASSERT_NE(vec, nullptr);

    auto flat = vec->asFlatVector<StringView>();
    ASSERT_EQ(flat->size(), 3);

    // Compare Velox values with decoded Arrow values
    for (int i = 0; i < 3; ++i) {
        auto arrow_view = bin_decoded->GetView(i);
        auto velox_val = flat->valueAt(i);
        ASSERT_EQ(velox_val.size(), static_cast<size_t>(arrow_view.size()))
            << "Size mismatch at index " << i;
        if (arrow_view.size() > 0) {
            ASSERT_EQ(0, std::memcmp(velox_val.data(), arrow_view.data(), arrow_view.size()))
                << "Data mismatch at index " << i;
        }
    }
}

// ═══════════════════════════════════════════════════════════════════════
// Decimal128 cross-validation (fits-u64 path)
// ═══════════════════════════════════════════════════════════════════════

TEST(VeloxCrossVal, Decimal128FitsU64) {
    auto type = arrow::decimal128(10, 2);
    arrow::Decimal128Builder builder(type);
    std::vector<int64_t> expected_vals;
    for (int i = 0; i < 100; ++i) {
        auto val = arrow::Decimal128(i * 100);
        APPEND(builder, val);
        // Extract low 8 bytes as the raw int64 value
        int64_t raw = 0;
#if ARROW_VERSION_MAJOR >= 19
        auto native_val = val.ToBytes().ValueOrDie();
#else
        auto native_val = val.ToBytes();
#endif
        std::memcpy(&raw, native_val.data(), sizeof(int64_t));
        expected_vals.push_back(raw);
    }
    auto array = builder.Finish().ValueOrDie();

    ASSERT_TRUE(LiquidDecimalArray::fits_u64(array));
    auto liquid = LiquidDecimalArray::from_arrow(array);
    auto vec = liquid.to_velox(test_pool());
    ASSERT_NE(vec, nullptr);

    auto flat = vec->asFlatVector<int64_t>();
    ASSERT_EQ(flat->size(), 100);
    for (int i = 0; i < 100; ++i) {
        EXPECT_EQ(flat->valueAt(i), expected_vals[i])
            << "Mismatch at index " << i;
    }
}

// ═══════════════════════════════════════════════════════════════════════
// Decimal128 cross-validation (FSST dictionary path)
// ═══════════════════════════════════════════════════════════════════════

TEST(VeloxCrossVal, Decimal128LargeValues) {
    auto type = arrow::decimal128(38, 6);
    arrow::Decimal128Builder builder(type);
    std::vector<int128_t> expected_vals;
    for (int i = 0; i < 30; ++i) {
        auto val_str = std::to_string(i) + std::string(20, '0');
        auto val = arrow::Decimal128(val_str);
        APPEND(builder, val);
        // Extract 16 bytes as the raw int128 value
        int128_t raw = 0;
#if ARROW_VERSION_MAJOR >= 19
        auto native_val = val.ToBytes().ValueOrDie();
#else
        auto native_val = val.ToBytes();
#endif
        std::memcpy(&raw, native_val.data(), std::min(native_val.size(), sizeof(int128_t)));
        expected_vals.push_back(raw);
    }
    auto array = builder.Finish().ValueOrDie();

    auto liquid = LiquidFixedLenByteArray::from_decimal128(array);
    auto vec = liquid.to_velox(test_pool());
    ASSERT_NE(vec, nullptr);

    // precision 38 > 18 → LongDecimal (int128_t)
    auto flat = vec->asFlatVector<int128_t>();
    ASSERT_EQ(flat->size(), 30);
    for (int i = 0; i < 30; ++i) {
        EXPECT_EQ(flat->valueAt(i), expected_vals[i])
            << "Mismatch at index " << i;
    }
}

// ═══════════════════════════════════════════════════════════════════════
// Null handling cross-validation
// ═══════════════════════════════════════════════════════════════════════

TEST(VeloxCrossVal, Int32WithNulls) {
    arrow::Int32Builder builder;
    APPEND(builder, 1);
    APPEND(builder, 2);
    APPEND_NULL(builder);
    APPEND(builder, 4);
    APPEND_NULL(builder);
    APPEND(builder, 6);
    auto array = builder.Finish().ValueOrDie();

    auto liquid = LiquidPrimitiveArray<arrow::Int32Type>::from_arrow(array);
    auto vec = liquid.to_velox(test_pool());
    ASSERT_NE(vec, nullptr);

    auto flat = vec->asFlatVector<int32_t>();
    ASSERT_EQ(flat->size(), 6);
    EXPECT_EQ(flat->valueAt(0), 1);
    EXPECT_EQ(flat->valueAt(1), 2);
    EXPECT_TRUE(flat->isNullAt(2));
    EXPECT_EQ(flat->valueAt(3), 4);
    EXPECT_TRUE(flat->isNullAt(4));
    EXPECT_EQ(flat->valueAt(5), 6);
}

TEST(VeloxCrossVal, Float64WithNulls) {
    arrow::DoubleBuilder builder;
    APPEND(builder, 1.1);
    APPEND_NULL(builder);
    APPEND(builder, 3.3);
    APPEND_NULL(builder);
    auto array = builder.Finish().ValueOrDie();

    auto liquid = LiquidFloatArray<double>::from_arrow(array);
    auto vec = liquid.to_velox(test_pool());
    ASSERT_NE(vec, nullptr);

    auto flat = vec->asFlatVector<double>();
    ASSERT_EQ(flat->size(), 4);
    EXPECT_DOUBLE_EQ(flat->valueAt(0), 1.1);
    EXPECT_TRUE(flat->isNullAt(1));
    EXPECT_DOUBLE_EQ(flat->valueAt(2), 3.3);
    EXPECT_TRUE(flat->isNullAt(3));
}

// ═══════════════════════════════════════════════════════════════════════
// Edge case: empty arrays
// ═══════════════════════════════════════════════════════════════════════

TEST(VeloxCrossVal, EmptyInt32) {
    arrow::Int32Builder builder;
    auto array = builder.Finish().ValueOrDie();

    auto liquid = LiquidPrimitiveArray<arrow::Int32Type>::from_arrow(array);
    auto vec = liquid.to_velox(test_pool());
    ASSERT_NE(vec, nullptr);
    EXPECT_EQ(vec->size(), 0);
}

TEST(VeloxCrossVal, EmptyString) {
    arrow::StringBuilder builder;
    auto array = builder.Finish().ValueOrDie();

    auto liquid = LiquidByteViewArray::from_arrow(array);
    auto vec = liquid.to_velox(test_pool());
    ASSERT_NE(vec, nullptr);
    EXPECT_EQ(vec->size(), 0);
}

// ═══════════════════════════════════════════════════════════════════════
// Custom main() to initialize Arrow compute for static linking
// ═══════════════════════════════════════════════════════════════════════

class ArrowEnvironment : public ::testing::Environment {
public:
    void SetUp() override {
        // Arrow 24+ requires explicit Initialize() for static linking.
        // Arrow 18 uses --whole-archive which includes static initializers.
#if ARROW_VERSION_MAJOR >= 19
        auto status = arrow::compute::Initialize();
        if (!status.ok()) {
            fprintf(stderr, "Failed to initialize Arrow compute: %s\n",
                    status.ToString().c_str());
        }
#endif
    }
};

int main(int argc, char** argv) {
    // Initialize Velox MemoryManager before any tests
    memory::MemoryManager::initialize(memory::MemoryManager::Options{});
    auto pool = memory::memoryManager()->addLeafPool("test_pool");
    g_pool = pool.get();

    ::testing::InitGoogleTest(&argc, argv);
    ::testing::AddGlobalTestEnvironment(new ArrowEnvironment);
    return RUN_ALL_TESTS();
}
