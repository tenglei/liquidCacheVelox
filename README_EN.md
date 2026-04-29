# liquid-cache-cpp

A C++20 high-performance columnar data in-memory cache and encoding/compression library, supporting efficient encoding/decoding of Parquet data with optional integration of the Facebook Velox vector engine.

## Directory Structure

```
liquid-cache-cpp/
├── CMakeLists.txt                    # Main build configuration
├── include/liquid_cache/             # Header files
│   ├── transcoder.h                  # Main transcoding interface
│   ├── liquid_cache_store.h          # Columnar cache storage (LRU)
│   ├── liquid_arrays.h               # Encoded array implementations
│   ├── liquid_decimal_array.h        # Decimal type support
│   ├── liquid_fixed_len_byte_array.h # Fixed-length byte arrays
│   ├── liquid_byte_view_array.h      # ByteView arrays
│   ├── liquid_to_velox.h             # Velox vector conversion
│   ├── fsst.h                        # FSST string compression
│   ├── bit_packed_array.h            # Bit-packing utilities
│   ├── ipc_header.h                  # IPC header format
│   ├── lru_policy.h                  # LRU eviction policy
│   └── jni_bridge.h                  # JNI bridge header
├── src/
│   ├── transcoder_arrow.cpp          # Arrow integration & encoding/decoding
│   ├── jni_bridge.cpp                # JNI bridge implementation
│   └── liquid_to_velox.cpp           # Velox vector conversion implementation
├── examples/
│   ├── transcode_example.cpp         # Basic transcoding example
│   └── velox_benchmark.cpp           # Velox performance benchmark
├── tools/
│   ├── generate_test_parquet.cpp     # Test Parquet data generator
│   └── verify_parquet.cpp            # Parquet file verifier
└── tests/
    ├── test_roundtrip.cpp            # Encode/decode round-trip tests
    ├── test_velox_crossval.cpp       # Velox cross-validation tests
    ├── test_linear_integer.cpp       # LinearInteger tests
    ├── test_float_quantize.cpp       # Float quantization tests
    └── test_cache_budget.cpp         # Cache budget and LRU tests
```

## Automation Scripts

The project provides four automation scripts located in the `scripts/` directory, covering common operations from test data generation, Velox benchmark builds, JNI library builds, to complete test runs.

### Script Overview

| Script | Function | Typical Duration |
|--------|----------|------------------|
| `generate_test_parquet.sh` | Build and run the Parquet test data generator | 10-15s |
| `build_velox_benchmark.sh` | Configure and compile Velox integration benchmark | 30-60s |
| `build_jni_library.sh` | Compile JNI shared library (.so) | 10-20s |
| `run_all_tests.sh` | Run all unit tests + Parquet verification | Build+test ~2min |

All scripts support `-h/--help` for full help, `-n/--dry-run` to preview commands, and `-d/--build-dir` to specify the build directory.

---

### 1. `generate_test_parquet.sh` — Generate Test Parquet Data

Builds the `generate_test_parquet` tool and generates a Parquet test file containing 20 columns covering all Liquid Cache supported formats.

**Usage:**

```bash
./scripts/generate_test_parquet.sh [options]
```

**Common Parameters:**

| Parameter | Default | Description |
|-----------|---------|-------------|
| `-o, --output <path>` | `build/test_data_512mb.parquet` | Output file path |
| `-s, --size <GB>` | — | Target file size (GB), automatically calculates row count |
| `-r, --rows <count>` | `5000000` | Target row count |
| `-c, --compression <c>` | `snappy` | Compression format (snappy/gzip/zstd/lz4/brotli/none) |
| `-d, --build-dir <dir>` | `build` | CMake build directory |
| `-j <N>` | `nproc` | Parallel compilation jobs |
| `-n, --dry-run` | — | Display commands only, do not execute |

**Use Cases:**

- Generate benchmark data after initial environment setup
- Generate Parquet files of different sizes for performance testing
- Provide input for `liquid_velox_benchmark` and `verify_parquet`

**Examples:**

```bash
# Default configuration (5M rows, ~512MB, Snappy compression)
./scripts/generate_test_parquet.sh

# Generate ~1GB Parquet file
./scripts/generate_test_parquet.sh -s 1

# Custom row count and output path
./scripts/generate_test_parquet.sh -r 10000000 -o /tmp/large.parquet

# Specify a different build directory
./scripts/generate_test_parquet.sh -d build_debug -s 2
```

**Note:** The current C++ tool has fixed 20 columns and Snappy compression. The script will warn when using other compression types or column counts, indicating that C++ source modification is required.

---

### 2. `build_velox_benchmark.sh` — Build Velox Integration Benchmark

Configures and compiles `liquid_velox_benchmark`, including ABI compatibility checks (Velox bundled Arrow 18 vs. system Arrow 24).

**Usage:**

```bash
./scripts/build_velox_benchmark.sh [options]
```

**Common Parameters:**

| Parameter | Default | Description |
|-----------|---------|-------------|
| `-p, --velox-prefix <path>` | `/home/tenglei/code/velox/build` | Velox build directory path |
| `-d, --build-dir <dir>` | `build` | CMake build directory |
| `-t, --build-type <type>` | `Release` | Build type (Release/Debug) |
| `-j <N>` | `nproc` | Parallel compilation jobs |
| `--clean` | — | Clean build directory before reconfiguring |
| `-n, --dry-run` | — | Display commands only, do not execute |

**Use Cases:**

- When the system/project already has complete Velox build artifacts
- Need to generate `liquid_velox_benchmark` binary for performance comparisons
- Automatic Velox benchmark builds in CI/CD

**Examples:**

```bash
# Default configuration (uses ~/code/velox/build)
./scripts/build_velox_benchmark.sh

# Specify Velox build path
./scripts/build_velox_benchmark.sh -p /opt/velox/build

# Clean rebuild (Debug mode)
./scripts/build_velox_benchmark.sh --clean -t Debug -j 8
```

**Output Files:**
- `build/liquid_velox_benchmark` — Velox performance benchmark executable
- `build/libliquid_cache_velox.a` — Velox integration static library

**ABI Notes:** The script automatically detects conflicts between the system Arrow version and Velox bundled Arrow 18 and issues warnings. Compilation flags `-mavx2 -mfma -mavx -mf16c -mlzcnt -mbmi2` ensure ABI compatibility with Velox.

---

### 3. `build_jni_library.sh` — Build JNI Shared Library

Compiles `libliquid_cache_jni.so`, including JNI header detection, dynamic library dependency analysis, and JNI symbol export verification.

**Usage:**

```bash
./scripts/build_jni_library.sh [options]
```

**Common Parameters:**

| Parameter | Default | Description |
|-----------|---------|-------------|
| `-d, --build-dir <dir>` | `build` | CMake build directory |
| `-t, --build-type <type>` | `Release` | Build type (Release/Debug) |
| `-j <N>` | `nproc` | Parallel compilation jobs |
| `--clean` | — | Clean build directory before reconfiguring |
| `--with-velox <path>` | — | Enable Velox simultaneously (not recommended, see below) |
| `-n, --dry-run` | — | Display commands only, do not execute |

**Use Cases:**

- Need to call Liquid Cache via JNI from Java/Scala
- Verify JNI symbol export correctness
- Check JNI library buildability in CI/CD

**Examples:**

```bash
# Default configuration
./scripts/build_jni_library.sh

# Debug build + clean
./scripts/build_jni_library.sh -t Debug --clean

# Custom build directory
./scripts/build_jni_library.sh -d build_jni
```

**Important Warning:** Using `--with-velox` is not recommended. The JNI code is based on system Arrow 24 API, which is ABI-incompatible with Velox bundled Arrow 18. If the build directory contains a cached Velox configuration, the script will automatically detect this and force-disable Velox (`-DLIQUID_ENABLE_VELOX=OFF`).

**Advanced Notes:** After building, the script performs:
- `ldd` / `readelf` dynamic library dependency analysis
- `nm` JNI symbol inspection (`Java_` prefixed exported functions)

---

### 4. `run_all_tests.sh` — Run All Tests

Builds (optional) and runs all unit test suites along with Parquet verification, producing a PASS/FAIL/SKIP summary report.

**Usage:**

```bash
./scripts/run_all_tests.sh [options]
```

**Common Parameters:**

| Parameter | Default | Description |
|-----------|---------|-------------|
| `-p, --parquet <path>` | `build/test_data_512mb.parquet` | Parquet test file path |
| `-d, --build-dir <dir>` | `build` | CMake build directory |
| `-t, --build-type <type>` | `Release` | Build type (Release/Debug) |
| `-j <N>` | `nproc` | Parallel compilation jobs |
| `--with-velox <path>` | — | Enable Velox cross-validation tests |
| `--clean` | — | Clean build directory before reconfiguring |
| `--no-build` | — | Skip build, run existing tests only |
| `--gtest-filter <f>` | `*` | Google Test filter |
| `-n, --dry-run` | — | Display commands only, do not execute |

**Test Suites:**

| # | Test Name | Binary | Description |
|---|-----------|--------|-------------|
| 1 | Core Round-trip Tests | `liquid_cache_tests` | Encode/decode round-trip correctness (37 tests) |
| 2 | Velox Cross-validation | `liquid_velox_tests` | Only run with `--with-velox` |
| 3 | LinearInteger Tests | `liquid_linear_test` | Linear integer encoding tests |
| 4 | Float Quantization Tests | `liquid_float_quantize_test` | Float quantization algorithm tests (10 tests) |
| 5 | Cache Budget/LRU Tests | `liquid_cache_budget_test` | Cache policy tests (19 tests) |
| 6 | Parquet File Verification | `verify_parquet` | Row/column/Row Group integrity |

**Use Cases:**

- Verify all functionality correctness after development
- Run regression tests after modifying encoding algorithms
- Automated testing steps in CI/CD pipelines

**Examples:**

```bash
# Complete test workflow (build + all tests)
./scripts/run_all_tests.sh

# Run tests only (skip compilation)
./scripts/run_all_tests.sh --no-build

# Specify Parquet file and run specific Google Test
./scripts/run_all_tests.sh -p /tmp/test.parquet --gtest-filter "LiquidCache*"

# Enable Velox integration tests
./scripts/run_all_tests.sh --with-velox /home/tenglei/code/velox/build
```

**Output Example (Summary Report):**

```
╔══════════════════════════════════════════════════════════════╗
║                    Test Summary Report                      ║
╚══════════════════════════════════════════════════════════════╝

  Total Suites:   6
  Passed:         4
  Failed:         0
  Skipped:        2
  Total Time:     125s

  #   Test Name                              Status      Time/Detail
  --- -----------------------------------   ----------  --------------------
  1   Core Round-trip (test_roundtrip)      PASS        3.2s
  2   Velox Cross-val (test_velox_crossval) SKIP        Velox not enabled
  3   LinearInteger Tests                   PASS        0.5s
  4   Float Quantization Tests              PASS        0.3s
  5   Cache Budget/LRU Tests                PASS        2.1s
  6   Parquet File Verification             PASS        1.8s

  ✓ All tests passed!
```

---

### Typical Workflow

The following demonstrates how to combine the four scripts, covering the flow from scratch to full validation:

```bash
cd /home/tenglei/code/liquid-cache-cpp

# ── Step 1: Generate test Parquet data ───────────────────────
./scripts/generate_test_parquet.sh -s 1 -o build/test_1gb.parquet

# ── Step 2: Run all unit tests (verify core functionality) ───
./scripts/run_all_tests.sh -p build/test_1gb.parquet

# ── Step 3: Build Velox Benchmark (if Velox available) ───────
./scripts/build_velox_benchmark.sh -p /home/tenglei/code/velox/build

# ── Step 4: Verify Velox conversion correctness ──────────────
./build/liquid_velox_benchmark build/test_1gb.parquet verify

# ── Step 5: Run Velox performance benchmark ──────────────────
./build/liquid_velox_benchmark build/test_1gb.parquet bench

# ── Step 6: Build JNI shared library (for Java invocation) ───
./scripts/build_jni_library.sh
```

**CI/CD Integration:**

The GitHub Actions workflow file is located at `.github/workflows/ci.yml` and automatically executes the project's build, test, and verification processes. See the workflow file comments for details.

## Prerequisites

### Compiler

- GCC 13.3+ (C++20 support required)
- Verified environment: GCC 13.3.0 (Ubuntu 24.04)

### Required Dependencies

| Dependency | Version | Purpose |
|------------|---------|---------|
| CMake | >= 3.16 | Build system |
| Apache Arrow | 24.0.0 | Columnar data framework |
| Apache Parquet | 24.0.0 | Parquet file I/O |
| Abseil | Static lib | Google base library |
| JNI | — | Java Native Interface headers |
| OpenSSL (ssl/crypto) | 3.0.13 | Encryption/security |
| Thrift | 0.19.0 | RPC serialization |
| Protobuf | — | Serialization |
| Snappy | — | Data compression |
| RE2 | 10.0.0 | Regular expressions |
| LZ4 | 1.9.4 | Fast compression |
| Zstd | — | High-ratio compression |
| Brotli | 1.1.0 | Google compression library |
| libxml2, nghttp2, gssapi_krb5, curl | — | Arrow system-level transitive dependencies |

### Optional Dependencies (Velox Integration)

Required when `-DLIQUID_ENABLE_VELOX=ON`:

| Dependency | Description |
|------------|-------------|
| Facebook Velox | Complete build artifacts, including bundled Arrow 18 + Parquet |
| Folly | Facebook C++ utility library |
| fmt | Formatting library |
| SimdJson | JSON parsing |
| DuckDB | Embedded database (static library) |
| Boost | context, filesystem, program_options, regex, thread |
| glog / gflags | Logging and command-line parsing |
| double-conversion | Float conversion |
| libevent / libsodium / libunwind | System transitive dependencies |
| ICU (icui18n, icuuc, icudata) | Internationalization support |

## CMake Configuration Parameters

| Parameter | Type | Default | Description |
|-----------|------|---------|-------------|
| `CMAKE_BUILD_TYPE` | STRING | — | Build type, `Release` recommended |
| `LIQUID_ENABLE_VELOX` | BOOL | `OFF` | Enable Velox vector engine integration |
| `VELOX_PREFIX` | STRING | `""` | Velox build directory path (required when Velox is enabled) |
| `ABSL_STATIC_PREFIX` | STRING | `""` | Static Abseil installation path, e.g., `/opt/absl-static` |
| `LIQUID_BUILD_TESTS` | BOOL | `ON` | Build Google Test unit tests |

## Build Steps

### Basic Build (without Velox)

The basic build generates all core components: static library, JNI shared library, example programs, and tools.

```bash
cd /home/tenglei/code/liquid-cache-cpp
mkdir -p build && cd build

# Configure (system Arrow 24 uses DEBIAN3 ABI namespace absl, no extra specification needed)
cmake .. -DCMAKE_BUILD_TYPE=Release

# Build all targets
cmake --build . -j$(nproc)

# Or build specific targets only
cmake --build . --target generate_test_parquet -j$(nproc)
cmake --build . --target liquid_cache_example -j$(nproc)
```

> **Note**: If the system Arrow's absl ABI namespace differs from a custom-installed absl (e.g., system uses `absl::debian3`, custom uses `absl`), linking will fail with undefined reference errors. In this case, do not set `ABSL_STATIC_PREFIX`; let CMake automatically use the system absl. Only specify this parameter when the custom absl namespace matches the system Arrow's.

**Output Files** (in `build/` directory):

| File | Path | Type |
|------|------|------|
| `generate_test_parquet` | `build/generate_test_parquet` | Executable |
| `verify_parquet` | `build/verify_parquet` | Executable |
| `liquid_cache_example` | `build/liquid_cache_example` | Executable |
| `libliquid_cache_core.a` | `build/libliquid_cache_core.a` | Static library |
| `libliquid_cache_jni.so` | `build/libliquid_cache_jni.so` | Shared library |

### Velox Integration Build

> **Important**: Velox uses bundled Arrow 18 (not system Arrow 24); the two are ABI-incompatible. When Velox is enabled, all targets uniformly use Velox's bundled Arrow 18 headers and libraries.

```bash
cd /home/tenglei/code/liquid-cache-cpp
mkdir -p build && cd build

# Configure (replace /path/to/velox/build with your Velox build directory)
cmake .. \
  -DCMAKE_BUILD_TYPE=Release \
  -DLIQUID_ENABLE_VELOX=ON \
  -DVELOX_PREFIX=/path/to/velox/build

# Build benchmark
cmake --build . --target liquid_velox_benchmark -j$(nproc)

# Or build all targets (including Velox-related)
cmake --build . -j$(nproc)
```

**Additional Outputs:**

| File | Path | Type |
|------|------|------|
| `liquid_velox_benchmark` | `build/liquid_velox_benchmark` | Executable |
| `libliquid_cache_velox.a` | `build/libliquid_cache_velox.a` | Static library |

**Compilation Flags:** Velox targets use `-mavx2 -mfma -mavx -mf16c -mlzcnt -mbmi2` to ensure ABI compatibility with Velox.

**Key Points:**
- `-Wl,--whole-archive` wrapping `libarrow.a` is mandatory — Arrow compute kernels (such as `min_max`) are registered through static initializers; without this flag, the linker discards these .o files, causing runtime `"No function registered with name: min_max"` errors
- If `LIQUID_ENABLE_VELOX=ON` is set but `VELOX_PREFIX` is not specified, CMake will output a warning and skip Velox targets
- System Arrow 24 uses the DEBIAN3 ABI namespace versions of system thrift/protobuf/re2 etc.; Velox's bundled Arrow 18 uses its own versions. Mixing the two causes linking or runtime crashes

## Generating Test Parquet Data

`generate_test_parquet` generates synthetic Parquet test files containing all Liquid Cache supported field types.

### Usage

```bash
# Default configuration: 5M rows, output to build/test_data_512mb.parquet
./build/generate_test_parquet

# Custom output path
./build/generate_test_parquet /path/to/output.parquet

# Custom output path and row count
./build/generate_test_parquet /path/to/output.parquet 10000000
```

### Default Parameters

- **Rows**: 5,000,000 (~512MB Parquet file)
- **Columns**: 20
- **Batch size**: 100,000 rows/batch
- **Compression**: Snappy
- **Random seed**: 42 (fixed, reproducible)

### Generated Schema (20 columns)

| Category | Column Name | Type |
|----------|-------------|------|
| Integer | `col_int8`, `col_int16`, `col_int32`, `col_int64` | int8, int16, int32, int64 |
| Unsigned Integer | `col_uint8`, `col_uint16`, `col_uint32`, `col_uint64` | uint8, uint16, uint32, uint64 |
| Floating Point | `col_float32`, `col_float64` | float32, float64 |
| Date | `col_date32`, `col_date64` | date32, date64 |
| Timestamp | `col_ts_s`, `col_ts_ms`, `col_ts_us`, `col_ts_ns` | timestamp(s/ms/us/ns) |
| String | `col_string_high` (high cardinality), `col_string_low` (low cardinality) | utf8 |
| Binary | `col_binary` | binary |
| Decimal | `col_decimal` | decimal128(10,2) |

### Example Output

```
Generating 5000000 rows with 20 columns...
Output: /home/tenglei/code/liquid-cache-cpp/build/test_data_512mb.parquet

  0%  0 / 5000000 rows  (0.00 M rows/s)
  10%  500000 / 5000000 rows  (25.50 M rows/s)
  20%  1000000 / 5000000 rows  (24.80 M rows/s)
  ...

Done in 12.5 s
  Rows:    5000000
  Columns: 20
  File:    /home/tenglei/code/liquid-cache-cpp/build/test_data_512mb.parquet
  Size:    512.3 MB
```

## Verifying Parquet Files

```bash
./build/verify_parquet test_data_512mb.parquet
```

Example output:
```
Rows: 5000000
Cols: 20
Schema: col_int8:int8, col_int16:int16, col_int32:int32, ...
Row Groups: 50
```

## Running Velox Performance Benchmarks

`liquid_velox_benchmark` compares the performance of two paths:

1. **Velox Parquet Reader** → Velox Vector (reads from in-memory Parquet data)
2. **Liquid Cache** → Velox Vector (decodes from in-memory Liquid-encoded data)

Both paths start from in-memory Parquet data, ensuring a fair comparison by excluding disk I/O differences.

### Usage

```bash
# Benchmark mode (default)
./build/liquid_velox_benchmark /path/to/test_data.parquet bench

# Conversion verification mode
./build/liquid_velox_benchmark /path/to/test_data.parquet verify

# Omitting the mode parameter defaults to bench
./build/liquid_velox_benchmark /path/to/test_data.parquet
```

### Benchmark Parameters

| Parameter | Value | Description |
|-----------|-------|-------------|
| `ITERS` | 50 | Measurement iterations per scenario |
| `WARMUP` | 3 | Warmup iterations per scenario |
| Batch Size | 8192 | Rows per batch |

### Test Scenarios

Benchmarks automatically select test scenarios based on the Parquet file schema:

- **Single-column tests**: Int32 (FoR+BitPacking), Int64, Float64 (ALP), Timestamp, String Low (FSST+Dict), String High (FSST+Dict), Decimal128
- **Mixed tests**: 3-column analytics mix (int32+float64+string_high), 5-column analytics mix (int32+int64+float64+ts_us+string_low)
- **Full table test**: All 20 columns

### Output Metrics

Each scenario outputs the following statistics (for both Velox Parquet Reader and Liquid Cache paths):

| Metric | Description |
|--------|-------------|
| Mean | Average elapsed time (ms) |
| Median | Median elapsed time (ms) |
| StdDev | Standard deviation |
| P5 / P95 | 5th / 95th percentile |
| CI95± | 95% confidence interval half-width |
| Speedup | Liquid Cache speedup relative to Parquet Reader |
| rows/sec | Rows processed per second |
| MB/sec | Data throughput |

Outliers are removed using the MAD (Median Absolute Deviation) method (4x MAD threshold).

### Output Example

```
  LIQUID CACHE -> VELOX VECTOR BENCHMARK
  Velox Parquet->Velox vs Liquid Cache->Velox (to_velox)
  Both paths read from in-memory Parquet data
  50 iterations, 3 warmup per scenario
  =================================================================

  Cache loaded:
    Entries:       12220 (Liquid: 12220, Arrow: 0)
    Memory:        675.2 MB
    Total rows:    5000000

  Schema (20 columns):
    [ 0] col_int8             TINYINT
    [1 ] col_int16            SMALLINT
    [2 ] col_int32            INTEGER
    [3 ] col_int64            BIGINT
    [4 ] col_uint8            TINYINT
    [5 ] col_uint16           SMALLINT
    [6 ] col_uint32           INTEGER
    [7 ] col_uint64           BIGINT
    [8 ] col_float32          REAL
    [9 ] col_float64          DOUBLE
    [10] col_date32           INTEGER
    [11] col_date64           INTEGER
    [12] col_ts_s             TIMESTAMP
    [13] col_ts_ms            TIMESTAMP
    [14] col_ts_us            TIMESTAMP
    [15] col_ts_ns            TIMESTAMP
    [16] col_string_high      VARCHAR
    [17] col_string_low       VARCHAR
    [18] col_binary           VARBINARY
    [19] col_decimal          DECIMAL(10, 2)

  --- Int32 (1 col: col_int32 (FoR+BitPacking)) ---
    Rows:            5000000
    Parquet->Velox:  252.62 ms  (med 250.92 ms)
    Liquid->Velox:   7.04 ms   (med 6.97 ms)
    Speedup:         35.91x
    Throughput:      710700397 rows/sec

  ...

+-------------------------+-------+------------+------------+----------+-------------+
|  Scenario               |  Cols | Parq->Velox| Liq->Velox | Speedup  | rows/sec    |
+-------------------------+-------+------------+------------+----------+-------------+
|  Int32                   |     1 |     252.62 |       7.04 |  35.91x  | 710700397   |
|  Int64                   |     1 |     291.82 |       8.16 |  35.75x  | 612478196   |
|  Float64                 |     1 |     263.61 |      13.96 |  18.88x  | 358172731   |
|  Timestamp               |     1 |     308.40 |       7.89 |  39.10x  | 633902787   |
|  String Low              |     1 |     242.80 |      56.67 |   4.28x  |  88224354   |
|  String High             |     1 |     374.25 |      61.27 |   6.11x  |  81603437   |
|  Decimal128              |     1 |     290.01 |       7.34 |  39.50x  | 680999129   |
|  Analytics 3-col         |     3 |     386.59 |      78.66 |   4.91x  |  63565293   |
|  Analytics 5-col         |     5 |     386.78 |      87.80 |   4.41x  |  56945443   |
|  Full Table              |    20 |    1050.87 |     301.57 |   3.48x  |  16579672   |
+-------------------------+-------+------------+------------+----------+-------------+
```

## Complete Build & Test Workflow

The following is the complete workflow from scratch (using this environment's verified paths):

```bash
# ── Step 1: Basic build ──────────────────────────────────────
cd /home/tenglei/code/liquid-cache-cpp
mkdir -p build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release
cmake --build . --target generate_test_parquet -j$(nproc)

# ── Step 2: Generate test data ───────────────────────────────
./generate_test_parquet
# Optional: verify the generated file
./verify_parquet test_data_512mb.parquet

# ── Step 3: Velox integration build ──────────────────────────
cmake .. \
  -DCMAKE_BUILD_TYPE=Release \
  -DLIQUID_ENABLE_VELOX=ON \
  -DVELOX_PREFIX=/home/tenglei/code/velox/build
cmake --build . --target liquid_velox_benchmark -j$(nproc)

# ── Step 4: Verify conversion correctness ────────────────────
./liquid_velox_benchmark test_data_512mb.parquet verify

# ── Step 5: Run performance benchmark ────────────────────────
./liquid_velox_benchmark test_data_512mb.parquet bench
```

### Benchmark Results (5M rows, 20 cols, WSL2 Ubuntu 24.04)

All scenarios have passed verify mode validation (611 batches, 0 FAIL). The following are bench mode measured data:

| Scenario | Cols | Parquet->Velox (ms) | Liquid->Velox (ms) | Speedup | Throughput (rows/s) |
|----------|------|---------------------|--------------------|---------|---------------------|
| Int32 (FoR+BitPacking) | 1 | 252.62 | 7.04 | **35.91x** | 710M |
| Int64 (FoR+BitPacking) | 1 | 291.82 | 8.16 | **35.75x** | 612M |
| Float64 (ALP) | 1 | 263.61 | 13.96 | **18.88x** | 358M |
| Timestamp (FoR+BitPacking) | 1 | 308.40 | 7.89 | **39.10x** | 634M |
| String Low (FSST+Dict, low cardinality) | 1 | 242.80 | 56.67 | **4.28x** | 88M |
| String High (FSST+Dict, high cardinality) | 1 | 374.25 | 61.27 | **6.11x** | 82M |
| Decimal128 (FoR+BitPacking) | 1 | 290.01 | 7.34 | **39.50x** | 681M |
| Analytics 3-col | 3 | 386.59 | 78.66 | **4.91x** | 64M |
| Analytics 5-col | 5 | 386.78 | 87.80 | **4.41x** | 57M |
| Full Table | 20 | 1050.87 | 301.57 | **3.48x** | 17M |

## FAQ

### Q: Build error `No function registered with name: min_max`

**Cause**: The linker discarded compute kernels in `libarrow.a` (they are registered through static initializers, and the linker considers them unreferenced).

**Solution**: Ensure `CMakeLists.txt` wraps the Arrow library with `-Wl,--whole-archive libarrow.a -Wl,--no-whole-archive`.

### Q: Compilation errors with Velox integration (missing headers / ABI incompatibility)

**Cause**: Velox uses bundled Arrow 18, which is incompatible with system Arrow 24.

**Solution**:
1. Set `LIQUID_ENABLE_VELOX=OFF` to use system Arrow 24 (without Velox functionality)
2. Or set `LIQUID_ENABLE_VELOX=ON` and specify the correct `VELOX_PREFIX` (all targets will then use bundled Arrow 18)

### Q: PIC-related errors when linking JNI shared library

**Cause**: System static libraries `.a` were not compiled with `-fPIC` and cannot be linked into `.so`.

**Solution**: The JNI shared library is configured to use dynamic library versions (system `.so`) for non-standard dependencies.

### Q: absl symbols not found during compilation

**Cause**: The system Arrow uses a specific ABI namespace absl (e.g., `absl::debian3` on Debian/Ubuntu), which is incompatible with a custom-installed absl.

**Solution**: Do not set `ABSL_STATIC_PREFIX`; let CMake automatically select the absl version matching the system Arrow. The build system will try static `.a` files under system paths first, then fall back to dynamic `.so`. Only specify this parameter when the custom absl namespace matches the system Arrow's.

### Q: `generate_test_parquet` default output path

The default output path is hardcoded as `build/test_data_512mb.parquet`. Use the first command-line argument to specify a custom path:

```bash
./build/generate_test_parquet /tmp/my_test.parquet
```
