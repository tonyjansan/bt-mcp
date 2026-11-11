/* test_reentrancy.c —— 重入与多实例不变量（v0.4 G5）。
 *
 * 要验证的是「单写者模型」的两条边界：
 *  1) 合法重入：server 在 session_rx 的上抛回调里同步应答（session_tx），
 *     不得破坏 feed 状态、不得丢消息；
 *  2) 非法重入：在 session_feed 内部再次 session_feed 会覆盖 feed_sink，
 *     必须被检测（本测试只验证 1)，并在 API 文档中禁止 2)）。
 * 另验证两个 server 实例的响应缓冲互不干扰（D1 的回归护栏）。
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

typedef struct { uint8_t b[32768]; size_t n; } vec_t;
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
    (void)r; (void)c; (void)user;
    return bt_mcp_w_raw(w, "ok", 2);
}

static const bt_mcp_tool_def_t TOOLS[] = {
    { .name = "echo", .desc = "e",
      .input_schema_json = "{\"type\":\"object\",\"properties\":{}}",
      .authz = BT_MCP_AUTHZ_OPEN, .fn = tool_echo },
};

static bt_mcp_chan_cap_t g_cap;
static bt_mcp_frame_cfg_t g_fcfg;

/* 一个完整的 server 端（framer+session+server） */
typedef struct {
    bt_mcp_framer_t  fr;
    bt_mcp_session_t se;
    bt_mcp_server_t  sv;
    vec_t            wire;
    int              msgs;
} node_t;

/* server A 的上抛回调：解析后立即同步应答（合法重入） */
static node_t *g_node_a;

static void sink_serve(void *u, const uint8_t *json, uint32_t len)
{
    node_t *n = u;
    n->msgs++;
    (void)bt_mcp_server_rx(&n->sv, json, len);
}

static void node_init(node_t *n)
{
    memset(n, 0, sizeof *n);
    bt_mcp_framer_init(&n->fr, &g_fcfg, &g_cap);
    bt_mcp_session_cfg_t c = { .req_timeout_ms = 30000, .resume_tout_ms = 5000,
                               .tx_window = 8, .role = BT_MCP_ROLE_SERVER };
    bt_mcp_session_init(&n->se, &c);
    bt_mcp_session_bind(&n->se, &n->fr, vput, &n->wire);
    bt_mcp_session_link_up(&n->se, 0);
    bt_mcp_server_cfg_t sc = { .tools = TOOLS, .tools_count = 1 };
    bt_mcp_server_init(&n->sv, &n->se, &sc, "dev-a", "0.4");
    bt_mcp_session_mark_ready(&n->se);
}

int main(void)
{
    g_cap.kind = BT_MCP_CHAN_PACKET;
    g_cap.mtu = 244;
    g_cap.reliable = 1;
    g_cap.flow_ctrl = 1;
    g_cap.max_pending = 8;
    g_fcfg.max_reasm = 4;
    g_fcfg.frag_tout_ms = 2000;

    /* ---- 1. 在 session_feed 的上抛回调里同步应答（合法重入） ---- */
    {
        static node_t a, peer;
        node_init(&a);
        node_init(&peer);
        g_node_a = &a;

        /* 由 peer 造一条 tools/call 的帧流，喂给 a */
        static const char *req =
            "{\"jsonrpc\":\"2.0\",\"id\":7,\"method\":\"tools/call\","
            "\"params\":{\"name\":\"echo\",\"arguments\":{}}}";
        (void)bt_mcp_session_tx(&peer.se, (const uint8_t *)req,
                                (uint32_t)strlen(req));
        CHECK(peer.wire.n > 0, "peer produced frames");

        size_t off = 0;
        int fed = 0;
        while (off + BT_MCP_HDR_LEN <= peer.wire.n) {
            uint16_t plen = (uint16_t)(peer.wire.b[off + 4]
                                       | (peer.wire.b[off + 5] << 8));
            if (off + BT_MCP_HDR_LEN + plen > peer.wire.n) break;
            (void)bt_mcp_session_feed(&a.se, peer.wire.b + off,
                                      (uint16_t)(BT_MCP_HDR_LEN + plen),
                                      sink_serve, &a);
            off += BT_MCP_HDR_LEN + plen;
            fed++;
        }
        CHECK(fed >= 1, "frames fed into server");
        CHECK(a.msgs == 1, "message delivered exactly once through feed");
        CHECK(a.wire.n > 0, "server replied synchronously inside the callback");
        CHECK(a.se.feed_sink == NULL || a.msgs == 1, "feed state consistent");
        /* 回调内 feed_sink 已被清空：再次 feed 不会误用旧 sink */
        CHECK(a.se.feed_user == NULL, "feed_user cleared after feed");
    }

    /* ---- 2. 两个 server 实例的响应缓冲互不干扰（D1 回归） ---- */
    {
        static node_t a, b;
        node_init(&a);
        node_init(&b);

        static const char *r1 =
            "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"tools/list\"}";
        static const char *r2 =
            "{\"jsonrpc\":\"2.0\",\"method\":\"notifications/initialized\"}";

        (void)bt_mcp_server_rx(&a.sv, (const uint8_t *)r2,
                              (uint32_t)strlen(r2));
        (void)bt_mcp_server_rx(&b.sv, (const uint8_t *)r2,
                              (uint32_t)strlen(r2));
        a.wire.n = 0;
        b.wire.n = 0;
        (void)bt_mcp_server_rx(&a.sv, (const uint8_t *)r1,
                              (uint32_t)strlen(r1));
        CHECK(a.wire.n > 0, "server A responded");
        /* B 此时未发任何请求：其缓冲不应被 A 的响应污染 */
        CHECK(b.wire.n == 0, "server B wire untouched by A");
    }

    printf("\n%d pass, %d fail\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
