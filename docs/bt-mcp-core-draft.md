# BT-MCP 核心协议草案 v0.6 —— 平台无关设计

日期：2026-11-11 · 状态：草案。v0.6 实现已验证：**1295 项差分 + 198 项 port HIL
+ 695 项单元/定向 + 11469 项表驱动矩阵断言 + 38 项官方 MCP SDK 互验 + 20 项 golden**，
全绿；行覆盖 **95.1%** / 分支 **68.2%**；ASan/UBSan/TSan 干净。

> 本文是**唯一的协议与接口权威文档**（与公开头文件同步）。版本演进见下方摘要；
> 各版本的发布说明与验证报告见 [`docs/README.md`](README.md) 的索引。

## 版本演进摘要

**v0.6 相对 v0.5 的变化（主要是验证与健壮性，协议语义仅两处）：**

- **协议语义 1：空闲信用刷新**。累计 CREDIT 帧不受 ARQ 保护，丢一个就少一格；
  发送方信用耗尽且管道已空时，对端无新消费可用来补发 → 永久 `E_AGAIN`。
  修复为 `session_tick` 在空闲 ≥ `BT_MCP_CFG_CREDIT_REFRESH_MS` 时重发累计
  CREDIT（载荷是绝对值，重发幂等）（§5.4）；
- **协议语义 2：去重窗口 4 → 8**（`RX_FP_SLOTS`）。同时把「恰好一次」的表述
  从**无条件保证**改为**有界保证**，并给出实测对照表（§5.4）；
- **writer 去掉 libc `snprintf` 依赖**：改为自研整数格式化（`fmt_u64`/`fmt_i64`，
  含 INT64_MIN 溢出边界），使 JSON 写出层可在无 printf 的裸机环境构建；
- **writer 错误语义**：透传 sink 错误码（区分 `E_TOOBIG` 与 `E_IO`）；
- **验证引擎换代**：表驱动组合矩阵（3141 生成用例）、语法感知 fuzz（四条不变量）、
  正交差分矩阵（配置签名比对）、链路仿真 soak、多会话压测、RAM 预算门禁、
  文档一致性检查。详见 `roadmap-v0.6.md`。

**v0.5 相对 v0.4 的变化：**
- **规范语义修正**：`ping` 响应改为空结果对象 `{}`（原 `{"pong":true}` 不符规范）；
  未初始化错误码改为 `-32001`，与「资源不存在」`-32002` 分离（§5.2）；
- **`_meta` 透传**：工具经 `bt_mcp_call_set_meta()` 声明，结果层输出；
  进度通知增加规范可选字段 `message`（§5.6）；
- **JSON-RPC batch 按协商版本分支**：`2025-03-26` 及更早聚合为数组，
  `2025-06-18+` 明确回 `-32600`（§5.2）；
- **大消息重传保护**：`bt_mcp_session_tx_ref()` 引用式保护，突破 512B 限制（§5.4）；
- **拥塞退避**：RTO 指数退避（上限 `BT_MCP_CFG_RTO_MAX_MS`），ACK 到达即回落（§5.4）；
- **多会话**：`bt_mcp_server_pool_t` 按 session 分派（§7.1）；
- **共享重组缓冲**：`BT_MCP_CFG_SHARED_REASM=1` 把 framer 从 41072B 降到 16448B（§7.3）；
- **解析器收紧**：拒绝尾随垃圾与尾随逗号（帧长度已界定边界，尾随字节即损坏）（§4.5）；
- **健壮性**：writer 校验 sink、client/JSON getter 校验空指针，全部 fail-closed（§7.4）。

**v0.4 相对 v0.3 的变化：**
- **授权真正实施**：`AUTHZ_CONSENT` 走 consent 回调、`AUTHZ_RESTRICTED` 一律拒绝，
  拒绝映射 `-32003`（§6.3）；
- **prompts / 资源订阅 / 日志分级 / list_changed** 四项能力落地（§5.2）；
- **富内容结果**：`text` / `image` / `audio` / `embedded resource` 多内容项（§5.6）；
- **server→client 请求**：`sampling`/`roots`/`elicitation` 的承载能力，
  在途槽 + id 匹配 + 超时（§5.7）；
- **MTU 热更新**：`bt_mcp_framer_set_mtu()`，BLE MTU 交换后无需重建 framer（§4.1）；
- **NVM 持久化**：resume token / 绑定标志可落盘，掉电可恢复（§5.8）；
- **keepalive**：`KEEPALIVE_MS` / `IDLE_DEAD_MS`（默认关闭）（§5.8）；
- **响应缓冲去静态化**：server 与 client 的拼装缓冲移入实例，多设备桥接才正确（§7.4）。

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
| 0x07 | NACK | u8 seq ⁝ u8 idx | 缺片补发请求：对端只重发该片（v0.3） |

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
| 顶层数组 | 走 batch 分支（§5.2），不当作单条消息 |
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

### 5.4 不可靠包通道的消息级可靠（v0.3 新增）

`cap.reliable == 0`（GATT notify 即此类）时 L4 启用：

| 机制 | 触发 | 行为 |
|---|---|---|
| 发送重传槽 | `session_tx` 成功发出 | 消息体（≤ `BT_MCP_CFG_TX_RETRY_BYTES`）入槽环，记录 seq |
| ACK | 对端消费该消息 | 回 `ACK(seq)`；本地按 seq 释放槽 |
| NACK 补片 | 本地重组发现缺片 | 发 `NACK(seq, idx)`；对端只重发第 idx 片 |
| 超时整条重传 | `ack_tout_ms` 内未收到 ACK | 用**原 seq** 重发整条，最多 `retry_max` 次，之后弃槽并计 `stat_tx_dropped` |
| 接收去重 | 收到已交付过的消息 | `seq + len + CRC32` 指纹窗口（默认 5s / `RX_FP_SLOTS`=8 条），命中则只回 ACK、不再上抛 |

**普通 `tx` 只保护「放得下」的消息**（默认 512B/槽 × 2 槽）；更大的消息按 §5.5
走 resources 分页，或由应用层兜底重试。`BT_MCP_CFG_TX_SLOTS` /
`BT_MCP_CFG_TX_RETRY_BYTES` 可调。

**大消息保护：`tx_ref`（v0.5）**——`bt_mcp_session_tx_ref(s, json, len)` 不复制
原文，只在槽里记 `(seq, len, 指向调用方缓冲的指针)`，重传时从该指针重新编码，
因此可覆盖到 `MAX_MSG`。**契约**：调用方必须在 ACK 或放弃之前保证该缓冲有效；
槽释放（ACK / 超限丢弃 / NACK 完成）时一律清空引用，避免悬空指针。

**去重的边界（v0.6 实测校准，重要）**：
「恰好一次」是**有界保证**，不是无条件保证。去重表只有固定个槽，且带
`rx_msg_no` 记代护栏；一旦副本的到达延迟超过窗口深度，原指纹已被挤出，
该副本会被当作新消息二次上抛。

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

### 5.2 MCP 方法面（线上与标准 MCP 完全同形）

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

### 5.6 异步调用、进度、取消与 `_meta`（v0.3 起，v0.5 扩充）

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

### 5.7 server→client 请求（v0.4）

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

### 5.8 生命周期：MTU 热更新 / NVM / keepalive（v0.4）

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

## 7. C99 模块接口骨架

> 本节是**公开头文件的镜像**（v0.5）。改动 API 时必须同步更新本节；
> §7.5 给出防漂移的自动检查方法。

约定：所有类型以 `bt_mcp_` 前缀；所有函数非阻塞、返回 `int`（0 成功，
负值 = `BT_MCP_E*`）；时间一律由调用方传入 `now_ms`（无 OS 时钟依赖）；
无动态分配；所有实例状态都在调用方持有的结构体里（无模块级可变状态，
trace 钩子除外且仅调试用）。

### 7.0 错误码（`bt_mcp/err.h`）

```c
enum {
    BT_MCP_OK        =  0,
    BT_MCP_E_AGAIN   = -1,   /* 缓冲/窗口暂满，稍后 tick 重试 */
    BT_MCP_E_ARGS    = -2,   /* 参数非法（含空指针） */
    BT_MCP_E_STATE   = -3,   /* 当前会话状态不接受此操作 */
    BT_MCP_E_TOOBIG  = -4,   /* 超 MAX_MSG / 分片超限 / 结果过大 */
    BT_MCP_E_NOMEM   = -5,   /* 静态池耗尽（槽满） */
    BT_MCP_E_SYNC    = -6,   /* 流失步 / 重组校验失败 */
    BT_MCP_E_PARSE   = -7,   /* JSON 语法/类型不匹配 */
    BT_MCP_E_IO      = -8,   /* 底层 emit/sink 失败 */
    BT_MCP_E_PENDING = -9,   /* 工具已受理，结果稍后经 complete 送达 */
    BT_MCP_E_CANCEL  = -10,  /* 已被 notifications/cancelled 取消 */
    BT_MCP_E_LOCKED  = -11,  /* RESUME 退避锁定中 */
    BT_MCP_E_AUTHZ   = -12,  /* 授权拒绝（consent 未放行 / RESTRICTED） */
    BT_MCP_E_BUSY    = -13,  /* 在途请求槽已满 */
};
const char *bt_mcp_strerror(int rc);   /* 稳定字符串，便于日志 */
```

### 7.1 L3 分帧（`bt_mcp/frame.h`）

```c
typedef void (*bt_mcp_msg_sink_t)(void *user, uint8_t kind, uint8_t seq,
                                  const uint8_t *payload, uint16_t len);
    /* kind: BT_MCP_KIND_MCP=0（完整重组后的 MCP 消息）
             BT_MCP_KIND_CTRL=1（控制帧，原样上抛 L4） */

typedef struct {
    uint8_t  max_reasm;      /* 重组槽数；0 = 用编译期默认 */
    uint16_t frag_tout_ms;
} bt_mcp_frame_cfg_t;

typedef struct bt_mcp_framer bt_mcp_framer_t;   /* 定长，大小编译期定 */

int  bt_mcp_framer_init(bt_mcp_framer_t *f, const bt_mcp_frame_cfg_t *cfg,
                        const bt_mcp_chan_cap_t *cap);
/* 发送：整条消息按 cap.mtu 切片，经 emit 逐片吐出（一片 = 一次回调） */
int  bt_mcp_framer_send(bt_mcp_framer_t *f, uint8_t kind,
                        const uint8_t *payload, uint32_t len,
                        int (*emit)(void*, const uint8_t*, uint16_t), void *io);
/* 指定 seq / 指定单片的发送（重传与 NACK 补片用；v0.3） */
int  bt_mcp_framer_send_seq(bt_mcp_framer_t *f, uint8_t kind, uint8_t seq,
                            const uint8_t *payload, uint32_t len,
                            int (*emit)(void*, const uint8_t*, uint16_t), void *io);
int  bt_mcp_framer_send_idx(bt_mcp_framer_t *f, uint8_t kind, uint8_t seq,
                            uint8_t idx, const uint8_t *payload, uint32_t len,
                            int (*emit)(void*, const uint8_t*, uint16_t), void *io);
uint8_t bt_mcp_framer_alloc_seq(bt_mcp_framer_t *f);
/* 接收：喂原始字节（包通道喂整片；流通道喂任意字节段），重组完成经 sink 上抛 */
int  bt_mcp_framer_feed(bt_mcp_framer_t *f, const uint8_t *in, uint16_t len,
                        bt_mcp_msg_sink_t sink, void *user);
/* 取走待发 NACK 登记：1=有（seq/idx 已填），0=无 */
int  bt_mcp_framer_take_nack(bt_mcp_framer_t *f, uint8_t *seq, uint8_t *idx);
void bt_mcp_framer_tick(bt_mcp_framer_t *f, uint32_t now_ms);
/* 链路 MTU 热更新（v0.4）：0 = 回退 cap->mtu；流通道返回 E_ARGS */
int  bt_mcp_framer_set_mtu(bt_mcp_framer_t *f, uint16_t mtu);
void bt_mcp_framer_reset(bt_mcp_framer_t *f);   /* 断链丢弃半帧 */
```

### 7.2 L4 会话（`bt_mcp/session.h`）

```c
typedef enum {
    BT_MCP_S_IDLE = 0, BT_MCP_S_LINK_UP, BT_MCP_S_READY
} bt_mcp_state_t;

typedef void (*bt_mcp_mcp_sink_t)(void *user, const uint8_t *json, uint32_t len);

typedef struct {
    uint32_t req_timeout_ms;   /* 在途请求超时（0 = 30000） */
    uint32_t resume_tout_ms;
    uint8_t  tx_window;        /* 包通道应用层信用初值 */
    uint8_t  retry_max;        /* 重传上限（0 = 默认 3） */
    uint16_t ack_tout_ms;      /* ACK 时限基准（0 = 默认 600），也是 RTO 回落值 */
    uint8_t  role;             /* BT_MCP_ROLE_CLIENT / _SERVER（RESUME 裁决方） */
} bt_mcp_session_cfg_t;

typedef struct {               /* NVM 持久化钩子（v0.4） */
    int (*save)(void *user, const void *buf, uint16_t len);
    int (*load)(void *user, void *buf, uint16_t len);
    void *user;
} bt_mcp_storage_t;

typedef struct {               /* 落盘快照布局 */
    uint32_t magic;            /* 'BTM1' */
    uint16_t version;          /* 1 */
    uint16_t flags;            /* bit0 = 已绑定 */
    uint32_t resume_token;
} bt_mcp_session_state_t;

typedef struct bt_mcp_session bt_mcp_session_t;   /* 定长 */

int  bt_mcp_session_init(bt_mcp_session_t *s, const bt_mcp_session_cfg_t *cfg);
void bt_mcp_session_bind(bt_mcp_session_t *s, bt_mcp_framer_t *fr,
                         int (*emit)(void*, const uint8_t*, uint16_t), void *io);
bt_mcp_state_t bt_mcp_session_state(const bt_mcp_session_t *s);

/* 状态迁移（由 port / 应用驱动） */
void bt_mcp_session_link_up(bt_mcp_session_t *s, uint32_t resume_token);
void bt_mcp_session_mark_ready(bt_mcp_session_t *s);
void bt_mcp_session_link_down(bt_mcp_session_t *s, int bond_intact);

/* 下行（来自 L5 以外的管道）；上行（来自 L5），受信用窗口约束 */
int  bt_mcp_session_rx(bt_mcp_session_t *s, uint8_t kind, uint8_t seq,
                       const uint8_t *payload, uint32_t len,
                       bt_mcp_mcp_sink_t sink, void *user);
int  bt_mcp_session_tx(bt_mcp_session_t *s, const uint8_t *json, uint32_t len);
/* v0.5：引用式发送——槽里只记指针，重传时读调用方缓冲，突破 512B 限制。
 * 契约：缓冲必须在 ACK/放弃之前有效。 */
int  bt_mcp_session_tx_ref(bt_mcp_session_t *s, const uint8_t *json, uint32_t len);

/* 便捷路径：喂字节 → framer 重组 → 状态门 → 上抛 sink（推荐 port 用这个） */
int  bt_mcp_session_feed(bt_mcp_session_t *s, const uint8_t *in, uint16_t len,
                         bt_mcp_mcp_sink_t sink, void *user);
int  bt_mcp_session_pump(bt_mcp_session_t *s);       /* 冲待发 NACK/控制帧 */
int  bt_mcp_session_ctrl(bt_mcp_session_t *s, uint8_t type,
                         const uint8_t *args, uint8_t alen);
int  bt_mcp_session_ping(bt_mcp_session_t *s, uint8_t epoch);
uint32_t bt_mcp_session_rtt(const bt_mcp_session_t *s, int *valid);
void bt_mcp_session_tick(bt_mcp_session_t *s, uint32_t now_ms);

/* 持久化与 keepalive（v0.4） */
void     bt_mcp_session_set_storage(bt_mcp_session_t *s, const bt_mcp_storage_t *st);
int      bt_mcp_session_save_state(const bt_mcp_session_t *s);
int      bt_mcp_session_load_state(bt_mcp_session_t *s);
uint32_t bt_mcp_session_resume_token(const bt_mcp_session_t *s);
```

### 7.3 L5 MCP server（`bt_mcp/mcp_server.h`）

注册表（**全部建议用指定初始化器**，字段顺序在 v0.4 变过）：

```c
/* 授权级别 */
enum { BT_MCP_AUTHZ_OPEN = 0, BT_MCP_AUTHZ_CONSENT, BT_MCP_AUTHZ_RESTRICTED };
/* 工具注解位（对应 MCP annotations） */
#define BT_MCP_TOOL_READONLY     0x01
#define BT_MCP_TOOL_DESTRUCTIVE  0x02
#define BT_MCP_TOOL_IDEMPOTENT   0x04
#define BT_MCP_TOOL_OPENWORLD    0x08
/* 日志级别 */
enum { BT_MCP_LOG_DEBUG = 0, BT_MCP_LOG_INFO, BT_MCP_LOG_NOTICE,
       BT_MCP_LOG_WARNING, BT_MCP_LOG_ERROR, BT_MCP_LOG_CRITICAL,
       BT_MCP_LOG_ALERT, BT_MCP_LOG_EMERGENCY };

typedef int (*bt_mcp_tool_fn)(const bt_mcp_json_req_t *args, bt_mcp_writer_t *out,
                             bt_mcp_call_t *call, void *user);
typedef struct {
    const char *name; const char *desc; const char *title;
    const char *input_schema_json; const char *output_schema_json;
    uint8_t     annotations;    /* BT_MCP_TOOL_* 位或 */
    uint8_t     authz;          /* BT_MCP_AUTHZ_* */
    bt_mcp_tool_fn fn; void *user;
} bt_mcp_tool_def_t;

typedef int (*bt_mcp_resource_read_fn)(void *user, uint32_t cursor,
                                       const char **text, uint32_t *len,
                                       uint32_t *next_cursor);
typedef int (*bt_mcp_resource_sub_fn)(void *user, int subscribe);
typedef struct {
    const char *name; const char *desc; const char *title;
    const char *mime_type; uint32_t size;
    uint8_t     authz;
    bt_mcp_resource_read_fn read; bt_mcp_resource_sub_fn sub; void *user;
} bt_mcp_resource_def_t;

typedef int (*bt_mcp_prompt_get_fn)(const bt_mcp_json_req_t *args,
                                    bt_mcp_writer_t *out, void *user);
typedef struct {
    const char *name; const char *desc; const char *title;
    const char *arguments_schema_json;
    bt_mcp_prompt_get_fn get; void *user;
} bt_mcp_prompt_def_t;

/* consent 回调：返回 0 = 放行，非 0 = 拒绝 */
typedef int (*bt_mcp_consent_fn)(void *user, const char *tool_name,
                                 const bt_mcp_json_req_t *req);

typedef struct {
    const bt_mcp_tool_def_t     *tools;     uint16_t tools_count;
    const bt_mcp_resource_def_t *resources; uint16_t resources_count;
    const bt_mcp_prompt_def_t   *prompts;   uint16_t prompts_count;
    bt_mcp_consent_fn consent; void *consent_user;
} bt_mcp_server_cfg_t;
```

生命周期与入口：

```c
int  bt_mcp_server_init(bt_mcp_server_t *sv, bt_mcp_session_t *s,
                        const bt_mcp_server_cfg_t *cfg,
                        const char *fw_name, const char *fw_version);
/* 统一入口：单条消息、JSON-RPC batch（按协商版本分支）都在这里分派 */
int  bt_mcp_server_rx(bt_mcp_server_t *sv, const uint8_t *json, uint32_t len);
void bt_mcp_server_tick(bt_mcp_server_t *sv, uint32_t now_ms);
```

异步调用与进度：

```c
int  bt_mcp_server_complete(bt_mcp_call_t *call, const char *text, int is_error);
int  bt_mcp_call_progress(bt_mcp_call_t *call, uint64_t done, uint64_t total);
int  bt_mcp_call_progress_msg(bt_mcp_call_t *call, uint64_t done, uint64_t total,
                              const char *message);      /* v0.5：规范可选字段 */
int  bt_mcp_call_cancelled(const bt_mcp_call_t *call);
int  bt_mcp_call_set_meta(bt_mcp_call_t *call, const char *json, uint32_t len);
                            /* v0.5：声明结果级 _meta（"k":v 片段原文） */
```

富内容写入（工具回调内，v0.4）：

```c
int  bt_mcp_w_content_text(bt_mcp_writer_t *w, const char *text, uint32_t len);
int  bt_mcp_w_content_image(bt_mcp_writer_t *w, const char *b64, uint32_t b64len,
                            const char *mime);
int  bt_mcp_w_content_audio(bt_mcp_writer_t *w, const char *b64, uint32_t b64len,
                            const char *mime);
int  bt_mcp_w_content_resource(bt_mcp_writer_t *w, const char *uri,
                               const char *mime, const char *text, uint32_t len);
/* _meta 追加：name 是 _meta 内的键，json 是该键的 **JSON 片段原文**
 * （不额外加引号）：传 "\"0.5\"" 得到 "k":"0.5"，传 {"a":1} 得到 "k":{"a":1}。
 * 仅适用于**结果对象级** writer（如 prompts/get）；工具回调请用
 * bt_mcp_call_set_meta()（那边传的是整个 "k":v 片段）。 */
int  bt_mcp_w_meta(bt_mcp_writer_t *w, const char *name,
                   const char *json, uint32_t len);
```

下行通知与 server→client 请求：

```c
int  bt_mcp_server_notify(bt_mcp_server_t *sv, const char *method,
                          const char *params_json, uint32_t params_len);
int  bt_mcp_server_notify_resource_updated(bt_mcp_server_t *sv, const char *uri);
int  bt_mcp_server_notify_list_changed(bt_mcp_server_t *sv, const char *what);
int  bt_mcp_server_log(bt_mcp_server_t *sv, uint8_t level, const char *logger,
                       const char *text, uint32_t len);

typedef void (*bt_mcp_server_resp_fn)(void *user, int rc,
                                      const char *result_json, uint32_t result_len);
int  bt_mcp_server_request(bt_mcp_server_t *sv, const char *method,
                           const char *params_json, uint32_t params_len,
                           bt_mcp_server_resp_fn cb, void *user);
int  bt_mcp_server_inflight(const bt_mcp_server_t *sv);
```

多会话（v0.5）：

```c
typedef struct {
    bt_mcp_server_t *sv[BT_MCP_CFG_MAX_CONN];
    uint8_t          n;
} bt_mcp_server_pool_t;

void    bt_mcp_pool_init(bt_mcp_server_pool_t *pool);
int     bt_mcp_pool_add(bt_mcp_server_pool_t *pool, bt_mcp_server_t *sv);
int     bt_mcp_pool_rx(bt_mcp_server_pool_t *pool, bt_mcp_session_t *sess,
                       const uint8_t *json, uint32_t len);
void    bt_mcp_pool_tick(bt_mcp_server_pool_t *pool, uint32_t now_ms);
uint8_t bt_mcp_pool_count(const bt_mcp_server_pool_t *pool);
```

> `bt_mcp_server_t` 本身就是**每会话**上下文（`sess`/`initialized`/`calls[]`/
> `subs[]`/`log_level`/`proto`/batch 状态都在实例内，cfg 是值拷贝）。
> 因此多会话不需要拆分单实例逻辑，只需要按 session 找实例的分派器。
> 约束与单会话一致：**每个实例只被一个线程使用**。

### 7.4 L5 MCP client（`bt_mcp/mcp_client.h`）

```c
typedef struct {
    int         is_notification; int is_error; int32_t error_code;
    uint32_t    id;
    const char *method; uint16_t method_len;
    const char *result; uint32_t result_len;
} bt_mcp_client_event_t;
typedef void (*bt_mcp_client_cb_t)(void *user, const bt_mcp_client_event_t *ev);

int  bt_mcp_client_init(bt_mcp_client_t *c, bt_mcp_session_t *s,
                        bt_mcp_client_cb_t cb, void *user);
int  bt_mcp_client_initialize(bt_mcp_client_t *c, const char *client_name,
                              const char *client_version);
int  bt_mcp_client_request(bt_mcp_client_t *c, const char *method,
                           const char *params_json, uint32_t params_len,
                           uint32_t *out_id);
int  bt_mcp_client_list_tools(bt_mcp_client_t *c, const char *cursor,
                              uint32_t *out_id);
int  bt_mcp_client_call_tool(bt_mcp_client_t *c, const char *name,
                             const char *args_json, uint32_t args_len,
                             uint32_t *out_id);
int  bt_mcp_client_read_resource(bt_mcp_client_t *c, const char *uri,
                                 uint32_t *out_id);
int  bt_mcp_client_rx(bt_mcp_client_t *c, const uint8_t *json, uint32_t len);
int  bt_mcp_client_inflight(const bt_mcp_client_t *c);
void bt_mcp_client_tick(bt_mcp_client_t *c, uint32_t now_ms);
```

### 7.5 JSON / writer / trace（内部但对 port 可见）

```c
/* bt_mcp/json.h —— 零分配解析器 */
int  bt_mcp_json_parse(const char *src, uint32_t len, bt_mcp_json_req_t *out);
int  bt_mcp_json_parse_msg(const char *src, uint32_t len, bt_mcp_json_req_t *out);
int  bt_mcp_json_get_str(const bt_mcp_json_req_t*, const char*, const char**, uint16_t*);
int  bt_mcp_json_get_u32/i32/f64(...);
int  bt_mcp_json_get_raw(const bt_mcp_json_req_t*, const char*, const char**, uint32_t*);
int  bt_mcp_json_top_raw(const bt_mcp_json_req_t*, const char*, const char**, uint32_t*);
int  bt_mcp_json_get_sub(const bt_mcp_json_req_t*, const char*, bt_mcp_json_req_t*);
uint16_t bt_mcp_json_decode_str(const char *s, const char *e,
                                char *out, uint16_t cap, uint16_t *need);

/* bt_mcp/writer.h —— 零分配 JSON 写出 */
int bt_mcp_w_raw(bt_mcp_writer_t*, const char *data, uint32_t len);
int bt_mcp_w_str_n(w, const char *s, uint32_t n);
int bt_mcp_w_str_lit(w, const char *s);
int bt_mcp_w_key(w, const char *key);
int bt_mcp_w_kv_str / kv_raw / kv_u32 / kv_u64(...);
int bt_mcp_w_begin_result / begin_result_id / begin_error / begin_error_id(...);
int bt_mcp_w_end(w);

/* bt_mcp/trace.h —— 编译期可裁的调试钩子（BT_MCP_CFG_TRACE=0 时全部消失） */
typedef void (*bt_mcp_trace_fn)(int level, const char *what,
                                const void *data, uint16_t len);
void bt_mcp_trace_set(void *unused, bt_mcp_trace_fn fn);
void bt_mcp_trace(int level, const char *what, const void *data, uint16_t len);
```

**健壮性契约（v0.5 收紧）**：所有 writer 入口校验 `w`/`put`/`sink`，所有 JSON
getter 校验输出指针——传 NULL 一律返回 `E_ARGS`，**绝不崩溃**（嵌入式应
fail-closed）。v0.5 之前这两处都会解引用空指针。

### 7.6 驱动模型（裸机与 RTOS 同构）

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

### 7.7 配置（编译期，全部集中在 bt_mcp/config.h，无 vendor ifdef）

```c
/* ---- 容量（可被 BT_MCP_TIER 一键覆盖，§7.8；逐项仍可单独 #ifndef 覆盖）---- */
#define BT_MCP_CFG_SHARED_REASM     0      /* 1 = 单槽共享缓冲（v0.5，见 §4.3） */
#define BT_MCP_CFG_MAX_REASM        4      /* 重组槽数（共享模式下强制为 1） */
#define BT_MCP_CFG_MAX_MSG          8192   /* 单消息字节上限（≤ u16 可承载） */
#define BT_MCP_CFG_MAX_FRAG         255    /* 单消息分片数上限（u8 idx 决定） */
#define BT_MCP_CFG_FRAG_TOUT_MS     2000

/* ---- 不可靠包通道的消息级可靠（§5.4）---- */
#define BT_MCP_CFG_TX_SLOTS         2      /* 重传槽数 */
#define BT_MCP_CFG_TX_RETRY_BYTES   512    /* 普通 tx 的逐槽副本容量 */
#define BT_MCP_CFG_RX_FP_SLOTS      8      /* 接收去重指纹槽（≥ TX_SLOTS；v0.6: 4→8） */
#define BT_MCP_CFG_ACK_TOUT_MS      600    /* ACK 时限基准，也是 RTO 回落值 */
#define BT_MCP_CFG_MAX_RETRY        3      /* 每消息重传上限 */
#define BT_MCP_CFG_DEDUPE_MS        5000
#define BT_MCP_CFG_RTO_MAX_MS       4800   /* 指数退避上限（v0.5） */
#define BT_MCP_CFG_CREDIT_REFRESH_MS 600   /* 空闲信用刷新周期（v0.6） */

/* ---- 安全（§6）---- */
#define BT_MCP_CFG_RESUME_MAX_FAIL  3
#define BT_MCP_CFG_RESUME_LOCK_MS   30000

/* ---- L5 ---- */
#define BT_MCP_CFG_PAGE_SIZE        8      /* tools/resources/prompts 每页条数 */
#define BT_MCP_CFG_MAX_PENDING      2      /* 并发 tools/call 槽 */
#define BT_MCP_CFG_IDSTR_MAX        32     /* 字符串 request id 复制上限 */
#define BT_MCP_CFG_KEY_MAX          40     /* JSON 键名比较缓冲 */
#define BT_MCP_CFG_JSON_MAX_DEPTH   6      /* 嵌套深度上限（防栈爆） */
#define BT_MCP_CFG_NOTIFY_BUF       512    /* 单条通知缓冲（≥192 有编译期断言） */
#define BT_MCP_CFG_MAX_SUBS         8      /* 同时订阅的资源数（v0.4） */
#define BT_MCP_CFG_MAX_S2C_REQ      2      /* server→client 在途请求（v0.4） */
#define BT_MCP_CFG_S2C_TOUT_MS      30000  /* S→C 请求超时（v0.4） */
#define BT_MCP_CFG_MAX_CONN         4      /* 一个 pool 的并发会话数（v0.5） */
#define BT_MCP_CFG_BATCH_BUF        4096   /* JSON-RPC batch 响应聚合（v0.5） */
#define BT_MCP_CFG_META_MAX         256    /* 单次调用可携带的 _meta 原文（v0.5） */

/* ---- 链路活性（v0.4，默认关闭 = 行为不变）---- */
#define BT_MCP_CFG_KEEPALIVE_MS     0      /* >0：空闲该毫秒数后主动 PING */
#define BT_MCP_CFG_IDLE_DEAD_MS     0      /* >0：超该毫秒无入站即判链路失效 */

/* ---- 可选链路校验 ---- */
#define BT_MCP_CFG_FCS16            0      /* 1 = hdr|FCS16|payload（§4.1） */

/* ---- 调试 ---- */
#define BT_MCP_CFG_TRACE            0      /* 1 = 编译进 trace 钩子（§7.5） */
```

**编译期一致性断言**（`config.h` 末尾，违反即编译失败，不是运行期才发现）：
`MAX_MSG ≤ 65535`、`MAX_REASM ∈ 1..8`、`TX_SLOTS ∈ 1..8`、`RX_FP_SLOTS ≥ TX_SLOTS`、
`MAX_FRAG ∈ 1..255`、`NOTIFY_BUF ≥ 192`、`MAX_FRAG × 255 ≥ MAX_MSG`、
`TX_RETRY_BYTES ≥ 32`（或 retry_max=0 显式关闭）。

（全静态：无 `BT_MCP_CFG_HEAP`。宿主机侧 mock/oracle 自行用动态内存，core 不用。）

### 7.8 内存档位（`bt_mcp/tier.h`）

`-DBT_MCP_TIER=BT_MCP_TIER_COMPACT|STANDARD|FULL` 一键选容量；线上字节完全一致，
只有单消息上限/槽数/分页大小不同。

**实测静态 RAM**（`sizeof(framer) + sizeof(session) + sizeof(server)`，
2026-11-11 v0.5 实测，含 v0.4/v0.5 新增的订阅表、S2C 在途槽、batch 聚合缓冲、
`_meta` 缓冲、多会话池字段）：

| 档位 | MAX_MSG | 重组槽 | framer | session | server | **合计** |
|---|---|---|---|---|---|---|
| COMPACT | 1024 | 2 | 3152 | 808 | 6624 | **10584 B** |
| STANDARD | 2048 | 4 | 10352 | 1320 | 7776 | **19448 B** |
| FULL | 8192 | 4 | 41072 | 1320 | 14048 | **56440 B** |

**开启 `BT_MCP_CFG_SHARED_REASM=1`（§4.3）后**：

| 档位 | framer | 合计 | 相对默认 |
|---|---|---|---|
| COMPACT | 2112 | **9544 B** | −1040 B（−10%） |
| STANDARD | 4160 | **13256 B** | −6192 B（−32%） |
| FULL | 16448 | **31816 B** | **−24624 B（−44%）** |

档位选择建议：单消息上限决定 `MAX_MSG`，`MAX_MSG × MAX_REASM` 决定接收缓冲
主体。RAM 紧张且同时只有一个大消息在途时，优先开共享模式。
（`tools/bench.c` 可复现上表；CI 里应把这三行做成基线，回归即失败——v0.6 N7。）

> 注：v0.4 及更早的文档给的是 4048/11760/42480，那是**只算 framer+session、
> 且当时的 server 结构更小**的数字。v0.4 新增订阅表/S2C 槽、v0.5 新增 batch
> 聚合与 `_meta` 后，含 server 的完整占用如上表。

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
| 协议版本协商 | `initialize` 回显对端版本；未知版本回退本端默认。版本差异被**显式分支**（batch 见 §5.2），不做隐式兼容 |
| 第三方 Host 实现 | 已用官方 MCP TypeScript SDK 1.32.2 作 Host 跑通 38 项校验（`tests/sdk_interop/`），覆盖 initialize / tools / resources / prompts / logging / ping |
| 工具链 | Clang/GCC C99、`-Wall -Wextra -Werror` 零告警；ASan/UBSan/TSan 跑通 |

## 10. 路线与验证

- M1 ✅：frame.c + host-mock + 独立 Rust oracle 做差分 + 独立 fuzz 引擎喂帧
  （截断/乱序/重复/失步流）；出口 = 双向字节 100% 一致、fuzz 零 crash。
- M2 ✅：session+mcp_server 闭环；出口 = initialize→tools/list→tools/call E2E + 单元检查。
- M3 ✅：GATT（包/不可靠）与 SPP（流/可靠）两套 port 同一份 core 背靠背对照（198 项 HIL）；
  断链注入/窗口压满/RESUME/去重/重传全覆盖。
- M4 ✅：CoC port 落地（`coc_port.h`）；差分矩阵扩到四套 harness。
- M5 ✅（协议侧）：开销基线实测（`tools/bench.c`，见下表）。待办：真实芯片移植示例
  （Zephyr/NimBLE）、手机 Bridge 代码、真实硬件上的功耗实测。

**v0.4/v0.5 的验证体系**（四层互补，每层都抓到过其他层发现不了的缺陷）：

| 层 | 手段 | 规模 | 能发现 | 看不见 |
|---|---|---|---|---|
| L1 | C ↔ 独立 Rust oracle 差分 | 1295 项（主 1045 + FCS16 配对 196 + 共享模式 54） | 两侧**不一致** | 两侧一起错 |
| L2 | 结构符合性（`test_conformance.c`） | 64 项 | 响应**不符合规范结构** | 规范之外的语义错 |
| L3 | golden transcript（人工按规范核对） | 20 条 | 字节**非预期漂移** | 规范理解本身错 |
| L4 | 官方 MCP SDK 互验 | 38 项 | 第三方**无法正确理解**输出 | 传输层之外的实现缺陷 |
| 附加 | 有损通道 / TSan / 覆盖率 / 故障注入 | 17 + 10 + 门禁 + 21 | ARQ 收敛性 / 并发安全 / 倒退 / 错误传播 | — |

实测战果（v0.4+v0.5 共 10 个真缺陷）：L1 抓到 2 个（重写引入的引号/花括号）、
L2 抓到 2 个（`level` 缺引号、内嵌资源多一个右花括号导致**非法 JSON**）、
L3 抓到 1 个（`ping` 不是空对象）、L4 抓到 1 个（transport 未重组分片）、
边界/故障注入抓到 4 个（writer sink、client 空指针、JSON getter 空指针、
client 模块级缓冲）。**每一个都是其他层发现不了的。**

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
