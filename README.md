# bt-mcp

Bluetooth-carried MCP (Model Context Protocol) stack — a platform-agnostic
C99 protocol core that runs MCP over BLE GATT / Classic SPP / L2CAP CoC / any
byte channel, with zero vendor ifdefs and zero OS dependencies.

## Status (2026-11-11, v0.5)

| Layer | Module | Notes |
|---|---|---|
| L3 | `core/src/frame.c` | 0xB7 分帧/重组；缺片登记 NACK；单次 emit/片；可选 FCS16；MTU 热更新 |
| L4 | `core/src/session.c` | 状态机 + 信用闭环 + 消息级 ACK/NACK/重传/去重 + RESUME 防爆破 + RTT + RTO 退避 |
| L5 | `core/src/mcp_server.c` | 标准 MCP 握手、tools/resources/prompts、订阅、日志分级、富内容、`_meta`、双向请求、batch |
| L5 | `core/src/mcp_client.c` | 主动握手、请求/响应 id 匹配、超时上抛 |
| port | `port/bt_mcp/` | GATT（包/不可靠）、SPP（流/可靠）、L2CAP CoC（包/可靠/自带流控） |
| 辅助 | `json.c` `writer.c` `err.c` `trace.c` | 零分配解析/写出、稳定错误串、可裁调试钩子 |

**回归基线**（`./build.sh`，2026-11-11 实测）：

| 套件 | 数量 |
|---|---|
| C ↔ 独立 Rust oracle 差分 | **1295**（主矩阵 1045 + FCS16 配对 196 + 共享模式 54） |
| Port HIL（GATT/SPP/CoC） | **198** |
| 单元与定向 | **626** |
| 官方 MCP TypeScript SDK 互验 | **38** |
| golden transcript | **20** |

覆盖率：行 **94.7%** / 分支 **67.1%**（门禁 94/66）。
ASan/UBSan 全绿，TSan 干净，fuzz 帧层 200 万轮 + FCS16 80 万轮零崩溃。

## 规范覆盖（v0.5）

| 能力 | 方法 |
|---|---|
| 握手 | `initialize`、`notifications/initialized`、`ping`（响应为 `{}`） |
| 工具 | `tools/list`、`tools/call`（同步/异步/进度/取消/富内容/`_meta`） |
| 资源 | `resources/list`、`resources/read`（游标分页）、`subscribe` / `unsubscribe` |
| prompts | `prompts/list`、`prompts/get` |
| 日志 | `logging/setLevel` + `notifications/message` |
| 下行通知 | `notifications/{tools,resources,prompts}/list_changed`、`resources/updated`、`progress`、`cancelled` |
| 双向 | server→client 请求承载（sampling/roots/elicitation），在途槽 + 超时 |
| 安全 | 授权真正实施：`CONSENT` 走回调、`RESTRICTED` 一律拒绝 → `-32003` |

未实现：`resources/templates/list`、`completion/complete`、sampling 完整语义、OAuth。

## 版本演进要点

**v0.5（可验证性与互操作）**
- 官方 MCP TS SDK 作 Host 互验 38 项——这是唯一能回答「第三方能否理解我们的输出」的手段
- JSON-RPC batch 按协商版本分支（旧版本聚合、新版本 `-32600`）
- `_meta` 透传（结果级）、进度通知 `message`
- 大消息重传保护（`tx_ref` 引用式，突破 512B）、拥塞退避（RTO 指数退避）
- 多会话（`bt_mcp_server_pool_t`）、共享重组缓冲（FULL 档 −24.6KB）
- 错误码语义化（`-32001` 未就绪 与 `-32002` 资源不存在 分离）

**v0.4（规范覆盖）**
- 工具元数据（`title`/`annotations`/`outputSchema`）、富内容（image/audio/embedded）
- prompts、资源订阅、日志分级、`list_changed`
- 授权真正实施（此前 `authz` 字段零引用，是装饰品）
- server→client 请求（MCP 是双向协议，此前只会应答）
- MTU 热更新、NVM 持久化、keepalive（默认关）

**v0.3（能接真实 Host）**
- 丢包可恢复（ACK/NACK + 重传槽 + 去重窗口）
- 信用闭环（消费即回补，不再要求应用手工 CREDIT）
- `protocolVersion` 协商回显、`serverInfo` 嵌套、`_meta` 通道能力声明
- 字符串 request id、RESUME 防爆破、内存档位、单次 emit

## 破坏性变更（相对 v0.3）

`bt_mcp_tool_def_t` / `bt_mcp_resource_def_t` 字段顺序变化，
`bt_mcp_server_cfg_t` 新增 `prompts`/`prompts_count`/`consent`/`consent_user`。

**迁移方式：改用指定初始化器**（`.name = ...`），即不受字段顺序影响。
仓内全部测试已改；`mcp_client` 的拼装缓冲已移入实例（无 API 影响）。

## Layout

```
core/include/bt_mcp/  public headers (tier, config, capability, err, frame, session,
                      json, writer, mcp_server, mcp_client)
core/src/             protocol core (C99, no OS deps, no malloc)
port/bt_mcp/          channel adapters: gatt_port.h, spp_port.h, coc_port.h (header-only)
mock/                 C mock binary speaking the differential-harness instruction protocol
oracle/               independent Rust oracle for differential testing
tests/                diff harnesses (M1/M3/M4a/M4b) + HIL + unit + reliability + client tests
tools/                bench.c（开销基线）、coverage.sh（覆盖率门禁）
fuzz/                 standalone differential fuzz engine
docs/                 bt-mcp-interface-spec.html — Proxy/Host integration spec
build.sh              build mock + Rust oracle, run every suite
```

## 两个使用角色

- **设备端（server）**：`bt_mcp_server_t`——注册工具体/资源体，接 `initialize`、
  跑 `tools/call`、推 `notifications/progress`。
- **桥接端（client）**：`bt_mcp_client_t`——主动握手、发请求、按 id 匹配响应、
  超时上抛；同一份 core，同一套 L3/L4。

两者都可用 `bt_mcp_session_feed()` 一行把收到的字节喂进协议栈：

```c
/* port 的「收到数据」回调里 */
bt_mcp_session_feed(&sess, data, len, my_l5_sink, my_ctx);
/* 主循环节拍 */
bt_mcp_session_tick(&sess, now_ms);
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

## 文档

| 文档 | 用途 |
|---|---|
| [`CHANGELOG.md`](CHANGELOG.md) | 按版本记录对外变更（含破坏性变更） |
| [`docs/release-notes-v0.3.md`](docs/release-notes-v0.3.md) | v0.3 发布说明（GitHub Release 正文 / PR 描述） |
| [`docs/v0.3-validation.md`](docs/v0.3-validation.md) | 变更清单、验证口径、实测数据、已知限制 |
| [`docs/roadmap-v0.4.md`](docs/roadmap-v0.4.md) | v0.4 缺口清单（含代码证据、优先级、工作量） |
| [`docs/release-notes-v0.4.md`](docs/release-notes-v0.4.md) | v0.4.x 发布说明 + PR 描述（可直接贴 GitHub） |
| [`docs/v0.4-validation.md`](docs/v0.4-validation.md) | v0.4.x 验证报告（四层验证体系、矩阵、复现命令） |
| [`docs/roadmap-v0.5.md`](docs/roadmap-v0.5.md) | v0.5 规划与设计（可验证性与互操作）+ 执行结果 |
| [`docs/v0.5-validation.md`](docs/v0.5-validation.md) | v0.5 验证报告（含四层验证战果、已知未达标项） |
| [`docs/roadmap-v0.6.md`](docs/roadmap-v0.6.md) | v0.6 规划与设计（自动化对抗与规模验证） |
| [`docs/bt-mcp-core-draft.md`](docs/bt-mcp-core-draft.md) | 协议草案 **v0.5**（设计原理、线格式、完整 API 骨架、实测开销） |
| [`docs/bt-mcp-interface-spec.html`](docs/bt-mcp-interface-spec.html) | 集成者规约（Proxy/Host 接入） |
