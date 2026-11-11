/* mcp_client.c —— L5 客户端侧实现（Bridge/Host 用）。
 *
 * 判例：
 * 1. initialize 的响应由本层识别：成功则自动发 notifications/initialized
 *    并把 initialized 置 1；应用不必手写这个握手序。
 * 2. 响应按 "id" 数字匹配在途表；匹配不到（迟到/未知 id）按通知外抛，不报错。
 * 3. 超时不重发（MCP 无请求级重试语义）：清槽并回调 error_code=-32000
 *    （自定义“本地超时”码），由应用决定是否重试。
 * 4. 通知（无 id）一律上抛，应用按 method 自行处理（progress / cancelled / …）。
 */
#include "bt_mcp/mcp_client.h"
#include "bt_mcp/err.h"
#include <string.h>
#include <stdio.h>

#define BT_MCP_CLIENT_TIMEOUT_CODE (-32000)

/* 拼装缓冲已移入 bt_mcp_client_t（v0.5：每实例独立，避免多实例/多线程互相覆盖） */

int bt_mcp_client_init(bt_mcp_client_t *c, bt_mcp_session_t *s,
                       bt_mcp_client_cb_t cb, void *user)
{
    if (!c || !s) return BT_MCP_E_ARGS;
    memset(c, 0, sizeof(*c));
    c->sess = s;
    c->cb = cb;
    c->cb_user = user;
    c->next_id = 1;
    c->req_timeout_ms = 30000;
    return BT_MCP_OK;
}

static bt_mcp_client_req_t *req_alloc(bt_mcp_client_t *c, uint32_t id)
{
    for (int i = 0; i < BT_MCP_CFG_MAX_PENDING; i++)
        if (!c->req[i].in_use) {
            c->req[i].in_use = 1;
            c->req[i].id = id;
            c->req[i].t0 = c->now_ms;
            return &c->req[i];
        }
    return NULL;
}

static bt_mcp_client_req_t *req_find(bt_mcp_client_t *c, uint32_t id)
{
    for (int i = 0; i < BT_MCP_CFG_MAX_PENDING; i++)
        if (c->req[i].in_use && c->req[i].id == id) return &c->req[i];
    return NULL;
}

int bt_mcp_client_inflight(const bt_mcp_client_t *c)
{
    int n = 0;
    for (int i = 0; i < BT_MCP_CFG_MAX_PENDING; i++) if (c->req[i].in_use) n++;
    return n;
}

static int send_request(bt_mcp_client_t *c, const char *method,
                        const char *params_json, uint32_t params_len,
                        uint32_t *out_id)
{
    if (!c || !method) return BT_MCP_E_ARGS;
    uint32_t id = c->next_id++;
    if (c->next_id == 0) c->next_id = 1;
    if (!req_alloc(c, id)) return BT_MCP_E_NOMEM;

    size_t n = 0;
    char *sbuf = c->sbuf;
    int w = snprintf(sbuf, sizeof c->sbuf,
                     "{\"jsonrpc\":\"2.0\",\"id\":%u,\"method\":\"%s\"",
                     (unsigned)id, method);
    if (w < 0 || (size_t)w >= sizeof c->sbuf) { req_find(c, id)->in_use = 0; return BT_MCP_E_TOOBIG; }
    n = (size_t)w;
    if (params_json) {
        w = snprintf(sbuf + n, sizeof c->sbuf - n, ",\"params\":");
        if (w < 0 || n + (size_t)w >= sizeof c->sbuf) { req_find(c, id)->in_use = 0; return BT_MCP_E_TOOBIG; }
        n += (size_t)w;
        if (n + params_len + 1 > sizeof c->sbuf) { req_find(c, id)->in_use = 0; return BT_MCP_E_TOOBIG; }
        memmove(sbuf + n, params_json, params_len);   /* 源可能就是本实例的 cbuf，安全 */
        n += params_len;
    }
    if (n + 1 > sizeof c->sbuf) { req_find(c, id)->in_use = 0; return BT_MCP_E_TOOBIG; }
    sbuf[n++] = '}';

    int rc = bt_mcp_session_tx(c->sess, (const uint8_t *)sbuf, (uint32_t)n);
    if (rc) { req_find(c, id)->in_use = 0; return rc; }
    if (out_id) *out_id = id;
    return BT_MCP_OK;
}

int bt_mcp_client_request(bt_mcp_client_t *c, const char *method,
                          const char *params_json, uint32_t params_len,
                          uint32_t *out_id)
{
    return send_request(c, method, params_json, params_len, out_id);
}

int bt_mcp_client_initialize(bt_mcp_client_t *c, const char *client_name,
                             const char *client_version)
{
    char params[256];
    int n = snprintf(params, sizeof params,
        "{\"protocolVersion\":\"%s\",\"capabilities\":{},"
        "\"clientInfo\":{\"name\":\"%s\",\"version\":\"%s\"}}",
        BT_MCP_PROTO_VERSION,
        client_name ? client_name : "bt-mcp-bridge",
        client_version ? client_version : "0.3");
    if (n < 0 || (size_t)n >= sizeof params) return BT_MCP_E_TOOBIG;
    uint32_t id = 0;
    int rc = send_request(c, "initialize", params, (uint32_t)n, &id);
    if (rc == 0) { c->init_id = id; c->initialized = 0; }
    return rc;
}

int bt_mcp_client_list_tools(bt_mcp_client_t *c, const char *cursor, uint32_t *out_id)
{
    char params[64];
    if (!cursor) return send_request(c, "tools/list", NULL, 0, out_id);
    int n = snprintf(params, sizeof params, "{\"cursor\":\"%s\"}", cursor);
    if (n < 0 || (size_t)n >= sizeof params) return BT_MCP_E_TOOBIG;
    return send_request(c, "tools/list", params, (uint32_t)n, out_id);
}

int bt_mcp_client_call_tool(bt_mcp_client_t *c, const char *name,
                            const char *args_json, uint32_t args_len,
                            uint32_t *out_id)
{
    if (!name) return BT_MCP_E_ARGS;
    size_t n = 0;
    char *cbuf = c->cbuf;
    int w = snprintf(cbuf, sizeof c->cbuf, "{\"name\":\"%s\",\"arguments\":", name);
    if (w < 0 || (size_t)w >= sizeof c->cbuf) return BT_MCP_E_TOOBIG;
    n = (size_t)w;
    if (args_json && args_len) {
        if (n + args_len + 1 > sizeof c->cbuf) return BT_MCP_E_TOOBIG;
        memcpy(cbuf + n, args_json, args_len);
        n += args_len;
    } else {
        if (n + 2 > sizeof c->cbuf) return BT_MCP_E_TOOBIG;
        cbuf[n++] = '{';
        cbuf[n++] = '}';
    }
    cbuf[n++] = '}';
    return send_request(c, "tools/call", cbuf, (uint32_t)n, out_id);
}

int bt_mcp_client_read_resource(bt_mcp_client_t *c, const char *uri, uint32_t *out_id)
{
    char params[256];
    if (!uri) return BT_MCP_E_ARGS;
    int n = snprintf(params, sizeof params, "{\"uri\":\"%s\"}", uri);
    if (n < 0 || (size_t)n >= sizeof params) return BT_MCP_E_TOOBIG;
    return send_request(c, "resources/read", params, (uint32_t)n, out_id);
}

int bt_mcp_client_rx(bt_mcp_client_t *c, const uint8_t *json, uint32_t len)
{
    if (!c || (!json && len)) return BT_MCP_E_ARGS;
    bt_mcp_json_req_t r;
    if (bt_mcp_json_parse_msg((const char *)json, len, &r)) return BT_MCP_E_PARSE;

    bt_mcp_client_event_t ev;
    memset(&ev, 0, sizeof ev);

    if (!r.has_id) {                     /* 通知（progress/cancelled/…） */
        ev.is_notification = 1;
        ev.method = r.method;
        ev.method_len = r.method_len;
        ev.result = r.params;
        ev.result_len = r.params_len;
        if (c->cb) c->cb(c->cb_user, &ev);
        return BT_MCP_OK;
    }

    /* 响应：id 为数字或字符串。字符串 id 本端不产生，按未知处理 */
    if (r.id_is_str) return BT_MCP_OK;
    bt_mcp_client_req_t *slot = req_find(c, (uint32_t)r.id_num);
    if (!slot) return BT_MCP_OK;          /* 迟到/未知 id：静默 */

    const char *res = NULL; uint32_t rl = 0;
    const char *err = NULL; uint32_t el = 0;
    if (!bt_mcp_json_top_raw(&r, "result", &res, &rl)) {
        ev.result = res; ev.result_len = rl;
    } else if (!bt_mcp_json_top_raw(&r, "error", &err, &el)) {
        ev.is_error = 1;
        ev.result = err; ev.result_len = el;
        int32_t code = 0;
        bt_mcp_json_req_t sub;
        memset(&sub, 0, sizeof sub);
        sub.src = err;
        sub.len = el;
        sub.params = err;
        sub.params_len = el;
        if (!bt_mcp_json_get_i32(&sub, "code", &code)) ev.error_code = code;
    }
    ev.id = slot->id;

    /* 只有「本次 initialize 请求」的响应才算握手完成（v0.3 修：
     * 旧实现把任何首个成功响应都当 initialize 响应，会误判握手完成） */
    int is_init = 0;
    if (c->init_id != 0 && !c->initialized && !ev.is_error
        && slot->id == c->init_id)
        is_init = 1;
    slot->in_use = 0;

    if (c->cb) c->cb(c->cb_user, &ev);

    if (is_init) {
        c->initialized = 1;
        bt_mcp_session_mark_ready(c->sess);
        static const char N[] = "{\"jsonrpc\":\"2.0\",\"method\":\"notifications/initialized\"}";
        return bt_mcp_session_tx(c->sess, (const uint8_t *)N, (uint32_t)sizeof N - 1);
    }
    return BT_MCP_OK;
}

void bt_mcp_client_tick(bt_mcp_client_t *c, uint32_t now_ms)
{
    if (!c) return;
    c->now_ms = now_ms;
    for (int i = 0; i < BT_MCP_CFG_MAX_PENDING; i++) {
        bt_mcp_client_req_t *q = &c->req[i];
        if (!q->in_use) continue;
        if (now_ms - q->t0 <= c->req_timeout_ms) continue;
        uint32_t id = q->id;
        q->in_use = 0;
        bt_mcp_client_event_t ev;
        memset(&ev, 0, sizeof ev);
        ev.id = id;
        ev.is_error = 1;
        ev.error_code = BT_MCP_CLIENT_TIMEOUT_CODE;
        ev.result = "\"timeout\"";
        ev.result_len = 9;
        if (c->cb) c->cb(c->cb_user, &ev);
    }
}
