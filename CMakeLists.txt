cmake_minimum_required(VERSION 3.16)
project(liquid_cache_cpp LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

# Optimized: add general performance flags for FSST and BitPacking
if(NOT CMAKE_BUILD_TYPE OR CMAKE_BUILD_TYPE STREQUAL "Release")
    set(CMAKE_CXX_FLAGS_RELEASE "${CMAKE_CXX_FLAGS_RELEASE} -O3 -march=native -ffast-math")
endif()

# ══════════════════════════════════════════════════════════════════
# Velox Integration (optional, must be checked BEFORE finding deps)
# ══════════════════════════════════════════════════════════════════
option(LIQUID_ENABLE_VELOX "Enable Velox vector integration" OFF)
set(VELOX_PREFIX "" CACHE STRING "Velox build directory (e.g. /path/to/velox/build)")

option(LIQUID_ENABLE_SIMD "Enable AVX2/BMI2 SIMD acceleration for BitPacking" ON)

# Check for SIMD compiler support
if(LIQUID_ENABLE_SIMD)
    include(CheckCXXCompilerFlag)
    check_cxx_compiler_flag(-mavx2 COMPILER_SUPPORTS_AVX2)
    check_cxx_compiler_flag(-mbmi2 COMPILER_SUPPORTS_BMI2)
    if(COMPILER_SUPPORTS_AVX2 AND COMPILER_SUPPORTS_BMI2)
        set(LIQUID_SIMD_FLAGS -mavx2 -mbmi2)
        message(STATUS "SIMD acceleration enabled: AVX2 + BMI2")
    else()
        set(LIQUID_SIMD_FLAGS "")
        message(STATUS "SIMD acceleration disabled: compiler lacks AVX2/BMI2 support")
    endif()
else()
    set(LIQUID_SIMD_FLAGS "")
    message(STATUS "SIMD acceleration disabled by user")
endif()

# ── Find Dependencies ──────────────────────────────────────────────
find_package(Threads REQUIRED)
find_package(Arrow REQUIRED)
find_package(Parquet REQUIRED)
find_package(JNI REQUIRED)

# ── Locate static libraries ───────────────────────────────────────
# Arrow ships .a files alongside .so on Ubuntu/Debian
find_library(ARROW_STATIC_LIB   NAMES libarrow.a   PATHS /usr/lib/x86_64-linux-gnu NO_DEFAULT_PATH)
find_library(ARROW_COMPUTE_STATIC_LIB NAMES libarrow_compute.a PATHS /usr/lib/x86_64-linux-gnu NO_DEFAULT_PATH)
find_library(PARQUET_STATIC_LIB NAMES libparquet.a  PATHS /usr/lib/x86_64-linux-gnu NO_DEFAULT_PATH)
set(ARROW_BUNDLED_LIB "/usr/lib/x86_64-linux-gnu/libarrow_bundled_dependencies.a")

message(STATUS "Arrow         .a : ${ARROW_STATIC_LIB}")
message(STATUS "Arrow Compute .a : ${ARROW_COMPUTE_STATIC_LIB}")
message(STATUS "Parquet       .a : ${PARQUET_STATIC_LIB}")
message(STATUS "Bundled       .a : ${ARROW_BUNDLED_LIB}")

# ── Abseil static libraries (key fix) ─────────────────────────────
# When ABSL_STATIC_PREFIX is provided (e.g. /opt/absl-static),
# use the .a files there so the final binaries carry no absl .so deps.
set(ABSL_STATIC_PREFIX "" CACHE STRING "Root of a static-only Abseil install")

if(ABSL_STATIC_PREFIX AND EXISTS "${ABSL_STATIC_PREFIX}/lib")
    message(STATUS "ABSL_STATIC_PREFIX = ${ABSL_STATIC_PREFIX}")
    file(GLOB ABSL_STATIC_LIBS "${ABSL_STATIC_PREFIX}/lib/libabsl_*.a")
    list(LENGTH ABSL_STATIC_LIBS _absl_count)
    message(STATUS "Using STATIC absl (${_absl_count} archives) from ${ABSL_STATIC_PREFIX}/lib")
else()
    # Fallback: use system absl static libraries (debian3 ABI namespace)
    # These match the ABI used by the system Arrow/Parquet bundled deps.
    file(GLOB ABSL_STATIC_LIBS "/usr/lib/x86_64-linux-gnu/libabsl_*.a")
    list(LENGTH ABSL_STATIC_LIBS _absl_count)
    if(_absl_count GREATER 0)
        message(STATUS "Using system STATIC absl (${_absl_count} archives) from /usr/lib/x86_64-linux-gnu")
    else()
        # Last resort: dynamic absl
        file(GLOB ABSL_STATIC_LIBS "/lib/x86_64-linux-gnu/libabsl_*.so")
        list(LENGTH ABSL_STATIC_LIBS _absl_count)
        message(STATUS "Falling back to ${_absl_count} absl .so libraries")
    endif()
endif()

# 查找静态版本的非标准库（优先 .a，找不到回退动态名称）
find_library(THRIFT_STATIC_LIB       NAMES libthrift.a       PATHS /usr/lib/x86_64-linux-gnu NO_DEFAULT_PATH)
find_library(PROTOBUF_STATIC_LIB     NAMES libprotobuf.a     PATHS /usr/lib/x86_64-linux-gnu NO_DEFAULT_PATH)
find_library(SNAPPY_STATIC_LIB       NAMES libsnappy.a       PATHS /usr/lib/x86_64-linux-gnu NO_DEFAULT_PATH)
find_library(RE2_STATIC_LIB          NAMES libre2.a          PATHS /usr/lib/x86_64-linux-gnu NO_DEFAULT_PATH)
find_library(UTF8PROC_STATIC_LIB     NAMES libutf8proc.a     PATHS /usr/lib/x86_64-linux-gnu NO_DEFAULT_PATH)
find_library(LZ4_STATIC_LIB          NAMES liblz4.a          PATHS /usr/lib/x86_64-linux-gnu NO_DEFAULT_PATH)
find_library(ZSTD_STATIC_LIB         NAMES libzstd.a         PATHS /usr/lib/x86_64-linux-gnu NO_DEFAULT_PATH)
find_library(BROTLICOMMON_STATIC_LIB NAMES libbrotlicommon.a libbrotlicommon-static.a PATHS /usr/lib/x86_64-linux-gnu NO_DEFAULT_PATH)
find_library(BROTLIENC_STATIC_LIB    NAMES libbrotlienc.a    libbrotlienc-static.a    PATHS /usr/lib/x86_64-linux-gnu NO_DEFAULT_PATH)
find_library(BROTLIDEC_STATIC_LIB    NAMES libbrotlidec.a    libbrotlidec-static.a    PATHS /usr/lib/x86_64-linux-gnu NO_DEFAULT_PATH)
find_library(CURL_STATIC_LIB         NAMES libcurl.a         PATHS /usr/lib/x86_64-linux-gnu NO_DEFAULT_PATH)

# 回退到动态库名称
if(NOT THRIFT_STATIC_LIB)
    set(THRIFT_STATIC_LIB thrift)
endif()
if(NOT PROTOBUF_STATIC_LIB)
    set(PROTOBUF_STATIC_LIB protobuf)
endif()
if(NOT SNAPPY_STATIC_LIB)
    set(SNAPPY_STATIC_LIB snappy)
endif()
if(NOT RE2_STATIC_LIB)
    set(RE2_STATIC_LIB re2)
endif()
if(NOT UTF8PROC_STATIC_LIB)
    set(UTF8PROC_STATIC_LIB utf8proc)
endif()
if(NOT LZ4_STATIC_LIB)
    set(LZ4_STATIC_LIB lz4)
endif()
if(NOT ZSTD_STATIC_LIB)
    set(ZSTD_STATIC_LIB zstd)
endif()
if(NOT BROTLICOMMON_STATIC_LIB)
    set(BROTLICOMMON_STATIC_LIB brotlicommon)
endif()
if(NOT BROTLIENC_STATIC_LIB)
    set(BROTLIENC_STATIC_LIB brotlienc)
endif()
if(NOT BROTLIDEC_STATIC_LIB)
    set(BROTLIDEC_STATIC_LIB brotlidec)
endif()
if(NOT CURL_STATIC_LIB)
    set(CURL_STATIC_LIB curl)
endif()

set(NONSTD_STATIC_LIBS
    ${ZSTD_STATIC_LIB}
    ${BROTLICOMMON_STATIC_LIB}
    ${BROTLIENC_STATIC_LIB}
    ${BROTLIDEC_STATIC_LIB}
    ${LZ4_STATIC_LIB}
    ${RE2_STATIC_LIB}
    ${UTF8PROC_STATIC_LIB}
    ${THRIFT_STATIC_LIB}
    ${PROTOBUF_STATIC_LIB}
    ${SNAPPY_STATIC_LIB}
)

# 共享库回退到动态名称（系统 .a 通常无 -fPIC，不能链入 .so）
set(NONSTD_DYNAMIC_DEPS
    zstd
    brotlicommon
    brotlienc
    brotlidec
    lz4
    re2
    utf8proc
    thrift
    protobuf
    snappy
    curl
)

# ── System libraries needed by Arrow static build ─────────────────
# 标准系统库保持动态链接
# --whole-archive on libarrow.a pulls in all modules (incl. Azure Storage),
# which transitively need xml2, nghttp2, gssapi_krb5 via libcurl.
set(ARROW_SYSTEM_DEPS
    Threads::Threads
    ssl
    crypto
    z
    bz2
    xml2
    /usr/lib/x86_64-linux-gnu/libnghttp2.so.14
    /usr/lib/x86_64-linux-gnu/libgssapi_krb5.so.2
    curl
    dl
    rt
)

# ── Helper: full static link list (system Arrow 24) ───────────────
# Order matters for static linking:
#   parquet -> arrow -> bundled -> absl -> nonstd -> system
# --whole-archive on libarrow.a is REQUIRED: Arrow compute kernels
# (min_max, sum, etc.) register via static initializers. Without
# --whole-archive the linker strips those .o files, causing
# "No function registered with name: min_max" at runtime.
set(STATIC_LINK_LIBS
    -Wl,--start-group
    ${PARQUET_STATIC_LIB}
    -Wl,--whole-archive ${ARROW_STATIC_LIB} ${ARROW_COMPUTE_STATIC_LIB} ${ARROW_BUNDLED_LIB} -Wl,--no-whole-archive
    ${ABSL_STATIC_LIBS}
    ${NONSTD_STATIC_LIBS}
    -Wl,--end-group
    ${ARROW_SYSTEM_DEPS}
)

# 共享库使用动态非标准库（避免 .a 无 PIC 导致链接失败）
# absl 系统静态库同样无 -fPIC，需回退到动态版本
file(GLOB _ABSL_DYNAMIC_LIBS "/lib/x86_64-linux-gnu/libabsl_*.so")
set(JNI_LINK_LIBS
    -Wl,--start-group
    ${PARQUET_STATIC_LIB}
    -Wl,--whole-archive ${ARROW_STATIC_LIB} ${ARROW_COMPUTE_STATIC_LIB} ${ARROW_BUNDLED_LIB} -Wl,--no-whole-archive
    -Wl,--end-group
    ${_ABSL_DYNAMIC_LIBS}
    ${ARROW_SYSTEM_DEPS}
    ${NONSTD_DYNAMIC_DEPS}
)

# ══════════════════════════════════════════════════════════════════
# Target 1: liquid_cache_core  (static library)
# ══════════════════════════════════════════════════════════════════
add_library(liquid_cache_core STATIC
    src/transcoder_arrow.cpp
)
if(LIQUID_ENABLE_VELOX AND VELOX_PREFIX)
    # When Velox is enabled, compile with Velox's bundled Arrow 18 headers
    # to ensure ABI compatibility with libvelox.a.
    # CRITICAL: must also define LIQUID_ENABLE_VELOX so that LiquidArrayBase
    # has the same vtable layout (with to_velox()) across all translation units.
    # Objects are constructed here (transcoder_arrow.cpp) but to_velox() is
    # called from liquid_cache_velox — mismatched vtables cause SIGSEGV.
    get_filename_component(_VELOX_SRC_DIR "${VELOX_PREFIX}/.." ABSOLUTE)
    set(_VELOX_ARROW_INC "${VELOX_PREFIX}/CMake/resolve_dependency_modules/arrow/arrow_ep/install/include")
    target_include_directories(liquid_cache_core
        PUBLIC
            ${CMAKE_SOURCE_DIR}/include
            ${_VELOX_ARROW_INC}
            ${_VELOX_SRC_DIR}
            ${VELOX_PREFIX}
    )
    target_compile_definitions(liquid_cache_core
        PRIVATE LIQUID_ENABLE_VELOX NDEBUG
    )
    target_compile_options(liquid_cache_core PRIVATE -mavx2 -mfma -mavx -mf16c -mlzcnt -mbmi2)
else()
    target_include_directories(liquid_cache_core
        PUBLIC
            ${CMAKE_SOURCE_DIR}/include
            ${Arrow_INCLUDE_DIR}
            ${Parquet_INCLUDE_DIR}
    )
    if(LIQUID_SIMD_FLAGS)
        target_compile_options(liquid_cache_core PRIVATE ${LIQUID_SIMD_FLAGS})
        target_compile_definitions(liquid_cache_core PRIVATE __AVX2__)
    endif()
endif()

# ══════════════════════════════════════════════════════════════════
# Target 2: liquid_cache_jni  (shared library for JVM)
# ══════════════════════════════════════════════════════════════════
add_library(liquid_cache_jni SHARED
    src/jni_bridge.cpp
)
target_include_directories(liquid_cache_jni PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${JNI_INCLUDE_DIRS}
    ${Arrow_INCLUDE_DIR}
    ${Parquet_INCLUDE_DIR}
)
target_link_libraries(liquid_cache_jni PRIVATE
    liquid_cache_core
    ${JNI_LINK_LIBS}
)

# ══════════════════════════════════════════════════════════════════
# Target 3: liquid_cache_example  (executable)
# ══════════════════════════════════════════════════════════════════
add_executable(liquid_cache_example
    examples/transcode_example.cpp
)
target_include_directories(liquid_cache_example PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${Arrow_INCLUDE_DIR}
    ${Parquet_INCLUDE_DIR}
)
target_link_libraries(liquid_cache_example PRIVATE
    liquid_cache_core
    ${STATIC_LINK_LIBS}
)

# ══════════════════════════════════════════════════════════════════
# Target 4: generate_test_parquet  (temporary test data generator)
# ══════════════════════════════════════════════════════════════════
add_executable(generate_test_parquet
    tools/generate_test_parquet.cpp
)
target_include_directories(generate_test_parquet PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${Arrow_INCLUDE_DIR}
    ${Parquet_INCLUDE_DIR}
)
target_link_libraries(generate_test_parquet PRIVATE
    ${STATIC_LINK_LIBS}
)

# ══════════════════════════════════════════════════════════════════
# Target 5: verify_parquet  (Parquet 文件完整性验证工具)
# ══════════════════════════════════════════════════════════════════
add_executable(verify_parquet
    tools/verify_parquet.cpp
)


# ══════════════════════════════════════════════════════════════════
# Velox Integration (optional, requires LIQUID_ENABLE_VELOX=ON)
#
# When enabled, we use Velox's bundled Arrow 18 (with Parquet) for
# ALL targets to ensure ABI compatibility. System Arrow 24 cannot
# coexist because libvelox.a references Arrow 18-internal symbols.
# ══════════════════════════════════════════════════════════════════

if(LIQUID_ENABLE_VELOX AND VELOX_PREFIX)
    message(STATUS "Velox integration enabled, VELOX_PREFIX = ${VELOX_PREFIX}")

    # Velox source root (one level up from build dir)
    get_filename_component(VELOX_SRC_DIR "${VELOX_PREFIX}/.." ABSOLUTE)

    # ── Velox bundled Arrow 18 paths (with Parquet + Compute) ───────
    set(VELOX_ARROW_DIR     "${VELOX_PREFIX}/CMake/resolve_dependency_modules/arrow/arrow_ep")
    set(VELOX_ARROW_INC     "${VELOX_ARROW_DIR}/install/include")
    set(VELOX_ARROW_LIB     "${VELOX_ARROW_DIR}/install/lib/libarrow.a")
    set(VELOX_PARQUET_LIB   "${VELOX_ARROW_DIR}/install/lib/libparquet.a")
    # Arrow bundled deps may not exist when using system Thrift
    set(VELOX_ARROW_BUNDLED "${VELOX_ARROW_DIR}/install/lib/libarrow_bundled_dependencies.a")
    if(NOT EXISTS "${VELOX_ARROW_BUNDLED}")
        set(VELOX_ARROW_BUNDLED "")
    endif()

    # ── Velox include directories ──────────────────────────────────
    set(VELOX_INCLUDE_DIRS
        ${VELOX_SRC_DIR}
        ${VELOX_PREFIX}
        ${VELOX_PREFIX}/_deps/folly-src
        ${VELOX_PREFIX}/_deps/folly-build
        ${VELOX_PREFIX}/_deps/fmt-src/include
        ${VELOX_PREFIX}/_deps/simdjson-src/include
        ${VELOX_PREFIX}/_deps/duckdb-src/src/include
        ${VELOX_PREFIX}/_deps/duckdb-build
        ${VELOX_ARROW_INC}
    )

    # ── Velox static libraries (monolithic build) ──────────────────
    set(VELOX_LIB_PATH         "${VELOX_PREFIX}/lib/libvelox.a")
    set(VELOX_FOLLY_LIB_PATH   "${VELOX_PREFIX}/_deps/folly-build/libfolly.a")
    set(VELOX_FMT_LIB_PATH     "${VELOX_PREFIX}/_deps/fmt-build/libfmt.a")
    set(VELOX_SIMDJSON_LIB_PATH "${VELOX_PREFIX}/_deps/simdjson-build/libsimdjson.a")
    set(VELOX_STEMMER_LIB_PATH "${VELOX_PREFIX}/_deps/libstemmer/src/libstemmer/libstemmer.a")

    # DuckDB + sub-libraries
    set(VELOX_DUCKDB_LIB_PATH  "${VELOX_PREFIX}/_deps/duckdb-build/src/libduckdb_static.a")
    file(GLOB VELOX_DUCKDB_SUB_LIBS "${VELOX_PREFIX}/_deps/duckdb-build/third_party/**/*.a")

    # ── System transitive dependencies ─────────────────────────────
    find_package(Boost REQUIRED COMPONENTS context filesystem program_options regex thread)
    find_library(GLOG_LIB      NAMES glog      PATHS /usr/local/lib /usr/lib/x86_64-linux-gnu)
    find_library(GFLAGS_LIB    NAMES gflags    PATHS /usr/local/lib /usr/lib/x86_64-linux-gnu)
    find_library(DOUBLE_CONV_LIB NAMES double-conversion PATHS /usr/local/lib /usr/lib/x86_64-linux-gnu)
    find_library(EVENT_LIB     NAMES event     PATHS /usr/local/lib /usr/lib/x86_64-linux-gnu)
    find_library(SODIUM_LIB    NAMES sodium    PATHS /usr/lib/x86_64-linux-gnu)
    find_library(UNWIND_LIB    NAMES unwind    PATHS /usr/lib/x86_64-linux-gnu)
    find_library(PROTOBUF_LIB  NAMES protobuf  PATHS /usr/lib/x86_64-linux-gnu)
    find_library(LZMA_LIB      NAMES lzma      PATHS /usr/lib/x86_64-linux-gnu)
    find_library(ICUI18N_LIB   NAMES icui18n   PATHS /usr/lib/x86_64-linux-gnu)
    find_library(ICUUC_LIB     NAMES icuuc     PATHS /usr/lib/x86_64-linux-gnu)
    find_library(ICUDATA_LIB   NAMES icudata   PATHS /usr/lib/x86_64-linux-gnu)

    set(VELOX_TRANSITIVE_DEPS
        ${Boost_LIBRARIES}
        ${GLOG_LIB}
        ${GFLAGS_LIB}
        ${DOUBLE_CONV_LIB}
        ${EVENT_LIB}
        ${SODIUM_LIB}
        ${UNWIND_LIB}
        ${PROTOBUF_LIB}
        ${RE2_STATIC_LIB}
        ${SNAPPY_STATIC_LIB}
        ${LZ4_STATIC_LIB}
        ${ZSTD_STATIC_LIB}
        ${LZMA_LIB}
        ${ICUI18N_LIB}
        ${ICUUC_LIB}
        ${ICUDATA_LIB}
        ssl
        crypto
        z
        bz2
        dl
        rt
        pthread
    )

    # ── Arrow 18 static link list (replaces system Arrow 24) ───────
    # Use Velox bundled Arrow 18 + Parquet for ABI compatibility
    # --whole-archive on libarrow.a is REQUIRED: Arrow compute kernels
    # register via static initializers that the linker would otherwise strip.
    set(VELOX_ARROW_LINK_LIBS
        ${VELOX_PARQUET_LIB}
        -Wl,--whole-archive ${VELOX_ARROW_LIB} -Wl,--no-whole-archive
        ${VELOX_ARROW_BUNDLED}
        ${NONSTD_STATIC_LIBS}
        ${ARROW_SYSTEM_DEPS}
    )

    # Collect all Velox-related libraries in correct link order
    set(VELOX_ALL_LIBS
        ${VELOX_LIB_PATH}
        ${VELOX_FOLLY_LIB_PATH}
        ${VELOX_FMT_LIB_PATH}
        ${VELOX_SIMDJSON_LIB_PATH}
        ${VELOX_STEMMER_LIB_PATH}
        ${VELOX_DUCKDB_LIB_PATH}
        ${VELOX_DUCKDB_SUB_LIBS}
    )

    message(STATUS "Velox lib:         ${VELOX_LIB_PATH}")
    message(STATUS "Velox Arrow lib:   ${VELOX_ARROW_LIB}")
    message(STATUS "Velox Parquet lib: ${VELOX_PARQUET_LIB}")
    message(STATUS "Velox folly lib:   ${VELOX_FOLLY_LIB_PATH}")
    message(STATUS "Velox fmt lib:     ${VELOX_FMT_LIB_PATH}")

    # Target 6: liquid_cache_velox (static library)
    add_library(liquid_cache_velox STATIC
        src/liquid_to_velox.cpp
    )
    target_include_directories(liquid_cache_velox
        PUBLIC
            ${CMAKE_SOURCE_DIR}/include
            ${VELOX_INCLUDE_DIRS}
    )
    target_compile_definitions(liquid_cache_velox
        PRIVATE LIQUID_ENABLE_VELOX NDEBUG
    )
    # Match Velox's compile flags for ABI compatibility (especially F14 intrinsics mode)
    target_compile_options(liquid_cache_velox PRIVATE -mavx2 -mfma -mavx -mf16c -mlzcnt -mbmi2)

    # Target 7: liquid_velox_benchmark (executable)
    add_executable(liquid_velox_benchmark
        examples/velox_benchmark.cpp
    )
    target_include_directories(liquid_velox_benchmark PRIVATE
        ${CMAKE_SOURCE_DIR}/include
        ${VELOX_INCLUDE_DIRS}
    )
    target_compile_definitions(liquid_velox_benchmark
        PRIVATE LIQUID_ENABLE_VELOX NDEBUG
    )
    target_compile_options(liquid_velox_benchmark PRIVATE -mavx2 -mfma -mavx -mf16c -mlzcnt -mbmi2)
    target_link_libraries(liquid_velox_benchmark PRIVATE
        -Wl,--start-group
        liquid_cache_core
        liquid_cache_velox
        ${VELOX_ALL_LIBS}
        ${VELOX_ARROW_LINK_LIBS}
        -Wl,--end-group
        ${VELOX_TRANSITIVE_DEPS}
    )

    message(STATUS "Velox integration targets: liquid_cache_velox, liquid_velox_benchmark")

# verify_parquet Velox/non-Velox dual-mode linking
if(LIQUID_ENABLE_VELOX AND VELOX_PREFIX)
    target_include_directories(verify_parquet PRIVATE
        ${_VELOX_ARROW_INC}
    )
    target_link_libraries(verify_parquet PRIVATE
        -Wl,--start-group
        ${VELOX_ALL_LIBS}
        ${VELOX_ARROW_LINK_LIBS}
        -Wl,--end-group
        ${VELOX_TRANSITIVE_DEPS}
    )
else()
    target_include_directories(verify_parquet PRIVATE
        ${CMAKE_SOURCE_DIR}/include
        ${Arrow_INCLUDE_DIR}
        ${Parquet_INCLUDE_DIR}
    )
    target_link_libraries(verify_parquet PRIVATE
        ${STATIC_LINK_LIBS}
    )
endif()
else()
    if(LIQUID_ENABLE_VELOX AND NOT VELOX_PREFIX)
        message(WARNING "LIQUID_ENABLE_VELOX is ON but VELOX_PREFIX is not set. "
                        "Set -DVELOX_PREFIX=/path/to/velox/build to enable Velox integration.")
    endif()
endif()

# ══════════════════════════════════════════════════════════════════
# Target 6: bench_bit_packed  (microbenchmark)
# ══════════════════════════════════════════════════════════════════
add_executable(bench_bit_packed
    tools/bench_bit_packed.cpp
)
target_include_directories(bench_bit_packed PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${Arrow_INCLUDE_DIR}
)
target_link_libraries(bench_bit_packed PRIVATE
    liquid_cache_core
    ${STATIC_LINK_LIBS}
)
target_compile_options(bench_bit_packed PRIVATE -O2)
add_dependencies(bench_bit_packed liquid_cache_core)

# ══════════════════════════════════════════════════════════════════
# Testing (Google Test via FetchContent)
# ══════════════════════════════════════════════════════════════════
option(LIQUID_BUILD_TESTS "Build unit tests" ON)

if(LIQUID_BUILD_TESTS)
    include(FetchContent)
    FetchContent_Declare(googletest
        GIT_REPOSITORY https://github.com/google/googletest.git
        GIT_TAG v1.14.0)
    set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
    FetchContent_MakeAvailable(googletest)
    enable_testing()

    # Core round-trip tests (no Velox dependency)
    add_executable(liquid_cache_tests tests/test_roundtrip.cpp)
    if(LIQUID_ENABLE_VELOX AND VELOX_PREFIX)
        target_include_directories(liquid_cache_tests PRIVATE
            ${CMAKE_SOURCE_DIR}/include
            ${_VELOX_ARROW_INC}
        )
        target_compile_definitions(liquid_cache_tests PRIVATE LIQUID_ENABLE_VELOX NDEBUG)
        target_compile_options(liquid_cache_tests PRIVATE -mavx2 -mfma -mavx -mf16c -mlzcnt -mbmi2)
        target_link_libraries(liquid_cache_tests PRIVATE
            -Wl,--start-group
            liquid_cache_core
            liquid_cache_velox
            ${VELOX_ALL_LIBS}
            ${VELOX_ARROW_LINK_LIBS}
            -Wl,--end-group
            ${VELOX_TRANSITIVE_DEPS}
            GTest::gtest
        )
    else()
        target_include_directories(liquid_cache_tests PRIVATE
            ${CMAKE_SOURCE_DIR}/include
            ${Arrow_INCLUDE_DIR}
            ${Parquet_INCLUDE_DIR}
        )
        if(LIQUID_SIMD_FLAGS)
            target_compile_options(liquid_cache_tests PRIVATE ${LIQUID_SIMD_FLAGS})
            target_compile_definitions(liquid_cache_tests PRIVATE __AVX2__)
        endif()
        target_link_libraries(liquid_cache_tests PRIVATE
            liquid_cache_core
            ${STATIC_LINK_LIBS}
            GTest::gtest
        )
    endif()
    include(GoogleTest)
    gtest_discover_tests(liquid_cache_tests)

    # Velox cross-validation tests (only with Velox enabled)
    if(LIQUID_ENABLE_VELOX AND VELOX_PREFIX)
        add_executable(liquid_velox_tests tests/test_velox_crossval.cpp)
        target_include_directories(liquid_velox_tests PRIVATE
            ${CMAKE_SOURCE_DIR}/include
            ${VELOX_INCLUDE_DIRS}
        )
        target_compile_definitions(liquid_velox_tests PRIVATE LIQUID_ENABLE_VELOX NDEBUG)
        target_compile_options(liquid_velox_tests PRIVATE -mavx2 -mfma -mavx -mf16c -mlzcnt -mbmi2)
        target_link_libraries(liquid_velox_tests PRIVATE
            -Wl,--start-group
            liquid_cache_core
            liquid_cache_velox
            ${VELOX_ALL_LIBS}
            ${VELOX_ARROW_LINK_LIBS}
            -Wl,--end-group
            ${VELOX_TRANSITIVE_DEPS}
            GTest::gtest
        )
        gtest_discover_tests(liquid_velox_tests)

    endif()
    # LinearInteger roundtrip test (Arrow-based, no Velox dependency)
    add_executable(liquid_linear_test tests/test_linear_integer.cpp)
    if(LIQUID_ENABLE_VELOX AND VELOX_PREFIX)
        target_include_directories(liquid_linear_test PRIVATE
            ${CMAKE_SOURCE_DIR}/include
            ${_VELOX_ARROW_INC}
        )
        target_link_libraries(liquid_linear_test PRIVATE
            -Wl,--start-group
            liquid_cache_core
            liquid_cache_velox
            ${VELOX_ALL_LIBS}
            ${VELOX_ARROW_LINK_LIBS}
            -Wl,--end-group
            ${VELOX_TRANSITIVE_DEPS}
            GTest::gtest_main
        )
    else()
        target_include_directories(liquid_linear_test PRIVATE
            ${CMAKE_SOURCE_DIR}/include
            ${Arrow_INCLUDE_DIR}
            ${Parquet_INCLUDE_DIR}
        )
        target_link_libraries(liquid_linear_test PRIVATE
            liquid_cache_core
            ${STATIC_LINK_LIBS}
            GTest::gtest_main
        )
    endif()
    gtest_discover_tests(liquid_linear_test)

    # Float Quantization (Squeeze) roundtrip test (Arrow, no Velox)
    add_executable(liquid_float_quantize_test tests/test_float_quantize.cpp)
    if(LIQUID_ENABLE_VELOX AND VELOX_PREFIX)
        target_include_directories(liquid_float_quantize_test PRIVATE
            ${CMAKE_SOURCE_DIR}/include
            ${_VELOX_ARROW_INC}
        )
        target_link_libraries(liquid_float_quantize_test PRIVATE
            -Wl,--start-group
            liquid_cache_core
            liquid_cache_velox
            ${VELOX_ALL_LIBS}
            ${VELOX_ARROW_LINK_LIBS}
            -Wl,--end-group
            ${VELOX_TRANSITIVE_DEPS}
            GTest::gtest_main
        )
    else()
        target_include_directories(liquid_float_quantize_test PRIVATE
            ${CMAKE_SOURCE_DIR}/include
            ${Arrow_INCLUDE_DIR}
            ${Parquet_INCLUDE_DIR}
        )
        target_link_libraries(liquid_float_quantize_test PRIVATE
            liquid_cache_core
            ${STATIC_LINK_LIBS}
            GTest::gtest_main
        )
    endif()
    gtest_discover_tests(liquid_float_quantize_test)

    # Cache budget + LRU eviction test
    add_executable(liquid_cache_budget_test tests/test_cache_budget.cpp)
    if(LIQUID_ENABLE_VELOX AND VELOX_PREFIX)
        target_include_directories(liquid_cache_budget_test PRIVATE
            ${CMAKE_SOURCE_DIR}/include
            ${_VELOX_ARROW_INC}
        )
        target_link_libraries(liquid_cache_budget_test PRIVATE
            -Wl,--start-group
            liquid_cache_core
            liquid_cache_velox
            ${VELOX_ALL_LIBS}
            ${VELOX_ARROW_LINK_LIBS}
            -Wl,--end-group
            ${VELOX_TRANSITIVE_DEPS}
            GTest::gtest_main
        )
    else()
        target_include_directories(liquid_cache_budget_test PRIVATE
            ${CMAKE_SOURCE_DIR}/include
            ${Arrow_INCLUDE_DIR}
            ${Parquet_INCLUDE_DIR}
        )
        target_link_libraries(liquid_cache_budget_test PRIVATE
            liquid_cache_core
            ${STATIC_LINK_LIBS}
            GTest::gtest_main
        )
    endif()
    gtest_discover_tests(liquid_cache_budget_test)

    # BitPackedArray unit tests (header-only, no Arrow dependency)
    add_executable(liquid_bit_packed_test tests/test_bit_packed.cpp)
    target_include_directories(liquid_bit_packed_test PRIVATE
        ${CMAKE_SOURCE_DIR}/include
    )
    if(LIQUID_SIMD_FLAGS)
        target_compile_options(liquid_bit_packed_test PRIVATE ${LIQUID_SIMD_FLAGS})
        target_compile_definitions(liquid_bit_packed_test PRIVATE __AVX2__)
    endif()
    target_link_libraries(liquid_bit_packed_test PRIVATE
        GTest::gtest_main
    )
    gtest_discover_tests(liquid_bit_packed_test)

    # IPC Header unit tests (header-only, no Arrow dependency)
    add_executable(liquid_ipc_header_test tests/test_ipc_header.cpp)
    target_include_directories(liquid_ipc_header_test PRIVATE
        ${CMAKE_SOURCE_DIR}/include
    )
    target_link_libraries(liquid_ipc_header_test PRIVATE
        GTest::gtest_main
    )
    gtest_discover_tests(liquid_ipc_header_test)
endif()

