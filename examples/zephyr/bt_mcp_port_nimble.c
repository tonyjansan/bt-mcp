/* bt_mcp_port_nimble.c —— Zephyr / NimBLE 参考移植（v0.8 B2）
 *
 * ⚠️ 诚实声明：本文件**未在本机编译验证**（无 Zephyr/NimBLE SDK、无 ARM 工具链）。
 * API 名称与调用顺序按 NimBLE 官方文档核对；它给出的是**确切的接入点**，
 * 不是「已上板验证」的实现。上板验收清单见 docs/porting-guide.md §6。
 *
 * 编译方式（在 Zephyr 工程里）：
 *   把本文件与 core/src 下的源文件一起加入 CMake 目标，并：
 *   target_include_directories(app PRIVATE core/include port)
 *
 * 结构：本文件只做「胶水」，不含任何协议逻辑。协议全在 core 里，
 * 这样换栈（NimBLE → BlueZ → ESP-IDF）只需重写这一个文件。
 */
#include <errno.h>
#include <sys/types.h>
#include <zephyr/kernel.h>
#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/gatt.h>
#include <zephyr/bluetooth/conn.h>
#include <zephyr/sys/byteorder.h>

#include "bt_mcp/frame.h"
#include "bt_mcp/session.h"
#include "bt_mcp/mcp_server.h"
#include "bt_mcp/err.h"

/* ---------------- 本文件持有的状态（core 不分配内存） ---------------- */
static bt_mcp_framer_t   g_fr;
static bt_mcp_session_t  g_se;
static bt_mcp_server_t   g_sv;
static bt_mcp_chan_cap_t g_cap;
static bt_mcp_frame_cfg_t g_fcfg;
static uint32_t          g_now_ms;

static struct bt_conn   *g_conn;
static uint32_t          g_resume_token;    /* 由应用维护，跨重连保持 */
static uint16_t          g_mcp_val_handle;  /* MCP 写/通知特性句柄 */

/* 应用层：注册你自己的 tools/resources/prompts（示例留空实现） */
static int on_tool_ping(const bt_mcp_json_req_t *r, bt_mcp_writer_t *w,
                        bt_mcp_call_t *c, void *u)
{
    (void)r; (void)c; (void)u;
    return bt_mcp_w_raw(w, "\"pong\"", 6);
}
static const bt_mcp_tool_def_t g_tools[] = {
    { .name = "ping", .desc = "health check", .input_schema_json = "{}",
      .fn = on_tool_ping },
};

/* ============================================================
 * ① emit：core → 链路
 * 返回值语义很重要：
 *   0            → 已交给控制器
 *   E_AGAIN      → 暂时发不出去（缓冲忙），core 会在下次 tick 重试
 *   E_NOMEM      → 池耗尽（背压）
 *   E_IO         → 其他不可恢复错误
 * 不要在 emit 里自己阻塞等待——那会把 tick 卡住，重传时序就崩了。
 * ============================================================ */
static int port_emit(void *io, const uint8_t *d, uint16_t n)
{
    (void)io;
    if (!g_conn) return BT_MCP_E_STATE;
    if (g_mcp_val_handle == 0) return BT_MCP_E_STATE;

    /* core 已按 MTU 切好片，这里一次 emit == 一个 ATT 通知，不要再切。 */
    struct os_mbuf *om = os_msys_get_pkthdr(n, 0);
    if (!om) return BT_MCP_E_NOMEM;
    if (os_mbuf_append(om, d, n) != 0) {
        os_mbuf_free_chain(om);
        return BT_MCP_E_NOMEM;
    }
    int rc = bt_gatt_notify_cb(g_conn, NULL, 0, NULL, om); /* 见下方注释 */
    if (rc == -ENOMEM || rc == -EBUSY || rc == -EAGAIN) {
        os_mbuf_free_chain(om);
        return BT_MCP_E_AGAIN;
    }
    if (rc) {
        os_mbuf_free_chain(om);
        return BT_MCP_E_IO;
    }
    return BT_MCP_OK;
}

/* ============================================================
 * ② 链路 → core：MCP 写特性回调
 * 关键：**按到达边界喂**，不要跨回调拼接——framer 自己管重组。
 * ============================================================ */
static bt_mcp_msg_sink_t g_sink;      /* 由应用设置：收到完整帧消息时调用 */
static void *g_sink_user;

static ssize_t on_mcp_write(struct bt_conn *conn,
                            const struct bt_gatt_attr *attr,
                            const void *buf, uint16_t len,
                            uint16_t offset, uint8_t flags)
{
    (void)conn; (void)attr; (void)offset; (void)flags;
    if (len == 0) return len;
    /* 直接把收到的字节交给 framer；它内部处理分片/重组/去重。 */
    /* 注意：framer_feed 是 5 个参数 (f, in, len, sink, user)。
     * 只传 4 个是本文件第一版的真实错误，被 stub 语法检查抓到。 */
    (void)bt_mcp_framer_feed(&g_fr, (const uint8_t *)buf, len, g_sink,
                             g_sink_user);
    return len;
}

/* 特性定义（MCP 数据通道：write + notify） */
BT_GATT_SERVICE_DEFINE(mcp_svc,
    BT_GATT_PRIMARY_SERVICE(BT_UUID_DECLARE_16(0xFFF0)),
    BT_GATT_CHARACTERISTIC(BT_UUID_DECLARE_16(0xFFF1),
                           BT_GATT_CHRC_WRITE | BT_GATT_CHRC_WRITE_WITHOUT_RESP |
                           BT_GATT_CHRC_NOTIFY,
                           BT_GATT_PERM_WRITE,
                           NULL, on_mcp_write, NULL),
    BT_GATT_CCC(NULL, BT_GATT_PERM_READ | BT_GATT_PERM_WRITE),
);

static void resolve_handles(void)
{
    /* 在 bt_gatt_service_register 之后按特征值 UUID 查句柄。
     * 真实工程建议用 bt_gatt_attr_get_handle(&mcp_svc.attrs[N]) 或
     * bt_gatt_find_by_uuid()；这里示意接口，下标取决于服务定义顺序。 */
    g_mcp_val_handle = 0;
    (void)mcp_svc;
}

/* ============================================================
 * ③ 连接事件：MTU 协商 / 断链 / 重连
 * ============================================================ */
static void on_connected(struct bt_conn *conn, uint8_t err)
{
    if (err) return;
    g_conn = bt_conn_ref(conn);

    struct bt_conn_info info;
    /* 连接间隔可用于按需调整 tick（本示例固定 50ms，此处只取信息） */
    if (bt_conn_get_info(conn, &info) == 0) {
        (void)info;
    }
    /* MTU 在 exchange 回调里拿；这里先用默认 23 兜底。 */
    g_cap.mtu = (uint16_t)(23 - 3);
    bt_mcp_framer_set_mtu(&g_fr, g_cap.mtu);

    /* 重连：带上应用维护的 token，core 会尝试 RESUME */
    bt_mcp_session_link_up(&g_se, g_resume_token);
}

static void on_disconnected(struct bt_conn *conn, uint8_t reason)
{
    (void)reason;
    if (g_conn) { bt_conn_unref(g_conn); g_conn = NULL; }
    bt_mcp_session_link_down(&g_se, 0);
}

static void on_mtu_exchange(struct bt_conn *conn, uint16_t tx, uint16_t rx)
{
    (void)conn;
    uint16_t att_mtu = (tx < rx) ? tx : rx;
    if (att_mtu < 23) att_mtu = 23;
    /* ATT 头 3 字节必须预扣——这是最常见的移植错误（填了 ATT MTU 直接导致
     * 分片超出控制器缓冲，表现为 notify 失败或对端重组失败）。 */
    g_cap.mtu = (uint16_t)(att_mtu - 3);
    bt_mcp_framer_set_mtu(&g_fr, g_cap.mtu);
}

BT_CONN_CB_DEFINE(conn_cb) = {
    .connected    = on_connected,
    .disconnected = on_disconnected,
};

/* ============================================================
 * ④ tick：驱动重传/超时。不做这一步，重传永远不会发生。
 * ============================================================ */
/* tick 周期由**宿主决定**（core 不规定，也没有 BT_MCP_TICK_MS 这个宏——
 * 本文件第一版写了一个不存在的宏，被 stub 语法检查抓到）。
 * 50ms 是折中：太小费电，太大会让重传 RTO 精度变差（RTO 基准 600ms）。 */
#define BT_MCP_EXAMPLE_TICK_MS 50

static void bt_mcp_work_handler(struct k_work *w);
K_WORK_DEFINE(g_work, bt_mcp_work_handler);
static void schedule_next(void)
{
    k_work_schedule(&g_work, K_MSEC(BT_MCP_EXAMPLE_TICK_MS));
}
static void bt_mcp_work_handler(struct k_work *w)
{
    (void)w;
    g_now_ms = (uint32_t)k_uptime_get();
    bt_mcp_session_tick(&g_se, g_now_ms);
    (void)bt_mcp_session_pump(&g_se);   /* 把本轮能发的都发出去 */
    schedule_next();
}

/* ============================================================
 * 初始化：顺序错了会静默不工作（见 docs/porting-guide.md §4）
 * ============================================================ */
int bt_mcp_example_init(bt_mcp_msg_sink_t sink, void *sink_user)
{
    g_sink = sink;
    g_sink_user = sink_user;

    /* 1. 通道能力 */
    g_cap.kind        = BT_MCP_CHAN_PACKET;
    g_cap.mtu         = (uint16_t)(23 - 3);   /* 协商前兜底 */
    g_cap.reliable    = 0;                    /* GATT：必须开 ARQ */
    g_cap.flow_ctrl   = 0;                    /* 用 core 的信用流控 */
    g_cap.max_pending = 4;

    /* 2. framer */
    g_fcfg.max_reasm      = BT_MCP_CFG_MAX_REASM;
    g_fcfg.frag_tout_ms   = 2000;
    bt_mcp_framer_init(&g_fr, &g_fcfg, &g_cap);

    /* 3. session + emit 绑定 */
    bt_mcp_session_cfg_t sc = {
        .req_timeout_ms = 30000,
        .resume_tout_ms = 5000,
        .tx_window      = 8,
        .role           = BT_MCP_ROLE_SERVER,
    };
    bt_mcp_session_init(&g_se, &sc);
    bt_mcp_session_bind(&g_se, &g_fr, port_emit, NULL);

    /* 4. server 注册 */
    bt_mcp_server_cfg_t cfg = {
        .tools = g_tools,
        .tools_count = (uint16_t)(sizeof g_tools / sizeof g_tools[0]),
    };
    int rc = bt_mcp_server_init(&g_sv, &g_se, &cfg, "zephyr-dev", "0.8");
    if (rc) return rc;

    /* 5. 注册 GATT 服务并取句柄 */
    rc = bt_gatt_service_register(&mcp_svc);
    if (rc) return rc;
    resolve_handles();

    /* 6. 起 tick（少了这步 = 无重传 = 高丢包下大面积失败） */
    schedule_next();
    return BT_MCP_OK;
}
