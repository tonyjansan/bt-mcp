/* bt_mcp/session.h —— L4 会话层（草案 §5.1/§7）。
 * 状态机 IDLE→LINK_UP→READY；READY 前只收控制帧与 initialize；
 * 包通道信用窗口约束发送；RESUME token 绑定 bond。
 * 纯 C99、非阻塞、无 OS 依赖；时间一律由调用方传入。
 *
 * v0.3 变更：
 *  - 信用闭环：每消费一条对端消息自动回补 1 credit（饱和到 255），
 *    不再依赖应用层手工 session_ctrl(CREDIT)；
 *  - 不可靠包通道消息级可靠：发送重传槽环 + ACK 确认 + NACK 单片重传 +
 *    超时整条重传 + 接收侧窗口去重（CRC32 指纹，零额外消息缓冲）；
 *  - RESUME 防爆破：连续失败进入指数退避锁定，token 比较常数时间；
 *  - PING 关联与 RTT 测量。
 */
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

/* RESUME_REJ reason 码 */
#define BT_MCP_REJ_NO_TOKEN 1
#define BT_MCP_REJ_MISMATCH 2
#define BT_MCP_REJ_LOCKED   3

typedef struct {
    uint32_t req_timeout_ms;    /* 在途请求超时（默认 30000） */
    uint32_t resume_tout_ms;    /* RESUME_REQ 等待 RESUME_OK/REJ 超时（默认 5000） */
    uint8_t  tx_window;         /* 包通道应用层信用初值（=对端重组槽数） */
    uint8_t  role;              /* BT_MCP_ROLE_* */
    /* ---- v0.3 追加（既有 4 字段位置不变，新字段追加在尾部）---- */
    uint8_t  retry_max;         /* 每条消息重传次数上限；0=用 BT_MCP_CFG_MAX_RETRY */
    uint16_t ack_tout_ms;       /* 等 ACK 时限；0=用 BT_MCP_CFG_ACK_TOUT_MS */
} bt_mcp_session_cfg_t;

/* 上抛给 L5 的回调（session 已滤好状态/信用，L5 只管 MCP 语义） */
typedef void (*bt_mcp_mcp_sink_t)(void *user, const uint8_t *json, uint32_t len);

/* 发送重传槽：仅不可靠包通道使用。消息 ≤ slot 容量才受保护。 */
struct bt_mcp_tx_slot {
    uint8_t  valid;
    uint8_t  seq;              /* 该消息的帧头 seq（ACK/NACK 匹配键） */
    uint8_t  n;                /* 已重传次数 */
    uint16_t len;
    uint32_t t0;               /* 最近一次发出时刻（ACK 超时基准） */
    uint8_t  buf[BT_MCP_CFG_TX_RETRY_BYTES];
};

/* 接收去重窗口项：只存指纹，不存消息体（大消息也不额外吃 RAM） */
struct bt_mcp_rx_fp {
    uint8_t  valid;
    uint8_t  seq;
    uint16_t len;
    uint32_t crc;
    uint32_t t;
    uint32_t msg_no;      /* 注册时的消息计次：只对「最近几条」做去重判定 */
};

/* NVM 持久化钩子（v0.4 E2）：应用提供 save/load，core 只在状态变化时调用。 */
typedef struct {
    int (*save)(void *user, const void *buf, uint16_t len);
    int (*load)(void *user, void *buf, uint16_t len);
    void *user;
} bt_mcp_storage_t;

/* 持久化快照（结构体布局即线上格式，版本字段用于向后兼容） */
typedef struct {
    uint32_t magic;        /* 'BTM1' */
    uint16_t version;      /* 1 */
    uint16_t flags;        /* bit0=已绑定 */
    uint32_t resume_token;
} bt_mcp_session_state_t;

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
    bt_mcp_storage_t storage;     /* 可空：NVM 持久化钩子 */
    uint8_t          bond;        /* 当前链路是否已绑定 */
    uint32_t         last_rx_ms;  /* 最近一次收到有效 MCP 消息的时间 */
    /* v0.5 V9 拥塞退避：RTO 指数退避（收到 ACK 回落到基准）。
     * 默认态等于 ack_tout_ms，因此无丢包时行为与 v0.4 完全一致。 */
    uint16_t         rto_cur_ms;
    uint16_t         rto_base_ms;
    uint8_t        resume_pending;
    uint32_t       resume_t0;
    /* RESUME 防爆破 */
    uint8_t        resume_fail;      /* 连续失败次数 */
    uint8_t        resume_locked;    /* 1 = 锁定窗口内 */
    uint32_t       resume_lock_t0;   /* 锁定起始时刻 */
    uint32_t       resume_lock_ms;   /* 本次锁定时长 */
    /* 在途请求超时记账（最简形式：单槽时间戳） */
    uint32_t       req_t0;
    uint8_t        req_inflight;
    /* PING 关联 */
    uint8_t        ping_pending;
    uint8_t        ping_epoch;
    uint32_t       ping_t0;
    uint32_t       rtt_ms;           /* 最近一次测量值 */
    uint8_t        rtt_valid;
    /* ---- v0.3：不可靠包通道的发送重传槽环 ---- */
    struct bt_mcp_tx_slot txslot[BT_MCP_CFG_TX_SLOTS];
    uint8_t        txslot_next;      /* 环形替换指针 */
    /* ---- v0.3：接收去重指纹窗口 ---- */
    struct bt_mcp_rx_fp rx_fp[BT_MCP_CFG_RX_FP_SLOTS];
    uint8_t        rx_fp_next;
    uint32_t       rx_msg_no;         /* 单调消息计次（去重代次） */
    /* ---- v0.3：累计信用（CREDIT 丢失可自愈） ---- */
    uint16_t       consumed;          /* 本端已消费给对端的累计条数 */
    uint16_t       peer_consumed;     /* 对端已消费我方的累计条数（CREDIT 载荷） */
    /* 统计（port/应用可读） */
    uint32_t       stat_tx_retransmit;
    uint32_t       stat_tx_dropped;
    uint32_t       stat_rx_dup;
    uint32_t       now_ms;
    /* session_feed 期间暂存的上抛目标（单写者模型，非重入） */
    bt_mcp_mcp_sink_t feed_sink;
    void             *feed_user;
};

typedef struct bt_mcp_session bt_mcp_session_t;

int  bt_mcp_session_init(bt_mcp_session_t *s, const bt_mcp_session_cfg_t *cfg);
void bt_mcp_session_bind(bt_mcp_session_t *s, bt_mcp_framer_t *fr,
                         int (*emit)(void *io, const uint8_t *data, uint16_t len),
                         void *io);
bt_mcp_state_t bt_mcp_session_state(const bt_mcp_session_t *s);

/* port 在连接建立时调用（cap 已在 framer 绑定时注入） */
void bt_mcp_session_link_up(bt_mcp_session_t *s, uint32_t resume_token);
/* MCP 握手完成（收到 notifications/initialized）：LINK_UP → READY */
void bt_mcp_session_mark_ready(bt_mcp_session_t *s);
/* port 在断链时调用：作废在途映射；bond 丢失则 token 一并作废 */
void bt_mcp_session_link_down(bt_mcp_session_t *s, int bond_intact);

/* 下行：framer 上抛的完整消息（kind = BT_MCP_KIND_MCP/CTRL）。
 * 内部处理控制帧（信用/RESUME/ACK/NACK/状态门），MCP 消息转 mcp_sink。 */
int  bt_mcp_session_rx(bt_mcp_session_t *s, uint8_t kind, uint8_t seq,
                       const uint8_t *payload, uint16_t len,
                       bt_mcp_mcp_sink_t mcp_sink, void *user);
/* 上行：L5 要发的完整 MCP 消息；受信用窗口约束，可能 E_AGAIN */
int  bt_mcp_session_tx(bt_mcp_session_t *s, const uint8_t *json, uint32_t len);
/* 发送控制帧（RESUME/信用补给），绕过状态门的部分内部自理 */
int  bt_mcp_session_ctrl(bt_mcp_session_t *s, uint8_t type,
                         const uint8_t *args, uint8_t args_len);
/* 直接投喂原始字节：内部完成 framer 重组 → session_rx（供 port 的
 * 「收到数据」回调一行调用；等价于自己接 framer_feed + session_rx）。 */
int  bt_mcp_session_feed(bt_mcp_session_t *s, const uint8_t *in, uint16_t len,
                         bt_mcp_mcp_sink_t sink, void *user);

/* 泵：冲掉待发 NACK、到期整条重传。port 在每次 feed 之后与主循环节拍调用。 */
int  bt_mcp_session_pump(bt_mcp_session_t *s);

/* PING：发起一次带 epoch 的探针；对端自动 PONG；RTT 在后续消息/tick 里换算 */
int  bt_mcp_session_ping(bt_mcp_session_t *s, uint8_t epoch);
/* 最近一次 RTT（ms）；*valid=0 表示尚无样本 */
uint32_t bt_mcp_session_rtt(const bt_mcp_session_t *s, int *valid);

void bt_mcp_session_tick(bt_mcp_session_t *s, uint32_t now_ms);

/* ---- 持久化（v0.4 E2）---- */
void bt_mcp_session_set_storage(bt_mcp_session_t *s, const bt_mcp_storage_t *st);
/* 把 resume token / 绑定标志写入 storage；未设 storage 时是空操作（返回 0） */
int  bt_mcp_session_save_state(const bt_mcp_session_t *s);
/* 上电恢复：校验 magic/version 后回填；失败不影响会话可用性 */
int  bt_mcp_session_load_state(bt_mcp_session_t *s);
uint32_t bt_mcp_session_resume_token(const bt_mcp_session_t *s);

#endif
