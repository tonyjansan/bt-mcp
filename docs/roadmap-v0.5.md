# BT-MCP v0.5 规划与设计

日期：2026-11-11 · 基线：v0.4.0 · 前置文档：[roadmap-v0.4.md](roadmap-v0.4.md)

## 0. v0.4 实际交付了什么（与规划的差异）

| 规划项 | 状态 | 说明 |
|---|---|---|
| D1 响应缓冲去静态化 | ✅ 完成 | `rbuf`/`ebuf`/`nbuf` 移入实例 |
| F1 strerror | ✅ 完成 | `bt_mcp_strerror()` |
| F2 trace 钩子 | ✅ 完成 | 编译期可裁，已埋点 session/L5 |
| D4 浮点 getter | ✅ 完成 | `bt_mcp_json_get_f64()`，手写解析无 libm |
| A9 工具元数据 | ✅ 完成 | title / annotations / outputSchema；资源 title / size |
| A8 富内容 | ✅ 完成 | text / image / audio / embedded resource 多内容项 |
| A2 prompts | ✅ 完成 | `prompts/list` + `prompts/get` |
| A3 资源订阅 | ✅ 完成 | subscribe/unsubscribe + `notifications/resources/updated` |
| A5 日志分级 | ✅ 完成 | `logging/setLevel` + `notifications/message` |
| A7 list_changed | ✅ 完成 | `bt_mcp_server_notify_list_changed()`，capabilities 如实声明 |
| B1/B2 授权实施 | ✅ 完成 | consent 回调 + RESTRICTED 拒绝 + `-32003` |
| A1 双向请求 | ✅ 完成 | `bt_mcp_server_request()` + id 匹配 + 超时 |
| E2 NVM 钩子 | ✅ 完成 | save/load + magic/version 校验 |
| C3 keepalive | ✅ 完成 | 默认关闭（0），开启后空闲 PING / 判死收尾 |
| C4 MTU 热更新 | ✅ 完成 | `bt_mcp_framer_set_mtu()` |
| G8 丢包通道模型 | ✅ 完成 | `tests/test_lossy.c`，17 项不变量断言 |
| G1 FCS16 进差分矩阵 | ✅ 完成 | oracle 支持 FCS16（env 开关），m1/m3/m4 三组配对差分全绿 |
| G2 覆盖率门禁 | ✅ 完成 | `tools/coverage.sh`（llvm-cov），行 86.6% / 分支 55.6%，阈值 86/55 |
| G4 符合性测试 | ✅ 完成（结构校验版） | `tests/test_conformance.c` 57 项规范结构断言；**尚未**与官方 SDK/Inspector 对跑 |
| G5 重入用例 | ✅ 完成 | `tests/test_reentrancy.c` 8 项；TSan 构建仍未接 |

**G4 的价值已兑现**：它当场抓出两个差分发现不了的真缺陷（`level` 缺引号、
内嵌资源内容项多一个右花括号导致 tools/call 输出非法 JSON）。
这说明「结构校验」和「差分」是互补的，两者都必须有。

仍未完成的只有两件（见 §1 V2/V4 的收敛版）：
- 与**官方 MCP SDK/Inspector** 的真实 transcript 对跑（当前是自己校验自己）；
- TSan/并发工具链接入（当前是单线程 + 手工重入用例）。

---

## 1. v0.5 主题：**可验证性与互操作**

v0.3 解决「能不能用」，v0.4 解决「符合不符合规范」。v0.5 要解决的是更根本的问题：
**我们凭什么相信它是对的？**

v0.4 期间，差分测试当场抓出了我重写时引入的 6 个缺陷（引号偏移、双花括号、
内容项二次包装等）。这既证明差分的价值，也暴露一个问题：**未被差分覆盖的路径
（FCS16）曾静默失效数月**。v0.5 的核心目标是把「靠人盯」变成「靠机器证」。

### P0（必须完成）

| # | 任务 | 设计要点 | 验收 |
|---|---|---|---|
| ~~V1~~ | ~~FCS16 进差分~~ | **v0.4 已完成** | — |
| V2 | **真实 Host 符合性**（G4 的完全版） | 用官方 MCP SDK（TS/Python）作 Host，录制真实 transcript 作 golden；C 侧回放比对 | golden 套件纳入 CI |
| V3 | **分支覆盖率补齐** | 当前分支 55.6%，目标是 `mcp_server.c`/`session.c` 分支 ≥75%：补错误路径（TOOBIG/NOMEM/BUSY/E_LOCKED）、分页边界、游标越界、S2C 超时、取消竞态 | 分支覆盖 ≥75% 并把阈值提上去 |
| ~~V4~~ | ~~重入不变量~~ | **v0.4 已完成基础用例**；剩余：把断言做成 debug 构建可选开关 + TSan 接入 | — |

### P1（同批建议）

| # | 任务 | 设计要点 |
|---|---|---|
| V5 | **`_meta` 透传与双向关联** | 保留未知 `_meta` 键；`notifications/progress` 带 `relatedRequestId`；`sampling` 响应回填 `relatedRequestId`（roadmap A12/A14） |
| V6 | **批处理（按版本分支）** | 协商到 `2025-03-26` 及更早时接受 JSON-RPC batch；`2025-06-18+` 明确回 `-32600`（规范已移除）。需要 L5 入口的数组分派 |
| V7 | **错误码语义化** | 区分「未初始化」与「资源不存在」（当前都用 -32002）；补 `-32001` 保留语义；资源/工具不存在统一 `-32602` |
| V8 | **大消息保护（C1）** | >`TX_RETRY_BYTES` 的消息：改为「分片级 SACK」——发送方只在槽里记 `(seq, len, 分片位图)`，重传时由调用方持有的原文重编（新增 `bt_mcp_session_tx_ref()` 要求调用方保证原文生命周期）；或退化为「只保护首末片」 |
| V9 | **拥塞退避（C2）** | ACK 超时按 RTT 自适应（已有 RTT）并加指数退避上限；连续丢包时降低并发窗口（简单 AIMD） |
| V10 | **多会话（E1）** | `bt_mcp_server_t` 从「绑定一个 session」改为「session 表 + 每会话状态」，支持手机+手表并存；需拆分当前实例级 `calls[]`/`subs[]`/`log_level` |
| V11 | **共享重组缓冲（D2）** | 可选编译开关：单一大 buffer + 每槽偏移，把 FULL 档 4×8KB 降到 8KB+ 少量元数据；代价是碎片整理逻辑 |

### P2（v0.6 或按需）

| # | 任务 |
|---|---|
| V12 | 8/16 位 MCU 专项：`uint64_t` 收敛为 `uint32_t`+分段（进度计数、writer u64） |
| V13 | 真实平台 port：Zephyr/NimBLE、ESP-IDF、BlueZ 三份可编译示例 + HIL |
| V14 | CMake/Makefile/pkg-config + 单文件 amalgamation + WASM 模拟器 |
| V15 | MISRA-C 合规说明与静态分析基线（clang-tidy/cppcheck） |
| V16 | `resources/templates/list`、`completion/complete`、sampling 完整语义（含 `modelPreferences`） |
| V17 | 文档体系：doxygen API 参考、`UPGRADING.md`、协议草案从公开头文件自动生成（防漂移） |

---

## 2. 关键设计草案

### 2.1 V1：FCS16 差分的结构

现在 oracle 没有 FCS 概念。最小改动：

```
oracle: 加 fcs16 开关（env BT_MCP_FCS16=1 或 argv）
  - frame_seq / encode: 在 hdr 之后插入 crc16(hdr+payload) 两字节
  - feed_packet / feed_stream: 解析时先取 2B 校验并验证
差分 harness: 跑组合
  (mock_fcs0, oracle_fcs0) / (mock_fcs1, oracle_fcs1)
禁止跨组合比对（不同配置的线上字节本就不同）
```

**为什么必须成对而非交叉**：FCS 是编译期开关，两边必须同配置才能逐字节比对；
交叉组合（C 开 / Rust 关）没有意义。

### 2.2 V2：golden transcript 的格式

```json
{
  "version": "2025-11-25",
  "transport": "packet/mtu=244/reliable=0",
  "steps": [
    {"dir": "c2s", "json": {"jsonrpc":"2.0","id":1,"method":"initialize","params":{...}}},
    {"dir": "s2c", "json": {"jsonrpc":"2.0","id":1,"result":{...}}}
  ]
}
```

回放器读取 golden，把 c2s 注入 C 侧 server，收集 s2c，与 golden 的 s2c 逐字段比对
（id 允许重编号以适配客户端分配的 id）。这样「规范符合性」就有了可回归的锚点，
而不是靠人读规范。

### 2.3 V4：重入不变量的实现

```c
#if BT_MCP_CFG_ASSERT_INVARIANTS
  extern int g_bt_mcp_call_depth;
  #define BT_MCP_ENTER() (++g_bt_mcp_call_depth)
  #define BT_MCP_LEAVE() (--g_bt_mcp_call_depth)
  #define BT_MCP_ASSERT_REENTRANT() do { if (g_bt_mcp_call_depth > 1) \
      bt_mcp_trace(BT_MCP_TRACE_WARN, "reentrancy", NULL, 0); } while (0)
#endif
```
在 `bt_mcp_server_rx`、`bt_mcp_session_tx`、`bt_mcp_session_feed` 入口调用。

**注意**：`session_tx` 在 `session_rx` 的回调内被调用是**合法**的（服务器应答），
所以不能一律断言；真正要防的是 `session_feed` 的重入（会覆盖 `feed_sink`）。
因此断言分级：`tx` 在 `rx` 回调内 → 允许；`feed` 重入 → 告警/abort。

### 2.4 V10：多会话的迁移路径

```
v0.4                      v0.5
bt_mcp_server_t           bt_mcp_server_t
  sess (1)                  sessions[N] { sess, initialized, calls[], subs[], log_level }
  calls[]                   （降为每会话）
  subs[]                    （降为每会话）
  log_level                 （降为每会话）
```
兼容策略：`sessions[0]` + 单会话 API 保持原签名，新增 `bt_mcp_server_add_session()`
返回 `bt_mcp_session_handle_t`。旧代码零改动，新能力按需启用。

---

## 3. 质量门槛（v0.5 完成的定义）

1. 差分矩阵覆盖 `FCS16 ∈ {0,1}` × `通道 ∈ {packet, stream}` × `MTU ∈ {23,53,244,517}`，
   每组 ≥300 随机脚本全绿。
2. golden 符合性套件覆盖全部已实现的 MCP 方法，纳入 CI。
3. 行覆盖率 ≥85%，`mcp_server.c` / `session.c` 分支覆盖率 ≥75%，CI fail-on-regression。
4. ASan/UBSan 全绿；FCS16 开启时同样全绿；fuzz 双模式各 ≥300 万轮零崩溃。
5. 有损通道（`test_lossy.c`）扩展到 6 种丢包/乱序组合，全部满足「恰好一次」。

## 4. 已知设计取舍（v0.5 仍会保留）

- **`idx` 为 u8**：单消息最多 255 片。ATT_MTU 23 下有效上限 3570 B。要突破需要
  把 idx 扩到 u16（帧头 +1 字节），会破坏 v0.3/v0.4 的线格式——**不做**，改为
  文档明确「大 payload 走 resources 分页或流通道」。
- **信用计数随 `link_up` 重置**：前提是「断链是双向事件」。多会话时代需要每会话
  独立的信用基准，V10 一并处理。
- **`stat_tx_dropped` 语义**：「停止重试」而非「确认丢失」。要精确需要在
  接收侧回「已收到」的终态确认，成本高于收益——保留现状并在文档标注。

## 5. 建议执行顺序

```
Week 1:  V1（FCS16 差分）+ V3（覆盖率门禁）      ← 先把「看不见的」变成「看得见的」
Week 2:  V2（golden）+ V4（重入断言）             ← 把「规范符合性」变成可回归资产
Week 3:  V5/V7（_meta、错误码）+ V6（批处理）
Week 4+: V8/V9（大消息、拥塞）→ V10（多会话）→ V11（共享缓冲）
```

理由：V1/V3 是**度量能力**，先有度量再谈优化；V2/V4 是**回归资产**，一次性投入
长期受益；V8–V11 是性能与规模，需要在度量到位后做才不至于盲改。
