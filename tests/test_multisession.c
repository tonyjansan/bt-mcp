/* test_multisession.c —— 多会话隔离（v0.5 V10）。
 *
 * 场景：一个进程同时服务两台设备（手机 + 手表）。每个会话有自己的
 * initialized / log_level / proto / 订阅集合 / 调用槽 —— 任何一项串台都是 bug。
 * 本测试把它们逐个验证，并验证 pool 按 session 正确分派。
 */
#include <stdio.h>
#include <string.h>

#include "bt_mcp/frame.h"
#include "bt_mcp/session.h"
#include "bt_mcp/mcp_server.h"
#include "bt_mcp/err.h"

static int g_pass, g_fail;
#define CHECK(c, m) do { if (c) g_pass++; else { g_fail++; \
    printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, m); } } while (0)

typedef struct { uint8_t b[16384]; size_t n; } vec_t;
static int vput(void *io, const uint8_t *d, uint16_t n)
{
    vec_t *v = io;
    if (v->n + n > sizeof v->b) return BT_MCP_E_NOMEM;
    memcpy(v->b + v->n, d, n);
    v->n += n;
    return 0;
}

static int tool_echo(const bt_mcp_json_req_t *r, bt_mcp_writer_t *w,
                     bt_mcp_call_t *c, void *user)
{
    (void)c; (void)user;
    const char *t; uint16_t tl;
    if (bt_mcp_json_get_str(r, "text", &t, &tl)) return BT_MCP_E_ARGS;
    return bt_mcp_w_raw(w, t, tl);
}
static int res_read(void *u, uint32_t cur, const char **t, uint32_t *l,
                    uint32_t *nx)
{
    (void)u; (void)cur; *t = "x"; *l = 1; *nx = BT_MCP_CURSOR_END; return 0;
}
static int res_sub(void *u, int s) { (void)u; (void)s; return 0; }

static const bt_mcp_tool_def_t TOOLS[] = {
    { .name = "echo", .desc = "e",
      .input_schema_json = "{\"type\":\"object\",\"properties\":{}}",
      .authz = BT_MCP_AUTHZ_OPEN, .fn = tool_echo },
};
static const bt_mcp_resource_def_t RES[] = {
    { .name = "r", .desc = "d", .mime_type = "text/plain",
      .authz = BT_MCP_AUTHZ_OPEN, .read = res_read, .sub = res_sub },
};

typedef struct { bt_mcp_framer_t fr; bt_mcp_session_t se; bt_mcp_server_t sv;
                 vec_t wire; } conn_t;

static bt_mcp_chan_cap_t g_cap;
static bt_mcp_frame_cfg_t g_fcfg;
static bt_mcp_server_cfg_t g_scfg;

static void conn_init(conn_t *c)
{
    memset(c, 0, sizeof *c);
    bt_mcp_framer_init(&c->fr, &g_fcfg, &g_cap);
    bt_mcp_session_cfg_t sc = { .req_timeout_ms = 30000, .resume_tout_ms = 5000,
                                .tx_window = 8, .role = BT_MCP_ROLE_SERVER };
    bt_mcp_session_init(&c->se, &sc);
    bt_mcp_session_bind(&c->se, &c->fr, vput, &c->wire);
    bt_mcp_session_link_up(&c->se, 0);
    bt_mcp_server_init(&c->sv, &c->se, &g_scfg, "dev", "0.5");
}

static int send_json(conn_t *c, const char *json)
{
    c->wire.n = 0;
    return bt_mcp_server_rx(&c->sv, (const uint8_t *)json,
                            (uint32_t)strlen(json));
}

int main(void)
{
    g_cap.kind = BT_MCP_CHAN_PACKET;
    g_cap.mtu = 244;
    g_cap.reliable = 1;
    g_cap.flow_ctrl = 1;
    g_cap.max_pending = 4;
    g_fcfg.max_reasm = 4;
    g_fcfg.frag_tout_ms = 2000;
    g_scfg.tools = TOOLS;
    g_scfg.tools_count = 1;
    g_scfg.resources = RES;
    g_scfg.resources_count = 1;

    static conn_t phone, watch;
    conn_init(&phone);
    conn_init(&watch);

    static bt_mcp_server_pool_t pool;
    bt_mcp_pool_init(&pool);
    CHECK(bt_mcp_pool_add(&pool, &phone.sv) == 0, "pool add phone");
    CHECK(bt_mcp_pool_add(&pool, &watch.sv) == 0, "pool add watch");
    CHECK(bt_mcp_pool_count(&pool) == 2, "pool count 2");
    CHECK(bt_mcp_pool_add(&pool, &phone.sv) == 0 && bt_mcp_pool_count(&pool) == 2,
          "pool add is idempotent");

    /* ---- 1. initialized 状态彼此独立 ---- */
    {
        CHECK(send_json(&phone, "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":"
                                "\"initialize\",\"params\":{\"protocolVersion\":"
                                "\"2025-03-26\",\"capabilities\":{}}}") == 0,
              "phone initialize");
        CHECK(send_json(&watch, "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":"
                                "\"initialize\",\"params\":{\"protocolVersion\":"
                                "\"2025-06-18\",\"capabilities\":{}}}") == 0,
              "watch initialize");
        CHECK(phone.sv.initialized == 0 && watch.sv.initialized == 0,
              "neither initialized yet");
        CHECK(send_json(&phone, "{\"jsonrpc\":\"2.0\",\"method\":"
                                "\"notifications/initialized\"}") == 0,
              "phone initialized notification");
        CHECK(phone.sv.initialized == 1, "phone initialized");
        CHECK(watch.sv.initialized == 0, "watch still not initialized");
    }

    /* ---- 2. 协商版本彼此独立（决定 batch 行为） ---- */
    {
        CHECK(strcmp(phone.sv.proto, "2025-03-26") == 0, "phone proto = 2025-03-26");
        CHECK(strcmp(watch.sv.proto, "2025-06-18") == 0, "watch proto = 2025-06-18");
        /* 手机（旧版本）接受 batch */
        CHECK(send_json(&phone, "[{\"jsonrpc\":\"2.0\",\"id\":2,"
                                "\"method\":\"ping\"}]") == 0, "phone batch ok");
        CHECK(phone.wire.n > 0, "phone batch responded");
        const char *p = (const char *)phone.wire.b;
        CHECK(p[6] == '[', "phone batch response is array");
        /* 手表（新版本）拒绝 batch。先让它 initialized。 */
        CHECK(send_json(&watch, "{\"jsonrpc\":\"2.0\",\"method\":"
                                "\"notifications/initialized\"}") == 0,
              "watch initialized notification");
        CHECK(send_json(&watch, "[{\"jsonrpc\":\"2.0\",\"id\":2,"
                                "\"method\":\"ping\"}]") == 0, "watch batch rejected");
        /* 注意：wire 是**帧字节**（含 0x00），不能用 strstr —— 会撞上 NUL 截断。
         * 用长度感知的查找，先剥掉 6 字节帧头。 */
        const char *needle = "-32600";
        size_t nn = 6, hit = 0;
        for (size_t i = 6; i + 6 <= watch.wire.n; i++)
            if (!memcmp(watch.wire.b + i, needle, nn)) { hit = 1; break; }
        CHECK(hit, "watch gets -32600");
    }

    /* ---- 3. log_level 彼此独立 ---- */
    {
        CHECK(send_json(&phone, "{\"jsonrpc\":\"2.0\",\"id\":3,\"method\":"
                                "\"logging/setLevel\",\"params\":{\"level\":"
                                "\"error\"}}") == 0, "phone setLevel error");
        CHECK(phone.sv.log_level == BT_MCP_LOG_ERROR, "phone log_level = error");
        CHECK(watch.sv.log_level == BT_MCP_LOG_INFO, "watch log_level unchanged");

        phone.wire.n = 0;
        CHECK(bt_mcp_server_log(&phone.sv, BT_MCP_LOG_WARNING, "d", "w", 1) == 0,
              "phone log warning");
        CHECK(phone.wire.n == 0, "phone drops below threshold");
        watch.wire.n = 0;
        CHECK(bt_mcp_server_log(&watch.sv, BT_MCP_LOG_WARNING, "d", "w", 1) == 0,
              "watch log warning");
        CHECK(watch.wire.n > 0, "watch keeps warning (its level is info)");
    }

    /* ---- 4. 订阅状态彼此独立 ---- */
    {
        CHECK(send_json(&phone, "{\"jsonrpc\":\"2.0\",\"id\":4,\"method\":"
                                "\"resources/subscribe\",\"params\":"
                                "{\"uri\":\"bt-mcp://dev/r\"}}") == 0,
              "phone subscribe");
        CHECK(phone.sv.nsubs == 1, "phone has 1 subscription");
        CHECK(watch.sv.nsubs == 0, "watch has none");
        /* 手表未订阅 → 不产生通知；手机已订阅 → 产生 */
        watch.wire.n = 0;
        CHECK(bt_mcp_server_notify_resource_updated(&watch.sv,
              "bt-mcp://dev/r") == 0, "watch notify");
        CHECK(watch.wire.n == 0, "watch: no notification (not subscribed)");
        phone.wire.n = 0;
        CHECK(bt_mcp_server_notify_resource_updated(&phone.sv,
              "bt-mcp://dev/r") == 0, "phone notify");
        CHECK(phone.wire.n > 0, "phone: notification emitted");
    }

    /* ---- 5. pool 按 session 分派（不会串台） ---- */
    {
        watch.wire.n = 0;
        const char *q = "{\"jsonrpc\":\"2.0\",\"id\":9,\"method\":\"tools/list\"}";
        CHECK(bt_mcp_pool_rx(&pool, &watch.se, (const uint8_t *)q,
                            (uint32_t)strlen(q)) == 0, "pool rx for watch");
        CHECK(watch.wire.n > 0, "watch got the response");
        phone.wire.n = 0;
        CHECK(bt_mcp_pool_rx(&pool, &phone.se, (const uint8_t *)q,
                            (uint32_t)strlen(q)) == 0, "pool rx for phone");
        CHECK(phone.wire.n > 0, "phone got its own response");

        /* 未挂载的 session → 明确报错 */
        bt_mcp_session_t stray;
        memset(&stray, 0, sizeof stray);
        CHECK(bt_mcp_pool_rx(&pool, &stray, (const uint8_t *)q,
                            (uint32_t)strlen(q)) == BT_MCP_E_ARGS,
              "unmounted session rejected");
    }

    /* ---- 6. 调用槽彼此独立 ---- */
    {
        CHECK(phone.sv.cfg.tools_count == 1, "cfg copied per instance");
        for (int i = 0; i < BT_MCP_CFG_MAX_PENDING; i++)
            CHECK(phone.sv.calls[i].in_use == 0, "phone call slot free");
    }

    printf("\n%d pass, %d fail\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
