# bt-mcp

Bluetooth-carried MCP (Model Context Protocol) stack — a platform-agnostic
C99 protocol core that runs MCP over BLE GATT / Classic SPP / L2CAP CoC / any
byte channel, with zero vendor ifdefs and zero OS dependencies.

## Status (2026-11-10, v0.3)

M1–M4 完成，v0.3 补齐了「能接真实 MCP Host」的全部缺口：

| Layer | Module | Notes |
|---|---|---|
| L3 | `core/src/frame.c` | 0xB7 framing / 重组；缺片保持槽并登记 NACK；单次 emit/片 |
| L4 | `core/src/session.c` | 状态机 + 信用闭环 + 消息级 ACK/NACK/重传/去重 + RESUME 防爆破 + RTT |
| L5 | `core/src/mcp_server.c` | 标准 MCP 握手（serverInfo/_meta）、tools+resources 分页、异步/进度/取消 |
| port | `port/bt_mcp/` | GATT（包/不可靠）、SPP（流/可靠）、L2CAP CoC（包/可靠/自带流控） |

回归基线（`./build.sh`）：**1339 protocol diff checks + 198 port HIL checks
+ 106 unit checks（63 核心 + 22 可靠性 + 21 Bridge 客户端），全绿**，另有 ASan/UBSan 版差分与独立 fuzz 引擎。

v0.3 相对 v0.2 的实质修复：

- **丢包可恢复**：GATT 包通道不再「丢一片 = 丢整条」。L4 重传槽 + `ACK(0x06)` /
  `NACK(0x07)` + 接收侧 `seq+len+CRC32` 去重窗口；超时按原 seq 整条重发。
- **信用闭环**：每消费对端一条消息自动回补 1 credit（饱和 255），不再要求应用层
  手工 `session_ctrl(CREDIT)`——旧实现发满窗口即永久 `E_AGAIN`。
- **MCP 互通**：`initialize` 回 `serverInfo{name,version}` 嵌套对象、按对端
  `protocolVersion` 协商回显、`_meta.io.btmcp` 声明通道能力（kind/mtu/window/
  reliable/flow）——这两项此前完全缺失，标准 Host 握手即失败。
- **字符串 request id**：按原文片段回显；i64 解析改为定长饱和实现，去掉
  `strtoll` 越界读。
- **resources 面**：`resources/list` / `resources/read` + 游标分页（静态切片，零分配）。
- **异步工具**：`E_PENDING` + `bt_mcp_server_complete()` + `notifications/progress`
  + `notifications/cancelled`。
- **RESUME 防爆破**：失败≥3 次进入指数退避锁定，token 常数时间比较。
- **内存档位**：`-DBT_MCP_TIER=...` 一键把静态 RAM 从 4.0KB 调到 41KB（见 `core/include/bt_mcp/tier.h`）。
- **集成坑修复**：framer 每片只 emit 一次（头+载荷一体），port 不再需要自己聚合一帧。

## Layout

```
core/include/bt_mcp/  public headers (tier, config, capability, err, frame, session, json, writer, mcp_server)
core/src/             protocol core (C99, no OS deps, no malloc)
port/bt_mcp/          channel adapters: gatt_port.h, spp_port.h, coc_port.h (header-only)
mock/                 C mock binary speaking the differential-harness instruction protocol
oracle/               independent Rust oracle for differential testing
tests/                diff harnesses (M1/M3/M4a/M4b) + HIL + unit tests
fuzz/                 standalone differential fuzz engine
docs/                 bt-mcp-interface-spec.html — Proxy/Host integration spec
build.sh              build mock + Rust oracle, run every suite
```

## Build & test

```
./build.sh              # 全部差分 + HIL + 单元 + 档位构建
./build.sh 500          # 加大随机轮数
./build.sh 100 asan     # 追加 ASan/UBSan 差分（CI 用）
```

单独跑某一套：

```
python3 tests/diff_harness.py 200        # M1 帧层
python3 tests/m3_session_harness.py 300  # M3 L3+L4
python3 tests/m4_mcp_harness.py 300      # M4a L5
python3 tests/m4b_e2e_harness.py 300     # M4b 全栈 client↔server
```

Port 层 HIL 与单元测试由 `build.sh` 自动编译执行；也可单独编：

```
cc -O2 -std=c99 -Wall -Wextra -Werror -Icore/include -Iport \
   tests/test_m4c_port.c core/src/frame.c core/src/session.c -o build/test_m4c_port
cc -O2 -std=c99 -Wall -Wextra -Werror -Icore/include \
   tests/test_m2.c core/src/*.c -o build/test_m2
```

Fuzz：

```
cc -O2 -std=c99 -Icore/include -DBT_MCP_FUZZ_MAIN fuzz/fuzz_frame.c \
   core/src/frame.c -o build/fuzz_frame && ./build/fuzz_frame 10000000
```

协议草案：[`docs/bt-mcp-core-draft.md`](docs/bt-mcp-core-draft.md)（BT-MCP v0.3）。
集成者文档：[`docs/bt-mcp-interface-spec.html`](docs/bt-mcp-interface-spec.html)。
