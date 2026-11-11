/* test_l5_fuzz.c —— L5 入口的确定性随机输入（v0.5 V3 分支覆盖率）。
 *
 * 与 fuzz/fuzz_frame.c 互补：那个打帧层，这个直接打 JSON 层入口
 * （bt_mcp_server_rx / bt_mcp_client_rx）。随机但确定（固定种子），
 * 目标是覆盖「畸形但合法语法」的各种组合分支，并确保永不崩溃。
 */
#include <stdio.h>
#include <string.h>

#include "bt_mcp/frame.h"
#include "bt_mcp/session.h"
#include "bt_mcp/mcp_server.h"
#include "bt_mcp/mcp_client.h"
#include "bt_mcp/err.h"

static int g_pass, g_fail;
#define CHECK(c, m) do { if (c) g_pass++; else { g_fail++; \
    printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, m); } } while (0)

static uint32_t g_s = 0x1234ABCDu;
static uint32_t rnd(void)
{
    g_s = g_s * 1103515245u + 12345u;
    return (g_s >> 16) & 0x7FFF;
}

typedef struct { uint8_t b[65536]; size_t n; } vec_t;
static int vput(void *io, const uint8_t *d, uint16_t n)
{
    vec_t *v = io;
    if (v->n + n > sizeof v->b) return BT_MCP_E_NOMEM;
    memcpy(v->b + v->n, d, n); v->n += n; return BT_MCP_OK;
}
static int t_ok(const bt_mcp_json_req_t *r, bt_mcp_writer_t *w,
                bt_mcp_call_t *c, void *u)
{
    (void)r; (void)c; (void)u;
    return bt_mcp_w_raw(w, "ok", 2);
}
static const bt_mcp_tool_def_t TOOLS[] = {
    { .name = "ok", .desc = "d", .input_schema_json = "{}", .fn = t_ok },
};
static int res_read(void *u, uint32_t c, const char **t, uint32_t *l,
                    uint32_t *nx)
{
    (void)u; (void)c; *t = "x"; *l = 1; *nx = BT_MCP_CURSOR_END; return 0;
}
static const bt_mcp_resource_def_t RES[] = {
    { .name = "r", .desc = "d", .mime_type = "text/plain", .read = res_read },
};

/* 方法名池：命中的概率高于纯随机，能进到各方法分支 */
static const char *const METHODS[] = {
    "initialize", "notifications/initialized", "ping", "tools/list",
    "tools/call", "resources/list", "resources/read", "resources/subscribe",
    "resources/unsubscribe", "prompts/list", "prompts/get",
    "logging/setLevel", "notifications/cancelled", "nope",
};

static size_t gen_msg(char *out, size_t cap)
{
    size_t n = 0;
    const char *m = METHODS[rnd() % (sizeof METHODS / sizeof METHODS[0])];
    n += (size_t)snprintf(out + n, cap - n, "{\"jsonrpc\":\"2.0\"");
    /* id：数字/字符串/缺失 三种 */
    switch (rnd() % 3) {
    case 0: n += (size_t)snprintf(out + n, cap - n, ",\"id\":%u", rnd() % 50); break;
    case 1: n += (size_t)snprintf(out + n, cap - n, ",\"id\":\"s%u\"", rnd() % 9); break;
    default: break;
    }
    n += (size_t)snprintf(out + n, cap - n, ",\"method\":\"%s\"", m);
    /* params：随机形状 */
    switch (rnd() % 8) {
    case 0: break;                       /* 无 params */
    case 1: n += (size_t)snprintf(out + n, cap - n, ",\"params\":null"); break;
    case 2: n += (size_t)snprintf(out + n, cap - n, ",\"params\":[]"); break;
    case 3: n += (size_t)snprintf(out + n, cap - n, ",\"params\":{}"); break;
    case 4:
        n += (size_t)snprintf(out + n, cap - n,
                              ",\"params\":{\"name\":\"%s\"}",
                              (rnd() & 1) ? "ok" : "missing");
        break;
    case 5:
        n += (size_t)snprintf(out + n, cap - n,
                              ",\"params\":{\"uri\":\"bt-mcp://dev/%s\"}",
                              (rnd() & 1) ? "r" : "x");
        break;
    case 6:
        n += (size_t)snprintf(out + n, cap - n,
                              ",\"params\":{\"level\":\"%s\"}",
                              (rnd() & 1) ? "debug" : "bogus");
        break;
    default:
        n += (size_t)snprintf(out + n, cap - n,
                              ",\"params\":{\"requestId\":%u}", rnd() % 20);
        break;
    }
    /* 随机尾部：有时加 _meta，有时故意加垃圾（走严格拒绝分支） */
    if (rnd() % 4 == 0)
        n += (size_t)snprintf(out + n, cap - n,
                              ",\"_meta\":{\"progressToken\":%u}", rnd() % 5);
    n += (size_t)snprintf(out + n, cap - n, "}");
    if (rnd() % 16 == 0) out[n++] = ' ';   /* 合法尾随空白 */
    if (rnd() % 32 == 0) out[n++] = 'x';   /* 非法尾随垃圾 */
    return n;
}

int main(void)
{
    static bt_mcp_framer_t fr;
    static bt_mcp_session_t se;
    static bt_mcp_server_t sv;
    static vec_t wire;
    static bt_mcp_chan_cap_t cap = { .kind = BT_MCP_CHAN_PACKET, .mtu = 244,
                                     .reliable = 1, .flow_ctrl = 1,
                                     .max_pending = 4 };
    static bt_mcp_frame_cfg_t fcfg = { .max_reasm = 4, .frag_tout_ms = 2000 };
    bt_mcp_framer_init(&fr, &fcfg, &cap);
    bt_mcp_session_cfg_t sc = { .req_timeout_ms = 30000, .resume_tout_ms = 5000,
                                .tx_window = 8, .role = BT_MCP_ROLE_SERVER };
    bt_mcp_session_init(&se, &sc);
    bt_mcp_session_bind(&se, &fr, vput, &wire);
    bt_mcp_session_link_up(&se, 0);
    bt_mcp_server_cfg_t cfg = { .tools = TOOLS, .tools_count = 1,
                                .resources = RES, .resources_count = 1 };
    bt_mcp_server_init(&sv, &se, &cfg, "d", "0.5");

    /* 先用一条合法 initialize 把服务器推进到可用状态 */
    {
        const char *q = "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"initialize\","
                        "\"params\":{\"protocolVersion\":\"2025-11-25\"}}";
        (void)bt_mcp_server_rx(&sv, (const uint8_t *)q, (uint32_t)strlen(q));
        const char *q2 = "{\"jsonrpc\":\"2.0\",\"method\":"
                         "\"notifications/initialized\"}";
        (void)bt_mcp_server_rx(&sv, (const uint8_t *)q2, (uint32_t)strlen(q2));
    }

    /* 随机消息：只要求「返回码属于允许集合且不崩溃」 */
    char msg[512];
    int bad_rc = 0;
    for (int i = 0; i < 200000; i++) {
        size_t n = gen_msg(msg, sizeof msg);
        wire.n = 0;
        int rc = bt_mcp_server_rx(&sv, (const uint8_t *)msg, (uint32_t)n);
        if (rc != 0 && rc != BT_MCP_E_PARSE && rc != BT_MCP_E_TOOBIG
            && rc != BT_MCP_E_NOMEM && rc != BT_MCP_E_STATE
            && rc != BT_MCP_E_ARGS && rc != BT_MCP_E_IO
            && rc != BT_MCP_E_AGAIN && rc != BT_MCP_E_LOCKED
            && rc != BT_MCP_E_AUTHZ && rc != BT_MCP_E_BUSY) bad_rc++;
        /* 偶尔 tick，覆盖超时分支 */
        if ((i & 1023) == 0) bt_mcp_server_tick(&sv, (uint32_t)i * 100u);
    }
    CHECK(bad_rc == 0, "server_rx always returns a known error code");
    CHECK(1, "200k random L5 inputs: no crash");

    /* 客户端侧同样打一轮（响应/通知/垃圾混杂） */
    {
        static bt_mcp_framer_t cf;
        static bt_mcp_session_t cs;
        static bt_mcp_client_t cl;
        static vec_t cw;
        bt_mcp_framer_init(&cf, &fcfg, &cap);
        bt_mcp_session_cfg_t cc = { .req_timeout_ms = 30000,
                                    .resume_tout_ms = 5000, .tx_window = 8,
                                    .role = BT_MCP_ROLE_CLIENT };
        bt_mcp_session_init(&cs, &cc);
        bt_mcp_session_bind(&cs, &cf, vput, &cw);
        bt_mcp_session_link_up(&cs, 0);
        bt_mcp_client_init(&cl, &cs, NULL, NULL);
        int cbad = 0;
        for (int i = 0; i < 100000; i++) {
            size_t n = gen_msg(msg, sizeof msg);
            int rc = bt_mcp_client_rx(&cl, (const uint8_t *)msg, (uint32_t)n);
            if (rc != 0 && rc != BT_MCP_E_PARSE) cbad++;
        }
        CHECK(cbad == 0, "client_rx returns 0 or E_PARSE");
    }

    printf("\n%d pass, %d fail\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
