/* bt_mcp/mcp_client.h —— L5 客户端侧（Bridge/Host 用，v0.3 新增）。
 *
 * 设备端是 server（被动接 initialize/tools/call）；手机 Bridge 侧需要相反方向：
 * 主动 initialize、拉 tools/list、调 tools/call、收 notifications。
 * 与 server 共享 L3/L4，只是 L5 语义不同——同样是全静态、零 malloc、零 OS 依赖，
 * 因此 Bridge 侧可以把同一份 core 塞进 RTOS/嵌入式网关而不只是手机 App。
 *
 * 设计约束：
 *  - 在途请求表固定大小（BT_MCP_CFG_MAX_PENDING），超时可被 tick 判定；
 *  - id 由本端单调分配（u32 十进制），响应按 id 文本匹配；
 *  - 只做「请求-响应 + 通知上抛」，不做 JSON Schema 校验（那是 Host 的事）。
 */
#ifndef BT_MCP_MCP_CLIENT_H
#define BT_MCP_MCP_CLIENT_H

#include <stdint.h>
#include "session.h"
#include "writer.h"
#include "json.h"

/* 上抛给应用的响应/通知 */
typedef struct {
    uint32_t    id;             /* 匹配到的请求 id；通知为 0 */
    int         is_error;       /* 1 = result.error */
    int         is_notification;
    const char *method;         /* 通知方法名（响应为 NULL） */
    uint16_t    method_len;
    const char *result;         /* result 原文片段（零拷贝，回调期内有效） */
    uint32_t    result_len;
    int32_t     error_code;     /* is_error=1 时的 JSON-RPC code */
} bt_mcp_client_event_t;

typedef void (*bt_mcp_client_cb_t)(void *user, const bt_mcp_client_event_t *ev);

typedef struct {
    uint32_t          id;          /* 0 = 空槽 */
    uint8_t           in_use;
    uint32_t          t0;
} bt_mcp_client_req_t;

struct bt_mcp_client {
    bt_mcp_session_t  *sess;
    bt_mcp_client_cb_t cb;
    void              *cb_user;
    uint32_t           next_id;
    uint32_t           req_timeout_ms;
    uint32_t           now_ms;
    uint8_t            initialized;
    uint32_t           init_id;        /* initialize 的请求 id（0=未发起） */
    bt_mcp_client_req_t req[BT_MCP_CFG_MAX_PENDING];
    /* 请求拼装缓冲：每实例独立（v0.5 修：v0.4 是模块级 static，
     * 桥接同时连两台设备/两线程各持一个 client 时会互相覆盖） */
    char                cbuf[BT_MCP_CFG_MAX_MSG];
    char                sbuf[BT_MCP_CFG_MAX_MSG];
};

typedef struct bt_mcp_client bt_mcp_client_t;

int  bt_mcp_client_init(bt_mcp_client_t *c, bt_mcp_session_t *s,
                        bt_mcp_client_cb_t cb, void *user);
/* L4 上抛入口（port 把 session 的 mcp_sink 指到这里） */
int  bt_mcp_client_rx(bt_mcp_client_t *c, const uint8_t *json, uint32_t len);
void bt_mcp_client_tick(bt_mcp_client_t *c, uint32_t now_ms);

/* 握手：发 initialize（带本端版本/能力），响应到达后自动发 initialized 通知 */
int  bt_mcp_client_initialize(bt_mcp_client_t *c, const char *client_name,
                              const char *client_version);
/* 通用请求：params_json 可为 NULL（无 params） */
int  bt_mcp_client_request(bt_mcp_client_t *c, const char *method,
                           const char *params_json, uint32_t params_len,
                           uint32_t *out_id);
/* 便捷：列工具 / 调工具 / 读资源 */
int  bt_mcp_client_list_tools(bt_mcp_client_t *c, const char *cursor,
                              uint32_t *out_id);
int  bt_mcp_client_call_tool(bt_mcp_client_t *c, const char *name,
                             const char *args_json, uint32_t args_len,
                             uint32_t *out_id);
int  bt_mcp_client_read_resource(bt_mcp_client_t *c, const char *uri,
                                 uint32_t *out_id);
/* 在途请求数（用于背压判断） */
int  bt_mcp_client_inflight(const bt_mcp_client_t *c);

#endif
