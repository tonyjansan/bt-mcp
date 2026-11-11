/* writer.c —— 流式 JSON writer 实现。零 malloc、零 DOM。 */
#include "bt_mcp/writer.h"
#include "bt_mcp/err.h"
#include <string.h>
#include <stdio.h>

int bt_mcp_w_raw(bt_mcp_writer_t *w, const char *data, uint32_t len)
{
    if (!w || !w->put || !w->sink || (!data && len)) return BT_MCP_E_ARGS;
    return w->put(w->sink, data, len) ? BT_MCP_E_IO : BT_MCP_OK;
}

static int put_lit(bt_mcp_writer_t *w, const char *s)
{
    return bt_mcp_w_raw(w, s, (uint32_t)strlen(s));
}

int bt_mcp_w_str_n(bt_mcp_writer_t *w, const char *s, uint32_t n)
{
    static const char HEX[] = "0123456789abcdef";
    int rc = put_lit(w, "\"");
    for (uint32_t i = 0; !rc && i < n; i++) {
        unsigned char c = (unsigned char)s[i];
        char esc[8];
        switch (c) {
        case '"':  esc[0] = '\\'; esc[1] = '"';  rc = bt_mcp_w_raw(w, esc, 2); break;
        case '\\': esc[0] = '\\'; esc[1] = '\\'; rc = bt_mcp_w_raw(w, esc, 2); break;
        case '\n': esc[0] = '\\'; esc[1] = 'n';  rc = bt_mcp_w_raw(w, esc, 2); break;
        case '\r': esc[0] = '\\'; esc[1] = 'r';  rc = bt_mcp_w_raw(w, esc, 2); break;
        case '\t': esc[0] = '\\'; esc[1] = 't';  rc = bt_mcp_w_raw(w, esc, 2); break;
        default:
            if (c < 0x20) {
                esc[0] = '\\'; esc[1] = 'u'; esc[2] = '0'; esc[3] = '0';
                esc[4] = HEX[c >> 4]; esc[5] = HEX[c & 15];
                rc = bt_mcp_w_raw(w, esc, 6);
            } else {
                rc = bt_mcp_w_raw(w, (const char *)&c, 1);
            }
        }
    }
    return rc ? rc : put_lit(w, "\"");
}

int bt_mcp_w_str_lit(bt_mcp_writer_t *w, const char *s)
{
    if (!s) s = "";
    return bt_mcp_w_str_n(w, s, (uint32_t)strlen(s));
}

/* id 写出：字符串 id 用 "…" 包裹原文段（原文段已含转义，直接透传即合法 JSON） */
static int put_id(bt_mcp_writer_t *w, int64_t id, const char *id_str, uint16_t id_len)
{
    char buf[32];
    if (id_str) {
        int rc = put_lit(w, "\"");
        if (!rc) rc = bt_mcp_w_raw(w, id_str, id_len);
        return rc ? rc : put_lit(w, "\"");
    }
    int n = snprintf(buf, sizeof buf, "%lld", (long long)id);
    if (n < 0 || (size_t)n >= sizeof buf) return BT_MCP_E_ARGS;
    return bt_mcp_w_raw(w, buf, (uint32_t)n);
}

int bt_mcp_w_begin_result_id(bt_mcp_writer_t *w, int64_t id,
                             const char *id_str, uint16_t id_len)
{
    int rc = put_lit(w, "{\"jsonrpc\":\"2.0\",\"id\":");
    if (!rc) rc = put_id(w, id, id_str, id_len);
    w->need_comma = 0;
    return rc ? rc : put_lit(w, ",\"result\":{");
}

int bt_mcp_w_begin_result(bt_mcp_writer_t *w, int64_t id)
{
    return bt_mcp_w_begin_result_id(w, id, NULL, 0);
}

int bt_mcp_w_begin_error_id(bt_mcp_writer_t *w, int64_t id,
                            const char *id_str, uint16_t id_len,
                            int code, const char *message)
{
    char buf[16];
    int rc = put_lit(w, "{\"jsonrpc\":\"2.0\",\"id\":");
    if (!rc) rc = put_id(w, id, id_str, id_len);
    if (!rc) rc = put_lit(w, ",\"error\":{\"code\":");
    int n = snprintf(buf, sizeof buf, "%d", code);
    if (!rc && (n < 0 || (size_t)n >= sizeof buf)) rc = BT_MCP_E_ARGS;
    if (!rc) rc = bt_mcp_w_raw(w, buf, (uint32_t)n);
    if (!rc) rc = put_lit(w, ",\"message\":");
    if (!rc) rc = bt_mcp_w_str_lit(w, message);
    return rc ? rc : put_lit(w, "}}");
}

int bt_mcp_w_begin_error(bt_mcp_writer_t *w, int64_t id, int code, const char *message)
{
    return bt_mcp_w_begin_error_id(w, id, NULL, 0, code, message);
}

int bt_mcp_w_end(bt_mcp_writer_t *w)
{
    return put_lit(w, "}}");
}

int bt_mcp_w_key(bt_mcp_writer_t *w, const char *key)
{
    if (!w || !key) return BT_MCP_E_ARGS;
    int rc = BT_MCP_OK;
    if (w->need_comma) rc = bt_mcp_w_raw(w, ",", 1);
    if (!rc) rc = bt_mcp_w_raw(w, "\"", 1);
    if (!rc) rc = bt_mcp_w_raw(w, key, (uint32_t)strlen(key));
    if (!rc) rc = bt_mcp_w_raw(w, "\":", 2);
    w->need_comma = 0;
    return rc;
}

int bt_mcp_w_kv_str(bt_mcp_writer_t *w, const char *key, const char *val)
{
    int rc = w->need_comma ? put_lit(w, ",") : 0;
    w->need_comma = 1;
    if (rc) return rc;
    if ((rc = put_lit(w, "\""))) return rc;
    if ((rc = put_lit(w, key))) return rc;
    if ((rc = put_lit(w, "\":"))) return rc;
    return bt_mcp_w_str_lit(w, val);
}

int bt_mcp_w_kv_raw(bt_mcp_writer_t *w, const char *key,
                    const char *raw, uint32_t len)
{
    int rc = w->need_comma ? put_lit(w, ",") : 0;
    w->need_comma = 1;
    if (rc) return rc;
    if ((rc = put_lit(w, "\""))) return rc;
    if ((rc = put_lit(w, key))) return rc;
    if ((rc = put_lit(w, "\":"))) return rc;
    return bt_mcp_w_raw(w, raw, len);
}

int bt_mcp_w_kv_u32(bt_mcp_writer_t *w, const char *key, uint32_t v)
{
    char buf[24];
    int n = snprintf(buf, sizeof buf, "%lu", (unsigned long)v);
    if (n < 0 || (size_t)n >= sizeof buf) return BT_MCP_E_ARGS;
    return bt_mcp_w_kv_raw(w, key, buf, (uint32_t)n);
}

int bt_mcp_w_kv_u64(bt_mcp_writer_t *w, const char *key, uint64_t v)
{
    char buf[32];
    int n = snprintf(buf, sizeof buf, "%llu", (unsigned long long)v);
    if (n < 0 || (size_t)n >= sizeof buf) return BT_MCP_E_ARGS;
    return bt_mcp_w_kv_raw(w, key, buf, (uint32_t)n);
}
