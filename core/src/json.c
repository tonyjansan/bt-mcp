/* json.c —— 单遍 SAX 解析实现。
 *
 * 结构判例：
 * 1. 只认 MCP 请求形态：顶层必须是对象 {"jsonrpc":…,"method":…,…}。
 *    method/id/params 取自顶层；params 值以原文片段记录（零拷贝）。
 * 2. 深度上限 6（BT_MCP_JSON_MAX_DEPTH）：防深嵌套栈爆（嵌入式无保护栈）。
 * 3. 字符串严格 JSON 转义解码（比对时只看转义后实际字符）。
 * 4. 键名匹配在「字符串值已解码」后进行，白名单外键跳值即可，无需报错。
 */
#include "bt_mcp/json.h"
#include "bt_mcp/err.h"
#include <string.h>
#include <stdlib.h>

#define MAX_DEPTH 6

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

/* 前进一个 JSON 值（对象/数组/字符串/数字/字面量），返回值片段 [vout,vend)。
   深度超限或残缺返回负错误码。 */
static int skip_value(cur_t *c, const char **vout, const char **vend, int *is_str)
{
    skip_ws(c);
    if (c->p >= c->end) return BT_MCP_E_PARSE;
    const char *start = c->p;
    *is_str = 0;
    switch (*c->p) {
    case '"': {                        /* 字符串（含转义） */
        c->p++;
        while (c->p < c->end) {
            if (*c->p == '\\') { c->p += 2; continue; }
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
    default:                            /* 数字/true/false/null */
        for (;;) {
            if (c->p >= c->end) break;
            char ch = *c->p;
            if ((ch >= '0' && ch <= '9') || ch == '-' || ch == '+' || ch == '.'
                || ch == 'e' || ch == 'E') { c->p++; continue; }
            if ((ch >= 'a' && ch <= 'z')) { c->p++; continue; }
            break;
        }
        if (c->p == start) return BT_MCP_E_PARSE;
        *vout = start; *vend = c->p;
        return BT_MCP_OK;
    }
}

/* 解码字符串值内容（去引号、解转义到 dst）。返回内容长度。 */
static uint16_t decode_str(const char *s, const char *e, char *dst, uint16_t cap)
{
    uint16_t n = 0;
    s++; e--;                          /* 跳两侧引号 */
    while (s < e && n < cap) {
        if (*s == '\\') {
            s++;
            switch (*s) {
            case 'n': dst[n++] = '\n'; break;
            case 't': dst[n++] = '\t'; break;
            case 'r': dst[n++] = '\r'; break;
            case 'b': dst[n++] = '\b'; break;
            case 'f': dst[n++] = '\f'; break;
            case '"': case '\\': case '/': dst[n++] = *s; break;
            case 'u': {
                if (e - s < 5) return n;
                unsigned cp = 0;
                for (int i = 1; i <= 4; i++) {
                    char h = s[i];
                    cp = cp * 16 + (unsigned)(h <= '9' ? h - '0'
                                     : (h | 0x20) - 'a' + 10);
                }
                /* 仅 BMP；代理对按 UTF-8 三字节吐（MCP 场景无增补面需求） */
                if (cp < 0x80) dst[n++] = (char)cp;
                else if (cp < 0x800) {
                    dst[n++] = (char)(0xC0 | cp >> 6);
                    dst[n++] = (char)(0x80 | (cp & 0x3F));
                } else {
                    dst[n++] = (char)(0xE0 | cp >> 12);
                    dst[n++] = (char)(0x80 | ((cp >> 6) & 0x3F));
                    dst[n++] = (char)(0x80 | (cp & 0x3F));
                }
                s += 4;
                break;
            }
            default: dst[n++] = *s;
            }
            s++;
        } else {
            dst[n++] = *s++;
        }
    }
    return n;
}

int bt_mcp_json_parse(const char *src, uint32_t len, bt_mcp_json_req_t *out)
{
    if (!src || !out) return BT_MCP_E_ARGS;
    memset(out, 0, sizeof(*out));
    out->src = src; out->len = len;

    cur_t c = { src, src + len, 0 };
    if (!peek(&c, '{')) return BT_MCP_E_PARSE;
    c.p++;

    char kbuf[24];
    const char *v0, *v1; int is_str;
    const char *k0, *k1;

    while (!peek(&c, '}')) {
        if (c.p >= c.end) return BT_MCP_E_PARSE;
        if (!peek(&c, '"')) return BT_MCP_E_PARSE;
        if (skip_value(&c, &k0, &k1, &is_str)) return BT_MCP_E_PARSE;
        uint16_t klen = decode_str(k0, k1, kbuf, sizeof kbuf - 1);
        kbuf[klen] = 0;
        if (!peek(&c, ':')) return BT_MCP_E_PARSE;
        c.p++;
        if (skip_value(&c, &v0, &v1, &is_str)) return BT_MCP_E_PARSE;

        if (!strcmp(kbuf, "method")) {
            if (v1 <= v0 || *v0 != '"') return BT_MCP_E_PARSE;
            out->method = v0 + 1;
            out->method_len = (uint16_t)(v1 - v0 - 2);
        } else if (!strcmp(kbuf, "id")) {
            out->has_id = 1;
            if (is_str) { out->id_str = v0 + 1;
                          out->id_str_len = (uint16_t)(v1 - v0 - 2); }
            else out->id_num = strtoll(v0, NULL, 10);
        } else if (!strcmp(kbuf, "params")) {
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
    char kbuf[24];
    while (!peek(&c, '}')) {
        if (c.p >= c.end) return BT_MCP_E_PARSE;
        if (!peek(&c, '"')) return BT_MCP_E_PARSE;
        if (skip_value(&c, &k0, &k1, &dummy)) return BT_MCP_E_PARSE;
        uint16_t klen = decode_str(k0, k1, kbuf, sizeof kbuf - 1);
        kbuf[klen] = 0;
        if (!peek(&c, ':')) return BT_MCP_E_PARSE;
        c.p++;
        if (skip_value(&c, &v0, &v1, &dummy)) return BT_MCP_E_PARSE;
        if (!strcmp(kbuf, key)) { *vout = v0; *vlen = (uint32_t)(v1 - v0); return BT_MCP_OK; }
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
    uint64_t acc = 0; int got = 0;
    for (uint32_t i = 0; i < vl; i++)
        if (v[i] >= '0' && v[i] <= '9') { acc = acc * 10 + (unsigned)(v[i] - '0'); got = 1; }
        else if (v[i] != ' ' && v[i] != '\n' && v[i] != '\t') return BT_MCP_E_PARSE;
    if (!got) return BT_MCP_E_PARSE;
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
    sub->src = r->src;
    sub->params = v;
    sub->params_len = vl;
    sub->has_id = 0;
    sub->method = NULL;
    sub->method_len = 0;
    sub->id_str = NULL;
    sub->id_str_len = 0;
    sub->id_num = 0;
    return BT_MCP_OK;
}
