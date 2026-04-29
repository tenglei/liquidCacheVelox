# liquid-cache-cpp

C++20 高性能列式数据内存缓存与编码压缩库，支持 Parquet 数据的高效编码解码，可选集成 Facebook Velox 向量引擎。

## 目录结构

```
liquid-cache-cpp/
├── CMakeLists.txt                    # 主构建配置
├── include/liquid_cache/             # 头文件
│   ├── transcoder.h                  # 转码主接口
│   ├── liquid_cache_store.h          # 列式缓存存储 (LRU)
│   ├── liquid_arrays.h               # 编码数组实现
│   ├── liquid_decimal_array.h        # Decimal 类型支持
│   ├── liquid_fixed_len_byte_array.h # 定长字节数组
│   ├── liquid_byte_view_array.h      # ByteView 数组
│   ├── liquid_to_velox.h             # Velox 向量转换
│   ├── fsst.h                        # FSST 字符串压缩
│   ├── bit_packed_array.h            # 位打包工具
│   ├── ipc_header.h                  # IPC 头部格式
│   ├── lru_policy.h                  # LRU 淘汰策略
│   └── jni_bridge.h                  # JNI 桥接头文件
├── src/
│   ├── transcoder_arrow.cpp          # Arrow 集成与编解码
│   ├── jni_bridge.cpp                # JNI 桥接实现
│   └── liquid_to_velox.cpp           # Velox 向量转换实现
├── examples/
│   ├── transcode_example.cpp         # 基础转码示例
│   └── velox_benchmark.cpp           # Velox 性能基准测试
├── tools/
│   ├── generate_test_parquet.cpp     # 测试 Parquet 数据生成器
│   └── verify_parquet.cpp            # Parquet 文件验证器
└── tests/
    ├── test_roundtrip.cpp            # 编解码往返测试
    ├── test_velox_crossval.cpp       # Velox 交叉验证测试
    ├── test_linear_integer.cpp       # LinearInteger 测试
    ├── test_float_quantize.cpp       # 浮点量化测试
    └── test_cache_budget.cpp         # 缓存预算和 LRU 测试
```

## 前置依赖

### 编译器

- GCC 13.3+（需要 C++20 支持）
- 当前验证环境：GCC 13.3.0 (Ubuntu 24.04)

### 必需依赖

| 依赖 | 版本 | 用途 |
|------|------|------|
| CMake | >= 3.16 | 构建系统 |
| Apache Arrow | 24.0.0 | 列式数据框架 |
| Apache Parquet | 24.0.0 | Parquet 文件读写 |
| Abseil | 静态库 | Google 基础库 |
| JNI | — | Java Native Interface 头文件 |
| OpenSSL (ssl/crypto) | 3.0.13 | 加密/安全 |
| Thrift | 0.19.0 | RPC 序列化 |
| Protobuf | — | 序列化 |
| Snappy | — | 数据压缩 |
| RE2 | 10.0.0 | 正则表达式 |
| LZ4 | 1.9.4 | 快速压缩 |
| Zstd | — | 高压缩比 |
| Brotli | 1.1.0 | Google 压缩库 |
| libxml2, nghttp2, gssapi_krb5, curl | — | Arrow 系统级传递依赖 |

### 可选依赖 (Velox 集成)

启用 `-DLIQUID_ENABLE_VELOX=ON` 时需要：

| 依赖 | 说明 |
|------|------|
| Facebook Velox | 完整构建产物，含 bundled Arrow 18 + Parquet |
| Folly | Facebook C++ 工具库 |
| fmt | 格式化库 |
| SimdJson | JSON 解析 |
| DuckDB | 嵌入式数据库（静态库） |
| Boost | context, filesystem, program_options, regex, thread |
| glog / gflags | 日志和命令行解析 |
| double-conversion | 浮点转换 |
| libevent / libsodium / libunwind | 系统传递依赖 |
| ICU (icui18n, icuuc, icudata) | 国际化支持 |

## CMake 配置参数

| 参数 | 类型 | 默认值 | 说明 |
|------|------|--------|------|
| `CMAKE_BUILD_TYPE` | STRING | — | 构建类型，推荐 `Release` |
| `LIQUID_ENABLE_VELOX` | BOOL | `OFF` | 启用 Velox 向量引擎集成 |
| `VELOX_PREFIX` | STRING | `""` | Velox 构建目录路径（启用 Velox 时必填） |
| `ABSL_STATIC_PREFIX` | STRING | `""` | 静态 Abseil 安装路径，如 `/opt/absl-static` |
| `LIQUID_BUILD_TESTS` | BOOL | `ON` | 构建 Google Test 单元测试 |

## 构建步骤

### 基础构建（不含 Velox）

基础构建将生成所有核心组件：静态库、JNI 共享库、示例程序和工具。

```bash
cd /home/tenglei/code/liquid-cache-cpp
mkdir -p build && cd build

# 配置（系统 Arrow 24 使用 DEBIAN3 ABI namespace 的 absl，无需额外指定）
cmake .. -DCMAKE_BUILD_TYPE=Release

# 构建全部目标
cmake --build . -j$(nproc)

# 或仅构建特定目标
cmake --build . --target generate_test_parquet -j$(nproc)
cmake --build . --target liquid_cache_example -j$(nproc)
```

> **注意**：如果系统 Arrow 的 absl ABI namespace 与自定义安装的 absl 不同（如系统为 `absl::debian3`，自定义为 `absl`），链接会失败并出现 undefined reference 错误。此时不应设置 `ABSL_STATIC_PREFIX`，让 CMake 自动使用系统 absl。仅当自定义 absl 的 namespace 与系统 Arrow 一致时才指定该参数。

**产出文件**（在 `build/` 目录下）：

| 文件 | 路径 | 类型 |
|------|------|------|
| `generate_test_parquet` | `build/generate_test_parquet` | 可执行文件 |
| `verify_parquet` | `build/verify_parquet` | 可执行文件 |
| `liquid_cache_example` | `build/liquid_cache_example` | 可执行文件 |
| `libliquid_cache_core.a` | `build/libliquid_cache_core.a` | 静态库 |
| `libliquid_cache_jni.so` | `build/libliquid_cache_jni.so` | 共享库 |

### Velox 集成构建

> **重要**：Velox 使用 bundled Arrow 18（不是系统 Arrow 24），两者 ABI 不兼容。启用 Velox 后，所有目标统一使用 Velox 的 bundled Arrow 18 头文件和库文件。

```bash
cd /home/tenglei/code/liquid-cache-cpp
mkdir -p build && cd build

# 配置（将 /path/to/velox/build 替换为你的 Velox 构建目录）
cmake .. \
  -DCMAKE_BUILD_TYPE=Release \
  -DLIQUID_ENABLE_VELOX=ON \
  -DVELOX_PREFIX=/path/to/velox/build

# 构建 benchmark
cmake --build . --target liquid_velox_benchmark -j$(nproc)

# 或构建全部目标（含 Velox 相关）
cmake --build . -j$(nproc)
```

**额外产出：**

| 文件 | 路径 | 类型 |
|------|------|------|
| `liquid_velox_benchmark` | `build/liquid_velox_benchmark` | 可执行文件 |
| `libliquid_cache_velox.a` | `build/libliquid_cache_velox.a` | 静态库 |

**编译选项：** Velox 目标使用 `-mavx2 -mfma -mavx -mf16c -mlzcnt -mbmi2` 确保与 Velox 的 ABI 兼容。

**关键注意点：**
- `-Wl,--whole-archive` 包裹 `libarrow.a` 是必须的 —— Arrow 计算内核（如 `min_max`）通过静态初始化器注册，不加此选项链接器会丢弃这些 .o 文件，导致运行时报 `"No function registered with name: min_max"` 错误
- 若设置了 `LIQUID_ENABLE_VELOX=ON` 但未指定 `VELOX_PREFIX`，CMake 会输出警告并跳过 Velox 目标
- 系统 Arrow 24 使用系统 thrift/protobuf/re2 等库的 DEBIAN3 ABI namespace 版本；Velox 的 bundled Arrow 18 使用自己的版本。两者混用导致链接或运行时崩溃

## 生成测试 Parquet 数据

`generate_test_parquet` 生成包含所有 Liquid Cache 支持字段类型的合成 Parquet 测试文件。

### 用法

```bash
# 默认配置：5M 行，输出到 build/test_data_512mb.parquet
./build/generate_test_parquet

# 自定义输出路径
./build/generate_test_parquet /path/to/output.parquet

# 自定义输出路径和行数
./build/generate_test_parquet /path/to/output.parquet 10000000
```

### 默认参数

- **行数**：5,000,000（约 512MB Parquet 文件）
- **列数**：20 列
- **批大小**：100,000 行/批
- **压缩**：Snappy
- **随机种子**：42（固定，可复现）

### 生成的 Schema (20 列)

| 类别 | 列名 | 类型 |
|------|------|------|
| 整数 | `col_int8`, `col_int16`, `col_int32`, `col_int64` | int8, int16, int32, int64 |
| 无符号整数 | `col_uint8`, `col_uint16`, `col_uint32`, `col_uint64` | uint8, uint16, uint32, uint64 |
| 浮点数 | `col_float32`, `col_float64` | float32, float64 |
| 日期 | `col_date32`, `col_date64` | date32, date64 |
| 时间戳 | `col_ts_s`, `col_ts_ms`, `col_ts_us`, `col_ts_ns` | timestamp(s/ms/us/ns) |
| 字符串 | `col_string_high`（高基数）, `col_string_low`（低基数） | utf8 |
| 二进制 | `col_binary` | binary |
| 十进制 | `col_decimal` | decimal128(10,2) |

### 示例输出

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

## 验证 Parquet 文件

```bash
./build/verify_parquet test_data_512mb.parquet
```

输出示例：
```
Rows: 5000000
Cols: 20
Schema: col_int8:int8, col_int16:int16, col_int32:int32, ...
Row Groups: 50
```

## 运行 Velox 性能基准测试

`liquid_velox_benchmark` 对比以下两条路径的性能：

1. **Velox Parquet Reader** → Velox Vector（从内存中的 Parquet 数据读取）
2. **Liquid Cache** → Velox Vector（从内存中的 Liquid 编码解码）

两条路径都从内存中的 Parquet 数据开始，确保公平比较，排除磁盘 I/O 差异。

### 用法

```bash
# 性能基准模式（默认）
./build/liquid_velox_benchmark /path/to/test_data.parquet bench

# 转换验证模式
./build/liquid_velox_benchmark /path/to/test_data.parquet verify

# 省略 mode 参数则默认为 bench
./build/liquid_velox_benchmark /path/to/test_data.parquet
```

### 基准测试参数

| 参数 | 值 | 说明 |
|------|-----|------|
| `ITERS` | 50 | 每场景测量迭代次数 |
| `WARMUP` | 3 | 每场景预热迭代次数 |
| Batch Size | 8192 | 每批行数 |

### 测试场景

基准测试根据 Parquet 文件的 schema 自动选择测试场景：

- **单列测试**：Int32 (FoR+BitPacking), Int64, Float64 (ALP), Timestamp, String Low (FSST+Dict), String High (FSST+Dict), Decimal128
- **混合测试**：3 列分析混合 (int32+float64+string_high)、5 列分析混合 (int32+int64+float64+ts_us+string_low)
- **全表测试**：所有 20 列

### 输出指标

每个场景输出以下统计信息（分别针对 Velox Parquet Reader 和 Liquid Cache 路径）：

| 指标 | 说明 |
|------|------|
| Mean | 平均耗时 (ms) |
| Median | 中位数耗时 (ms) |
| StdDev | 标准差 |
| P5 / P95 | 5 分位 / 95 分位 |
| CI95± | 95% 置信区间半宽 |
| Speedup | Liquid Cache 相对 Parquet Reader 的加速比 |
| rows/sec | 每秒处理行数 |
| MB/sec | 每秒处理数据量 |

异常值通过 MAD (Median Absolute Deviation) 方法剔除（4x MAD 阈值）。

### 输出示例

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

## 完整构建与测试工作流

以下是从零开始的完整流程（使用本环境验证路径）：

```bash
# ── Step 1: 基础构建 ────────────────────────────────────────
cd /home/tenglei/code/liquid-cache-cpp
mkdir -p build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release
cmake --build . --target generate_test_parquet -j$(nproc)

# ── Step 2: 生成测试数据 ────────────────────────────────────
./generate_test_parquet
# 可选：验证生成的文件
./verify_parquet test_data_512mb.parquet

# ── Step 3: Velox 集成构建 ──────────────────────────────────
cmake .. \
  -DCMAKE_BUILD_TYPE=Release \
  -DLIQUID_ENABLE_VELOX=ON \
  -DVELOX_PREFIX=/home/tenglei/code/velox/build
cmake --build . --target liquid_velox_benchmark -j$(nproc)

# ── Step 4: 验证转换正确性 ──────────────────────────────────
./liquid_velox_benchmark test_data_512mb.parquet verify

# ── Step 5: 运行性能基准测试 ────────────────────────────────
./liquid_velox_benchmark test_data_512mb.parquet bench
```

### 基准测试结果 (5M rows, 20 cols, WSL2 Ubuntu 24.04)

所有场景均已通过 verify 模式验证（611 batches, 0 FAIL）。以下为 bench 模式实测数据：

| 场景 | 列数 | Parquet->Velox (ms) | Liquid->Velox (ms) | 加速比 | 吞吐量 (rows/s) |
|------|------|---------------------|--------------------|--------|-----------------|
| Int32 (FoR+BitPacking) | 1 | 252.62 | 7.04 | **35.91x** | 710M |
| Int64 (FoR+BitPacking) | 1 | 291.82 | 8.16 | **35.75x** | 612M |
| Float64 (ALP) | 1 | 263.61 | 13.96 | **18.88x** | 358M |
| Timestamp (FoR+BitPacking) | 1 | 308.40 | 7.89 | **39.10x** | 634M |
| String Low (FSST+Dict, 低基) | 1 | 242.80 | 56.67 | **4.28x** | 88M |
| String High (FSST+Dict, 高基) | 1 | 374.25 | 61.27 | **6.11x** | 82M |
| Decimal128 (FoR+BitPacking) | 1 | 290.01 | 7.34 | **39.50x** | 681M |
| Analytics 3-col | 3 | 386.59 | 78.66 | **4.91x** | 64M |
| Analytics 5-col | 5 | 386.78 | 87.80 | **4.41x** | 57M |
| Full Table | 20 | 1050.87 | 301.57 | **3.48x** | 17M |

## 常见问题

### Q: 构建时报 `No function registered with name: min_max`

**原因**：链接器丢弃了 `libarrow.a` 中的计算内核（它们通过静态初始化器注册，链接器认为未被引用）。

**解决**：确保 CMakeLists.txt 中使用 `-Wl,--whole-archive libarrow.a -Wl,--no-whole-archive` 包裹 Arrow 库。

### Q: Velox 集成时编译报错（头文件找不到 / ABI 不兼容）

**原因**：Velox 使用 bundled Arrow 18，与系统 Arrow 24 不兼容。

**解决**：
1. 设置 `LIQUID_ENABLE_VELOX=OFF` 使用系统 Arrow 24（不含 Velox 功能）
2. 或设置 `LIQUID_ENABLE_VELOX=ON` 并指定正确的 `VELOX_PREFIX`（此时所有目标使用 bundled Arrow 18）

### Q: JNI 共享库链接时报 PIC 相关错误

**原因**：系统静态库 `.a` 未使用 `-fPIC` 编译，无法链入 `.so`。

**解决**：JNI 共享库已配置为对非标准依赖使用动态库版本（系统 `.so`）。

### Q: 编译时找不到 absl 符号

**原因**：系统 Arrow 使用特定 ABI namespace 的 absl（如 Debian/Ubuntu 上为 `absl::debian3`），与自定义安装的 absl 不兼容。

**解决**：不要设置 `ABSL_STATIC_PREFIX`，让 CMake 自动选择与系统 Arrow 匹配的 absl 版本。构建系统会依次尝试系统路径下的静态 `.a` 文件，最后回退到动态 `.so`。仅当自定义 absl 的 namespace 与系统 Arrow 一致时才指定该参数。

### Q: `generate_test_parquet` 默认输出路径

默认输出路径硬编码为 `build/test_data_512mb.parquet`。通过第一个命令行参数可指定自定义路径：

```bash
./build/generate_test_parquet /tmp/my_test.parquet
```
