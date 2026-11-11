/* bt_mcp/mcp_server.h —— L5：MCP server 侧（草案 §5.2/§7）。
 *
 * v0.4：
 *  - 工具元数据：title / annotations / outputSchema；
 *  - 工具结果支持多内容项：text / image / audio / embedded resource；
 *  - prompts/list、prompts/get；
 *  - resources/subscribe、unsubscribe + notifications/resources/updated；
 *  - logging/setLevel + notifications/message；
 *  - notifications/{tools,resources,prompts}/list_changed；
 *  - 授权真正实施（authz + consent 回调）；
 *  - server→client 请求（sampling / roots / elicitation）。
 */
#ifndef BT_MCP_SERVER_H
#define BT_MCP_SERVER_H

/* MCP 错误码（v0.5 V7：语义化，避免「未初始化」与「资源不存在」共用 -32002）
 * JSON-RPC 2.0 保留：-32700 解析错 / -32600 请求无效 / -32601 方法不存在 /
 * -32602 参数无效 / -32603 内部错误。
 * MCP 定义：-32002 资源不存在。本项目追加：-32001 未就绪、-32003 权限不足
 * （均落在规范保留的 server error 区 -32000..-32099）。 */
#define BT_MCP_EC_PARSE        (-32700)
#define BT_MCP_EC_INVALID_REQ  (-32600)
#define BT_MCP_EC_NO_METHOD    (-32601)
#define BT_MCP_EC_BAD_PARAMS   (-32602)
#define BT_MCP_EC_INTERNAL     (-32603)
#define BT_MCP_EC_NOT_READY    (-32001)
#define BT_MCP_EC_NO_RESOURCE  (-32002)
#define BT_MCP_EC_DENIED       (-32003)

#include <stdint.h>
#include "session.h"
#include "writer.h"
#include "json.h"

/* 工具授权级别（草案 §6.3）。v0.4 起真正生效：
 *  OPEN=直接执行；CONSENT=先问 consent_cb；RESTRICTED=一律拒绝（除非授权钩子放行）。 */
#define BT_MCP_AUTHZ_OPEN       0
#define BT_MCP_AUTHZ_CONSENT    1
#define BT_MCP_AUTHZ_RESTRICTED 2

/* 工具注解位（MCP annotations） */
#define BT_MCP_TOOL_READONLY    0x01   /* readOnlyHint */
#define BT_MCP_TOOL_DESTRUCTIVE 0x02   /* destructiveHint */
#define BT_MCP_TOOL_IDEMPOTENT  0x04   /* idempotentHint */
#define BT_MCP_TOOL_OPENWORLD   0x08   /* openWorldHint */

/* resources/read 游标语义：返回 next_cursor = BT_MCP_CURSOR_END 表示无更多 */
#define BT_MCP_CURSOR_END 0xFFFFFFFFu

/* 日志级别（logging/setLevel），与 MCP 的 syslog 级别同名 */
#define BT_MCP_LOG_DEBUG     0
#define BT_MCP_LOG_INFO      1
#define BT_MCP_LOG_NOTICE    2
#define BT_MCP_LOG_WARNING   3
#define BT_MCP_LOG_ERROR     4
#define BT_MCP_LOG_CRITICAL  5
#define BT_MCP_LOG_ALERT     6
#define BT_MCP_LOG_EMERGENCY 7

typedef struct bt_mcp_server bt_mcp_server_t;
typedef struct bt_mcp_call   bt_mcp_call_t;   /* 单次 tools/call 上下文 */

/* server→client 请求的响应回调（result_json 为 result 片段原文，失败时为 error 片段） */
typedef void (*bt_mcp_server_resp_fn)(void *user, int rc,
                                      const char *result_json,
                                      uint32_t result_len);

/* 工具实现签名：req=请求视图（arguments 已嫁接为 params）；
 * out=内容写入器（已进入 result 对象）。
 *
 * 内容写入的两条路径都合法、可混用：
 *  1) 直接 bt_mcp_w_raw(out, text, len) —— 视为一个 text 内容项（旧行为，最常用）；
 *  2) bt_mcp_w_content_*(out, ...) —— 显式追加 text/image/audio/resource 项。
 * 二者按调用顺序拼成 result.content 数组。
 *
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

/* 订阅资源变化：server 收订阅请求时调用 bind，取消订阅/断链时调用 unbind。
 * 应用在数据变化时调 bt_mcp_server_notify_resource_updated()。 */
typedef int (*bt_mcp_resource_sub_fn)(void *user, int subscribe);

/* prompt 取内容：writer 已进入 result 对象（need_comma=0），实现写
 * "description"(可选) 与 "messages":[{role,content:{...}},...]。 */
typedef int (*bt_mcp_prompt_get_fn)(const bt_mcp_json_req_t *req,
                                    bt_mcp_writer_t *w, void *user);

/* 授权决策回调：返回 0=允许，负值=拒绝（回 -32003）。 */
typedef int (*bt_mcp_consent_fn)(void *user, const char *tool_name,
                                 const bt_mcp_json_req_t *req);

typedef struct {
    const char       *name;
    const char       *desc;
    const char       *title;              /* 可空：人类可读标题 */
    const char       *input_schema_json;  /* 内嵌 JSON Schema 字符串 */
    const char       *output_schema_json; /* 可空：结构化输出 schema */
    uint8_t           annotations;        /* BT_MCP_TOOL_* 位掩码，0=不输出 */
    uint8_t           authz;
    bt_mcp_tool_fn    fn;
    void             *user;
} bt_mcp_tool_def_t;

typedef struct {
    const char *name;
    const char *desc;
    const char *title;       /* 可空 */
    const char *mime_type;   /* 可空，默认 text/plain */
    uint32_t    size;        /* 0 = 未知/不输出 */
    uint8_t     authz;
    bt_mcp_resource_read_fn read;      /* 可空 = 空内容资源 */
    bt_mcp_resource_sub_fn  sub;       /* 可空 = 不可订阅 */
    void       *user;
} bt_mcp_resource_def_t;

typedef struct {
    const char          *name;
    const char          *desc;      /* 可空 */
    const char          *title;     /* 可空 */
    const char          *arguments_schema_json;  /* 可空 */
    bt_mcp_prompt_get_fn get;
    void                *user;
} bt_mcp_prompt_def_t;

/* 设备能力表：port/应用层提供（编译期 const 数组，运行期零注册 RAM） */
typedef struct {
    const bt_mcp_tool_def_t     *tools;
    uint16_t                     tools_count;
    const bt_mcp_resource_def_t *resources;
    uint16_t                     resources_count;
    const bt_mcp_prompt_def_t   *prompts;
    uint16_t                     prompts_count;
    bt_mcp_consent_fn            consent;   /* 可空：AUTHZ_CONSENT 时必填 */
    void                        *consent_user;
} bt_mcp_server_cfg_t;

/* 二进制内容（image/audio）的 base64 编码长度上限 */
#ifndef BT_MCP_CFG_MAX_B64
#define BT_MCP_CFG_MAX_B64 4096
#endif

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
    uint8_t  items;             /* 已产出的 content 项数 */
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
    /* 资源订阅状态（存 cfg.resources 的下标） */
    uint16_t subs[BT_MCP_CFG_MAX_SUBS];
    uint8_t  nsubs;
    /* server→client 在途请求（sampling/roots/elicitation） */
    struct bt_mcp_s2c_req {
        uint8_t               in_use;
        uint32_t              id;
        uint32_t              t0;
        bt_mcp_server_resp_fn cb;
        void                 *user;
    } s2c[BT_MCP_CFG_MAX_S2C_REQ];
    uint32_t next_req_id;
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
/* 资源变化通知（仅对已订阅的资源发出；未订阅则静默返回 OK） */
int  bt_mcp_server_notify_resource_updated(bt_mcp_server_t *sv, const char *uri);
/* 列表变化通知（tools/resources/prompts 之一） */
int  bt_mcp_server_notify_list_changed(bt_mcp_server_t *sv, const char *what);
/* 设备日志（受 logging/setLevel 过滤）：level 高于设定阈值则丢弃 */
int  bt_mcp_server_log(bt_mcp_server_t *sv, uint8_t level, const char *logger,
                       const char *text, uint32_t len);

/* ---- 异步 tools/call 与进度（v0.3） ---- */
int bt_mcp_server_complete(bt_mcp_call_t *call, const char *text, int is_error);
int bt_mcp_call_progress(bt_mcp_call_t *call, uint64_t done, uint64_t total);
int bt_mcp_call_cancelled(const bt_mcp_call_t *call);

/* ---- 富内容写入（v0.4）：在工具回调里通过 out 追加内容项 ---- */
/* 显式文本项（等价于 bt_mcp_w_raw 的语义，但边界明确） */
int bt_mcp_w_content_text(bt_mcp_writer_t *w, const char *text, uint32_t len);
/* 图片：data 为**已 base64 编码**的字符串；mime 如 "image/png" */
int bt_mcp_w_content_image(bt_mcp_writer_t *w, const char *b64, uint32_t b64len,
                           const char *mime);
/* 音频：同上 */
int bt_mcp_w_content_audio(bt_mcp_writer_t *w, const char *b64, uint32_t b64len,
                           const char *mime);
/* 内嵌资源：uri 必填，text 为文本内容（mime 可空 → text/plain） */
int bt_mcp_w_content_resource(bt_mcp_writer_t *w, const char *uri,
                              const char *mime, const char *text, uint32_t len);

/* ---- server→client 请求（v0.4） ---- */
/* 发起一个 S→C 请求（如 sampling/createMessage、roots/list、elicitation 系列）。
 * params_json 为 params 对象原文；完成后客户端响应经 server_rx 匹配到回调。
 * 返回 0 已发出；E_BUSY=在途请求已满；E_STATE=会话未就绪。 */
int bt_mcp_server_request(bt_mcp_server_t *sv, const char *method,
                          const char *params_json, uint32_t params_len,
                          bt_mcp_server_resp_fn cb, void *user);
/* 在途请求数 */
int bt_mcp_server_inflight(const bt_mcp_server_t *sv);

#endif
