# BT-MCP v0.4.x 发布说明

> 版本线：v0.4.0（规范覆盖与可验证性）→ v0.4.1（收尾：golden + TSan）
> 基线：v0.3.0 · 后续：[v0.5 规划](roadmap-v0.5.md)

---

## 一句话

v0.3 让协议核心**能接真实 MCP Host**；v0.4 让它在**规范上站得住，并且能被机器证明**。

## 为什么发这一版

v0.3 已经把「丢包可恢复、信用闭环、标准握手、字符串 id」这四个一票否决问题解决了，
但对照 MCP 规范逐条核对后，仍有一批「声明了但没实现」和「实现了但没验证」的缺口：

- `AUTHZ_CONSENT` / `AUTHZ_RESTRICTED` 有定义，但 `mcp_server.c` 里**零引用**——
  权限声明是装饰品，这比不声明更危险；
- `resources/subscribe`、`prompts/*`、`logging/setLevel` 完全没实现；
- MCP 是**双向**协议（sampling / roots / elicitation 都是 server 发起的），
  而 v0.3 的 server 只会应答；
- FCS16 曾经**静默失效**数月，因为差分矩阵从来不跑这个配置；
- 工具结果只有 `type:"text"`，图片/音频/内嵌资源无法表达。

## v0.4.0 —— 规范覆盖与可验证性

### 新增能力

| 能力 | 说明 |
|---|---|
| 工具元数据 | `title`、`annotations`（readOnly/destructive/idempotent/openWorld）、`outputSchema` |
| 富内容结果 | `text` / `image` / `audio` / `embedded resource` 多内容项混排 |
| prompts | `prompts/list` + `prompts/get`（`bt_mcp_prompt_def_t`） |
| 资源订阅 | `resources/subscribe` / `unsubscribe` + `notifications/resources/updated` |
| 日志分级 | `logging/setLevel` + `bt_mcp_server_log()` + `notifications/message` |
| 列表变化 | `bt_mcp_server_notify_list_changed()`；`capabilities` 如实声明 |
| **授权真正实施** | `AUTHZ_CONSENT` 走 consent 回调；`AUTHZ_RESTRICTED` 一律拒绝；→ `-32003` |
| **server→client 请求** | `bt_mcp_server_request()`：在途槽、id 匹配、超时 |
| MTU 热更新 | `bt_mcp_framer_set_mtu()`——BLE 完成 MTU 交换后无需重建 framer |
| NVM 持久化 | `bt_mcp_storage_t` + `save_state` / `load_state`（带 magic/version 校验） |
| keepalive | `KEEPALIVE_MS` / `IDLE_DEAD_MS`（默认关闭，行为不变） |
| 诊断 | `strerror`、`trace`（编译期可裁）、`json_get_f64`、`w_key` |

### 可验证性（这一版的重点）

| 手段 | 内容 | 能发现什么 |
|---|---|---|
| 差分 | 1339 项 + FCS16 配对（m1/m3/m4） | C 与 Rust 是否**一致** |
| 结构符合性 | `test_conformance.c` 59 项 | 响应是否**符合规范结构** |
| golden | `test/golden/*.json` 20 条，人工按规范核对 | 字节是否**非预期漂移** |
| 官方 SDK 互验 | 官方 MCP TS SDK 38 项（v0.4.1 后的 V2） | 第三方能否**真正理解**我们的输出 |
| 有损通道 | `test_lossy.c` 17 项 | ARQ 在丢包/重复/乱序下是否**收敛** |
| TSan | `test_threads.c` 10 项 | 多实例并发是否**有共享可变状态** |
| 覆盖率门禁 | 行 87% / 分支 57% | 是否**倒退** |

### 这一版抓到的真缺陷（5 个）

1. **`g_rbuf` 是文件级 static** —— 两个 server 实例互相覆盖（桥接同时连两台设备即触发）。
2. **内嵌资源内容项多一个右花括号** —— 含 embedded resource 的 `tools/call` 输出
   **非法 JSON**，任何 Host 都会解析失败。差分发现不了（C 与 Rust 一起错）。
3. **`notifications/message` 的 `level` 值缺引号**（`"level":warning`）。
4. **`ping` 响应不是空对象** —— 规范要求 `{}`，实现回的是 `{"pong":true}`。
   golden 人工核对时发现。
5. **`mcp_client.c` 的两个 8KB 模块级缓冲** —— 桥接同时连多台设备会互相覆盖
   （与 `g_rbuf` 同类）。

## v0.4.1 —— 收尾

- **G4 golden transcript**：三组场景 20 条期望响应固化，逐条对照规范人工核对
  （第 4 个缺陷就是这一步发现的）。
- **G5 TSan**：两组独立端到端栈、两线程各 200 轮工具调用，`-fsanitize=thread`
  零竞争；顺带在 TSan 下跑了四个单线程套件（第 5 个缺陷由这条路径暴露）。
- **V2（官方 SDK 互验）**：官方 MCP TS SDK 1.32.2 作 Host，38 项校验通过。
  这条测试当场发现一个真实集成缺陷：pipe 传输把 L3 的**分片**当成独立 JSON 写出，
  SDK 收到半截消息——必须在 transport 边界做重组。

## 破坏性变更（相对 v0.3）

`bt_mcp_tool_def_t` / `bt_mcp_resource_def_t` 字段顺序变化，
`bt_mcp_server_cfg_t` 新增 `prompts` / `prompts_count` / `consent` / `consent_user`。

迁移方式：**改用指定初始化器**（`.name = ...`），即不受字段顺序影响。
（v0.4.1 起 `mcp_client` 的拼装缓冲也移入实例，无 API 影响。）

## 验证数据（v0.4.1）

| 套件 | 数量 |
|---|---|
| 差分（C ↔ 独立 Rust oracle） | 1339，另加 FCS16 配对 m1 85 / m3 52 / m4 55 |
| Port HIL（GATT/SPP/CoC） | 198 |
| 单元与定向 | 295（核心 63 + 可靠性 47 + 有损 17 + FCS16 51 + 符合性 59 + golden 20 + 重入 8 + 并发 10 + 客户端 23） |
| 官方 SDK 互验 | 38 |
| 覆盖率 | 行 87.2% / 分支 57.4%（门禁 86/55） |
| 消毒器 | ASan/UBSan 全绿；TSan 干净 |
| fuzz | 默认 200 万轮 + FCS16 80 万轮，零崩溃 |

## 已知限制（v0.5 处理）

1. >512B 的消息在不可靠包通道上不享重传保护。
2. `idx` 为 u8 → 单消息最多 255 片；ATT_MTU 23 下有效上限 3570B。
3. 分支覆盖率 57%，错误路径仍有未覆盖分支。
4. 单会话模型（一个 server 绑一个 session）。
5. 无批处理、无 `_meta` 透传、无拥塞退避、无共享重组缓冲。

## 升级步骤

```sh
git pull && ./build.sh        # 全绿后再改自己的 port
cd tests/sdk_interop && npm install   # 可选：跑官方 SDK 互验
```

按破坏性变更清单改用指定初始化器；确认 GATT port 的 `cap.reliable = 0`。

---

# 附：PR 描述

## 标题

`feat(v0.4): 规范覆盖与可验证性——授权实施、双向请求、富内容、prompts/订阅/日志、FCS16 差分、SDK 互验`

## 摘要

把 v0.3 的「能接 Host」推进到「符合规范、可被机器证明」。补齐 6 类规范缺口，
把授权从装饰品变成真实施，并建立四层验证体系（差分 / 结构 / golden / 官方 SDK）。

## 动机

对照 MCP 规范逐条核对后发现：`authz` 声明未实施、`prompts`/订阅/日志缺失、
server 不能发起请求、FCS16 从未被差分覆盖、工具结果只有纯文本。
这些缺口共同导致「看起来能用，实际不符合规范」。

## 改动要点

- L5：工具元数据、富内容、prompts、订阅、日志、list_changed、授权、S2C 请求
- L3/L4：MTU 热更新、NVM 持久化、keepalive、响应缓冲去静态化（server + client）
- 工程：FCS16 进差分、结构符合性、golden、官方 SDK 互验、TSan、覆盖率门禁

## 测试方式

```sh
./build.sh              # 全部套件
./build.sh 150 asan     # ASan/UBSan
BT_MCP_TSAN=1 ./build.sh 60
cd tests/sdk_interop && npm install && node sdk_interop.mjs ../../build/btmcp_mock
```

## 风险与影响面

- **API 破坏性变更**：工具/资源结构体字段顺序（仓内已全改指定初始化器）。
- port 无需改动（v0.3 的 `cap.reliable = 0` 约定不变）。
- 多实例现在真正安全（server 与 client 的静态缓冲都已移入实例）。

## 检查清单

- [x] 差分全绿（含 FCS16 配对）
- [x] Port HIL 全绿
- [x] 单元/定向/golden/并发全绿
- [x] 官方 MCP SDK 互验通过
- [x] ASan/UBSan 与 TSan 干净
- [x] 覆盖率门禁通过
- [x] 文档更新（CHANGELOG / 草案 / 接口规约 / v0.5 规划）
