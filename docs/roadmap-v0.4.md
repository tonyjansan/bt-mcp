# BT-MCP 后续版本路线（v0.4 及以后）

日期：2026-11-11 · 基线：v0.3.0（`20a674b`）
方法：对 `core/`、`port/` 逐项审计（下文每条都标注了代码证据），再对照 MCP 规范
与真实 BLE 约束排出优先级。**不是愿望清单，是缺口清单。**

优先级定义：
- **P0**：不补则「接了真实 Host 也会失败」或存在安全/正确性缺陷。
- **P1**：真实产品落地必需，或显著影响内存/吞吐/可维护性。
- **P2**：生态与体验，缺了能用但不体面。

---

## A. MCP 规范覆盖缺口

当前 `mcp_server.c` 实际实现的方法（grep 证据）只有 8 个：
`initialize`、`notifications/initialized`、`notifications/cancelled`、`ping`、
`tools/list`、`tools/call`、`resources/list`、`resources/read`。

| # | 缺口 | 说明 | 优先级 | 工作量 |
|---|---|---|---|---|
| A1 | **server→client 请求**（`sampling/createMessage`、`roots/list`、`elicitation/*`） | 当前 server 只会「应答」，没有发起请求的能力：缺少出站请求表、id 分配、响应匹配、超时。这是**架构级**缺口——MCP 是双向协议，很多 Host 能力（采样、根目录、表单引导）依赖它。L5 需要一套与 `mcp_client` 对称的「server 侧请求器」 | **P0** | 大 |
| A2 | **`prompts/list` / `prompts/get`** | 完全未实现；`capabilities.prompts` 也没声明。Host 的斜杠命令/模板功能依赖它 | P1 | 中 |
| A3 | **`resources/subscribe` / `unsubscribe` + `notifications/resources/updated`** | 能力声明里写死 `"subscribe":false`，数据变化只能靠 Host 轮询 | P1 | 中 |
| A4 | **`resources/templates/list`** | 只有静态资源，没有 URI 模板（如 `bt-mcp://dev/log/{level}`） | P1 | 小 |
| A5 | **`logging/setLevel` + `notifications/message`** | 设备日志无法按 Host 要求分级推送 | P1 | 小 |
| A6 | **`completion/complete`** | 参数自动补全（Host 输入提示）缺失 | P2 | 小 |
| A7 | **`notifications/tools/list_changed` / `prompts/list_changed`** | 能力声明 `listChanged:false` 写死；工具集动态变化（如固件功能开关）时 Host 不知道 | P1 | 小 |
| A8 | **工具结果内容类型**：`image` / `audio` / `embedded resource` / `structuredContent` | 现在只产出 `{"type":"text"}`（grep 证据）。传感器截图、音频片段、结构化输出无法表达 | P1 | 中 |
| A9 | **工具元数据**：`title`、`annotations`（`readOnlyHint`/`destructiveHint`/`idempotentHint`/`openWorldHint`）、`outputSchema` | `tools/list` 只输出 `name`/`description`/`inputSchema`。Host 靠注解做安全提示与 UI | P1 | 小 |
| A10 | **资源元数据**：`title`、`size` | 只输出 `uri`/`name`/`description`/`mimeType` | P2 | 小 |
| A11 | **JSON-RPC 批处理** | 完全未实现（grep `batch` = 0）。2025-03-26 版本允许批量，2025-06-18 起移除——需要**按协商版本分支**，否则老 Host 发批量会整体失败 | P1 | 中 |
| A12 | **`_meta` 透传** | 只读取 `_meta.progressToken`，其余 `_meta` 字段（如 `traceparent`、限流提示）丢弃；规范建议原样保留/忽略而非报错 | P2 | 小 |
| A13 | **错误码细化** | `-32002` 同时用于「未初始化」与「资源不存在」；缺 `-32001`（保留）、`-32003`（授权）等语义化码 | P2 | 小 |
| A14 | **采样/日志/进度的双向 `_meta` 关联** | 通知里只回 `progressToken`，不带 `relatedRequestId` 等 | P2 | 小 |

---

## B. 安全

| # | 缺口 | 说明 | 优先级 | 工作量 |
|---|---|---|---|---|
| B1 | **`authz` 字段完全未实施** | `bt_mcp_tool_def_t.authz` / `bt_mcp_resource_def_t.authz` 有定义，但 `mcp_server.c` 里 **零引用**（grep 证据）。即 `AUTHZ_CONSENT` / `AUTHZ_RESTRICTED` 与 `AUTHZ_OPEN` 行为完全一样——声明了权限却不检查，比不声明更危险 | **P0** | 中 |
| B2 | **缺少授权回调与错误语义** | 需要 `consent_cb(tool, args) → allow/deny/ask-host`，以及拒绝时的规范错误码与文案 | **P0** | 中 |
| B3 | **RESUME token 仅 32 bit** | 可爆破面偏大（已有 3 次失败锁定缓解，但 token 熵不足）；建议 ≥64 bit 或 `HMAC(bond_key, counter)` 截断，并提供 token 轮换 | P1 | 小 |
| B4 | **无重放保护** | RESUME_REQ 可被链路内重放（同一 token 反复恢复）；建议加 nonce/计数器 | P1 | 小 |
| B5 | **无速率限制** | 单个 Host 可高频 `tools/call` 打满 CPU/RAM；需要令牌桶与「同时在途调用数」上限（现只有 `MAX_PENDING` 槽） | P1 | 小 |
| B6 | **字段级大小限制缺失** | 只有全局 `MAX_MSG`、键名上限、JSON 深度；缺「单字符串值上限」「数组元素上限」「总 token 预算」 | P2 | 小 |
| B7 | **token 生成无 CSPRNG 约定** | 代码只接收 token，未规定 port 如何生成；文档应给出 `bt_mcp_port_rand()` 钩子与要求 | P2 | 小 |
| B8 | **无完整性保护选项** | 除可选 FCS16 外，载荷本身无 MAC；对医疗/支付类场景需要「应用层签名」扩展点 | P2 | 中 |

---

## C. 可靠性与传输

| # | 缺口 | 说明 | 优先级 | 工作量 |
|---|---|---|---|---|
| C1 | **>512 B 消息在不可靠通道上无重传保护** | 已记录为已知限制；根因是重传槽要存消息体。可改为「分片级 SACK + 调用方持有原文」（发送时记住偏移而不复制），或对超大消息只保护首/末片 | P1 | 中 |
| C2 | **无拥塞/退避策略** | 重传固定 `ack_tout_ms`，无 RTT 自适应、无指数退避（RESUME 有，数据面没有）；链路差时会与丢包共振 | P1 | 中 |
| C3 | **无 keepalive / 空闲超时** | PING 需应用自行调度；无「空闲 N 秒自动探活、M 秒无响应断链」策略 | P1 | 小 |
| C4 | **MTU 协商未落到 port** | `cap.mtu` 由 port 一次性填写；没有「ATT MTU 变更后热更新 cap/framer」的接口（BLE 常见：连接后 MTU 从 23 升到 247） | P1 | 小 |
| C5 | **流通道无显式流控** | 依赖 `flow_ctrl` 标志「声称已有」；若 port 的发送队列会满，缺「E_AGAIN 后续传」的明确契约与测试 | P1 | 小 |
| C6 | **无多通道并存/切换** | 一个 session 绑定一个 framer/通道；GATT + SPP 双通道同时在线、或传输中切换，都不支持 | P2 | 中 |
| C7 | **去重窗口固定 8 条** | 高吞吐 + ACK 延迟大时可能不够；可做成按时间+条数双阈值可配 | P2 | 小 |
| C8 | **无消息级完整性校验（可选）** | CRC32 只用于去重指纹，不用于拒收；若通道无 FCS，损坏的载荷会被当合法消息上抛 | P1 | 小 |

---

## D. 内存与性能

| # | 缺口 | 说明 | 优先级 | 工作量 |
|---|---|---|---|---|
| D1 | **`g_rbuf` 是文件级 static** | `mcp_server.c:174 static char g_rbuf[BT_MCP_CFG_MAX_MSG]`——**两个 `bt_mcp_server_t` 实例会互相覆盖**，无法多会话/多实例（桥接端同时连两台设备就踩坑） | **P0** | 中 |
| D2 | **重组缓冲按槽独立分配** | `frame.h:60` 每槽 `buf[MAX_MSG]`，FULL 档 4×8 KB = 32 KB。可改「单一大缓冲 + 槽偏移」，砍掉约 3/4 | P1 | 中 |
| D3 | **无 8/16 位 MCU 的真机验证** | `uint64_t` 在 3 个头文件 + 7 处实现中出现（进度计数、writer u64）。8 位 MCU 上 64 位乘除是显著开销，可用 `uint32_t` + 分段或条件编译 | P1 | 中 |
| D4 | **JSON 无浮点 getter** | 只有 `get_str/get_u32/get_i32/get_raw/get_sub`。工具参数 `0.5`、`-12.75` 无法取值（必须自己 `get_raw` 再手解析） | P1 | 小 |
| D5 | **无零拷贝大 payload 路径** | `resources/read` 已是静态切片（好），但 `tools/call` 的结果必须经 `g_rbuf` 拼装；超大结果只能报 `E_TOOBIG` | P2 | 中 |
| D6 | **无内存用量自检 API** | 应用无法查询「当前静态 RAM 占用/各槽使用情况」，移植时只能靠 `sizeof` | P2 | 小 |

---

## E. 会话与生命周期

| # | 缺口 | 说明 | 优先级 | 工作量 |
|---|---|---|---|---|
| E1 | **单会话模型** | `bt_mcp_server_t` 绑定一个 `bt_mcp_session_t`；手机 + 手表同时连接、或 Host 重连期间旧会话未清，都会互相干扰 | P1 | 中 |
| E2 | **无 NVM/bond 持久化钩子** | RESUME 依赖 token 与 bond 状态，但没有任何「存/取/失效」回调；掉电后无法恢复 | P1 | 小 |
| E3 | **无空闲/半开连接回收** | 对端消失时靠 frag_tout 与重传超时，时间长且无统一回收策略 | P1 | 小 |
| E4 | **无会话级统计 API** | 有内部 `stat_*` 字段但无公开 getter（重传数、丢弃数、去重数），运维无法观测 | P2 | 小 |
| E5 | **无优雅关闭** | 缺「发完在途消息再断链」的 `session_close()` 语义 | P2 | 小 |

---

## F. 可观测性与调试

| # | 缺口 | 说明 | 优先级 | 工作量 |
|---|---|---|---|---|
| F1 | **无 `bt_mcp_strerror()`** | 错误码只能查头文件；日志里全是 `-6` | P1 | 小 |
| F2 | **无 trace 钩子** | 开发期要定位「为什么这条消息没上抛」只能加 printf。应有 `bt_mcp_set_trace(fn)` 回调，输出帧级/状态机级事件（编译期可裁掉） | P1 | 小 |
| F3 | **无统计快照** | 见 E4 | P2 | 小 |
| F4 | **无「协议一致性自检」模式** | 可在 debug 构建里断言状态机不变量（如 credits ≤ 255、槽不重复占用） | P2 | 小 |

---

## G. 工程质量与测试

| # | 缺口 | 说明 | 优先级 | 工作量 |
|---|---|---|---|---|
| G1 | **FCS16 未进 C↔Rust 差分矩阵** | 只有 C 侧往返测试（51 项）+ fuzz。这正是 v0.3 之前 FCS16 静默失效的根因；应给 oracle 加 FCS 支持并进矩阵 | **P0** | 小 |
| G2 | **无覆盖率门禁** | 有 1339 项差分但不测覆盖率；`mcp_client.c`、`coc_port.h`、`tier.h` 分支可能大片未覆盖 | P1 | 小 |
| G3 | **无属性测试（property-based）** | 差分是「同输入比输出」，缺「任意输入下不变量成立」类测试（如「重组结果长度必等于声明长度」） | P1 | 中 |
| G4 | **无对官方实现的符合性测试** | 未与 MCP 官方 SDK / Inspector 跑过 golden transcript；规范演进时无从回归 | **P0** | 中 |
| G5 | **无 TSan / 重入测试** | 单写者模型只写在文档里；`session_rx` 内回调 `session_tx` 的重入靠人工验证，应有 TSan CI 或用例固化 | P1 | 小 |
| G6 | **无长时间 soak（状态泄漏）** | 无 malloc 但仍可能「槽/计数/指纹」缓慢泄漏；应有 1e8 消息级长跑并断言内存与状态稳定 | P1 | 小 |
| G7 | **无 oracle 差分 fuzz** | 现在 fuzz 只查崩溃，不比对 oracle 输出 | P2 | 中 |
| G8 | **无真实 GATT 丢包模拟** | mock 是完美通道；应有「按概率丢片/乱序/重复」的通道模型，验证 ARQ 在真实丢包率下的收敛 | P1 | 小 |

---

## H. 平台与集成

| # | 缺口 | 说明 | 优先级 | 工作量 |
|---|---|---|---|---|
| H1 | **无真实平台 port** | 只有 header 适配器 + mock；`port/bt_mcp/*.h` 里没有 Zephyr/NimBLE、ESP-IDF、BlueZ 的可编译示例 | P1 | 大 |
| H2 | **无 CMake / Makefile / pkg-config** | 只有 `./build.sh`；第三方集成只能手工拷源文件 | P1 | 小 |
| H3 | **无单文件 amalgamation** | 很多嵌入式项目只吃一个 `.c`/`.h`；需要生成脚本 | P2 | 小 |
| H4 | **无 WASM/浏览器模拟器** | Bridge 开发者无法在没有设备时联调 | P2 | 中 |
| H5 | **无示例工程** | 缺「设备端 + 手机端 + 一条完整工具调用」的可运行样例 | P1 | 中 |
| H6 | **无 MISRA-C 合规说明** | 医疗/汽车客户的第一道门槛 | P2 | 大 |
| H7 | **无静态分析接入** | 未接 clang-tidy / cppcheck / Coverity 基线 | P2 | 小 |

---

## I. 文档与流程

| # | 缺口 | 说明 | 优先级 | 工作量 |
|---|---|---|---|---|
| I1 | **无自动生成 API 参考** | 头文件注释质量尚可，但没有 doxygen/`docs/api/` | P2 | 小 |
| I2 | **无版本/弃用策略** | 缺「协议版本与库版本如何对应」「弃用提前几个版本通知」的明文规则 | P1 | 小 |
| I3 | **无升级指南（跨版本）** | v0.3 的迁移片段在 release notes 里，但没有常驻的 `UPGRADING.md` | P2 | 小 |
| I4 | **CHANGELOG 靠手工维护** | 可按 conventional commits 自动生成 | P2 | 小 |
| I5 | **协议草案与代码仍可能漂移** | v0.3 已修过一次（接口概要停留在 v0.1）；应从公开头文件生成草案中的接口段 | P2 | 中 |

---

## 优先级汇总（建议的 v0.4 范围）

**必须做（否则「能跑」不成立）：**

1. **B1/B2 授权真正实施** —— 现在 `authz` 是装饰品，安全承诺是空的。
2. **A1 双向请求能力** —— sampling / roots / elicitation 全部依赖它，是 L5 的架构补全。
3. **D1 `g_rbuf` 去静态化** —— 多实例/多会话的硬阻塞，改动不大但影响面广。
4. **G1 FCS16 进差分 + G4 符合性测试** —— 把「曾经静默失效」的教训制度化。

**强烈建议同批（成本低、收益直接）：**

5. A2/A3/A5/A7 —— prompts、订阅推送、日志分级、list_changed：四个「小而独立」的规范补齐。
6. A8/A9 —— 内容类型与工具注解：Host UI 与安全提示的基础。
7. D4 浮点 getter、F1 strerror、F2 trace —— 开发体验三件套，各几十行。
8. E2 NVM 钩子、C3 keepalive、C4 MTU 热更新 —— 真机落地三件套。
9. G2 覆盖率门禁、G5 TSan/重入用例、G8 丢包通道模型 —— 把「靠人盯」换成「靠 CI」。

**可以放到 v0.5+：**

10. A11 批处理（版本分支）、A4 资源模板、A6 completion、A12/A13 `_meta` 与错误码细化
11. C1 大消息分片级保护、C2 拥塞退避、D2 共享重组缓冲、D3 8/16 位优化
12. H1 真实平台 port、H5 示例工程、H2 CMake、H3 amalgamation
13. B3/B4 token 强化与重放保护（若目标市场有合规要求则提前）
14. H4 WASM 模拟器、H6 MISRA、I1–I5 文档体系

---

## 附：审计证据索引

| 结论 | 证据 |
|---|---|
| 仅 8 个方法实现 | `grep -oE '"[a-z]+/[a-z_]+"' core/src/mcp_server.c` |
| `authz` 未实施 | `grep -n authz core/src/mcp_server.c` → 无输出 |
| `g_rbuf` 为文件级 static | `core/src/mcp_server.c:174` |
| 每槽独立 `MAX_MSG` 缓冲 | `core/include/bt_mcp/frame.h:60` |
| 能力写死 `listChanged:false` / `subscribe:false` | `core/src/mcp_server.c:373-374` |
| 工具结果仅 `type:"text"` | `grep -oE '\\"type\\":\\"(text|image|audio|resource)\\"'` → 仅 text |
| `tools/list` 仅 3 字段 | `grep -oE '\\"(name|description|inputSchema|title|annotations|outputSchema)\\":'` |
| 无浮点 getter | `core/include/bt_mcp/json.h` 仅 u32/i32/str/raw/sub |
| 无批处理 | `grep -c batch core/src/*.c` → 全 0 |
| 无 CMake/Makefile | `ls CMakeLists.txt Makefile` → 不存在 |
| `uint64_t` 使用 10 处 | `grep -rn uint64_t core/` |
| 端序可移植 | `frame.c` 全用字节拼接，无 `memcpy(&u16)` 直读 |
