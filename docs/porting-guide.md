# BT-MCP 移植指南（v0.9.1）

> **本文档的诚实前提**：以下参考实现是**按真实栈的 API 写的、经人工核对但未在本机编译验证**的
> （本机无 Zephyr/NimBLE SDK、无 ARM 工具链、无 Linux/BlueZ）。它的作用是给出
> **确切的接入点与顺序**，而不是「已上板验证」的代码。上板验证见 M1 里程碑 E1/E2。

## 1. 你需要实现什么（全部只有 4 个接入点）

core 对平台的假设只有这些，没有任何 OS / 动态分配 / libc 格式化依赖：

| # | 接入点 | 方向 | 你要做什么 |
|---|---|---|---|
| ① | `emit(io, bytes, len)` | core → 链路 | 把这段字节**原样**发出去（GATT notify / SPP write / CoC send） |
| ② | `framer_feed(&fr, bytes, len)` | 链路 → core | 收到字节时喂进来（**按到达边界**喂，不要自己拼） |
| ③ | `bt_mcp_chan_cap_t` | 配置 | 告诉 core 通道语义（见下表） |
| ④ | `bt_mcp_session_tick(&se, now_ms)` | 定时 | 周期性调用（建议 50~100ms），驱动重传/超时 |

**关键点**：`emit` 返回非 0 时 core 会把它当作「这次没发出去」，并按 `E_AGAIN` 语义
重试或等下次 tick —— 所以 `emit` 里**不要自己阻塞重试**，直接返回 `BT_MCP_E_AGAIN`
或 `BT_MCP_E_NOMEM` 即可。

## 2. 通道语义怎么填（选错会静默退化）

| 通道 | `kind` | `reliable` | `flow_ctrl` | 为什么 |
|---|---|---|---|---|
| **BLE GATT**（notify/write） | `PACKET` | **0** | **0** | notify 可能丢、可能乱序、无跨包边界保证 → 必须开消息级 ACK/重传/去重 |
| **SPP / RFCOMM**（字节流） | `STREAM` | 1 | 1 | 流本身可靠且有序，再套 ARQ 只是浪费 |
| **L2CAP CoC** | `PACKET` | 1 | 1 | 包语义但链路可靠 → 不需要 ARQ，需要分片重组 |

GATT 还有个必踩的坑：**MTU 语义**。`bt_mcp_chan_cap_t.mtu` 要填
`ATT_MTU - 3`（ATT 头 3 字节），且必须 ≥ 20。参考实现里做了这层预扣。

## 3. Zephyr / NimBLE 参考实现（片段）

完整文件见 `examples/zephyr/bt_mcp_port_nimble.c`。核心结构：

```c
/* ① 你持有这些状态（core 不分配内存） */
static bt_mcp_framer_t  g_fr;
static bt_mcp_session_t g_se;
static bt_mcp_chan_cap_t g_cap;
static uint32_t          g_now_ms;      /* 由工作队列推进 */

/* ① emit：core → 链路。注意返回值语义。 */
static int port_emit(void *io, const uint8_t *d, uint16_t n)
{
    (void)io;
    /* NimBLE：按当前协商 MTU 分块 notify。这里假设 core 已按 mtu 切片，
     * 所以一次 emit 就是一个 ATT 通知（不要在这里再切）。 */
    struct os_mbuf *om = os_msys_get_pkthdr(n, 0);
    if (!om) return BT_MCP_E_NOMEM;              /* 池耗尽 → 背压 */
    os_mbuf_append(om, d, n);
    int rc = ble_gatts_notify_custom(g_conn, g_mcp_val_handle, om);
    if (rc == BLE_HS_ENOMEM || rc == BLE_HS_EBUSY) return BT_MCP_E_AGAIN;
    if (rc) return BT_MCP_E_IO;
    return BT_MCP_OK;
}

/* ② 链路 → core：GATT 写回调里直接喂。 */
static int on_write_cb(uint16_t conn, uint16_t attr, struct os_mbuf *om)
{
    uint8_t buf[BT_MCP_CFG_MAX_MSG];
    uint16_t n = OS_MBUF_PKTLEN(om);
    if (n > sizeof buf) return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
    os_mbuf_copydata(om, 0, n, buf);
    /* 按到达边界喂；不要跨回调拼接（framer 自己管重组）。 */
    (void)bt_mcp_framer_feed(&g_fr, buf, n, on_mcp_msg, NULL);
    return 0;
}

/* ④ 定时：由工作队列每 50ms 调用一次。 */
static void bt_mcp_work_handler(struct k_work *w)
{
    g_now_ms = k_uptime_get_32();
    bt_mcp_session_tick(&g_se, g_now_ms);
    (void)bt_mcp_session_pump(&g_se);   /* 把一次能发的都发出去 */
}

/* ③ MTU 协商完成 / 断链 / 重连：三个必须接的事件 */
static void on_mtu_exchange(uint16_t att_mtu)
{
    g_cap.mtu = (uint16_t)(att_mtu - 3);        /* ATT 头预扣 */
    bt_mcp_framer_set_mtu(&g_fr, g_cap.mtu);    /* v0.4 API，v0.7 起差分已覆盖 */
}
static void on_disconnect(uint16_t conn)
{
    bt_mcp_session_link_down(&g_se, 0);
}
static void on_reconnect(uint16_t conn, uint32_t token)
{
    bt_mcp_session_link_up(&g_se, token);       /* token 用于 RESUME */
}
```

## 4. 初始化顺序（顺序错了会静默不工作）

```c
/* 1. 通道能力（决定 reliable/flow_ctrl/mtu） */
g_cap.kind = BT_MCP_CHAN_PACKET;
g_cap.mtu  = (uint16_t)(att_mtu - 3);
g_cap.reliable  = 0;      /* GATT */
g_cap.flow_ctrl = 0;
g_cap.max_pending = 4;

/* 2. framer（分片/重组/去重指纹），并绑定能力 */
bt_mcp_framer_init(&g_fr, &frame_cfg, &g_cap);

/* 3. session（ARQ/RTO/信用），并绑定 emit */
bt_mcp_session_cfg_t sc = { .req_timeout_ms = 30000, .resume_tout_ms = 5000,
                            .tx_window = 8, .role = BT_MCP_ROLE_SERVER };
bt_mcp_session_init(&g_se, &sc);
bt_mcp_session_bind(&g_se, &g_fr, port_emit, NULL);

/* 4. 上线（拿到连接和 token 之后） */
bt_mcp_session_link_up(&g_se, resume_token);

/* 5. server（注册 tools/resources/prompts） */
bt_mcp_server_init(&g_sv, &g_se, &cfg, "my-device", "1.0");

/* 6. 启动 tick 工作队列（不做这一步，重传永远不会发生） */
k_work_schedule(&g_work, K_MSEC(50));
```

## 5. 内存规划（用 `tools/ram_baseline.py` 的实测值）

三档实测（`sizeof(framer)+sizeof(session)+sizeof(server)`，2026-11 数据）：

| 档位 | 默认 | `SHARED_REASM=1` | 适用 |
|---|---|---|---|
| COMPACT | 10.7 KB | 9.6 KB | nRF52 / 256KB RAM 级 |
| STANDARD | 19.6 KB | 13.3 KB | 默认，平衡 |
| FULL | 56.6 KB | 31.9 KB | 大消息（8KB）场景 |

**这些是结构体大小，不含**：栈用量（建议 ≥2KB）、你加的 port 胶水、以及
`BT_MCP_CFG_MAX_MSG` 之外的应用缓冲。真实固件增量要用 `arm-none-eabi-size`
量 demo 而不是信这张表 —— 这正是 M1 的 E4。

## 6. 上板时的验收清单

按顺序做，每步都能独立判定成败：

1. **单帧往返**：发一条 `ping`，能在对端看到响应（验证 ①②④ 通了）。
2. **MTU 协商**：把手机 MTU 改到 247，确认大消息不再被切成 20 字节片
   （验证 `on_mtu_exchange` 接对了）。
3. **长消息**：发 1KB 消息，验证分片/重组（看 `stat_tx_retransmit`）。
4. **断链恢复**：拔掉连接再连，验证 RESUME 或完整握手（看 `resume_pending`）。
5. **丢包**：用干扰或对端限速制造丢包，验证重传收敛且**不重复执行工具**
   （v0.7 A1 修的正是这个；看 `stat_rx_dup` 与 `stat_tx_stale_drop`）。
6. **RAM 实测**：`arm-none-eabi-size` 对比 demo 与空工程，和 §5 的表比对。

## 6.5 两个必须知道的实现约束（v0.9.1）

**① MTU 不能在一条消息中途变化。** 接收侧用 `idx * chunk` 计算分片偏移，
chunk 在消息开始时确定。若 MTU 中途变了，已收到的片落在旧边界、后续片落在
新边界，拼出来是垃圾。因此 `bt_mcp_framer_set_mtu()` 在 chunk 变化时会
**弃掉全部在途重组槽**（这些消息本就不可能正确完成，由消息级重传重新发）。
所以你应当在 `MTU 协商完成` 后再让 MCP 流量开始，而不是边协商边传。

**② NACK 载荷是 `type | seq | count | idx[count]`。**
`count == 1` 时与旧格式（`type | seq | idx`）**字节完全一致**，所以与老对端
互通是安全的。若你自己实现一份，建议直接实现新格式。

## 7. 已知未验证项（如实记录）

| 项 | 状态 |
|---|---|
| 参考实现能否直接编译 | ❌ 未验证（无 SDK）。API 名称按 NimBLE 官方文档核对 |
| 目标代码体积 | ✅ **v0.9.1 已用真 Thumb-2 交叉编译量出**：core 约 42.5KB（`tools/footprint.py --arm`），FCS16 +2.4KB、TRACE +0.45KB |
| 真实 MTU 协商行为 | ❌ 未上板。`mtu-3` 的预扣按 ATT 规范推断 |
| 真实 notify 背压语义 | ❌ 未上板。`E_AGAIN`/`E_NOMEM` 的映射是推断 |
| 固件 RAM 增量 | ❌ 无 ARM 工具链。§5 只有结构体大小 |
| BlueZ（Linux）路径 | ❌ 本机是 macOS，无 BlueZ |

**这些不是「做完了」的遗漏，而是 M1 里程碑 E1/E2/E4 的待办**——
本版能做的是把接入点固定下来、把顺序和坑写清楚，让有板子的人不必再猜。
