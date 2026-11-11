/* writer.c —— 流式 JSON writer 实现。零 malloc、零 DOM、零 libc 格式化。 */
#include "bt_mcp/writer.h"
#include "bt_mcp/err.h"
#include <string.h>

int bt_mcp_w_raw(bt_mcp_writer_t *w, const char *data, uint32_t len)
{
    if (!w || !w->put || !w->sink || (!data && len)) return BT_MCP_E_ARGS;
    /* v0.6：透传 sink 的错误码，不再压平成 E_IO。
     * 原因：调用方需要区分「缓冲放不下（E_TOOBIG，可分段/换档）」与
     * 「链路 I/O 故障（E_IO，要重连）」——两者的处置完全不同。
     * v0.5 之前一律返回 E_IO，把小缓冲配置下的容量问题伪装成 I/O 故障。 */
    return w->put(w->sink, data, len);
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

/* 自研整数格式化：不依赖 libc 的 snprintf。
 * 动机：writer 是设备端热路径，snprintf 会带入 2~6KB 的格式化代码，
 * 与「8/16 位 MCU 可裁剪」的定位冲突。语义需与 snprintf("%lld"/"%d"/"%lu"/"%llu")
 * 完全一致（含 INT64_MIN 的溢出边界），由差分与单测守住。 */
#if defined(__GNUC__) || defined(__clang__)
#  define BT_MCP_NOINLINE __attribute__((noinline))
#else
#  define BT_MCP_NOINLINE
#endif
/* 不内联：这 4 个调用点若各自内联，目标文件会多出数份相同代码
 * （实测 .o 从 3638B 涨到 6265B）；保持单份反而更省。 */
static BT_MCP_NOINLINE int fmt_u64(char *buf, uint32_t cap, uint64_t v)
{
    char tmp[24];
    int n = 0, i;
    do { tmp[n++] = (char)('0' + (int)(v % 10u)); v /= 10u; } while (v);
    if ((uint32_t)n + 1 > cap) return -1;
    for (i = 0; i < n; i++) buf[i] = tmp[n - 1 - i];
    buf[n] = 0;
    return n;
}

static BT_MCP_NOINLINE int fmt_i64(char *buf, uint32_t cap, int64_t v)
{
    if (v < 0) {
        if (cap < 2) return -1;
        buf[0] = '-';
        /* -INT64_MIN 溢出：按无符号取绝对值 */
        uint64_t mag = (uint64_t)(-(v + 1)) + 1u;
        int n = fmt_u64(buf + 1, cap - 1, mag);
        return n < 0 ? -1 : n + 1;
    }
    return fmt_u64(buf, cap, (uint64_t)v);
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
    int n = fmt_i64(buf, sizeof buf, id);
    if (n < 0) return BT_MCP_E_ARGS;
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
    int n = fmt_i64(buf, sizeof buf, (int64_t)code);
    if (!rc && n < 0) rc = BT_MCP_E_ARGS;
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
    int n = fmt_u64(buf, sizeof buf, (uint64_t)v);
    if (n < 0) return BT_MCP_E_ARGS;
    return bt_mcp_w_kv_raw(w, key, buf, (uint32_t)n);
}

int bt_mcp_w_kv_u64(bt_mcp_writer_t *w, const char *key, uint64_t v)
{
    char buf[32];
    int n = fmt_u64(buf, sizeof buf, (uint64_t)v);
    if (n < 0) return BT_MCP_E_ARGS;
    return bt_mcp_w_kv_raw(w, key, buf, (uint32_t)n);
}
