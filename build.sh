#!/bin/sh
# build.sh —— 构建 C mock 与 Rust oracle，跑全部差分 + 单元/HIL 测试
#
# 用法：
#   ./build.sh              # 默认轮数（200/300/300/300）
#   ./build.sh 500          # 指定随机轮数
#   ./build.sh 200 asan     # 额外跑 ASan/UBSan 版差分（CI 用，较慢）
set -e
cd "$(dirname "$0")"
ROUNDS="${1:-200}"
MODE="${2:-plain}"
CC="${CC:-cc}"

echo "== build C mock =="
mkdir -p build
$CC -O2 -std=c99 -Wall -Wextra -Werror \
   -Icore/include mock/mock_main.c core/src/frame.c core/src/session.c \
   core/src/json.c core/src/writer.c core/src/mcp_server.c \
   core/src/err.c core/src/trace.c \
   -o build/btmcp_mock

echo "== build Rust oracle =="
(cd oracle && cargo build --release --quiet)

echo "== port HIL（GATT/SPP/CoC 背靠背，core 零改动） =="
$CC -O2 -std=c99 -Wall -Wextra -Werror -Icore/include -Iport \
   tests/test_m4c_port.c core/src/frame.c core/src/session.c \
   core/src/trace.c -o build/test_m4c_port
./build/test_m4c_port

echo "== 单元测试（L2~L5 + v0.3 新特性） =="
$CC -O2 -std=c99 -Wall -Wextra -Werror -Icore/include \
   tests/test_m2.c core/src/frame.c core/src/session.c core/src/json.c \
   core/src/writer.c core/src/mcp_server.c core/src/err.c core/src/trace.c -o build/test_m2
./build/test_m2

echo "== 可靠性定向测试（丢片/重传/去重/槽回收） =="
$CC -O2 -std=c99 -Wall -Wextra -Werror -Icore/include \
   tests/test_reliability.c core/src/frame.c core/src/session.c \
   core/src/trace.c -o build/test_reliability
./build/test_reliability

echo "== FCS16 往返（通道无链路校验时才开） =="
$CC -O2 -std=c99 -Wall -Wextra -Werror -DBT_MCP_CFG_FCS16=1 -Icore/include \
   tests/test_fcs16.c core/src/frame.c core/src/trace.c -o build/test_fcs16
./build/test_fcs16
# 同一批核心单测在 FCS16 模式下再跑一遍（帧长差 2B，硬编码偏移会暴露）
$CC -O2 -std=c99 -Wall -Wextra -Werror -DBT_MCP_CFG_FCS16=1 -Icore/include \
   tests/test_m2.c core/src/frame.c core/src/session.c core/src/json.c \
   core/src/writer.c core/src/mcp_server.c core/src/err.c core/src/trace.c -o build/test_m2_fcs16
./build/test_m2_fcs16 > /dev/null && echo "  单元( FCS16 ) OK"

echo "== Bridge 客户端端到端（client L5 ↔ server L5） =="
$CC -O2 -std=c99 -Wall -Wextra -Werror -Icore/include \
   tests/test_client.c core/src/frame.c core/src/session.c core/src/json.c \
   core/src/writer.c core/src/mcp_server.c core/src/mcp_client.c \
   core/src/err.c core/src/trace.c -o build/test_client
./build/test_client

echo "== 内存档位构建（COMPACT/STANDARD 各编一份，验证零语义差异的可裁剪性） =="
for tier in BT_MCP_TIER_COMPACT BT_MCP_TIER_STANDARD; do
    $CC -O2 -std=c99 -Wall -Wextra -Werror -Icore/include -DBT_MCP_TIER=$tier \
       tests/test_m2.c core/src/frame.c core/src/session.c core/src/json.c \
       core/src/writer.c core/src/mcp_server.c core/src/err.c core/src/trace.c -o build/test_m2_$tier
    ./build/test_m2_$tier > /dev/null && echo "  $tier OK"
done

echo "== 开销基线（参考值，不影响判定） =="
$CC -O2 -std=c99 -Wall -Wextra -Werror -Icore/include \
   tools/bench.c core/src/frame.c core/src/session.c core/src/trace.c -o build/bench
./build/bench | head -8

echo "== diff harness =="
python3 tests/diff_harness.py "$ROUNDS"

echo "== M3 session diff =="
python3 tests/m3_session_harness.py "$ROUNDS"

echo "== M4 mcp diff =="
python3 tests/m4_mcp_harness.py "$ROUNDS"

echo "== M4b e2e diff =="
python3 tests/m4b_e2e_harness.py "$ROUNDS"

if [ "$MODE" = "asan" ]; then
    echo "== ASan/UBSan 差分（同一批脚本，内存错误即失败） =="
    $CC -O1 -g -std=c99 -fsanitize=address,undefined -fno-omit-frame-pointer \
       -Icore/include mock/mock_main.c core/src/frame.c core/src/session.c \
       core/src/json.c core/src/writer.c core/src/mcp_server.c \
       -o build/btmcp_mock_asan
    BT_MCP_MOCK=build/btmcp_mock_asan python3 tests/m4_mcp_harness.py "$ROUNDS"
    BT_MCP_MOCK=build/btmcp_mock_asan python3 tests/m3_session_harness.py "$ROUNDS"
fi

echo "ALL SUITES GREEN"
