/* session.c —— L4 实现。状态门 + 信用 + 控制帧终结 + 不可靠通道重传。
 *
 * 状态机判例（草案 §5.1 钉死）：
 * 1. READY 前只允许控制帧与 MCP initialize（其余 MCP 一律 E_STATE 丢弃，
 *    不回错误——对端还不可收 JSON 响应）。
 * 2. CREDIT/PING/ACK/NACK 只在 READY 期有意义；READY 期到达的 RESUME_* =
 *    对端状态机疑似失联，按 E_STATE 丢弃。
 * 3. 包通道信用：tx_window 为对端重组槽等价窗口，每发一条 MCP 消息扣 1，
 *    每消费对端一条 MCP 消息自动回补 1（饱和到 255）；0 时 E_AGAIN。
 *    流通道或 flow_ctrl=1 的通道不记账。
 * 4. RESUME token 由 LINK_UP 侧生成、绑定 bond；bond 丢失后 token 清零，
 *    RESUME_REQ 中的 token 不匹配 → RESUME_REJ；连续失败进入指数退避锁定
 *    （响应 3=locked），token 比较常数时间。
 * 5. v0.3 可靠子层（仅 !cap.reliable 的包通道）：
 *    - 发送：消息体进重传槽环（容量=BT_MCP_CFG_TX_RETRY_BYTES），等对端
 *      ACK(seq)；超 ack_tout 未确认就地整条重发，重发满 retry_max 次弃槽并计数。
 *    - 接收：消费后回 ACK(seq)；NACK(seq,idx) 到达 → 从槽里补发该片。
 *    - 去重：seq+len+CRC32 指纹窗口，重复消息回 ACK 但不再上抛 L5。
 */
#include "bt_mcp/session.h"
#include "bt_mcp/trace.h"
#include "bt_mcp/err.h"
#include <string.h>

/* CRC32 (IEEE 802.3, 反射多项式 0xEDB88320)：去重指纹用，4B 状态即可 */
static uint32_t crc32_of(const uint8_t *p, uint32_t n)
{
    uint32_t c = 0xFFFFFFFFu;
    while (n--) {
        c ^= *p++;
        for (int i = 0; i < 8; i++)
            c = (c >> 1) ^ (0xEDB88320u & (uint32_t)(-(int32_t)(c & 1)));
    }
    return ~c;
}

static int chan_unreliable(const bt_mcp_session_t *s)
{
    return s->fr && s->fr->cap->kind == BT_MCP_CHAN_PACKET
        && !s->fr->cap->reliable;
}

/* 包通道是否记账信用（流通道与通道自带流控的免记） */
static int chan_credit(const bt_mcp_session_t *s)
{
    return s->fr && s->fr->cap->kind == BT_MCP_CHAN_PACKET
        && !s->fr->cap->flow_ctrl;
}

static int tx_frame(bt_mcp_session_t *s, uint8_t kind,
                    const uint8_t *payload, uint32_t len)
{
    if (!s->fr || !s->emit) return BT_MCP_E_STATE;
    uint8_t seq = bt_mcp_framer_alloc_seq(s->fr);
    return bt_mcp_framer_send_seq(s->fr, kind, seq, payload, len, s->emit, s->io);
}

/* 发送控制帧：payload = type | args */
static int ctrl_frame(bt_mcp_session_t *s, uint8_t type,
                      const uint8_t *args, uint8_t args_len)
{
    uint8_t buf[BT_MCP_CTRL_MAX_PAYLOAD];
    if ((uint32_t)1 + args_len > sizeof buf) return BT_MCP_E_TOOBIG;
    buf[0] = type;
    if (args_len) memcpy(buf + 1, args, args_len);
    return tx_frame(s, BT_MCP_KIND_CTRL, buf, (uint32_t)1 + args_len);
}

static int send_ack(bt_mcp_session_t *s, uint8_t seq)
{
    return ctrl_frame(s, BT_MCP_CTRL_ACK, &seq, 1);
}

int bt_mcp_session_init(bt_mcp_session_t *s, const bt_mcp_session_cfg_t *cfg)
{
    if (!s) return BT_MCP_E_ARGS;
    memset(s, 0, sizeof(*s));
    if (cfg) s->cfg = *cfg;
    if (!s->cfg.req_timeout_ms) s->cfg.req_timeout_ms = 30000;
    if (!s->cfg.resume_tout_ms) s->cfg.resume_tout_ms = 5000;
    if (!s->cfg.tx_window) s->cfg.tx_window = BT_MCP_CFG_MAX_REASM;
    if (!s->cfg.retry_max) s->cfg.retry_max = BT_MCP_CFG_MAX_RETRY;
    if (!s->cfg.ack_tout_ms) s->cfg.ack_tout_ms = BT_MCP_CFG_ACK_TOUT_MS;
    s->rto_base_ms = s->cfg.ack_tout_ms;
    s->rto_cur_ms = s->rto_base_ms;
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

static void clear_reliability(bt_mcp_session_t *s)
{
    memset(s->txslot, 0, sizeof s->txslot);
    memset(s->rx_fp, 0, sizeof s->rx_fp);
    s->txslot_next = 0;
    s->rx_fp_next = 0;
    s->rx_msg_no = 0;
    s->consumed = 0;
    s->peer_consumed = 0;
    if (s->fr) bt_mcp_framer_reset(s->fr);
}

void bt_mcp_session_link_up(bt_mcp_session_t *s, uint32_t resume_token)
{
    bt_mcp_trace(BT_MCP_TRACE_INFO, "session:link_up", &resume_token, 4);
    s->bond = resume_token ? 1 : 0;
    s->last_rx_ms = s->now_ms;
    s->state = BT_MCP_S_LINK_UP;
    s->credits = s->cfg.tx_window;
    s->resume_pending = 0;
    s->req_inflight = 0;
    s->resume_token = resume_token;
    s->ping_pending = 0;
    clear_reliability(s);
}

void bt_mcp_session_mark_ready(bt_mcp_session_t *s)
{
    bt_mcp_trace(BT_MCP_TRACE_INFO, "session:mark_ready", NULL, 0);
    if (!s) return;
    if (s->state == BT_MCP_S_LINK_UP) s->state = BT_MCP_S_READY;
}

void bt_mcp_session_link_down(bt_mcp_session_t *s, int bond_intact)
{
    bt_mcp_trace(BT_MCP_TRACE_INFO, "session:link_down", &bond_intact, 1);
    s->state = BT_MCP_S_IDLE;
    s->req_inflight = 0;
    s->resume_pending = 0;
    s->ping_pending = 0;
    if (!bond_intact) s->resume_token = 0;   /* 判例 4：bond 丢→token 作废 */
    clear_reliability(s);
}

/* ---- 重传槽 ---- */

static struct bt_mcp_tx_slot *slot_by_seq(bt_mcp_session_t *s, uint8_t seq)
{
    for (int i = 0; i < BT_MCP_CFG_TX_SLOTS; i++)
        if (s->txslot[i].valid && s->txslot[i].seq == seq) return &s->txslot[i];
    return NULL;
}

static struct bt_mcp_tx_slot *slot_claim(bt_mcp_session_t *s)
{
    /* 优先空槽，否则环形替换最老的一个 */
    for (int i = 0; i < BT_MCP_CFG_TX_SLOTS; i++)
        if (!s->txslot[i].valid) return &s->txslot[i];
    struct bt_mcp_tx_slot *sl = &s->txslot[s->txslot_next % BT_MCP_CFG_TX_SLOTS];
    s->txslot_next++;
    return sl;
}

/* ---- 去重指纹 ---- */

/* 返回 1 = 命中重复（窗口内同 seq/len/CRC） */
static int rx_dup_check(bt_mcp_session_t *s, uint8_t seq,
                        const uint8_t *pl, uint16_t len)
{
    uint32_t crc = crc32_of(pl, len);
    for (int i = 0; i < BT_MCP_CFG_RX_FP_SLOTS; i++) {
        struct bt_mcp_rx_fp *fp = &s->rx_fp[i];
        if (!fp->valid) continue;
        if (fp->seq != seq || fp->len != len || fp->crc != crc) continue;
        if (s->now_ms - fp->t > BT_MCP_CFG_DEDUPE_MS) continue;  /* 窗口外不复用 */
        /* 代次护栏：seq 是 u8，256 条后会同号。只有「最近 8 条」内的同号同内容
         * 才算重传；否则视为合法的重复内容消息（例如重复的传感器通知）。 */
        if (s->rx_msg_no - fp->msg_no > 8) continue;
        return 1;
    }
    struct bt_mcp_rx_fp *fp = &s->rx_fp[s->rx_fp_next % BT_MCP_CFG_RX_FP_SLOTS];
    s->rx_fp_next++;
    fp->valid = 1;
    fp->seq = seq;
    fp->len = len;
    fp->crc = crc;
    fp->t = s->now_ms;
    fp->msg_no = s->rx_msg_no++;
    return 0;
}

/* ---- 控制帧接收终结（不进 L5） ---- */

/* 常数时间比较（防时序侧信道） */
static int ct_eq32(uint32_t a, uint32_t b)
{
    uint32_t x = a ^ b;
    x |= x >> 16; x |= x >> 8; x |= x >> 4; x |= x >> 2; x |= x >> 1;
    return (int)(x & 1u) == 0;
}

static int resume_locked_now(bt_mcp_session_t *s)
{
    if (!s->resume_locked) return 0;
    if (s->now_ms - s->resume_lock_t0 >= s->resume_lock_ms) {
        s->resume_locked = 0;
        s->resume_fail = 0;
        s->resume_lock_ms = 0;
        return 0;
    }
    return 1;
}

static int resume_fail_note(bt_mcp_session_t *s)
{
    if (s->resume_fail < 0xFF) s->resume_fail++;
    if (s->resume_fail >= BT_MCP_CFG_RESUME_MAX_FAIL) {
        uint32_t backoff = BT_MCP_CFG_RESUME_LOCK_MS;
        for (uint8_t i = BT_MCP_CFG_RESUME_MAX_FAIL; i < s->resume_fail
             && backoff < (BT_MCP_CFG_RESUME_LOCK_MS * 8u); i++)
            backoff <<= 1;
        s->resume_locked = 1;
        s->resume_lock_t0 = s->now_ms;
        s->resume_lock_ms = backoff;
    }
    return 0;
}

static int rx_ctrl(bt_mcp_session_t *s, const uint8_t *pl, uint16_t len)
{
    if (len < 1) return BT_MCP_E_SYNC;
    uint8_t type = pl[0];
    switch (type) {
    case BT_MCP_CTRL_CREDIT: {
        /* 累计回补：载荷 = 对端「已消费总条数」（u16，回绕安全）。
         * 用累计值而非 delta，单个 CREDIT 丢失可被下一个自愈（v0.3 修）。
         * 握手期（LINK_UP）也允许——initialize 响应同样占信用。 */
        if (len < 3) return BT_MCP_E_SYNC;
        uint16_t total = (uint16_t)(pl[1] | (pl[2] << 8));
        uint16_t delta = (uint16_t)(total - s->peer_consumed);
        s->peer_consumed = total;
        uint32_t cur = (uint32_t)s->credits + delta;
        s->credits = (uint8_t)(cur > 255u ? 255u : cur);
        return BT_MCP_OK;
    }
    case BT_MCP_CTRL_PING: {
        /* 对端探针 → 回 PONG；本端探针的 PONG 回来 → 结算 RTT */
        uint8_t epoch = len > 1 ? pl[1] : 0;
        if (s->ping_pending && epoch == s->ping_epoch) {
            s->rtt_ms = s->now_ms - s->ping_t0;
            s->rtt_valid = 1;
            s->ping_pending = 0;
            return BT_MCP_OK;
        }
        uint8_t pong[2] = { BT_MCP_CTRL_PING, epoch };
        return tx_frame(s, BT_MCP_KIND_CTRL, pong, 2);
    }
    case BT_MCP_CTRL_ACK: {
        if (len < 2) return BT_MCP_E_SYNC;
        struct bt_mcp_tx_slot *sl = slot_by_seq(s, pl[1]);
        if (sl) sl->valid = 0;                   /* 已被对端消费 → 释放重传槽 */
        /* V9：任何合法 ACK 都证明链路可用 → RTO 回落基准（即使槽已放弃） */
        s->rto_cur_ms = s->rto_base_ms;
        return BT_MCP_OK;
    }
    case BT_MCP_CTRL_NACK: {
        if (len < 3) return BT_MCP_E_SYNC;
        struct bt_mcp_tx_slot *sl = slot_by_seq(s, pl[1]);
        if (!sl || !s->fr || !s->emit) return BT_MCP_OK;   /* 无槽可补：交给对端超时 */
        int rc = bt_mcp_framer_send_idx(s->fr, BT_MCP_KIND_MCP, sl->seq, pl[2],
                                        sl->buf, sl->len, s->emit, s->io);
        if (!rc) s->stat_tx_retransmit++;
        return rc;
    }
    case BT_MCP_CTRL_RESUME_REQ: {               /* server 侧裁决 */
        if (s->state != BT_MCP_S_LINK_UP) return BT_MCP_E_STATE;
        if (resume_locked_now(s)) {
            uint8_t rej[2] = { BT_MCP_CTRL_RESUME_REJ, BT_MCP_REJ_LOCKED };
            return tx_frame(s, BT_MCP_KIND_CTRL, rej, 2);
        }
        if (len < 5 || !s->resume_token) {
            resume_fail_note(s);
            uint8_t rej[2] = { BT_MCP_CTRL_RESUME_REJ, BT_MCP_REJ_NO_TOKEN };
            return tx_frame(s, BT_MCP_KIND_CTRL, rej, 2);
        }
        uint32_t tok = (uint32_t)pl[1] | (uint32_t)pl[2] << 8
                    | (uint32_t)pl[3] << 16 | (uint32_t)pl[4] << 24;
        if (!ct_eq32(tok, s->resume_token)) {
            resume_fail_note(s);
            uint8_t rej[2] = { BT_MCP_CTRL_RESUME_REJ, BT_MCP_REJ_MISMATCH };
            return tx_frame(s, BT_MCP_KIND_CTRL, rej, 2);
        }
        uint8_t ok[5];
        ok[0] = BT_MCP_CTRL_RESUME_OK;
        ok[1] = (uint8_t)tok; ok[2] = (uint8_t)(tok >> 8);
        ok[3] = (uint8_t)(tok >> 16); ok[4] = (uint8_t)(tok >> 24);
        s->resume_fail = 0;                      /* 成功即清零爆破计数 */
        s->resume_locked = 0;
        s->state = BT_MCP_S_READY;               /* 恢复成功直达 READY */
        return tx_frame(s, BT_MCP_KIND_CTRL, ok, 5);
    }
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

int bt_mcp_session_rx(bt_mcp_session_t *s, uint8_t kind, uint8_t seq,
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
            && !(len >= 11 && !memcmp(payload, "\"initialize", 11))
            && memcmp(payload, "{\"jsonrpc", 9))) {
            /* 粗门：L5 会做 JSON 级判定，这里只挡明显越权流量 */
            return BT_MCP_E_STATE;
        }
    }

    /* 判例 5：不可靠包通道做窗口去重（重传到的重复消息） */
    if (chan_unreliable(s)) {
        int dup = rx_dup_check(s, seq, payload, len);
        if (dup) {
            s->stat_rx_dup++;
            bt_mcp_trace(BT_MCP_TRACE_DBG, "rx:dup_dropped", payload,
                         (uint16_t)(len > 16 ? 16 : len));
            send_ack(s, seq);                    /* 重 ACK，不再上抛 */
            return BT_MCP_OK;
        }
    }

    s->last_rx_ms = s->now_ms;
    if (mcp_sink) mcp_sink(user, payload, len);
    /* 判例 3/5：消费即回补信用、消费即 ACK。LINK_UP 期同样要做——
     * 握手消息（initialize 响应/initialized）也占对端窗口，漏补会永久少一格。 */
    if (chan_credit(s)) {
        s->consumed++;
        uint8_t cum[2] = { (uint8_t)(s->consumed & 0xFF),
                           (uint8_t)(s->consumed >> 8) };
        ctrl_frame(s, BT_MCP_CTRL_CREDIT, cum, 2);
    }
    if (chan_unreliable(s)) send_ack(s, seq);
    return BT_MCP_OK;
}

int bt_mcp_session_tx(bt_mcp_session_t *s, const uint8_t *json, uint32_t len)
{
    if (!s || (!json && len)) return BT_MCP_E_ARGS;
    if (s->state != BT_MCP_S_READY && s->state != BT_MCP_S_LINK_UP)
        return BT_MCP_E_STATE;
    if (chan_credit(s) && s->credits == 0) return BT_MCP_E_AGAIN;   /* 判例 3 */

    uint8_t seq = bt_mcp_framer_alloc_seq(s->fr);
    int rc = bt_mcp_framer_send_seq(s->fr, BT_MCP_KIND_MCP, seq, json, len,
                                    s->emit, s->io);
    if (rc) return rc;               /* 没上线就不扣信用（v0.3 修信用泄漏） */
    if (chan_credit(s)) s->credits--;

    /* 判例 5：不可靠包通道登记重传槽（放不下就放弃保护，仍照常发送） */
    if (chan_unreliable(s) && len && len <= BT_MCP_CFG_TX_RETRY_BYTES) {
        struct bt_mcp_tx_slot *sl = slot_claim(s);
        memcpy(sl->buf, json, len);
        sl->len = (uint16_t)len;
        sl->seq = seq;
        sl->n = 0;
        sl->t0 = s->now_ms;
        sl->valid = 1;
    }
    return BT_MCP_OK;
}

int bt_mcp_session_ctrl(bt_mcp_session_t *s, uint8_t type,
                        const uint8_t *args, uint8_t args_len)
{
    if (!s || (args_len && !args)) return BT_MCP_E_ARGS;
    if (s->state == BT_MCP_S_IDLE) return BT_MCP_E_STATE;
    if (type == BT_MCP_CTRL_RESUME_REQ) {
        if (s->state != BT_MCP_S_LINK_UP) return BT_MCP_E_STATE;
        if (resume_locked_now(s)) return BT_MCP_E_LOCKED;
        s->resume_pending = 1;
        s->resume_t0 = s->now_ms;
    }
    return ctrl_frame(s, type, args, args_len);
}

int bt_mcp_session_ping(bt_mcp_session_t *s, uint8_t epoch)
{
    if (!s) return BT_MCP_E_ARGS;
    if (s->state != BT_MCP_S_READY) return BT_MCP_E_STATE;
    int rc = ctrl_frame(s, BT_MCP_CTRL_PING, &epoch, 1);
    if (!rc) {
        s->ping_pending = 1;
        s->ping_epoch = epoch;
        s->ping_t0 = s->now_ms;
    }
    return rc;
}

uint32_t bt_mcp_session_rtt(const bt_mcp_session_t *s, int *valid)
{
    if (valid) *valid = s ? s->rtt_valid : 0;
    return s ? s->rtt_ms : 0;
}

/* framer 上抛 → session_rx（session_feed 的内部 sink） */
static void feed_trampoline(void *user, uint8_t kind, uint8_t seq,
                            const uint8_t *payload, uint16_t len)
{
    bt_mcp_session_t *s = user;
    (void)bt_mcp_session_rx(s, kind, seq, payload, len, s->feed_sink, s->feed_user);
}

int bt_mcp_session_feed(bt_mcp_session_t *s, const uint8_t *in, uint16_t len,
                        bt_mcp_mcp_sink_t sink, void *user)
{
    if (!s || !s->fr) return BT_MCP_E_ARGS;
    s->feed_sink = sink;
    s->feed_user = user;
    int rc = bt_mcp_framer_feed(s->fr, in, len, feed_trampoline, s);
    s->feed_sink = NULL;
    s->feed_user = NULL;
    return rc;
}

int bt_mcp_session_pump(bt_mcp_session_t *s)
{
    if (!s || !s->fr) return BT_MCP_E_ARGS;
    int rc = BT_MCP_OK;
    uint8_t seq, idx;
    /* 1) 缺片 → NACK 请对端补发（仅不可靠包通道） */
    if (chan_unreliable(s) && bt_mcp_framer_take_nack(s->fr, &seq, &idx)) {
        uint8_t args[2] = { seq, idx };
        int r = ctrl_frame(s, BT_MCP_CTRL_NACK, args, 2);
        if (r) rc = r;
    }
    /* 2) ACK 超时 → 整条重传（用原 seq，接收侧按指纹去重） */
    for (int i = 0; i < BT_MCP_CFG_TX_SLOTS; i++) {
        struct bt_mcp_tx_slot *sl = &s->txslot[i];
        if (!sl->valid) continue;
        if (s->now_ms - sl->t0 < s->rto_cur_ms) continue;
        if (sl->n >= s->cfg.retry_max) {
            bt_mcp_trace(BT_MCP_TRACE_WARN, "tx:drop_after_retries",
                         &sl->seq, 1);
            sl->valid = 0;
            s->stat_tx_dropped++;
            /* 该消息确认永久丢失：退还发送时扣掉的信用，否则窗口单调泄漏 */
            if (chan_credit(s) && s->credits < 255) s->credits++;
            continue;
        }
        int r = bt_mcp_framer_send_seq(s->fr, BT_MCP_KIND_MCP, sl->seq,
                                       sl->buf, sl->len, s->emit, s->io);
        sl->n++;
        sl->t0 = s->now_ms;
        if (r) { rc = r; continue; }
        s->stat_tx_retransmit++;
        /* V9：指数退避——连续超时说明链路拥塞，拉长等待再试，避免重传风暴 */
        {
            uint32_t nx = (uint32_t)s->rto_cur_ms * 2u;
            if (nx > BT_MCP_CFG_RTO_MAX_MS) nx = BT_MCP_CFG_RTO_MAX_MS;
            s->rto_cur_ms = (uint16_t)nx;
        }
        bt_mcp_trace(BT_MCP_TRACE_WARN, "tx:retransmit", &sl->seq, 1);
    }
    return rc;
}

#define BT_MCP_STATE_MAGIC 0x31544D42u   /* 'BTM1' */

void bt_mcp_session_set_storage(bt_mcp_session_t *s, const bt_mcp_storage_t *st)
{
    if (!s) return;
    if (st) s->storage = *st;
    else memset(&s->storage, 0, sizeof s->storage);
}

int bt_mcp_session_save_state(const bt_mcp_session_t *s)
{
    if (!s || !s->storage.save) return BT_MCP_OK;   /* 无钩子：静默成功 */
    bt_mcp_session_state_t st;
    memset(&st, 0, sizeof st);
    st.magic = BT_MCP_STATE_MAGIC;
    st.version = 1;
    st.flags = s->bond ? 1 : 0;
    st.resume_token = s->resume_token;
    return s->storage.save(s->storage.user, &st, (uint16_t)sizeof st);
}

int bt_mcp_session_load_state(bt_mcp_session_t *s)
{
    if (!s || !s->storage.load) return BT_MCP_E_ARGS;
    bt_mcp_session_state_t st;
    memset(&st, 0, sizeof st);
    int rc = s->storage.load(s->storage.user, &st, (uint16_t)sizeof st);
    if (rc) return rc;
    if (st.magic != BT_MCP_STATE_MAGIC || st.version != 1) return BT_MCP_E_PARSE;
    s->resume_token = st.resume_token;
    s->bond = (uint8_t)(st.flags & 1);
    return BT_MCP_OK;
}

uint32_t bt_mcp_session_resume_token(const bt_mcp_session_t *s)
{
    return s ? s->resume_token : 0;
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
    bt_mcp_session_pump(s);                     /* 超时重传 / 待发 NACK */
}
