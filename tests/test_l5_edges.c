/* test_l5_edges.c —— L5 方法级错误路径（v0.5 V3 分支覆盖率）。
 *
 * 目标：把每个 MCP 方法的**参数缺失/类型错/越界**分支都跑一遍。
 * 这些分支在正常会话里永不执行，是分支覆盖率的主要缺口，也是「畸形输入
 * 是否会让服务器崩溃或返回错码」的真正检验点。
 */
#include <stdio.h>
#include <string.h>

#include "bt_mcp/frame.h"
#include "bt_mcp/session.h"
#include "bt_mcp/mcp_server.h"
#include "bt_mcp/json.h"
#include "bt_mcp/err.h"

static int g_pass, g_fail;
#define CHECK(c, m) do { if (c) g_pass++; else { g_fail++; \
    printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, m); } } while (0)

typedef struct { uint8_t b[65536]; size_t n; } vec_t;
static int vput(void *io, const uint8_t *d, uint16_t n)
{
    vec_t *v = io;
    if (v->n + n > sizeof v->b) return BT_MCP_E_NOMEM;
    memcpy(v->b + v->n, d, n); v->n += n; return BT_MCP_OK;
}

/* 工具：若干形态各异的实现，用于覆盖 content 构建器的各分支 */
static int t_ok(const bt_mcp_json_req_t *r, bt_mcp_writer_t *w,
                bt_mcp_call_t *c, void *u)
{
    (void)r; (void)c; (void)u;
    return bt_mcp_w_raw(w, "ok", 2);
}
static int t_big(const bt_mcp_json_req_t *r, bt_mcp_writer_t *w,
                 bt_mcp_call_t *c, void *u)
{
    (void)r; (void)c; (void)u;
    char chunk[200];
    memset(chunk, 'Z', sizeof chunk);
    int rc = 0;
    for (int i = 0; i < 60 && !rc; i++) rc = bt_mcp_w_raw(w, chunk, sizeof chunk);
    return rc;
}
static int t_pending(const bt_mcp_json_req_t *r, bt_mcp_writer_t *w,
                     bt_mcp_call_t *c, void *u)
{
    (void)r; (void)w; (void)c; (void)u;
    return BT_MCP_E_PENDING;
}
static int t_meta(const bt_mcp_json_req_t *r, bt_mcp_writer_t *w,
                  bt_mcp_call_t *c, void *u)
{
    (void)r; (void)u;
    if (bt_mcp_call_set_meta(c, "k\":1", 4) == BT_MCP_OK) { }
    /* 故意传超长 → E_TOOBIG 分支 */
    static char huge[BT_MCP_CFG_META_MAX + 8];
    memset(huge, 'a', sizeof huge);
    CHECK(bt_mcp_call_set_meta(c, huge, sizeof huge) == BT_MCP_E_TOOBIG,
          "meta over limit rejected");
    return bt_mcp_w_content_text(w, "m", 1);
}
/* 异步 + 进度：先发两条进度，再把槽留给调用方（返回 PENDING） */
static int t_progress(const bt_mcp_json_req_t *r, bt_mcp_writer_t *w,
                      bt_mcp_call_t *c, void *u)
{
    (void)r; (void)w; (void)u;
    (void)bt_mcp_call_progress_msg(c, 1, 2, "half");
    (void)bt_mcp_call_progress(c, 2, 2);
    return BT_MCP_E_PENDING;
}

static const bt_mcp_tool_def_t TOOLS[] = {
    { .name = "ok", .desc = "d", .input_schema_json = "{}", .fn = t_ok },
    { .name = "big", .desc = "d", .input_schema_json = "{}", .fn = t_big },
    { .name = "pending", .desc = "d", .input_schema_json = "{}", .fn = t_pending },
    { .name = "meta", .desc = "d", .input_schema_json = "{}", .fn = t_meta },
    { .name = "prog", .desc = "d", .input_schema_json = "{}", .fn = t_progress },
};

static int res_read(void *u, uint32_t cur, const char **t, uint32_t *l,
                    uint32_t *nx)
{
    (void)u;
    if (cur == 0) { *t = "ab"; *l = 2; *nx = 2; return 0; }
    if (cur == 2) { *t = "cd"; *l = 2; *nx = BT_MCP_CURSOR_END; return 0; }
    *t = ""; *l = 0; *nx = BT_MCP_CURSOR_END;
    return 0;                      /* 越界游标 → 空内容 */
}
static int res_fail(void *u, uint32_t cur, const char **t, uint32_t *l,
                    uint32_t *nx)
{
    (void)u; (void)cur; (void)t; (void)l; (void)nx;
    return -1;                     /* 读失败分支 */
}
static int res_sub(void *u, int s) { (void)u; return s ? -1 : 0; }

static const bt_mcp_resource_def_t RES[] = {
    { .name = "r", .desc = "d", .mime_type = "text/plain",
      .read = res_read, .sub = res_sub },
    { .name = "bad", .desc = "d", .mime_type = "text/plain", .read = res_fail },
    { .name = "nosub", .desc = "d", .mime_type = "text/plain", .read = res_read },
};

static int prompt_ok(const bt_mcp_json_req_t *r, bt_mcp_writer_t *w, void *u)
{
    (void)r; (void)u;
    int rc = bt_mcp_w_key(w, "messages");
    if (!rc) rc = bt_mcp_w_raw(w, "[]", 2);
    w->need_comma = 1;
    return rc;
}
static const bt_mcp_prompt_def_t PROMPTS[] = {
    { .name = "p", .desc = "d", .get = prompt_ok },
    { .name = "nofn", .desc = "d", .get = NULL },
};

static bt_mcp_framer_t fr;
static bt_mcp_session_t se;
static bt_mcp_server_t sv;
static vec_t wire;
static bt_mcp_chan_cap_t cap;
static bt_mcp_frame_cfg_t fcfg;

static int rpc(const char *j)
{
    wire.n = 0;
    return bt_mcp_server_rx(&sv, (const uint8_t *)j, (uint32_t)strlen(j));
}
/* 在最近一次响应里找子串（长度感知，避开帧头里的 0x00） */
static int has(const char *needle)
{
    size_t nn = strlen(needle);
    for (size_t i = BT_MCP_HDR_LEN; i + nn <= wire.n; i++)
        if (!memcmp(wire.b + i, needle, nn)) return 1;
    return 0;
}

int main(void)
{
    cap.kind = BT_MCP_CHAN_PACKET;
    cap.mtu = 244;
    cap.reliable = 1;
    cap.flow_ctrl = 1;
    cap.max_pending = 4;
    fcfg.max_reasm = 4;
    fcfg.frag_tout_ms = 2000;
    bt_mcp_framer_init(&fr, &fcfg, &cap);
    bt_mcp_session_cfg_t sc = { .req_timeout_ms = 30000, .resume_tout_ms = 5000,
                                .tx_window = 8, .role = BT_MCP_ROLE_SERVER };
    bt_mcp_session_init(&se, &sc);
    bt_mcp_session_bind(&se, &fr, vput, &wire);
    bt_mcp_session_link_up(&se, 0);
    bt_mcp_server_cfg_t cfg = { .tools = TOOLS,
        .tools_count = (uint16_t)(sizeof TOOLS / sizeof TOOLS[0]),
        .resources = RES, .resources_count = 3,
        .prompts = PROMPTS, .prompts_count = 2 };
    bt_mcp_server_init(&sv, &se, &cfg, "d", "0.5");

    /* 握手 */
    CHECK(rpc("{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"initialize\","
              "\"params\":{\"protocolVersion\":\"2025-11-25\"}}") == 0, "init");
    CHECK(rpc("{\"jsonrpc\":\"2.0\",\"method\":\"notifications/initialized\"}") == 0,
          "initialized");

    /* ---- tools/call 的各种参数畸形 ---- */
    CHECK(rpc("{\"jsonrpc\":\"2.0\",\"id\":10,\"method\":\"tools/call\"}") == 0,
          "call without params");
    CHECK(has("-32602"), "call without params -> -32602");
    CHECK(rpc("{\"jsonrpc\":\"2.0\",\"id\":11,\"method\":\"tools/call\","
              "\"params\":{}}") == 0, "call without name");
    CHECK(has("-32602"), "call without name -> -32602");
    CHECK(rpc("{\"jsonrpc\":\"2.0\",\"id\":12,\"method\":\"tools/call\","
              "\"params\":{\"name\":123}}") == 0, "call name wrong type");
    CHECK(has("-32602"), "name wrong type -> -32602");
    /* 结果超 MAX_MSG → -32603 Result too large */
    CHECK(rpc("{\"jsonrpc\":\"2.0\",\"id\":13,\"method\":\"tools/call\","
              "\"params\":{\"name\":\"big\"}}") == 0, "call big");
    CHECK(has("Result too large") || has("isError"), "big result handled");
    /* 异步：pending 不产生响应 */
    wire.n = 0;
    CHECK(bt_mcp_server_rx(&sv, (const uint8_t *)
          "{\"jsonrpc\":\"2.0\",\"id\":14,\"method\":\"tools/call\","
          "\"params\":{\"name\":\"pending\"}}",
          (uint32_t)strlen("{\"jsonrpc\":\"2.0\",\"id\":14,\"method\":\"tools/call\","
                           "\"params\":{\"name\":\"pending\"}}")) == 0,
          "pending call returns 0");
    CHECK(wire.n == 0, "pending produces no immediate response");
    /* 取消这个挂起调用 */
    CHECK(rpc("{\"jsonrpc\":\"2.0\",\"method\":\"notifications/cancelled\","
              "\"params\":{\"requestId\":14}}") == 0, "cancel pending");
    /* meta + progress */
    CHECK(rpc("{\"jsonrpc\":\"2.0\",\"id\":15,\"method\":\"tools/call\","
              "\"params\":{\"name\":\"meta\"}}") == 0, "call meta");
    CHECK(has("_meta"), "meta emitted");
    CHECK(rpc("{\"jsonrpc\":\"2.0\",\"id\":16,\"method\":\"tools/call\","
              "\"params\":{\"name\":\"prog\",\"_meta\":"
              "{\"progressToken\":\"tok\"}}}") == 0, "call with str token");
    /* 字符串 progressToken 的进度通知 */
    wire.n = 0;
    {
        bt_mcp_call_t *c = NULL;
        for (int i = 0; i < BT_MCP_CFG_MAX_PENDING; i++)
            if (sv.calls[i].in_use && sv.calls[i].has_progress) {
                c = &sv.calls[i]; break;
            }
        CHECK(c != NULL, "call slot held for async");
        if (c) {
            CHECK(bt_mcp_call_progress_msg(c, 1, 2, "msg") == 0, "progress msg");
            CHECK(bt_mcp_server_complete(c, "done", 0) == 0, "complete async");
        }
    }

    /* ---- resources/read 参数畸形与边界 ---- */
    CHECK(rpc("{\"jsonrpc\":\"2.0\",\"id\":20,\"method\":\"resources/read\"}") == 0,
          "read without params");
    CHECK(has("-32602"), "read without uri -> -32602");
    CHECK(rpc("{\"jsonrpc\":\"2.0\",\"id\":21,\"method\":\"resources/read\","
              "\"params\":{\"uri\":\"not-a-uri\"}}") == 0, "read bad uri");
    CHECK(has("-32602"), "bad uri -> -32602");
    CHECK(rpc("{\"jsonrpc\":\"2.0\",\"id\":22,\"method\":\"resources/read\","
              "\"params\":{\"uri\":\"bt-mcp://dev/\"}}") == 0, "read empty name");
    CHECK(has("-32602"), "empty name -> -32602");
    CHECK(rpc("{\"jsonrpc\":\"2.0\",\"id\":23,\"method\":\"resources/read\","
              "\"params\":{\"uri\":\"bt-mcp://dev/r?cursor=2\"}}") == 0,
          "read with uri cursor");
    CHECK(has("cd"), "uri cursor honored");
    CHECK(rpc("{\"jsonrpc\":\"2.0\",\"id\":24,\"method\":\"resources/read\","
              "\"params\":{\"uri\":\"bt-mcp://dev/r\",\"cursor\":\"99\"}}") == 0,
          "read beyond end");
    CHECK(has("contents"), "beyond-end returns empty contents");
    CHECK(rpc("{\"jsonrpc\":\"2.0\",\"id\":25,\"method\":\"resources/read\","
              "\"params\":{\"uri\":\"bt-mcp://dev/bad\"}}") == 0, "read read-fails");
    CHECK(has("-32002"), "read failure -> -32002");

    /* ---- 订阅错误路径 ---- */
    CHECK(rpc("{\"jsonrpc\":\"2.0\",\"id\":30,\"method\":\"resources/subscribe\"}")
          == 0, "subscribe without params");
    CHECK(has("-32602"), "subscribe no uri -> -32602");
    CHECK(rpc("{\"jsonrpc\":\"2.0\",\"id\":31,\"method\":\"resources/subscribe\","
              "\"params\":{\"uri\":\"bt-mcp://dev/nosub\"}}") == 0,
          "subscribe unsupported");
    CHECK(has("-32602"), "resource without sub -> -32602");
    CHECK(rpc("{\"jsonrpc\":\"2.0\",\"id\":32,\"method\":\"resources/subscribe\","
              "\"params\":{\"uri\":\"bt-mcp://dev/r\"}}") == 0, "subscribe fail cb");
    CHECK(has("-32603"), "subscribe callback failure -> -32603");
    /* 未订阅时 notify_resource_updated 静默 */
    wire.n = 0;
    CHECK(bt_mcp_server_notify_resource_updated(&sv, "bt-mcp://dev/nosub") == 0,
          "notify unsubscribed ok");
    CHECK(wire.n == 0, "no notification when not subscribed");
    CHECK(bt_mcp_server_notify_resource_updated(&sv, "bad-uri")
          == BT_MCP_E_PARSE, "notify bad uri -> E_PARSE");

    /* ---- prompts 错误路径 ---- */
    CHECK(rpc("{\"jsonrpc\":\"2.0\",\"id\":40,\"method\":\"prompts/get\"}") == 0,
          "prompt get without params");
    CHECK(has("-32602"), "prompt get no name -> -32602");
    CHECK(rpc("{\"jsonrpc\":\"2.0\",\"id\":41,\"method\":\"prompts/get\","
              "\"params\":{\"name\":\"nofn\"}}") == 0, "prompt without get fn");
    CHECK(has("Prompt not found"), "prompt without get -> not found");
    CHECK(rpc("{\"jsonrpc\":\"2.0\",\"id\":42,\"method\":\"prompts/get\","
              "\"params\":{\"name\":\"missing\"}}") == 0, "prompt missing");
    CHECK(has("-32602"), "prompt missing -> -32602");

    /* ---- logging/setLevel 错误路径 ---- */
    CHECK(rpc("{\"jsonrpc\":\"2.0\",\"id\":50,\"method\":\"logging/setLevel\"}") == 0,
          "setLevel without params");
    CHECK(has("-32602"), "setLevel no level -> -32602");
    CHECK(rpc("{\"jsonrpc\":\"2.0\",\"id\":51,\"method\":\"logging/setLevel\","
              "\"params\":{\"level\":\"verbose\"}}") == 0, "setLevel bad level");
    CHECK(has("-32602"), "bad level -> -32602");
    CHECK(rpc("{\"jsonrpc\":\"2.0\",\"id\":52,\"method\":\"logging/setLevel\","
              "\"params\":{\"level\":\"emergency\"}}") == 0, "setLevel emergency");
    CHECK(sv.log_level == BT_MCP_LOG_EMERGENCY, "log_level updated to max");

    /* ---- 通知/请求的边界 ---- */
    CHECK(rpc("{\"jsonrpc\":\"2.0\",\"id\":60,\"method\":\"notifications/cancelled\","
              "\"params\":{}}") == 0, "cancel without requestId");
    CHECK(rpc("{\"jsonrpc\":\"2.0\",\"id\":61,\"method\":\"notifications/cancelled\","
              "\"params\":{\"requestId\":\"str-id\"}}") == 0, "cancel str id");
    CHECK(rpc("{\"jsonrpc\":\"2.0\",\"id\":62,\"method\":\"notifications/cancelled\","
              "\"params\":{\"requestId\":99999}}") == 0, "cancel unknown id");
    /* 无 id 的未知方法：通知语义 → 不回错误 */
    wire.n = 0;
    CHECK(rpc("{\"jsonrpc\":\"2.0\",\"method\":\"notifications/other\"}") == 0,
          "unknown notification ignored");
    CHECK(wire.n == 0, "unknown notification gets no response");
    /* 超长字符串 id → -32600 + id:null */
    {
        char q[512]; int n = 0;
        n += snprintf(q, sizeof q, "{\"jsonrpc\":\"2.0\",\"id\":\"");
        for (int i = 0; i < 200; i++) q[n++] = 'x';
        n += snprintf(q + n, sizeof q - (size_t)n, "\",\"method\":\"ping\"}");
        wire.n = 0;
        CHECK(bt_mcp_server_rx(&sv, (const uint8_t *)q, (uint32_t)n) == 0,
              "long string id handled");
        /* JSON-RPC 要求原样回显 id（即使超过 IDSTR_MAX 的**存储**上限）。
         * -32600 + id:null 只用于「连存储都放不下、无法定位」的场景。 */
        CHECK(!has("\"id\":null"), "long id echoed verbatim (spec)");
        CHECK(has("xxxxxxxx"), "long id content preserved");
    }
    /* S2C 请求槽耗尽 → E_BUSY；未就绪 → E_STATE */
    {
        bt_mcp_session_link_down(&se, 1);
        CHECK(bt_mcp_server_request(&sv, "sampling/createMessage", NULL, 0,
                                    NULL, NULL) == BT_MCP_E_STATE,
              "s2c request while down -> E_STATE");
        bt_mcp_session_link_up(&se, 0);
        bt_mcp_session_mark_ready(&se);
        int rc = 0;
        for (int i = 0; i <= BT_MCP_CFG_MAX_S2C_REQ && !rc; i++)
            rc = bt_mcp_server_request(&sv, "roots/list", NULL, 0, NULL, NULL);
        CHECK(rc == BT_MCP_E_BUSY, "s2c slots exhausted -> E_BUSY");
        CHECK(bt_mcp_server_inflight(&sv) == BT_MCP_CFG_MAX_S2C_REQ,
              "inflight count matches slots");
        /* 超时回收 */
        bt_mcp_server_tick(&sv, 0xFFFFFFFFu);
        CHECK(bt_mcp_server_inflight(&sv) == 0, "s2c timeout reclaims slots");
    }

    printf("\n%d pass, %d fail\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
