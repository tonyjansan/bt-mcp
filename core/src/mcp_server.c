/* mcp_server.c —— L5 实现。JSON-RPC 2.0 分派（MCP 2025-11-25 有状态语义）。
 *
 * 分派判例：
 * 1. initialize 响应立即返回 serverInfo+capabilities（裁剪面：只声明 tools/
 *    resources），收到后 session 保持 LINK_UP，直到 notifications/initialized
 *    才把会话推 READY——这是标准 MCP 有状态握手序。
 * 2. 未 initialize 先到 tools 系方法 → 回 JSON-RPC error -32002（会话未就绪）。
 * 3. 未知方法 + 有 id → -32601；未知方法无 id（notification）→ 静默。
 * 4. malformed JSON → -32700（有 id 时；无 id 静默丢弃）。
 * 5. tools/call 的 name 不在表 → -32602；工具自身错误 → 工具回调返回值
 *    作为 isError 内容原样上抛（content.text 保留诊断信息，MCP 惯例）。
 */
#include "bt_mcp/mcp_server.h"
#include "bt_mcp/err.h"
#include "bt_mcp/session.h"
#include <string.h>

int bt_mcp_server_init(bt_mcp_server_t *sv, bt_mcp_session_t *s,
                       const bt_mcp_server_cfg_t *cfg,
                       const char *fw_name, const char *fw_version)
{
    if (!sv || !s || !cfg) return BT_MCP_E_ARGS;
    memset(sv, 0, sizeof(*sv));
    sv->sess = s;
    sv->cfg = *cfg;
    sv->fw_name = fw_name ? fw_name : "bt-mcp-device";
    sv->fw_ver = fw_version ? fw_version : "0.2";
    return BT_MCP_OK;
}

/* 把 writer 缓冲经 session_tx 发出（发不出去不重试：返回 E_AGAIN 上抛） */
static int tx_response(bt_mcp_server_t *sv, const char *buf, uint32_t len)
{
    if (!sv->sess) return BT_MCP_E_STATE;
    return bt_mcp_session_tx(sv->sess, (const uint8_t *)buf, len);
}

static int method_is(const bt_mcp_json_req_t *r, const char *m)
{
    uint16_t ml = (uint16_t)strlen(m);
    return r->method_len == ml && !memcmp(r->method, m, ml);
}

/* --- 响应拼装用内存 sink（宿主机/测试）: 环形直写版本见 port --- */
typedef struct { char *buf; uint32_t len, cap; } memsink_t;

static int mem_put(void *sink, const char *data, uint32_t len)
{
    memsink_t *m = sink;
    if (m->len + len > m->cap) return BT_MCP_E_TOOBIG;
    memcpy(m->buf + m->len, data, len);
    m->len += len;
    return 0;
}

static int respond(bt_mcp_server_t *sv, int64_t id,
                   int (*body)(bt_mcp_writer_t *, const bt_mcp_json_req_t *, void *),
                   const bt_mcp_json_req_t *r, void *arg)
{
    static char rbuf[BT_MCP_CFG_MAX_MSG];   /* 串行非重入：静态响应缓冲 */
    memsink_t ms = { rbuf, 0, sizeof rbuf };
    bt_mcp_writer_t w = { .put = mem_put, .sink = &ms };
    int rc = bt_mcp_w_begin_result(&w, id);
    if (!rc) rc = body(&w, r, arg);
    if (!rc) rc = bt_mcp_w_end(&w);
    if (rc) return rc;
    return tx_response(sv, rbuf, ms.len);
}

static int respond_error(bt_mcp_server_t *sv, int64_t id, int code, const char *msg)
{
    static char rbuf[256];
    memsink_t ms = { rbuf, 0, sizeof rbuf };
    bt_mcp_writer_t w = { .put = mem_put, .sink = &ms };
    int rc = bt_mcp_w_begin_error(&w, id, code, msg);
    if (rc) return rc;
    return tx_response(sv, rbuf, ms.len);
}

/* ---- 各方法 body ---- */

static int body_initialize(bt_mcp_writer_t *w, const bt_mcp_json_req_t *r, void *arg)
{
    (void)r;
    bt_mcp_server_t *sv = arg;
    int rc = bt_mcp_w_kv_str(w, "protocolVersion", "2025-11-25");
    if (!rc) rc = bt_mcp_w_kv_str(w, "serverName", sv->fw_name);
    if (!rc) rc = bt_mcp_w_kv_str(w, "serverVersion", sv->fw_ver);
    if (!rc) rc = bt_mcp_w_kv_raw(w, "capabilities",
        "{\"tools\":{\"listChanged\":false},\"resources\":{\"subscribe\":false,"
        "\"listChanged\":false}}",
        sizeof "{\"tools\":{\"listChanged\":false},\"resources\":{\"subscribe\":false,"
        "\"listChanged\":false}}" - 1);
    return rc;
}

static int body_tools_list(bt_mcp_writer_t *w, const bt_mcp_json_req_t *r, void *arg)
{
    (void)r;
    bt_mcp_server_t *sv = arg;
    int rc = bt_mcp_w_raw(w, "\"tools\":[", sizeof "\"tools\":[" - 1);
    for (uint16_t i = 0; !rc && i < sv->cfg.tools_count; i++) {
        const bt_mcp_tool_def_t *t = &sv->cfg.tools[i];
        if (i) rc = bt_mcp_w_raw(w, ",", 1);
        if (!rc) rc = bt_mcp_w_raw(w, "{\"name\":", sizeof "{\"name\":" - 1);
        if (!rc) rc = bt_mcp_w_str_lit(w, t->name);
        if (!rc) rc = bt_mcp_w_raw(w, ",\"description\":", sizeof ",\"description\":" - 1);
        if (!rc) rc = bt_mcp_w_str_lit(w, t->desc);
        if (!rc) rc = bt_mcp_w_raw(w, ",\"inputSchema\":", sizeof ",\"inputSchema\":" - 1);
        if (!rc) rc = bt_mcp_w_raw(w, t->input_schema_json,
                                   (uint32_t)strlen(t->input_schema_json));
        if (!rc) rc = bt_mcp_w_raw(w, "}", 1);
    }
    if (!rc) rc = bt_mcp_w_raw(w, "]", 1);
    return rc;
}

static int body_tool_call(bt_mcp_writer_t *w, const bt_mcp_json_req_t *r, void *arg)
{
    bt_mcp_server_t *sv = arg;
    const char *name; uint16_t name_len;
    if (bt_mcp_json_get_str(r, "name", &name, &name_len))
        return BT_MCP_E_PARSE;

    const bt_mcp_tool_def_t *found = NULL;
    for (uint16_t i = 0; i < sv->cfg.tools_count; i++) {
        const bt_mcp_tool_def_t *t = &sv->cfg.tools[i];
        if (strlen(t->name) == name_len && !memcmp(t->name, name, name_len)) {
            found = t; break;
        }
    }
    if (!found) return BT_MCP_E_PARSE;      /* 上层转 -32602 */

    /* 工具参数在 params.arguments 下：把 arguments 片段嫁接为工具视图的
       params（工具回调统一用 get_str/get_raw 取参，无需感知层级） */
    bt_mcp_json_req_t treq;
    if (bt_mcp_json_get_sub(r, "arguments", &treq)) {
        treq = *r;                  /* 无 arguments：透传 params 顶层 */
    }
    int rc = bt_mcp_w_raw(w, "\"content\":[{\"type\":\"text\",\"text\":",
        sizeof "\"content\":[{\"type\":\"text\",\"text\":" - 1);
    /* 工具文本先落静态缓冲再转义写出（工具输出量小、单条） */
    static char tool_out[2048];
    memsink_t ms = { tool_out, 0, sizeof tool_out - 1 };
    bt_mcp_writer_t tw = { .put = mem_put, .sink = &ms };
    int trc = found->fn(&treq, &tw, found->user);
    tool_out[ms.len] = 0;
    if (!rc) rc = bt_mcp_w_str_lit(w, trc ? "tool error" : tool_out);
    if (!rc) rc = bt_mcp_w_raw(w, "}],\"isError\":", sizeof "}],\"isError\":" - 1);
    if (!rc) rc = bt_mcp_w_raw(w, trc ? "true" : "false", trc ? 4 : 5);
    return rc;
}

static int body_ping(bt_mcp_writer_t *w, const bt_mcp_json_req_t *r, void *arg)
{
    (void)r; (void)arg;
    return bt_mcp_w_raw(w, "\"pong\":true", 11);   /* 有 result 即可，惯例放 pong */
}

int bt_mcp_server_rx(bt_mcp_server_t *sv, const uint8_t *json, uint32_t len)
{
    if (!sv || (!json && len)) return BT_MCP_E_ARGS;
    bt_mcp_json_req_t r;
    int rc = bt_mcp_json_parse((const char *)json, len, &r);
    if (rc) {
        /* 判例 4：malformed 且无法定位 id → 静默 */
        return r.has_id ? respond_error(sv, r.id_num, -32700, "Parse error")
                        : BT_MCP_E_PARSE;
    }

    if (method_is(&r, "initialize")) {
        rc = respond(sv, r.id_num, body_initialize, &r, sv);
        return rc;
    }
    if (method_is(&r, "notifications/initialized")) {
        sv->initialized = 1;
        return BT_MCP_OK;                       /* notification 无响应 */
    }
    if (!sv->initialized)
        return r.has_id ? respond_error(sv, r.id_num, -32002, "Server not initialized")
                        : BT_MCP_E_STATE;
    if (method_is(&r, "ping"))
        return respond(sv, r.id_num, body_ping, &r, sv);
    if (method_is(&r, "tools/list"))
        return respond(sv, r.id_num, body_tools_list, &r, sv);
    if (method_is(&r, "tools/call")) {
        rc = respond(sv, r.id_num, body_tool_call, &r, sv);
        if (rc == BT_MCP_E_PARSE)               /* name 缺失/不在表 → -32602 */
            return respond_error(sv, r.id_num, -32602, "Invalid params");
        return rc;
    }
    /* 判例 3 */
    if (r.has_id)
        return respond_error(sv, r.id_num, -32601, "Method not found");
    return BT_MCP_OK;                          /* 未知 notification：静默 */
}

int bt_mcp_server_notify(bt_mcp_server_t *sv, const char *method,
                         const char *params_json, uint32_t params_len)
{
    if (!sv || !method) return BT_MCP_E_ARGS;
    static char buf[512];
    memsink_t ms = { buf, 0, sizeof buf };
    bt_mcp_writer_t w = { .put = mem_put, .sink = &ms };
    int rc = bt_mcp_w_raw(&w, "{\"jsonrpc\":\"2.0\",\"method\":",
        sizeof "{\"jsonrpc\":\"2.0\",\"method\":" - 1);
    if (!rc) rc = bt_mcp_w_str_lit(&w, method);
    if (!rc && params_json) {
        rc = bt_mcp_w_raw(&w, ",\"params\":", 10);
        if (!rc) rc = bt_mcp_w_raw(&w, params_json, params_len);
    }
    if (!rc) rc = bt_mcp_w_raw(&w, "}", 1);
    if (rc) return rc;
    return bt_mcp_session_tx(sv->sess, (const uint8_t *)buf, ms.len);
}

void bt_mcp_server_tick(bt_mcp_server_t *sv, uint32_t now_ms)
{
    if (!sv) return;
    sv->now_ms = now_ms;
}
