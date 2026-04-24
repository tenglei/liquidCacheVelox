// liquid_cache_example.cpp
// LiquidCacheStore Benchmark & Verification
//
// Loads Parquet data into LiquidCacheStore (in-memory Liquid structs),
// verifies round-trip correctness, and benchmarks CacheStore decode
// performance vs direct Parquet reads.
//
// Usage:
//   liquid_cache_example <parquet_file_or_directory> [mode]
//
//   mode:
//     bench    - (default) Performance benchmark: Parquet vs CacheStore
//     verify   - Round-trip correctness verification
//
// Build:
//   mkdir build && cd build
//   cmake .. -DCMAKE_PREFIX_PATH=/path/to/arrow
//   cmake --build .
//   ./liquid_cache_example path/to/data.parquet

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <numeric>
#include <sstream>
#include <string>
#include <vector>

#include <arrow/api.h>
#include <arrow/io/file.h>
#include <arrow/compute/initialize.h>
#include <parquet/arrow/reader.h>

#include "liquid_cache/liquid_array.h"
#include "liquid_cache/liquid_cache_store.h"

namespace fs = std::filesystem;
using namespace liquid_cache;

// Forward declaration (defined in transcoder_arrow.cpp)
namespace liquid_cache {
LiquidArrayRef transcode_to_liquid_array(
    const std::shared_ptr<arrow::Array>& array);
}

// ═══════════════════════════════════════════════════════════════════════
// Benchmark Utilities
// ═══════════════════════════════════════════════════════════════════════

using SteadyClock = std::chrono::steady_clock;
using Duration    = std::chrono::duration<double>;  // seconds

/// Prevent the compiler from optimizing away a computed value.
template <typename T>
inline void benchmark_escape(const T& val) {
#if defined(__GNUC__) || defined(__clang__)
    asm volatile("" : : "g"(&val) : "memory");
#else
    volatile auto sink = &val;
    (void)sink;
#endif
}

/// Format time value (ms) with auto-scaled units.
inline std::string fmt_time_ms(double ms) {
    std::ostringstream oss;
    if (ms >= 1.0) {
        oss << std::fixed << std::setprecision(2) << ms << " ms";
    } else if (ms >= 0.001) {
        oss << std::fixed << std::setprecision(1) << (ms * 1000.0) << " us";
    } else if (ms > 0) {
        oss << std::fixed << std::setprecision(1) << (ms * 1000000.0) << " ns";
    } else {
        oss << "0.00 ms";
    }
    return oss.str();
}

/// Format time value (ms) as fixed-width numeric field for tables.
inline std::string fmt_time_ms_table(double ms, int width = 10) {
    std::ostringstream oss;
    if (ms >= 1.0) {
        oss << std::fixed << std::setprecision(2) << ms;
    } else if (ms >= 0.01) {
        oss << std::fixed << std::setprecision(4) << ms;
    } else {
        oss << std::fixed << std::setprecision(6) << ms;
    }
    std::string s = oss.str();
    while (static_cast<int>(s.size()) < width) s = " " + s;
    return s;
}

// ═══════════════════════════════════════════════════════════════════════
// File Discovery
// ═══════════════════════════════════════════════════════════════════════

/// Collect all .parquet files from a path (file or directory, recursive).
std::vector<std::string> collect_parquet_files(const std::string& path) {
    std::vector<std::string> files;
    fs::path p(path);

    if (fs::is_regular_file(p)) {
        auto ext = p.extension().string();
        std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
        if (ext == ".parquet" || ext == ".pqt") {
            files.push_back(p.string());
        }
    } else if (fs::is_directory(p)) {
        for (const auto& entry : fs::recursive_directory_iterator(p)) {
            if (!entry.is_regular_file()) continue;
            auto ext = entry.path().extension().string();
            std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
            if (ext == ".parquet" || ext == ".pqt") {
                files.push_back(entry.path().string());
            }
        }
        std::sort(files.begin(), files.end());
    }
    return files;
}

/// Read schema from the first Parquet file.
std::shared_ptr<arrow::Schema> get_schema_from_files(
        const std::vector<std::string>& files) {
    if (files.empty()) return nullptr;
    auto maybe_infile = arrow::io::ReadableFile::Open(files[0]);
    if (!maybe_infile.ok()) return nullptr;
    auto reader_result = parquet::arrow::OpenFile(
        maybe_infile.ValueOrDie(), arrow::default_memory_pool());
    if (!reader_result.ok()) return nullptr;
    auto reader = std::move(reader_result).ValueOrDie();
    std::shared_ptr<arrow::Schema> schema;
    if (!reader->GetSchema(&schema).ok()) return nullptr;
    return schema;
}

// ═══════════════════════════════════════════════════════════════════════
// Benchmark Scenarios
// ═══════════════════════════════════════════════════════════════════════

struct ColumnScenario {
    std::string name;
    std::vector<int> col_indices;  // empty = all columns
    std::string description;
};

/// Return benchmark scenarios tuned for the 20-column test schema.
/// Covers each encoding type individually, multi-column projections,
/// and full table scan.
std::vector<ColumnScenario> get_bench_scenarios(int num_cols) {
    if (num_cols == 20) {
        return {
            // --- Per-type single-column scenarios ---
            {"Int32",           {2},                 "col_int32 (FoR+BitPacking)"},
            {"Int64",           {3},                 "col_int64 (FoR+BitPacking)"},
            {"Float64",         {9},                 "col_float64 (ALP)"},
            {"Timestamp",       {14},                "col_ts_us (FoR+BitPacking)"},
            {"String Low",      {17},                "col_string_low (FSST+Dict, low card)"},
            {"String High",     {16},                "col_string_high (FSST+Dict, high card)"},
            {"Decimal128",      {19},                "col_decimal (FoR+BitPacking)"},
            // --- Multi-column projection scenarios ---
            {"Analytics 3-col", {2, 9, 16},          "int32 + float64 + string_high"},
            {"Analytics 5-col", {2, 3, 9, 14, 17},   "int32 + int64 + float64 + ts_us + string_low"},
            // --- Full table ---
            {"Full Table",      {},                   "all 20 columns"},
        };
    }
    // Fallback for non-standard schemas
    std::vector<ColumnScenario> scenarios;
    if (num_cols >= 1)
        scenarios.push_back({"Single Column", {0}, "first column"});
    if (num_cols >= 3)
        scenarios.push_back({"3 Columns", {0, num_cols / 2, num_cols - 1},
                             "first, middle, last"});
    scenarios.push_back({"Full Table", {}, "all columns"});
    return scenarios;
}

// ═══════════════════════════════════════════════════════════════════════
// Parquet Baseline Read
// ═══════════════════════════════════════════════════════════════════════

struct ReadResult {
    size_t rows_read = 0;
    size_t bytes_read = 0;
};

/// Read selected columns from Parquet files. Parquet reader opens the file
/// each time, but OS page cache makes subsequent reads fast (simulating
/// the warm-cache scenario that real analytics engines encounter).
ReadResult read_parquet_cols_once(const std::vector<std::string>& files,
                                  const std::vector<int>& col_indices) {
    ReadResult res;
    for (const auto& path : files) {
        auto maybe_infile = arrow::io::ReadableFile::Open(path);
        if (!maybe_infile.ok()) continue;
        auto infile = maybe_infile.ValueOrDie();

        auto reader_result = parquet::arrow::OpenFile(
            infile, arrow::default_memory_pool());
        if (!reader_result.ok()) continue;
        auto reader = std::move(reader_result).ValueOrDie();
        reader->set_batch_size(8192);

        arrow::Result<std::unique_ptr<arrow::RecordBatchReader>> rb_result;
        if (col_indices.empty()) {
            rb_result = reader->GetRecordBatchReader();
        } else {
            std::vector<int> all_rg(reader->num_row_groups());
            std::iota(all_rg.begin(), all_rg.end(), 0);
            rb_result = reader->GetRecordBatchReader(all_rg, col_indices);
        }
        if (!rb_result.ok()) continue;
        auto batch_reader = std::move(rb_result).ValueOrDie();

        while (true) {
            std::shared_ptr<arrow::RecordBatch> batch;
            auto st = batch_reader->ReadNext(&batch);
            if (!st.ok() || !batch) break;
            res.rows_read += batch->num_rows();
            for (int c = 0; c < batch->num_columns(); ++c) {
                auto col = batch->column(c);
                for (size_t b = 0; b < col->data()->buffers.size(); ++b) {
                    if (auto buf = col->data()->buffers[b]) {
                        res.bytes_read += buf->size();
                        volatile uint8_t sink = buf->data()[0];
                        (void)sink;
                    }
                }
            }
        }
    }
    return res;
}

// ═══════════════════════════════════════════════════════════════════════
// Verify Mode: Round-trip Correctness
// ═══════════════════════════════════════════════════════════════════════

/// Verify every column of every batch: Arrow -> Liquid -> Arrow round-trip.
void run_verify(const std::vector<std::string>& files) {
    std::cout << "\n";
    std::cout << "╔══════════════════════════════════════════════════════════╗\n";
    std::cout << "║  ROUND-TRIP CORRECTNESS VERIFICATION                    ║\n";
    std::cout << "║  Arrow -> Liquid (in-memory struct) -> Arrow            ║\n";
    std::cout << "╚══════════════════════════════════════════════════════════╝\n";

    size_t total_pass = 0, total_fail = 0, total_cols = 0;
    size_t total_batches = 0, total_rows = 0;

    for (const auto& path : files) {
        std::cout << "\n  File: " << path << "\n";

        auto maybe_infile = arrow::io::ReadableFile::Open(path);
        if (!maybe_infile.ok()) {
            std::cerr << "  ERROR: Cannot open file\n";
            continue;
        }
        auto reader_result = parquet::arrow::OpenFile(
            maybe_infile.ValueOrDie(), arrow::default_memory_pool());
        if (!reader_result.ok()) continue;
        auto reader = std::move(reader_result).ValueOrDie();
        reader->set_batch_size(8192);

        std::shared_ptr<arrow::Schema> schema;
        if (!reader->GetSchema(&schema).ok()) continue;

        auto rb_result = reader->GetRecordBatchReader();
        if (!rb_result.ok()) continue;
        auto batch_reader = std::move(rb_result).ValueOrDie();

        size_t file_batches = 0;
        while (true) {
            std::shared_ptr<arrow::RecordBatch> batch;
            auto st = batch_reader->ReadNext(&batch);
            if (!st.ok() || !batch) break;

            total_rows += batch->num_rows();
            file_batches++;

            for (int c = 0; c < batch->num_columns(); ++c) {
                total_cols++;
                auto original = batch->column(c);

                // Transcode to in-memory Liquid struct
                auto liquid = transcode_to_liquid_array(original);
                if (!liquid) {
                    total_fail++;
                    std::cout << "    FAIL (transcode): batch " << file_batches
                              << " col " << c << " ("
                              << schema->field(c)->name() << ")\n";
                    continue;
                }

                // Decode back to Arrow
                auto decoded = liquid->to_arrow();
                if (!decoded || !original->Equals(*decoded)) {
                    total_fail++;
                    std::cout << "    FAIL (mismatch): batch " << file_batches
                              << " col " << c << " ("
                              << schema->field(c)->name() << ")\n";
                } else {
                    total_pass++;
                }
            }
        }
        total_batches += file_batches;
        std::cout << "    Batches: " << file_batches
                  << ", Columns per batch: " << schema->num_fields() << "\n";
    }

    std::cout << "\n╔══════════════════════════════════════════════════════════╗\n";
    std::cout << "║  VERIFICATION SUMMARY                                    ║\n";
    std::cout << "╠══════════════════════════════════════════════════════════╣\n";
    std::cout << "║  Total rows:           " << std::setw(10) << total_rows
              << "                     ║\n";
    std::cout << "║  Total batches:        " << std::setw(10) << total_batches
              << "                     ║\n";
    std::cout << "║  Columns tested:       " << std::setw(10) << total_cols
              << "                     ║\n";
    std::cout << "║  Round-trip PASS:      " << std::setw(10) << total_pass
              << "                     ║\n";
    std::cout << "║  Round-trip FAIL:      " << std::setw(10) << total_fail
              << "                     ║\n";
    std::cout << "╚══════════════════════════════════════════════════════════╝\n";
}

// ═══════════════════════════════════════════════════════════════════════
// Bench Mode: CacheStore vs Parquet
// ═══════════════════════════════════════════════════════════════════════

/// Main benchmark: load all data into LiquidCacheStore, then compare
/// CacheStore to_arrow() decode speed vs Parquet read speed for each
/// scenario. Both sides operate from hot caches (OS page cache for
/// Parquet, in-memory structs for CacheStore).
void run_bench(const std::vector<std::string>& files) {
    auto schema = get_schema_from_files(files);
    if (!schema) {
        std::cerr << "Cannot read schema.\n";
        return;
    }
    int num_cols = schema->num_fields();
    auto scenarios = get_bench_scenarios(num_cols);

    static constexpr int ITERS = 50;
    static constexpr int WARMUP = 3;

    std::cout << "\n";
    std::cout << "╔══════════════════════════════════════════════════════════════════════════════╗\n";
    std::cout << "║              LIQUID CACHE STORE BENCHMARK                                    ║\n";
    std::cout << "║  Parquet (hot page cache) vs CacheStore (in-memory Liquid structs)           ║\n";
    std::cout << "║  " << ITERS << " iterations, "
              << WARMUP << " warmup per scenario"
              << "                                           ║\n";
    std::cout << "╚══════════════════════════════════════════════════════════════════════════════╝\n";

    // --- Load all data into CacheStore ---
    LiquidCacheStore store;
    std::shared_ptr<arrow::Schema> cache_schema;
    double transcode_sec = 0;
    auto rg_infos = store.load_from_parquet(
        files, cache_schema, transcode_sec, transcode_to_liquid_array);
    auto st = store.stats();

    size_t total_rows = 0;
    for (auto& rg : rg_infos) total_rows += rg.total_rows;

    std::cout << "\n  Cache loaded:\n";
    std::cout << "    Entries:       " << st.entry_count
              << " (Liquid: " << st.liquid_entries
              << ", Arrow: " << st.arrow_entries << ")\n";
    std::cout << "    Memory:        "
              << std::fixed << std::setprecision(1)
              << (st.total_memory_bytes / (1024.0 * 1024.0)) << " MB\n";
    std::cout << "    Transcode:     "
              << fmt_time_ms(transcode_sec * 1000.0) << "\n";
    std::cout << "    Total rows:    " << total_rows << "\n";

    // --- Schema summary ---
    std::cout << "\n  Schema (" << num_cols << " columns):\n";
    for (int i = 0; i < num_cols; ++i) {
        std::cout << "    [" << std::setw(2) << i << "] "
                  << std::left << std::setw(20) << schema->field(i)->name()
                  << " " << schema->field(i)->type()->ToString() << "\n";
    }
    std::cout << std::right;  // reset

    // --- Run scenarios ---
    struct ScenarioResult {
        std::string name;
        int ncols;
        double pq_avg_ms;
        double cache_avg_ms;
        double speedup;
    };
    std::vector<ScenarioResult> results;

    for (const auto& sc : scenarios) {
        int ncols = sc.col_indices.empty() ? num_cols
                    : static_cast<int>(sc.col_indices.size());
        std::cout << "\n  --- " << sc.name << " (" << ncols
                  << " col" << (ncols > 1 ? "s" : "") << ": "
                  << sc.description << ") ---\n";

        std::vector<int> projection = sc.col_indices;

        // --- Parquet: warmup + measure ---
        for (int w = 0; w < WARMUP; ++w) {
            auto wr = read_parquet_cols_once(files, sc.col_indices);
            benchmark_escape(wr);
        }
        double pq_total = 0;
        for (int i = 0; i < ITERS; ++i) {
            auto t0 = SteadyClock::now();
            auto res = read_parquet_cols_once(files, sc.col_indices);
            auto t1 = SteadyClock::now();
            benchmark_escape(res);
            pq_total += Duration(t1 - t0).count();
        }
        double pq_avg_ms = pq_total / ITERS * 1000.0;

        // --- CacheStore: warmup + measure ---
        for (int w = 0; w < WARMUP; ++w) {
            for (const auto& rg : rg_infos) {
                for (uint16_t b = 0; b < rg.num_batches; ++b) {
                    auto rb = store.read_batch(
                        rg.file_id, rg.rg_id, b, cache_schema, projection);
                    benchmark_escape(rb);
                }
            }
        }

        double cache_total = 0;
        size_t cache_rows = 0;
        for (int i = 0; i < ITERS; ++i) {
            auto t0 = SteadyClock::now();
            size_t iter_rows = 0;
            for (const auto& rg : rg_infos) {
                for (uint16_t b = 0; b < rg.num_batches; ++b) {
                    auto rb = store.read_batch(
                        rg.file_id, rg.rg_id, b, cache_schema, projection);
                    if (rb) {
                        iter_rows += rb->num_rows();
                        benchmark_escape(rb);
                    }
                }
            }
            auto t1 = SteadyClock::now();
            cache_total += Duration(t1 - t0).count();
            cache_rows = iter_rows;
        }
        double cache_avg_ms = cache_total / ITERS * 1000.0;

        double speedup = (cache_avg_ms > 1e-9) ? pq_avg_ms / cache_avg_ms : 0;

        std::cout << "    Rows:          " << cache_rows << "\n";
        std::cout << "    Parquet avg:   " << fmt_time_ms(pq_avg_ms) << "\n";
        std::cout << "    CacheStore:    " << fmt_time_ms(cache_avg_ms) << "\n";
        std::cout << "    Speedup:       " << std::fixed << std::setprecision(2)
                  << speedup << "x\n";

        results.push_back({sc.name, ncols, pq_avg_ms, cache_avg_ms, speedup});
    }

    // --- Summary table ---
    std::cout << "\n╔═════════════════════════╦═══════╦════════════╦════════════╦══════════╗\n";
    std::cout <<   "║  Scenario               ║  Cols ║ Parquet(ms)║ Cache(ms)  ║ Speedup  ║\n";
    std::cout <<   "╠═════════════════════════╬═══════╬════════════╬════════════╬══════════╣\n";
    for (const auto& r : results) {
        std::cout << "║  " << std::left << std::setw(23) << r.name
                  << " ║ " << std::right << std::setw(5) << r.ncols
                  << " ║ " << fmt_time_ms_table(r.pq_avg_ms)
                  << " ║ " << fmt_time_ms_table(r.cache_avg_ms)
                  << " ║ " << std::setw(6) << std::fixed << std::setprecision(2)
                  << r.speedup << "x "
                  << " ║\n";
    }
    std::cout << "╚═════════════════════════╩═══════╩════════════╩════════════╩══════════╝\n";

    std::cout << "\n  Note: Parquet reads from OS page cache (hot). CacheStore decodes\n"
              << "        in-memory Liquid structs to Arrow. Speedup > 1.0 means\n"
              << "        CacheStore is faster than Parquet.\n";
}

// ═══════════════════════════════════════════════════════════════════════
// Main
// ═══════════════════════════════════════════════════════════════════════

void print_usage(const char* prog) {
    std::cout << "\nUsage: " << prog << " <parquet_path> [mode]\n\n";
    std::cout << "  parquet_path   Path to a .parquet file or directory\n\n";
    std::cout << "  mode (optional):\n";
    std::cout << "    bench    (default) Performance benchmark: Parquet vs CacheStore\n";
    std::cout << "    verify   Round-trip correctness verification\n";
    std::cout << "\n";
}

int main(int argc, char* argv[]) {
    // Initialize Arrow compute module (required for static linking)
    auto init_status = arrow::compute::Initialize();
    if (!init_status.ok()) {
        std::cerr << "ERROR: Failed to initialize Arrow compute: "
                  << init_status.ToString() << "\n";
        return 1;
    }

    std::cout << "╔══════════════════════════════════════════════════════════╗\n";
    std::cout << "║  Liquid Cache C++ - CacheStore Benchmark                ║\n";
    std::cout << "║  Compare in-memory Liquid decode vs Parquet read        ║\n";
    std::cout << "╚══════════════════════════════════════════════════════════╝\n";

    if (argc < 2) {
        print_usage(argv[0]);
        return 0;
    }

    std::string input_path = argv[1];
    std::string mode = (argc >= 3) ? argv[2] : "bench";

    if (!fs::exists(input_path)) {
        std::cerr << "Error: path does not exist: " << input_path << "\n";
        return 1;
    }

    auto files = collect_parquet_files(input_path);
    if (files.empty()) {
        std::cerr << "No .parquet files found in: " << input_path << "\n";
        return 1;
    }
    std::cout << "\nFiles: " << files.size() << "\n";

    if (mode == "bench") {
        run_bench(files);
    } else if (mode == "verify") {
        run_verify(files);
    } else {
        std::cerr << "Unknown mode: " << mode << "\n";
        print_usage(argv[0]);
        return 1;
    }

    return 0;
}
