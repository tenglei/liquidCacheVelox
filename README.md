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

## 自动化脚本

项目提供四个自动化脚本，位于 `scripts/` 目录下，覆盖从测试数据生成、Velox benchmark 构建、JNI 库构建到完整测试运行的常见操作。

### 脚本一览

| 脚本 | 功能 | 典型耗时 |
|------|------|----------|
| `generate_test_parquet.sh` | 构建并运行 Parquet 测试数据生成器 | 10-15s |
| `build_velox_benchmark.sh` | 配置并编译 Velox 集成 benchmark | 30-60s |
| `build_jni_library.sh` | 编译 JNI 共享库 (.so) | 10-20s |
| `run_all_tests.sh` | 运行全部单元测试 + Parquet 验证 | 构建+测试 ~2min |

所有脚本均支持 `-h/--help` 查看完整帮助，`-n/--dry-run` 预览将执行的命令，`-d/--build-dir` 指定构建目录。

---

### 1. `generate_test_parquet.sh` — 生成测试 Parquet 数据

构建 `generate_test_parquet` 工具并生成包含 20 列、覆盖全部 Liquid Cache 支持格式的 Parquet 测试文件。

**用法：**

```bash
./scripts/generate_test_parquet.sh [选项]
```

**常用参数：**

| 参数 | 默认值 | 说明 |
|------|--------|------|
| `-o, --output <path>` | `build/test_data_512mb.parquet` | 输出文件路径 |
| `-s, --size <GB>` | — | 目标文件大小（GB），自动换算行数 |
| `-r, --rows <count>` | `5000000` | 目标行数 |
| `-c, --compression <c>` | `snappy` | 压缩格式（snappy/gzip/zstd/lz4/brotli/none） |
| `-d, --build-dir <dir>` | `build` | CMake 构建目录 |
| `-j <N>` | `nproc` | 并行编译任务数 |
| `-n, --dry-run` | — | 仅显示命令，不实际执行 |

**使用场景：**

- 首次搭建环境后生成基准测试数据
- 生成不同规模的 Parquet 文件用于性能测试
- 为 `liquid_velox_benchmark` 和 `verify_parquet` 提供输入

**示例：**

```bash
# 默认配置（5M 行，~512MB，Snappy 压缩）
./scripts/generate_test_parquet.sh

# 生成 ~1GB 的 Parquet 文件
./scripts/generate_test_parquet.sh -s 1

# 自定义行数和输出路径
./scripts/generate_test_parquet.sh -r 10000000 -o /tmp/large.parquet

# 指定不同构建目录
./scripts/generate_test_parquet.sh -d build_debug -s 2
```

**注意：** 当前 C++ 工具内置固定 20 列、Snappy 压缩。脚本会在使用其他压缩或列数时发出警告，提示需修改 C++ 源码。

---

### 2. `build_velox_benchmark.sh` — 构建 Velox 集成 Benchmark

配置并编译 `liquid_velox_benchmark`，包含 ABI 兼容性检查（Velox bundled Arrow 18 vs 系统 Arrow 24）。

**用法：**

```bash
./scripts/build_velox_benchmark.sh [选项]
```

**常用参数：**

| 参数 | 默认值 | 说明 |
|------|--------|------|
| `-p, --velox-prefix <path>` | `/home/tenglei/code/velox/build` | Velox 构建目录路径 |
| `-d, --build-dir <dir>` | `build` | CMake 构建目录 |
| `-t, --build-type <type>` | `Release` | 构建类型（Release/Debug） |
| `-j <N>` | `nproc` | 并行编译任务数 |
| `--clean` | — | 清理构建目录后重新配置 |
| `-n, --dry-run` | — | 仅显示命令，不实际执行 |

**使用场景：**

- 系统/项目已有完整 Velox 构建产物
- 需要生成 `liquid_velox_benchmark` 二进制进行性能对比
- CI/CD 中自动构建 Velox benchmark

**示例：**

```bash
# 默认配置（使用 ~/code/velox/build）
./scripts/build_velox_benchmark.sh

# 指定 Velox 构建路径
./scripts/build_velox_benchmark.sh -p /opt/velox/build

# 清理后重新构建（Debug 模式）
./scripts/build_velox_benchmark.sh --clean -t Debug -j 8
```

**产出文件：**
- `build/liquid_velox_benchmark` — Velox 性能基准测试可执行文件
- `build/libliquid_cache_velox.a` — Velox 集成静态库

**ABI 注意事项：** 脚本会自动检测系统 Arrow 版本与 Velox bundled Arrow 18 的冲突并给出警告。编译选项 `-mavx2 -mfma -mavx -mf16c -mlzcnt -mbmi2` 确保与 Velox 的 ABI 兼容。

---

### 3. `build_jni_library.sh` — 构建 JNI 共享库

编译 `libliquid_cache_jni.so`，包含 JNI 头文件检测、动态库依赖分析和 JNI 符号导出验证。

**用法：**

```bash
./scripts/build_jni_library.sh [选项]
```

**常用参数：**

| 参数 | 默认值 | 说明 |
|------|--------|------|
| `-d, --build-dir <dir>` | `build` | CMake 构建目录 |
| `-t, --build-type <type>` | `Release` | 构建类型（Release/Debug） |
| `-j <N>` | `nproc` | 并行编译任务数 |
| `--clean` | — | 清理构建目录后重新配置 |
| `--with-velox <path>` | — | 同时启用 Velox（不推荐，见下文） |
| `-n, --dry-run` | — | 仅显示命令，不实际执行 |

**使用场景：**

- 需要在 Java/Scala 中通过 JNI 调用 Liquid Cache
- 验证 JNI 符号导出正确性
- CI/CD 中检查 JNI 库是否能正常构建

**示例：**

```bash
# 默认配置
./scripts/build_jni_library.sh

# Debug 构建 + 清理
./scripts/build_jni_library.sh -t Debug --clean

# 自定义构建目录
./scripts/build_jni_library.sh -d build_jni
```

**重要警告：** 不建议使用 `--with-velox`。JNI 代码基于系统 Arrow 24 API，与 Velox bundled Arrow 18 ABI 不兼容。若构建目录中已有 Velox 缓存配置，脚本会自动检测并强制禁用 Velox（`-DLIQUID_ENABLE_VELOX=OFF`）。

**高级说明：** 构建完成后脚本会执行：
- `ldd` / `readelf` 动态库依赖分析
- `nm` JNI 符号检查（`Java_` 前缀导出函数）

---

### 4. `run_all_tests.sh` — 运行全部测试

构建（可选）并运行所有单元测试套件和 Parquet 验证，生成 PASS/FAIL/SKIP 汇总报告。

**用法：**

```bash
./scripts/run_all_tests.sh [选项]
```

**常用参数：**

| 参数 | 默认值 | 说明 |
|------|--------|------|
| `-p, --parquet <path>` | `build/test_data_512mb.parquet` | Parquet 测试文件路径 |
| `-d, --build-dir <dir>` | `build` | CMake 构建目录 |
| `-t, --build-type <type>` | `Release` | 构建类型（Release/Debug） |
| `-j <N>` | `nproc` | 并行编译任务数 |
| `--with-velox <path>` | — | 启用 Velox 交叉验证测试 |
| `--clean` | — | 清理构建目录后重新配置 |
| `--no-build` | — | 跳过构建，仅运行已有测试 |
| `--gtest-filter <f>` | `*` | Google Test 过滤器 |
| `-n, --dry-run` | — | 仅显示命令，不实际执行 |

**测试套件：**

| 序号 | 测试名称 | 二进制文件 | 说明 |
|------|----------|-----------|------|
| 1 | 核心往返测试 | `liquid_cache_tests` | 编解码往返正确性（37 tests） |
| 2 | Velox 交叉验证 | `liquid_velox_tests` | 仅 `--with-velox` 时运行 |
| 3 | LinearInteger 测试 | `liquid_linear_test` | 线性整数编码测试 |
| 4 | 浮点量化测试 | `liquid_float_quantize_test` | 浮点量化算法测试（10 tests） |
| 5 | 缓存预算/LRU 测试 | `liquid_cache_budget_test` | 缓存策略测试（19 tests） |
| 6 | Parquet 文件验证 | `verify_parquet` | 行数/列数/Row Group 完整性 |

**使用场景：**

- 开发完成后验证所有功能正确性
- 修改编码算法后运行回归测试
- CI/CD 流水线中的自动化测试步骤

**示例：**

```bash
# 完整测试流程（构建 + 全部测试）
./scripts/run_all_tests.sh

# 仅运行测试（跳过编译）
./scripts/run_all_tests.sh --no-build

# 指定 Parquet 文件并只运行特定 Google Test
./scripts/run_all_tests.sh -p /tmp/test.parquet --gtest-filter "LiquidCache*"

# 启用 Velox 集成测试
./scripts/run_all_tests.sh --with-velox /home/tenglei/code/velox/build
```

**输出示例（汇总报告）：**

```
╔══════════════════════════════════════════════════════════════╗
║                    测试汇总报告                             ║
╚══════════════════════════════════════════════════════════════╝

  总计测试套件:  6
  通过:          4
  失败:          0
  跳过:          2
  总耗时:        125s

  序号 测试名称                             状态       耗时/详情
  ---- ----------------------------------- ---------- --------------------
  1    核心往返测试 (test_roundtrip)        PASS       3.2s
  2    Velox 交叉验证 (test_velox_crossval) SKIP       未启用Velox
  3    LinearInteger 测试                   PASS       0.5s
  4    浮点量化测试                         PASS       0.3s
  5    缓存预算/LRU 测试                    PASS       2.1s
  6    Parquet 文件验证                     PASS       1.8s

  ✓ 所有测试通过!
```

---

### 典型工作流

以下展示如何将四个脚本组合使用，覆盖从零开始到完整验证的流程：

```bash
cd /home/tenglei/code/liquid-cache-cpp

# ── Step 1: 生成测试 Parquet 数据 ──────────────────────────
./scripts/generate_test_parquet.sh -s 1 -o build/test_1gb.parquet

# ── Step 2: 运行全部单元测试（验证核心功能） ───────────────
./scripts/run_all_tests.sh -p build/test_1gb.parquet

# ── Step 3: 构建 Velox Benchmark（如有 Velox） ─────────────
./scripts/build_velox_benchmark.sh -p /home/tenglei/code/velox/build

# ── Step 4: 验证 Velox 转换正确性 ─────────────────────────
./build/liquid_velox_benchmark build/test_1gb.parquet verify

# ── Step 5: 运行 Velox 性能基准测试 ────────────────────────
./build/liquid_velox_benchmark build/test_1gb.parquet bench

# ── Step 6: 构建 JNI 共享库（供 Java 调用） ────────────────
./scripts/build_jni_library.sh
```

**CI/CD 集成：**

GitHub Actions workflow 文件位于 `.github/workflows/ci.yml`，自动执行本项目的构建、测试和验证流程。详见 workflow 文件中的注释。

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
