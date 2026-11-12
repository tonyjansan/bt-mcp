/* test_client.c —— Bridge 侧 MCP 客户端（L5 client）+ 设备端 server 端到端。
 *
 * 拓扑：client session ←→ server session（同一进程，交叉 wire），
 * 验证：握手自动化、tools/list、tools/call、resources/read、通知上抛、超时。
 *
 * 编：cc -O2 -std=c99 -Wall -Wextra -Werror -Icore/include \
 *        tests/test_client.c core/src/frame.c core/src/session.c \
 *        core/src/json.c core/src/writer.c core/src/mcp_server.c \
 *        core/src/mcp_client.c -o build/test_client && ./build/test_client
 */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "bt_mcp/frame.h"
#include "bt_mcp/session.h"
#include "bt_mcp/mcp_server.h"
#include "bt_mcp/mcp_client.h"
#include "bt_mcp/err.h"

static int g_pass, g_fail;
#define CHECK(cond, msg) do { \
    if (cond) g_pass++; \
    else { g_fail++; printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, msg); } \
} while (0)

/* 帧队列：framer 每片一次 emit（v0.3），按 emit 边界切成独立记录，
 * 包通道逐条 feed（framer_feed 一次一片）。 */
#define MAX_FRAG 128
typedef struct {
    uint8_t  buf[65536];
    size_t   len;
    uint16_t frag[MAX_FRAG];   /* 每片长度（含帧头） */
    int      nfrag;
} vec_t;
static vec_t c2s, s2c;

static int q_put(vec_t *v, const uint8_t *d, uint16_t n)
{
    if (v->len + n > sizeof v->buf || v->nfrag >= MAX_FRAG) return BT_MCP_E_NOMEM;
    memcpy(v->buf + v->len, d, n);
    v->len += n;
    v->frag[v->nfrag++] = n;
    return 0;
}
static int c_put(void *io, const uint8_t *d, uint16_t n) { return q_put(io, d, n); }

static int tool_echo(const bt_mcp_json_req_t *req, bt_mcp_writer_t *out,
                     bt_mcp_call_t *call, void *user)
{
    (void)call; (void)user;
    const char *t; uint16_t tl;
    if (bt_mcp_json_get_str(req, "text", &t, &tl)) { bt_mcp_w_raw(out, "?", 1); return 0; }
    char b[128]; uint16_t need = 0;
    uint16_t n = bt_mcp_json_decode_str(t - 1, t + tl + 1, b, sizeof b - 1, &need);
    (void)need;
    return bt_mcp_w_raw(out, b, n);
}

static const char RES_TXT[] = "hello-resource";
static int res_read(void *user, uint32_t cursor, const char **text,
                    uint32_t *len, uint32_t *next)
{
    (void)user; (void)cursor;
    /* 一次性给完整内容（无分页） */
    *text = RES_TXT; *len = (uint32_t)(sizeof RES_TXT - 1);
    *next = BT_MCP_CURSOR_END;
    return 0;
}

static const bt_mcp_tool_def_t TOOLS[] = {
    { .name = "echo", .desc = "Echo",
      .input_schema_json =
          "{\"type\":\"object\",\"properties\":{\"text\":{\"type\":\"string\"}}}",
      .authz = BT_MCP_AUTHZ_OPEN, .fn = tool_echo },
};
static const bt_mcp_resource_def_t RES[] = {
    { .name = "greeting", .desc = "greeting text", .mime_type = "text/plain",
      .authz = BT_MCP_AUTHZ_OPEN, .read = res_read },
};

/* 交叉泵：客户端 wire → server，server wire → client */
static bt_mcp_framer_t  fr_c, fr_s;
static bt_mcp_session_t se_c, se_s;
static bt_mcp_server_t  sv;
static bt_mcp_client_t  cl;

static bt_mcp_client_event_t last_ev;
static int  ev_count;
static char last_result[2048];
static int  last_result_len;

static void on_event(void *user, const bt_mcp_client_event_t *ev)
{
    (void)user;
    last_ev = *ev;
    ev_count++;
    if (ev->result && ev->result_len < sizeof last_result) {
        memcpy(last_result, ev->result, ev->result_len);
        last_result[ev->result_len] = 0;
        last_result_len = (int)ev->result_len;
    }
}

static int feed_bridge(bt_mcp_framer_t *fr, bt_mcp_session_t *se,
                      bt_mcp_server_t *s, const uint8_t *d, uint16_t n);
static int feed_bridge_client(bt_mcp_framer_t *fr, bt_mcp_session_t *se,
                             bt_mcp_client_t *c, const uint8_t *d, uint16_t n);

/* 泵空两条线：逐片搬运（一条记录 = 一个包通道片） */
static void pump(void)
{
    for (int guard = 0; guard < 64; guard++) {
        int moved = 0;
        if (c2s.nfrag) {
            vec_t t = c2s; c2s.nfrag = 0; c2s.len = 0;
            size_t off = 0;
            for (int i = 0; i < t.nfrag; i++) {
                feed_bridge(&fr_s, &se_s, &sv, t.buf + off, t.frag[i]);
                off += t.frag[i];
            }
            moved = 1;
        }
        if (s2c.nfrag) {
            vec_t t = s2c; s2c.nfrag = 0; s2c.len = 0;
            size_t off = 0;
            for (int i = 0; i < t.nfrag; i++) {
                feed_bridge_client(&fr_c, &se_c, &cl, t.buf + off, t.frag[i]);
                off += t.frag[i];
            }
            moved = 1;
        }
        if (!moved) break;
    }
}

/* 两个桥接函数：framer → session_rx → L5 */
static bt_mcp_server_t *g_sv;
static bt_mcp_client_t *g_cl;

static void srv_sink(void *user, const uint8_t *json, uint32_t len)
{
    (void)user;
    bt_mcp_server_rx(g_sv, json, len);
}
static void cli_sink(void *user, const uint8_t *json, uint32_t len)
{
    (void)user;
    bt_mcp_client_rx(g_cl, json, len);
}

static int feed_bridge(bt_mcp_framer_t *fr, bt_mcp_session_t *se,
                       bt_mcp_server_t *s, const uint8_t *d, uint16_t n)
{
    (void)fr;
    g_sv = s;
    return bt_mcp_session_feed(se, d, n, srv_sink, NULL);
}

static int feed_bridge_client(bt_mcp_framer_t *fr, bt_mcp_session_t *se,
                              bt_mcp_client_t *c, const uint8_t *d, uint16_t n)
{
    (void)fr;
    g_cl = c;
    return bt_mcp_session_feed(se, d, n, cli_sink, NULL);
}

int main(void)
{
    bt_mcp_chan_cap_t cap = { BT_MCP_CHAN_PACKET, 244, 1, 1, 4 };  /* CoC 型：可靠+自带流控 */
    bt_mcp_frame_cfg_t fcfg = { .max_reasm = 4, .frag_tout_ms = 2000 };

    bt_mcp_framer_init(&fr_c, &fcfg, &cap);
    bt_mcp_framer_init(&fr_s, &fcfg, &cap);

    bt_mcp_session_cfg_t ccfg = { .req_timeout_ms = 30000, .resume_tout_ms = 5000,
                                  .tx_window = 4, .role = BT_MCP_ROLE_CLIENT };
    bt_mcp_session_cfg_t scfg = ccfg;
    scfg.role = BT_MCP_ROLE_SERVER;

    bt_mcp_session_init(&se_c, &ccfg);
    bt_mcp_session_init(&se_s, &scfg);
    bt_mcp_session_bind(&se_c, &fr_c, c_put, &c2s);
    bt_mcp_session_bind(&se_s, &fr_s, c_put, &s2c);

    bt_mcp_server_cfg_t svcfg = { .tools = TOOLS, .tools_count = 1,
                                   .resources = RES, .resources_count = 1 };
    bt_mcp_server_init(&sv, &se_s, &svcfg, "device", "0.3");
    bt_mcp_client_init(&cl, &se_c, on_event, NULL);

    bt_mcp_session_link_up(&se_c, 0);
    bt_mcp_session_link_up(&se_s, 0);

    /* 1. 客户端主动握手 */
    CHECK(bt_mcp_client_initialize(&cl, "test-bridge", "0.3") == 0, "initialize sent");
    pump();
    CHECK(ev_count == 1, "initialize response delivered");
    CHECK(!last_ev.is_error, "initialize ok");
    CHECK(strstr(last_result, "\"serverInfo\"") != NULL, "serverInfo present");
    CHECK(cl.initialized == 1, "client marked initialized");
    CHECK(bt_mcp_session_state(&se_c) == BT_MCP_S_READY, "client session READY");
    CHECK(bt_mcp_session_state(&se_s) == BT_MCP_S_READY, "server session READY");

    /* 2. tools/list */
    ev_count = 0;
    CHECK(bt_mcp_client_list_tools(&cl, NULL, NULL) == 0, "tools/list sent");
    pump();
    if (getenv("CL_DBG")) printf("[dbg] ev=%d err=%d result=%s\n", ev_count, last_ev.is_error, last_result);
    CHECK(ev_count == 1 && !last_ev.is_error, "tools/list responded");
    CHECK(strstr(last_result, "\"echo\"") != NULL, "echo tool listed");

    /* 3. tools/call */
    ev_count = 0;
    static const char ARGS[] = "{\"text\":\"ping\"}";
    CHECK(bt_mcp_client_call_tool(&cl, "echo", ARGS,
                                  (uint32_t)(sizeof ARGS - 1), NULL) == 0,
          "tools/call sent");
    pump();
    if (getenv("CL_DBG")) printf("[dbg call] ev=%d err=%d code=%d res=%.120s\n",
                                 ev_count, last_ev.is_error, last_ev.error_code, last_result);
    CHECK(ev_count == 1 && !last_ev.is_error, "tools/call responded");
    CHECK(strstr(last_result, "\"text\":\"ping\"") != NULL, "tool result text");

    /* 4. resources/read */
    ev_count = 0;
    CHECK(bt_mcp_client_read_resource(&cl, "bt-mcp://dev/greeting", NULL) == 0,
          "resources/read sent");
    pump();
    CHECK(ev_count == 1 && !last_ev.is_error, "resources/read responded");
    CHECK(strstr(last_result, "hello-resource") != NULL, "resource text");

    /* 5. 错误响应 */
    ev_count = 0;
    bt_mcp_client_request(&cl, "no/such/method", NULL, 0, NULL);
    pump();
    if (getenv("CL_DBG")) printf("[dbg unk] ev=%d err=%d code=%d\n",
                                 ev_count, last_ev.is_error, last_ev.error_code);
    CHECK(ev_count == 1 && last_ev.is_error, "unknown method → error event");
    CHECK(last_ev.error_code == -32601, "error code -32601");

    /* 6a. 未先发 initialize 时，任何响应都不得把客户端判成「已握手」 */
    {
        bt_mcp_client_t c2;
        bt_mcp_session_t s2;
        bt_mcp_session_cfg_t c2cfg = { .req_timeout_ms = 30000, .resume_tout_ms = 5000,
                                       .tx_window = 4, .role = BT_MCP_ROLE_CLIENT };
        static bt_mcp_framer_t f2;
        static vec_t w2;
        bt_mcp_framer_init(&f2, &(bt_mcp_frame_cfg_t){4, 2000}, &cap);
        bt_mcp_session_init(&s2, &c2cfg);
        memset(&w2, 0, sizeof w2);
        bt_mcp_session_bind(&s2, &f2, c_put, &w2);
        bt_mcp_session_link_up(&s2, 0);
        bt_mcp_session_mark_ready(&s2);
        bt_mcp_client_init(&c2, &s2, on_event, NULL);
        /* 直接发 tools/list（跳过 initialize）并伪造其响应 */
        uint32_t rid = 0;
        bt_mcp_client_list_tools(&c2, NULL, &rid);
        s2.consumed = s2.consumed;   /* no-op，保持可读性 */
        char resp[256];
        snprintf(resp, sizeof resp,
                 "{\"jsonrpc\":\"2.0\",\"id\":%u,\"result\":{\"tools\":[]}}",
                 (unsigned)rid);
        bt_mcp_client_rx(&c2, (const uint8_t *)resp, (uint32_t)strlen(resp));
        CHECK(c2.initialized == 0, "non-initialize response must not mark initialized");
        CHECK(bt_mcp_session_state(&s2) == BT_MCP_S_READY, "state untouched");
    }

    /* 6. 超时 */
    ev_count = 0;
    bt_mcp_client_tick(&cl, 0);
    bt_mcp_client_request(&cl, "tools/list", NULL, 0, NULL);
    bt_mcp_client_tick(&cl, 100000);
    CHECK(ev_count == 1 && last_ev.is_error, "timeout emits error event");
    CHECK(last_ev.error_code == -32000, "timeout code -32000");
    CHECK(bt_mcp_client_inflight(&cl) == 0, "no inflight after timeout");


    /* v0.7（A4）：工具名/URI 里的引号与反斜杠必须被 JSON 转义。
     * v0.6 及之前用 snprintf("%s") 直接拼接，遇到引号会生成**非法 JSON**。 */
    {
        static bt_mcp_chan_cap_t cap_s = { .kind = BT_MCP_CHAN_STREAM, .mtu = 0,
            .reliable = 1, .flow_ctrl = 0, .max_pending = 4 };
        static bt_mcp_frame_cfg_t cfg_s = { .max_reasm = 4, .frag_tout_ms = 2000 };
        static bt_mcp_framer_t f2; static bt_mcp_session_t s2;
        static bt_mcp_client_t c2; static vec_t w2;
        w2.len = 0; w2.nfrag = 0;
        bt_mcp_framer_init(&f2, &cfg_s, &cap_s);
        bt_mcp_session_cfg_t sc2 = { .req_timeout_ms = 30000,
                                     .resume_tout_ms = 5000, .tx_window = 8,
                                     .role = BT_MCP_ROLE_CLIENT };
        bt_mcp_session_init(&s2, &sc2);
        bt_mcp_session_bind(&s2, &f2, c_put, &w2);
        bt_mcp_session_link_up(&s2, 0);
        bt_mcp_session_mark_ready(&s2);
        bt_mcp_client_init(&c2, &s2, NULL, NULL);

        CHECK(bt_mcp_client_call_tool(&c2, "evil\"name", "{}", 2, NULL) == 0,
              "call_tool with quote in name ok");
        int esc = 0;
        for (size_t i = BT_MCP_HDR_LEN; i + 1 < w2.len; i++)
            if (w2.buf[i] == '\\' && w2.buf[i + 1] == '"') esc = 1;
        CHECK(esc, "quote in tool name is JSON-escaped");

        w2.len = 0; w2.nfrag = 0;
        CHECK(bt_mcp_client_read_resource(&c2, "bt-mcp://a\"b", NULL) == 0,
              "read_resource with quote in uri ok");
        esc = 0;
        for (size_t i = BT_MCP_HDR_LEN; i + 1 < w2.len; i++)
            if (w2.buf[i] == '\\' && w2.buf[i + 1] == '"') esc = 1;
        CHECK(esc, "quote in uri is JSON-escaped");
    }

    printf("\n%d pass, %d fail\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
