# BT-MCP 核心协议草案 v0.3 —— 平台无关设计

日期：2026-11-10 · 状态：草案（v0.3 实现已验证：1339 项差分 + 198 项 HIL + 169 项单元检查全绿）
本版相对 v0.2 的关键变化：
- **不可靠包通道从「不可用」变为「可用」**：新增 ACK/NACK 控制帧、发送重传槽、
  接收去重窗口，丢片可在本层恢复（§4.2/§5.4）；
- **信用闭环**：消费即回补，不再依赖应用层手工 `session_ctrl(CREDIT)`（§5.3）；
- **MCP 互通补齐**：`protocolVersion` 协商回显、`serverInfo` 嵌套对象、
  `_meta.io.btmcp` 通道能力声明真正上线（§5.2/§5.5）；
- **resources 面落地**：`resources/list` / `resources/read` + 游标分页（§5.5）；
- **异步与取消**：`tools/call` 支持 `E_PENDING` + `notifications/progress` +
  `notifications/cancelled`（§5.6）；
- **RESUME 防爆破**：连续失败指数退避锁定、token 常数时间比较（§6.2）；
- **内存档位**：`BT_MCP_TIER` 一处开关把静态 RAM 从 4.0KB 调到 41KB（§7.3）；
- **单次 emit**：一个分片 = 一次 port 回调，消除「头/载荷两次回调」的集成坑（§4.4）。

---

## 1. 设计原则

1. **协议核心不感知蓝牙**：core/ 只看到「带 MTU 语义的发送通道 + 字节/包回调 + 连接状态事件」。GATT、SPP、CoC、甚至 USB-CDC/UART 都是同一抽象的一个 port。
2. **两类通道，一套帧格式**：
   - **包通道**（GATT notify/write、L2CAP CoC）：有 MTU、可能丢包/无跨包边界保证 → 帧层带序号与分片重组；
   - **流通道**（SPP/RFCOMM、UART、USB-CDC）：字节流有序可靠 → 同一帧格式直接连发，序号退化为对齐/丢弃用途。
   两种通道**共用同一帧格式**（流通道多付 4 字节/帧头的成本，换取实现唯一性与互操作性）。
3. **最大平台兼容**：core/ 为纯 C99、无 OS 调用、无芯片头文件、无动态分配（编译期可切静态池）、无 `#ifdef <vendor>`。RTOS 与裸机（主循环轮询 `*_tick()`）均可驱动。
4. **MCP 语义严格子集**：线上字节与标准 MCP（JSON-RPC 2.0）互通，裁剪面在 §5。
5. **不锚定任何手机 OS**：手机/网关侧 Bridge 是配套件而非协议一部分；协议只定义设备端与 Bridge 之间的空中字节。

## 2. 层次与模块总览

```
L6  provider      设备业务（传感器/工具/资源表，编译期注册）
L5  mcp_server    JSON-RPC 编解码、方法分派、能力声明、超时/取消/进度
L4  session       会话状态机、请求-响应关联、信用流控、断链恢复
L3  frame         分帧/分片/重组、序号去重、通道类型自适应
L2  transport      通道抽象（port 实现：GATT / SPP / CoC / UART / host-mock）
L1  （任意蓝牙协议栈：vendor host、Zephyr、bluedroid…仅 port 层接触）
```

依赖方向严格单向向下：L6→L5→L4→L3→L2；反向通信只经注册的回调（下行数据、事件）。
**所有模块为非阻塞、事件驱动、单写者模型**：任意时刻只有一个执行上下文进入协议栈（裸机主循环或单一协议线程），port 层负责把 ISR/栈回调投递进该上下文（队列/信号量由 port 提供）。

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
- 无 CRC：蓝牙链路层（GATT over LE、RFCOMM/CoC L2CAP FCS）已含完整性校验；流通道失步检测由 magic+len 语法校验承担。若 port 声明通道无链路级校验（自定义 UART），编译期可开 `BT_MCP_CFG_FCS16` 追加 2 字节 CRC16-CCITT。

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
| 0x07 | NACK | u8 seq ⁝ u8 idx | 缺片补发请求：对端只重发该片（v0.3） |

控制帧不解 JSON、不进 L5，在 L4 终结；这是唯一绕过 MCP 语义的旁路，用于「还没握手/已断语义」两种 MCP 管不到的状态。

### 4.3 重组与去重

- 包通道：按 `seq` 匹配重组槽，槽数 = 通道能力 `max_pending`（编译期上限
  `BT_MCP_CFG_MAX_REASM`，默认 4）。v0.3 判例：`idx < next_idx` 的迟到重复片
  静默忽略；`idx > next_idx` 视为丢片，**槽位保持存活**并登记 NACK，等对端
  单片补发（同一槽连续 4 次补不齐才弃槽，防对端永久失联占死缓冲）。
- 流通道：帧边界即消息边界（CHUNKED=0），重组器退化为「6B 头 + len 字节」的流解析器，同一份状态机代码、不同喂入路径。
- 重组超时：跨片间隔 > `BT_MCP_CFG_FRAG_TOUT_MS`（默认 2000ms）丢弃整消息并回收槽位（防半帧占死缓冲）。

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

### 5.4 不可靠包通道的消息级可靠（v0.3 新增）

`cap.reliable == 0`（GATT notify 即此类）时 L4 启用：

| 机制 | 触发 | 行为 |
|---|---|---|
| 发送重传槽 | `session_tx` 成功发出 | 消息体（≤ `BT_MCP_CFG_TX_RETRY_BYTES`）入槽环，记录 seq |
| ACK | 对端消费该消息 | 回 `ACK(seq)`；本地按 seq 释放槽 |
| NACK 补片 | 本地重组发现缺片 | 发 `NACK(seq, idx)`；对端只重发第 idx 片 |
| 超时整条重传 | `ack_tout_ms` 内未收到 ACK | 用**原 seq** 重发整条，最多 `retry_max` 次，之后弃槽并计 `stat_tx_dropped` |
| 接收去重 | 收到已交付过的消息 | `seq + len + CRC32` 指纹窗口（默认 5s/2 条），命中则只回 ACK、不再上抛 |

槽只保护「放得下」的消息（默认 512B/槽 × 2 槽）；更大的消息按 §5.5 走 resources
分页，或由应用层兜底重试——这是显式的 RAM/可靠性权衡，不是静默降级。
`BT_MCP_CFG_TX_SLOTS` / `BT_MCP_CFG_TX_RETRY_BYTES` 可调。

### 5.2 MCP 子集（线上方法与标准 MCP 完全同形）

保留：`initialize`、`notifications/initialized`、`tools/list`、`tools/call`、
`resources/list`、`resources/read`、`notifications/resources/updated`、
`notifications/progress`、`notifications/cancelled`、`ping`。

裁剪（不声明 capabilities 即规范合法）：prompts / sampling / roots / elicitation /
completions / logging / batch / OAuth。

### 5.6 异步调用、进度与取消（v0.3 新增）

- 工具回调可返回 `BT_MCP_E_PENDING`：L5 暂不回响应，调用槽（`bt_mcp_call_t`）
  保持有效，直到应用调用 `bt_mcp_server_complete(call, text, is_error)`。
- 请求带 `params._meta.progressToken` 时，工具内可调 `bt_mcp_call_progress()`，
  L5 发出 `notifications/progress`（`progressToken/progress/total`）。
- `notifications/cancelled`（`params.requestId`）按 id 标记调用；被取消的调用
  在 `complete()` 时**不发出任何字节**，工具可轮询 `bt_mcp_call_cancelled()` 提前收手。
- 同步路径不变：工具照常把文本写进 `out`，L5 负责转义并包成
  `{"content":[{"type":"text","text":…}],"isError":…}`（v0.3 修掉了旧的
  双层转义与缺引号问题）。

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

## 6. 安全

1. 强制要求蓝牙配对加密（LE：Secure Connections；Classic：SSP MITM 模式），由 port 层在 LINK_UP 前核验，未加密通道拒绝进入握手（调试编译期可豁免）。
2. RESUME token 绑定 bond 标识（地址解析/链路密钥指纹），bond 删除即 token 失效；
   防「换手机静默继承会话」。v0.3 追加防爆破：连续失败 ≥3 次进入退避锁定
   （30s 起、逐次翻倍、上限 8×），锁定期间一律回 `RESUME_REJ(3)`；token 比较
   用常数时间实现，避免时序侧信道。port 侧应生成 ≥32bit 且来自 CSPRNG 的 token
   （强建议：`HMAC(bond_key, counter)` 截断到 32bit 或扩展为 64bit 载荷）。
3. 工具级授权：`open / consent / restricted` 三级，consent 弹窗确认在设备 UI（无 UI 设备则由 Bridge 端展示）；授权缓存随 session（resume 后失效，须重新确认 restricted 级）。
4. 不做 OAuth/token——会话生命周期 = 加密链路 + bond 生命周期。

## 7. C99 模块接口骨架

约定：所有类型以 `bt_mcp_` 前缀；所有函数非阻塞、返回 `int`（0 成功，负值 = `BT_MCP_E*` 错误码）；时间一律由调用方传入 `now_ms`（无 OS 时钟依赖）。

```c
/* ===== bt_mcp/err.h ===== */
enum {
    BT_MCP_OK = 0,
    BT_MCP_E_AGAIN  = -1,   /* 缓冲/窗口暂满，稍后 tick 重试 */
    BT_MCP_E_ARGS   = -2,
    BT_MCP_E_STATE  = -3,   /* 当前会话状态不接受此操作 */
    BT_MCP_E_TOOBIG = -4,   /* 超 8KB 消息上限或分片超限 */
    BT_MCP_E_NOMEM  = -5,   /* 静态池耗尽 */
    BT_MCP_E_SYNC  = -6     /* 流失步/重组校验失败 */
};

/* ===== bt_mcp/transport.h —— L2：port 层实现，core 只调用 ===== */
typedef struct bt_mcp_transport {
    /* 连接建立时由 port 填写，core 只读 */
    bt_mcp_chan_cap_t cap;

    /* 非阻塞发送一片/一帧；返回 BT_MCP_E_AGAIN 表示通道忙，core 挂起重试。
       stream 通道 len 无上限；packet 通道 len ≤ cap.mtu */
    int (*send)(void *io, const uint8_t *data, uint16_t len);

    /* 事件回调：port 在协议栈上下文内调用（port 负责线程/中断归一） */
    void (*on_rx)(void *io, const uint8_t *data, uint16_t len);   /* 完整片/帧字节 */
    void (*on_conn)(void *io, int up);   /* 1=连接建立(cap 已填), 0=断开 */
} bt_mcp_transport_t;

/* ===== bt_mcp/frame.h —— L3：纯协议，可宿主机 fuzz/golden ===== */
typedef void (*bt_mcp_msg_sink_t)(void *user, uint8_t kind,
                                  const uint8_t *payload, uint16_t len);
    /* kind: 0=MCP 消息(完整重组) 1=控制帧(原样上抛 L4) */

typedef struct {
    /* 编译期配置注入（替代 #ifdef） */
    uint8_t  max_reasm;      /* 重组槽数（包通道）；默认 4 */
    uint16_t frag_tout_ms;
} bt_mcp_frame_cfg_t;

typedef struct bt_mcp_framer bt_mcp_framer_t;   /* 不透明，大小编译期定长 */

int  bt_mcp_framer_init(bt_mcp_framer_t *f, const bt_mcp_frame_cfg_t *cfg,
                        const bt_mcp_chan_cap_t *cap);
/* 发送侧：整条消息 → 由 sink 按片吐出（core 把 sink 输出接 transport->send），
   自动编 seq/idx/CHUNKED；包通道按 cap.mtu 切片，流通道单帧直发 */
int  bt_mcp_framer_send(bt_mcp_framer_t *f, uint8_t kind,
                        const uint8_t *payload, uint32_t len,
                        int (*emit)(void*, const uint8_t*, uint16_t), void *io);
/* 接收侧：喂入原始字节（片/流皆可），重组完成经 msg_sink 上抛 */
int  bt_mcp_framer_feed(bt_mcp_framer_t *f, const uint8_t *in, uint16_t len,
                        bt_mcp_msg_sink_t sink, void *user);
/* 每次主循环/协议线程节拍调用：驱动重组超时回收 */
void bt_mcp_framer_tick(bt_mcp_framer_t *f, uint32_t now_ms);

/* ===== bt_mcp/session.h —— L4 ===== */
typedef enum {
    BT_MCP_S_IDLE = 0, BT_MCP_S_LINK_UP, BT_MCP_S_READY
} bt_mcp_state_t;

typedef struct bt_mcp_session_cfg {
    uint32_t req_timeout_ms;   /* 在途请求超时（默认 30000） */
    uint32_t resume_tout_ms;
    uint8_t  tx_window;        /* 包通道应用层信用初值（= 对端重组槽数） */
} bt_mcp_session_cfg_t;

typedef struct bt_mcp_session bt_mcp_session_t;

int  bt_mcp_session_init(bt_mcp_session_t *s, const bt_mcp_session_cfg_t *cfg);
void bt_mcp_session_bind(bt_mcp_session_t *s, bt_mcp_framer_t *f,
                         bt_mcp_transport_t *t);
bt_mcp_state_t bt_mcp_session_state(const bt_mcp_session_t *s);
/* 下行（来自 framer 上抛）：完整 MCP 消息或控制帧 */
int  bt_mcp_session_rx(bt_mcp_session_t *s, uint8_t kind,
                       const uint8_t *payload, uint16_t len);
/* 上行（来自 L5）：完整 MCP 消息，受信用窗口约束，可能 E_AGAIN */
int  bt_mcp_session_tx(bt_mcp_session_t *s, const uint8_t *json, uint32_t len);
void bt_mcp_session_tick(bt_mcp_session_t *s, uint32_t now_ms);
/* port 在断链时调用：作废在途映射、token 处置按 bond 状态参数化 */
void bt_mcp_session_link_down(bt_mcp_session_t *s, int bond_intact);

/* ===== bt_mcp/mcp_server.h —— L5 ===== */
/* JSON 解析为单遍 SAX 式（深度上限 6，键名白名单匹配，目标峰值 < 2KB RAM）。
   写出为流式 writer 直写发送缓冲，不建 DOM。v0.3 实际接口： */

/* ---- bt_mcp/json.h —— 零拷贝请求视图 + 按需 getter ---- */
typedef struct {
    const char *src;      uint32_t len;        /* 原始报文（只读，不拷贝） */
    const char *method;   uint16_t method_len;
    const char *params;   uint16_t params_len; /* 顶层 params 片段 */
    const char *id_str;   uint16_t id_str_len; /* 字符串 id 原文（含转义） */
    int64_t     id_num;
    uint8_t     id_is_str, has_id, has_method;
} bt_mcp_json_req_t;

int bt_mcp_json_parse    (const char *src, uint32_t len, bt_mcp_json_req_t *out);
int bt_mcp_json_parse_msg(const char *src, uint32_t len, bt_mcp_json_req_t *out); /* 允许无 method（响应） */
int bt_mcp_json_get_str  (const bt_mcp_json_req_t *r, const char *key,
                          const char **out, uint16_t *len);   /* 返回引号内原文段 */
int bt_mcp_json_get_u32  (const bt_mcp_json_req_t *r, const char *key, uint32_t *out);
int bt_mcp_json_get_i32  (const bt_mcp_json_req_t *r, const char *key, int32_t *out);
int bt_mcp_json_get_raw  (const bt_mcp_json_req_t *r, const char *key,
                          const char **out, uint32_t *len);
int bt_mcp_json_top_raw  (const bt_mcp_json_req_t *r, const char *key,
                          const char **out, uint32_t *len);   /* 顶层键（响应 result/error） */
int bt_mcp_json_get_sub  (const bt_mcp_json_req_t *r, const char *key,
                          bt_mcp_json_req_t *sub);            /* params 内子对象嫁接 */
uint16_t bt_mcp_json_decode_str(const char *s, const char *e,
                                char *dst, uint16_t cap, uint16_t *need); /* 解转义 */

/* ---- bt_mcp/writer.h —— 流式 JSON 写出（不建 DOM、不分配） ---- */
int bt_mcp_w_raw    (bt_mcp_writer_t *w, const char *data, uint32_t len);
int bt_mcp_w_str_lit(bt_mcp_writer_t *w, const char *s);      /* 转义并加引号 */
int bt_mcp_w_str_n  (bt_mcp_writer_t *w, const char *s, uint32_t n);
int bt_mcp_w_kv_str (bt_mcp_writer_t *w, const char *key, const char *val);
int bt_mcp_w_kv_u32 (bt_mcp_writer_t *w, const char *key, uint32_t v);
int bt_mcp_w_kv_u64 (bt_mcp_writer_t *w, const char *key, uint64_t v);
int bt_mcp_w_kv_raw (bt_mcp_writer_t *w, const char *key, const char *raw_json);
int bt_mcp_w_begin_result_id(bt_mcp_writer_t *w, int64_t id,
                             const char *id_str, uint16_t id_str_len);
int bt_mcp_w_begin_error_id (bt_mcp_writer_t *w, int64_t id,
                             const char *id_str, uint16_t id_str_len,
                             int code, const char *msg);
int bt_mcp_w_end    (bt_mcp_writer_t *w);

/* ---- bt_mcp/session.h —— L4（port 只需这几行） ---- */
int  bt_mcp_session_init     (bt_mcp_session_t *s, const bt_mcp_session_cfg_t *cfg);
void bt_mcp_session_bind     (bt_mcp_session_t *s, bt_mcp_framer_t *fr,
                              int (*emit)(void *, const uint8_t *, uint16_t), void *io);
void bt_mcp_session_link_up  (bt_mcp_session_t *s, uint32_t resume_token);
void bt_mcp_session_mark_ready(bt_mcp_session_t *s);   /* 收到 notifications/initialized */
void bt_mcp_session_link_down(bt_mcp_session_t *s, int bond_intact);
int  bt_mcp_session_feed     (bt_mcp_session_t *s, const uint8_t *in, uint16_t len,
                              bt_mcp_mcp_sink_t sink, void *user);  /* port 收数一行入口 */
int  bt_mcp_session_rx       (bt_mcp_session_t *s, uint8_t kind, uint8_t seq,
                              const uint8_t *payload, uint16_t len,
                              bt_mcp_mcp_sink_t sink, void *user);
int  bt_mcp_session_tx       (bt_mcp_session_t *s, const uint8_t *json, uint32_t len);
int  bt_mcp_session_ctrl     (bt_mcp_session_t *s, uint8_t type, const uint8_t *args, uint16_t len);
int  bt_mcp_session_pump     (bt_mcp_session_t *s);    /* 冲 NACK / 到期重传 */
void bt_mcp_session_tick     (bt_mcp_session_t *s, uint32_t now_ms);
int  bt_mcp_session_ping     (bt_mcp_session_t *s, uint8_t epoch);
uint32_t bt_mcp_session_rtt  (const bt_mcp_session_t *s, int *valid);  /* 0 或 RTT(ms) */

/* ---- bt_mcp/mcp_server.h —— L5 注册表（编译期 const 数组，运行期零注册开销） ---- */
/* 工具实现：返回 0=同步成功；BT_MCP_E_PENDING=已受理（稍后 server_complete）；
 * 其余=错误（响应带 isError:true）。文本由工具直写 out（server 负责 JSON 转义）。 */
typedef int (*bt_mcp_tool_fn)(const bt_mcp_json_req_t *req,
                              bt_mcp_writer_t *out,
                              bt_mcp_call_t *call,        /* 异步/进度/取消上下文 */
                              void *user);

typedef struct {
    const char     *name;
    const char     *desc;
    const char     *input_schema_json;      /* 内嵌 JSON Schema 字符串 */
    uint8_t         authz;                  /* 0=open 1=consent 2=restricted */
    bt_mcp_tool_fn  fn;
    void           *user;
} bt_mcp_tool_def_t;

/* 资源读取：交出 [cursor, next) 窗口的静态文本（server 只读不拷贝） */
#define BT_MCP_CURSOR_END 0xFFFFFFFFu
typedef int (*bt_mcp_resource_read_fn)(void *user, uint32_t cursor,
                                       const char **text, uint32_t *len,
                                       uint32_t *next_cursor);

typedef struct {
    const char               *name;         /* 如 "hr_window" */
    const char               *desc;
    const char               *mime_type;    /* 可空 → text/plain */
    uint8_t                   authz;
    bt_mcp_resource_read_fn   read;         /* 可空 = 空内容资源 */
    void                     *user;
} bt_mcp_resource_def_t;

int  bt_mcp_server_init  (bt_mcp_server_t *sv, bt_mcp_session_t *s,
                          const bt_mcp_server_cfg_t *cfg,
                          const char *fw_name, const char *fw_version);
int  bt_mcp_server_rx    (bt_mcp_server_t *sv, const uint8_t *json, uint32_t len);
void bt_mcp_server_tick  (bt_mcp_server_t *sv, uint32_t now_ms);
int  bt_mcp_server_notify(bt_mcp_server_t *sv, const char *method,
                          const char *params_json, uint32_t params_len);
/* 异步完成 / 进度 / 取消（工具回调期间与 complete 之前持有 call 指针） */
int  bt_mcp_server_complete(bt_mcp_call_t *call, const char *text, int is_error);
int  bt_mcp_call_progress  (bt_mcp_call_t *call, uint64_t done, uint64_t total);
int  bt_mcp_call_cancelled (const bt_mcp_call_t *call);
int bt_mcp_w_error        (bt_mcp_writer_t*, int64_t req_id,
                           int code, const char *message);
```

### 7.1 驱动模型（裸机与 RTOS 同构）

```c
/* 裸机主循环 */
for (;;) {
    bt_mcp_framer_tick(&framer, now());
    bt_mcp_session_tick(&sess, now());
    bt_mcp_server_tick(&server, now());
    platform_run_bt_stack();        /* 产生 on_rx/on_conn → core 内部状态推进 */
    idle_sleep();
}
/* RTOS：单一协议任务把同样的三个 tick + 队列 drain 包成 while(1)。
   core 本身不含任何队列/信号量原语 —— 并发归一由 port 层完成。 */
```

### 7.2 配置（编译期，全部集中在 bt_mcp/config.h，无 vendor ifdef）

```c
/* 容量（可被 BT_MCP_TIER 一键覆盖，§7.3） */
#define BT_MCP_CFG_MAX_REASM     4      /* 重组槽数 */
#define BT_MCP_CFG_MAX_MSG       8192   /* 单消息字节上限 */
#define BT_MCP_CFG_MAX_FRAG      32     /* 单消息分片数上限 */
#define BT_MCP_CFG_FRAG_TOUT_MS  2000
/* 不可靠包通道可靠性（§5.4） */
#define BT_MCP_CFG_TX_SLOTS        2
#define BT_MCP_CFG_TX_RETRY_BYTES  512
#define BT_MCP_CFG_RX_FP_SLOTS     2
#define BT_MCP_CFG_ACK_TOUT_MS     600
#define BT_MCP_CFG_MAX_RETRY       3
#define BT_MCP_CFG_DEDUPE_MS       5000
/* 安全（§6.2） */
#define BT_MCP_CFG_RESUME_MAX_FAIL 3
#define BT_MCP_CFG_RESUME_LOCK_MS  30000
/* L5 */
#define BT_MCP_CFG_PAGE_SIZE       8
#define BT_MCP_CFG_MAX_PENDING     2
#define BT_MCP_CFG_FCS16           0   /* 通道无链路校验时置 1 */
```
（全静态：无 `BT_MCP_CFG_HEAP`。宿主机侧 mock/oracle 自行用动态内存，core 不用。）

### 7.3 内存档位（`bt_mcp/tier.h`）

`-DBT_MCP_TIER=BT_MCP_TIER_COMPACT|STANDARD|FULL` 一键选容量；线上字节完全一致，
只有单消息上限/槽数/分页大小不同。实测静态 RAM（framer+session+server）：

| 档位 | MAX_MSG | 槽 | 静态 RAM | 适用 |
|---|---|---|---|---|
| COMPACT | 1024 | 2 | **4048 B** | 64KB SRAM 级 BLE SoC（8/16 位 MCU）|
| STANDARD | 2048 | 4 | **11760 B** | 128KB SRAM 级，主流可穿戴 |
| FULL | 8192 | 4 | **42480 B** | 256KB+ SRAM / 网关 / 宿主 |

## 8. Port 清单（协议核心之外的全部工作）

| port | 平台无关度 | 实现要点 |
|---|---|---|
| GATT | 任意 BLE 栈（vendor host/Zephyr/bluedroid/NimBLE…）：定义一个 3 特征服务（Ctrl notify+write / Dn notify / Up write-no-rsp），ATT_MTU−3 填 cap.mtu，**reliable=0**（开启 §5.4 重传）；栈回调转投协议上下文 | 唯一必选 |
| SPP | 任意 Classic 栈：SDP 注册 SPP、RFCOMM 通道，直接流通道（reliable=1、flow_ctrl=1）| 双模设备可选 |
| L2CAP CoC | 支持 LE Credit Based 的栈：`coc_port.h`，cap = PACKET + reliable=1 + flow_ctrl=1（只保留分片，免信用免重传）| 网关场景可选 |
| host-mock | POSIX：socketpair 仿真包/流通道 | CI/golden/fuzz 必备 |

SPP 与 GATT 之上协议核心**零代码差异**：唯一区别是 port 填的 `bt_mcp_chan_cap_t`。

## 9. 兼容性矩阵

| 兼容目标 | 本设计的支撑点 |
|---|---|
| 任意 MCU（8/16/32 位） | C99、无堆、无 OS 原语、整型定宽、定长结构体 |
| 任意蓝牙栈 | 栈接触面收敛在 4 个 port（§8） |
| 任意 Profile | 能力描述符自适应（分片/信用按 cap 收敛，流通道自动跳过） |
| 任意 MCP Host | 线上 = 标准 JSON-RPC MCP（Bridge 转成 stdio/HTTP 对上） |
| 存量 MCP 规范版本 | 锚定 2025-11-25 有状态语义；无状态版（2026-07-28）作为 v2 适配层，握手面已收敛在 mcp_server.c 单点 |

## 10. 路线与验证

- M1 ✅：frame.c + host-mock + 独立 Rust oracle 做差分 + 独立 fuzz 引擎喂帧（截断/乱序/重复/失步流）；
  出口 = 双向字节 100% 一致、fuzz 零 crash。
- M2 ✅：session+mcp_server 闭环；出口 = initialize→tools/list→tools/call E2E + 63 项单元检查。
- M3 ✅：GATT（包/不可靠）与 SPP（流/可靠）两套 port 同一份 core 背靠背对照（184 项 HIL）；
  断链注入/窗口压满/RESUME/去重/重传全覆盖。
- M4 ✅：CoC port 落地（`coc_port.h`）；差分覆盖升到 1339 项。
- M5 ✅（协议侧）：开销基线实测（`tools/bench.c`，见下表）。待办：真实芯片移植示例
  （Zephyr/NimBLE）、手机 Bridge 代码、真实硬件上的功耗实测。

### 10.1 开销基线（`tools/bench.c` 实测，非估算）

包通道分片开销（512B MCP 消息）：

| MTU | 分片 | 线缆字节 | 开销 | 放大 |
|---|---|---|---|---|
| 23 | 31 | 698 | +36.3% | 1.36× |
| 53 | 11 | 578 | +12.9% | 1.13× |
| 100 | 6 | 548 | +7.0% | 1.07× |
| 185 | 3 | 530 | +3.5% | 1.04× |
| 244 | 3 | 530 | +3.5% | 1.04× |

流通道（SPP/UART）：整条消息单帧，固定 **+6B**（与消息长度无关；MTU 无关）。
可靠性控制帧开销（不可靠包通道，MTU=244）：消费 128B 请求 + 发 512B 响应时，
控制帧 2 帧 / 17B，占链路总字节 **3.1%**（即每条被消费消息约 +8.5B）。

工程含义：**MTU 是首要旋钮**——ATT_MTU 23 的默认档放大 1.36×，而交换到 244
后降到 1.04×。所以 GATT port 应尽早做 MTU 交换；若不能，可考虑用流通道
（SPP/CoC）承载大 payload 类工具。
