/* mcp_server.c —— L5 实现。JSON-RPC 2.0 分派（MCP 2025-11-25 有状态语义）。
 *
 * 分派判例：
 * 1. initialize 响应返回 protocolVersion（对端版本命中支持集则回显，否则回最新）
 *    + serverInfo{name,version} + capabilities + _meta.io.btmcp 通道能力声明；
 *    收到 notifications/initialized 才把会话推 READY——标准 MCP 有状态握手序。
 * 2. 未 initialize 先到 tools/resources 系方法 → -32002（会话未就绪）。
 * 3. 未知方法 + 有 id → -32601；未知方法无 id（notification）→ 静默。
 * 4. malformed JSON → -32700（能定位 id 时回显该 id）。
 * 5. tools/call 的 name 不在表 → -32602；工具自身错误 → isError=true 内容上抛。
 * 6. 工具返回 E_PENDING → 不回响应，等应用 server_complete()；期间可
 *    bt_mcp_call_progress() 发 notifications/progress，可被 notifications/cancelled
 *    取消（取消后 complete 不再发任何字节）。
 * 7. 字符串 request id 原样回显（含转义），不做数值化——否则 Host 无法关联。
 * 8. 工具文本输出经「转义直写」进入响应缓冲：无中间缓冲、无双层转义，
 *    输出量受 MAX_MSG 自然背压（超限即 -32603 + E_TOOBIG）。
 * 9. tools/list 与 resources/list 支持 cursor 分页（每页 PAGE_SIZE 条）。
 */
#include "bt_mcp/mcp_server.h"
#include "bt_mcp/err.h"
#include "bt_mcp/session.h"
#include <string.h>

/* 支持的协议版本（新→旧）；命中即回显对端版本，否则回最新 */
static const char *const k_versions[] = {
    "2025-11-25", "2025-06-18", "2025-03-26", "2024-11-05"
};
#define K_VERSION_COUNT (sizeof k_versions / sizeof k_versions[0])

/* ---- 内存 sink（响应拼装） ---- */
typedef struct { char *buf; uint32_t len, cap; } memsink_t;

static int mem_put(void *sink, const char *data, uint32_t len)
{
    memsink_t *m = sink;
    if (m->len + len > m->cap) return BT_MCP_E_TOOBIG;
    if (len) memcpy(m->buf + m->len, data, len);
    m->len += len;
    return 0;
}

/* ---- 转义直写 sink：把工具原始文本按 JSON 字符串转义后灌进目标 writer ---- */
typedef struct { bt_mcp_writer_t *w; uint8_t err; uint32_t bytes; } esc_t;

static int esc_put(void *sink, const char *data, uint32_t len)
{
    esc_t *e = sink;
    static const char HEX[] = "0123456789abcdef";
    char out[64];
    uint32_t o = 0;
    for (uint32_t i = 0; i < len; i++) {
        unsigned char c = (unsigned char)data[i];
        e->bytes += 1;
        if (o + 8 > sizeof out) {                 /* 批量冲刷，避免逐字节 put */
            int rc = bt_mcp_w_raw(e->w, out, o);
            if (rc) { e->err = 1; return rc; }
            o = 0;
        }
        switch (c) {
        case '"':  out[o++] = '\\'; out[o++] = '"';  break;
        case '\\': out[o++] = '\\'; out[o++] = '\\'; break;
        case '\n': out[o++] = '\\'; out[o++] = 'n';  break;
        case '\r': out[o++] = '\\'; out[o++] = 'r';  break;
        case '\t': out[o++] = '\\'; out[o++] = 't';  break;
        default:
            if (c < 0x20) {
                out[o++] = '\\'; out[o++] = 'u'; out[o++] = '0'; out[o++] = '0';
                out[o++] = HEX[c >> 4]; out[o++] = HEX[c & 15];
            } else {
                out[o++] = (char)c;
            }
        }
    }
    if (o) {
        int rc = bt_mcp_w_raw(e->w, out, o);
        if (rc) { e->err = 1; return rc; }
    }
    return 0;
}

/* ---- writer 手工键值（支持非 NUL 结尾切片） ---- */
static int kv_open(bt_mcp_writer_t *w, const char *key)
{
    int rc = w->need_comma ? bt_mcp_w_raw(w, ",", 1) : 0;
    w->need_comma = 1;
    if (!rc) rc = bt_mcp_w_raw(w, "\"", 1);
    if (!rc) rc = bt_mcp_w_raw(w, key, (uint32_t)strlen(key));
    if (!rc) rc = bt_mcp_w_raw(w, "\":", 2);
    return rc;
}

static int kv_str_slice(bt_mcp_writer_t *w, const char *key,
                        const char *val, uint32_t len)
{
    int rc = kv_open(w, key);
    return rc ? rc : bt_mcp_w_str_n(w, val, len);
}

static int kv_u32_new(bt_mcp_writer_t *w, const char *key, uint32_t v)
{
    char buf[16];
    uint32_t n = 0;
    if (v == 0) buf[n++] = '0';
    else {
        char tmp[16]; uint32_t t = 0;
        while (v) { tmp[t++] = (char)('0' + (v % 10)); v /= 10; }
        while (t) buf[n++] = tmp[--t];
    }
    int rc = kv_open(w, key);
    return rc ? rc : bt_mcp_w_raw(w, buf, n);
}

static int kv_raw_new(bt_mcp_writer_t *w, const char *key,
                      const char *raw, uint32_t len)
{
    int rc = kv_open(w, key);
    return rc ? rc : bt_mcp_w_raw(w, raw, len);
}

/* 切片十进制解析（游标）：非数字一律按 0 处理 */
static uint32_t parse_digits(const char *p, uint32_t n)
{
    uint32_t v = 0;
    for (uint32_t i = 0; i < n; i++) {
        if (p[i] < '0' || p[i] > '9') break;
        if (v > 0x0FFFFFFFu) return 0xFFFFFFFFu;
        v = v * 10 + (uint32_t)(p[i] - '0');
    }
    return v;
}

int bt_mcp_server_init(bt_mcp_server_t *sv, bt_mcp_session_t *s,
                       const bt_mcp_server_cfg_t *cfg,
                       const char *fw_name, const char *fw_version)
{
    if (!sv || !s || !cfg) return BT_MCP_E_ARGS;
    memset(sv, 0, sizeof(*sv));
    sv->sess = s;
    sv->cfg = *cfg;
    sv->fw_name = fw_name ? fw_name : "bt-mcp-device";
    sv->fw_ver = fw_version ? fw_version : "0.3";
    return BT_MCP_OK;
}

static int tx_response(bt_mcp_server_t *sv, const char *buf, uint32_t len)
{
    if (!sv->sess) return BT_MCP_E_STATE;
    return bt_mcp_session_tx(sv->sess, (const uint8_t *)buf, len);
}

static int method_is(const bt_mcp_json_req_t *r, const char *m)
{
    uint16_t ml = (uint16_t)strlen(m);
    return r->method && r->method_len == ml && !memcmp(r->method, m, ml);
}

/* 响应缓冲：单写者模型下静态复用 */
static char g_rbuf[BT_MCP_CFG_MAX_MSG];

static int respond(bt_mcp_server_t *sv, const bt_mcp_json_req_t *r,
                   int (*body)(bt_mcp_writer_t *, const bt_mcp_json_req_t *, void *),
                   void *arg)
{
    memsink_t ms = { g_rbuf, 0, sizeof g_rbuf };
    bt_mcp_writer_t w = { .put = mem_put, .sink = &ms, .need_comma = 0 };
    int rc = bt_mcp_w_begin_result_id(&w, r->id_num,
                                      r->id_is_str ? r->id_str : NULL,
                                      r->id_str_len);
    if (!rc) rc = body(&w, r, arg);
    if (rc) return rc;                       /* E_PENDING/E_TOOBIG：不发响应 */
    rc = bt_mcp_w_end(&w);
    if (rc) return rc;
    return tx_response(sv, g_rbuf, ms.len);
}

static int respond_error(bt_mcp_server_t *sv, const bt_mcp_json_req_t *r,
                         int code, const char *msg)
{
    static char ebuf[288];
    memsink_t ms = { ebuf, 0, sizeof ebuf };
    bt_mcp_writer_t w = { .put = mem_put, .sink = &ms, .need_comma = 0 };
    int rc = bt_mcp_w_begin_error_id(&w, r->id_num,
                                     r->id_is_str ? r->id_str : NULL,
                                     r->id_str_len, code, msg);
    if (rc) return rc;
    return tx_response(sv, ebuf, ms.len);
}

/* ---- 调用槽（异步 tools/call） ---- */

static bt_mcp_call_t *call_alloc(bt_mcp_server_t *sv, const bt_mcp_json_req_t *r)
{
    for (int i = 0; i < BT_MCP_CFG_MAX_PENDING; i++) {
        bt_mcp_call_t *c = &sv->calls[i];
        if (c->in_use) continue;
        memset(c, 0, sizeof *c);
        c->srv = sv;
        c->in_use = 1;
        c->id_num = r->id_num;
        if (r->id_is_str) {
            if (r->id_str_len >= sizeof c->id_str) { c->in_use = 0; return NULL; }
            if (r->id_str_len) memcpy(c->id_str, r->id_str, r->id_str_len);
            c->id_str[r->id_str_len] = 0;
            c->id_str_len = r->id_str_len;
            c->id_is_str = 1;
        }
        return c;
    }
    return NULL;
}

static void call_free(bt_mcp_call_t *c)
{
    if (c) c->in_use = 0;
}

int bt_mcp_call_cancelled(const bt_mcp_call_t *c)
{
    return c && c->cancelled;
}

int bt_mcp_call_progress(bt_mcp_call_t *c, uint64_t done, uint64_t total)
{
    if (!c || !c->srv || !c->has_progress) return BT_MCP_E_STATE;
    static char pbuf[192];
    memsink_t ms = { pbuf, 0, sizeof pbuf };
    bt_mcp_writer_t w = { .put = mem_put, .sink = &ms, .need_comma = 0 };
    int rc = bt_mcp_w_raw(&w, "{\"jsonrpc\":\"2.0\",\"method\":"
                               "\"notifications/progress\",\"params\":{",
                          sizeof "{\"jsonrpc\":\"2.0\",\"method\":"
                                 "\"notifications/progress\",\"params\":{" - 1);
    if (!rc) rc = kv_u32_new(&w, "progressToken", (uint32_t)c->progress_token);
    if (!rc) rc = bt_mcp_w_kv_u64(&w, "progress", done);
    if (!rc) rc = bt_mcp_w_kv_u64(&w, "total", total);
    if (!rc) rc = bt_mcp_w_raw(&w, "}}", 2);
    if (rc) return rc;
    return tx_response(c->srv, pbuf, ms.len);
}

/* content 前缀 + 转义写 text + 后缀；is_error 决定 isError 布尔 */
static int write_content_prefix(bt_mcp_writer_t *w)
{
    /* 开引号进前缀、闭引号进后缀：中间由 esc_put 流式灌原文，天然合法 JSON */
    return bt_mcp_w_raw(w, "\"content\":[{\"type\":\"text\",\"text\":\"",
                        sizeof "\"content\":[{\"type\":\"text\",\"text\":\"" - 1);
}

static int write_content_suffix(bt_mcp_writer_t *w, int is_error)
{
    int rc = bt_mcp_w_raw(w, "\"}],\"isError\":", sizeof "\"}],\"isError\":" - 1);
    if (!rc) rc = bt_mcp_w_raw(w, is_error ? "true" : "false", is_error ? 4 : 5);
    return rc;
}

/* 异步完成：拼一条完整响应并发送 */
static int send_tool_result(bt_mcp_server_t *sv, const bt_mcp_call_t *c,
                            const char *text, uint32_t len, int is_error)
{
    memsink_t ms = { g_rbuf, 0, sizeof g_rbuf };
    bt_mcp_writer_t w = { .put = mem_put, .sink = &ms, .need_comma = 0 };
    int rc = bt_mcp_w_begin_result_id(&w, c->id_num,
                                      c->id_is_str ? c->id_str : NULL,
                                      c->id_str_len);
    if (!rc) rc = write_content_prefix(&w);
    esc_t es = { &w, 0, 0 };
    if (!rc) {
        bt_mcp_writer_t tw = { .put = esc_put, .sink = &es, .need_comma = 0 };
        rc = bt_mcp_w_raw(&tw, text ? text : "", len);
        if (es.err) rc = BT_MCP_E_TOOBIG;
    }
    if (!rc) rc = write_content_suffix(&w, is_error);
    if (!rc) rc = bt_mcp_w_end(&w);
    if (rc) return rc;
    return tx_response(sv, g_rbuf, ms.len);
}

int bt_mcp_server_complete(bt_mcp_call_t *c, const char *text, int is_error)
{
    if (!c || !c->in_use || !c->srv) return BT_MCP_E_ARGS;
    uint32_t n = 0;
    if (text) while (text[n]) n++;
    int rc = 0;
    if (!c->cancelled) rc = send_tool_result(c->srv, c, text, n, is_error);
    call_free(c);
    return rc;
}

/* ---- 各方法 body ---- */

static int version_supported(const char *v, uint16_t len)
{
    for (size_t i = 0; i < K_VERSION_COUNT; i++) {
        size_t l = strlen(k_versions[i]);
        if (len == l && !memcmp(v, k_versions[i], l)) return 1;
    }
    return 0;
}

static int body_initialize(bt_mcp_writer_t *w, const bt_mcp_json_req_t *r, void *arg)
{
    bt_mcp_server_t *sv = arg;
    const char *cv = NULL; uint16_t cvl = 0;
    int echo = 0;
    if (!bt_mcp_json_get_str(r, "protocolVersion", &cv, &cvl)
        && version_supported(cv, cvl))
        echo = 1;

    const bt_mcp_chan_cap_t *cap = (sv->sess && sv->sess->fr) ? sv->sess->fr->cap : NULL;
    uint32_t kind = cap ? (uint32_t)cap->kind : 0;
    uint32_t mtu  = cap ? cap->mtu : 0;
    uint32_t rel  = cap ? cap->reliable : 1;
    uint32_t flow = cap ? cap->flow_ctrl : 0;
    uint32_t win  = sv->sess ? sv->sess->cfg.tx_window : 0;

    int rc;
    if (echo) rc = kv_str_slice(w, "protocolVersion", cv, cvl);
    else      rc = kv_str_slice(w, "protocolVersion", BT_MCP_PROTO_VERSION,
                                (uint32_t)strlen(BT_MCP_PROTO_VERSION));
    if (rc) return rc;
    rc = kv_raw_new(w, "capabilities",
        "{\"tools\":{\"listChanged\":false},\"resources\":{\"subscribe\":false,"
        "\"listChanged\":false}}",
        sizeof "{\"tools\":{\"listChanged\":false},\"resources\":{\"subscribe\":false,"
        "\"listChanged\":false}}" - 1);
    if (!rc) rc = bt_mcp_w_raw(w, ",\"serverInfo\":{\"name\":",
                               sizeof ",\"serverInfo\":{\"name\":" - 1);
    if (!rc) { w->need_comma = 1; rc = bt_mcp_w_str_lit(w, sv->fw_name); }
    if (!rc) rc = bt_mcp_w_raw(w, ",\"version\":", sizeof ",\"version\":" - 1);
    if (!rc) rc = bt_mcp_w_str_lit(w, sv->fw_ver);
    if (!rc) rc = bt_mcp_w_raw(w, "}", 1);
    /* _meta.io.btmcp：通道能力空中声明（草案 §3），供 Bridge 侧收敛行为 */
    if (!rc) rc = bt_mcp_w_raw(w, ",\"_meta\":{\"io.btmcp/ver\":\"0.3\","
                                  "\"io.btmcp/chan\":{",
                               sizeof ",\"_meta\":{\"io.btmcp/ver\":\"0.3\","
                                      "\"io.btmcp/chan\":{" - 1);
    w->need_comma = 0;                /* 进入新的内层对象：重置逗号状态 */
    if (!rc) rc = kv_u32_new(w, "kind", kind);
    if (!rc) rc = kv_u32_new(w, "mtu", mtu);
    if (!rc) rc = kv_u32_new(w, "window", win);
    if (!rc) rc = kv_u32_new(w, "reliable", rel);
    if (!rc) rc = kv_u32_new(w, "flow", flow);
    if (!rc) rc = bt_mcp_w_raw(w, "}}", 2);
    return rc;
}

static int body_ping(bt_mcp_writer_t *w, const bt_mcp_json_req_t *r, void *arg)
{
    (void)r; (void)arg;
    return bt_mcp_w_raw(w, "\"pong\":true", 11);   /* 有 result 即可，惯例放 pong */
}

/* 分页窗口：[start, end)，下一页游标 = end */
static void page_of(const bt_mcp_json_req_t *r, uint16_t total,
                    uint32_t *start, uint32_t *end)
{
    const char *cur; uint16_t curl;
    uint32_t s = 0;
    if (!bt_mcp_json_get_str(r, "cursor", &cur, &curl)) s = parse_digits(cur, curl);
    if (s > total) s = total;
    uint32_t e = s + BT_MCP_CFG_PAGE_SIZE;
    if (e > total) e = total;
    *start = s; *end = e;
}

static int put_next_cursor(bt_mcp_writer_t *w, uint32_t next, uint32_t total)
{
    if (next >= total) return BT_MCP_OK;
    char buf[16];
    w->need_comma = 1;               /* 数组已闭合：nextCursor 需要前导逗号 */
    uint32_t n = 0;
    if (next == 0) buf[n++] = '0';
    else {
        char tmp[16]; uint32_t t = 0;
        while (next) { tmp[t++] = (char)('0' + (next % 10)); next /= 10; }
        while (t) buf[n++] = tmp[--t];
    }
    int rc = kv_open(w, "nextCursor");
    return rc ? rc : bt_mcp_w_str_n(w, buf, n);
}

static int body_tools_list(bt_mcp_writer_t *w, const bt_mcp_json_req_t *r, void *arg)
{
    bt_mcp_server_t *sv = arg;
    uint32_t start, end;
    page_of(r, sv->cfg.tools_count, &start, &end);

    int rc = bt_mcp_w_raw(w, "\"tools\":[", sizeof "\"tools\":[" - 1);
    for (uint32_t i = start; !rc && i < end; i++) {
        const bt_mcp_tool_def_t *t = &sv->cfg.tools[i];
        if (i != start) rc = bt_mcp_w_raw(w, ",", 1);
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
    if (!rc) rc = put_next_cursor(w, end, sv->cfg.tools_count);
    return rc;
}

/* 资源 URI：bt-mcp://dev/<name>[?cursor=N] */
#define BT_MCP_URI_PREFIX "bt-mcp://dev/"

static int parse_resource_uri(const char *uri, uint16_t len,
                             const char **name, uint16_t *nlen, uint32_t *cursor)
{
    static const char PFX[] = BT_MCP_URI_PREFIX;
    const uint16_t pfl = (uint16_t)(sizeof PFX - 1);
    if (!uri || len <= pfl || memcmp(uri, PFX, pfl)) return BT_MCP_E_PARSE;
    uint16_t i = pfl;
    uint16_t ns = i;
    while (i < len && uri[i] != '?' && uri[i] != '/' ) i++;
    if (i == ns) return BT_MCP_E_PARSE;
    *name = uri + ns;
    *nlen = (uint16_t)(i - ns);
    *cursor = 0;
    if (i < len && uri[i] == '?') {
        i++;
        while (i + 7 <= len) {
            if (!memcmp(uri + i, "cursor=", 7)) {
                *cursor = parse_digits(uri + i + 7, (uint32_t)(len - i - 7));
                break;
            }
            i++;
        }
    }
    return BT_MCP_OK;
}

static const bt_mcp_resource_def_t *find_resource(bt_mcp_server_t *sv,
                                                  const char *name, uint16_t nlen)
{
    for (uint16_t i = 0; i < sv->cfg.resources_count; i++) {
        const bt_mcp_resource_def_t *rd = &sv->cfg.resources[i];
        size_t l = strlen(rd->name);
        if (l == nlen && !memcmp(rd->name, name, nlen)) return rd;
    }
    return NULL;
}

static int write_resource_uri(bt_mcp_writer_t *w, const char *name)
{
    static const char PFX[] = BT_MCP_URI_PREFIX;
    int rc = bt_mcp_w_raw(w, PFX, (uint32_t)(sizeof PFX - 1));
    return rc ? rc : bt_mcp_w_raw(w, name, (uint32_t)strlen(name));
}

static int body_resources_list(bt_mcp_writer_t *w, const bt_mcp_json_req_t *r, void *arg)
{
    bt_mcp_server_t *sv = arg;
    uint32_t start, end;
    page_of(r, sv->cfg.resources_count, &start, &end);

    int rc = bt_mcp_w_raw(w, "\"resources\":[", sizeof "\"resources\":[" - 1);
    for (uint32_t i = start; !rc && i < end; i++) {
        const bt_mcp_resource_def_t *rd = &sv->cfg.resources[i];
        if (i != start) rc = bt_mcp_w_raw(w, ",", 1);
        if (!rc) rc = bt_mcp_w_raw(w, "{\"name\":", sizeof "{\"name\":" - 1);
        if (!rc) rc = bt_mcp_w_str_lit(w, rd->name);
        if (!rc) rc = bt_mcp_w_raw(w, ",\"uri\":\"", sizeof ",\"uri\":\"" - 1);
        if (!rc) rc = write_resource_uri(w, rd->name);
        if (!rc) rc = bt_mcp_w_raw(w, "\"", 1);
        if (!rc) rc = bt_mcp_w_raw(w, ",\"description\":", sizeof ",\"description\":" - 1);
        if (!rc) rc = bt_mcp_w_str_lit(w, rd->desc ? rd->desc : "");
        if (!rc) rc = bt_mcp_w_raw(w, ",\"mimeType\":", sizeof ",\"mimeType\":" - 1);
        if (!rc) rc = bt_mcp_w_str_lit(w, rd->mime_type ? rd->mime_type : "text/plain");
        if (!rc) rc = bt_mcp_w_raw(w, "}", 1);
    }
    if (!rc) rc = bt_mcp_w_raw(w, "]", 1);
    if (!rc) rc = put_next_cursor(w, end, sv->cfg.resources_count);
    return rc;
}

static int body_resources_read(bt_mcp_writer_t *w, const bt_mcp_json_req_t *r, void *arg)
{
    bt_mcp_server_t *sv = arg;
    const char *uri; uint16_t ul;
    if (bt_mcp_json_get_str(r, "uri", &uri, &ul)) return BT_MCP_E_PARSE;
    const char *name; uint16_t nlen; uint32_t cursor;
    if (parse_resource_uri(uri, ul, &name, &nlen, &cursor)) return BT_MCP_E_PARSE;
    /* params.cursor 也可给游标（URI 查询串优先） */
    if (!cursor) {
        const char *cur; uint16_t curl;
        if (!bt_mcp_json_get_str(r, "cursor", &cur, &curl))
            cursor = parse_digits(cur, curl);
    }
    const bt_mcp_resource_def_t *rd = find_resource(sv, name, nlen);
    if (!rd) return BT_MCP_E_ARGS;              /* 上层转 -32002 */

    const char *text = ""; uint32_t tlen = 0, next = BT_MCP_CURSOR_END;
    if (rd->read && rd->read(rd->user, cursor, &text, &tlen, &next))
        return BT_MCP_E_IO;

    int rc = bt_mcp_w_raw(w, "\"contents\":[{\"uri\":\"", sizeof "\"contents\":[{\"uri\":\"" - 1);
    if (!rc) rc = write_resource_uri(w, rd->name);
    if (!rc) rc = bt_mcp_w_raw(w, "\",\"mimeType\":", sizeof "\",\"mimeType\":" - 1);
    if (!rc) rc = bt_mcp_w_str_lit(w, rd->mime_type ? rd->mime_type : "text/plain");
    if (!rc) rc = bt_mcp_w_raw(w, ",\"text\":\"", sizeof ",\"text\":\"" - 1);
    if (!rc) {
        esc_t es = { w, 0, 0 };
        bt_mcp_writer_t tw = { .put = esc_put, .sink = &es, .need_comma = 0 };
        rc = bt_mcp_w_raw(&tw, text ? text : "", tlen);
        if (es.err) rc = BT_MCP_E_TOOBIG;
    }
    if (!rc) rc = bt_mcp_w_raw(w, "\"}]", 3);
    w->need_comma = 1;               /* 后续 nextCursor 需要前导逗号 */
    if (!rc && next != BT_MCP_CURSOR_END) {
        char buf[16]; uint32_t n = 0;
        if (next == 0) buf[n++] = '0';
        else {
            char tmp[16]; uint32_t t = 0;
            while (next) { tmp[t++] = (char)('0' + (next % 10)); next /= 10; }
            while (t) buf[n++] = tmp[--t];
        }
        rc = kv_open(w, "nextCursor");
        if (!rc) rc = bt_mcp_w_str_n(w, buf, n);
    }
    return rc;
}

/* 工具调用：同步（结果直接写进响应）/ 异步（E_PENDING） */
static int body_tool_call(bt_mcp_writer_t *w, const bt_mcp_json_req_t *r, void *arg)
{
    bt_mcp_server_t *sv = arg;
    const char *name; uint16_t name_len;
    if (bt_mcp_json_get_str(r, "name", &name, &name_len)) return BT_MCP_E_PARSE;

    const bt_mcp_tool_def_t *found = NULL;
    for (uint16_t i = 0; i < sv->cfg.tools_count; i++) {
        const bt_mcp_tool_def_t *t = &sv->cfg.tools[i];
        if (strlen(t->name) == name_len && !memcmp(t->name, name, name_len)) {
            found = t; break;
        }
    }
    if (!found) return BT_MCP_E_PARSE;      /* 上层转 -32602 */

    /* 工具参数在 params.arguments 下：把 arguments 片段嫁接为工具视图 params */
    bt_mcp_json_req_t treq;
    if (bt_mcp_json_get_sub(r, "arguments", &treq))
        treq = *r;                          /* 无 arguments：透传 params 顶层 */

    bt_mcp_call_t *c = call_alloc(sv, r);
    if (!c) return BT_MCP_E_NOMEM;
    /* _meta.progressToken（数字形态）→ 后续可发 notifications/progress */
    bt_mcp_json_req_t meta;
    if (!bt_mcp_json_get_sub(r, "_meta", &meta)) {
        uint32_t pt;
        if (!bt_mcp_json_get_u32(&meta, "progressToken", &pt)) {
            c->has_progress = 1;
            c->progress_token = pt;
        }
    }
    c->t0 = sv->now_ms;

    int rc = write_content_prefix(w);
    esc_t es = { w, 0, 0 };
    int trc = BT_MCP_E_ARGS;
    if (!rc) {
        bt_mcp_writer_t tw = { .put = esc_put, .sink = &es, .need_comma = 0 };
        trc = found->fn(&treq, &tw, c, found->user);
    }
    if (trc == BT_MCP_E_PENDING) return BT_MCP_E_PENDING;  /* 槽保持占用 */
    call_free(c);
    if (rc) return rc;
    if (es.err) return BT_MCP_E_TOOBIG;
    /* 工具失败且没写任何内容 → 给标准诊断文本（MCP 惯例，便于 Host 展示） */
    if (trc != 0 && es.bytes == 0) {
        bt_mcp_writer_t tw = { .put = esc_put, .sink = &es, .need_comma = 0 };
        rc = bt_mcp_w_raw(&tw, "tool error", 10);   /* 已在引号内：只做转义 */
    }
    rc = write_content_suffix(w, trc != 0);
    return rc;
}

/* notifications/cancelled：按 requestId 标记挂起调用 */
static int64_t parse_i64_local(const char *p, uint32_t n)
{
    int neg = 0;
    uint32_t i = 0;
    uint64_t acc = 0;
    if (i < n && (p[i] == '-' || p[i] == '+')) { neg = (p[i] == '-'); i++; }
    for (; i < n && p[i] >= '0' && p[i] <= '9'; i++)
        acc = acc * 10 + (uint64_t)(p[i] - '0');
    return neg ? -(int64_t)acc : (int64_t)acc;
}

static int handle_cancelled(bt_mcp_server_t *sv, const bt_mcp_json_req_t *r)
{
    const char *rid; uint32_t rl;
    if (bt_mcp_json_get_raw(r, "requestId", &rid, &rl)) return BT_MCP_OK;
    for (int i = 0; i < BT_MCP_CFG_MAX_PENDING; i++) {
        bt_mcp_call_t *c = &sv->calls[i];
        if (!c->in_use) continue;
        if (c->id_is_str) {
            /* 原文段（含引号）与保存的 id 文本比对 */
            if (rl != (uint32_t)c->id_str_len + 2) continue;
            if (rid[0] != '"' || memcmp(rid + 1, c->id_str, c->id_str_len)) continue;
        } else {
            if (rl == 0 || rid[0] == '"') continue;
            if (parse_i64_local(rid, rl) != c->id_num) continue;
        }
        c->cancelled = 1;
        return BT_MCP_OK;
    }
    return BT_MCP_OK;
}

int bt_mcp_server_rx(bt_mcp_server_t *sv, const uint8_t *json, uint32_t len)
{
    if (!sv || (!json && len)) return BT_MCP_E_ARGS;
    bt_mcp_json_req_t r;
    int rc = bt_mcp_json_parse((const char *)json, len, &r);
    if (rc) {
        /* 判例 4：malformed 且能定位 id → 回 -32700，否则静默 */
        return r.has_id ? respond_error(sv, &r, -32700, "Parse error")
                        : BT_MCP_E_PARSE;
    }

    if (method_is(&r, "initialize"))
        return respond(sv, &r, body_initialize, sv);

    if (method_is(&r, "notifications/initialized")) {
        sv->initialized = 1;
        bt_mcp_session_mark_ready(sv->sess);     /* LINK_UP → READY（握手完成） */
        return BT_MCP_OK;                        /* notification 无响应 */
    }
    if (!sv->initialized)
        return r.has_id ? respond_error(sv, &r, -32002, "Server not initialized")
                        : BT_MCP_E_STATE;

    if (method_is(&r, "notifications/cancelled"))
        return handle_cancelled(sv, &r);
    if (method_is(&r, "ping"))
        return respond(sv, &r, body_ping, sv);
    if (method_is(&r, "tools/list"))
        return respond(sv, &r, body_tools_list, sv);
    if (method_is(&r, "tools/call")) {
        rc = respond(sv, &r, body_tool_call, sv);
        if (rc == BT_MCP_E_PENDING) return BT_MCP_OK;      /* 挂起：等 complete */
        if (rc == BT_MCP_E_PARSE)
            return respond_error(sv, &r, -32602, "Invalid params");
        if (rc == BT_MCP_E_NOMEM)
            return respond_error(sv, &r, -32603, "Too many pending calls");
        if (rc == BT_MCP_E_TOOBIG)
            return respond_error(sv, &r, -32603, "Result too large");
        return rc;
    }
    if (method_is(&r, "resources/list"))
        return respond(sv, &r, body_resources_list, sv);
    if (method_is(&r, "resources/read")) {
        rc = respond(sv, &r, body_resources_read, sv);
        if (rc == BT_MCP_E_ARGS)
            return respond_error(sv, &r, -32002, "Resource not found");
        if (rc == BT_MCP_E_PARSE)
            return respond_error(sv, &r, -32602, "Invalid params");
        return rc;
    }
    /* 判例 3 */
    if (r.has_id)
        return respond_error(sv, &r, -32601, "Method not found");
    return BT_MCP_OK;                          /* 未知 notification：静默 */
}

int bt_mcp_server_notify(bt_mcp_server_t *sv, const char *method,
                         const char *params_json, uint32_t params_len)
{
    if (!sv || !method) return BT_MCP_E_ARGS;
    static char buf[BT_MCP_CFG_NOTIFY_BUF];
    memsink_t ms = { buf, 0, sizeof buf };
    bt_mcp_writer_t w = { .put = mem_put, .sink = &ms, .need_comma = 0 };
    int rc = bt_mcp_w_raw(&w, "{\"jsonrpc\":\"2.0\",\"method\":",
        sizeof "{\"jsonrpc\":\"2.0\",\"method\":" - 1);
    if (!rc) rc = bt_mcp_w_str_lit(&w, method);
    if (!rc && params_json) {
        rc = bt_mcp_w_raw(&w, ",\"params\":", 10);
        if (!rc) rc = bt_mcp_w_raw(&w, params_json, params_len);
    }
    if (!rc) rc = bt_mcp_w_raw(&w, "}", 1);
    if (rc) return rc;
    return tx_response(sv, buf, ms.len);
}

void bt_mcp_server_tick(bt_mcp_server_t *sv, uint32_t now_ms)
{
    if (!sv) return;
    sv->now_ms = now_ms;
}
