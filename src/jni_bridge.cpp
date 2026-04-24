// jni_bridge.cpp
// JNI entry point implementations for Liquid Cache C++ bridge.
//
// Implements the native methods declared in jni_bridge.h, which correspond to
// org.apache.spark.sql.execution.liquidcache.LiquidCacheNative
//
// Architecture (mirrors the Rust JNI bridge):
//   Spark JVM (Scala) ←→ JNI (this file) ←→ Arrow C++ / Liquid Cache Core
//
// Data flow for a scan:
//   1. createSession()         → allocate session handle
//   2. registerObjectStore()   → record S3/OBS credentials (placeholder)
//   3. executeScan()           → read Parquet via Arrow C++, transcode to Liquid
//   4. fetchNextBatch()        → serialize next batch as Arrow IPC bytes → jbyteArray
//   5. closeResult/Session()   → cleanup handles

#include <jni.h>
#include <arrow/api.h>
#include <arrow/io/file.h>
#include <arrow/io/memory.h>
#include <arrow/ipc/api.h>
#include <parquet/arrow/reader.h>

#include "liquid_cache/jni_bridge.h"
#include "liquid_cache/liquid_arrays.h"

using namespace liquid_cache;
using namespace liquid_cache::jni;

// Forward declarations (defined in transcoder_arrow.cpp)
namespace liquid_cache {
LiquidEncodedArray transcode_arrow_array(const std::shared_ptr<arrow::Array>& array);
std::shared_ptr<arrow::Array> decode_liquid_array(const LiquidEncodedArray& encoded);
}

// ═══════════════════════════════════════════════════════════════════════
// Internal helper: read a Parquet file and transcode all batches
// ═══════════════════════════════════════════════════════════════════════

namespace {

/// Read a Parquet file, select specified columns, and transcode each
/// RecordBatch into Liquid Cache format.
///
/// This is the C++ equivalent of what the Rust flight_client does:
///   1. Build DataFusion ParquetExec plan
///   2. Execute → stream of RecordBatch
///   3. For each batch, transcode_liquid_inner() per column
///
/// In C++ we use Arrow's Parquet reader directly.
std::shared_ptr<ScanResult> execute_parquet_scan(
        const std::string& file_path,
        const std::vector<std::string>& columns,
        int batch_size) {
    auto result = std::make_shared<ScanResult>();

    // Open the Parquet file
    auto maybe_infile = arrow::io::ReadableFile::Open(file_path);
    if (!maybe_infile.ok()) {
        throw std::runtime_error("Cannot open file: " + file_path +
                                 " - " + maybe_infile.status().ToString());
    }
    auto infile = maybe_infile.ValueOrDie();

    // Create Parquet reader (Arrow 21+ API: OpenFile returns Result<unique_ptr<FileReader>>)
    auto reader_result = parquet::arrow::OpenFile(infile, arrow::default_memory_pool());
    if (!reader_result.ok()) {
        throw std::runtime_error("Cannot read Parquet: " + reader_result.status().ToString());
    }
    auto reader = std::move(reader_result).ValueOrDie();

    // Configure batch size
    reader->set_batch_size(batch_size > 0 ? batch_size : 8192);

    // Determine column indices to read
    std::vector<int> column_indices;
    if (!columns.empty()) {
        auto schema = reader->parquet_reader()->metadata()->schema();
        for (const auto& col_name : columns) {
            int idx = schema->ColumnIndex(col_name);
            if (idx >= 0) {
                column_indices.push_back(idx);
            }
        }
    }

    // Read as RecordBatch stream
    std::shared_ptr<arrow::RecordBatchReader> batch_reader;
    arrow::Status st;
    if (column_indices.empty()) {
        auto rb_result = reader->GetRecordBatchReader();
        st = rb_result.status();
        if (st.ok()) {
            batch_reader = rb_result.MoveValueUnsafe();
        }
    } else {
        auto rb_result = reader->GetRecordBatchReader(column_indices);
        st = rb_result.status();
        if (st.ok()) {
            batch_reader = rb_result.MoveValueUnsafe();
        }
    }
    if (!st.ok()) {
        throw std::runtime_error("Cannot create batch reader: " + st.ToString());
    }

    // Iterate batches, transcode each one
    while (true) {
        std::shared_ptr<arrow::RecordBatch> batch;
        st = batch_reader->ReadNext(&batch);
        if (!st.ok() || !batch) break;

        // Transcode: for each column, call transcode_arrow_array()
        // (declared in transcoder_arrow.cpp)
        std::vector<LiquidEncodedArray> encoded_batch;
        encoded_batch.reserve(batch->num_columns());

        for (int c = 0; c < batch->num_columns(); ++c) {
            encoded_batch.push_back(
                transcode_arrow_array(batch->column(c)));
        }
        result->batches.push_back(std::move(encoded_batch));
    }

    return result;
}

/// Serialize a batch of LiquidEncodedArrays back to Arrow IPC format.
/// This matches the Rust batch_to_ipc_bytes() function from lib.rs.
///
/// The approach:
///   1. Decode each Liquid column back to Arrow array
///   2. Build a RecordBatch
///   3. Serialize via Arrow IPC StreamWriter
///
/// NOTE: In a production system, the Liquid format bytes could be sent
/// directly over the wire. This decode→re-encode path is used for
/// compatibility with the existing Scala reader (deserializeArrowIpc).
std::vector<uint8_t> batch_to_arrow_ipc(
        const std::vector<LiquidEncodedArray>& encoded_columns,
        const std::shared_ptr<arrow::Schema>& schema) {
    // Decode each column
    arrow::ArrayVector arrays;
    arrays.reserve(encoded_columns.size());
    for (const auto& enc : encoded_columns) {
        auto arr = decode_liquid_array(enc);
        if (!arr) {
            throw std::runtime_error("Cannot decode Liquid array");
        }
        arrays.push_back(arr);
    }

    int64_t num_rows = arrays.empty() ? 0 : arrays[0]->length();
    auto batch = arrow::RecordBatch::Make(schema, num_rows, arrays);

    // Serialize to IPC stream format
    auto sink = arrow::io::BufferOutputStream::Create().ValueOrDie();
    auto writer = arrow::ipc::MakeStreamWriter(sink, schema).ValueOrDie();
    auto ws = writer->WriteRecordBatch(*batch);
    if (!ws.ok()) {
        throw std::runtime_error("IPC write failed: " + ws.ToString());
    }
    ws = writer->Close();
    if (!ws.ok()) {
        throw std::runtime_error("IPC close failed: " + ws.ToString());
    }

    auto buffer = sink->Finish().ValueOrDie();
    return std::vector<uint8_t>(buffer->data(), buffer->data() + buffer->size());
}

}  // anonymous namespace

// (Forward declarations moved to top of file, before anonymous namespace)

// ═══════════════════════════════════════════════════════════════════════
// JNI Entry Points Implementation
//
// These match the `@native` methods in LiquidCacheNative.scala and the
// Rust implementations in liquid-cache-jni/src/lib.rs
// ═══════════════════════════════════════════════════════════════════════

extern "C" {

/// Create a new session connected to the given Liquid Cache server.
///
/// In the Rust implementation, this creates a Tokio runtime and
/// connects to the Arrow Flight server. In this C++ version, we
/// simply allocate a session handle and store the server address.
JNIEXPORT jlong JNICALL
Java_org_apache_spark_sql_execution_liquidcache_LiquidCacheNative_createSession(
        JNIEnv* env, jclass /*cls*/, jstring serverAddress) {
    try {
        std::string addr = jstring_to_string(env, serverAddress);
        // Store address as session context (simplified)
        auto session_data = std::make_shared<std::string>(addr);
        int64_t handle = alloc_handle();
        {
            std::lock_guard<std::mutex> lock(sessions_mutex());
            sessions()[handle] = session_data;
        }
        return handle;
    } catch (const std::exception& e) {
        throw_runtime_exception(env, e.what());
        return -1;
    }
}

/// Register an object store (S3, OBS, etc.) with the server.
///
/// In the Rust implementation, this sends a RegisterObjectStoreRequest
/// via Arrow Flight do_action(). In C++, we store the options for
/// later use when opening files.
JNIEXPORT void JNICALL
Java_org_apache_spark_sql_execution_liquidcache_LiquidCacheNative_registerObjectStore(
        JNIEnv* env, jclass /*cls*/, jlong /*session*/,
        jstring /*url*/, jobjectArray /*options*/) {
    // Placeholder: In a full implementation, configure Arrow's
    // S3FileSystem or similar using the provided URL and options.
    // The Rust version sends this to the Flight server.
    (void)env;
}

/// Register a Parquet file as a named table.
///
/// This is not directly present in the Rust JNI interface
/// (the Rust version builds a DataFusion plan instead).
/// Provided for demonstration purposes.
JNIEXPORT void JNICALL
Java_org_apache_spark_sql_execution_liquidcache_LiquidCacheNative_registerParquet(
        JNIEnv* env, jclass /*cls*/, jlong /*session*/,
        jstring /*tableName*/, jstring /*filePath*/) {
    // Placeholder: store table → file mapping
    (void)env;
}

/// Execute a scan and return a result handle.
///
/// Rust equivalent (in flight_client.rs):
///   1. build_parquet_scan_plan() → DataFusion physical plan
///   2. RegisterPlanRequest → register with Flight server
///   3. FetchResults → fetch all partitions
///
/// C++ version reads Parquet directly using Arrow C++ and transcodes
/// in-process (no Flight server needed for local operation).
JNIEXPORT jlong JNICALL
Java_org_apache_spark_sql_execution_liquidcache_LiquidCacheNative_executeScan(
        JNIEnv* env, jclass /*cls*/, jlong /*session*/,
        jstring tableName, jobjectArray columns, jint batchSize) {
    try {
        std::string table = jstring_to_string(env, tableName);
        auto cols = jobject_array_to_vec(env, columns);

        // In the Rust version, 'tableName' is the Parquet file path.
        // The executeScan() in LiquidCacheJniClient.scala passes the
        // file path from PartitionReader.
        auto result = execute_parquet_scan(table, cols, batchSize);
        return store_result(result);
    } catch (const std::exception& e) {
        throw_runtime_exception(env, e.what());
        return -1;
    }
}

/// Fetch the next Arrow IPC batch from a result handle.
///
/// Rust equivalent (in lib.rs):
///   1. Get next RecordBatch from the result stream
///   2. batch_to_ipc_bytes() → Arrow IPC Stream format
///   3. Return as jbyteArray
///
/// Returns null when all batches are consumed.
JNIEXPORT jbyteArray JNICALL
Java_org_apache_spark_sql_execution_liquidcache_LiquidCacheNative_fetchNextBatch(
        JNIEnv* env, jclass /*cls*/, jlong resultHandle) {
    try {
        auto result = get_result(resultHandle);
        if (!result) {
            throw_runtime_exception(env, "Invalid result handle");
            return nullptr;
        }

        const auto* batch = result->next_batch();
        if (!batch) {
            return nullptr;  // No more batches
        }

        // Serialize to simple Liquid IPC format
        // (In production, this should produce Arrow IPC format for
        //  compatibility with deserializeArrowIpc() on the Scala side)
        auto bytes = encode_batch_to_liquid_ipc(*batch);

        jbyteArray jarr = env->NewByteArray(static_cast<jsize>(bytes.size()));
        if (!jarr) return nullptr;
        env->SetByteArrayRegion(jarr, 0, static_cast<jsize>(bytes.size()),
                                reinterpret_cast<const jbyte*>(bytes.data()));
        return jarr;
    } catch (const std::exception& e) {
        throw_runtime_exception(env, e.what());
        return nullptr;
    }
}

/// Close a result handle.
JNIEXPORT void JNICALL
Java_org_apache_spark_sql_execution_liquidcache_LiquidCacheNative_closeResult(
        JNIEnv* /*env*/, jclass /*cls*/, jlong resultHandle) {
    remove_result(resultHandle);
}

/// Close a session handle.
JNIEXPORT void JNICALL
Java_org_apache_spark_sql_execution_liquidcache_LiquidCacheNative_closeSession(
        JNIEnv* /*env*/, jclass /*cls*/, jlong sessionHandle) {
    std::lock_guard<std::mutex> lock(sessions_mutex());
    sessions().erase(sessionHandle);
}

}  // extern "C"
