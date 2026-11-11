# Changelog

本文件记录 bt-mcp 协议核心的对外变更。格式参考
[Keep a Changelog](https://keepachangelog.com/zh-CN/1.1.0/)，
版本号遵循 [语义化版本](https://semver.org/lang/zh-CN/)。

## [Unreleased]

（无）

## [0.3.0] - 2026-11-11

代号：互通与可靠性。这一版的目标是让协议核心「能接真实 MCP Host，且在会丢包的
GATT 上可用」。

### 新增

- **不可靠包通道消息级可靠**：`ACK(0x06)` / `NACK(0x07)` 控制帧、发送重传槽环、
  超时整条重传、接收侧 `seq+len+CRC32` 指纹去重（`BT_MCP_CFG_TX_SLOTS`、
  `BT_MCP_CFG_TX_RETRY_BYTES`、`BT_MCP_CFG_RX_FP_SLOTS`、`BT_MCP_CFG_ACK_TOUT_MS`、
  `BT_MCP_CFG_MAX_RETRY`、`BT_MCP_CFG_DEDUPE_MS`）。
- **`resources/list` 与 `resources/read`**，含游标分页（`BT_MCP_CURSOR_END` 表示读完）。
- **`tools/call` 异步化**：`BT_MCP_E_PENDING` + `bt_mcp_server_complete()`，
  `bt_mcp_call_progress()` 发 `notifications/progress`，
  `notifications/cancelled` + `bt_mcp_call_cancelled()`。
- **PING RTT 测量**：`bt_mcp_session_ping()` / `bt_mcp_session_rtt()`。
- **Bridge 侧 MCP 客户端**（`mcp_client.h` / `mcp_client.c`）：主动握手、请求表、
  id 匹配、超时上抛、通知回调，与设备端共用同一份 core。
- **`bt_mcp_session_feed()`**：port 一行把收到的字节喂进协议栈。
- **内存档位**（`tier.h`）：`-DBT_MCP_TIER=BT_MCP_TIER_COMPACT|STANDARD|FULL`
  一键选容量，实测静态 RAM 4048 B / 11760 B / 42480 B。
- **L2CAP CoC 适配**（`port/bt_mcp/coc_port.h`）。
- **开销基线工具**（`tools/bench.c`）。
- **CI**（`.github/workflows/ci.yml`）：差分 + HIL + 单测 + 档位构建 + ASan/UBSan +
  双模式 fuzz + 周级长跑。
- 新增测试：`test_reliability.c`（32）、`test_fcs16.c`（51）、`test_client.c`（23）。

### 变更

- `initialize` 响应改为标准 MCP 形态：`protocolVersion` 按对端版本协商回显
  （支持 `2025-11-25` / `2025-06-18` / `2025-03-26` / `2024-11-05`），
  新增嵌套 `serverInfo{name,version}` 与 `_meta.io.btmcp.chan{kind,mtu,window,
  reliable,flow}` 通道能力声明。
- 信用改为**闭环累计语义**：每消费对端一条 MCP 消息自动回补 1 credit，
  载荷为「累计已消费条数」（u16，回绕安全），单个 CREDIT 丢失可自愈；
  饱和到 255。应用层不再需要手工发 CREDIT。
- `cap.reliable == 0` 的通道（GATT）启用重传；`flow_ctrl == 1` 的通道（SPP/CoC）
  不做信用记账。
- FCS16 线序统一为 `hdr(6) | FCS(2) | payload`。
- framer 每个分片只回调一次 `emit`（帧头与载荷已拼好），port 不再需要自行聚合。
- `BT_MCP_CFG_MAX_FRAG` 32 → 255（u8 idx 上限），修复 8161..8192 B 消息在
  MTU 261 下的假性 `E_TOOBIG`。
- JSON 解析严格化：RFC 8259 数字/字面量语法、`\u` 代理对合并（孤代理 → U+FFFD）、
  键名不再截断（超长按键名未知跳过，`BT_MCP_CFG_KEY_MAX` 可配）、
  i64 定长饱和解析（移除 `strtoll` 越界读）。

### 修复

- **信用泄漏导致永久死锁**：重传放弃、发送失败、CREDIT 丢帧、握手期漏补四条路径
  均会单调消耗窗口直至 `E_AGAIN` 永久失败。
- **FCS16 完全失效**：收发线序不一致导致该模式所有包通道帧 CRC 失败、
  流通道大帧载荷错位。
- **流通道一次畸形帧后永久卡死**（`CHUNKED` / `CTRL>255` / `plen>MAX_MSG`），
  现复位解析器并重新同步。
- **握手误判**：Bridge 客户端把任何首个成功响应都当作 `initialize` 响应，
  现按 `initialize` 的请求 id 匹配。
- **去重窗口不足**：指纹槽 2 < 发送窗口 4，ACK 全丢时会二次上抛导致工具重复执行；
  补到 4 并增加代次护栏（只判最近 8 条）。
- **RESUME 可爆破**：连续失败 ≥3 次进入指数退避锁定（`RESUME_REJ` reason=3），
  token 比较改为常数时间。
- 超长字符串 id 现回 `"id":null` + `-32600`（原为误导性 `-32603` 或静默无响应）。
- 取消后不再发送进度通知；工具回调内同步调用 `server_complete()` 返回 `E_STATE`
  （避免双响应与静态缓冲争用）。
- `notifications/cancelled` 的 `requestId` 要求整段为数字（避免 `1.5` 误取消 id 1）。
- `tier.h` 各项改为 `#ifndef` 包裹，档位与逐项覆盖可共存。
- fuzz 引擎在 FCS16 模式下的片长计算错误导致死循环。

### 移除

- `bt_mcp_progress_t` 回调类型（进度能力移到 `bt_mcp_call_t`）。
- `bt_mcp_session_cfg_t` 中的旧字段布局（改为指定初始化器；新增 `retry_max`、
  `ack_tout_ms`）。

### 破坏性变更（迁移见下）

- `bt_mcp_session_rx()` 增加 `seq` 参数。
- `bt_mcp_msg_sink_t` 增加 `seq` 参数。
- 工具回调签名增加 `bt_mcp_call_t *call` 参数。
- `bt_mcp_resource_def_t` 增加 `mime_type` / `read` / `user` 字段
  （旧的三字段初始化器不再编译）。
- `bt_mcp_session_cfg_t` 建议改用指定初始化器（`.tx_window = ...`）。

### 验证

- 差分（C ↔ 独立 Rust oracle）：1339 项全绿（帧 405 / 会话 312 / MCP 313 / 全栈 309）。
- Port HIL：198 项（GATT / SPP / CoC 背靠背，core 零改动）。
- 单元与定向：169 项（核心 63 + 可靠性 32 + FCS16 51 + Bridge 客户端 23）。
- ASan+UBSan 差分全绿；fuzz 默认模式 300 万轮、FCS16 模式 20 万轮零崩溃。

### 已知限制

1. 大于 `BT_MCP_CFG_TX_RETRY_BYTES`（默认 512 B）的消息在不可靠包通道上不享重传保护。
2. `idx` 为 u8 → 单消息最多 255 片；ATT_MTU 23（chunk 14 B）下有效上限 3570 B。
3. 信用计数基准随 `link_up` 重置，前提是「断链是双向事件」。
4. FCS16 目前只有 C 侧往返测试与 fuzz，尚未纳入 C↔Rust 差分矩阵。
5. 真实芯片移植（Zephyr/NimBLE）与手机 Bridge 仍是待办。

## [0.2.0] - 2026-11-04

- 帧层 0xB7 分片/重组、会话状态机、信用窗口、RESUME、MCP 最小闭环、
  GATT/SPP 两种 port、差分 harness 与 fuzz 基线。
- 已知缺口：GATT 丢包不可恢复、信用不闭环、`initialize` 不符合标准 MCP 形态、
  字符串 id 不支持、`resources/*` 未实现。
