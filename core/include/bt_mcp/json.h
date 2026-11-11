/* bt_mcp/json.h —— 单遍 SAX 式 JSON 解析（草案 §7 mcp_server 辅件）。
 * 深度上限可配（默认 6）、零 DOM、零 malloc：解析时只记「顶层 method + id +
 * params 原始片段偏移」，按需用 getter 惰性取参。
 * 目标峰值 RAM = 请求原文缓冲（调用方持有）+ 128B 解析状态。
 *
 * v0.3 加固：
 *  - 严格 JSON 语法（数字/字面量按 RFC 8259 校验，不再吞非法 token）；
 *  - 数字 id 按 i64 定长解析（不依赖 NUL 结尾，无越界读）；
 *  - 字符串 id 保留原文段并由 id_is_str 标记（响应原样回显）；
 *  - 超长键名不再截断（宁可不命中，也不静默错配）；
 *  - \uXXXX 代理对按 UTF-16 正确合并为 4 字节 UTF-8。
 */
#ifndef BT_MCP_JSON_H
#define BT_MCP_JSON_H

#include <stdint.h>
#include <stddef.h>
#include "config.h"

typedef struct {
    const char *src;        /* 请求原文（调用方持有，解析期只读） */
    uint32_t    len;
    /* 顶层字段 */
    const char *method;     /* 指向 src 内部，不拷贝 */
    uint16_t    method_len;
    int         has_id;     /* request=1 / notification=0 */
    int         id_is_str;  /* 1 = 字符串 id（id_str 有效），0 = 数字 id */
    int64_t     id_num;     /* 数字 id（id_is_str=0 时有效） */
    const char *id_str;     /* 字符串 id 原文段（不含引号，可能含转义） */
    uint16_t    id_str_len;
    /* params 原始片段（含外层 {} 或裸值） */
    const char *params;
    uint32_t    params_len;
} bt_mcp_json_req_t;

/* 解析 JSON-RPC 请求对象；失败返回负错误码（E_PARSE）。
 * 失败时若已能确定 id，out 里会填好 id 字段（供 -32700 回应）。 */
int bt_mcp_json_parse(const char *src, uint32_t len, bt_mcp_json_req_t *out);

/* 在 params 片段内找键（仅顶层一层查找，符合 MCP params 形态）：
 * - get_str: 字符串值（指向 src 内部，不拷贝；转义已解码时用 get_str_dec）
 * - get_u32: 非负整数值（严格校验）
 * - get_raw: 任意值的原始 JSON 片段（对象/数组/字符串均可）
 * 未找到返回 E_PARSE（调用方以返回值区分缺参）。 */
int bt_mcp_json_get_str(const bt_mcp_json_req_t *r, const char *key,
                        const char **out, uint16_t *len);
int bt_mcp_json_get_u32(const bt_mcp_json_req_t *r, const char *key, uint32_t *out);
/* 有符号整数（JSON-RPC error.code 等为负值） */
int bt_mcp_json_get_i32(const bt_mcp_json_req_t *r, const char *key, int32_t *out);
/* 浮点数（工具参数常见 0.5 / -12.75 / 1e3）。不依赖 strtod，无 locale 影响；
 * 值域/精度按 IEEE-754 double，超出范围返回 E_PARSE。 */
int bt_mcp_json_get_f64(const bt_mcp_json_req_t *r, const char *key, double *out);
int bt_mcp_json_get_raw(const bt_mcp_json_req_t *r, const char *key,
                        const char **out, uint32_t *len);

/* 宽松形态解析：允许没有 method（响应报文）。客户端/响应侧用。
 * 与 bt_mcp_json_parse 的唯一差异就在「必须有 method」这条校验上。 */
int bt_mcp_json_parse_msg(const char *src, uint32_t len, bt_mcp_json_req_t *out);

/* 在「整个请求对象」顶层查找键（响应解析用：result / error） */
int bt_mcp_json_top_raw(const bt_mcp_json_req_t *r, const char *key,
                        const char **out, uint32_t *len);

/* 把 params 内 key 对应的对象值嫁接成子请求（sub->params = 该对象片段）。
 * tools/call 工具参数在 params.arguments 下，用此函数下钻一层。 */
int bt_mcp_json_get_sub(const bt_mcp_json_req_t *r, const char *key,
                        bt_mcp_json_req_t *sub);

/* 字符串值解码到调用方缓冲（去转义、含代理对合并）。
 * 返回写入字节数；*need 回填解码后完整长度（cap 不足时用于报错）。 */
uint16_t bt_mcp_json_decode_str(const char *s, const char *e,
                                char *dst, uint16_t cap, uint16_t *need);

#endif
