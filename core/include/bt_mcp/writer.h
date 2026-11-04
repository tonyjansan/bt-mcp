/* bt_mcp/writer.h —— 流式 JSON 写出（草案 §7 writer 辅件）。
 * 底层 put 直写 sink（环形缓冲/transport），零中间 DOM。 */
#ifndef BT_MCP_WRITER_H
#define BT_MCP_WRITER_H

#include <stdint.h>
#include <stdarg.h>

typedef struct bt_mcp_writer {
    int  (*put)(void *sink, const char *data, uint32_t len);
    void *sink;
    uint8_t need_comma;   /* writer 内部状态：对象内补逗号 */
} bt_mcp_writer_t;

/* 直接 put 一段 */
int bt_mcp_w_raw(bt_mcp_writer_t *w, const char *data, uint32_t len);
/* 写字符串字面量（自动 JSON 转义） */
int bt_mcp_w_str_lit(bt_mcp_writer_t *w, const char *s);
/* begin: {"jsonrpc":"2.0","id":<id>,"result":{  / end: }}
 * error 版本：…"error":{"code":<code>,"message":"<msg>"} } */
int bt_mcp_w_begin_result (bt_mcp_writer_t *w, int64_t id);
int bt_mcp_w_begin_error  (bt_mcp_writer_t *w, int64_t id,
                           int code, const char *message);
int bt_mcp_w_end          (bt_mcp_writer_t *w);
/* 对象内键值（自动逗号）：str=字符串值，raw=内嵌 JSON */
int bt_mcp_w_kv_str (bt_mcp_writer_t *w, const char *key, const char *val);
int bt_mcp_w_kv_raw (bt_mcp_writer_t *w, const char *key,
                      const char *raw, uint32_t len);
int bt_mcp_w_kv_u32 (bt_mcp_writer_t *w, const char *key, uint32_t v);

#endif
