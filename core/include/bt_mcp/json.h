/* bt_mcp/json.h —— 单遍 SAX 式 JSON 解析（草案 §7 mcp_server 辅件）。
 * 深度上限 6、零 DOM、零 malloc：解析时只记「顶层 method + id +
 * params 原始片段偏移」，按需用 getter 惰性取参。
 * 目标峰值 RAM = 请求原文缓冲（调用方持有）+ 96B 解析状态。 */
#ifndef BT_MCP_JSON_H
#define BT_MCP_JSON_H

#include <stdint.h>
#include <stddef.h>

typedef struct {
    const char *src;        /* 请求原文（调用方持有，解析期只读） */
    uint32_t    len;
    /* 顶层字段 */
    const char *method;     /* 指向 src 内部，不拷贝 */
    uint16_t    method_len;
    int         has_id;    /* request=1 / notification=0 */
    int64_t     id_num;    /* 数字 id；字符串 id 记原文段 */
    const char *id_str;
    uint16_t    id_str_len;
    /* params 原始片段（含外层 {} 或裸值） */
    const char *params;
    uint32_t    params_len;
} bt_mcp_json_req_t;

/* 解析 JSON-RPC 请求对象；失败返回负错误码（E_PARSE）。 */
int bt_mcp_json_parse(const char *src, uint32_t len, bt_mcp_json_req_t *out);

/* 在 params 片段内找键（仅顶层一层查找，符合 MCP params 形态）：
 * - get_str: 字符串值（指向 src 内部，不拷贝）
 * - get_u32: 数字值
 * - get_raw: 任意值的原始 JSON 片段（对象/数组/字符串均可）
 * 未找到返回 E_PARSE（调用方以返回值区分缺参）。 */
int bt_mcp_json_get_str(const bt_mcp_json_req_t *r, const char *key,
                        const char **out, uint16_t *len);
int bt_mcp_json_get_u32(const bt_mcp_json_req_t *r, const char *key, uint32_t *out);
int bt_mcp_json_get_raw(const bt_mcp_json_req_t *r, const char *key,
                        const char **out, uint32_t *len);

/* 把 params 内 key 对应的对象值嫁接成子请求（sub->params = 该对象片段）。
 * tools/call 工具参数在 params.arguments 下，用此函数下钻一层。 */
int bt_mcp_json_get_sub(const bt_mcp_json_req_t *r, const char *key,
                        bt_mcp_json_req_t *sub);

#endif
