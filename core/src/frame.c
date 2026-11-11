/* frame.c —— L3 实现。纯 C99，无 OS 调用，非阻塞。
 *
 * 重组语义判例（钉死）：
 * 1. 帧头 seq 是「消息键」：发送方每条消息自增一次 seq，同消息全部分片共享。
 *    包通道按 seq 匹配重组槽，使多条消息分片可在通道上交错。
 * 2. 包通道单特征连接内有序（GATT/CoC 语义），故分片按 idx 严格递增顺序追加：
 *    - idx < next_idx：迟到重复片，直接忽略（不报错、不污染槽位）；
 *    - idx > next_idx：链路上丢片。槽位【保持存活】并登记 NACK(seq, next_idx)，
 *      等对端单片重传补齐（v0.3：不再弃整消息，否则大消息在丢包链路上不可用）。
 *    同一槽连续 gap_wait 次仍补不齐 → 放弃该槽（防对端永久失联占死缓冲）。
 * 3. LAST 分片到达且 idx 连续 → 立即上抛完整消息（携带 seq）。
 * 4. FCS16 仅在通道无链路级校验时开启（BT_MCP_CFG_FCS16），覆盖 帧头+载荷。
 */
#include "bt_mcp/frame.h"
#include "bt_mcp/err.h"
#include <string.h>

#define BT_MCP_GAP_WAIT_MAX 4   /* 同一槽位最多容忍 4 次缺片，超限弃槽 */

static uint16_t chunk_cap(const bt_mcp_chan_cap_t *cap)
{
    /* 单片载荷上限：包通道 = min(mtu,261)-6（≤255）；流通道 = 整消息单帧直发，
     * 上限即 MAX_MSG（u16 len 可承载）。返回 0 = 通道不可用。 */
    if (cap->kind == BT_MCP_CHAN_STREAM) return BT_MCP_CFG_MAX_MSG;
    uint16_t m = cap->mtu;
    if (m > 261) m = 261;
    if (m <= BT_MCP_HDR_LEN) return 0;
    m -= BT_MCP_HDR_LEN;
#if BT_MCP_CFG_FCS16
    if (m <= 2) return 0;
    m -= 2;
#endif
    return m;
}

int bt_mcp_framer_init(bt_mcp_framer_t *f, const bt_mcp_frame_cfg_t *cfg,
                       const bt_mcp_chan_cap_t *cap)
{
    if (!f || !cap) return BT_MCP_E_ARGS;
    if (chunk_cap(cap) == 0) return BT_MCP_E_ARGS;
    memset(f, 0, sizeof(*f));
    f->cap = cap;
    f->cfg = cfg ? *cfg
                 : (bt_mcp_frame_cfg_t){ .max_reasm = BT_MCP_CFG_MAX_REASM,
                                         .frag_tout_ms = BT_MCP_CFG_FRAG_TOUT_MS };
    if (f->cfg.max_reasm == 0 || f->cfg.max_reasm > BT_MCP_CFG_MAX_REASM)
        f->cfg.max_reasm = BT_MCP_CFG_MAX_REASM;
    if (f->cfg.frag_tout_ms == 0) f->cfg.frag_tout_ms = BT_MCP_CFG_FRAG_TOUT_MS;
    return BT_MCP_OK;
}

void bt_mcp_framer_reset(bt_mcp_framer_t *f)
{
    if (!f) return;
    memset(f->reasm, 0, sizeof(f->reasm));
    f->s_hdr_got = 0;
    f->s_len_got = 0;
    f->s_active = 0;
    f->nack_pending = 0;
}

#if BT_MCP_CFG_FCS16
/* CRC16-CCITT (0x1021, init 0xFFFF)，可续算：state 即当前 crc */
static uint16_t crc_cont(const uint8_t *p, uint32_t n, uint16_t crc)
{
    while (n--) {
        crc ^= (uint16_t)(*p++) << 8;
        for (int i = 0; i < 8; i++)
            crc = (crc & 0x8000) ? (uint16_t)((crc << 1) ^ 0x1021)
                                 : (uint16_t)(crc << 1);
    }
    return crc;
}
#endif

/* ---- 发送侧 ---- */

/* 发一片：hdr + [fcs] + payload。idx/总数由调用方给定。 */
static int emit_chunk(bt_mcp_framer_t *f, uint8_t kind, uint8_t seq,
                      uint8_t idx, uint8_t nfrag, uint16_t chunk,
                      const uint8_t *payload, uint32_t len,
                      int (*emit)(void *, const uint8_t *, uint16_t), void *io)
{
    uint32_t done = (uint32_t)idx * chunk;
    if (done > len) done = len;
    uint16_t plen = (uint16_t)(len - done < chunk ? len - done : chunk);
    const uint8_t flags = (uint8_t)((kind == BT_MCP_KIND_CTRL ? BT_MCP_FLAG_CTRL : 0)
                          | (nfrag > 1 ? BT_MCP_FLAG_CHUNK : 0)
                          | (idx == nfrag - 1 ? BT_MCP_FLAG_LAST : 0));
    uint8_t hdr[BT_MCP_HDR_LEN];
    hdr[0] = BT_MCP_MAGIC;
    hdr[1] = flags;
    hdr[2] = seq;
    hdr[3] = idx;
    hdr[4] = (uint8_t)(plen & 0xFF);
    hdr[5] = (uint8_t)(plen >> 8);

#if BT_MCP_CFG_FCS16
    uint8_t fcs[2];
    uint16_t crc = crc_cont(hdr, BT_MCP_HDR_LEN, 0xFFFF);
    crc = crc_cont(payload + done, plen, crc);
    fcs[0] = (uint8_t)(crc & 0xFF);
    fcs[1] = (uint8_t)(crc >> 8);
    int rc = emit(io, hdr, BT_MCP_HDR_LEN);
    if (!rc) rc = emit(io, fcs, 2);
    if (!rc && plen) rc = emit(io, payload + done, plen);
    if (rc) return rc;               /* 透传 port 背压码（E_NOMEM/E_AGAIN） */
#else
    (void)f;
    int rc = emit(io, hdr, BT_MCP_HDR_LEN);
    if (!rc && plen) rc = emit(io, payload + done, plen);
    if (rc) return rc;               /* 透传 port 背压码（E_NOMEM/E_AGAIN） */
#endif
    return BT_MCP_OK;
}

/* 校验 + 归一：返回分片数，负值=错误码 */
static int plan_frags(bt_mcp_framer_t *f, uint8_t kind, uint32_t len,
                      uint16_t *chunk_out, uint16_t *nfrag_out)
{
    if (len > BT_MCP_CFG_MAX_MSG) return BT_MCP_E_TOOBIG;
    uint16_t chunk = chunk_cap(f->cap);
    if (chunk == 0) return BT_MCP_E_STATE;
    if (kind == BT_MCP_KIND_CTRL && len > BT_MCP_CTRL_MAX_PAYLOAD)
        return BT_MCP_E_TOOBIG;
    uint32_t nfrag = len ? (len + chunk - 1) / chunk : 1; /* 空消息=1 片零载荷 */
    if (kind == BT_MCP_KIND_CTRL && nfrag > 1) return BT_MCP_E_TOOBIG;
    if (nfrag > BT_MCP_CFG_MAX_FRAG) return BT_MCP_E_TOOBIG;
    *chunk_out = chunk;
    *nfrag_out = (uint16_t)nfrag;
    return BT_MCP_OK;
}

int bt_mcp_framer_send_seq(bt_mcp_framer_t *f, uint8_t kind, uint8_t seq,
                           const uint8_t *payload, uint32_t len,
                           int (*emit)(void *, const uint8_t *, uint16_t),
                           void *io)
{
    if (!f || !f->cap || !emit || (!payload && len)) return BT_MCP_E_ARGS;
    uint16_t chunk, nfrag;
    int rc = plan_frags(f, kind, len, &chunk, &nfrag);
    if (rc) return rc;
    for (uint8_t idx = 0; idx < (uint8_t)nfrag; idx++) {
        rc = emit_chunk(f, kind, seq, idx, (uint8_t)nfrag, chunk,
                        payload, len, emit, io);
        if (rc) return rc;
    }
    return BT_MCP_OK;
}

uint8_t bt_mcp_framer_alloc_seq(bt_mcp_framer_t *f)
{
    return f ? f->tx_seq++ : 0;
}

int bt_mcp_framer_send(bt_mcp_framer_t *f, uint8_t kind,
                       const uint8_t *payload, uint32_t len,
                       int (*emit)(void *, const uint8_t *, uint16_t),
                       void *io)
{
    if (!f) return BT_MCP_E_ARGS;
    const uint8_t seq = f->tx_seq++;
    return bt_mcp_framer_send_seq(f, kind, seq, payload, len, emit, io);
}

int bt_mcp_framer_send_idx(bt_mcp_framer_t *f, uint8_t kind, uint8_t seq,
                           uint8_t idx, const uint8_t *payload, uint32_t len,
                           int (*emit)(void *, const uint8_t *, uint16_t),
                           void *io)
{
    if (!f || !f->cap || !emit || (!payload && len)) return BT_MCP_E_ARGS;
    uint16_t chunk, nfrag;
    int rc = plan_frags(f, kind, len, &chunk, &nfrag);
    if (rc) return rc;
    if (idx >= (uint8_t)nfrag) return BT_MCP_E_ARGS;
    return emit_chunk(f, kind, seq, idx, (uint8_t)nfrag, chunk,
                      payload, len, emit, io);
}

/* ---- 接收侧：包通道（每次 feed 一完整片） ---- */

static struct bt_mcp_reasm_slot *slot_for(bt_mcp_framer_t *f, uint8_t seq)
{
    for (int i = 0; i < f->cfg.max_reasm; i++)
        if (f->reasm[i].in_use && f->reasm[i].msg_seq == seq)
            return &f->reasm[i];
    for (int i = 0; i < f->cfg.max_reasm; i++)
        if (!f->reasm[i].in_use) {
            memset(&f->reasm[i], 0, sizeof(f->reasm[i]));
            f->reasm[i].in_use = 1;
            f->reasm[i].msg_seq = seq;
            return &f->reasm[i];
        }
    return NULL;
}

static int feed_packet(bt_mcp_framer_t *f, const uint8_t *frag, uint16_t len,
                       bt_mcp_msg_sink_t sink, void *user)
{
    if (len < BT_MCP_HDR_LEN) return BT_MCP_E_SYNC;
    uint8_t  flags = frag[1];
    uint8_t  seq   = frag[2];
    uint8_t  idx   = frag[3];
    uint16_t plen  = (uint16_t)(frag[4] | ((uint16_t)frag[5] << 8));
    const uint8_t *pl = frag + BT_MCP_HDR_LEN;

#if BT_MCP_CFG_FCS16
    if (len < BT_MCP_HDR_LEN + 2u + plen) return BT_MCP_E_SYNC;
    uint16_t want = crc_cont(frag, BT_MCP_HDR_LEN, 0xFFFF);
    want = crc_cont(frag + BT_MCP_HDR_LEN + 2, plen, want);
    if (frag[6] != (want & 0xFF) || frag[7] != (want >> 8)) return BT_MCP_E_SYNC;
    pl = frag + BT_MCP_HDR_LEN + 2;
#else
    if (len < BT_MCP_HDR_LEN + (uint32_t)plen) return BT_MCP_E_SYNC;
#endif
    if (idx >= BT_MCP_CFG_MAX_FRAG) return BT_MCP_E_SYNC;

    const int ctrl = (flags & BT_MCP_FLAG_CTRL) != 0;

    if (!(flags & BT_MCP_FLAG_CHUNK)) {           /* 单帧消息：零重组直抛 */
        sink(user, ctrl ? BT_MCP_KIND_CTRL : BT_MCP_KIND_MCP, seq, pl, plen);
        return BT_MCP_OK;
    }

    struct bt_mcp_reasm_slot *s = slot_for(f, seq);
    if (!s) return BT_MCP_E_NOMEM;                /* 窗口满：丢弃，信用层背压 */

    if (s->len_total == 0 && s->next_idx == 0) {  /* 新槽首片：记录元数据 */
        s->ctrl = (uint8_t)ctrl;
        s->t_first = f->now_ms;
    }
    if (idx != s->next_idx) {
        if (idx < s->next_idx) return BT_MCP_OK;  /* 迟到重复片：静默忽略 */
        /* 缺片（判例 #2）：保持槽存活，登记 NACK 请对端补发 next_idx 片 */
        if (++s->gap_wait > BT_MCP_GAP_WAIT_MAX) {
            s->in_use = 0;                        /* 补不齐（对端失联）→ 弃槽 */
            return BT_MCP_E_SYNC;
        }
        if (!f->nack_pending) {
            f->nack_pending = 1;
            f->nack_seq = seq;
            f->nack_idx = s->next_idx;
        }
        return BT_MCP_OK;
    }
    if ((uint32_t)s->len_total + plen > BT_MCP_CFG_MAX_MSG) {
        s->in_use = 0;
        return BT_MCP_E_TOOBIG;
    }
    memcpy(s->buf + s->len_total, pl, plen);
    s->len_total = (uint16_t)(s->len_total + plen);
    s->next_idx++;
    s->t_last = f->now_ms;

    if (flags & BT_MCP_FLAG_LAST) {
        sink(user, s->ctrl ? BT_MCP_KIND_CTRL : BT_MCP_KIND_MCP,
             seq, s->buf, s->len_total);
        s->in_use = 0;
    }
    return BT_MCP_OK;
}

/* ---- 接收侧：流通道（任意字节段） ---- */

static int feed_stream(bt_mcp_framer_t *f, const uint8_t *in, uint16_t len,
                       bt_mcp_msg_sink_t sink, void *user)
{
    while (len) {
        if (!f->s_active) {
            while (len && f->s_hdr_got < BT_MCP_HDR_LEN) {
                f->s_hdr[f->s_hdr_got++] = *in++; len--;
            }
            if (f->s_hdr_got < BT_MCP_HDR_LEN) return BT_MCP_OK;
            if (f->s_hdr[0] != BT_MCP_MAGIC) {     /* 失步：滑窗找 magic */
                int i = 1;
                while (i < BT_MCP_HDR_LEN && f->s_hdr[i] != BT_MCP_MAGIC) i++;
                f->s_hdr_got = (uint8_t)(BT_MCP_HDR_LEN - i);
                memmove(f->s_hdr, f->s_hdr + i, (size_t)f->s_hdr_got);
                continue;
            }
            f->s_len_got = 0;
            f->s_active = 1;
        }

        uint8_t  flags = f->s_hdr[1];
        uint8_t  seq   = f->s_hdr[2];
        uint16_t plen  = (uint16_t)(f->s_hdr[4] | ((uint16_t)f->s_hdr[5] << 8));
        /* 流通道：单帧承载整条消息（CHUNKED=0），plen 上限 = MAX_MSG；
           CTRL 帧仍恒 ≤255（判例）。 */
        if ((flags & BT_MCP_FLAG_CTRL) && plen > BT_MCP_CTRL_MAX_PAYLOAD)
            return BT_MCP_E_SYNC;
        if (plen > BT_MCP_CFG_MAX_MSG) return BT_MCP_E_SYNC;
        if (f->s_len_got + (uint32_t)plen > BT_MCP_CFG_MAX_MSG)
            return BT_MCP_E_SYNC;

        uint16_t take = (uint16_t)(plen - f->s_len_got);
        if (take > len) take = len;
        memcpy(f->s_cur + f->s_len_got, in, take);
        f->s_len_got = (uint16_t)(f->s_len_got + take);
        in += take; len -= take;
        if (f->s_len_got < plen) return BT_MCP_OK;

        if (flags & BT_MCP_FLAG_CHUNK) return BT_MCP_E_SYNC;
        sink(user, (flags & BT_MCP_FLAG_CTRL) ? BT_MCP_KIND_CTRL
                                              : BT_MCP_KIND_MCP,
             seq, f->s_cur, plen);
        f->s_active = 0; f->s_hdr_got = 0; f->s_len_got = 0;
    }
    return BT_MCP_OK;
}

int bt_mcp_framer_feed(bt_mcp_framer_t *f, const uint8_t *in, uint16_t len,
                       bt_mcp_msg_sink_t sink, void *user)
{
    if (!f || !f->cap || (!in && len) || !sink) return BT_MCP_E_ARGS;
    if (!len) return BT_MCP_OK;
    if (f->cap->kind == BT_MCP_CHAN_PACKET)
        return feed_packet(f, in, len, sink, user);
    return feed_stream(f, in, len, sink, user);
}

int bt_mcp_framer_take_nack(bt_mcp_framer_t *f, uint8_t *seq, uint8_t *idx)
{
    if (!f || !f->nack_pending) return 0;
    if (seq) *seq = f->nack_seq;
    if (idx) *idx = f->nack_idx;
    f->nack_pending = 0;
    return 1;
}

void bt_mcp_framer_tick(bt_mcp_framer_t *f, uint32_t now_ms)
{
    if (!f) return;
    f->now_ms = now_ms;
    for (int i = 0; i < f->cfg.max_reasm; i++)
        if (f->reasm[i].in_use) {
            if (now_ms - f->reasm[i].t_first > f->cfg.frag_tout_ms)
                f->reasm[i].in_use = 0;           /* 半帧超时回收（草案 §4.3） */
            else
                f->reasm[i].t_last = now_ms;
        }
}
