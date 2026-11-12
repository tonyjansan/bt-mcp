# Changelog

本文件记录 bt-mcp 协议核心的对外变更。格式参考
[Keep a Changelog](https://keepachangelog.com/zh-CN/1.1.0/)，
版本号遵循 [语义化版本](https://semver.org/lang/zh-CN/)。

## [Unreleased]

（无）

## [0.7.0] - 进行中（2026-11-12）

### 修复

- **重复投递（P0，v0.6 遗留）**：根因是接收去重时间窗 `BT_MCP_CFG_DEDUPE_MS`
  (5s) 短于发送侧完整重传寿命（指数退避下约 9s），迟到副本落在窗外被当作
  新消息二次上抛。修复为 20000ms，并新增发送侧 **stale 守卫**
  （重传前判断对端指纹是否已被挤出，超出则放弃而非制造重复，
  计入 `stat_tx_stale_drop`）。8 个链路场景实测重复全部归零。
- **小 MTU 交付率过低**：MTU 23 + 200B 载荷（12 片）在 20% 丢包下交付率仅 23%。
  原因是 NACK 一次只补一片，12 轮才能补齐而 `retry_max=3` 早已耗尽。
  改为 **NACK 尾部重放**（从缺片补到末尾，一轮补齐），交付率提升到 **57%**。

### 新增

- `bt_mcp_framer_frag_count()`：消息在当前 MTU 下的分片数（公开 API）。
- `BT_MCP_CFG_NACK_REPLAY_MAX`（默认 16）、`BT_MCP_CFG_NACK_REPLAY_BUDGET_X`（默认 16）。
- 差分覆盖 MTU 热更新：mock/oracle 新增 op `0x09 SET_MTU`，oracle 增加
  `mtu_cur`；harness 新增场景 `v07_set_mtu`。此前 `bt_mcp_framer_set_mtu()`
  虽已实现但**从未被差分覆盖**（未测的公开 API）。

## [0.6.0] - 2026-11-11

代号：自动化对抗与规模验证。v0.5 靠人写用例，边际收益已明显下降；
v0.6 把验证引擎换成「自动生成 + 不变量驱动」。

### 新增

- **表驱动组合矩阵**（`tests/test_matrix.c`）：方法 × 参数形态 × 失败点
  自动生成 **3141 个用例 / 6213 项断言**；另含逐字节失败扫描与容量扫描
  （同一套用例用 4 种 `MAX_MSG` 编译，覆盖不同溢出位置）。
- **语法感知 fuzz**（`fuzz/fuzz_l5.c`）：保留 JSON 外壳、变异结构，
  四条不变量（不崩溃 / 返回码合法 / 状态不粘 / 槽位不泄漏）；
  实测 375 万次零违反。
- **正交差分矩阵**（`tests/matrix_runner.py`）：(FCS16 × 重组槽 × MTU × 版本)
  = 16 组合自动枚举；**配置签名前置比对**（mock 与 oracle 各新增 `config` 模式）。
- **链路仿真 soak**（`tests/test_soak.c`）：丢包/重复/乱序/突发 8 轮阶梯加压。
- **多会话压测**（`tests/test_stress.c`）：4 会话 × 4 线程 + 随机断链重连。
- **RAM 预算门禁**（`tools/ram_baseline.py`）：3 档位 × 4 配置 = 12 组合基线。
- **文档一致性检查**（`tools/check_docs.py`）：符号存在性、基线数字、版本标记。

### 修复

- **信用饥饿死锁**（高）：累计 CREDIT 帧不受 ARQ 保护，丢一个就少一格；
  发送方信用耗尽且管道已空时，对端无新消费可用来补发 → 永久 `E_AGAIN`。
  修复：空闲期周期性重发累计 CREDIT（幂等），`BT_MCP_CFG_CREDIT_REFRESH_MS`。
- **writer 错误语义**：原来把所有 sink 失败压平成 `E_IO`，把「放不下」
  伪装成「I/O 故障」；改为透传 sink 错误码（`E_TOOBIG` vs `E_IO`）。
- **去重窗口 4 → 8**：soak 实测 4 个槽覆盖不住「放弃重试后仍在途的副本」，
  10% 丢包下 120 条消息重复投递 6 次；加宽后降到 1 次。

### 验证

- 差分：1295 项 + 正交矩阵 16 组合 × 2 harness 全绿（配置签名已对齐）。
- Port HIL：198。单元与定向：**14000+ 项断言**（含矩阵 6213 + 11469）。
- 官方 SDK 互验：38。golden：20。
- 覆盖率：行 **95%** / 分支 **68%**（门禁 95/68）。
- ASan/UBSan 全绿；TSan 干净；fuzz 帧层 200 万轮 + L5 375 万次零违反。

### 已知未解决

- **重复投递**在 30%~50% 丢包下仍出现（2~5/120 条），已定位方向未定位根因。
- **小 MTU(23) + 大载荷交付率仅 ~8%**（31 片/条，任一片补不齐即放弃）。
- 分支覆盖率 68%，未达 v0.5 定的 75%；N1 曾预测能跨过，**实测证伪**
  （剩余缺口是「短路条件的边」而非「未执行的行」）。

## [0.5.0] - 2026-11-11

代号：可验证性与互操作。v0.4 证明「符合规范」，v0.5 证明「第三方也这么认为」，
并把工程能力从「人写用例」推进到「配置矩阵 + 故障注入」。

### 新增

- **官方 MCP SDK 互验**：`@modelcontextprotocol/sdk` 作 Host，38 项校验通过
  （`tests/sdk_interop/`）。mock 新增交互式 `pipe` 模式。
- **JSON-RPC batch（按协商版本分支）**：2025-03-26 及更早聚合成数组，
  2025-06-18+ 明确回 `-32600`。含 depth/string-aware 元素切分。
- **`_meta` 透传**：`bt_mcp_call_set_meta()`（结果级，content 闭合后输出）、
  `bt_mcp_w_meta()`（结果对象 writer 用）；进度通知支持 `message`。
- **大消息重传保护**：`bt_mcp_session_tx_ref()` 引用式，突破 512B 限制。
- **拥塞退避**：RTO 指数退避（上限 `BT_MCP_CFG_RTO_MAX_MS`），ACK 回落；
  默认态与 v0.4 逐字节一致。
- **多会话**：`bt_mcp_server_pool_t`（init/add/rx/tick/count），按 session 分派。
- **共享重组缓冲**：`BT_MCP_CFG_SHARED_REASM`，framer 41072B → 16448B（-60%）。
- **错误码语义化**：`BT_MCP_EC_*` 常量；未初始化改 `-32001`，与 `-32002` 分离。

### 修复

- **`mcp_client.c` 两个 8KB 模块级缓冲**（`g_cbuf`/`g_sbuf`）→ 移入实例。
- **`ping` 响应不是空对象**（规范要求 `{}`）。
- **writer 在 `sink == NULL` 时崩溃** → 所有写入口校验，返回 `E_ARGS`。
- **`bt_mcp_client_call_tool` / `read_resource` 缺 `!c` 校验** → NULL 崩溃。
- **JSON 取值器缺输出指针校验** → 传 NULL 崩溃，统一返回 `E_ARGS`。
- **解析器接受尾随垃圾与尾随逗号** → 改为严格拒绝（oracle 同步）。

### 验证

- 差分：1339+，含 FCS16 配对与共享模式配对；配置签名需两侧一致。
- Port HIL：198。单元/定向：**578**。
- 官方 SDK 互验：38。golden：20。
- 覆盖率：行 **94.7%** / 分支 **67.1%**（v0.4 为 88.4%/57.9%）。
- ASan/UBSan 全绿；TSan 干净；fuzz 帧层 200 万轮 + FCS16 80 万轮零崩溃。

### 已知未达标

- 分支覆盖率目标 75%，实际 **67.1%**。缺口集中在 `mcp_server.c` 的
  「方法 × 参数形态 × 失败点」组合空间（432 个分支）。转 v0.6 N1（表驱动矩阵）。

## [0.4.0] - 2026-11-11

代号：规范覆盖与可验证性。把 v0.3 的「能接 Host」推进到「符合规范、可被机器证明」。

### 新增

- **工具元数据**：`title`、`annotations`（readOnly/destructive/idempotent/openWorld）、
  `outputSchema`；资源增加 `title`、`size`。
- **富内容结果**：`bt_mcp_w_content_text/image/audio/resource`，一条 `tools/call`
  可返回多个内容项（文本 + 图片 + 音频 + 内嵌资源混排）。
- **prompts**：`prompts/list`、`prompts/get`（`bt_mcp_prompt_def_t` 注册表）。
- **资源订阅**：`resources/subscribe` / `unsubscribe`、
  `bt_mcp_server_notify_resource_updated()`。
- **日志分级**：`logging/setLevel`、`bt_mcp_server_log()`、`notifications/message`。
- **列表变化通知**：`bt_mcp_server_notify_list_changed()`；
  `capabilities` 如实声明 `listChanged` / `subscribe` / `logging`。
- **授权真正实施**：`AUTHZ_CONSENT` 走 `consent` 回调、`AUTHZ_RESTRICTED` 一律拒绝，
  拒绝映射为 `-32003`（新增 `E_AUTHZ`）。
- **server→client 请求**：`bt_mcp_server_request()` / `bt_mcp_server_inflight()`，
  在途槽、按 id 匹配响应、超时（`BT_MCP_CFG_S2C_TOUT_MS`）。sampling / roots /
  elicitation 由此可发。
- **MTU 热更新**：`bt_mcp_framer_set_mtu()`——BLE MTU 交换后无需重建 framer。
- **NVM 持久化**：`bt_mcp_storage_t` + `bt_mcp_session_set_storage/save_state/
  load_state/resume_token`（快照带 magic/version 校验）。
- **keepalive**：`BT_MCP_CFG_KEEPALIVE_MS` / `BT_MCP_CFG_IDLE_DEAD_MS`（默认关闭）。
- **诊断**：`bt_mcp_strerror()`、`bt_mcp_trace()`（`BT_MCP_CFG_TRACE` 编译期可裁）、
  `bt_mcp_json_get_f64()`（浮点参数，无 libm 依赖）、`bt_mcp_w_key()`。

### 修复

- **响应缓冲是文件级 static**：`g_rbuf`/`ebuf`/`nbuf` 多实例互相覆盖
  （桥接端同时连两台设备即触发）→ 全部移入 `bt_mcp_server_t`。
- 顺带修掉重写过程中引入的 6 处缺陷（引号偏移、双花括号、内容项二次包装等），
  全部由差分测试当场抓出。
- **内嵌资源内容项多一个右花括号**，导致含 embedded resource 的 `tools/call`
  响应是非法 JSON（Host 侧必解析失败）。差分发现不了（C 与 Rust 一起错），
  由符合性测试 + 独立 Python 解析器定位。
- **`notifications/message` 的 level 值缺引号**（`"level":warning`）。
- **`ping` 响应不是空对象**（回的是 `{"pong":true}`，规范要求 `{}`）。golden
  核对时发现。
- **`mcp_client.c` 的两个 8KB 模块级缓冲**（`g_cbuf`/`g_sbuf`）会让同时连接
  两台设备的桥接互相覆盖 → 移入实例（与 `g_rbuf` 同类缺陷）。

### 破坏性变更

- `bt_mcp_tool_def_t` 字段顺序变化（新增 title / output_schema_json / annotations）。
- `bt_mcp_resource_def_t` 字段顺序变化（新增 title / size / sub）。
- `bt_mcp_server_cfg_t` 新增 `prompts` / `prompts_count` / `consent` / `consent_user`。
- 迁移：改用**指定初始化器**（`.name = ...`），即不受字段顺序影响。

### 验证

- 差分：1339 项（C ↔ 独立 Rust oracle，15 个 MCP 场景含授权/富内容/prompts/订阅/日志），
  **另加 FCS16 配对差分**（m1 65 + m3 52 + m4 55）。
- Port HIL：198 项。
- 单元与定向：295 项（核心 63 + 可靠性 47 + 有损通道 17 + FCS16 51 + 符合性 59 +
  golden 20 + 重入 8 + 并发 10 + 客户端 23）。
- 覆盖率：行 87.1% / 分支 57.6%（llvm-cov，`tools/coverage.sh` 门禁 86/55）。
- TSan：多实例并发用例无数据竞争；`test_m2`/`client`/`reentrancy`/`reliability`
  在 TSan 下同样干净。
- 有损通道（50% 丢包 + 10% 重复 + 乱序）下「每条消息恰好投递一次」成立。
- ASan/UBSan 全绿；fuzz 双模式零崩溃。

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
