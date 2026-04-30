#!/bin/bash
# ============================================================================
# build_velox_benchmark.sh — 构建 liquid_velox_benchmark（Velox 集成）
# ============================================================================
# 功能：
#   - CMake 配置（LIQUID_ENABLE_VELOX=ON + VELOX_PREFIX）
#   - 编译 liquid_velox_benchmark 目标
#   - ABI 兼容性检查（Velox Arrow 18 vs 系统 Arrow 24）
#   - 构建状态报告与二进制位置确认
#
# 用法：
#   ./build_velox_benchmark.sh [选项]
#
# 选项：
#   -p, --velox-prefix <path>  Velox 构建目录路径（默认: /home/tenglei/code/velox/build）
#   -d, --build-dir <dir>      CMake 构建目录（默认: build）
#   -t, --build-type <type>    构建类型 Release|Debug（默认: Release）
#   -j <N>                     并行编译任务数（默认: nproc）
#   --clean                    清理构建目录后重新配置
#   -n, --dry-run              仅显示将执行的命令
#   -h, --help                 显示此帮助
#
# 示例：
#   ./scripts/build_velox_benchmark.sh
#   ./scripts/build_velox_benchmark.sh -p /opt/velox/build
#   ./scripts/build_velox_benchmark.sh --clean -j 8
# ============================================================================

set -euo pipefail

# ── 默认值 ───────────────────────────────────────────────────────────────
PROJECT_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="${PROJECT_ROOT}/build"
VELOX_PREFIX=""  # 自动检测，也可通过 -p 或 $VELOX_PREFIX 环境变量指定
BUILD_TYPE="Release"
JOBS=$(nproc)
DO_CLEAN=false
DRY_RUN=false

# ── 颜色输出 ─────────────────────────────────────────────────────────────
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
CYAN='\033[0;36m'
BLUE='\033[0;34m'
NC='\033[0m'

log_info()  { echo -e "${CYAN}[INFO]${NC}  $*"; }
log_ok()    { echo -e "${GREEN}[OK]${NC}    $*"; }
log_warn()  { echo -e "${YELLOW}[WARN]${NC}  $*"; }
log_error() { echo -e "${RED}[ERROR]${NC} $*" >&2; }
log_step()  { echo -e "${BLUE}[STEP]${NC}  $*"; }

# ── 帮助 ──────────────────────────────────────────────────────────────────
show_help() {
    head -35 "$0" | tail -34
    exit 0
}

# ── 参数解析 ─────────────────────────────────────────────────────────────
while [[ $# -gt 0 ]]; do
    case "$1" in
        -p|--velox-prefix)
            VELOX_PREFIX="$2"; shift 2 ;;
        -d|--build-dir)
            BUILD_DIR="$2"; shift 2 ;;
        -t|--build-type)
            BUILD_TYPE="$2"; shift 2 ;;
        -j)
            JOBS="$2"; shift 2 ;;
        --clean)
            DO_CLEAN=true; shift ;;
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
if [[ ! "$BUILD_TYPE" =~ ^(Release|Debug|RelWithDebInfo|MinSizeRel)$ ]]; then
    log_error "无效的构建类型: $BUILD_TYPE"
    log_error "有效值: Release | Debug | RelWithDebInfo | MinSizeRel"
    exit 1
fi

if [[ ! "$JOBS" =~ ^[0-9]+$ ]] || [[ "$JOBS" -eq 0 ]]; then
    log_error "-j 必须为正整数，得到: $JOBS"
    exit 1
fi

# ── 自动检测 VELOX_PREFIX ──────────────────────────────────────────────
if [[ -z "$VELOX_PREFIX" ]]; then
    log_info "未指定 --velox-prefix，尝试自动检测 Velox 构建目录..."

    # 1) 检查环境变量
    if [[ -n "${VELOX_PREFIX:-}" ]] && [[ -d "$VELOX_PREFIX" ]]; then
        VELOX_PREFIX="$VELOX_PREFIX"
        log_info "  从环境变量 \$VELOX_PREFIX 检测到: $VELOX_PREFIX"
    fi

    # 2) 搜索常见路径
    if [[ -z "$VELOX_PREFIX" ]]; then
        _search_paths=()
        for d in /home/*/code/velox/build /opt/velox/build "$HOME/code/velox/build" /usr/local/velox/build; do
            [[ -d "$d" ]] && _search_paths+=("$d")
        done

        if [[ ${#_search_paths[@]} -gt 0 ]]; then
            VELOX_PREFIX="${_search_paths[0]}"
            if [[ ${#_search_paths[@]} -gt 1 ]]; then
                log_info "  找到多个候选路径，使用第一个: $VELOX_PREFIX"
                for p in "${_search_paths[@]}"; do
                    log_info "    - $p"
                done
            else
                log_info "  自动检测到: $VELOX_PREFIX"
            fi
        fi
    fi

    # 3) 未找到
    if [[ -z "$VELOX_PREFIX" ]]; then
        log_error "无法自动检测 Velox 构建目录"
        log_error ""
        log_error "请通过以下方式之一指定:"
        log_error "  1. 命令行:    $0 -p /path/to/velox/build"
        log_error "  2. 环境变量:  export VELOX_PREFIX=/path/to/velox/build"
        log_error "  3. 确保 Velox 已构建在以下路径之一:"
        log_error "     /home/<user>/code/velox/build"
        log_error "     /opt/velox/build"
        log_error "     ~/code/velox/build"
        exit 1
    fi
fi

# ── 环境预检查 ───────────────────────────────────────────────────────────
log_info "环境预检查..."
if ! command -v cmake &>/dev/null; then
    log_error "未找到 cmake，请先安装: sudo apt install cmake"
    exit 1
fi
log_info "  cmake: $(cmake --version | head -1)"

if command -v g++ &>/dev/null; then
    log_info "  编译器: $(g++ --version | head -1)"
elif command -v clang++ &>/dev/null; then
    log_info "  编译器: $(clang++ --version | head -1)"
else
    log_warn "  未检测到 g++/clang++，编译可能失败"
fi

# ── Velox 路径校验 ───────────────────────────────────────────────────────
validate_velox_prefix() {
    local vprefix="$1"

    if [[ ! -d "$vprefix" ]]; then
        log_error "Velox 构建目录不存在: $vprefix"
        log_error "请确认 VELOX_PREFIX 路径，可通过 -p 指定"
        return 1
    fi

    # 检查关键文件
    local velox_lib="${vprefix}/lib/libvelox.a"
    local velox_arrow_inc="${vprefix}/CMake/resolve_dependency_modules/arrow/arrow_ep/install/include"
    local velox_folly_lib="${vprefix}/_deps/folly-build/libfolly.a"

    local missing=()
    [[ -f "$velox_lib" ]] || missing+=("libvelox.a")
    [[ -d "$velox_arrow_inc" ]] || missing+=("bundled Arrow 头文件")
    [[ -f "$velox_folly_lib" ]] || missing+=("libfolly.a")

    if [[ ${#missing[@]} -gt 0 ]]; then
        log_error "Velox 构建目录不完整，缺少:"
        for m in "${missing[@]}"; do
            echo "         - $m"
        done
        log_error "请在 $vprefix 执行完整 Velox 构建"
        return 1
    fi

    # 检查 Velox 使用的 Arrow 版本（bundled Arrow 18）
    local arrow_version_h="${vprefix}/CMake/resolve_dependency_modules/arrow/arrow_ep/install/include/arrow/util/config.h"
    if [[ -f "$arrow_version_h" ]]; then
        local arrow_ver
        arrow_ver=$(grep -oP 'ARROW_VERSION "\K[^"]+' "$arrow_version_h" 2>/dev/null || echo "unknown")
        log_info "Velox bundled Arrow 版本: $arrow_ver"
    fi

    return 0
}

log_step "验证 Velox 构建环境..."
if ! validate_velox_prefix "$VELOX_PREFIX"; then
    exit 1
fi
log_ok "Velox 构建环境完整"

# ── ABI 兼容性说明 ───────────────────────────────────────────────────────
log_info "ABI 兼容性检查:"
log_info "  Velox 使用 bundled Arrow 18 (ABI 与系统 Arrow 24 不兼容)"
log_info "  启用 LIQUID_ENABLE_VELOX 后，所有目标统一使用 Arrow 18 头文件"
log_info "  编译选项: -mavx2 -mfma -mavx -mf16c -mlzcnt -mbmi2 (匹配 Velox ABI)"

# 检查系统是否安装了 Arrow 24（可能产生冲突）
if pkg-config --exists arrow 2>/dev/null; then
    sys_arrow_ver=$(pkg-config --modversion arrow 2>/dev/null || echo "unknown")
    if [[ "$sys_arrow_ver" != "18"* ]]; then
        log_warn "检测到系统 Arrow $sys_arrow_ver，与 Velox bundled Arrow 18 不兼容"
        log_warn "此构建将使用 Velox bundled Arrow 18，不会使用系统 Arrow"
    fi
fi

# ── 清理构建目录 ─────────────────────────────────────────────────────────
if $DO_CLEAN; then
    log_step "清理构建目录..."
    if [[ -d "$BUILD_DIR" ]]; then
        if $DRY_RUN; then
            echo "  rm -rf \"$BUILD_DIR\""
        else
            rm -rf "$BUILD_DIR"
        fi
    fi
fi

# ── CMake 配置 ───────────────────────────────────────────────────────────
log_step "CMake 配置..."

CMAKE_ARGS=(
    -DCMAKE_BUILD_TYPE="$BUILD_TYPE"
    -DLIQUID_ENABLE_VELOX=ON
    "-DVELOX_PREFIX=$VELOX_PREFIX"
    -DLIQUID_BUILD_TESTS=OFF
)

if $DRY_RUN; then
    echo "  cmake -S \"$PROJECT_ROOT\" -B \"$BUILD_DIR\" ${CMAKE_ARGS[*]}"
else
    cmake -S "$PROJECT_ROOT" -B "$BUILD_DIR" "${CMAKE_ARGS[@]}"
fi
log_ok "CMake 配置完成"

# ── 编译 ──────────────────────────────────────────────────────────────────
log_step "编译 liquid_velox_benchmark (jobs=$JOBS)..."

BUILD_START=$(date +%s)

if $DRY_RUN; then
    echo "  cmake --build \"$BUILD_DIR\" --target liquid_velox_benchmark -j $JOBS"
else
    if cmake --build "$BUILD_DIR" --target liquid_velox_benchmark -j "$JOBS"; then
        BUILD_OK=true
    else
        BUILD_OK=false
    fi
fi

BUILD_END=$(date +%s)
BUILD_ELAPSED=$(( BUILD_END - BUILD_START ))

# ── 结果验证 ─────────────────────────────────────────────────────────────
BENCHMARK_PATH="${BUILD_DIR}/liquid_velox_benchmark"
VELOX_LIB_PATH="${BUILD_DIR}/libliquid_cache_velox.a"

if $DRY_RUN; then
    echo ""
    echo "  [DRY RUN] 预期产出:"
    echo "    $BENCHMARK_PATH"
    echo "    $VELOX_LIB_PATH"
    exit 0
fi

echo ""
echo "============================================"
echo "  liquid_velox_benchmark 构建报告"
echo "============================================"
echo ""

if [[ "$BUILD_OK" == true ]] && [[ -x "$BENCHMARK_PATH" ]]; then
    BENCH_SIZE=$(du -h "$BENCHMARK_PATH" | cut -f1)
    log_ok "liquid_velox_benchmark 构建成功!"
    echo ""
    echo "  二进制位置: $BENCHMARK_PATH"
    echo "  文件大小:   $BENCH_SIZE"
    echo "  编译耗时:   ${BUILD_ELAPSED}s"
    echo "  构建类型:   $BUILD_TYPE"
    echo "  Velox 路径: $VELOX_PREFIX"

    if [[ -f "$VELOX_LIB_PATH" ]]; then
        VELOX_LIB_SIZE=$(du -h "$VELOX_LIB_PATH" | cut -f1)
        echo "  Velox 静态库: $VELOX_LIB_PATH ($VELOX_LIB_SIZE)"
    fi

    echo ""
    echo "  快速验证:"
    echo "    $BENCHMARK_PATH /path/to/test.parquet verify"
    echo "    $BENCHMARK_PATH /path/to/test.parquet bench"
elif [[ "$BUILD_OK" == true ]] && [[ ! -x "$BENCHMARK_PATH" ]]; then
    log_error "编译过程未报错，但未找到二进制文件: $BENCHMARK_PATH"
    log_error "可能原因: 编译目标名称变更，或输出路径异常"
    log_info "请检查 CMakeLists.txt 中的目标定义"
    exit 1
else
    log_error "liquid_velox_benchmark 编译失败 (耗时: ${BUILD_ELAPSED}s)"
    echo ""
    log_info "常见问题排查:"
    echo "  1. Velox 路径是否正确? (-p 指定)"
    echo "  2. Velox 是否完整构建? (libvelox.a, libfolly.a, bundled Arrow 18)"
    echo "  3. 编译错误: 检查头文件前向声明 (liquid_arrays.h)"
    echo "  4. ABI 不兼容: 确认使用 bundled Arrow 18 而非系统 Arrow 24"
    echo "  5. 链接错误: 检查 --whole-archive 包裹方式"
    exit 1
fi

exit 0
