#!/bin/sh
# coverage.sh —— 覆盖率门禁（v0.4 G2）
#
# 跑核心单元/定向测试，产出 core/src 的行覆盖率与分支覆盖率，低于阈值则失败。
# 用法： ./tools/coverage.sh [行阈值] [分支阈值]     默认 86 55
#
# 阈值的取法：**防回归**（略低于当前实测值），不是「跳到目标」。
# 当前实测：行 86.6% / 分支 55.6%（2026-11-11，含 conformance/重入/有损/客户端）。
# v0.5 的目标是行 85 / 分支 75（行目标已达成，分支差距见 roadmap V3）
# （见 docs/roadmap-v0.5.md V3），达标后再把阈值提上去。
#
# 为什么只统计单元/定向测试、不统计差分：差分跑的是 mock 可执行文件，
# 其覆盖率反映的是 mock 的分派逻辑而非 core 的分支；单元测试才是分支覆盖主力。
set -e

LINE_MIN=${1:-86}
BRANCH_MIN=${2:-55}
OUT=build/cov
mkdir -p "$OUT"

LLVM_COV=$(command -v llvm-cov || xcrun --find llvm-cov 2>/dev/null || true)
LLVM_PROFDATA=$(command -v llvm-profdata || \
  xcrun --find llvm-profdata 2>/dev/null || true)
if [ -z "$LLVM_COV" ] || [ -z "$LLVM_PROFDATA" ]; then
    echo "SKIP: 未找到 llvm-cov / llvm-profdata（需要 LLVM 工具链）"
    exit 0
fi

CC=${CC:-cc}
COVFLAGS="-fprofile-instr-generate -fcoverage-mapping"
CORE="core/src/frame.c core/src/session.c core/src/json.c core/src/writer.c \
      core/src/mcp_server.c core/src/mcp_client.c core/src/err.c core/src/trace.c"
INC="-Icore/include"

echo "== 构建带覆盖率的核心与测试 =="
# 核心单测
$CC -O0 -g -std=c99 $COVFLAGS $INC \
    tests/test_m2.c $CORE -o "$OUT/t_m2"
$CC -O0 -g -std=c99 $COVFLAGS $INC \
    tests/test_reliability.c core/src/frame.c core/src/session.c core/src/trace.c \
    -o "$OUT/t_rel"
$CC -O0 -g -std=c99 $COVFLAGS $INC \
    tests/test_lossy.c core/src/frame.c core/src/session.c core/src/trace.c \
    -o "$OUT/t_lossy"
$CC -O0 -g -std=c99 $COVFLAGS $INC \
    tests/test_client.c $CORE -o "$OUT/t_client"
$CC -O0 -g -std=c99 $COVFLAGS $INC \
    tests/test_reentrancy.c $CORE -o "$OUT/t_reent"
$CC -O0 -g -std=c99 $COVFLAGS -DBT_MCP_CFG_FCS16=1 $INC \
    tests/test_fcs16.c core/src/frame.c core/src/trace.c -o "$OUT/t_fcs"
$CC -O0 -g -std=c99 $COVFLAGS $INC \
    tests/test_conformance.c $CORE -o "$OUT/t_conf"
$CC -O0 -g -std=c99 $COVFLAGS $INC \
    tests/test_threads.c $CORE -o "$OUT/t_thr" -lpthread
$CC -O0 -g -std=c99 $COVFLAGS $INC \
    tests/test_multisession.c $CORE -o "$OUT/t_multi"

echo "== 运行 =="
rm -f "$OUT"/*.profraw
for t in t_m2 t_rel t_lossy t_client t_reent t_fcs t_conf t_thr t_multi; do
    LLVM_PROFILE_FILE="$OUT/$t.profraw" "$OUT/$t" > "$OUT/$t.log" 2>&1 || {
        echo "FAIL: $t"; tail -5 "$OUT/$t.log"; exit 1; }
done

echo "== 汇总 =="
"$LLVM_PROFDATA" merge -sparse "$OUT"/*.profraw -o "$OUT/all.profdata"
OBJS=""
for t in t_m2 t_rel t_lossy t_client t_reent t_fcs t_conf t_thr t_multi; do
    OBJS="$OBJS -object=$OUT/$t"
done
"$LLVM_COV" report $OBJS -instr-profile="$OUT/all.profdata" \
    > "$OUT/report_all.txt" 2>"$OUT/report_err.txt" || {
        echo "FAIL: llvm-cov report 失败"; head -3 "$OUT/report_err.txt"; exit 1; }
# 只保留 core/src 与 TOTAL（测试文件自身的覆盖率不是门禁目标）
{ grep -E '^core/src/' "$OUT/report_all.txt"; grep -E '^TOTAL' "$OUT/report_all.txt"; } \
    | tee "$OUT/report.txt"

# TOTAL 行形如：
# TOTAL  4948 1744 64.75%  213 39 81.69%  3443 753 78.13%  2878 1470 48.92%
# 取「行覆盖率」与「分支覆盖率」两列（各自的 Cover 字段）
LINE=$(awk '/^TOTAL/ {
    n = split($0, a, "%");
    # a[n-1] 是分支 Cover 前的片段，a[n-2] 是行 Cover 前的片段
    split(a[n-2], b, " "); printf "%s", b[length(b)];
}' "$OUT/report.txt")
BRANCH=$(awk '/^TOTAL/ {
    n = split($0, a, "%");
    split(a[n-1], b, " "); printf "%s", b[length(b)];
}' "$OUT/report.txt")

echo
echo "行覆盖率 ${LINE}%（阈值 ${LINE_MIN}%）  分支覆盖率 ${BRANCH}%（阈值 ${BRANCH_MIN}%）"

awk -v l="$LINE" -v lm="$LINE_MIN" -v b="$BRANCH" -v bm="$BRANCH_MIN" 'BEGIN {
    if (l + 0 < lm + 0) { printf "FAIL: 行覆盖率 %.1f%% < %d%%\n", l, lm; exit 1 }
    if (b + 0 < bm + 0) { printf "FAIL: 分支覆盖率 %.1f%% < %d%%\n", b, bm; exit 1 }
    print "COVERAGE OK"
}'
