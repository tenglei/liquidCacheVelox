#!/bin/bash
# ============================================================================
# build_jni_library.sh — 构建 libliquid_cache_jni.so JNI 共享库
# ============================================================================
# 功能：
#   - CMake 配置与编译 liquid_cache_jni 目标
#   - 处理 JNI 所需的动态库链接（PIC 兼容性）
#   - 验证 .so 符号与依赖
#   - 构建状态报告
#
# 用法：
#   ./build_jni_library.sh [选项]
#
# 选项：
#   -d, --build-dir <dir>    CMake 构建目录（默认: build）
#   -t, --build-type <type>  构建类型 Release|Debug（默认: Release）
#   -j <N>                   并行编译任务数（默认: nproc）
#   --clean                  清理构建目录后重新配置
#   --with-velox <path>     同时启用 Velox 集成（需指定 VELOX_PREFIX）
#   -n, --dry-run            仅显示将执行的命令
#   -h, --help               显示此帮助
#
# 示例：
#   ./scripts/build_jni_library.sh
#   ./scripts/build_jni_library.sh -t Debug -j 4
#   ./scripts/build_jni_library.sh --clean
# ============================================================================

set -euo pipefail

# ── 默认值 ───────────────────────────────────────────────────────────────
PROJECT_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="${PROJECT_ROOT}/build"
BUILD_TYPE="Release"
JOBS=$(nproc)
DO_CLEAN=false
DRY_RUN=false
WITH_VELOX=false
VELOX_PREFIX=""

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
        -d|--build-dir)
            BUILD_DIR="$2"; shift 2 ;;
        -t|--build-type)
            BUILD_TYPE="$2"; shift 2 ;;
        -j)
            JOBS="$2"; shift 2 ;;
        --clean)
            DO_CLEAN=true; shift ;;
        --with-velox)
            WITH_VELOX=true
            VELOX_PREFIX="$2"; shift 2 ;;
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
    exit 1
fi

if [[ ! "$JOBS" =~ ^[0-9]+$ ]] || [[ "$JOBS" -eq 0 ]]; then
    log_error "-j 必须为正整数"
    exit 1
fi

# ── 检查 JNI 头文件 ─────────────────────────────────────────────────────
check_jni_headers() {
    # 检查常见 JNI 头文件路径
    local jni_paths=(
        "/usr/lib/jvm/java-17-openjdk-amd64/include/jni.h"
        "/usr/lib/jvm/java-11-openjdk-amd64/include/jni.h"
        "/usr/lib/jvm/default-java/include/jni.h"
        "/usr/include/jni.h"
    )

    for jp in "${jni_paths[@]}"; do
        if [[ -f "$jp" ]]; then
            log_info "找到 JNI 头文件: $jp"
            return 0
        fi
    done

    log_warn "未检测到 JNI 头文件，CMake 配置可能失败"
    log_warn "请安装 openjdk: sudo apt install openjdk-17-jdk"
    return 1
}

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
)

if $WITH_VELOX; then
    log_info "启用 Velox 集成 (VELOX_PREFIX=$VELOX_PREFIX)"
    CMAKE_ARGS+=(
        -DLIQUID_ENABLE_VELOX=ON
        "-DVELOX_PREFIX=$VELOX_PREFIX"
    )
else
    # 检测 CMake 缓存中是否已有 Velox 配置
    if [[ -f "${BUILD_DIR}/CMakeCache.txt" ]] && grep -q 'LIQUID_ENABLE_VELOX:BOOL=ON' "${BUILD_DIR}/CMakeCache.txt" 2>/dev/null; then
        log_warn "检测到 CMake 缓存中 Velox 已启用"
        log_warn "JNI 共享库基于系统 Arrow 24 API，与 Velox bundled Arrow 18 不兼容"
        log_warn "自动禁用 Velox (传递 -DLIQUID_ENABLE_VELOX=OFF)"
        log_info "如需同时启用 JNI + Velox，请修改 jni_bridge.cpp 适配 Arrow 18 API"
    fi
    CMAKE_ARGS+=(-DLIQUID_ENABLE_VELOX=OFF)
fi

if $DRY_RUN; then
    echo "  cmake -S \"$PROJECT_ROOT\" -B \"$BUILD_DIR\" ${CMAKE_ARGS[*]}"
else
    cmake -S "$PROJECT_ROOT" -B "$BUILD_DIR" "${CMAKE_ARGS[@]}"
fi
log_ok "CMake 配置完成"

# ── 编译 ──────────────────────────────────────────────────────────────────
log_step "编译 JNI 共享库 (target: liquid_cache_jni, jobs=$JOBS)..."

BUILD_START=$(date +%s)

if $DRY_RUN; then
    echo "  cmake --build \"$BUILD_DIR\" --target liquid_cache_jni -j $JOBS"
else
    if cmake --build "$BUILD_DIR" --target liquid_cache_jni -j "$JOBS" 2>&1; then
        BUILD_OK=true
    else
        BUILD_OK=false
    fi
fi

BUILD_END=$(date +%s)
BUILD_ELAPSED=$(( BUILD_END - BUILD_START ))

# ── 结果验证 ─────────────────────────────────────────────────────────────
JNI_SO_PATH="${BUILD_DIR}/libliquid_cache_jni.so"

if $DRY_RUN; then
    echo ""
    echo "  [DRY RUN] 预期产出: $JNI_SO_PATH"
    exit 0
fi

echo ""
echo "============================================"
echo "  libliquid_cache_jni.so 构建报告"
echo "============================================"
echo ""

if [[ "$BUILD_OK" == true ]] && [[ -f "$JNI_SO_PATH" ]]; then
    SO_SIZE=$(du -h "$JNI_SO_PATH" | cut -f1)
    log_ok "libliquid_cache_jni.so 构建成功!"
    echo ""
    echo "  文件位置:   $JNI_SO_PATH"
    echo "  文件大小:   $SO_SIZE"
    echo "  编译耗时:   ${BUILD_ELAPSED}s"
    echo "  构建类型:   $BUILD_TYPE"

    # 检查动态库依赖
    echo ""
    log_info "动态库依赖分析:"

    if command -v ldd &>/dev/null; then
        echo "  ---- ldd 输出 (前 30 行) ----"
        ldd "$JNI_SO_PATH" 2>/dev/null | head -30 | while read -r line; do
            echo "  $line"
        done
    elif command -v readelf &>/dev/null; then
        echo "  ---- NEEDED 库 (readelf) ----"
        readelf -d "$JNI_SO_PATH" 2>/dev/null | grep NEEDED | while read -r line; do
            echo "  $line"
        done
    else
        log_warn "无法分析依赖 (ldd/readelf 不可用)"
    fi

    # 检查 JNI 符号
    echo ""
    log_info "JNI 符号检查:"
    if command -v nm &>/dev/null; then
        JNI_SYMBOLS=$(nm -D "$JNI_SO_PATH" 2>/dev/null | grep -c 'Java_' || echo 0)
        if [[ "$JNI_SYMBOLS" -gt 0 ]]; then
            log_ok "找到 $JNI_SYMBOLS 个 JNI 导出符号"
            nm -D "$JNI_SO_PATH" 2>/dev/null | grep 'Java_' | head -20 | while read -r line; do
                echo "  $line"
            done
        else
            log_warn "未找到 JNI 导出符号 (Java_ 前缀)"
        fi
    fi

    echo ""
    echo "  JVM 加载方式:"
    echo "    System.load(\"$JNI_SO_PATH\");"
    echo ""
    echo "  或复制到 java.library.path:"
    echo "    java -Djava.library.path=$BUILD_DIR ..."

elif [[ "$BUILD_OK" == true ]] && [[ ! -f "$JNI_SO_PATH" ]]; then
    log_error "编译过程未报错，但未找到 .so 文件: $JNI_SO_PATH"
    exit 1
else
    log_error "JNI 共享库编译失败 (耗时: ${BUILD_ELAPSED}s)"
    echo ""
    log_info "常见问题排查:"
    echo "  1. JNI 头文件缺失: sudo apt install openjdk-17-jdk"
    echo "  2. 静态库 PIC 问题: 非标准依赖已自动回退到动态 .so"
    echo "  3. Arrow 符号缺失: 检查 ARROW_STATIC_LIB 路径"
    exit 1
fi

exit 0
