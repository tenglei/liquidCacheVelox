#!/bin/bash
# ============================================================================
# generate_test_parquet.sh — 生成 Liquid Cache 测试用 Parquet 文件
# ============================================================================
# 功能：
#   - 构建 generate_test_parquet 工具（如未构建）
#   - 支持指定输出路径、行数或目标文件大小
#   - 参数校验与错误处理
#   - 覆盖所有 Liquid Cache 支持的列格式：
#     int8/int16/int32/int64, uint8/uint16/uint32/uint64,
#     float32/float64, date32/date64, timestamp(s/ms/us/ns),
#     string(高基数+低基数), binary, decimal128(10,2)
#
# 用法：
#   ./generate_test_parquet.sh [选项]
#
# 选项：
#   -o, --output <path>    输出文件路径（默认: build/test_data_512mb.parquet）
#   -s, --size <GB>        目标 Parquet 文件大小（GB），自动计算行数
#   -r, --rows <count>     目标行数（默认: 5000000）
#   -c, --compression <c>  压缩格式（snappy|gzip|zstd|lz4|brotli|none）
#                           注意：当前 C++ 工具内置固定使用 Snappy，
#                           如需更换需修改 tools/generate_test_parquet.cpp
#   --cols <count>         列数量（注意：当前 C++ 工具固定为 20 列，
#                           如需调整需修改 tools/generate_test_parquet.cpp）
#   -d, --build-dir <dir>  CMake 构建目录（默认: build）
#   -j <N>                 并行编译任务数（默认: nproc）
#   -n, --dry-run          仅显示将执行的命令，不实际运行
#   -h, --help             显示此帮助
#
# 示例：
#   ./scripts/generate_test_parquet.sh                           # 默认 512MB
#   ./scripts/generate_test_parquet.sh -s 1                      # 生成 ~1GB 文件
#   ./scripts/generate_test_parquet.sh -r 10000000 -o /tmp/big.parquet
#   ./scripts/generate_test_parquet.sh -s 2 -o data/2gb.parquet  # ~2GB
# ============================================================================

set -euo pipefail

# ── 默认值 ───────────────────────────────────────────────────────────────
PROJECT_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="${PROJECT_ROOT}/build"
OUTPUT="${BUILD_DIR}/test_data_512mb.parquet"
TARGET_SIZE_GB=""
TARGET_ROWS="5000000"
COMPRESSION="snappy"
COL_COUNT=""
JOBS=$(nproc)
DRY_RUN=false

# 基于默认 20 列 schema，每行压缩后约 100 bytes
# 实际大小取决于数据分布（随机 vs 有序）和压缩算法
BYTES_PER_ROW_ESTIMATE=100

# ── 颜色输出 ─────────────────────────────────────────────────────────────
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
CYAN='\033[0;36m'
NC='\033[0m' # No Color

log_info()  { echo -e "${CYAN}[INFO]${NC}  $*"; }
log_ok()    { echo -e "${GREEN}[OK]${NC}    $*"; }
log_warn()  { echo -e "${YELLOW}[WARN]${NC}  $*"; }
log_error() { echo -e "${RED}[ERROR]${NC} $*" >&2; }

# ── 帮助 ──────────────────────────────────────────────────────────────────
show_help() {
    head -38 "$0" | tail -37
    exit 0
}

# ── 参数解析 ─────────────────────────────────────────────────────────────
while [[ $# -gt 0 ]]; do
    case "$1" in
        -o|--output)
            OUTPUT="$2"; shift 2 ;;
        -s|--size)
            TARGET_SIZE_GB="$2"; shift 2 ;;
        -r|--rows)
            TARGET_ROWS="$2"; shift 2 ;;
        -c|--compression)
            COMPRESSION="${2,,}"; shift 2 ;;  # 转小写
        --cols)
            COL_COUNT="$2"; shift 2 ;;
        -d|--build-dir)
            BUILD_DIR="$2"; shift 2 ;;
        -j)
            JOBS="$2"; shift 2 ;;
        -n|--dry-run)
            DRY_RUN=true; shift ;;
        -h|--help)
            show_help ;;
        *)
            log_error "未知选项: $1"
            echo "使用 -h 查看帮助"
            exit 1 ;;
    esac
done

# ── 参数校验 ─────────────────────────────────────────────────────────────

# 校验数字参数
validate_positive_number() {
    local val="$1" name="$2"
    if [[ ! "$val" =~ ^[0-9]+$ ]] || [[ "$val" -eq 0 ]]; then
        log_error "${name} 必须为正整数，得到: ${val}"
        exit 1
    fi
}

validate_positive_number "$JOBS" "并行任务数(-j)"

if [[ -n "$TARGET_SIZE_GB" ]]; then
    validate_positive_number "$TARGET_SIZE_GB" "目标文件大小(-s)"
fi

if [[ -n "$TARGET_ROWS" ]]; then
    validate_positive_number "$TARGET_ROWS" "行数(-r)"
fi

if [[ -n "$COL_COUNT" ]]; then
    validate_positive_number "$COL_COUNT" "列数(--cols)"
fi

# 校验压缩格式
VALID_COMPRESSIONS="snappy gzip zstd lz4 brotli none"
if ! echo "$VALID_COMPRESSIONS" | grep -qw "$COMPRESSION"; then
    log_error "不支持的压缩格式: ${COMPRESSION}"
    log_error "支持的格式: ${VALID_COMPRESSIONS}"
    exit 1
fi

# ── 环境预检查 ───────────────────────────────────────────────────────────
log_info "环境预检查..."

# 检查 cmake
if ! command -v cmake &>/dev/null; then
    log_error "未找到 cmake，请先安装: sudo apt install cmake"
    exit 1
fi
log_info "  cmake: $(cmake --version | head -1)"

# 检查 C++ 编译器
if command -v g++ &>/dev/null; then
    log_info "  编译器: $(g++ --version | head -1)"
elif command -v clang++ &>/dev/null; then
    log_info "  编译器: $(clang++ --version | head -1)"
else
    log_warn "  未检测到 g++/clang++，编译可能失败"
fi

# ── 计算行数（从目标文件大小估算）────────────────────────────────────────
if [[ -n "$TARGET_SIZE_GB" ]]; then
    TARGET_BYTES=$(( TARGET_SIZE_GB * 1024 * 1024 * 1024 ))
    TARGET_ROWS=$(( TARGET_BYTES / BYTES_PER_ROW_ESTIMATE ))
    log_info "目标文件大小: ${TARGET_SIZE_GB} GB"
    log_info "估算每行压缩后: ${BYTES_PER_ROW_ESTIMATE} bytes"
    log_info "估算目标行数: ${TARGET_ROWS} (实际大小可能有 ±20% 偏差)"
fi

# 检查输出目录所在磁盘剩余空间
_preserve_output_dir="$(dirname "$OUTPUT")"
if command -v df &>/dev/null && [[ -d "$_preserve_output_dir" ]] 2>/dev/null; then
    AVAIL_KB=$(df --output=avail "$_preserve_output_dir" 2>/dev/null | tail -1 | tr -d ' ' || echo "0")
    if [[ "$AVAIL_KB" =~ ^[0-9]+$ ]] && [[ "$AVAIL_KB" -gt 0 ]]; then
        AVAIL_MB=$(( AVAIL_KB / 1024 ))
        AVAIL_GB=$(( AVAIL_MB / 1024 ))
        ESTIMATED_SIZE_MB=$(( TARGET_ROWS * BYTES_PER_ROW_ESTIMATE / 1048576 ))
        if [[ $AVAIL_MB -lt $(( ESTIMATED_SIZE_MB * 2 )) ]]; then
            log_warn "  磁盘剩余空间: ${AVAIL_MB}MB (约 ${AVAIL_GB}GB)，预估文件 ~${ESTIMATED_SIZE_MB}MB"
            log_warn "  空间可能不足，生成可能失败"
        else
            log_info "  磁盘剩余空间: ${AVAIL_GB}GB (预估文件 ~${ESTIMATED_SIZE_MB}MB)"
        fi
    fi
fi

# 输出目录校验
OUTPUT_DIR="$(dirname "$OUTPUT")"
if [[ ! -d "$OUTPUT_DIR" ]]; then
    log_info "创建输出目录: $OUTPUT_DIR"
    $DRY_RUN || mkdir -p "$OUTPUT_DIR"
fi

# 检查输出文件是否已存在
if [[ -f "$OUTPUT" ]] && [[ "$DRY_RUN" == false ]]; then
    EXISTING_SIZE=$(du -h "$OUTPUT" 2>/dev/null | cut -f1 || echo "unknown")
    log_warn "输出文件已存在: $OUTPUT ($EXISTING_SIZE)"
    read -r -p "覆盖? [y/N] " REPLY
    if [[ ! "$REPLY" =~ ^[Yy]$ ]]; then
        log_info "已取消"
        exit 0
    fi
    rm -f "$OUTPUT"
fi

# ── 压缩格式说明 ─────────────────────────────────────────────────────────
if [[ "$COMPRESSION" != "snappy" ]]; then
    log_warn "当前 generate_test_parquet 工具内置使用 Snappy 压缩"
    log_warn "如需使用 ${COMPRESSION}，请修改 tools/generate_test_parquet.cpp 第 106 行"
    log_warn "将 .compression(arrow::Compression::SNAPPY) 改为 .compression(arrow::Compression::$(echo "$COMPRESSION" | tr '[:lower:]' '[:upper:]'))"
    log_warn "然后重新编译"
fi

if [[ -n "$COL_COUNT" ]] && [[ "$COL_COUNT" != "20" ]]; then
    log_warn "当前 generate_test_parquet 工具固定生成 20 列（覆盖全部 Liquid Cache 支持类型）"
    log_warn "如需调整列数，请修改 tools/generate_test_parquet.cpp 中的 schema 定义"
fi

# ── 列类型覆盖说明 ───────────────────────────────────────────────────────
log_info "当前工具生成的 20 列完整覆盖以下 Liquid Cache 支持类型："
log_info "  Integer (signed):   col_int8, col_int16, col_int32, col_int64"
log_info "  Integer (unsigned): col_uint8, col_uint16, col_uint32, col_uint64"
log_info "  Float:              col_float32, col_float64"
log_info "  Date:               col_date32, col_date64"
log_info "  Timestamp:          col_ts_s, col_ts_ms, col_ts_us, col_ts_ns"
log_info "  String:             col_string_high (高基数), col_string_low (低基数)"
log_info "  Binary:             col_binary"
log_info "  Decimal128:         col_decimal (10,2)"
log_info "共覆盖: int8/16/32/64, uint8/16/32/64, float32/64, date32/64,"
log_info "        timestamp(s/ms/us/ns), utf8, binary, decimal128"
log_info "兼容性说明: 所有 20 列均为 Liquid Cache 原生支持的格式，"
log_info "           无额外非支持格式（如 interval, list, map）"

# ── 构建 generate_test_parquet 工具 ───────────────────────────────────────
TOOL_PATH="${BUILD_DIR}/generate_test_parquet"

if [[ ! -x "$TOOL_PATH" ]]; then
    log_info "generate_test_parquet 未构建，开始构建..."
    if [[ ! -f "${BUILD_DIR}/CMakeCache.txt" ]]; then
        log_info "运行 CMake 配置..."
        if $DRY_RUN; then
            echo "  cmake -S \"$PROJECT_ROOT\" -B \"$BUILD_DIR\" -DCMAKE_BUILD_TYPE=Release"
        else
            cmake -S "$PROJECT_ROOT" -B "$BUILD_DIR" -DCMAKE_BUILD_TYPE=Release
        fi
    fi

    log_info "编译 generate_test_parquet..."
    if $DRY_RUN; then
        echo "  cmake --build \"$BUILD_DIR\" --target generate_test_parquet -j $JOBS"
    else
        cmake --build "$BUILD_DIR" --target generate_test_parquet -j "$JOBS"
        if [[ ! -x "$TOOL_PATH" ]]; then
            log_error "编译失败，未找到 $TOOL_PATH"
            exit 1
        fi
    fi
    log_ok "编译完成"
else
    log_info "使用已有二进制: $TOOL_PATH"
fi

# ── 执行生成 ─────────────────────────────────────────────────────────────
log_info "开始生成 Parquet 测试数据:"
log_info "  输出路径: $OUTPUT"
log_info "  目标行数: $TARGET_ROWS"
log_info "  列数:     20 (覆盖全部支持类型)"
log_info "  压缩:     Snappy (内置固定)"

START_TIME=$(date +%s)

if $DRY_RUN; then
    echo ""
    echo "  [DRY RUN] 将执行:"
    echo "  $TOOL_PATH \"$OUTPUT\" $TARGET_ROWS"
    echo ""
else
    echo ""
    "$TOOL_PATH" "$OUTPUT" "$TARGET_ROWS"
    EXIT_CODE=$?

    END_TIME=$(date +%s)
    ELAPSED=$(( END_TIME - START_TIME ))

    if [[ $EXIT_CODE -ne 0 ]]; then
        log_error "生成失败 (exit code: $EXIT_CODE)"
        exit $EXIT_CODE
    fi

    # 验证输出文件
    if [[ -f "$OUTPUT" ]]; then
        FILE_SIZE=$(du -h "$OUTPUT" | cut -f1)
        FILE_SIZE_BYTES=$(stat --format=%s "$OUTPUT" 2>/dev/null || stat -f%z "$OUTPUT" 2>/dev/null || echo 0)
        FILE_SIZE_MB=$(( FILE_SIZE_BYTES / 1048576 ))
        log_ok "生成成功!"
        log_info "  文件: $OUTPUT"
        log_info "  大小: $FILE_SIZE (${FILE_SIZE_MB} MB)"
        log_info "  耗时: ${ELAPSED}s"
    else
        log_error "生成完成但未找到输出文件: $OUTPUT"
        exit 1
    fi
fi

exit 0
