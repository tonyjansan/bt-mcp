/* json.c —— 单遍 SAX 解析实现。
 *
 * 结构判例：
 * 1. 只认 MCP 请求形态：顶层必须是对象 {"jsonrpc":…,"method":…,…}。
 *    method/id/params 取自顶层；params 值以原文片段记录（零拷贝）。
 * 2. 深度上限 BT_MCP_CFG_JSON_MAX_DEPTH（默认 6）：防深嵌套栈爆。
 * 3. 字符串严格 JSON 转义解码（含 \u 代理对合并）。
 * 4. 键名不再截断：超长键按「未知键」跳过，避免静默错配参数。
 * 5. 数字/字面量按 RFC 8259 严格校验；非法 token → E_PARSE。
 */
#include "bt_mcp/json.h"
#include "bt_mcp/err.h"
#include <string.h>

#define MAX_DEPTH BT_MCP_CFG_JSON_MAX_DEPTH

/* --- 微型游标 --- */
typedef struct {
    const char *p, *end;
    int depth;
} cur_t;

static void skip_ws(cur_t *c)
{
    while (c->p < c->end && (*c->p == ' ' || *c->p == '\t'
        || *c->p == '\n' || *c->p == '\r')) c->p++;
}

static int peek(cur_t *c, char ch)
{
    skip_ws(c);
    return c->p < c->end && *c->p == ch;
}

static int is_digit(char c) { return c >= '0' && c <= '9'; }

/* JSON 数字：-?(0|[1-9]\d*)(\.\d+)?([eE][+-]?\d+)? */
static int scan_number(cur_t *c)
{
    const char *p = c->p;
    if (p < c->end && *p == '-') p++;
    if (p >= c->end) return BT_MCP_E_PARSE;
    if (*p == '0') p++;
    else if (is_digit(*p)) { while (p < c->end && is_digit(*p)) p++; }
    else return BT_MCP_E_PARSE;
    if (p < c->end && *p == '.') {
        p++;
        if (p >= c->end || !is_digit(*p)) return BT_MCP_E_PARSE;
        while (p < c->end && is_digit(*p)) p++;
    }
    if (p < c->end && (*p == 'e' || *p == 'E')) {
        p++;
        if (p < c->end && (*p == '+' || *p == '-')) p++;
        if (p >= c->end || !is_digit(*p)) return BT_MCP_E_PARSE;
        while (p < c->end && is_digit(*p)) p++;
    }
    c->p = p;
    return BT_MCP_OK;
}

static int scan_literal(cur_t *c, const char *lit)
{
    size_t n = strlen(lit);
    if ((size_t)(c->end - c->p) < n || memcmp(c->p, lit, n)) return BT_MCP_E_PARSE;
    c->p += n;
    return BT_MCP_OK;
}

/* 前进一个 JSON 值（对象/数组/字符串/数字/字面量），返回值片段 [vout,vend)。
   深度超限或残缺返回负错误码。 */
static int skip_value(cur_t *c, const char **vout, const char **vend, int *is_str)
{
    skip_ws(c);
    if (c->p >= c->end) return BT_MCP_E_PARSE;
    const char *start = c->p;
    *is_str = 0;
    switch (*c->p) {
    case '"': {                        /* 字符串（含转义；非法转义一律报错） */
        c->p++;
        while (c->p < c->end) {
            if (*c->p == '\\') {
                c->p++;
                if (c->p >= c->end) return BT_MCP_E_PARSE;
                if (*c->p == 'u') {
                    if (c->end - c->p < 5) return BT_MCP_E_PARSE;
                    for (int i = 1; i <= 4; i++)
                        if (!((c->p[i] >= '0' && c->p[i] <= '9')
                              || ((c->p[i] | 0x20) >= 'a' && (c->p[i] | 0x20) <= 'f')))
                            return BT_MCP_E_PARSE;
                    c->p += 4;
                } else if (!strchr("\"\\/bfnrt", *c->p)) {
                    return BT_MCP_E_PARSE;
                }
                c->p++;
                continue;
            }
            if (*c->p == '"') { c->p++; *vout = start; *vend = c->p; *is_str = 1;
                                return BT_MCP_OK; }
            c->p++;
        }
        return BT_MCP_E_PARSE;
    }
    case '{': case '[': {
        if (++c->depth > MAX_DEPTH) return BT_MCP_E_PARSE;
        char open = *c->p, close = (open == '{') ? '}' : ']';
        c->p++;
        for (;;) {
            if (peek(c, close)) { c->p++; c->depth--; *vout = start; *vend = c->p;
                                  return BT_MCP_OK; }
            if (c->p >= c->end) return BT_MCP_E_PARSE;
            const char *v0, *v1; int dummy;
            if (open == '{') {          /* 对象内先跳键 */
                if (!peek(c, '"')) return BT_MCP_E_PARSE;
                if (skip_value(c, &v0, &v1, &dummy)) return BT_MCP_E_PARSE;
                if (!peek(c, ':')) return BT_MCP_E_PARSE;
                c->p++;
            }
            if (skip_value(c, &v0, &v1, &dummy)) return BT_MCP_E_PARSE;
            if (peek(c, ',')) c->p++;
            else if (!peek(c, close) && c->p < c->end) return BT_MCP_E_PARSE;
        }
    }
    default:                            /* 数字 / true / false / null */
        if (*c->p == 't') { if (scan_literal(c, "true")) return BT_MCP_E_PARSE; }
        else if (*c->p == 'f') { if (scan_literal(c, "false")) return BT_MCP_E_PARSE; }
        else if (*c->p == 'n') { if (scan_literal(c, "null")) return BT_MCP_E_PARSE; }
        else if (*c->p == '-' || is_digit(*c->p)) {
            if (scan_number(c)) return BT_MCP_E_PARSE;
        } else {
            return BT_MCP_E_PARSE;
        }
        *vout = start; *vend = c->p;
        return BT_MCP_OK;
    }
}

/* 十六进制 → 码元 */
static unsigned hex4(const char *s)
{
    unsigned v = 0;
    for (int i = 0; i < 4; i++) {
        char h = s[i];
        v <<= 4;
        v |= (unsigned)(h <= '9' ? h - '0' : (h | 0x20) - 'a' + 10);
    }
    return v;
}

/* 把码点写成 UTF-8，返回字节数 */
static int put_utf8(unsigned cp, char *dst)
{
    if (cp < 0x80) { dst[0] = (char)cp; return 1; }
    if (cp < 0x800) {
        dst[0] = (char)(0xC0 | (cp >> 6));
        dst[1] = (char)(0x80 | (cp & 0x3F));
        return 2;
    }
    if (cp < 0x10000) {
        dst[0] = (char)(0xE0 | (cp >> 12));
        dst[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
        dst[2] = (char)(0x80 | (cp & 0x3F));
        return 3;
    }
    dst[0] = (char)(0xF0 | (cp >> 18));
    dst[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
    dst[2] = (char)(0x80 | ((cp >> 6) & 0x3F));
    dst[3] = (char)(0x80 | (cp & 0x3F));
    return 4;
}

uint16_t bt_mcp_json_decode_str(const char *s, const char *e,
                               char *dst, uint16_t cap, uint16_t *need)
{
    uint16_t n = 0;
    uint32_t total = 0;
    const char *p = s;
    if (p < e && *p == '"') p++;
    if (e > p && e[-1] == '"') e--;
    while (p < e) {
        char out[4];
        int k;
        if (*p == '\\') {
            p++;
            if (p >= e) break;
            switch (*p) {
            case 'n': out[0] = '\n'; k = 1; p++; break;
            case 't': out[0] = '\t'; k = 1; p++; break;
            case 'r': out[0] = '\r'; k = 1; p++; break;
            case 'b': out[0] = '\b'; k = 1; p++; break;
            case 'f': out[0] = '\f'; k = 1; p++; break;
            case 'u': {
                if (e - p < 5) { p = e; continue; }
                unsigned cp = hex4(p + 1);
                p += 5;
                if (cp >= 0xD800 && cp <= 0xDBFF && e - p >= 6
                    && p[0] == '\\' && p[1] == 'u') {
                    unsigned lo = hex4(p + 2);
                    if (lo >= 0xDC00 && lo <= 0xDFFF) {
                        cp = 0x10000u + ((cp - 0xD800u) << 10) + (lo - 0xDC00u);
                        p += 6;
                    }
                }
                if (cp >= 0xD800 && cp <= 0xDFFF) cp = 0xFFFD; /* 孤代理→替换符 */
                k = put_utf8(cp, out);
                break;
            }
            default: out[0] = *p; k = 1; p++; break;   /* \" \\ \/ */
            }
        } else {
            out[0] = *p++; k = 1;
        }
        for (int i = 0; i < k; i++) {
            if (n < cap) dst[n++] = out[i];
            total++;
        }
    }
    if (need) *need = (uint16_t)(total > 0xFFFFu ? 0xFFFFu : total);
    return n;
}

/* 片段内 i64 解析（不依赖 NUL 结尾；溢出饱和，避免 UB） */
static int64_t parse_i64(const char *p, const char *e)
{
    int neg = 0;
    if (p < e && (*p == '-' || *p == '+')) { neg = (*p == '-'); p++; }
    uint64_t acc = 0;
    const uint64_t lim = neg ? 9223372036854775808ull : 9223372036854775807ull;
    while (p < e && is_digit(*p)) {
        unsigned d = (unsigned)(*p - '0');
        if (acc > (lim - d) / 10) { acc = lim; while (p < e && is_digit(*p)) p++; break; }
        acc = acc * 10 + d;
        p++;
    }
    if (neg) return (acc >= 9223372036854775808ull) ? (int64_t)(-9223372036854775807LL - 1)
                                                    : -(int64_t)acc;
    return (int64_t)acc;
}

int bt_mcp_json_parse(const char *src, uint32_t len, bt_mcp_json_req_t *out)
{
    if (!src || !out) return BT_MCP_E_ARGS;
    memset(out, 0, sizeof(*out));
    out->src = src; out->len = len;
    out->id_num = 0;

    cur_t c = { src, src + len, 0 };
    if (!peek(&c, '{')) return BT_MCP_E_PARSE;
    c.p++;

    char kbuf[BT_MCP_CFG_KEY_MAX];
    const char *v0, *v1; int is_str;
    const char *k0, *k1;

    while (!peek(&c, '}')) {
        if (c.p >= c.end) return BT_MCP_E_PARSE;
        if (!peek(&c, '"')) return BT_MCP_E_PARSE;
        if (skip_value(&c, &k0, &k1, &is_str)) return BT_MCP_E_PARSE;
        uint16_t need = 0;
        bt_mcp_json_decode_str(k0, k1, kbuf, sizeof kbuf - 1, &need);
        int key_known = (need < sizeof kbuf);   /* 超长键：跳过不匹配（判例 4） */
        if (key_known) kbuf[need] = 0;
        if (!peek(&c, ':')) return BT_MCP_E_PARSE;
        c.p++;
        if (skip_value(&c, &v0, &v1, &is_str)) return BT_MCP_E_PARSE;

        if (key_known && !strcmp(kbuf, "method")) {
            if (v1 <= v0 || *v0 != '"') return BT_MCP_E_PARSE;
            out->method = v0 + 1;
            out->method_len = (uint16_t)(v1 - v0 - 2);
        } else if (key_known && !strcmp(kbuf, "id")) {
            out->has_id = 1;
            if (is_str) {
                out->id_is_str = 1;
                out->id_str = v0 + 1;
                out->id_str_len = (uint16_t)(v1 - v0 - 2);
            } else {
                out->id_is_str = 0;
                out->id_num = parse_i64(v0, v1);
            }
        } else if (key_known && !strcmp(kbuf, "params")) {
            out->params = v0;
            out->params_len = (uint32_t)(v1 - v0);
        }
        if (peek(&c, ',')) c.p++;
        else if (!peek(&c, '}') && c.p < c.end) return BT_MCP_E_PARSE;
    }
    if (!out->method) return BT_MCP_E_PARSE;   /* MCP 最小请求必须有 method */
    return BT_MCP_OK;
}

/* params 片段内顶层键查找（params 可能是 {} 或裸值；只支持对象形态） */
static int find_key(const bt_mcp_json_req_t *r, const char *key,
                    const char **vout, uint32_t *vlen)
{
    if (!r->params || *r->params != '{') return BT_MCP_E_PARSE;
    cur_t c = { r->params, r->params + r->params_len, 0 };
    c.p++;
    const char *k0, *k1, *v0, *v1;
    int dummy;
    char kbuf[BT_MCP_CFG_KEY_MAX];
    while (!peek(&c, '}')) {
        if (c.p >= c.end) return BT_MCP_E_PARSE;
        if (!peek(&c, '"')) return BT_MCP_E_PARSE;
        if (skip_value(&c, &k0, &k1, &dummy)) return BT_MCP_E_PARSE;
        uint16_t need = 0;
        bt_mcp_json_decode_str(k0, k1, kbuf, sizeof kbuf - 1, &need);
        int key_known = (need < sizeof kbuf);
        if (key_known) kbuf[need] = 0;
        if (!peek(&c, ':')) return BT_MCP_E_PARSE;
        c.p++;
        if (skip_value(&c, &v0, &v1, &dummy)) return BT_MCP_E_PARSE;
        if (key_known && !strcmp(kbuf, key)) {
            *vout = v0; *vlen = (uint32_t)(v1 - v0); return BT_MCP_OK;
        }
        if (peek(&c, ',')) c.p++;
        else if (!peek(&c, '}') && c.p < c.end) return BT_MCP_E_PARSE;
    }
    return BT_MCP_E_PARSE;
}

int bt_mcp_json_get_str(const bt_mcp_json_req_t *r, const char *key,
                        const char **out, uint16_t *len)
{
    const char *v; uint32_t vl;
    int rc = find_key(r, key, &v, &vl);
    if (rc) return rc;
    if (vl < 2 || *v != '"') return BT_MCP_E_PARSE;
    *out = v + 1; *len = (uint16_t)(vl - 2);
    return BT_MCP_OK;
}

int bt_mcp_json_get_u32(const bt_mcp_json_req_t *r, const char *key, uint32_t *out)
{
    const char *v; uint32_t vl;
    int rc = find_key(r, key, &v, &vl);
    if (rc) return rc;
    if (!vl) return BT_MCP_E_PARSE;
    uint64_t acc = 0;
    for (uint32_t i = 0; i < vl; i++) {
        if (!is_digit(v[i])) return BT_MCP_E_PARSE;
        acc = acc * 10 + (unsigned)(v[i] - '0');
        if (acc > 0xFFFFFFFFull) acc = 0xFFFFFFFFull;
    }
    *out = (uint32_t)acc;
    return BT_MCP_OK;
}

int bt_mcp_json_get_raw(const bt_mcp_json_req_t *r, const char *key,
                        const char **out, uint32_t *len)
{
    return find_key(r, key, out, len);
}

int bt_mcp_json_get_sub(const bt_mcp_json_req_t *r, const char *key,
                        bt_mcp_json_req_t *sub)
{
    const char *v; uint32_t vl;
    int rc = find_key(r, key, &v, &vl);
    if (rc) return rc;
    if (vl < 2 || *v != '{') return BT_MCP_E_PARSE;
    memset(sub, 0, sizeof(*sub));
    sub->src = r->src;
    sub->params = v;
    sub->params_len = vl;
    return BT_MCP_OK;
}
