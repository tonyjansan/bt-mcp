/* bt_mcp/mcp_server.h —— L5（草案 §5.2/§7）。 */
#ifndef BT_MCP_SERVER_H
#define BT_MCP_SERVER_H

#include <stdint.h>
#include "session.h"
#include "writer.h"
#include "json.h"

/* 工具授权级别（草案 §6.3） */
#define BT_MCP_AUTHZ_OPEN       0
#define BT_MCP_AUTHZ_CONSENT    1
#define BT_MCP_AUTHZ_RESTRICTED 2

/* 进度通知回调（工具实现内可发进度；server 负责包 JSON-RPC 通知） */
typedef int (*bt_mcp_progress_t)(void *user, uint64_t progress, uint64_t total);

/* 工具实现签名：req=解析好的请求；out=流式 writer（已进入 result 对象） */
typedef int (*bt_mcp_tool_fn)(const bt_mcp_json_req_t *req,
                              bt_mcp_writer_t *out,
                              void *user);

typedef struct {
    const char       *name;
    const char       *desc;
    const char       *input_schema_json;  /* 内嵌 JSON Schema 字符串 */
    uint8_t           authz;
    bt_mcp_tool_fn    fn;
    void             *user;
} bt_mcp_tool_def_t;

typedef struct {
    const char *name;
    const char *desc;
    uint8_t     authz;
} bt_mcp_resource_def_t;

/* 设备工具表：port/应用层提供（编译期 const 数组，运行期零注册 RAM） */
typedef struct {
    const bt_mcp_tool_def_t     *tools;
    uint16_t                     tools_count;
    const bt_mcp_resource_def_t *resources;
    uint16_t                     resources_count;
} bt_mcp_server_cfg_t;

struct bt_mcp_server {
    bt_mcp_session_t *sess;
    bt_mcp_server_cfg_t cfg;
    const char *fw_name, *fw_ver;
    uint8_t  initialized;    /* notifications/initialized 已到 */
    uint32_t now_ms;
};

typedef struct bt_mcp_server bt_mcp_server_t;

int  bt_mcp_server_init(bt_mcp_server_t *sv, bt_mcp_session_t *s,
                        const bt_mcp_server_cfg_t *cfg,
                        const char *fw_name, const char *fw_version);
/* L4 上抛的完整 MCP 消息入口：解析→分派→响应（经 session_tx） */
int  bt_mcp_server_rx(bt_mcp_server_t *sv, const uint8_t *json, uint32_t len);
void bt_mcp_server_tick(bt_mcp_server_t *sv, uint32_t now_ms);
/* 设备主动通知（S→C）；params_json 为内嵌 params 对象原文 */
int  bt_mcp_server_notify(bt_mcp_server_t *sv, const char *method,
                          const char *params_json, uint32_t params_len);

#endif
