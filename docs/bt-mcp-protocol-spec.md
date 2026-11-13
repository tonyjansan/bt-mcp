# BT-MCP 协议规范（wire protocol + 语义）

日期：2026-11-12 · 状态：**规范（normative）**。本文件是给**第三方实现者**看的：
只描述「线上长什么样、语义怎么定」，不涉及本仓的 C 实现结构。

> 实现说明（C API 骨架、内存档位、配置项、port 清单、验证体系）见
> [`bt-mcp-core-draft.md`](bt-mcp-core-draft.md)。两份文件的**规范部分以本文件为准**。


## 3. 通道抽象与能力协商

core/ 对通道的全部知识收敛为一个能力描述符，由 port 在连接建立时填写：

```c
/* bt_mcp/capability.h —— core 对通道的全部假设仅此一个结构体 */
typedef enum {
    BT_MCP_CHAN_PACKET = 0,   /* 包语义：有 MTU 上限，帧可能不达（队列满/链路丢），
                                 无跨包边界保证（GATT notify、L2CAP CoC） */
    BT_MCP_CHAN_STREAM = 1    /* 流语义：字节流有序可靠（SPP、RFCOMM、USB-CDC、UART） */
} bt_mcp_chan_kind_t;

typedef struct bt_mcp_chan_cap {
    bt_mcp_chan_kind_t kind;
    uint16_t mtu;             /* 单次 send() 的 payload 上限；流通道填 0 表示无限 */
    uint8_t  reliable : 1;    /* 1 = 送达有序可靠（流通道恒 1；CoC credit 下视为 1） */
    uint8_t  flow_ctrl : 1;   /* 1 = 通道自带流控（CoC credit / RFCOMM credit），
                                 0 = 需 L4 应用层信用 */
    uint8_t  max_pending : 4; /* 建议同时在途消息数（包通道=重组缓冲槽数） */
} bt_mcp_chan_cap_t;
```

各 Profile 的 port 填写示例：

| Profile | kind | mtu | reliable | flow_ctrl | 说明 |
|---|---|---|---|---|---|
| GATT notify/write | PACKET | 协商 ATT_MTU−3（未知则 20 保守值） | 0 | 0 | 需 L3 分片 + L4 信用 |
| GATT+LE CoC | PACKET | CoC MTU | 1 | 1 | 免信用，仅保留分片（MTU 可能仍 < 消息） |
| SPP / RFCOMM | STREAM | 0（无界） | 1 | 1 | 帧直连发，序号仅作同步 |
| USB-CDC / UART（调试与网关） | STREAM | 0 | 1* | 0 | *外部干扰仍可能，可选开校验 |

协议版本/通道能力在 **MCP initialize 握手内**的 `_meta` 中互相声明（不占用额外空中往返）：

```json
{ "capabilities": { "tools": {...} },
  "_meta": { "io.btmcp/chan": { "kind": 0, "mtu": 244, "window": 4 } } }
```

设备端以本地通道能力如实声明；两端各自按 min(自身, 对端) 收敛行为。任何一端不认识 `_meta.io.btmcp/*` 时按裸 MCP 语义退化（帧层照常工作，只是对端需按默认窗口发）。

## 4. 分帧格式（L3）

### 4.1 统一帧头（wire format，小端）

```
偏移  长度  字段        说明
0     1    magic      0xB7（与常见 BLE 载荷/TLV 首字节冲突概率低）
1     1    flags      bit0: FRAG_LAST（本消息最后一片）
                      bit1: CHUNKED（分片消息；流通道单帧直发时=0）
                      bit2: CTRL（控制帧：信用/心跳/resume，非 MCP 消息）
2     1    seq        发送方递增序号（mod 256）；重传/去重/流对齐用
3     1    idx        分片号（0..n-1，CHUNKED=0 时须为 0）
4     2    len        本片载荷长度（小端 u16，不含帧头）
6     n    payload    len 字节
```

- 头部定长 6 字节 + ≤255 字节载荷/片；片大小 = min(通道 MTU, 261) − 6。
- **流通道同步**：magic + len 前缀天然自同步；接收端失步时扫到 magic 且 `len ≤ 255`、校验通过即恢复（流通道多付 6B/片的代价换来与包通道共用编解码器）。
- **消息上限**：idx 8 位 → 最多 256 片 × 255B ≈ 65KB；v0.2 规定**单 MCP 消息 ≤ 8KB**（防止对端 RAM 勒索），更大 payload 一律走 resources 分页 URI（§5.3），违反上限回 `-32602`。
- 无 CRC：蓝牙链路层（GATT over LE、RFCOMM/CoC L2CAP FCS）已含完整性校验；流通道失步检测由 magic+len 语法校验承担。若 port 声明通道无链路级校验（自定义 UART），编译期可开 `BT_MCP_CFG_FCS16`。
- **FCS16 线序（v0.4 修正）**：开启后为 `hdr(6) | FCS16(2, 小端) | payload`，
  覆盖 `hdr + payload`（CRC16-CCITT，初值 0xFFFF，多项式 0x1021）。片大小相应
  减 2：`chunk = min(MTU,261) − 6 − 2`。**v0.3 的「发出/解析顺序不一致」曾让
  该配置静默失效数月**，现由差分矩阵的 FCS16 配对（§10）守住。
- **MTU 热更新（v0.4）**：`bt_mcp_framer_set_mtu(f, mtu)` 立即生效于后续分片，
  传 0 回退 `cap->mtu`。BLE 连接后 ATT MTU 从 23 升到 247 时无需重建 framer；
  已发出的帧不受影响（接收侧按其头部的 `len` 解析）。流通道无 MTU 概念，调用返回 `E_ARGS`。

### 4.2 控制帧（flags.CTRL=1）

控制帧与数据帧共用头，payload 为 1 字节类型 + 参数，走任一通道方向：

| type | 名称 | payload | 用途 |
|---|---|---|---|
| 0x01 | CREDIT | u16 credits | L4 应用层信用补给（包通道用；饱和到 255） |
| 0x02 | PING | u8 epoch | 应用层存活探针 + RTT 测量（epoch 回显匹配） |
| 0x03 | RESUME_REQ | u32 token | 重连后请求恢复会话 |
| 0x04 | RESUME_OK | u32 token | 同意恢复（回相同 token） |
| 0x05 | RESUME_REJ | u8 reason | 拒绝：1=无 token 2=不匹配 3=锁定中 |
| 0x06 | ACK | u8 seq | 消息级确认：释放对端重传槽（v0.3） |
| 0x07 | NACK | u8 seq \| u8 count \| u8 idx[count] | 缺片补发请求：对端**只重发列出的这些片**（v0.9.1 扩展；`count==1` 时与旧格式 `u8 seq \| u8 idx` 字节一致） |

控制帧不解 JSON、不进 L5，在 L4 终结；这是唯一绕过 MCP 语义的旁路，用于「还没握手/已断语义」两种 MCP 管不到的状态。

### 4.3 重组与去重

- 包通道：按 `seq` 匹配重组槽，槽数 = 通道能力 `max_pending`（编译期上限
  `BT_MCP_CFG_MAX_REASM`，默认 4）。v0.3 判例：`idx < next_idx` 的迟到重复片
  静默忽略；`idx > next_idx` 视为丢片，**槽位保持存活**并登记 NACK，等对端
  单片补发（同一槽连续 4 次补不齐才弃槽，防对端永久失联占死缓冲）。
- 流通道：帧边界即消息边界（CHUNKED=0），重组器退化为「6B 头 + len 字节」的流解析器，同一份状态机代码、不同喂入路径。
- 重组超时：跨片间隔 > `BT_MCP_CFG_FRAG_TOUT_MS`（默认 2000ms）丢弃整消息并回收槽位（防半帧占死缓冲）。
- **共享重组缓冲（v0.5）**：`BT_MCP_CFG_SHARED_REASM=1` 时强制 `MAX_REASM=1`、
  共用一份 `MAX_MSG` 缓冲，framer 从 41072B 降到 16448B（-60%）。
  **代价**：只允许一条**分片**消息在重组中；对端交织发送多条分片消息时，
  后到的按「槽已占用」丢弃（协议允许，吞吐下降）。单片消息不经过重组槽，
  不受此限制。

### 4.5 JSON 解析契约（v0.5 收紧）

L5 用自带解析器（零分配、无递归爆栈风险），契约如下：

| 输入 | 行为 |
|---|---|
| 合法对象（允许尾随空白） | 解析成功 |
| 尾随垃圾（`{"a":1}xyz`） | **拒绝**（帧长度已界定边界，尾随字节即报文损坏） |
| 尾随逗号（`{"a":1,}`） | **拒绝**（严格 JSON） |
| 顶层数组 | 走 batch 分支（§5.3），不当作单条消息 |
| 嵌套深度 > `BT_MCP_CFG_JSON_MAX_DEPTH` | 拒绝（防深嵌套栈爆） |
| 超长键名 | 跳过不匹配（不视为语法错，判例 4） |
| 孤立 UTF-16 代理（`\ud83d`） | 容忍（不视为语法错） |

取值器（`get_str/u32/i32/f64/raw/top_raw/get_sub`）对**类型不匹配**与
**空指针输出参数**一律返回 `E_ARGS`/`E_PARSE`，绝不写入。v0.5 之前传 NULL
会直接崩溃。

## 5. 会话与 MCP 语义

### 5.1 状态机（L4）

```
IDLE ──连接建立(chan_cap)──▶ LINK_UP ──initialize──▶ READY
 ▲                                │                     │
 │                     RESUME_REQ │  RESUME_OK          │ 正常 MCP 流量
 └───────────── 断链/超时 ────────▼──────────────────────┘
```

- READY 前只允许控制帧与 MCP initialize；READY 后只允许 MCP + CREDIT/PING。
- 断链重连：先试 RESUME（1 往返，恢复 pending 请求映射与授权缓存），被拒或无 token 走完整握手。RESUME token 绑定 bond（见 §6），bond 丢失则 token 一并作废。
- 包通道信用：对端重组槽数即我方发送窗口；**每消费对端一条 MCP 消息自动回补 1
  credit**（饱和到 255），不再依赖应用层手工调用 `session_ctrl(CREDIT)`。
  流通道或 `flow_ctrl=1` 的通道（CoC）直接放行，不记账。

  v0.3 判例（信用必须「不会泄漏、不会丢帧」）：
  - **CREDIT 载荷是「累计已消费条数」而非增量**：单个 CREDIT 帧丢失会被下一个
    自动补偿（增量语义下丢一帧就永久少一格）；重复 CREDIT 也不重复加credit。
    计数器是 u16，用回绕减法算增量，65535→0 处正确。
  - 发送失败不扣信用（消息没上线就不该占窗口）；重传次数耗尽而放弃时**退还**
    该消息占的信用，否则窗口单调泄漏、最终 `E_AGAIN` 永久死锁。
  - 握手期（LINK_UP）同样回补/ACK——`initialize` 响应也占对端窗口，漏补会让
    会话少一格。
  - 计数基准（`consumed`/`peer_consumed`）随 `link_up` 重置，前提是**断链是
    链路层双向事件**（BLE/RFCOMM 断开时两端都会收到 `link_down`）。若 port 在
    单边重连时不调用 `link_down`，两边基准会错位——此时信用最多饱和到 255，
    接收侧以 `E_NOMEM` 丢帧背压，不会崩溃或溢出，但属于 port 违例。

### 5.2 不可靠包通道的消息级可靠（v0.3 新增）

`cap.reliable == 0`（GATT notify 即此类）时 L4 启用：

| 机制 | 触发 | 行为 |
|---|---|---|
| 发送重传槽 | `session_tx` 成功发出 | 消息体（≤ `BT_MCP_CFG_TX_RETRY_BYTES`）入槽环，记录 seq |
| ACK | 对端消费该消息 | 回 `ACK(seq)`；本地按 seq 释放槽 |
| NACK 补片 | 本地重组发现缺片 | 发 `NACK(seq, count, idx[count])`；对端**只重发这些片**（v0.9.1：一次可报多个，见 §4.2） |
| 超时整条重传 | `ack_tout_ms` 内未收到 ACK | 用**原 seq** 重发整条，最多 `retry_max` 次，之后弃槽并计 `stat_tx_dropped` |
| 接收去重 | 收到已交付过的消息 | `seq + len + CRC32` 指纹窗口（默认 5s / `RX_FP_SLOTS`=8 条），命中则只回 ACK、不再上抛 |

**普通 `tx` 只保护「放得下」的消息**（默认 512B/槽 × 2 槽）；更大的消息按 §5.5
走 resources 分页，或由应用层兜底重试。`BT_MCP_CFG_TX_SLOTS` /
`BT_MCP_CFG_TX_RETRY_BYTES` 可调。

**大消息保护：`tx_ref`（v0.5）**——`bt_mcp_session_tx_ref(s, json, len)` 不复制
原文，只在槽里记 `(seq, len, 指向调用方缓冲的指针)`，重传时从该指针重新编码，
因此可覆盖到 `MAX_MSG`。**契约**：调用方必须在 ACK 或放弃之前保证该缓冲有效；
槽释放（ACK / 超限丢弃 / NACK 完成）时一律清空引用，避免悬空指针。

**去重的边界（v0.6 起逐步校准，重要）**：
「恰好一次」是**有界保证**，不是无条件保证。去重表只有固定个槽，且带
`rx_msg_no` 记代护栏；一旦副本的到达延迟超过窗口深度与时间窗，原指纹已被
挤出，该副本会被当作新消息二次上抛。

**v0.7 的根因修正**：时间窗 `DEDUPE_MS` 必须覆盖发送侧的**完整重传寿命**
（指数退避下 ≈ `ACK_TOUT × (2^retry_max − 1)`），否则迟到副本必然落在窗外。
默认已从 5s 提到 20s。同时发送侧加 **stale 守卫**：重传前若该消息之后已发出
≥ `RX_FP_SLOTS` 条新消息（对端指纹必然已被挤出），则**放弃重传**而不是制造
重复。实测 8 个链路场景重复全零。

**v0.9.1 的重组模型**：接收侧改为**乱序缓存 + 缺片位图**——乱序片按
`idx × chunk` 落位并置位图，只对真正缺失的片发 NACK。这要求**同一条消息内
chunk 稳定**，因此 MTU 在消息中途变化会弃掉在途重组槽（见 §4.3）。

v0.6 的链路仿真 soak 实测（`tests/test_soak.c`）：

| 场景 | 重复投递 | 说明 |
|---|---|---|
| 干净链路 | 0 | — |
| 10% 丢包（窗口 4） | 6 / 120 条 | v0.6 之前 |
| 10% 丢包（窗口 8） | 1 / 120 条 | 加宽后大幅收敛 |
| 30%~50% 丢包 + 突发 | 2~5 / 120 条 | **仍有缺口**，见 roadmap N6 遗留项 |

两种正确用法：(a) 把窗口调到 ≥ 链路最大乱序/延迟深度；(b) 要求幂等的工具
由应用层按自己的业务键去重。**协议不会静默假装零重复**——
`stat_rx_dup` 统计被拦下的重复，`stat_tx_dropped` 统计「放弃重试」的次数。

**v0.7 的两处可靠性语义修正**（都由 soak 实测驱动）：

| 项 | v0.6 | v0.7 | 原因 |
|---|---|---|---|
| `DEDUPE_MS` | 5000ms | **20000ms** | 去重时间窗必须覆盖发送侧**完整重传寿命**（指数退避 600×(2³−1)=4200ms + 末次在途）。否则迟到副本落在窗外 → 被当新消息二次上抛 |
| NACK 补片 | 只补 `next_idx` 一片 | **从缺片尾部重放**（上限 16 片/次，预算 分片数×16） | 严格按序重组下，缺口之后的分片均白丢；只补一片要 N 轮，而 `retry_max=3` 早已耗尽 → 小 MTU 交付率仅 23%，改后 57% |

**发送侧 stale 守卫**：重传前若该消息之后已发出 ≥ `RX_FP_SLOTS` 条新消息，
说明对端指纹必然已被挤出，此时**放弃重传**（计入 `stat_tx_stale_drop`），
而不是发一个对端认不出、会二次上抛的副本。

**已知遗留**：尾部重放会重发已到达的分片，本质是浪费。根因修法是接收侧
乱序缓存 + 缺片位图（只补真正缺的），见 `roadmap-v0.7.md` 的 A2 遗留项。

**空闲信用刷新（v0.6）**——累计 CREDIT 帧不受 ARQ 保护，丢一个就少一格信用；
若此时发送方信用耗尽且管道已空，对端没有「新的消费」可用来重发 CREDIT，
于是**永久 `E_AGAIN`**（v0.6 soak 实测：30% 丢包下整个会话卡死）。
修复：`session_tick` 在空闲超过 `BT_MCP_CFG_CREDIT_REFRESH_MS`（默认 600ms）
且本周期无实际流量时，重发一次累计 CREDIT。载荷是绝对值，重发幂等。

**拥塞退避（v0.5）**——ACK 超时不再固定，而是会话级 RTO：

| 事件 | 行为 |
|---|---|
| 超时重传 | `rto = min(rto × 2, BT_MCP_CFG_RTO_MAX_MS)`（默认上限 4800ms） |
| 收到任何合法 ACK | `rto` 立即回落基准（`ack_tout_ms`） |
| NACK 定点补片 | **不**退避（对端明确告知缺哪片，不是拥塞信号） |

默认态 `rto == 基准`，因此**无丢包时行为与 v0.4 逐字节一致**（既有差分不受影响）。
回落不要求槽仍存活：ACK 证明链路可用，即使该消息已因超过 `retry_max` 被放弃，
拥塞状态也应解除——否则一次严重丢包会把 RTO 永久顶在上限。

### 5.3 MCP 方法面（线上与标准 MCP 完全同形）

**已实现（v0.5）：**

| 类别 | 方法 |
|---|---|
| 握手 | `initialize`、`notifications/initialized`、`ping` |
| 工具 | `tools/list`（分页）、`tools/call`（同步/异步/富内容/`_meta`） |
| 资源 | `resources/list`（分页）、`resources/read`（游标分页）、`resources/subscribe`、`resources/unsubscribe` |
| prompts | `prompts/list`（分页）、`prompts/get` |
| 日志 | `logging/setLevel` |
| 通知（下行） | `notifications/{tools,resources,prompts}/list_changed`、`notifications/resources/updated`、`notifications/message`、`notifications/progress`、`notifications/cancelled` |

**capabilities 如实声明**（`initialize` 响应）：`tools.listChanged=true`、
`resources.subscribe=true` + `resources.listChanged=true`、`prompts.listChanged=true`
（仅当注册了 prompt）、`logging={}`。未实现的能力一律不声明——规范允许。

**尚未实现**：`resources/templates/list`、`completion/complete`、
sampling/roots/elicitation 的**完整语义**（承载能力已有，见 §5.7）、OAuth。

**JSON-RPC batch 按协商版本分支（v0.5）**：batch 存在于 MCP `2025-03-26` 及更早，
`2025-06-18` 起被规范移除。因此行为取决于 `initialize` 协商结果：

| 协商版本 | 收到 `[...]` 时 |
|---|---|
| `2025-03-26` / `2024-11-05` / `2024-10-07` | 逐元素处理，非空响应聚合成 JSON 数组一次发出；全为通知则不回响应 |
| `2025-06-18` / `2025-11-25`（及未协商） | 回 `-32600 "Batch not supported"`（明确拒绝，避免 Host 干等） |

**错误码语义（v0.5 收敛，见 `BT_MCP_EC_*`）：**

| 码 | 语义 | 典型场景 |
|---|---|---|
| -32700 | Parse error | JSON 语法错（能定位 id 才回） |
| -32600 | Invalid request | 超长字符串 id、batch 不支持 |
| -32601 | Method not found | 未知方法 |
| -32602 | Invalid params | 参数缺失/类型错、未知工具、未知 prompt |
| -32603 | Internal error | 结果超限、在途调用槽耗尽、订阅失败 |
| **-32001** | **Not ready** | 未完成 `initialize` 就调用（**v0.5 从 -32002 改过来**） |
| -32002 | Resource not found | 未知资源 URI |
| -32003 | Permission denied | 授权拒绝（consent 未放行 / RESTRICTED） |

### 5.4 异步调用、进度、取消与 `_meta`（v0.3 起，v0.5 扩充）

- 工具回调可返回 `BT_MCP_E_PENDING`：L5 暂不回响应，调用槽（`bt_mcp_call_t`）
  保持有效，直到应用调用 `bt_mcp_server_complete(call, text, is_error)`。
- 请求带 `params._meta.progressToken`（数字或字符串）时，工具内可调
  `bt_mcp_call_progress()` / `bt_mcp_call_progress_msg()`，L5 发出
  `notifications/progress`（`progressToken/progress/total/message?`）。
  `message` 是规范可选字段（v0.5 补齐），便于 Host 直接展示。
- `notifications/cancelled`（`params.requestId`）按 id 标记调用；被取消的调用
  在 `complete()` 时**不发出任何字节**，工具可轮询 `bt_mcp_call_cancelled()` 提前收手。
- 同步路径不变：工具照常把文本写进 `out`，L5 负责转义并包成
  `{"content":[{"type":"text","text":…}],"isError":…}`（v0.3 修掉了旧的
  双层转义与缺引号问题）。

**富内容（v0.4）**：`content[]` 支持四种内容项，工具按需混排：

| 调用 | 产出 |
|---|---|
| `bt_mcp_w_raw(w, …)`（裸写） | 追加到**当前文本项**（没有就开一个）——v0.3 字节输出不变 |
| `bt_mcp_w_content_text(w, …)` | 关闭当前文本项并开新的 text 项 |
| `bt_mcp_w_content_image/audio(w, b64, len, mime)` | `{"type":"image"|"audio","data":…,"mimeType":…}` |
| `bt_mcp_w_content_resource(w, uri, mime, text, len)` | `{"type":"resource","resource":{uri,mimeType,text}}` |

**`_meta` 透传（v0.5）**：`_meta` 属于**结果对象**，不属于 `content[]`。
工具拿到的是 content 写入器，因此必须用 `bt_mcp_call_set_meta(call, "k":v 片段)`
声明，由 server 在 content 数组闭合之后输出：

```
{"jsonrpc":"2.0","id":N,"result":{
   "content":[…],"           ← 先闭合
   "_meta":{"io.btmcp/ver":"0.5"},
   "isError":false}}
```

直接对 content 写入器调 `bt_mcp_w_meta()` 会把 `_meta` 打成一个文本内容项
（v0.5 实现时踩过，差分当场抓出）。`bt_mcp_w_meta()` 只适用于**结果对象级**
writer，例如 `prompts/get` 的实现。

**补充：`ping` 的响应必须是空对象 `{}`**（v0.5 修正；原实现回 `{"pong":true}`
不符规范，由 golden 人工核对发现）。

### 5.5 资源与大 payload 分页约定

resource URI 自带窗口与游标：`bt-mcp://dev/<name>?cursor=<token>`，
`resources/read` 响应为
`{"contents":[{"uri","mimeType","text"}],"nextCursor"?}`；
`resources/list` 与 `tools/list` 用 `{"…":[…],"nextCursor"?}` 同形分页
（每页 `BT_MCP_CFG_PAGE_SIZE`，默认 8）。

资源读取由注册表里的 `read(user, cursor, &text, &len, &next_cursor)` 提供：
返回 `[cursor, next)` 窗口的**静态切片**（server 只读不拷贝，零分配），
`next_cursor == BT_MCP_CURSOR_END` 表示读完。分页让单消息上限固化在档位值
（4KB/2KB/8KB），任何档位 RAM 的设备都能实现完整资源面。

### 5.6 server→client 请求（v0.4）

MCP 是双向协议：`sampling/createMessage`、`roots/list`、`elicitation/*` 都由
**server 发起**。L5 提供承载能力：

```c
int bt_mcp_server_request(sv, method, params_json, params_len, cb, user);
int bt_mcp_server_inflight(sv);
```

- 在途槽数 `BT_MCP_CFG_MAX_S2C_REQ`（默认 2），满则 `E_BUSY`；
- 会话非 READY 时返回 `E_STATE`；
- 客户端响应经 `server_rx` 的「无 method 有 id」分支按 id 匹配到回调，
  回调收到 result 片段原文（失败时为 error 片段 + 非 0 rc）；
- 超时 `BT_MCP_CFG_S2C_TOUT_MS`（默认 30s）由 `bt_mcp_server_tick` 回收，
  超时回调 `rc = E_AGAIN`。

### 5.7 生命周期：MTU 热更新 / NVM / keepalive（v0.4）

| 能力 | API | 说明 |
|---|---|---|
| MTU 热更新 | `bt_mcp_framer_set_mtu()` | BLE MTU 交换后立即生效；0 = 回退到 `cap->mtu` |
| NVM 持久化 | `bt_mcp_session_set_storage/save_state/load_state/resume_token` | 快照含 magic+version 校验；未设钩子时 `save` 是空操作而非错误 |
| keepalive | `BT_MCP_CFG_KEEPALIVE_MS` / `IDLE_DEAD_MS` | **默认 0=关闭**，行为与关闭前一致；开启后 READY 态空闲超阈值主动 PING，判死阈值则按 `link_down` 收尾 |

持久化快照布局（`bt_mcp_session_state_t`，即落盘格式）：

```
u32 magic('BTM1') | u16 version(1) | u16 flags(bit0=已绑定) | u32 resume_token
```

## 6. 安全

1. 强制要求蓝牙配对加密（LE：Secure Connections；Classic：SSP MITM 模式），由 port 层在 LINK_UP 前核验，未加密通道拒绝进入握手（调试编译期可豁免）。
2. RESUME token 绑定 bond 标识（地址解析/链路密钥指纹），bond 删除即 token 失效；
   防「换手机静默继承会话」。v0.3 追加防爆破：连续失败 ≥3 次进入退避锁定
   （30s 起、逐次翻倍、上限 8×），锁定期间一律回 `RESUME_REJ(3)`；token 比较
   用常数时间实现，避免时序侧信道。port 侧应生成 ≥32bit 且来自 CSPRNG 的 token
   （强建议：`HMAC(bond_key, counter)` 截断到 32bit 或扩展为 64bit 载荷）。
3. 工具级授权：`open / consent / restricted` 三级，consent 弹窗确认在设备 UI（无 UI 设备则由 Bridge 端展示）；授权缓存随 session（resume 后失效，须重新确认 restricted 级）。
4. 不做 OAuth/token——会话生命周期 = 加密链路 + bond 生命周期。

## 9. 兼容性矩阵

| 兼容目标 | 本设计的支撑点 |
|---|---|
| 任意 MCU（8/16/32 位） | C99、无堆、无 OS 原语、整型定宽、定长结构体 |
| 任意蓝牙栈 | 栈接触面收敛在 4 个 port（§8） |
| 任意 Profile | 能力描述符自适应（分片/信用按 cap 收敛，流通道自动跳过） |
| 任意 MCP Host | 线上 = 标准 JSON-RPC MCP（Bridge 转成 stdio/HTTP 对上） |
| 存量 MCP 规范版本 | 锚定 2025-11-25 有状态语义；无状态版（2026-07-28）作为 v2 适配层，握手面已收敛在 mcp_server.c 单点 |
| 协议版本协商 | `initialize` 回显对端版本；未知版本回退本端默认。版本差异被**显式分支**（batch 见 §5.3），不做隐式兼容 |
| 第三方 Host 实现 | 已用官方 MCP TypeScript SDK 1.32.2 作 Host 跑通 38 项校验（`tests/sdk_interop/`），覆盖 initialize / tools / resources / prompts / logging / ping |
| 工具链 | Clang/GCC C99、`-Wall -Wextra -Werror` 零告警；ASan/UBSan/TSan 跑通 |
