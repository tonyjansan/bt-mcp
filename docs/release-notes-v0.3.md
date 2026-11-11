# BT-MCP v0.3.0 —— 互通与可靠性

> 一句话：**让协议核心能接真实 MCP Host，并且在会丢包的 GATT 上可用。**
> v0.2 的核心设计（零厂商 ifdef、零 OS 依赖、全静态）保持不变，补的是「能不能用」。

---

## 为什么发这一版

v0.2 把架构立住了，但有四个问题让它无法上真机：

1. **GATT 丢一片 = 丢整条消息**（没有重传），而 GATT notify 本来就会丢；
2. **信用窗口不闭环**，发满 4 条后永久 `E_AGAIN`，会话必然死锁；
3. **`initialize` 不符合 MCP 规范**（`serverName` 平铺、缺 `serverInfo` 与 `_meta`），
   标准 Host 握手直接失败；
4. **字符串 `request id` 被解析成 0**，且 `strtoll` 有越界读，请求-响应无法关联。

这四条都是「一票否决」级别，所以 v0.3 优先把它们连同 `resources/*`、
异步工具、RTT、安全加固一起做完了。

## 亮点

| 能力 | 说明 |
|---|---|
| 消息级可靠 | `ACK(0x06)` / `NACK(0x07)` + 重传槽 + 超时整条重传 + 指纹去重；NACK 只补缺失的那一片 |
| 信用闭环 | 消费即回补，载荷改为**累计已消费条数**，单帧丢失可自愈；应用不再手工发 CREDIT |
| 标准 MCP 握手 | `protocolVersion` 协商回显 + 嵌套 `serverInfo{}` + `_meta.io.btmcp.chan` 通道声明 |
| 资源面 | `resources/list` / `resources/read` + 游标分页，静态切片零分配 |
| 异步工具 | `E_PENDING` + `server_complete()` + `notifications/progress` + `notifications/cancelled` |
| Bridge 客户端 | 新增 `mcp_client`：主动握手、请求表、id 匹配、超时上抛——同一份 core 可跑在嵌入式网关 |
| 内存档位 | `-DBT_MCP_TIER=...`：4048 B / 11760 B / 42480 B（实测），协议字节不变 |
| 安全 | RESUME 连续失败 ≥3 次指数退避锁定；token 常数时间比较 |

## 破坏性变更与迁移

这一版**不兼容 v0.2 的 C API**（协议层向后兼容，API 层不兼容）。

```c
/* 1) session_rx 增加 seq 参数 */
- bt_mcp_session_rx(sess, kind, payload, len, sink, user);
+ bt_mcp_session_rx(sess, kind, seq, payload, len, sink, user);

/* 2) 上抛 sink 增加 seq */
- void sink(void *user, uint8_t kind, const uint8_t *p, uint16_t n);
+ void sink(void *user, uint8_t kind, uint8_t seq, const uint8_t *p, uint16_t n);

/* 3) 工具回调增加 call 上下文（异步/进度/取消都从这里走） */
- int tool(const bt_mcp_json_req_t *req, bt_mcp_writer_t *out, void *user);
+ int tool(const bt_mcp_json_req_t *req, bt_mcp_writer_t *out,
+          bt_mcp_call_t *call, void *user);

/* 4) 资源表从三字段扩到六字段 */
- static const bt_mcp_resource_def_t R[] = { { "log", "device log", BT_MCP_AUTHZ_OPEN } };
+ static int res_read(void *u, uint32_t cur, const char **t, uint32_t *n, uint32_t *next);
+ static const bt_mcp_resource_def_t R[] = {
+     { "log", "device log", "text/plain", BT_MCP_AUTHZ_OPEN, res_read, NULL },
+ };

/* 5) session cfg 请改用指定初始化器（尾部新增 retry_max / ack_tout_ms） */
- bt_mcp_session_cfg_t cfg = { 30000, 5000, 4, role };
+ bt_mcp_session_cfg_t cfg = { .req_timeout_ms = 30000, .resume_tout_ms = 5000,
+                              .tx_window = 4, .role = BT_MCP_ROLE_SERVER };
```

**port 侧必改一处**：GATT 通道必须声明 `cap.reliable = 0`（v0.2 的示例写的是 1），
否则 L4 不会启用重传，丢包问题依旧。

**port 侧可简化一处**：framer 现在每个分片只回调一次 `emit`（帧头+载荷已拼好），
v0.2 文档里「emit 两次、port 自己聚合」的写法可以删掉。

## 验证

一条命令 `./build.sh` 跑完全部套件：

| 套件 | 结果 |
|---|---|
| 差分（C ↔ 独立 Rust oracle） | **1339** 项全绿：帧 405 / 会话 312 / MCP 313 / 全栈 309 |
| Port HIL（GATT / SPP / CoC 背靠背） | **198** 项全绿 |
| 单元与定向 | **169** 项：核心 63 + 可靠性 32 + FCS16 51 + Bridge 客户端 23 |
| ASan + UBSan 差分 | 全绿（`./build.sh 150 asan`） |
| fuzz | 默认模式 300 万轮、FCS16 模式 20 万轮，零崩溃 |

差分测试是这一版的工程质量底座：C 与**独立实现**的 Rust oracle 跑同一批随机脚本，
输出必须逐字节相同。协议可见的每次改动都在两侧同步，所以「FCS16 线序写反」
这类问题一旦进入矩阵就会被立刻抓住——它之所以曾经漏掉，是因为 FCS16 当时完全
不在覆盖范围内（现已补 51 项往返测试 + fuzz）。

## 实测开销

| 场景 | 结果 |
|---|---|
| 512 B 消息，MTU 244 | 3 片 / 530 B，放大 **1.04×** |
| 512 B 消息，MTU 23 | 31 片 / 698 B，放大 **1.36×** |
| 流通道（SPP/UART） | 整条消息单帧，固定 **+6 B** |
| 不可靠包通道控制帧 | 消费 128 B + 发 512 B 时，控制帧 2 帧 / 17 B，占 **3.1%** |

结论：**MTU 是首要旋钮**。GATT port 应尽早做 MTU 交换；做不到就考虑用流通道
承载大 payload 类工具。

## 已知限制（显式记录，非静默降级）

1. 大于 `BT_MCP_CFG_TX_RETRY_BYTES`（默认 512 B）的消息在不可靠包通道上不享重传保护。
2. `idx` 是 u8 → 单消息最多 255 片；ATT_MTU 23（chunk 14 B）下有效上限 3570 B，
   小于 FULL 档 8 KB 的声明值。
3. 信用计数基准随 `link_up` 重置，前提是「断链是双向事件」；port 若单边重连会以
   `E_NOMEM` 背压，不会崩溃但属 port 违例。
4. FCS16 只有 C 侧往返测试与 fuzz，尚未进入 C↔Rust 差分矩阵。
5. 真实芯片移植（Zephyr/NimBLE）与手机 Bridge 仍是待办。

## 升级步骤

```sh
git pull                     # 或在你的分支上 merge v0.3
./build.sh                   # 全绿后再改自己的 port
```

然后按上面的迁移清单改 port 与工具/资源注册代码，重点确认：
`cap.reliable = 0`、`session_rx` 传 `seq`、工具回调接 `bt_mcp_call_t *`。

## 提交清单

```
811465e P0 全部修复：信用闭环、包通道 ACK/NACK 重传+去重、MCP 标准互通、字符串 id
fade35b P2/P3/P4 + 扩展：json 加固、RESUME 防爆破、内存档位、CoC port、Bridge 客户端、CI、文档
2df61d5 可靠性定向测试 + 开销基线 + 文档数字校准
92a964f MAX_FRAG 修正 + 配置合法性编译期断言
451b6d6 独立审查发现的 8 类缺陷（信用死锁 / FCS16 / 流卡死 / 去重 / 握手误判等）
c32cd2d fuzz 的 FCS16 死循环修复；取消后不再发进度通知
efecde0 v0.3 变更与验证报告
4c24e84 核心单测在 FCS16 模式下同样通过
```

完整细节见 [`CHANGELOG.md`](../CHANGELOG.md) 与
[`docs/v0.3-validation.md`](v0.3-validation.md)。

---

# 附：PR 描述（可直接粘贴）

## 标题

`feat(v0.3): 互通与可靠性——包通道 ARQ、信用闭环、标准 MCP 握手、resources/异步工具`

## 摘要

把 bt-mcp 从「架构成立但不可用」推进到「能接真实 MCP Host、能在丢包的 GATT 上跑」。
修复 4 个一票否决问题（丢包不可恢复、信用不闭环死锁、`initialize` 不符合规范、
字符串 id 解析错误），并补齐 `resources/*`、异步工具 + 进度/取消、PING RTT、
RESUME 防爆破、内存档位、CoC port、Bridge 客户端、CI 与文档。

## 动机

v0.2 的协议设计是自洽的，但对照 MCP 规范与真实 BLE 链路逐条核对后，发现四处
会让它在真机上直接失败：GATT 丢包无恢复、信用窗口单调泄漏至死锁、`initialize`
响应缺 `serverInfo`/`_meta`、字符串 id 无法关联。这四条不修，后面所有功能都无处落地。

## 改动要点

- **L3/L4 可靠性**：缺片保持槽 + NACK 登记；L4 重传槽环 + ACK/NACK + 超时整条重传 +
  `seq+len+CRC32` 指纹去重（含代次护栏）；信用改累计语义并闭环。
- **L5 互通**：标准 `initialize`、`tools/list`/`resources/list` 分页、
  `resources/read` 游标、异步 `tools/call` + progress/cancel、字符串 id 回显。
- **端口/扩展**：CoC port、`session_feed()`、Bridge 客户端、`tier.h`、`bench.c`。
- **工程质量**：CI（差分/HIL/单测/档位 + ASan/UBSan + 双模式 fuzz + 周级长跑）、
  编译期配置断言、FCS16 首次纳入测试矩阵。

## 测试方式

```sh
./build.sh            # 1339 差分 + 198 HIL + 169 单元，全绿
./build.sh 150 asan   # ASan/UBSan 差分
```

关键回归（新增）：
- `tests/test_reliability.c`：丢片补发 / 迟到重片忽略 / 整条去重 / 超时重传与放弃 /
  ACK 释放槽 / 信用自愈与退信 / 发送失败不扣信用。
- `tests/test_fcs16.c`：包+流、小帧+大帧、坏帧拒绝、坏帧后自恢复（此前该模式零覆盖）。
- `tests/test_client.c`：Bridge 客户端全流程 + 握手 id 匹配 + 超时。

## 风险与影响面

- **API 破坏性变更**（见迁移清单）：`session_rx` 与 sink 增加 `seq`、工具回调增加
  `call`、资源表扩字段、session cfg 尾部新增字段。仓内 mock/oracle/harness/测试已全部迁移。
- **port 必改**：GATT 的 `cap.reliable` 必须为 0，否则仍在旧行为（不重传）。
- 协议层对 v0.2 向后兼容；新增控制帧只在 `reliable == 0` 的通道出现。
- 已知限制已在 `docs/v0.3-validation.md` 显式记录，未做静默降级。

## 检查清单

- [x] 差分 C↔Rust 全绿（1339）
- [x] Port HIL 全绿（198，含 CoC）
- [x] 单元/定向全绿（169）
- [x] ASan+UBSan 全绿
- [x] fuzz 默认 + FCS16 双模式零崩溃
- [x] 文档更新（草案 v0.3 / README / 接口规约 / CHANGELOG / 验证报告）
- [x] CI 可复现
