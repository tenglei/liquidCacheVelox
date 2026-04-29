#!/bin/bash
# ============================================================================
# run_all_tests.sh — 运行全部单元测试与 Parquet 验证
# ============================================================================
# 功能：
#   - 构建所有测试目标（含 Velox 测试，若启用）
#   - 运行所有单元测试：test_roundtrip, test_velox_crossval,
#     test_linear_integer, test_float_quantize, test_cache_budget
#   - 运行 verify_parquet 验证 Parquet 文件完整性
#   - 汇总 PASS/FAIL 状态、执行时间、错误详情
#   - 生成测试报告
#
# 用法：
#   ./run_all_tests.sh [选项]
#
# 选项：
#   -p, --parquet <path>     指定 Parquet 测试文件（用于 verify_parquet）
#                             （默认: build/test_data_512mb.parquet）
#   -d, --build-dir <dir>    CMake 构建目录（默认: build）
#   -t, --build-type <type>  构建类型 Release|Debug（默认: Release）
#   -j <N>                   并行编译任务数（默认: nproc）
#   --with-velox <path>     启用 Velox 集成测试（需指定 VELOX_PREFIX）
#   --clean                  清理构建目录后重新配置
#   --no-build               跳过构建，仅运行已有测试
#   --gtest-filter <f>       传递 Google Test --gtest_filter
#   -n, --dry-run            仅显示将执行的命令
#   -h, --help               显示此帮助
#
# 示例：
#   ./scripts/run_all_tests.sh
#   ./scripts/run_all_tests.sh --with-velox /home/tenglei/code/velox/build
#   ./scripts/run_all_tests.sh --gtest-filter "LiquidCache*" -p /tmp/test.parquet
#   ./scripts/run_all_tests.sh --no-build
# ============================================================================

set -euo pipefail

# ── 默认值 ───────────────────────────────────────────────────────────────
PROJECT_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="${PROJECT_ROOT}/build"
BUILD_TYPE="Release"
JOBS=$(nproc)
DO_CLEAN=false
NO_BUILD=false
DRY_RUN=false
WITH_VELOX=false
VELOX_PREFIX=""
GTEST_FILTER="*"
PARQUET_FILE="${BUILD_DIR}/test_data_512mb.parquet"

# ── 颜色输出 ─────────────────────────────────────────────────────────────
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
CYAN='\033[0;36m'
BLUE='\033[0;34m'
MAGENTA='\033[0;35m'
BOLD='\033[1m'
NC='\033[0m'

log_info()  { echo -e "${CYAN}[INFO]${NC}  $*"; }
log_ok()    { echo -e "${GREEN}[OK]${NC}    $*"; }
log_warn()  { echo -e "${YELLOW}[WARN]${NC}  $*"; }
log_error() { echo -e "${RED}[ERROR]${NC} $*" >&2; }
log_step()  { echo -e "${BLUE}[STEP]${NC}  $*"; }

# ── 帮助 ──────────────────────────────────────────────────────────────────
show_help() {
    head -43 "$0" | tail -42
    exit 0
}

# ── 参数解析 ─────────────────────────────────────────────────────────────
while [[ $# -gt 0 ]]; do
    case "$1" in
        -p|--parquet)
            PARQUET_FILE="$2"; shift 2 ;;
        -d|--build-dir)
            BUILD_DIR="$2"; shift 2 ;;
        -t|--build-type)
            BUILD_TYPE="$2"; shift 2 ;;
        -j)
            JOBS="$2"; shift 2 ;;
        --with-velox)
            WITH_VELOX=true
            VELOX_PREFIX="$2"; shift 2 ;;
        --clean)
            DO_CLEAN=true; shift ;;
        --no-build)
            NO_BUILD=true; shift ;;
        --gtest-filter)
            GTEST_FILTER="$2"; shift 2 ;;
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

# ── 测试结果追踪 ─────────────────────────────────────────────────────────
declare -a TEST_RESULTS=()
TOTAL_TESTS=0
PASSED_TESTS=0
FAILED_TESTS=0
SKIPPED_TESTS=0
TEST_START_TIME=$(date +%s)

# ── 辅助函数：运行单个测试 ───────────────────────────────────────────────
run_test() {
    local test_name="$1"
    local test_binary="$2"
    local extra_args="${3:-}"

    TOTAL_TESTS=$(( TOTAL_TESTS + 1 ))

    echo ""
    echo -e "${BOLD}━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━${NC}"
    echo -e "${BOLD}  测试 #${TOTAL_TESTS}: ${test_name}${NC}"
    echo -e "${BOLD}━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━${NC}"
    echo -e "  二进制:  ${test_binary}"
    echo -e "  过滤器:  ${GTEST_FILTER}"
    echo ""

    if $DRY_RUN; then
        echo "  [DRY RUN] $test_binary --gtest_filter=\"$GTEST_FILTER\" $extra_args"
        TEST_RESULTS+=("SKIP|${test_name}|0.0|dry-run")
        SKIPPED_TESTS=$(( SKIPPED_TESTS + 1 ))
        return 0
    fi

    if [[ ! -x "$test_binary" ]]; then
        echo -e "  ${YELLOW}[SKIP]${NC}  二进制不存在: $test_binary"
        TEST_RESULTS+=("SKIP|${test_name}|0.0|二进制未构建")
        SKIPPED_TESTS=$(( SKIPPED_TESTS + 1 ))
        return 0
    fi

    local test_start test_end elapsed exit_code
    test_start=$(date +%s.%N)

    # 运行测试，同时捕获输出
    local tmp_output
    tmp_output=$(mktemp /tmp/liquid_test_XXXXXX)

    if ${test_binary} --gtest_filter="$GTEST_FILTER" $extra_args > "$tmp_output" 2>&1; then
        exit_code=0
    else
        exit_code=$?
    fi

    test_end=$(date +%s.%N)
    elapsed=$(echo "$test_end - $test_start" | bc 2>/dev/null || echo "0")

    # 解析 Google Test 输出
    local tests_run=0 tests_passed=0 tests_failed=0
    if grep -q '\[  PASSED  \]' "$tmp_output"; then
        tests_passed=$(grep -c '\[  PASSED  \]' "$tmp_output" || echo 0)
    fi
    if grep -q '\[  FAILED  \]' "$tmp_output"; then
        tests_failed=$(grep -c '\[  FAILED  \]' "$tmp_output" || echo 0)
    fi

    # 显示输出（限制行数）
    local output_lines
    output_lines=$(wc -l < "$tmp_output")
    if [[ $output_lines -le 80 ]]; then
        cat "$tmp_output"
    else
        head -40 "$tmp_output"
        echo -e "  ... (省略 $(( output_lines - 80 )) 行) ..."
        tail -40 "$tmp_output"
    fi

    echo ""

    if [[ $exit_code -eq 0 ]]; then
        echo -e "  ${GREEN}[PASS]${NC}  ${test_name}"
        echo -e "         通过: ${tests_passed}  失败: ${tests_failed}  耗时: ${elapsed}s"
        TEST_RESULTS+=("PASS|${test_name}|${elapsed}|")
        PASSED_TESTS=$(( PASSED_TESTS + 1 ))
    else
        echo -e "  ${RED}[FAIL]${NC}  ${test_name} (exit code: $exit_code)"
        echo -e "         通过: ${tests_passed}  失败: ${tests_failed}  耗时: ${elapsed}s"
        TEST_RESULTS+=("FAIL|${test_name}|${elapsed}|exit_code=${exit_code}, failed=${tests_failed}")

        # 提取失败详情
        if grep -q 'FAILED TEST\|Failure\|EXPECT_\|ASSERT_\|error:\|Error' "$tmp_output"; then
            echo ""
            echo -e "  ${RED}失败详情:${NC}"
            grep -A 5 'FAILED TEST\|Failure\|EXPECT_\|ASSERT_\|error:\|Error' "$tmp_output" | head -30 | while read -r line; do
                echo -e "    ${RED}$line${NC}"
            done
        fi
        FAILED_TESTS=$(( FAILED_TESTS + 1 ))
    fi

    rm -f "$tmp_output"
}

# ── 运行 Parquet 验证 ────────────────────────────────────────────────────
run_parquet_verify() {
    local verify_binary="${BUILD_DIR}/verify_parquet"
    local parquet_file="$1"

    TOTAL_TESTS=$(( TOTAL_TESTS + 1 ))

    echo ""
    echo -e "${BOLD}━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━${NC}"
    echo -e "${BOLD}  测试 #${TOTAL_TESTS}: Parquet 文件验证${NC}"
    echo -e "${BOLD}━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━${NC}"
    echo -e "  文件:    ${parquet_file}"
    echo ""

    if $DRY_RUN; then
        echo "  [DRY RUN] $verify_binary \"$parquet_file\""
        TEST_RESULTS+=("SKIP|Parquet验证|0.0|dry-run")
        SKIPPED_TESTS=$(( SKIPPED_TESTS + 1 ))
        return 0
    fi

    if [[ ! -x "$verify_binary" ]]; then
        echo -e "  ${YELLOW}[SKIP]${NC}  verify_parquet 未构建"
        TEST_RESULTS+=("SKIP|Parquet验证|0.0|verify_parquet未构建")
        SKIPPED_TESTS=$(( SKIPPED_TESTS + 1 ))
        return 0
    fi

    if [[ ! -f "$parquet_file" ]]; then
        echo -e "  ${YELLOW}[SKIP]${NC}  Parquet 文件不存在: $parquet_file"
        echo "  请先运行 generate_test_parquet 生成测试数据:"
        echo "    ./scripts/generate_test_parquet.sh"
        TEST_RESULTS+=("SKIP|Parquet验证|0.0|文件不存在: ${parquet_file}")
        SKIPPED_TESTS=$(( SKIPPED_TESTS + 1 ))
        return 0
    fi

    local test_start test_end elapsed
    test_start=$(date +%s.%N)

    local tmp_output
    tmp_output=$(mktemp /tmp/liquid_test_XXXXXX)

    if "$verify_binary" "$parquet_file" > "$tmp_output" 2>&1; then
        exit_code=0
    else
        exit_code=$?
    fi

    test_end=$(date +%s.%N)
    elapsed=$(echo "$test_end - $test_start" | bc 2>/dev/null || echo "0")

    cat "$tmp_output"
    echo ""

    if [[ $exit_code -eq 0 ]]; then
        # 从输出提取统计信息
        local rows cols rgs
        rows=$(grep -oP 'Rows:\s*\K[0-9]+' "$tmp_output" || echo "?")
        cols=$(grep -oP 'Cols:\s*\K[0-9]+' "$tmp_output" || echo "?")
        rgs=$(grep -oP 'Row Groups:\s*\K[0-9]+' "$tmp_output" || echo "?")
        local file_size
        file_size=$(du -h "$parquet_file" 2>/dev/null | cut -f1 || echo "?")

        echo -e "  ${GREEN}[PASS]${NC}  Parquet 文件验证"
        echo -e "         ${rows} 行, ${cols} 列, ${rgs} Row Groups, ${file_size}"
        echo -e "         耗时: ${elapsed}s"
        TEST_RESULTS+=("PASS|Parquet验证(${rows}行,${cols}列)|${elapsed}|")
        PASSED_TESTS=$(( PASSED_TESTS + 1 ))
    else
        echo -e "  ${RED}[FAIL]${NC}  Parquet 文件验证 (exit code: $exit_code)"
        echo -e "         耗时: ${elapsed}s"
        TEST_RESULTS+=("FAIL|Parquet验证|${elapsed}|exit_code=${exit_code}")
        FAILED_TESTS=$(( FAILED_TESTS + 1 ))
    fi

    rm -f "$tmp_output"
}

# ══════════════════════════════════════════════════════════════════════════
# 主流程
# ══════════════════════════════════════════════════════════════════════════

echo ""
echo -e "${BOLD}${BLUE}╔══════════════════════════════════════════════════════════════╗${NC}"
echo -e "${BOLD}${BLUE}║       Liquid Cache C++ — 完整测试套件                      ║${NC}"
echo -e "${BOLD}${BLUE}╚══════════════════════════════════════════════════════════════╝${NC}"
echo ""
echo -e "  项目根目录:  ${PROJECT_ROOT}"
echo -e "  构建目录:    ${BUILD_DIR}"
echo -e "  构建类型:    ${BUILD_TYPE}"
echo -e "  并行任务:    ${JOBS}"
echo -e "  Velox 集成:  $($WITH_VELOX && echo "是 (${VELOX_PREFIX})" || echo "否")"
echo -e "  GTEST 过滤:  ${GTEST_FILTER}"
echo -e "  Parquet 文件: ${PARQUET_FILE}"
echo ""

# ── 清理构建目录 ─────────────────────────────────────────────────────────
if $DO_CLEAN; then
    log_step "清理构建目录..."
    if $DRY_RUN; then
        echo "  rm -rf \"$BUILD_DIR\""
    else
        rm -rf "$BUILD_DIR"
    fi
fi

# ── CMake 配置与构建 ─────────────────────────────────────────────────────
if ! $NO_BUILD; then
    log_step "CMake 配置..."

    CMAKE_ARGS=(
        -DCMAKE_BUILD_TYPE="$BUILD_TYPE"
        -DLIQUID_BUILD_TESTS=ON
    )

    if $WITH_VELOX; then
        CMAKE_ARGS+=(
            -DLIQUID_ENABLE_VELOX=ON
            "-DVELOX_PREFIX=$VELOX_PREFIX"
        )
    fi

    if $DRY_RUN; then
        echo "  cmake -S \"$PROJECT_ROOT\" -B \"$BUILD_DIR\" ${CMAKE_ARGS[*]}"
    else
        cmake -S "$PROJECT_ROOT" -B "$BUILD_DIR" "${CMAKE_ARGS[@]}"
    fi
    log_ok "CMake 配置完成"

    log_step "编译所有目标 (jobs=$JOBS)..."
    BUILD_START=$(date +%s)
    if $DRY_RUN; then
        echo "  cmake --build \"$BUILD_DIR\" -j $JOBS"
    else
        cmake --build "$BUILD_DIR" -j "$JOBS"
    fi
    BUILD_END=$(date +%s)
    BUILD_ELAPSED=$(( BUILD_END - BUILD_START ))
    log_ok "编译完成 (耗时: ${BUILD_ELAPSED}s)"
else
    log_info "跳过构建 (--no-build)"
fi

echo ""

# ── 运行所有测试 ─────────────────────────────────────────────────────────
log_step "开始运行测试套件..."

# 1. test_roundtrip — 核心编解码往返测试
run_test "核心往返测试 (test_roundtrip)" \
    "${BUILD_DIR}/liquid_cache_tests" \
    ""

# 2. test_velox_crossval — Velox 交叉验证（仅 Velox 模式）
if $WITH_VELOX; then
    run_test "Velox 交叉验证 (test_velox_crossval)" \
        "${BUILD_DIR}/liquid_velox_tests" \
        ""
else
    echo ""
    echo -e "${BOLD}━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━${NC}"
    echo -e "${BOLD}  测试 #$(( TOTAL_TESTS + 1 )): Velox 交叉验证 (test_velox_crossval)${NC}"
    echo -e "${BOLD}━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━${NC}"
    TOTAL_TESTS=$(( TOTAL_TESTS + 1 ))
    echo -e "  ${YELLOW}[SKIP]${NC}  未启用 Velox (使用 --with-velox <path> 启用)"
    TEST_RESULTS+=("SKIP|Velox交叉验证|0.0|未启用Velox")
    SKIPPED_TESTS=$(( SKIPPED_TESTS + 1 ))
fi

# 3. test_linear_integer — LinearInteger 测试
run_test "LinearInteger 测试 (test_linear_integer)" \
    "${BUILD_DIR}/liquid_linear_test" \
    ""

# 4. test_float_quantize — 浮点量化测试
run_test "浮点量化测试 (test_float_quantize)" \
    "${BUILD_DIR}/liquid_float_quantize_test" \
    ""

# 5. test_cache_budget — 缓存预算和 LRU 测试
run_test "缓存预算/LRU 测试 (test_cache_budget)" \
    "${BUILD_DIR}/liquid_cache_budget_test" \
    ""

# 6. verify_parquet — Parquet 文件完整性验证
run_parquet_verify "$PARQUET_FILE"

# ── 测试汇总报告 ─────────────────────────────────────────────────────────
TEST_END_TIME=$(date +%s)
TOTAL_ELAPSED=$(( TEST_END_TIME - TEST_START_TIME ))

echo ""
echo ""
echo -e "${BOLD}${BLUE}╔══════════════════════════════════════════════════════════════╗${NC}"
echo -e "${BOLD}${BLUE}║                    测试汇总报告                             ║${NC}"
echo -e "${BOLD}${BLUE}╚══════════════════════════════════════════════════════════════╝${NC}"
echo ""
echo -e "  总计测试套件:  ${TOTAL_TESTS}"
echo -e "  ${GREEN}通过:          ${PASSED_TESTS}${NC}"
echo -e "  ${RED}失败:          ${FAILED_TESTS}${NC}"
echo -e "  ${YELLOW}跳过:          ${SKIPPED_TESTS}${NC}"
echo -e "  总耗时:        ${TOTAL_ELAPSED}s"
echo ""

# 详细结果表
printf "  %-4s %-35s %-10s %s\n" "序号" "测试名称" "状态" "耗时/详情"
printf "  %-4s %-35s %-10s %s\n" "----" "-----------------------------------" "----------" "--------------------"

idx=0
for result in "${TEST_RESULTS[@]}"; do
    idx=$(( idx + 1 ))
    IFS='|' read -r status name elapsed detail <<< "$result"

    status_color=""
    case "$status" in
        PASS) status_color="${GREEN}PASS${NC}" ;;
        FAIL) status_color="${RED}FAIL${NC}" ;;
        SKIP) status_color="${YELLOW}SKIP${NC}" ;;
        *)    status_color="$status" ;;
    esac

    info="${elapsed}s"
    if [[ -n "$detail" ]]; then
        info="$detail"
    fi

    printf "  %-4s %-35s %b          %s\n" \
        "$idx" \
        "$(echo "$name" | cut -c1-35)" \
        "$status_color" \
        "$info"
done

echo ""

if [[ $FAILED_TESTS -eq 0 ]]; then
    echo -e "  ${GREEN}${BOLD}✓ 所有测试通过!${NC}"
    exit_code=0
else
    echo -e "  ${RED}${BOLD}✗ ${FAILED_TESTS} 个测试失败${NC}"
    exit_code=1
fi

echo ""

exit $exit_code
