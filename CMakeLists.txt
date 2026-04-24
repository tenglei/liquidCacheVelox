cmake_minimum_required(VERSION 3.16)
project(liquid_cache_cpp LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

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

# ── Helper: full static link list ─────────────────────────────────
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
target_include_directories(liquid_cache_core
    PUBLIC
        ${CMAKE_SOURCE_DIR}/include
        ${Arrow_INCLUDE_DIR}
        ${Parquet_INCLUDE_DIR}
)
# Core lib only needs Arrow headers at compile time; actual linking
# is deferred to the final targets below.

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
# Target 5: verify_parquet  (temporary verifier)
# ══════════════════════════════════════════════════════════════════
add_executable(verify_parquet
    tools/verify_parquet.cpp
)
target_include_directories(verify_parquet PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${Arrow_INCLUDE_DIR}
    ${Parquet_INCLUDE_DIR}
)
target_link_libraries(verify_parquet PRIVATE
    ${STATIC_LINK_LIBS}
)
