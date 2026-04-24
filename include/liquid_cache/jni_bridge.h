// liquid_cache/jni_bridge.h
// JNI bridge for Liquid Cache - C++ implementation
// Provides the same interface as the Rust JNI layer, allowing direct
// integration from Spark JVM into C++ Liquid Cache operations.
//
// This header demonstrates how a C++ JNI layer would interact with
// the Liquid Cache format, replacing or complementing the Rust JNI bridge.
#pragma once

#include <jni.h>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>
#include <atomic>

#include "liquid_cache/ipc_header.h"
#include "liquid_cache/transcoder.h"

namespace liquid_cache {
namespace jni {

// ═══════════════════════════════════════════════════════════════════════
// Session and Result Handle Management
// Mirrors the Rust session.rs handle allocation pattern.
// ═══════════════════════════════════════════════════════════════════════

/// Atomic handle counter for session/result allocation.
inline std::atomic<int64_t>& next_handle() {
    static std::atomic<int64_t> h{1};
    return h;
}

inline int64_t alloc_handle() {
    return next_handle().fetch_add(1, std::memory_order_relaxed);
}

/// Represents a cached scan result containing transcoded batches.
/// Each batch is stored as LiquidEncodedArray per column.
struct ScanResult {
    /// Each element is one batch: a vector of per-column encoded arrays.
    std::vector<std::vector<LiquidEncodedArray>> batches;
    std::atomic<size_t> current_index{0};

    /// Returns the next batch, or nullptr when exhausted.
    const std::vector<LiquidEncodedArray>* next_batch() {
        size_t idx = current_index.fetch_add(1, std::memory_order_relaxed);
        if (idx < batches.size()) return &batches[idx];
        return nullptr;
    }
};

/// Global session and result stores (thread-safe).
inline std::mutex& sessions_mutex() {
    static std::mutex m;
    return m;
}

inline std::unordered_map<int64_t, std::shared_ptr<void>>& sessions() {
    static std::unordered_map<int64_t, std::shared_ptr<void>> s;
    return s;
}

inline std::mutex& results_mutex() {
    static std::mutex m;
    return m;
}

inline std::unordered_map<int64_t, std::shared_ptr<ScanResult>>& results() {
    static std::unordered_map<int64_t, std::shared_ptr<ScanResult>> r;
    return r;
}

inline int64_t store_result(std::shared_ptr<ScanResult> result) {
    int64_t handle = alloc_handle();
    std::lock_guard<std::mutex> lock(results_mutex());
    results()[handle] = std::move(result);
    return handle;
}

inline std::shared_ptr<ScanResult> get_result(int64_t handle) {
    std::lock_guard<std::mutex> lock(results_mutex());
    auto it = results().find(handle);
    if (it != results().end()) return it->second;
    return nullptr;
}

inline void remove_result(int64_t handle) {
    std::lock_guard<std::mutex> lock(results_mutex());
    results().erase(handle);
}

// ═══════════════════════════════════════════════════════════════════════
// JNI Helper Functions
// ═══════════════════════════════════════════════════════════════════════

/// Convert a Java String to a C++ std::string.
inline std::string jstring_to_string(JNIEnv* env, jstring jstr) {
    if (!jstr) return "";
    const char* chars = env->GetStringUTFChars(jstr, nullptr);
    std::string result(chars);
    env->ReleaseStringUTFChars(jstr, chars);
    return result;
}

/// Convert a Java String[] to a vector of strings.
inline std::vector<std::string> jobject_array_to_vec(JNIEnv* env, jobjectArray arr) {
    std::vector<std::string> result;
    if (!arr) return result;
    jsize len = env->GetArrayLength(arr);
    result.reserve(len);
    for (jsize i = 0; i < len; ++i) {
        auto jstr = static_cast<jstring>(env->GetObjectArrayElement(arr, i));
        result.push_back(jstring_to_string(env, jstr));
        env->DeleteLocalRef(jstr);
    }
    return result;
}

/// Throw a Java RuntimeException with the given message.
inline void throw_runtime_exception(JNIEnv* env, const char* msg) {
    jclass cls = env->FindClass("java/lang/RuntimeException");
    if (cls) env->ThrowNew(cls, msg);
}

// ═══════════════════════════════════════════════════════════════════════
// Arrow IPC Serialization Helpers
//
// These functions serialize/deserialize Arrow RecordBatch to/from
// Arrow IPC Stream format, matching the Rust batch_to_ipc_bytes().
// Requires Arrow C++ library linked.
// ═══════════════════════════════════════════════════════════════════════

/// Serialize transcoded columns into a simple binary format
/// that the Java side can parse. For full compatibility with the existing
/// Scala code, this should produce Arrow IPC bytes.
///
/// This is a simplified version; a production implementation would use
/// arrow::ipc::MakeStreamWriter to produce standard Arrow IPC format.
inline std::vector<uint8_t> encode_batch_to_liquid_ipc(
        const std::vector<LiquidEncodedArray>& columns) {
    // Simple format: [num_columns: u32] [per column: length_u32 + bytes]
    std::vector<uint8_t> out;
    uint32_t num_cols = static_cast<uint32_t>(columns.size());
    const uint8_t* np = reinterpret_cast<const uint8_t*>(&num_cols);
    out.insert(out.end(), np, np + 4);

    for (const auto& col : columns) {
        uint32_t col_len = static_cast<uint32_t>(col.serialized_bytes.size());
        const uint8_t* lp = reinterpret_cast<const uint8_t*>(&col_len);
        out.insert(out.end(), lp, lp + 4);
        out.insert(out.end(), col.serialized_bytes.begin(),
                   col.serialized_bytes.end());
    }
    return out;
}

}  // namespace jni
}  // namespace liquid_cache

// ═══════════════════════════════════════════════════════════════════════
// JNI Entry Points
//
// These C functions implement the native methods declared in
// org.apache.spark.sql.execution.liquidcache.LiquidCacheNative
//
// Method signatures match the Scala @native declarations.
// ═══════════════════════════════════════════════════════════════════════

#ifdef __cplusplus
extern "C" {
#endif

/// Create a new session connected to the given Liquid Cache server.
/// Returns a session handle.
JNIEXPORT jlong JNICALL
Java_org_apache_spark_sql_execution_liquidcache_LiquidCacheNative_createSession(
        JNIEnv* env, jclass cls, jstring serverAddress);

/// Register an object store with the server.
JNIEXPORT void JNICALL
Java_org_apache_spark_sql_execution_liquidcache_LiquidCacheNative_registerObjectStore(
        JNIEnv* env, jclass cls, jlong session, jstring url, jobjectArray options);

/// Register a parquet file as a named table.
JNIEXPORT void JNICALL
Java_org_apache_spark_sql_execution_liquidcache_LiquidCacheNative_registerParquet(
        JNIEnv* env, jclass cls, jlong session, jstring tableName, jstring filePath);

/// Execute a scan and return a result handle.
JNIEXPORT jlong JNICALL
Java_org_apache_spark_sql_execution_liquidcache_LiquidCacheNative_executeScan(
        JNIEnv* env, jclass cls, jlong session, jstring tableName,
        jobjectArray columns, jint batchSize);

/// Fetch the next Arrow IPC batch from a result handle.
/// Returns null when all batches are consumed.
JNIEXPORT jbyteArray JNICALL
Java_org_apache_spark_sql_execution_liquidcache_LiquidCacheNative_fetchNextBatch(
        JNIEnv* env, jclass cls, jlong resultHandle);

/// Close a result handle.
JNIEXPORT void JNICALL
Java_org_apache_spark_sql_execution_liquidcache_LiquidCacheNative_closeResult(
        JNIEnv* env, jclass cls, jlong resultHandle);

/// Close a session handle.
JNIEXPORT void JNICALL
Java_org_apache_spark_sql_execution_liquidcache_LiquidCacheNative_closeSession(
        JNIEnv* env, jclass cls, jlong sessionHandle);

#ifdef __cplusplus
}
#endif
