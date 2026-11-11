/* bt_mcp/mcp_server.h —— L5（草案 §5.2/§7）。
 *
 * v0.3：
 *  - initialize 回标准 MCP 形态（protocolVersion 协商 + serverInfo + capabilities
 *    + _meta.io.btmcp 通道能力声明）；
 *  - 补齐 resources/list、resources/read（游标分页）；
 *  - tools/call 支持异步完成（E_PENDING + server_complete）、进度通知与取消；
 *  - 字符串 request id 原样回显。
 */
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

/* resources/read 游标语义：返回 next_cursor = BT_MCP_CURSOR_END 表示无更多 */
#define BT_MCP_CURSOR_END 0xFFFFFFFFu

typedef struct bt_mcp_server bt_mcp_server_t;
typedef struct bt_mcp_call   bt_mcp_call_t;   /* 单次 tools/call 上下文 */

/* 工具实现签名：req=请求视图（arguments 已嫁接为 params）；
 * out=流式 writer（已进入 result 对象）；call=本次调用上下文（进度/取消/异步）。
 * 返回 0=同步成功；BT_MCP_E_PENDING=已受理，稍后 server_complete；其余=错误。 */
typedef int (*bt_mcp_tool_fn)(const bt_mcp_json_req_t *req,
                              bt_mcp_writer_t *out,
                              bt_mcp_call_t *call,
                              void *user);

/* 资源读取：把 [cursor, next) 窗口的文本取出来。
 * text/len 为静态或应用持有内存（server 只读，不拷贝）；*next_cursor 回填
 * 下一页起点（或 BT_MCP_CURSOR_END）。返回 0 成功。 */
typedef int (*bt_mcp_resource_read_fn)(void *user, uint32_t cursor,
                                       const char **text, uint32_t *len,
                                       uint32_t *next_cursor);

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
    const char *mime_type;   /* 可空，默认 text/plain */
    uint8_t     authz;
    bt_mcp_resource_read_fn read;   /* 可空 = 空内容资源 */
    void       *user;
} bt_mcp_resource_def_t;

/* 设备工具表：port/应用层提供（编译期 const 数组，运行期零注册 RAM） */
typedef struct {
    const bt_mcp_tool_def_t     *tools;
    uint16_t                     tools_count;
    const bt_mcp_resource_def_t *resources;
    uint16_t                     resources_count;
} bt_mcp_server_cfg_t;

/* 单次调用上下文（异步/进度/取消）：应用只在工具回调期间与
 * server_complete 之前持有它的指针；server 内部静态池管理生命周期。 */
struct bt_mcp_call {
    bt_mcp_server_t *srv;
    uint8_t  in_use;
    uint8_t  cancelled;
    uint8_t  has_progress;      /* progressToken 存在（数字或字符串） */
    uint8_t  progress_is_str;   /* 1 = 字符串 token（原样回显） */
    uint8_t  in_callback;       /* 工具回调执行中（禁止在回调内 complete） */
    uint8_t  id_is_str;
    int64_t  id_num;
    char     id_str[BT_MCP_CFG_IDSTR_MAX];
    uint16_t id_str_len;
    uint64_t progress_token;    /* 数字 token */
    char     progress_str[BT_MCP_CFG_IDSTR_MAX];
    uint16_t progress_str_len;
    uint32_t t0;
};

struct bt_mcp_server {
    bt_mcp_session_t *sess;
    bt_mcp_server_cfg_t cfg;
    const char *fw_name, *fw_ver;
    uint8_t  initialized;    /* notifications/initialized 已到 */
    uint8_t  log_level;      /* logging/setLevel 设定的最低级别 */
    uint32_t now_ms;
    struct bt_mcp_call calls[BT_MCP_CFG_MAX_PENDING];
    /* 响应拼装缓冲：每实例独立（v0.4 修：v0.3 是文件级 static，
     * 两个 server 实例会互相覆盖，桥接端同时连两台设备即触发） */
    char     rbuf[BT_MCP_CFG_MAX_MSG];
    char     ebuf[288];      /* 错误响应拼装（含 id，故比 rbuf 小得多） */
    char     nbuf[BT_MCP_CFG_NOTIFY_BUF];  /* 通知拼装（progress / notify 共用） */
};

int  bt_mcp_server_init(bt_mcp_server_t *sv, bt_mcp_session_t *s,
                        const bt_mcp_server_cfg_t *cfg,
                        const char *fw_name, const char *fw_version);
/* L4 上抛的完整 MCP 消息入口：解析→分派→响应（经 session_tx） */
int  bt_mcp_server_rx(bt_mcp_server_t *sv, const uint8_t *json, uint32_t len);
void bt_mcp_server_tick(bt_mcp_server_t *sv, uint32_t now_ms);
/* 设备主动通知（S→C）；params_json 为内嵌 params 对象原文 */
int  bt_mcp_server_notify(bt_mcp_server_t *sv, const char *method,
                          const char *params_json, uint32_t params_len);

/* ---- 异步 tools/call 与进度（v0.3） ---- */
/* 工具返回 E_PENDING 后：发结果。text 为纯文本（server 负责转义包进 content）。
 * 返回后 call 句柄失效。 */
int bt_mcp_server_complete(bt_mcp_call_t *call, const char *text, int is_error);
/* 发 notifications/progress（仅有 progressToken 时实际发出） */
int bt_mcp_call_progress(bt_mcp_call_t *call, uint64_t done, uint64_t total);
/* 该调用是否已被 notifications/cancelled 取消（工具轮询用） */
int bt_mcp_call_cancelled(const bt_mcp_call_t *call);

#endif
