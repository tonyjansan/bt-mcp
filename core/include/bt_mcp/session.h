/* bt_mcp/session.h —— L4 会话层（草案 §5.1/§7）。
 * 状态机 IDLE→LINK_UP→READY；READY 前只收控制帧与 initialize；
 * 包通道信用窗口约束发送；RESUME token 绑定 bond。
 * 纯 C99、非阻塞、无 OS 依赖；时间一律由调用方传入。 */
#ifndef BT_MCP_SESSION_H
#define BT_MCP_SESSION_H

#include <stdint.h>
#include "frame.h"

typedef enum {
    BT_MCP_S_IDLE = 0,      /* 无链路：仅等 on_conn */
    BT_MCP_S_LINK_UP,       /* 链路通：允许 RESUME / initialize */
    BT_MCP_S_READY          /* 会话就绪：MCP 全量 + CREDIT/PING */
} bt_mcp_state_t;

/* 会话角色：设备端默认 server（被动接 initialize） */
#define BT_MCP_ROLE_SERVER 0
#define BT_MCP_ROLE_CLIENT 1

typedef struct {
    uint32_t req_timeout_ms;    /* 在途请求超时（默认 30000） */
    uint32_t resume_tout_ms;    /* RESUME_REQ 等待 RESUME_OK/REJ 超时（默认 5000） */
    uint8_t  tx_window;         /* 包通道应用层信用初值（=对端重组槽数） */
    uint8_t  role;              /* BT_MCP_ROLE_* */
} bt_mcp_session_cfg_t;

/* 上抛给 L5 的回调（session 已滤好状态/信用，L5 只管 MCP 语义） */
typedef void (*bt_mcp_mcp_sink_t)(void *user, const uint8_t *json, uint32_t len);

struct bt_mcp_session {
    bt_mcp_session_cfg_t cfg;
    bt_mcp_framer_t     *fr;         /* 绑定的 L3 */
    /* 发射 sink：framer_send 的 emit 直通（宿主机测试=内存 sink，port=transport->send） */
    int (*emit)(void *io, const uint8_t *data, uint16_t len);
    void             *io;

    bt_mcp_state_t state;
    uint32_t       rx_ctrl_seq;   /* 最近一次 RX 控制帧消息序号（审计） */
    /* 发送信用（包通道）：窗口满时 session_tx 返回 E_AGAIN */
    uint8_t        credits;
    /* RESUME */
    uint32_t       resume_token;  /* 本端持有的会话恢复令牌（绑 bond） */
    uint8_t        resume_pending;
    uint32_t       resume_t0;
    /* 在途请求超时记账（最简形式：单槽时间戳） */
    uint32_t       req_t0;
    uint8_t        req_inflight;
    uint32_t       now_ms;
};

typedef struct bt_mcp_session bt_mcp_session_t;

int  bt_mcp_session_init(bt_mcp_session_t *s, const bt_mcp_session_cfg_t *cfg);
void bt_mcp_session_bind(bt_mcp_session_t *s, bt_mcp_framer_t *fr,
                         int (*emit)(void *io, const uint8_t *data, uint16_t len),
                         void *io);
bt_mcp_state_t bt_mcp_session_state(const bt_mcp_session_t *s);

/* port 在连接建立时调用（cap 已在 framer 绑定时注入） */
void bt_mcp_session_link_up(bt_mcp_session_t *s, uint32_t resume_token);
/* port 在断链时调用：作废在途映射；bond 丢失则 token 一并作废 */
void bt_mcp_session_link_down(bt_mcp_session_t *s, int bond_intact);

/* 下行：framer 上抛的完整消息（kind = BT_MCP_KIND_MCP/CTRL）。
 * 内部处理控制帧（信用/RESUME/状态门），MCP 消息转 mcp_sink。 */
int  bt_mcp_session_rx(bt_mcp_session_t *s, uint8_t kind,
                       const uint8_t *payload, uint16_t len,
                       bt_mcp_mcp_sink_t mcp_sink, void *user);
/* 上行：L5 要发的完整 MCP 消息；受信用窗口约束，可能 E_AGAIN */
int  bt_mcp_session_tx(bt_mcp_session_t *s, const uint8_t *json, uint32_t len);
/* 发送控制帧（RESUME/信用补给），绕过状态门的部分内部自理 */
int  bt_mcp_session_ctrl(bt_mcp_session_t *s, uint8_t type,
                         const uint8_t *args, uint8_t args_len);

void bt_mcp_session_tick(bt_mcp_session_t *s, uint32_t now_ms);

#endif
