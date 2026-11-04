/* session.c —— L4 实现。状态门 + 信用 + 控制帧终结。
 *
 * 状态机判例（草案 §5.1 钉死）：
 * 1. READY 前只允许控制帧与 MCP initialize（其余 MCP 一律 E_STATE 丢弃，
 *    不回错误——对端还不可收 JSON 响应）。
 * 2. CREDIT/PING 只在 READY 期有意义；READY 期到达的 RESUME_* = 对端状态机
 *    疑似失联，按 E_STATE 丢弃。
 * 3. 包通道信用：tx_window 为对端重组槽等价窗口，每发一条 MCP 消息扣 1，
 *    CREDIT 增量回补；0 时 E_AGAIN。流通道不记账（framer 单帧直发）。
 * 4. RESUME token 由 LINK_UP 侧生成、绑定 bond；bond 丢失后 token 清零，
 *    RESUME_REQ 中的 token 不匹配 → RESUME_REJ，双方回 LINK_UP。
 */
#include "bt_mcp/session.h"
#include "bt_mcp/err.h"
#include <string.h>

static int tx_frame(bt_mcp_session_t *s, uint8_t kind,
                    const uint8_t *payload, uint32_t len)
{
    if (!s->fr || !s->emit) return BT_MCP_E_STATE;
    return bt_mcp_framer_send(s->fr, kind, payload, len, s->emit, s->io);
}

int bt_mcp_session_init(bt_mcp_session_t *s, const bt_mcp_session_cfg_t *cfg)
{
    if (!s) return BT_MCP_E_ARGS;
    memset(s, 0, sizeof(*s));
    s->cfg = *cfg;
    if (!s->cfg.req_timeout_ms) s->cfg.req_timeout_ms = 30000;
    if (!s->cfg.resume_tout_ms) s->cfg.resume_tout_ms = 5000;
    if (!s->cfg.tx_window) s->cfg.tx_window = BT_MCP_CFG_MAX_REASM;
    return BT_MCP_OK;
}

void bt_mcp_session_bind(bt_mcp_session_t *s, bt_mcp_framer_t *fr,
                         int (*emit)(void *io, const uint8_t *data, uint16_t len),
                         void *io)
{
    s->fr = fr;
    s->emit = emit;
    s->io = io;
}

bt_mcp_state_t bt_mcp_session_state(const bt_mcp_session_t *s)
{
    return s->state;
}

void bt_mcp_session_link_up(bt_mcp_session_t *s, uint32_t resume_token)
{
    s->state = BT_MCP_S_LINK_UP;
    s->credits = s->cfg.tx_window;
    s->resume_pending = 0;
    s->req_inflight = 0;
    s->resume_token = resume_token;
}

void bt_mcp_session_link_down(bt_mcp_session_t *s, int bond_intact)
{
    s->state = BT_MCP_S_IDLE;
    s->req_inflight = 0;
    s->resume_pending = 0;
    if (!bond_intact) s->resume_token = 0;   /* 判例 4：bond 丢→token 作废 */
}

/* 控制帧接收终结（不进 L5） */
static int rx_ctrl(bt_mcp_session_t *s, const uint8_t *pl, uint16_t len)
{
    if (len < 1) return BT_MCP_E_SYNC;
    uint8_t type = pl[0];
    switch (type) {
    case BT_MCP_CTRL_CREDIT:                     /* u16 credits 增量回补 */
        if (s->state != BT_MCP_S_READY) return BT_MCP_E_STATE;
        if (len < 3) return BT_MCP_E_SYNC;
        s->credits = (uint8_t)(s->credits
                     + (uint16_t)(pl[1] | (pl[2] << 8)));
        return BT_MCP_OK;
    case BT_MCP_CTRL_PING: {
        uint8_t pong[2] = { BT_MCP_CTRL_PING, len > 1 ? pl[1] : 0 };
        return tx_frame(s, BT_MCP_KIND_CTRL, pong, 2);
    }
    case BT_MCP_CTRL_RESUME_REQ:                 /* server 侧裁决 */
        if (s->state != BT_MCP_S_LINK_UP) return BT_MCP_E_STATE;
        if (len < 5 || !s->resume_token) {
            uint8_t rej[2] = { BT_MCP_CTRL_RESUME_REJ, 1 };
            return tx_frame(s, BT_MCP_KIND_CTRL, rej, 2);
        }
        uint32_t tok = (uint32_t)pl[1] | (uint32_t)pl[2] << 8
                    | (uint32_t)pl[3] << 16 | (uint32_t)pl[4] << 24;
        if (tok != s->resume_token) {
            uint8_t rej[2] = { BT_MCP_CTRL_RESUME_REJ, 2 };
            return tx_frame(s, BT_MCP_KIND_CTRL, rej, 2);
        }
        uint8_t ok[5];
        ok[0] = BT_MCP_CTRL_RESUME_OK;
        ok[1] = (uint8_t)tok; ok[2] = (uint8_t)(tok >> 8);
        ok[3] = (uint8_t)(tok >> 16); ok[4] = (uint8_t)(tok >> 24);
        s->state = BT_MCP_S_READY;               /* 恢复成功直达 READY */
        return tx_frame(s, BT_MCP_KIND_CTRL, ok, 5);
    case BT_MCP_CTRL_RESUME_OK:                  /* client 侧 */
        if (s->state != BT_MCP_S_LINK_UP || !s->resume_pending)
            return BT_MCP_E_STATE;
        s->resume_pending = 0;
        s->state = BT_MCP_S_READY;
        return BT_MCP_OK;
    case BT_MCP_CTRL_RESUME_REJ:
        s->resume_pending = 0;
        s->state = BT_MCP_S_LINK_UP;             /* 退回完整握手 */
        return BT_MCP_OK;
    default:
        return BT_MCP_E_SYNC;                    /* 未知控制帧类型 */
    }
}

int bt_mcp_session_rx(bt_mcp_session_t *s, uint8_t kind,
                     const uint8_t *payload, uint16_t len,
                     bt_mcp_mcp_sink_t mcp_sink, void *user)
{
    if (!s || !payload) return BT_MCP_E_ARGS;
    if (s->state == BT_MCP_S_IDLE) return BT_MCP_E_STATE;

    if (kind == BT_MCP_KIND_CTRL)
        return rx_ctrl(s, payload, len);

    /* MCP 消息：READY 前只放行 initialize（判例 1） */
    if (s->state == BT_MCP_S_LINK_UP) {
        if (len < 9 || (memcmp(payload, "initialize", 9)
            && memcmp(payload, "\"initialize", 11)
            && memcmp(payload, "{\"jsonrpc", 9))) {
            /* 粗门：L5 会做 JSON 级判定，这里只挡明显越权流量 */
            return BT_MCP_E_STATE;
        }
    }
    if (mcp_sink) mcp_sink(user, payload, len);
    return BT_MCP_OK;
}

int bt_mcp_session_tx(bt_mcp_session_t *s, const uint8_t *json, uint32_t len)
{
    if (!s || (!json && len)) return BT_MCP_E_ARGS;
    /* LINK_UP 放行：initialize 响应是握手期唯一合法的 MCP 下行（判例 1），
       其余越权流量由 rx 侧状态门拦截，tx 侧不再重复挡。 */
    if (s->state != BT_MCP_S_READY && s->state != BT_MCP_S_LINK_UP)
        return BT_MCP_E_STATE;
    if (s->fr->cap->kind == BT_MCP_CHAN_PACKET) {
        if (s->credits == 0) return BT_MCP_E_AGAIN;   /* 判例 3 */
        s->credits--;
    }
    return tx_frame(s, BT_MCP_KIND_MCP, json, len);
}

int bt_mcp_session_ctrl(bt_mcp_session_t *s, uint8_t type,
                        const uint8_t *args, uint8_t args_len)
{
    if (!s || (args_len && !args)) return BT_MCP_E_ARGS;
    if (s->state == BT_MCP_S_IDLE) return BT_MCP_E_STATE;
    if (type == BT_MCP_CTRL_RESUME_REQ) {
        if (s->state != BT_MCP_S_LINK_UP) return BT_MCP_E_STATE;
        s->resume_pending = 1;
        s->resume_t0 = s->now_ms;
    }
    /* args_len ≤255 但控制帧恒单片 ≤255B：type(1)+args 上限 255，用栈上
       满额缓冲防 memcpy 越界（M3 差分第一轮抓到的栈溢出根因）。 */
    uint8_t buf[255];
    if ((uint32_t)1 + args_len > 255) return BT_MCP_E_TOOBIG;
    buf[0] = type;
    if (args_len) memcpy(buf + 1, args, args_len);
    return tx_frame(s, BT_MCP_KIND_CTRL, buf, (uint32_t)(1 + args_len));
}

void bt_mcp_session_tick(bt_mcp_session_t *s, uint32_t now_ms)
{
    if (!s) return;
    s->now_ms = now_ms;
    if (s->state == BT_MCP_S_LINK_UP && s->resume_pending
        && now_ms - s->resume_t0 > s->cfg.resume_tout_ms) {
        s->resume_pending = 0;                  /* 超时：退回完整握手 */
    }
    if (s->state == BT_MCP_S_READY && s->req_inflight
        && now_ms - s->req_t0 > s->cfg.req_timeout_ms) {
        s->req_inflight = 0;                    /* 在途请求超时记账（L5 可查询计数） */
    }
    if (s->fr) bt_mcp_framer_tick(s->fr, now_ms);
}
