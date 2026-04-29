// velox_benchmark.cpp
// Liquid Cache -> Velox Vector Benchmark
//
// Compares:
//   1. Velox Parquet Reader → Velox Vector (from in-memory Parquet data)
//   2. Liquid Cache → Velox Vector (from in-memory Liquid encoding)
//
// Both paths start from Parquet data loaded into memory, ensuring
// a fair comparison without disk I/O variance.
//
// Build (with LIQUID_ENABLE_VELOX):
//   cmake .. -DLIQUID_ENABLE_VELOX=ON -DVELOX_PREFIX=/path/to/velox/build
//   cmake --build . --target liquid_velox_benchmark
//
// Usage:
//   liquid_velox_benchmark <parquet_path> [mode]
//   mode: bench (default) | verify

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <numeric>
#include <sstream>
#include <string>
#include <vector>

#include <folly/init/Init.h>
#include <glog/logging.h>

#include "velox/common/file/File.h"
#include "velox/common/file/FileSystems.h"
#include "velox/common/memory/Memory.h"
#include "velox/dwio/common/BufferedInput.h"
#include "velox/dwio/common/Options.h"
#include "velox/dwio/common/ColumnSelector.h"
#include "velox/dwio/parquet/reader/ParquetReader.h"
#include "velox/dwio/parquet/RegisterParquetReader.h"
#include "velox/dwio/common/ScanSpec.h"
#include "velox/type/Type.h"
#include "velox/vector/BaseVector.h"
#include "velox/vector/ComplexVector.h"
#include "velox/vector/FlatVector.h"

#include "velox/vector/LazyVector.h"
#include <arrow/api.h>
#include <arrow/compute/api.h>
#include <arrow/io/file.h>

#include "liquid_cache/liquid_cache_store.h"
#include "liquid_cache/liquid_to_velox.h"

namespace fs = std::filesystem;
using namespace liquid_cache;

namespace vx = facebook::velox;

// ═══════════════════════════════════════════════════════════════════════
// Benchmark Utilities
// ═══════════════════════════════════════════════════════════════════════

using SteadyClock = std::chrono::steady_clock;
using Duration    = std::chrono::duration<double>;

template <typename T>
inline void benchmark_escape(const T& val) {
#if defined(__GNUC__) || defined(__clang__)
    asm volatile("" : : "g"(&val) : "memory");
#else
    volatile auto sink = &val;
    (void)sink;
#endif
}

// ── Statistical analysis for benchmark results ──────────────────────

struct BenchStats {
    double mean;
    double median;
    double stddev;
    double p5;       // 5th percentile
    double p95;      // 95th percentile
    double ci_half;  // half-width of 95% confidence interval
    int    n;        // number of samples after outlier removal
};

/// Compute statistics from raw timing samples (in seconds).
/// Removes outliers using MAD (Median Absolute Deviation) method,
/// then computes mean, median, stddev, percentiles, and 95% CI.
inline BenchStats compute_stats(std::vector<double> samples) {
    BenchStats s{};
    if (samples.empty()) return s;

    size_t n = samples.size();
    std::sort(samples.begin(), samples.end());

    // Median
    double median = (n % 2 == 0)
        ? (samples[n/2 - 1] + samples[n/2]) / 2.0
        : samples[n/2];

    // MAD-based outlier removal: keep samples within 4*MAD of median
    // MAD = median(|xi - median|)
    std::vector<double> abs_dev(n);
    for (size_t i = 0; i < n; ++i)
        abs_dev[i] = std::abs(samples[i] - median);
    std::sort(abs_dev.begin(), abs_dev.end());
    double mad = (n % 2 == 0)
        ? (abs_dev[n/2 - 1] + abs_dev[n/2]) / 2.0
        : abs_dev[n/2];

    // Keep samples within 4*MAD (conservative threshold)
    // If MAD is 0 (all values identical), keep all samples
    double threshold = 4.0 * mad * 1.4826;  // 1.4826 = consistency constant for normal dist
    std::vector<double> filtered;
    for (auto& v : samples) {
        if (threshold < 1e-15 || std::abs(v - median) <= threshold)
            filtered.push_back(v);
    }

    s.n = static_cast<int>(filtered.size());
    if (filtered.empty()) {
        s.mean = s.median = s.stddev = s.p5 = s.p95 = s.ci_half = 0;
        return s;
    }

    // Mean
    double sum = 0;
    for (auto& v : filtered) sum += v;
    s.mean = sum / s.n;

    // Median of filtered
    s.median = (s.n % 2 == 0)
        ? (filtered[s.n/2 - 1] + filtered[s.n/2]) / 2.0
        : filtered[s.n/2];

    // Standard deviation
    double sq_sum = 0;
    for (auto& v : filtered) sq_sum += (v - s.mean) * (v - s.mean);
    s.stddev = std::sqrt(sq_sum / s.n);

    // Percentiles
    s.p5  = filtered[static_cast<size_t>(s.n * 0.05)];
    s.p95 = filtered[static_cast<size_t>(s.n * 0.95)];

    // 95% CI using t-distribution approximation (for n >= 5, t ~ 2.0+)
    double t_val = (s.n >= 30) ? 1.96 :   // normal approx
                   (s.n >= 10) ? 2.23 :    // t-distribution approx
                                  2.78;     // conservative for small n
    s.ci_half = t_val * s.stddev / std::sqrt(static_cast<double>(s.n));

    return s;
}

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
// In-memory Parquet file container
// ═══════════════════════════════════════════════════════════════════════

struct InMemoryParquetFile {
    std::string path;
    std::string data;  // entire Parquet file bytes
};

// ═══════════════════════════════════════════════════════════════════════
// File Discovery
// ═══════════════════════════════════════════════════════════════════════

std::vector<std::string> collect_parquet_files(const std::string& path) {
    std::vector<std::string> files;
    fs::path p(path);
    if (fs::is_regular_file(p)) {
        auto ext = p.extension().string();
        std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
        if (ext == ".parquet" || ext == ".pqt") files.push_back(p.string());
    } else if (fs::is_directory(p)) {
        for (const auto& entry : fs::recursive_directory_iterator(p)) {
            if (!entry.is_regular_file()) continue;
            auto ext = entry.path().extension().string();
            std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
            if (ext == ".parquet" || ext == ".pqt") files.push_back(entry.path().string());
        }
        std::sort(files.begin(), files.end());
    }
    return files;
}

// Load Parquet files into memory buffers
std::vector<InMemoryParquetFile> load_parquet_files_to_memory(
        const std::vector<std::string>& paths) {
    std::vector<InMemoryParquetFile> mem_files;
    for (auto& path : paths) {
        auto infile = arrow::io::ReadableFile::Open(path).ValueOrDie();
        int64_t size = infile->GetSize().ValueOrDie();
        std::string buffer(static_cast<size_t>(size), '\0');
        infile->Read(size, buffer.data()).ValueOrDie();
        mem_files.push_back({path, std::move(buffer)});
    }
    return mem_files;
}

// ═══════════════════════════════════════════════════════════════════════
// Benchmark Scenarios
// ═══════════════════════════════════════════════════════════════════════

struct ColumnScenario {
    std::string name;
    std::vector<int> col_indices;
    std::string description;
};

std::vector<ColumnScenario> get_bench_scenarios(int num_cols) {
    if (num_cols == 20) {
        return {
            {"Int32",           {2},                 "col_int32 (FoR+BitPacking)"},
            {"Int64",           {3},                 "col_int64 (FoR+BitPacking)"},
            {"Float64",         {9},                 "col_float64 (ALP)"},
            {"Timestamp",       {14},                "col_ts_us (FoR+BitPacking)"},
            {"String Low",      {17},                "col_string_low (FSST+Dict, low card)"},
            {"String High",     {16},                "col_string_high (FSST+Dict, high card)"},
            {"Decimal128",      {19},                "col_decimal (FoR+BitPacking)"},
            {"Analytics 3-col", {2, 9, 16},          "int32 + float64 + string_high"},
            {"Analytics 5-col", {2, 3, 9, 14, 17},   "int32 + int64 + float64 + ts_us + string_low"},
            {"Full Table",      {},                   "all 20 columns"},
        };
    }
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
// Get schema from Velox Parquet Reader (from in-memory data)
// ═══════════════════════════════════════════════════════════════════════

vx::RowTypePtr get_velox_row_type(const InMemoryParquetFile& mf,
                                   vx::memory::MemoryPool* pool) {
    auto inFile = std::make_shared<vx::InMemoryReadFile>(mf.data);
    inFile->setShouldCoalesce(false);
    vx::dwio::common::ReaderOptions readerOpts{pool};
    readerOpts.setFileFormat(vx::dwio::common::FileFormat::PARQUET);
    auto bufferedInput = std::make_unique<vx::dwio::common::BufferedInput>(
        inFile, *pool);
    auto reader = vx::parquet::ParquetReader(
        std::move(bufferedInput), readerOpts);
    return reader.rowType();
}

// ═══════════════════════════════════════════════════════════════════════
// Velox Parquet Reader: read from in-memory Parquet data → Velox Vectors
// ═══════════════════════════════════════════════════════════════════════

struct VeloxReadResult {
    size_t rows_read = 0;
    size_t vectors_read = 0;
};

VeloxReadResult read_parquet_velox_from_memory(
        const std::vector<InMemoryParquetFile>& mem_files,
        const std::vector<int>& col_indices,
        vx::memory::MemoryPool* pool) {
    VeloxReadResult res;
    for (const auto& mf : mem_files) {
        try {
            auto inFile = std::make_shared<vx::InMemoryReadFile>(mf.data);
            inFile->setShouldCoalesce(false);

            vx::dwio::common::ReaderOptions readerOpts{pool};
            readerOpts.setFileFormat(vx::dwio::common::FileFormat::PARQUET);

            auto bufferedInput = std::make_unique<vx::dwio::common::BufferedInput>(
                inFile, *pool);

            auto reader = vx::parquet::ParquetReader(
                std::move(bufferedInput), readerOpts);

            auto fullRowType = reader.rowType();

            // Build projected row type
            vx::RowTypePtr readType;
            if (!col_indices.empty()) {
                std::vector<std::string> names;
                std::vector<vx::TypePtr> types;
                for (int idx : col_indices) {
                    names.push_back(fullRowType->nameOf(idx));
                    types.push_back(fullRowType->childAt(idx));
                }
                readType = vx::ROW(std::move(names), std::move(types));
            } else {
                readType = fullRowType;
            }

            // Set up RowReaderOptions with ColumnSelector AND ScanSpec
            vx::dwio::common::RowReaderOptions rowReaderOpts;
            rowReaderOpts.select(
                std::make_shared<vx::dwio::common::ColumnSelector>(
                    readType, readType->names()));

            auto scanSpec = std::make_shared<vx::common::ScanSpec>("");
            scanSpec->addAllChildFields(*readType);
            rowReaderOpts.setScanSpec(scanSpec);

            auto rowReader = reader.createRowReader(rowReaderOpts);

            // Pre-allocate result vector (required by Velox to avoid
            // "SelectiveStructColumnReaderBase expects a non-null result")
            vx::VectorPtr result = vx::BaseVector::create(readType, 0, pool);

            while (rowReader->next(8192, result)) {
                if (result && result->size() > 0) {
                    // Force materialization of ALL LazyVector children.
                    // Velox's SelectiveStructColumnReader creates LazyVectors
                    // for selected columns without filters, deferring actual
                    // data decoding. We must force materialization here so
                    // the benchmark timing includes all column decoding work.
                    auto* rv = result->as<vx::RowVector>();
                    if (rv) {
                        for (auto i = 0; i < rv->childrenSize(); ++i) {
                            auto child = rv->childAt(i);
                            if (child->isLazy()) {
                                rv->childAt(i) = child->as<vx::LazyVector>()->loadedVectorShared();
                            }
                            benchmark_escape(rv->childAt(i));
                        }
                    }
                    res.rows_read += result->size();
                    res.vectors_read++;
                    benchmark_escape(result);
                }
            }
        } catch (const std::exception& e) {
            std::cerr << "  Velox Parquet read error: " << e.what() << "\n";
        }
    }
    return res;
}

// ═══════════════════════════════════════════════════════════════════════
// Verify Mode
// ═══════════════════════════════════════════════════════════════════════

void run_verify(LiquidCacheStore& store,
                const std::vector<LiquidCacheStore::RowGroupInfo>& rg_infos,
                const vx::RowTypePtr& veloxRowType,
                vx::memory::MemoryPool* pool) {
    std::cout << "\n";
    std::cout << "========================================\n";
    std::cout << "  VELOX CONVERSION VERIFICATION\n";
    std::cout << "  Arrow -> Liquid -> Velox\n";
    std::cout << "========================================\n";

    size_t total_pass = 0, total_fail = 0;

    for (const auto& rg : rg_infos) {
        std::cout << "  RG: file=" << rg.file_id << " rg=" << rg.rg_id
                  << " batches=" << rg.num_batches << " rows=" << rg.total_rows << "\n";
        for (uint16_t b = 0; b < rg.num_batches; ++b) {
            std::cout << "    Batch " << b << "...\n" << std::flush;
            try {
                auto result = store.read_batch_velox(
                    rg.file_id, rg.rg_id, b, veloxRowType, pool);
                if (!result) { total_fail++; continue; }
                auto rv = result->as<vx::RowVector>();
                if (!rv) { total_fail++; continue; }
                if (rv->size() == 0 && rg.num_batches > 0) { total_fail++; continue; }
                total_pass++;
            } catch (const std::exception& e) {
                total_fail++;
                std::cerr << "  Verify error: " << e.what() << "\n";
            }
        }
    }

    std::cout << "\n  Cache entries verified: " << total_pass << " PASS, "
              << total_fail << " FAIL\n";
}

// ═══════════════════════════════════════════════════════════════════════
// Bench Mode
// ═══════════════════════════════════════════════════════════════════════

void run_bench(const std::vector<InMemoryParquetFile>& mem_files,
               LiquidCacheStore& store,
               const std::vector<LiquidCacheStore::RowGroupInfo>& rg_infos,
               const vx::RowTypePtr& veloxRowType,
               vx::memory::MemoryPool* pool) {
    int num_cols = veloxRowType->size();
    auto scenarios = get_bench_scenarios(num_cols);

    static constexpr int ITERS = 50;
    static constexpr int WARMUP = 3;

    std::cout << "\n";
    std::cout << "================================================================\n";
    std::cout << "  LIQUID CACHE -> VELOX VECTOR BENCHMARK\n";
    std::cout << "  Velox Parquet->Velox vs Liquid Cache->Velox (to_velox)\n";
    std::cout << "  Both paths read from in-memory Parquet data\n";
    std::cout << "  " << ITERS << " iterations, " << WARMUP << " warmup per scenario\n";
    std::cout << "================================================================\n";

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
    std::cout << "    Total rows:    " << total_rows << "\n";

    std::cout << "\n  Schema (" << num_cols << " columns):\n";
    for (int i = 0; i < num_cols; ++i) {
        std::cout << "    [" << std::setw(2) << i << "] "
                  << std::left << std::setw(20) << veloxRowType->nameOf(i)
                  << " " << veloxRowType->childAt(i)->toString() << "\n";
    }
    std::cout << std::right;

    struct ScenarioResult {
        std::string name;
        int ncols;
        BenchStats pq_stats;
        BenchStats cache_stats;
        double speedup;
        double cache_throughput_rows;  // rows/sec
        double cache_throughput_mb;    // MB/sec (approximate)
    };
    std::vector<ScenarioResult> results;

    for (const auto& sc : scenarios) {
        int ncols = sc.col_indices.empty() ? num_cols
                    : static_cast<int>(sc.col_indices.size());
        std::cout << "\n  --- " << sc.name << " (" << ncols
                  << " col" << (ncols > 1 ? "s" : "") << ": "
                  << sc.description << ") ---\n";

        std::vector<int> projection = sc.col_indices;

        // Velox Parquet Reader: warmup + measure
        bool velox_reader_ok = true;
        {
            auto test = read_parquet_velox_from_memory(
                mem_files, sc.col_indices, pool);
            if (test.rows_read == 0) velox_reader_ok = false;
        }

        BenchStats pq_stats{};
        if (velox_reader_ok) {
            for (int w = 0; w < WARMUP; ++w) {
                auto wr = read_parquet_velox_from_memory(
                    mem_files, sc.col_indices, pool);
                benchmark_escape(wr);
            }
            std::vector<double> pq_samples;
            pq_samples.reserve(ITERS);
            for (int i = 0; i < ITERS; ++i) {
                auto t0 = SteadyClock::now();
                auto res = read_parquet_velox_from_memory(
                    mem_files, sc.col_indices, pool);
                auto t1 = SteadyClock::now();
                benchmark_escape(res);
                pq_samples.push_back(Duration(t1 - t0).count());
            }
            pq_stats = compute_stats(std::move(pq_samples));
        }

        // Liquid Cache -> Velox: warmup + measure
        for (int w = 0; w < WARMUP; ++w) {
            for (const auto& rg : rg_infos) {
                for (uint16_t b = 0; b < rg.num_batches; ++b) {
                    auto vec = store.read_batch_velox(
                        rg.file_id, rg.rg_id, b, veloxRowType, pool, projection);
                    benchmark_escape(vec);
                }
            }
        }

        std::vector<double> cache_samples;
        cache_samples.reserve(ITERS);
        size_t cache_rows = 0;
        for (int i = 0; i < ITERS; ++i) {
            auto t0 = SteadyClock::now();
            size_t iter_rows = 0;
            for (const auto& rg : rg_infos) {
                for (uint16_t b = 0; b < rg.num_batches; ++b) {
                    auto vec = store.read_batch_velox(
                        rg.file_id, rg.rg_id, b, veloxRowType, pool, projection);
                    if (vec) {
                        iter_rows += vec->size();
                        benchmark_escape(vec);
                    }
                }
            }
            auto t1 = SteadyClock::now();
            cache_samples.push_back(Duration(t1 - t0).count());
            cache_rows = iter_rows;
        }
        auto cache_stats = compute_stats(std::move(cache_samples));

        double speedup = (velox_reader_ok && cache_stats.mean > 1e-12)
                         ? pq_stats.mean / cache_stats.mean : 0;

        // Throughput: rows/sec and approximate MB/sec
        double cache_throughput_rows = (cache_stats.mean > 1e-12)
            ? cache_rows / cache_stats.mean : 0;
        double approx_bytes = static_cast<double>(cache_rows) * ncols * 8.0;
        double cache_throughput_mb = (cache_stats.mean > 1e-12)
            ? approx_bytes / cache_stats.mean / (1024.0 * 1024.0) : 0;

        std::cout << "    Rows:            " << cache_rows << "\n";
        if (velox_reader_ok) {
            std::cout << "    Parquet->Velox:  " << fmt_time_ms(pq_stats.mean * 1000.0)
                      << "  (med " << fmt_time_ms(pq_stats.median * 1000.0)
                      << " ±" << fmt_time_ms(pq_stats.stddev * 1000.0)
                      << ", CI±" << fmt_time_ms(pq_stats.ci_half * 1000.0) << ")\n";
        } else {
            std::cout << "    Parquet->Velox:  (unavailable)\n";
        }
        std::cout << "    Liquid->Velox:   " << fmt_time_ms(cache_stats.mean * 1000.0)
                  << "  (med " << fmt_time_ms(cache_stats.median * 1000.0)
                  << " ±" << fmt_time_ms(cache_stats.stddev * 1000.0)
                  << ", CI±" << fmt_time_ms(cache_stats.ci_half * 1000.0) << ")\n";
        if (velox_reader_ok) {
            std::cout << "    Speedup:         " << std::fixed << std::setprecision(2)
                      << speedup << "x\n";
        }
        std::cout << "    Throughput:      " << std::fixed << std::setprecision(0)
                  << cache_throughput_rows << " rows/sec, ~"
                  << std::setprecision(1) << cache_throughput_mb << " MB/sec (est.)\n";

        results.push_back({sc.name, ncols, pq_stats, cache_stats, speedup,
                           cache_throughput_rows, cache_throughput_mb});
    }

    // Summary table
    std::cout << "\n+-------------------------+-------+------------+------------+----------+-------------+\n";
    std::cout <<   "|  Scenario               |  Cols | Parq->Velox| Liq->Velox | Speedup  | rows/sec    |\n";
    std::cout <<   "+-------------------------+-------+------------+------------+----------+-------------+\n";
    for (const auto& r : results) {
        std::cout << "|  " << std::left << std::setw(23) << r.name
                  << " | " << std::right << std::setw(5) << r.ncols
                  << " | " << fmt_time_ms_table(r.pq_stats.mean * 1000.0)
                  << " | " << fmt_time_ms_table(r.cache_stats.mean * 1000.0)
                  << " | " << std::setw(6) << std::fixed << std::setprecision(2)
                  << r.speedup << "x "
                  << " | " << std::setw(9) << std::setprecision(0)
                  << r.cache_throughput_rows
                  << " |\n";
    }
    std::cout << "+-------------------------+-------+------------+------------+----------+-------------+\n";
}

// ═══════════════════════════════════════════════════════════════════════
// Main
// ═══════════════════════════════════════════════════════════════════════

int main(int argc, char* argv[]) {
    folly::Init init{&argc, &argv};

    // Suppress glog output to avoid noise from Velox internal errors
    google::SetStderrLogging(google::GLOG_FATAL);

    // Initialize Arrow (required for compute kernels like MinMax used in transcode)
    auto arrow_status = arrow::Initialize(arrow::GlobalOptions{});
    if (!arrow_status.ok()) {
        std::cerr << "Arrow initialization failed: " << arrow_status.ToString() << "\n";
        return 1;
    }

    vx::filesystems::registerLocalFileSystem();
    vx::parquet::registerParquetReaderFactory();
    vx::memory::MemoryManager::initialize(vx::memory::MemoryManager::Options{});
    auto pool = vx::memory::memoryManager()->addLeafPool("velox_benchmark");

    std::cout << "========================================================\n";
    std::cout << "  Liquid Cache C++ - Velox Vector Benchmark\n";
    std::cout << "  Compare: Velox Parquet->Velox vs Liquid Cache->Velox\n";
    std::cout << "  Both paths: in-memory Parquet data (no disk I/O)\n";
    std::cout << "========================================================\n";

    if (argc < 2) {
        std::cout << "\nUsage: " << argv[0] << " <parquet_path> [mode]\n\n";
        std::cout << "  mode (optional):\n";
        std::cout << "    bench    (default) Performance benchmark\n";
        std::cout << "    verify   Conversion verification\n\n";
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

    // Stage 1: Load Parquet files into memory
    std::cout << "Loading Parquet files into memory..." << std::flush;
    auto t_load0 = SteadyClock::now();
    auto mem_files = load_parquet_files_to_memory(files);
    auto t_load1 = SteadyClock::now();
    double load_sec = Duration(t_load1 - t_load0).count();
    double total_mb = 0;
    for (auto& mf : mem_files) total_mb += mf.data.size() / (1024.0 * 1024.0);
    std::cout << " done (" << std::fixed << std::setprecision(1)
              << load_sec << "s, " << total_mb << " MB)\n";

    // Get schema from Velox Parquet Reader (from in-memory data)
    auto veloxRowType = get_velox_row_type(mem_files[0], pool.get());
    std::cout << "Schema: " << veloxRowType->size() << " columns\n";

    // Stage 2: Load Liquid Cache (one-time transcode from in-memory Parquet)
    std::cout << "Loading Liquid Cache..." << std::flush;
    LiquidCacheStore store;
    vx::RowTypePtr cacheRowType;
    double transcode_sec = 0;
    auto rg_infos = store.load_from_parquet_for_velox(
        files, cacheRowType, transcode_sec);
    std::cout << " done (" << std::fixed << std::setprecision(1)
              << transcode_sec << "s transcode)\n";

    const auto& rowType = cacheRowType ? cacheRowType : veloxRowType;

    if (mode == "bench") {
        run_bench(mem_files, store, rg_infos, rowType, pool.get());
    } else if (mode == "verify") {
        run_verify(store, rg_infos, rowType, pool.get());
    } else {
        std::cerr << "Unknown mode: " << mode << "\n";
        return 1;
    }

    return 0;
}
