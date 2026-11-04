#!/bin/sh
# build.sh —— 构建 C mock 与 Rust oracle，然后跑差分
set -e
cd "$(dirname "$0")"

echo "== build C mock =="
mkdir -p build
cc -O2 -std=c99 -Wall -Wextra -Werror \
   -Icore/include mock/mock_main.c core/src/frame.c core/src/session.c \
   core/src/json.c core/src/writer.c core/src/mcp_server.c \
   -o build/btmcp_mock

echo "== build Rust oracle =="
(cd oracle && cargo build --release --quiet)

echo "== diff harness =="
python3 tests/diff_harness.py "${1:-200}"

echo "== M3 session diff =="
python3 tests/m3_session_harness.py 300

echo "== M4 mcp diff =="
python3 tests/m4_mcp_harness.py 300

echo "== M4b e2e diff =="
python3 tests/m4b_e2e_harness.py 300
