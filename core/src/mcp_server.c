/* mcp_server.c —— L5：MCP server 侧（v0.4）。
 *
 * 内容拼装的关键设计：工具通过 writer 写入 result.content 数组。
 *  - 裸 bt_mcp_w_raw(out, ...) 追加到「当前文本项」（没有就开一个）——保持
 *    v0.3 的字节输出完全不变；
 *  - bt_mcp_w_content_* 关闭当前文本项并追加新的 text/image/audio/resource 项。
 * 两条路径都由同一个 content sink 顺序拼装，因此不会产生非法 JSON。
 */
#include "bt_mcp/mcp_server.h"
#include "bt_mcp/err.h"
#include "bt_mcp/trace.h"
#include <string.h>

/* ---- 通用小工具 ---- */

static int snprintf_int(char *buf, uint32_t cap, int v)
{
    char tmp[16];
    uint32_t t = 0, n = 0;
    unsigned u;
    if (v < 0) { if (n < cap) buf[n++] = '-'; u = (unsigned)(-(v + 1)) + 1u; }
    else u = (unsigned)v;
    if (u == 0) { if (n < cap) buf[n++] = '0'; }
    else {
        while (u) { tmp[t++] = (char)('0' + (u % 10)); u /= 10; }
        while (t && n < cap) buf[n++] = tmp[--t];
    }
    return (int)n;
}

static int snprintf_u32(char *buf, uint32_t cap, uint32_t v)
{
    char tmp[16];
    uint32_t t = 0, n = 0;
    if (v == 0) { if (n < cap) buf[n++] = '0'; }
    else {
        while (v) { tmp[t++] = (char)('0' + (v % 10)); v /= 10; }
        while (t && n < cap) buf[n++] = tmp[--t];
    }
    return (int)n;
}

/* ---- 内存 sink（响应拼装） ---- */

typedef struct { char *buf; uint32_t len; uint32_t cap; } memsink_t;

static int mem_put(void *sink, const char *data, uint32_t len)
{
    memsink_t *m = sink;
    if (m->len + len > m->cap) return BT_MCP_E_TOOBIG;
    memcpy(m->buf + m->len, data, len);
    m->len += len;
    return BT_MCP_OK;
}

/* ---- writer 手工键值（支持非 NUL 结尾切片） ---- */

static int kv_open(bt_mcp_writer_t *w, const char *key)
{
    int rc = BT_MCP_OK;
    if (w->need_comma) rc = bt_mcp_w_raw(w, ",", 1);
    if (!rc) rc = bt_mcp_w_raw(w, "\"", 1);
    if (!rc) rc = bt_mcp_w_raw(w, key, (uint32_t)strlen(key));
    if (!rc) rc = bt_mcp_w_raw(w, "\":", 2);
    w->need_comma = 0;
    return rc;
}

static int kv_str_slice(bt_mcp_writer_t *w, const char *key,
                        const char *val, uint32_t len)
{
    int rc = kv_open(w, key);
    if (!rc) rc = bt_mcp_w_raw(w, "\"", 1);
    if (!rc) rc = bt_mcp_w_raw(w, val, len);
    if (!rc) rc = bt_mcp_w_raw(w, "\"", 1);
    w->need_comma = 1;
    return rc;
}

static int kv_u32_new(bt_mcp_writer_t *w, const char *key, uint32_t v)
{
    char buf[16];
    int n = snprintf_u32(buf, sizeof buf, v);
    int rc = kv_open(w, key);
    if (!rc) rc = bt_mcp_w_raw(w, buf, (uint32_t)n);
    w->need_comma = 1;
    return rc;
}

static int kv_raw_new(bt_mcp_writer_t *w, const char *key,
                      const char *raw, uint32_t len)
{
    int rc = kv_open(w, key);
    if (!rc) rc = bt_mcp_w_raw(w, raw, len);
    w->need_comma = 1;
    return rc;
}

/* ---- content sink：content 数组拼装 ---- */

typedef struct {
    bt_mcp_writer_t *w;     /* 外层 result writer */
    uint8_t  items;         /* 已开启的内容项数 */
    uint8_t  text_open;     /* 当前是否有一个未闭合的 text 项 */
    uint8_t  err;
} cs_t;

static int cs_begin_item(cs_t *c)
{
    if (!c->items) return BT_MCP_OK;
    return bt_mcp_w_raw(c->w, ",", 1);
}

static int cs_open_text(cs_t *c)
{
    if (c->text_open) return BT_MCP_OK;
    int rc = cs_begin_item(c);
    if (!rc) rc = bt_mcp_w_raw(c->w, "{\"type\":\"text\",\"text\":\"",
                               sizeof "{\"type\":\"text\",\"text\":\"" - 1);
    if (!rc) { c->text_open = 1; c->items++; }
    return rc;
}

static int cs_close_text(cs_t *c)
{
    if (!c->text_open) return BT_MCP_OK;
    c->text_open = 0;
    return bt_mcp_w_raw(c->w, "\"}", 2);
}

/* 转义直写：JSON 字符串内容（调用方保证已处于引号内） */
static int cs_put(void *sink, const char *data, uint32_t len)
{
    cs_t *c = sink;
    static const char hex[] = "0123456789abcdef";
    char out[8];
    for (uint32_t i = 0; i < len; i++) {
        unsigned char ch = (unsigned char)data[i];
        uint32_t o = 0;
        c->w->need_comma = 0;
        if (c->err) return BT_MCP_E_IO;
        if (!c->text_open) {
            int rc = cs_open_text(c);
            if (rc) { c->err = 1; return rc; }
        }
        switch (ch) {
        case '"':  out[o++] = '\\'; out[o++] = '"';  break;
        case '\\': out[o++] = '\\'; out[o++] = '\\'; break;
        case '\n': out[o++] = '\\'; out[o++] = 'n';  break;
        case '\r': out[o++] = '\\'; out[o++] = 'r';  break;
        case '\t': out[o++] = '\\'; out[o++] = 't';  break;
        default:
            if (ch < 0x20) {
                out[o++] = '\\'; out[o++] = 'u'; out[o++] = '0'; out[o++] = '0';
                out[o++] = hex[(ch >> 4) & 0xF];
                out[o++] = hex[ch & 0xF];
            } else {
                out[o++] = (char)ch;
            }
        }
        int rc = bt_mcp_w_raw(c->w, out, o);
        if (rc) { c->err = 1; return rc; }
    }
    return BT_MCP_OK;
}

/* 内联转义：只转义字节，不涉及内容项状态 */
static int cs_escape_inline(bt_mcp_writer_t *w, const char *data, uint32_t len)
{
    static const char hex[] = "0123456789abcdef";
    char out[8];
    for (uint32_t i = 0; i < len; i++) {
        unsigned char ch = (unsigned char)data[i];
        uint32_t o = 0;
        switch (ch) {
        case '"':  out[o++] = '\\'; out[o++] = '"';  break;
        case '\\': out[o++] = '\\'; out[o++] = '\\'; break;
        case '\n': out[o++] = '\\'; out[o++] = 'n';  break;
        case '\r': out[o++] = '\\'; out[o++] = 'r';  break;
        case '\t': out[o++] = '\\'; out[o++] = 't';  break;
        default:
            if (ch < 0x20) {
                out[o++] = '\\'; out[o++] = 'u'; out[o++] = '0'; out[o++] = '0';
                out[o++] = hex[(ch >> 4) & 0xF];
                out[o++] = hex[ch & 0xF];
            } else {
                out[o++] = (char)ch;
            }
        }
        int rc = bt_mcp_w_raw(w, out, o);
        if (rc) return rc;
    }
    return BT_MCP_OK;
}

/* 纯转义 sink：把原文按 JSON 字符串转义后直写目标 writer（调用方负责引号）。
 * 用于 resources/read 的 text 与 logging 的 data —— 这些不是 content 数组项。 */
typedef struct { bt_mcp_writer_t *w; uint8_t err; } esc_t;

static int esc_put(void *sink, const char *data, uint32_t len)
{
    esc_t *e = sink;
    static const char hex[] = "0123456789abcdef";
    char out[8];
    for (uint32_t i = 0; i < len; i++) {
        unsigned char ch = (unsigned char)data[i];
        uint32_t o = 0;
        if (e->err) return BT_MCP_E_IO;
        switch (ch) {
        case '"':  out[o++] = '\\'; out[o++] = '"';  break;
        case '\\': out[o++] = '\\'; out[o++] = '\\'; break;
        case '\n': out[o++] = '\\'; out[o++] = 'n';  break;
        case '\r': out[o++] = '\\'; out[o++] = 'r';  break;
        case '\t': out[o++] = '\\'; out[o++] = 't';  break;
        default:
            if (ch < 0x20) {
                out[o++] = '\\'; out[o++] = 'u'; out[o++] = '0'; out[o++] = '0';
                out[o++] = hex[(ch >> 4) & 0xF];
                out[o++] = hex[ch & 0xF];
            } else {
                out[o++] = (char)ch;
            }
        }
        int rc = bt_mcp_w_raw(e->w, out, o);
        if (rc) { e->err = 1; return rc; }
    }
    return BT_MCP_OK;
}

static int cs_end(cs_t *c)
{
    int rc = cs_close_text(c);
    if (!rc) rc = bt_mcp_w_raw(c->w, "]", 1);
    if (!rc) c->w->need_comma = 1;
    return rc;
}

/* ---- 富内容写入 API（工具回调内使用） ---- */

static int content_opaque(bt_mcp_writer_t *w, const char *type,
                          const char *data, uint32_t len, const char *mime)
{
    if (!w || !data) return BT_MCP_E_ARGS;
    cs_t *c = w->sink;
    if (!c || c->err) return BT_MCP_E_IO;
    int rc = cs_close_text(c);
    if (!rc) rc = cs_begin_item(c);
    if (!rc) rc = bt_mcp_w_raw(c->w, "{\"type\":\"", sizeof "{\"type\":\"" - 1);
    if (!rc) rc = bt_mcp_w_raw(c->w, type, (uint32_t)strlen(type));
    if (!rc) rc = bt_mcp_w_raw(c->w, "\",\"data\":\"", sizeof "\",\"data\":\"" - 1);
    if (!rc) rc = bt_mcp_w_raw(c->w, data, len);
    if (!rc) rc = bt_mcp_w_raw(c->w, "\",\"mimeType\":\"",
                               sizeof "\",\"mimeType\":\"" - 1);
    if (!rc) rc = bt_mcp_w_raw(c->w, mime ? mime : "application/octet-stream",
                               (uint32_t)strlen(mime ? mime : "application/octet-stream"));
    if (!rc) rc = bt_mcp_w_raw(c->w, "\"}", 2);
    if (rc) { c->err = 1; return rc; }
    c->items++;
    return BT_MCP_OK;
}

int bt_mcp_w_content_text(bt_mcp_writer_t *w, const char *text, uint32_t len)
{
    if (!w) return BT_MCP_E_ARGS;
    cs_t *c = w->sink;
    if (!c || c->err) return BT_MCP_E_IO;
    int rc = cs_close_text(c);       /* 显式新项 */
    if (!rc) rc = cs_open_text(c);
    if (!rc) rc = cs_put(c, text ? text : "", len);
    if (rc) { c->err = 1; return rc; }
    return BT_MCP_OK;
}

int bt_mcp_w_content_image(bt_mcp_writer_t *w, const char *b64, uint32_t b64len,
                           const char *mime)
{
    return content_opaque(w, "image", b64, b64len, mime);
}

int bt_mcp_w_content_audio(bt_mcp_writer_t *w, const char *b64, uint32_t b64len,
                           const char *mime)
{
    return content_opaque(w, "audio", b64, b64len, mime);
}

int bt_mcp_w_content_resource(bt_mcp_writer_t *w, const char *uri,
                              const char *mime, const char *text, uint32_t len)
{
    if (!w || !uri) return BT_MCP_E_ARGS;
    cs_t *c = w->sink;
    if (!c || c->err) return BT_MCP_E_IO;
    int rc = cs_close_text(c);
    if (!rc) rc = cs_begin_item(c);
    if (!rc) rc = bt_mcp_w_raw(c->w, "{\"type\":\"resource\",\"resource\":{",
                               sizeof "{\"type\":\"resource\",\"resource\":{" - 1);
    if (!rc) rc = bt_mcp_w_raw(c->w, "\"uri\":\"", sizeof "\"uri\":\"" - 1);
    if (!rc) rc = bt_mcp_w_raw(c->w, uri, (uint32_t)strlen(uri));
    if (!rc) rc = bt_mcp_w_raw(c->w, "\",\"mimeType\":\"",
                               sizeof "\",\"mimeType\":\"" - 1);
    if (!rc) rc = bt_mcp_w_raw(c->w, mime ? mime : "text/plain",
                               (uint32_t)strlen(mime ? mime : "text/plain"));
    if (!rc) rc = bt_mcp_w_raw(c->w, "\",\"text\":\"", sizeof "\",\"text\":\"" - 1);
    if (!rc) rc = cs_escape_inline(c->w, text ? text : "", len);
    if (!rc) rc = bt_mcp_w_raw(c->w, "\"}}", 3);   /* 关 resource 对象 + 关内容项 */
    if (rc) { c->err = 1; return rc; }
    c->items++;
    return BT_MCP_OK;
}

/* ---- 生命周期 ---- */

static int method_is(const bt_mcp_json_req_t *r, const char *m)
{
    uint16_t ml = (uint16_t)strlen(m);
    return r->method && r->method_len == ml && !memcmp(r->method, m, ml);
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
    sv->fw_ver = fw_version ? fw_version : "0.4";
    sv->log_level = BT_MCP_LOG_INFO;      /* MCP 默认 info */
    return BT_MCP_OK;
}

static int tx_response(bt_mcp_server_t *sv, const char *buf, uint32_t len)
{
    if (!sv->sess) return BT_MCP_E_STATE;
    return bt_mcp_session_tx(sv->sess, (const uint8_t *)buf, len);
}

static int respond(bt_mcp_server_t *sv, const bt_mcp_json_req_t *r,
                   int (*body)(bt_mcp_writer_t *, const bt_mcp_json_req_t *, void *),
                   void *arg)
{
    memsink_t ms = { sv->rbuf, 0, (uint32_t)sizeof sv->rbuf };
    bt_mcp_writer_t w = { .put = mem_put, .sink = &ms, .need_comma = 0 };
    int rc = bt_mcp_w_begin_result_id(&w, r->id_num,
                                      r->id_is_str ? r->id_str : NULL,
                                      r->id_str_len);
    if (!rc) rc = body(&w, r, arg);
    if (rc) return rc;                       /* E_PENDING/E_TOOBIG：不发响应 */
    rc = bt_mcp_w_end(&w);
    if (rc) return rc;
    return tx_response(sv, sv->rbuf, ms.len);
}

/* id 不可回显（超长字符串 id / 无法解析）时按 JSON-RPC 规范回 "id":null */
static int respond_error_null_id(bt_mcp_server_t *sv, int code, const char *msg)
{
    memsink_t ms = { sv->ebuf, 0, (uint32_t)sizeof sv->ebuf };
    bt_mcp_writer_t w = { .put = mem_put, .sink = &ms, .need_comma = 0 };
    char head[64];
    int n = snprintf_int(head, sizeof head, code);
    int rc = bt_mcp_w_raw(&w, "{\"jsonrpc\":\"2.0\",\"id\":null,\"error\":"
                               "{\"code\":", sizeof "{\"jsonrpc\":\"2.0\",\"id\":null,"
                               "\"error\":{\"code\":" - 1);
    if (!rc) { w.need_comma = 0; rc = bt_mcp_w_raw(&w, head, (uint32_t)n); }
    if (!rc) rc = bt_mcp_w_raw(&w, ",\"message\":", sizeof ",\"message\":" - 1);
    if (!rc) rc = bt_mcp_w_str_lit(&w, msg);
    if (!rc) rc = bt_mcp_w_raw(&w, "}}", 2);
    if (rc) return rc;
    return tx_response(sv, sv->ebuf, ms.len);
}

static int respond_error(bt_mcp_server_t *sv, const bt_mcp_json_req_t *r,
                         int code, const char *msg)
{
    if (r->id_is_str && r->id_str_len >= BT_MCP_CFG_IDSTR_MAX)
        return respond_error_null_id(sv, -32600, "Invalid request id");
    memsink_t ms = { sv->ebuf, 0, (uint32_t)sizeof sv->ebuf };
    bt_mcp_writer_t w = { .put = mem_put, .sink = &ms, .need_comma = 0 };
    int rc = bt_mcp_w_begin_error_id(&w, r->id_num,
                                     r->id_is_str ? r->id_str : NULL,
                                     r->id_str_len, code, msg);
    if (rc) return rc;
    return tx_response(sv, sv->ebuf, ms.len);
}

/* ---- 调用槽（异步 tools/call） ---- */

static bt_mcp_call_t *call_alloc(bt_mcp_server_t *sv, const bt_mcp_json_req_t *r)
{
    for (int i = 0; i < BT_MCP_CFG_MAX_PENDING; i++) {
        bt_mcp_call_t *c = &sv->calls[i];
        if (c->in_use) continue;
        memset(c, 0, sizeof(*c));
        c->srv = sv;
        c->in_use = 1;
        c->id_is_str = r->id_is_str;
        c->id_num = r->id_num;
        if (r->id_is_str) {
            uint16_t n = r->id_str_len;
            if (n >= sizeof c->id_str) n = sizeof c->id_str - 1;
            memcpy(c->id_str, r->id_str, n);
            c->id_str[n] = 0;
            c->id_str_len = n;
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
    return (c && c->in_use && c->cancelled) ? 1 : 0;
}

int bt_mcp_call_progress(bt_mcp_call_t *c, uint64_t done, uint64_t total)
{
    if (!c || !c->srv || !c->has_progress) return BT_MCP_E_STATE;
    if (c->cancelled) return BT_MCP_E_CANCEL;
    char *pbuf = c->srv->nbuf;
    if (!pbuf) return BT_MCP_E_STATE;
    memsink_t ms = { pbuf, 0, BT_MCP_CFG_NOTIFY_BUF };
    bt_mcp_writer_t w = { .put = mem_put, .sink = &ms, .need_comma = 0 };
    int rc = bt_mcp_w_raw(&w, "{\"jsonrpc\":\"2.0\",\"method\":"
                               "\"notifications/progress\",\"params\":{",
                          sizeof "{\"jsonrpc\":\"2.0\",\"method\":"
                                 "\"notifications/progress\",\"params\":{" - 1);
    if (c->progress_is_str) {
        if (!rc) rc = kv_open(&w, "progressToken");
        if (!rc) rc = bt_mcp_w_raw(&w, "\"", 1);
        if (!rc) rc = bt_mcp_w_raw(&w, c->progress_str, c->progress_str_len);
        if (!rc) rc = bt_mcp_w_raw(&w, "\"", 1);
        w.need_comma = 1;
    } else if (!rc) {
        rc = kv_u32_new(&w, "progressToken", (uint32_t)c->progress_token);
    }
    if (!rc) rc = kv_u32_new(&w, "progress", (uint32_t)(done > 0xFFFFFFFFu
                                                       ? 0xFFFFFFFFu : done));
    if (!rc && total) rc = kv_u32_new(&w, "total", (uint32_t)(total > 0xFFFFFFFFu
                                                             ? 0xFFFFFFFFu : total));
    if (!rc) rc = bt_mcp_w_raw(&w, "}}", 2);
    if (rc) return rc;
    return tx_response(c->srv, pbuf, ms.len);
}

/* ---- 结果拼装（同步 / 异步共用） ---- */

/* 只开 content 数组（同步路径：result 头已由 respond() 写好） */
static int content_array_begin(bt_mcp_writer_t *w)
{
    int rc = kv_open(w, "content");
    if (!rc) rc = bt_mcp_w_raw(w, "[", 1);
    if (!rc) w->need_comma = 0;
    return rc;
}

/* 异步路径：自带 result 头 */
static int tool_result_begin(bt_mcp_writer_t *w, const bt_mcp_call_t *c)
{
    int rc = bt_mcp_w_begin_result_id(w, c->id_num,
                                      c->id_is_str ? c->id_str : NULL,
                                      c->id_str_len);
    if (!rc) rc = content_array_begin(w);
    return rc;
}

/* 内容数组收尾 + isError；close_result=1 时再关 result 对象（异步路径） */
static int tool_result_end(bt_mcp_writer_t *w, cs_t *c, int is_error,
                           int close_result)
{
    int rc = cs_end(c);
    if (!rc) rc = bt_mcp_w_raw(w, ",\"isError\":", sizeof ",\"isError\":" - 1);
    if (!rc) rc = bt_mcp_w_raw(w, is_error ? "true" : "false",
                               is_error ? 4 : 5);
    if (!rc && close_result) rc = bt_mcp_w_end(w);
    return rc;
}

static int send_tool_result(bt_mcp_server_t *sv, const bt_mcp_call_t *c,
                            const char *text, uint32_t len, int is_error)
{
    memsink_t ms = { sv->rbuf, 0, (uint32_t)sizeof sv->rbuf };
    bt_mcp_writer_t w = { .put = mem_put, .sink = &ms, .need_comma = 0 };
    cs_t cs = { &w, 0, 0, 0 };
    int rc = tool_result_begin(&w, c);
    if (!rc) cs_put(&cs, text ? text : "", len);
    if (!rc) rc = tool_result_end(&w, &cs, is_error, 1);   /* 异步：自己关 result */
    if (rc) return rc;
    return tx_response(sv, sv->rbuf, ms.len);
}

int bt_mcp_server_complete(bt_mcp_call_t *c, const char *text, int is_error)
{
    if (!c || !c->in_use || !c->srv) return BT_MCP_E_ARGS;
    if (c->in_callback) return BT_MCP_E_STATE;
    uint32_t n = 0;
    if (text) while (text[n]) n++;
    int rc = 0;
    if (!c->cancelled) rc = send_tool_result(c->srv, c, text, n, is_error);
    call_free(c);
    return rc;
}

/* ---- 分页 ---- */

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

static void page_of(const bt_mcp_json_req_t *r, uint16_t total,
                    uint32_t *start, uint32_t *end)
{
    uint32_t s = 0;
    const char *cur; uint16_t cl;
    if (!bt_mcp_json_get_str(r, "cursor", &cur, &cl))
        s = parse_digits(cur, cl);
    if (s > total) s = total;
    uint32_t e = s + BT_MCP_CFG_PAGE_SIZE;
    if (e > total) e = total;
    *start = s; *end = e;
}

static int put_next_cursor(bt_mcp_writer_t *w, uint32_t next, uint32_t total)
{
    if (next >= total) return BT_MCP_OK;
    char buf[16];
    int n = snprintf_u32(buf, sizeof buf, next);
    w->need_comma = 1;
    int rc = kv_open(w, "nextCursor");
    if (!rc) rc = bt_mcp_w_str_n(w, buf, (uint32_t)n);
    w->need_comma = 1;
    return rc;
}

/* ---- initialize ---- */

static int version_supported(const char *v, uint16_t len)
{
    static const char *const ks[] = {
        "2025-11-25", "2025-06-18", "2025-03-26", "2024-11-05", "2024-10-07"
    };
    for (unsigned i = 0; i < sizeof ks / sizeof ks[0]; i++)
        if (strlen(ks[i]) == len && !memcmp(ks[i], v, len)) return 1;
    return 0;
}

static int body_initialize(bt_mcp_writer_t *w, const bt_mcp_json_req_t *r, void *arg)
{
    bt_mcp_server_t *sv = arg;
    const char *pv; uint16_t pvl;
    int have = !bt_mcp_json_get_str(r, "protocolVersion", &pv, &pvl)
               && version_supported(pv, pvl);

    int rc = kv_open(w, "protocolVersion");
    if (!rc) {
        if (have) {
            rc = bt_mcp_w_raw(w, "\"", 1);
            if (!rc) rc = bt_mcp_w_raw(w, pv, (uint32_t)pvl);
            if (!rc) rc = bt_mcp_w_raw(w, "\"", 1);
        } else {
            rc = bt_mcp_w_str_lit(w, BT_MCP_PROTO_VERSION);
        }
    }
    w->need_comma = 1;
    /* capabilities：如实反映本实现支持的通知能力 */
    if (!rc) rc = kv_raw_new(w, "capabilities",
        "{\"tools\":{\"listChanged\":true},"
        "\"resources\":{\"subscribe\":true,\"listChanged\":true}",
        sizeof "{\"tools\":{\"listChanged\":true},"
               "\"resources\":{\"subscribe\":true,\"listChanged\":true}" - 1);
    if (!rc && sv->cfg.prompts_count)
        rc = bt_mcp_w_raw(w, ",\"prompts\":{\"listChanged\":true}", 
                          sizeof ",\"prompts\":{\"listChanged\":true}" - 1);
    if (!rc) rc = bt_mcp_w_raw(w, ",\"logging\":{}}",
                               sizeof ",\"logging\":{}}" - 1);
    /* serverInfo */
    if (!rc) rc = bt_mcp_w_raw(w, ",\"serverInfo\":{\"name\":",
                               sizeof ",\"serverInfo\":{\"name\":" - 1);
    if (!rc) rc = bt_mcp_w_str_lit(w, sv->fw_name);
    if (!rc) rc = bt_mcp_w_raw(w, ",\"version\":", sizeof ",\"version\":" - 1);
    if (!rc) rc = bt_mcp_w_str_lit(w, sv->fw_ver);
    if (!rc) rc = bt_mcp_w_raw(w, "}", 1);
    /* _meta.io.btmcp */
    if (!rc) rc = bt_mcp_w_raw(w, ",\"_meta\":{\"io.btmcp/ver\":\"0.4\","
                                  "\"io.btmcp/chan\":{",
                               sizeof ",\"_meta\":{\"io.btmcp/ver\":\"0.4\","
                                      "\"io.btmcp/chan\":{" - 1);
    w->need_comma = 0;
    if (!rc) rc = kv_u32_new(w, "kind",
                             sv->sess && sv->sess->fr ? sv->sess->fr->cap->kind : 0);
    if (!rc) rc = kv_u32_new(w, "mtu",
                             sv->sess && sv->sess->fr ? sv->sess->fr->cap->mtu : 0);
    if (!rc) rc = kv_u32_new(w, "window", sv->sess ? sv->sess->cfg.tx_window : 0);
    if (!rc) rc = kv_u32_new(w, "reliable",
                             sv->sess && sv->sess->fr && sv->sess->fr->cap->reliable);
    if (!rc) rc = kv_u32_new(w, "flow",
                             sv->sess && sv->sess->fr && sv->sess->fr->cap->flow_ctrl);
    if (!rc) rc = bt_mcp_w_raw(w, "}}", 2);
    return rc;
}

/* 空 result：{"jsonrpc":...,"result":{}} */
static int body_empty(bt_mcp_writer_t *w, const bt_mcp_json_req_t *r, void *arg)
{
    (void)r; (void)arg; (void)w;
    return BT_MCP_OK;
}

static int body_ping(bt_mcp_writer_t *w, const bt_mcp_json_req_t *r, void *arg)
{
    (void)r; (void)arg;
    return kv_raw_new(w, "pong", "true", 4);
}

/* ---- tools ---- */

static const bt_mcp_tool_def_t *find_tool(const bt_mcp_server_t *sv,
                                          const char *name, uint16_t nlen)
{
    for (uint16_t i = 0; i < sv->cfg.tools_count; i++) {
        const bt_mcp_tool_def_t *t = &sv->cfg.tools[i];
        if (strlen(t->name) == nlen && !memcmp(t->name, name, nlen)) return t;
    }
    return NULL;
}

static int body_tools_list(bt_mcp_writer_t *w, const bt_mcp_json_req_t *r, void *arg)
{
    bt_mcp_server_t *sv = arg;
    uint32_t start, end;
    page_of(r, sv->cfg.tools_count, &start, &end);

    int rc = bt_mcp_w_raw(w, "\"tools\":[", sizeof "\"tools\":[" - 1);
    w->need_comma = 0;
    for (uint32_t i = start; !rc && i < end; i++) {
        const bt_mcp_tool_def_t *t = &sv->cfg.tools[i];
        if (i != start) rc = bt_mcp_w_raw(w, ",", 1);
        if (!rc) rc = bt_mcp_w_raw(w, "{\"name\":", sizeof "{\"name\":" - 1);
        if (!rc) rc = bt_mcp_w_str_lit(w, t->name);
        if (!rc && t->title) {
            rc = bt_mcp_w_raw(w, ",\"title\":", sizeof ",\"title\":" - 1);
            if (!rc) rc = bt_mcp_w_str_lit(w, t->title);
        }
        if (!rc) rc = bt_mcp_w_raw(w, ",\"description\":",
                                   sizeof ",\"description\":" - 1);
        if (!rc) rc = bt_mcp_w_str_lit(w, t->desc);
        if (!rc) rc = bt_mcp_w_raw(w, ",\"inputSchema\":",
                                   sizeof ",\"inputSchema\":" - 1);
        if (!rc) rc = bt_mcp_w_raw(w, t->input_schema_json,
                                   (uint32_t)strlen(t->input_schema_json));
        if (!rc && t->output_schema_json) {
            rc = bt_mcp_w_raw(w, ",\"outputSchema\":",
                              sizeof ",\"outputSchema\":" - 1);
            if (!rc) rc = bt_mcp_w_raw(w, t->output_schema_json,
                                       (uint32_t)strlen(t->output_schema_json));
        }
        if (!rc && t->annotations) {
            rc = bt_mcp_w_raw(w, ",\"annotations\":{",
                              sizeof ",\"annotations\":{" - 1);
            w->need_comma = 0;
            if (!rc && (t->annotations & BT_MCP_TOOL_READONLY))
                rc = kv_raw_new(w, "readOnlyHint", "true", 4);
            if (!rc && (t->annotations & BT_MCP_TOOL_DESTRUCTIVE))
                rc = kv_raw_new(w, "destructiveHint", "true", 4);
            if (!rc && (t->annotations & BT_MCP_TOOL_IDEMPOTENT))
                rc = kv_raw_new(w, "idempotentHint", "true", 4);
            if (!rc && (t->annotations & BT_MCP_TOOL_OPENWORLD))
                rc = kv_raw_new(w, "openWorldHint", "true", 4);
            if (!rc) rc = bt_mcp_w_raw(w, "}", 1);
        }
        if (!rc) rc = bt_mcp_w_raw(w, "}", 1);
        w->need_comma = 0;
    }
    if (!rc) rc = bt_mcp_w_raw(w, "]", 1);
    if (!rc) rc = put_next_cursor(w, end, sv->cfg.tools_count);
    return rc;
}

/* ---- resources ---- */

#define BT_MCP_URI_PREFIX "bt-mcp://dev/"

static int parse_resource_uri(const char *uri, uint16_t len,
                              const char **name, uint16_t *nlen, uint32_t *cursor)
{
    static const char PFX[] = BT_MCP_URI_PREFIX;
    const uint16_t pfl = (uint16_t)(sizeof PFX - 1);
    if (!uri || len <= pfl || memcmp(uri, PFX, pfl)) return BT_MCP_E_PARSE;
    uint16_t i = pfl;
    uint16_t ns = i;
    while (i < len && uri[i] != '?' && uri[i] != '/') i++;
    if (i == ns) return BT_MCP_E_PARSE;
    *name = uri + ns;
    *nlen = (uint16_t)(i - ns);
    *cursor = 0;
    if (i < len && uri[i] == '?') {
        i++;
        while ((uint16_t)(i + 7) <= len) {
            if (!memcmp(uri + i, "cursor=", 7)) {
                *cursor = parse_digits(uri + i + 7, (uint32_t)(len - i - 7));
                break;
            }
            i++;
        }
    }
    return BT_MCP_OK;
}

static const bt_mcp_resource_def_t *find_resource(const bt_mcp_server_t *sv,
                                                  const char *name, uint16_t nlen)
{
    for (uint16_t i = 0; i < sv->cfg.resources_count; i++) {
        const bt_mcp_resource_def_t *d = &sv->cfg.resources[i];
        if (strlen(d->name) == nlen && !memcmp(d->name, name, nlen)) return d;
    }
    return NULL;
}

static int body_resources_list(bt_mcp_writer_t *w, const bt_mcp_json_req_t *r,
                               void *arg)
{
    bt_mcp_server_t *sv = arg;
    uint32_t start, end;
    page_of(r, sv->cfg.resources_count, &start, &end);

    int rc = bt_mcp_w_raw(w, "\"resources\":[", sizeof "\"resources\":[" - 1);
    w->need_comma = 0;
    for (uint32_t i = start; !rc && i < end; i++) {
        const bt_mcp_resource_def_t *d = &sv->cfg.resources[i];
        if (i != start) rc = bt_mcp_w_raw(w, ",", 1);
        if (!rc) rc = bt_mcp_w_raw(w, "{\"name\":", sizeof "{\"name\":" - 1);
        if (!rc) rc = bt_mcp_w_str_lit(w, d->name);
        if (!rc && d->title) {
            rc = bt_mcp_w_raw(w, ",\"title\":", sizeof ",\"title\":" - 1);
            if (!rc) rc = bt_mcp_w_str_lit(w, d->title);
        }
        if (!rc) rc = bt_mcp_w_raw(w, ",\"uri\":\"bt-mcp://dev/",
                                   sizeof ",\"uri\":\"bt-mcp://dev/" - 1);
        if (!rc) rc = bt_mcp_w_raw(w, d->name, (uint32_t)strlen(d->name));
        if (!rc) rc = bt_mcp_w_raw(w, "\"", 1);
        if (!rc) rc = bt_mcp_w_raw(w, ",\"description\":",
                                   sizeof ",\"description\":" - 1);
        if (!rc) rc = bt_mcp_w_str_lit(w, d->desc);
        if (!rc) rc = bt_mcp_w_raw(w, ",\"mimeType\":",
                                   sizeof ",\"mimeType\":" - 1);
        if (!rc) rc = bt_mcp_w_str_lit(w, d->mime_type ? d->mime_type : "text/plain");
        w->need_comma = 1;
        if (!rc && d->size) rc = kv_u32_new(w, "size", d->size);
        if (!rc) rc = bt_mcp_w_raw(w, "}", 1);
        w->need_comma = 0;
    }
    if (!rc) rc = bt_mcp_w_raw(w, "]", 1);
    if (!rc) rc = put_next_cursor(w, end, sv->cfg.resources_count);
    return rc;
}

static int body_resources_read(bt_mcp_writer_t *w, const bt_mcp_json_req_t *r,
                               void *arg)
{
    bt_mcp_server_t *sv = arg;
    const char *uri; uint16_t ul;
    if (bt_mcp_json_get_str(r, "uri", &uri, &ul)) return BT_MCP_E_PARSE;
    const char *name; uint16_t nl; uint32_t cursor = 0;
    int rc = parse_resource_uri(uri, ul, &name, &nl, &cursor);
    if (rc) return BT_MCP_E_PARSE;
    if (!cursor) {
        const char *cur; uint16_t cl;
        if (!bt_mcp_json_get_str(r, "cursor", &cur, &cl))
            cursor = parse_digits(cur, cl);
    }
    const bt_mcp_resource_def_t *d = find_resource(sv, name, nl);
    if (!d) return BT_MCP_E_ARGS;

    const char *text = ""; uint32_t tlen = 0; uint32_t next = BT_MCP_CURSOR_END;
    if (d->read) {
        rc = d->read(d->user, cursor, &text, &tlen, &next);
        if (rc) return BT_MCP_E_ARGS;
    } else {
        next = BT_MCP_CURSOR_END;
    }

    rc = bt_mcp_w_raw(w, "\"contents\":[{\"uri\":\"bt-mcp://dev/",
                      sizeof "\"contents\":[{\"uri\":\"bt-mcp://dev/" - 1);
    if (!rc) rc = bt_mcp_w_raw(w, d->name, (uint32_t)strlen(d->name));
    if (!rc) rc = bt_mcp_w_raw(w, "\"", 1);
    if (!rc) rc = bt_mcp_w_raw(w, ",\"mimeType\":", sizeof ",\"mimeType\":" - 1);
    if (!rc) rc = bt_mcp_w_str_lit(w, d->mime_type ? d->mime_type : "text/plain");
    if (!rc) w->need_comma = 1;
    if (!rc) {
        esc_t es = { w, 0 };
        bt_mcp_writer_t ew = { .put = esc_put, .sink = &es, .need_comma = 0 };
        rc = kv_open(w, "text");
        if (!rc) rc = bt_mcp_w_raw(w, "\"", 1);
        if (!rc) rc = bt_mcp_w_raw(&ew, text, tlen);
        if (es.err) rc = BT_MCP_E_TOOBIG;
        if (!rc) rc = bt_mcp_w_raw(w, "\"}]", 3);
        w->need_comma = 1;
    }
    if (!rc && next != BT_MCP_CURSOR_END) {
        char buf[16];
        int n = snprintf_u32(buf, sizeof buf, next);
        rc = kv_open(w, "nextCursor");
        if (!rc) rc = bt_mcp_w_str_n(w, buf, (uint32_t)n);
        w->need_comma = 1;
    }
    return rc;
}

/* ---- prompts ---- */

static const bt_mcp_prompt_def_t *find_prompt(const bt_mcp_server_t *sv,
                                              const char *name, uint16_t nlen)
{
    for (uint16_t i = 0; i < sv->cfg.prompts_count; i++) {
        const bt_mcp_prompt_def_t *p = &sv->cfg.prompts[i];
        if (strlen(p->name) == nlen && !memcmp(p->name, name, nlen)) return p;
    }
    return NULL;
}

static int body_prompts_list(bt_mcp_writer_t *w, const bt_mcp_json_req_t *r, void *arg)
{
    bt_mcp_server_t *sv = arg;
    uint32_t start, end;
    page_of(r, sv->cfg.prompts_count, &start, &end);

    int rc = bt_mcp_w_raw(w, "\"prompts\":[", sizeof "\"prompts\":[" - 1);
    w->need_comma = 0;
    for (uint32_t i = start; !rc && i < end; i++) {
        const bt_mcp_prompt_def_t *p = &sv->cfg.prompts[i];
        if (i != start) rc = bt_mcp_w_raw(w, ",", 1);
        if (!rc) rc = bt_mcp_w_raw(w, "{\"name\":", sizeof "{\"name\":" - 1);
        if (!rc) rc = bt_mcp_w_str_lit(w, p->name);
        if (!rc && p->title) {
            rc = bt_mcp_w_raw(w, ",\"title\":", sizeof ",\"title\":" - 1);
            if (!rc) rc = bt_mcp_w_str_lit(w, p->title);
        }
        if (!rc && p->desc) {
            rc = bt_mcp_w_raw(w, ",\"description\":", sizeof ",\"description\":" - 1);
            if (!rc) rc = bt_mcp_w_str_lit(w, p->desc);
        }
        if (!rc && p->arguments_schema_json) {
            rc = bt_mcp_w_raw(w, ",\"argumentsSchema\":",
                              sizeof ",\"argumentsSchema\":" - 1);
            if (!rc) rc = bt_mcp_w_raw(w, p->arguments_schema_json,
                                       (uint32_t)strlen(p->arguments_schema_json));
        }
        if (!rc) rc = bt_mcp_w_raw(w, "}", 1);
        w->need_comma = 0;
    }
    if (!rc) rc = bt_mcp_w_raw(w, "]", 1);
    if (!rc) rc = put_next_cursor(w, end, sv->cfg.prompts_count);
    return rc;
}

static int body_prompts_get(bt_mcp_writer_t *w, const bt_mcp_json_req_t *r, void *arg)
{
    bt_mcp_server_t *sv = arg;
    const char *name; uint16_t nl;
    if (bt_mcp_json_get_str(r, "name", &name, &nl)) return BT_MCP_E_PARSE;
    const bt_mcp_prompt_def_t *p = find_prompt(sv, name, nl);
    if (!p) return BT_MCP_E_ARGS;
    if (!p->get) return BT_MCP_E_ARGS;
    w->need_comma = 0;
    return p->get(r, w, p->user);
}

/* ---- tools/call ---- */

static int body_tool_call(bt_mcp_writer_t *w, const bt_mcp_json_req_t *r, void *arg)
{
    bt_mcp_server_t *sv = arg;
    const char *name; uint16_t nl;
    if (bt_mcp_json_get_str(r, "name", &name, &nl)) return BT_MCP_E_PARSE;
    const bt_mcp_tool_def_t *found = find_tool(sv, name, nl);
    if (!found) return BT_MCP_E_PARSE;      /* 上层转 -32602 */

    /* 授权：v0.4 起真正生效 */
    if (found->authz == BT_MCP_AUTHZ_RESTRICTED) return BT_MCP_E_AUTHZ;
    if (found->authz == BT_MCP_AUTHZ_CONSENT) {
        if (!sv->cfg.consent) return BT_MCP_E_AUTHZ;
        if (sv->cfg.consent(sv->cfg.consent_user, found->name, r) != 0)
            return BT_MCP_E_AUTHZ;
    }

    /* 工具参数在 params.arguments 下 */
    bt_mcp_json_req_t treq;
    if (bt_mcp_json_get_sub(r, "arguments", &treq))
        treq = *r;

    bt_mcp_call_t *c = call_alloc(sv, r);
    if (!c) return BT_MCP_E_NOMEM;
    bt_mcp_json_req_t meta;
    if (!bt_mcp_json_get_sub(r, "_meta", &meta)) {
        uint32_t pt;
        const char *ps; uint16_t psl;
        if (!bt_mcp_json_get_u32(&meta, "progressToken", &pt)) {
            c->has_progress = 1;
            c->progress_token = pt;
        } else if (!bt_mcp_json_get_str(&meta, "progressToken", &ps, &psl)
                   && psl < sizeof c->progress_str) {
            memcpy(c->progress_str, ps, psl);
            c->progress_str[psl] = 0;
            c->progress_str_len = psl;
            c->has_progress = 1;
            c->progress_is_str = 1;
        }
    }
    c->t0 = sv->now_ms;

    int rc = content_array_begin(w);      /* result 头已由 respond() 写 */
    cs_t cs = { w, 0, 0, 0 };
    int trc = BT_MCP_E_ARGS;
    if (!rc) {
        bt_mcp_writer_t tw = { .put = cs_put, .sink = &cs, .need_comma = 0 };
        c->in_callback = 1;
        trc = found->fn(&treq, &tw, c, found->user);
        c->in_callback = 0;
    }
    if (trc == BT_MCP_E_PENDING) return BT_MCP_E_PENDING;  /* 槽保持占用 */
    call_free(c);
    if (rc) return rc;
    if (cs.err) return BT_MCP_E_TOOBIG;
    /* 工具失败且没写任何内容 → 标准诊断文本 */
    if (trc != 0 && cs.items == 0) {
        if (!cs_put(&cs, "tool error", 10) && cs.err) return BT_MCP_E_TOOBIG;
    }
    return tool_result_end(w, &cs, trc != 0, 0);           /* 同步：respond 负责关 */
}

/* ---- notifications/cancelled ---- */

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
            if (rl != (uint32_t)c->id_str_len + 2) continue;
            if (rid[0] != '"' || memcmp(rid + 1, c->id_str, c->id_str_len)) continue;
        } else {
            if (rl == 0 || rid[0] == '"') continue;
            uint32_t k = (rid[0] == '-' || rid[0] == '+') ? 1 : 0;
            int all_digit = (k < rl);
            for (uint32_t q = k; q < rl; q++)
                if (rid[q] < '0' || rid[q] > '9') { all_digit = 0; break; }
            if (!all_digit) continue;
            if (parse_i64_local(rid, rl) != c->id_num) continue;
        }
        c->cancelled = 1;
        return BT_MCP_OK;
    }
    return BT_MCP_OK;
}

/* ---- 订阅 ---- */

static int sub_index(const bt_mcp_server_t *sv, uint16_t idx)
{
    for (uint8_t i = 0; i < sv->nsubs; i++) if (sv->subs[i] == idx) return (int)i;
    return -1;
}

static int handle_subscribe(bt_mcp_server_t *sv, const bt_mcp_json_req_t *r,
                            int subscribe)
{
    const char *uri; uint16_t ul;
    if (bt_mcp_json_get_str(r, "uri", &uri, &ul) || ul < 4) return BT_MCP_E_PARSE;
    const char *name; uint16_t nl; uint32_t cur = 0;
    if (parse_resource_uri(uri, ul, &name, &nl, &cur))
        return BT_MCP_E_PARSE;
    for (uint16_t i = 0; i < sv->cfg.resources_count; i++) {
        const bt_mcp_resource_def_t *d = &sv->cfg.resources[i];
        if (strlen(d->name) != nl || memcmp(d->name, name, nl)) continue;
        if (!d->sub) return BT_MCP_E_ARGS;
        int have = sub_index(sv, i);
        if (subscribe && have < 0) {
            if (sv->nsubs >= BT_MCP_CFG_MAX_SUBS) return BT_MCP_E_NOMEM;
            int rc = d->sub(d->user, 1);
            if (rc) return rc;
            sv->subs[sv->nsubs++] = i;
        } else if (!subscribe && have >= 0) {
            int rc = d->sub(d->user, 0);
            if (rc) return rc;
            sv->subs[have] = sv->subs[--sv->nsubs];
        }
        return BT_MCP_OK;
    }
    return BT_MCP_E_ARGS;
}

/* ---- logging/setLevel ---- */

static int log_level_parse(const char *p, uint16_t n, uint8_t *out)
{
    static const char *const names[] = {
        "debug", "info", "notice", "warning", "error", "critical", "alert", "emergency"
    };
    if (n < 1) return BT_MCP_E_PARSE;
    for (uint8_t i = 0; i < 8; i++) {
        if (strlen(names[i]) == (size_t)n && !memcmp(names[i], p, n)) {
            *out = i;
            return BT_MCP_OK;
        }
    }
    return BT_MCP_E_PARSE;
}

/* ---- server→client 请求 ---- */

static int handle_client_response(bt_mcp_server_t *sv, const bt_mcp_json_req_t *r)
{
    if (!r->has_id || r->id_is_str) return BT_MCP_OK;
    for (int i = 0; i < BT_MCP_CFG_MAX_S2C_REQ; i++) {
        struct bt_mcp_s2c_req *q = &sv->s2c[i];
        if (!q->in_use || (int64_t)q->id != r->id_num) continue;
        q->in_use = 0;
        const char *res = NULL, *err = NULL;
        uint32_t rl = 0, el = 0;
        int rc = 0;
        if (!bt_mcp_json_top_raw(r, "result", &res, &rl)) {
            rc = 0;
        } else if (!bt_mcp_json_top_raw(r, "error", &err, &el)) {
            rc = BT_MCP_E_IO;
            res = err; rl = el;
        } else {
            rc = BT_MCP_E_PARSE;
        }
        if (q->cb) q->cb(q->user, rc, res, rl);
        return BT_MCP_OK;
    }
    return BT_MCP_OK;
}

int bt_mcp_server_inflight(const bt_mcp_server_t *sv)
{
    int n = 0;
    if (!sv) return 0;
    for (int i = 0; i < BT_MCP_CFG_MAX_S2C_REQ; i++) if (sv->s2c[i].in_use) n++;
    return n;
}

int bt_mcp_server_request(bt_mcp_server_t *sv, const char *method,
                          const char *params_json, uint32_t params_len,
                          bt_mcp_server_resp_fn cb, void *user)
{
    if (!sv || !method) return BT_MCP_E_ARGS;
    if (!sv->sess) return BT_MCP_E_STATE;
    if (bt_mcp_session_state(sv->sess) != BT_MCP_S_READY) return BT_MCP_E_STATE;

    struct bt_mcp_s2c_req *slot = NULL;
    for (int i = 0; i < BT_MCP_CFG_MAX_S2C_REQ; i++)
        if (!sv->s2c[i].in_use) { slot = &sv->s2c[i]; break; }
    if (!slot) return BT_MCP_E_BUSY;

    uint32_t id = sv->next_req_id++;
    if (!sv->next_req_id) sv->next_req_id = 1;

    memsink_t ms = { sv->rbuf, 0, (uint32_t)sizeof sv->rbuf };
    bt_mcp_writer_t w = { .put = mem_put, .sink = &ms, .need_comma = 0 };
    char idb[16];
    int idn = snprintf_u32(idb, sizeof idb, id);
    int rc = bt_mcp_w_raw(&w, "{\"jsonrpc\":\"2.0\",\"id\":",
                          sizeof "{\"jsonrpc\":\"2.0\",\"id\":" - 1);
    if (!rc) rc = bt_mcp_w_raw(&w, idb, (uint32_t)idn);
    if (!rc) rc = bt_mcp_w_raw(&w, ",\"method\":", sizeof ",\"method\":" - 1);
    if (!rc) rc = bt_mcp_w_str_lit(&w, method);
    if (!rc && params_json) {
        rc = bt_mcp_w_raw(&w, ",\"params\":", sizeof ",\"params\":" - 1);
        if (!rc) rc = bt_mcp_w_raw(&w, params_json, params_len);
    }
    if (!rc) rc = bt_mcp_w_raw(&w, "}", 1);
    if (rc) return rc;

    rc = tx_response(sv, sv->rbuf, ms.len);
    if (rc) return rc;
    memset(slot, 0, sizeof(*slot));
    slot->in_use = 1;
    slot->id = id;
    slot->t0 = sv->now_ms;
    slot->cb = cb;
    slot->user = user;
    return BT_MCP_OK;
}

/* ---- 通知 ---- */

int bt_mcp_server_notify(bt_mcp_server_t *sv, const char *method,
                         const char *params_json, uint32_t params_len)
{
    if (!sv || !method) return BT_MCP_E_ARGS;
    memsink_t ms = { sv->nbuf, 0, BT_MCP_CFG_NOTIFY_BUF };
    bt_mcp_writer_t w = { .put = mem_put, .sink = &ms, .need_comma = 0 };
    int rc = bt_mcp_w_raw(&w, "{\"jsonrpc\":\"2.0\",\"method\":",
                          sizeof "{\"jsonrpc\":\"2.0\",\"method\":" - 1);
    if (!rc) rc = bt_mcp_w_str_lit(&w, method);
    if (!rc && params_json) {
        rc = bt_mcp_w_raw(&w, ",\"params\":", sizeof ",\"params\":" - 1);
        if (!rc) rc = bt_mcp_w_raw(&w, params_json, params_len);
    }
    if (!rc) rc = bt_mcp_w_raw(&w, "}", 1);
    if (rc) return rc;
    return tx_response(sv, sv->nbuf, ms.len);
}

int bt_mcp_server_notify_resource_updated(bt_mcp_server_t *sv, const char *uri)
{
    if (!sv || !uri) return BT_MCP_E_ARGS;
    const char *name; uint16_t nl; uint32_t cur = 0;
    if (parse_resource_uri(uri, (uint16_t)strlen(uri), &name, &nl, &cur))
        return BT_MCP_E_PARSE;
    int hit = -1;
    for (uint16_t i = 0; i < sv->cfg.resources_count; i++) {
        const bt_mcp_resource_def_t *d = &sv->cfg.resources[i];
        if (strlen(d->name) == nl && !memcmp(d->name, name, nl)) { hit = i; break; }
    }
    if (hit < 0 || sub_index(sv, (uint16_t)hit) < 0) return BT_MCP_OK; /* 未订阅：静默 */

    char pb[160];
    uint32_t n = 0;
    const char *a = "{\"uri\":\"";
    while (a[n]) { pb[n] = a[n]; n++; }
    for (uint16_t i = 0; uri[i] && n < sizeof pb - 4; i++) pb[n++] = uri[i];
    pb[n++] = '"'; pb[n++] = '}';
    return bt_mcp_server_notify(sv, "notifications/resources/updated", pb, n);
}

int bt_mcp_server_notify_list_changed(bt_mcp_server_t *sv, const char *what)
{
    if (!sv || !what) return BT_MCP_E_ARGS;
    char m[64];
    uint32_t n = 0;
    const char *pre = "notifications/";
    while (pre[n]) { m[n] = pre[n]; n++; }
    for (uint32_t i = 0; what[i] && n < sizeof m - 12; i++) m[n++] = what[i];
    const char *suf = "/list_changed";
    for (uint32_t i = 0; suf[i]; i++) m[n++] = suf[i];
    m[n] = 0;
    return bt_mcp_server_notify(sv, m, NULL, 0);
}

int bt_mcp_server_log(bt_mcp_server_t *sv, uint8_t level, const char *logger,
                      const char *text, uint32_t len)
{
    if (!sv) return BT_MCP_E_ARGS;
    if (level < sv->log_level) return BT_MCP_OK;   /* 低于阈值：丢弃 */
    static const char *const names[] = {
        "debug", "info", "notice", "warning", "error", "critical", "alert", "emergency"
    };
    if (level > 7) level = 7;

    memsink_t ms = { sv->nbuf, 0, BT_MCP_CFG_NOTIFY_BUF };
    bt_mcp_writer_t w = { .put = mem_put, .sink = &ms, .need_comma = 0 };
    int rc = bt_mcp_w_raw(&w, "{\"jsonrpc\":\"2.0\",\"method\":"
                               "\"notifications/message\",\"params\":{",
                          sizeof "{\"jsonrpc\":\"2.0\",\"method\":"
                                 "\"notifications/message\",\"params\":{" - 1);
    if (!rc) rc = kv_str_slice(&w, "level", names[level],
                               (uint32_t)strlen(names[level]));   /* 必须带引号 */
    if (!rc && logger) rc = kv_str_slice(&w, "logger", logger,
                                        (uint32_t)strlen(logger));
    esc_t es = { &w, 0 };
    bt_mcp_writer_t ew = { .put = esc_put, .sink = &es, .need_comma = 0 };
    if (!rc) rc = kv_open(&w, "data");
    if (!rc) rc = bt_mcp_w_raw(&w, "\"", 1);
    if (!rc) rc = bt_mcp_w_raw(&ew, text ? text : "", len);
    if (es.err) rc = BT_MCP_E_TOOBIG;
    if (!rc) rc = bt_mcp_w_raw(&w, "\"}}", 3);
    if (rc) return rc;
    return tx_response(sv, sv->nbuf, ms.len);
}

/* ---- 入口 ---- */

int bt_mcp_server_rx(bt_mcp_server_t *sv, const uint8_t *json, uint32_t len)
{
    bt_mcp_trace(BT_MCP_TRACE_DBG, "l5:rx", json,
                 (uint16_t)(len > 24 ? 24 : len));
    if (!sv || (!json && len)) return BT_MCP_E_ARGS;
    bt_mcp_json_req_t r;
    int rc = bt_mcp_json_parse_msg((const char *)json, len, &r);
    if (rc) {
        /* 语法错：能定位 id 就回 -32700，否则静默（避免放大攻击面） */
        return r.has_id ? respond_error(sv, &r, -32700, "Parse error")
                        : BT_MCP_E_PARSE;
    }

    /* 无 method 有 id → client 对本端 S→C 请求的响应 */
    if (!r.method) {
        if (r.has_id) return handle_client_response(sv, &r);
        return BT_MCP_E_PARSE;
    }

    if (method_is(&r, "initialize"))
        return respond(sv, &r, body_initialize, sv);

    if (method_is(&r, "notifications/initialized")) {
        sv->initialized = 1;
        bt_mcp_session_mark_ready(sv->sess);
        return BT_MCP_OK;
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
        if (rc == BT_MCP_E_PENDING) return BT_MCP_OK;
        if (rc == BT_MCP_E_PARSE)
            return respond_error(sv, &r, -32602, "Invalid params");
        if (rc == BT_MCP_E_AUTHZ)
            return respond_error(sv, &r, -32003, "Permission denied");
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
    if (method_is(&r, "resources/subscribe") ||
        method_is(&r, "resources/unsubscribe")) {
        int sub = method_is(&r, "resources/subscribe");
        rc = handle_subscribe(sv, &r, sub);
        if (rc == BT_MCP_E_PARSE || rc == BT_MCP_E_ARGS)
            return respond_error(sv, &r, -32602, "Invalid params");
        if (rc) return respond_error(sv, &r, -32603, "Subscribe failed");
        return respond(sv, &r, body_empty, sv);  /* 空 result {} */
    }
    if (method_is(&r, "prompts/list"))
        return respond(sv, &r, body_prompts_list, sv);
    if (method_is(&r, "prompts/get")) {
        rc = respond(sv, &r, body_prompts_get, sv);
        if (rc == BT_MCP_E_PARSE)
            return respond_error(sv, &r, -32602, "Invalid params");
        if (rc == BT_MCP_E_ARGS)
            return respond_error(sv, &r, -32602, "Prompt not found");
        return rc;
    }
    if (method_is(&r, "logging/setLevel")) {
        const char *lv; uint16_t lvl;
        if (bt_mcp_json_get_str(&r, "level", &lv, &lvl))
            return respond_error(sv, &r, -32602, "Invalid params");
        uint8_t l;
        if (log_level_parse(lv, lvl, &l)) 
            return respond_error(sv, &r, -32602, "Invalid params");
        sv->log_level = l;
        return respond(sv, &r, body_empty, sv);  /* 空 result {} */
    }
    if (r.has_id)
        return respond_error(sv, &r, -32601, "Method not found");
    return BT_MCP_OK;
}

void bt_mcp_server_tick(bt_mcp_server_t *sv, uint32_t now_ms)
{
    if (!sv) return;
    sv->now_ms = now_ms;
    for (int i = 0; i < BT_MCP_CFG_MAX_S2C_REQ; i++) {
        struct bt_mcp_s2c_req *q = &sv->s2c[i];
        if (!q->in_use) continue;
        if (now_ms - q->t0 <= BT_MCP_CFG_S2C_TOUT_MS) continue;
        q->in_use = 0;
        if (q->cb) q->cb(q->user, BT_MCP_E_AGAIN, NULL, 0);
    }
}
