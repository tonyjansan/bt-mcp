/* writer.c —— 流式 JSON writer 实现。零 malloc、零 DOM。 */
#include "bt_mcp/writer.h"
#include "bt_mcp/err.h"
#include <string.h>
#include <stdio.h>

int bt_mcp_w_raw(bt_mcp_writer_t *w, const char *data, uint32_t len)
{
    if (!w || !w->put || (!data && len)) return BT_MCP_E_ARGS;
    return w->put(w->sink, data, len) ? BT_MCP_E_IO : BT_MCP_OK;
}

static int put_lit(bt_mcp_writer_t *w, const char *s)
{
    return bt_mcp_w_raw(w, s, (uint32_t)strlen(s));
}

int bt_mcp_w_str_lit(bt_mcp_writer_t *w, const char *s)
{
    int rc = put_lit(w, "\"");
    static const char HEX[] = "0123456789abcdef";
    for (const char *p = s ? s : ""; *p && !rc; p++) {
        unsigned char c = (unsigned char)*p;
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

int bt_mcp_w_begin_result(bt_mcp_writer_t *w, int64_t id)
{
    /* buf 须容纳 i64 最长 20 位：26 前缀+20+11 后缀=57（M4a 根修：
       buf[40] 时 snprintf 截断但按返回值 n 写出 → 栈越界读）。 */
    char buf[64];
    int n = snprintf(buf, sizeof buf, "{\"jsonrpc\":\"2.0\",\"id\":%lld,\"result\":{",
                     (long long)id);
    if (n < 0) return BT_MCP_E_ARGS;
    w->need_comma = 0;
    return bt_mcp_w_raw(w, buf, (uint32_t)n);
}

int bt_mcp_w_begin_error(bt_mcp_writer_t *w, int64_t id,
                         int code, const char *message)
{
    /* 同 begin_result：i64 最长 20 位需 43+ 字节，buf[32] 越界读根修 */
    char buf[64];
    int n = snprintf(buf, sizeof buf, "{\"jsonrpc\":\"2.0\",\"id\":%lld,", (long long)id);
    int rc = bt_mcp_w_raw(w, buf, (uint32_t)n);
    if (!rc) rc = put_lit(w, "\"error\":{\"code\":");
    n = snprintf(buf, sizeof buf, "%d", code);
    if (!rc) rc = bt_mcp_w_raw(w, buf, (uint32_t)n);
    if (!rc) rc = put_lit(w, ",\"message\":");
    if (!rc) rc = bt_mcp_w_str_lit(w, message);
    return rc ? rc : put_lit(w, "}}");
}

int bt_mcp_w_end(bt_mcp_writer_t *w)
{
    return put_lit(w, "}}");
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
    int rc = bt_mcp_w_kv_raw(w, key, buf, (uint32_t)(n > 0 ? n : 0));
    return n < 0 ? BT_MCP_E_ARGS : rc;
}
