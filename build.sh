#!/bin/sh
# build.sh —— 构建并运行全部验证套件
#
# 用法：
#   ./build.sh                 # 默认轮数（差分 200 轮）
#   ./build.sh 500             # 指定随机轮数（发布前建议 ≥300）
#   ./build.sh 200 asan        # 追加 ASan/UBSan 版差分（较慢，CI 用）
#
# 环境变量：
#   CC=clang                   # 编译器
#   BT_MCP_TSAN=1              # 追加 TSan 版并发/压测
#
# 结构（按验证目的分组，而不是按加入时间）：
#   §1 构建            C mock / Rust oracle
#   §2 核心协议         L3 分帧 → L4 会话 → L5 语义
#   §3 可靠性          丢片/重传/去重/有损通道/流控
#   §4 规范符合性      结构符合性 / golden / 官方 SDK / 错误路径
#   §5 并发            多会话隔离 / 多线程压测
#   §6 差分           主矩阵 / 配置变体（FCS16、共享重组）/ 正交矩阵
#   §7 fuzz            帧层字节 / L5 结构
#   §8 门禁            覆盖率 / RAM 预算 / 文档一致性
#   §9 基线            内存档位 / 开销
set -e
cd "$(dirname "$0")"

ROUNDS="${1:-200}"
MODE="${2:-plain}"
CC="${CC:-cc}"

# 公共编译参数与源文件集（避免每处重复一长串路径）
CFLAGS="-O2 -std=c99 -Wall -Wextra -Werror -Icore/include"
CORE="core/src/frame.c core/src/session.c core/src/json.c core/src/writer.c \
      core/src/mcp_server.c core/src/mcp_client.c core/src/err.c core/src/trace.c"
CORE_SESS="core/src/frame.c core/src/session.c core/src/trace.c"
CORE_FRAME="core/src/frame.c core/src/trace.c"

mkdir -p build

# ───────────────────────────── §1 构建 ─────────────────────────────
echo "== §1 构建：C mock =="
$CC $CFLAGS mock/mock_main.c $CORE -o build/btmcp_mock

echo "== §1 构建：Rust oracle =="
(cd oracle && cargo build --release --quiet)

# ─────────────────────────── §2 核心协议 ───────────────────────────
echo "== §2 port HIL（GATT/SPP/CoC 背靠背，core 零改动） =="
$CC $CFLAGS -Iport tests/test_m4c_port.c $CORE_SESS -o build/test_m4c_port
./build/test_m4c_port

echo "== §2 单元（L2~L5） =="
$CC $CFLAGS tests/test_m2.c $CORE -o build/test_m2
./build/test_m2

echo "== §2 Bridge 客户端（client L5 ↔ server L5） =="
$CC $CFLAGS tests/test_client.c $CORE -o build/test_client
./build/test_client

echo "== §2 重入与多实例不变量 =="
$CC $CFLAGS tests/test_reentrancy.c $CORE -o build/test_reentrancy
./build/test_reentrancy

echo "== §2 JSON 解析语料（含严格/宽松两个入口的差异） =="
$CC $CFLAGS tests/test_json_corpus.c core/src/json.c -o build/test_json_corpus
./build/test_json_corpus

# ───────────────────────────── §3 可靠性 ─────────────────────────────
echo "== §3 可靠性定向（丢片/重传/去重/槽回收/退避） =="
$CC $CFLAGS tests/test_reliability.c $CORE_SESS -o build/test_reliability
./build/test_reliability

echo "== §3 有损通道收敛（丢片/重复/乱序） =="
$CC $CFLAGS tests/test_lossy.c $CORE_SESS -o build/test_lossy
./build/test_lossy

echo "== §3 链路仿真 soak（阶梯加压） =="
$CC $CFLAGS tests/test_soak.c $CORE_SESS -o build/test_soak -lpthread
./build/test_soak

echo "== §3 FCS16 往返（通道无链路校验时才开） =="
$CC $CFLAGS -DBT_MCP_CFG_FCS16=1 tests/test_fcs16.c $CORE_FRAME \
   -o build/test_fcs16
./build/test_fcs16

echo "== §3 共享重组缓冲（两种配置） =="
$CC $CFLAGS tests/test_shared_reasm.c $CORE_FRAME -o build/test_shared_reasm
./build/test_shared_reasm
$CC $CFLAGS -DBT_MCP_CFG_SHARED_REASM=1 tests/test_shared_reasm.c $CORE_FRAME \
   -o build/test_shared_reasm1
./build/test_shared_reasm1

# ────────────────────────── §4 规范符合性 ──────────────────────────
echo "== §4 结构符合性 =="
$CC $CFLAGS tests/test_conformance.c $CORE -o build/test_conformance
./build/test_conformance

echo "== §4 golden transcript 回归 =="
python3 tests/golden_replay.py

echo "== §4 官方 MCP SDK 互验 =="
if [ -d tests/sdk_interop/node_modules/@modelcontextprotocol/sdk ]; then
    ( cd tests/sdk_interop && node sdk_interop.mjs ../../build/btmcp_mock )
else
    echo "  SKIP: 未安装 SDK（cd tests/sdk_interop && npm install）"
fi

echo "== §4 L5 方法级错误路径 =="
$CC $CFLAGS tests/test_l5_edges.c $CORE -o build/test_l5_edges
./build/test_l5_edges

echo "== §4 通用边界与错误路径 =="
$CC $CFLAGS tests/test_edges.c $CORE -o build/test_edges
./build/test_edges

echo "== §4 故障注入（错误传播链） =="
$CC $CFLAGS tests/test_fault.c $CORE -o build/test_fault
./build/test_fault

echo "== §4 条件级分支覆盖补测（v0.7 A3） =="
$CC $CFLAGS tests/test_branches.c $CORE -o build/test_branches
./build/test_branches

echo "== §4 组合矩阵（表驱动，自动生成用例） =="
$CC $CFLAGS tests/test_matrix.c $CORE -o build/test_matrix
./build/test_matrix
echo "-- 受限资源配置（强制走容量不足分支） --"
$CC $CFLAGS -DBT_MCP_CFG_MAX_MSG=128 -DBT_MCP_CFG_PAGE_SIZE=1 \
   -DBT_MCP_CFG_MAX_PENDING=1 tests/test_matrix.c $CORE \
   -o build/test_matrix_small
./build/test_matrix_small

# ────────────────────────────── §5 并发 ──────────────────────────────
echo "== §5 多会话隔离（pool 按 session 分派） =="
$CC $CFLAGS tests/test_multisession.c $CORE -o build/test_multisession
./build/test_multisession

echo "== §5 多会话 × 多线程压测（4×4 + 断链重连） =="
$CC $CFLAGS tests/test_stress.c $CORE -o build/test_stress -lpthread
./build/test_stress
if [ "${BT_MCP_TSAN:-0}" = "1" ]; then
    echo "-- TSan 版 --"
    $CC -O1 -g -std=c99 -fsanitize=thread -Icore/include \
       tests/test_stress.c $CORE -o build/test_stress_tsan -lpthread
    ./build/test_stress_tsan
fi

# ────────────────────────────── §6 差分 ──────────────────────────────
echo "== §6 主差分矩阵（m1/m3/m4/m4b） =="
python3 tests/diff_harness.py "$ROUNDS"
python3 tests/m3_session_harness.py "$ROUNDS"
python3 tests/m4_mcp_harness.py "$ROUNDS"
python3 tests/m4b_e2e_harness.py "$ROUNDS"

echo "== §6 配置变体：FCS16 配对差分 =="
$CC $CFLAGS -DBT_MCP_CFG_FCS16=1 mock/mock_main.c $CORE \
   -o build/btmcp_mock_fcs
BT_MCP_MOCK=build/btmcp_mock_fcs BT_MCP_FCS16=1 \
    python3 tests/diff_harness.py 40
BT_MCP_MOCK=build/btmcp_mock_fcs BT_MCP_FCS16=1 \
    python3 tests/m3_session_harness.py 40
BT_MCP_MOCK=build/btmcp_mock_fcs BT_MCP_FCS16=1 \
    python3 tests/m4_mcp_harness.py 40

echo "== §6 配置变体：共享重组缓冲 =="
$CC $CFLAGS -DBT_MCP_CFG_SHARED_REASM=1 mock/mock_main.c $CORE \
   -o build/btmcp_mock_shared
$CC $CFLAGS -DBT_MCP_CFG_SHARED_REASM=1 -DBT_MCP_CFG_FCS16=1 \
   mock/mock_main.c $CORE -o build/btmcp_mock_shared_fcs
BT_MCP_MOCK=build/btmcp_mock_shared BT_MCP_MAX_REASM=1 \
    python3 tests/m3_session_harness.py 40

echo "== §6 正交差分矩阵（配置签名 + 笛卡尔积） =="
python3 tests/matrix_runner.py --rounds 30

# ────────────────────────────── §7 fuzz ──────────────────────────────
echo "== §7 帧层 fuzz（字节变异） =="
$CC -O2 -std=c99 -Icore/include -DBT_MCP_FUZZ_MAIN \
   fuzz/fuzz_frame.c $CORE_FRAME -o build/fuzz_frame
./build/fuzz_frame 2000000
$CC -O2 -std=c99 -Icore/include -DBT_MCP_FUZZ_MAIN -DBT_MCP_CFG_FCS16=1 \
   fuzz/fuzz_frame.c $CORE_FRAME -o build/fuzz_frame_fcs
./build/fuzz_frame_fcs 800000

echo "== §7 L5 fuzz（结构变异，四条不变量） =="
$CC $CFLAGS fuzz/fuzz_l5.c $CORE -o build/fuzz_l5
./build/fuzz_l5 300000

# ────────────────────────────── §8 门禁 ──────────────────────────────
echo "== §8 文档与代码一致性 =="
python3 tools/check_docs.py

echo "== §8 参考移植实现的 core API 校验 =="
python3 tools/check_example_ports.py

echo "== §8 RAM 预算门禁（12 组合） =="
python3 tools/ram_baseline.py

# ────────────────────────────── §9 基线 ──────────────────────────────
echo "== §9 内存档位构建（COMPACT/STANDARD 各编一份） =="
for tier in COMPACT STANDARD; do
    $CC $CFLAGS -DBT_MCP_TIER=BT_MCP_TIER_$tier -c core/src/frame.c \
       -o build/frame_$tier.o
    $CC $CFLAGS -DBT_MCP_TIER=BT_MCP_TIER_$tier -c core/src/session.c \
       -o build/session_$tier.o
    echo "  BT_MCP_TIER_$tier OK"
done

echo "== §9 开销基线（参考值，不影响判定） =="
$CC -O2 -std=c99 -Icore/include tools/bench.c $CORE_SESS -o build/bench
./build/bench | head -8

# ───────────────────────────── ASan 追加 ─────────────────────────────
if [ "$MODE" = "asan" ]; then
    echo "== ASan/UBSan 版差分 =="
    $CC -O1 -g -std=c99 -fsanitize=address,undefined -Icore/include \
       mock/mock_main.c $CORE -o build/btmcp_mock_asan
    BT_MCP_MOCK=build/btmcp_mock_asan python3 tests/diff_harness.py "$ROUNDS"
    BT_MCP_MOCK=build/btmcp_mock_asan python3 tests/m3_session_harness.py "$ROUNDS"
    BT_MCP_MOCK=build/btmcp_mock_asan python3 tests/m4_mcp_harness.py "$ROUNDS"
fi

echo
echo "ALL SUITES GREEN"
