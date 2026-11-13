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

static uint16_t chunk_cap(const bt_mcp_chan_cap_t *cap, uint16_t mtu_ovr)
{
    /* 单片载荷上限：包通道 = min(mtu,261)-6（≤255）；流通道 = 整消息单帧直发，
     * 上限即 MAX_MSG（u16 len 可承载）。返回 0 = 通道不可用。
     * mtu_ovr != 0 表示链路 MTU 热更新值（v0.4 C4）。 */
    if (cap->kind == BT_MCP_CHAN_STREAM) return BT_MCP_CFG_MAX_MSG;
    uint16_t m = mtu_ovr ? mtu_ovr : cap->mtu;
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
    if (chunk_cap(cap, 0) == 0) return BT_MCP_E_ARGS;
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

int bt_mcp_framer_set_mtu(bt_mcp_framer_t *f, uint16_t mtu)
{
    if (!f || !f->cap) return BT_MCP_E_ARGS;
    if (f->cap->kind == BT_MCP_CHAN_STREAM) return BT_MCP_E_ARGS;  /* 流通道无 MTU 概念 */
    if (mtu && mtu <= BT_MCP_HDR_LEN) return BT_MCP_E_ARGS;
    uint16_t old = chunk_cap(f->cap, f->mtu_override);
    f->mtu_override = mtu;      /* 0 = 回退 cap->mtu */
    /* v0.9.1（位图重组的必要约束）：MTU 变化会改变分片边界，**在途重组**
     * 的分片偏移随之失效（已经收到的片落在旧边界上，后续片落在新边界上，
     * 拼出来的是垃圾）。这类消息本来就无法正确完成，直接弃掉在途槽——
     * 由上层的消息级重传重新发一遍（重新发的片会用新 MTU 一致切片）。
     * 严格按序拼接的旧实现没有这个问题（它不按偏移落位），但代价是必须
     * 只补「缺的那一片」而不能乱序缓存；两者不可兼得，这里选正确的那个。 */
    if (chunk_cap(f->cap, f->mtu_override) != old) {
        memset(f->reasm, 0, sizeof(f->reasm));
        f->dead_valid = 0;
        f->nack_pending = 0;
    }
    return BT_MCP_OK;
}

void bt_mcp_framer_reset(bt_mcp_framer_t *f)
{
    if (!f) return;
    memset(f->reasm, 0, sizeof(f->reasm));
    f->s_hdr_got = 0;
    f->s_len_got = 0;
    f->s_active = 0;
    f->s_fcs_got = 0;
    f->nack_pending = 0;
    f->dead_valid = 0;
    f->dead_seq = 0;
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

/* 发一片。线序固定为 hdr(6) | FCS(2, 可选) | payload —— 与接收侧解析一致。
 * 包通道片载荷 ≤255B，恒可一次 emit；流通道大帧分两次（头+校验 / 载荷），
 * 字节流语义下等价。判例：一个分片 = 一次 emit（GATT notify 边界）。 */
static int emit_chunk(bt_mcp_framer_t *f, uint8_t kind, uint8_t seq,
                      uint8_t idx, uint8_t nfrag, uint16_t chunk,
                      const uint8_t *payload, uint32_t len,
                      int (*emit)(void *, const uint8_t *, uint16_t), void *io)
{
    (void)f;
    uint32_t done = (uint32_t)idx * chunk;
    if (done > len) done = len;
    uint16_t plen = (uint16_t)(len - done < chunk ? len - done : chunk);
    const uint8_t flags = (uint8_t)((kind == BT_MCP_KIND_CTRL ? BT_MCP_FLAG_CTRL : 0)
                          | (nfrag > 1 ? BT_MCP_FLAG_CHUNK : 0)
                          | (idx == nfrag - 1 ? BT_MCP_FLAG_LAST : 0));
    uint8_t out[BT_MCP_HDR_LEN + 2 + BT_MCP_MAX_CHUNK_PAYLOAD];
    uint16_t n = 0;
    out[n++] = BT_MCP_MAGIC;
    out[n++] = flags;
    out[n++] = seq;
    out[n++] = idx;
    out[n++] = (uint8_t)(plen & 0xFF);
    out[n++] = (uint8_t)(plen >> 8);
#if BT_MCP_CFG_FCS16
    uint16_t crc = crc_cont(out, BT_MCP_HDR_LEN, 0xFFFF);
    crc = crc_cont(payload + done, plen, crc);
    out[n++] = (uint8_t)(crc & 0xFF);
    out[n++] = (uint8_t)(crc >> 8);
#endif
    const int one_shot = (uint32_t)n + plen <= (uint32_t)sizeof out;
    if (plen && one_shot) {
        memcpy(out + n, payload + done, plen);
        n = (uint16_t)(n + plen);
    }
    int rc = emit(io, out, n);       /* 透传 port 背压码（E_NOMEM/E_AGAIN） */
    if (!rc && plen && !one_shot) rc = emit(io, payload + done, plen);
    return rc ? rc : BT_MCP_OK;
}

/* 校验 + 归一：返回分片数，负值=错误码 */
static int plan_frags(bt_mcp_framer_t *f, uint8_t kind, uint32_t len,
                      uint16_t *chunk_out, uint16_t *nfrag_out)
{
    if (len > BT_MCP_CFG_MAX_MSG) return BT_MCP_E_TOOBIG;
    uint16_t chunk = chunk_cap(f->cap, f->mtu_override);
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

/* 有效 MTU：优先用热更新值，否则用通道能力（与 send 内的取法一致） */
static uint16_t eff_mtu(const bt_mcp_framer_t *f)
{
    return f->mtu_override ? f->mtu_override : f->cap->mtu;
}

uint8_t bt_mcp_framer_frag_count(const bt_mcp_framer_t *f, uint32_t len)
{
    if (!f || !f->cap) return 1;
    uint16_t mtu = eff_mtu(f);
    if (f->cap->kind == BT_MCP_CHAN_STREAM) return 1;   /* 流通道单帧直发 */
    uint32_t chunk = (uint32_t)(mtu > 261 ? 261 : mtu);
    if (chunk <= BT_MCP_HDR_LEN + (BT_MCP_CFG_FCS16 ? 2u : 0u)) return 1;
    chunk -= BT_MCP_HDR_LEN + (BT_MCP_CFG_FCS16 ? 2u : 0u);
    uint32_t n = len ? (len + chunk - 1) / chunk : 1;
    if (n > 255) n = 255;
    return (uint8_t)n;
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

    /* 已放弃的消息：同 seq 的后续分片直接忽略（换 seq 后自然解除） */
    if (f->dead_valid && f->dead_seq == seq) return BT_MCP_OK;

    struct bt_mcp_reasm_slot *s = slot_for(f, seq);
    if (!s) return BT_MCP_E_NOMEM;                /* 窗口满：丢弃，信用层背压 */

    /* v0.9.1（A2 根因修复）：乱序缓存。
     *
     * 所有**非末尾片**的载荷长度都等于 chunk（见 emit_chunk），所以
     * 片 i 的字节偏移可以直接算成 i*chunk，不需要记录每片长度。
     * 收到就落位并置位图；只有位图里真正缺的片才进 NACK。
     * 这样发送端只补缺的，不再「从缺片补到末尾」（v0.8 的做法会把已到达
     * 的片重发一遍，实测约 19 倍带宽放大）。 */
    if (!s->in_use || (s->len_total == 0 && !s->have_last && s->got[0] == 0
                       && s->chunk == 0)) {
        /* 新槽首片：记录元数据并清位图 */
        s->ctrl = (uint8_t)ctrl;
        s->t_first = f->now_ms;
        memset(s->got, 0, sizeof s->got);
        s->have_last = 0;
        s->last_len = 0;
        s->len_total = 0;
        s->gap_wait = 0;
        s->nack_len = 0;
        /* **偏移基准 = 分片边界**。推导顺序：
         *  1) 非 LAST 片的长度**恒等于** chunk（发送侧就是这么切的）→ 最权威；
         *  2) 若本片是 LAST（长度更短），退回用本地 MTU 推导的 chunk_cap()；
         *  3) 若 chunk_cap() 也不可用（MTU 未配置），暂记 0 = 未知。
         * 注意不能「用片长当基准后又被更长的片修正」：那会让先前落的片
         * 偏移全错（实测表现为重复投递，test_lossy 抓到过）。 */
        uint16_t cc = (flags & BT_MCP_FLAG_LAST) ? 0 : plen;
        if (!cc) cc = chunk_cap(f->cap, f->mtu_override);
        s->chunk = cc;          /* 0 = 未知 */
    }

    if (!s->chunk) {
        /* 基准未知（只收到了 LAST 片，或 MTU 未配置）：本片无法定位。
         * 丢弃并不置位图 → 缺口仍在 → ARQ 会重发，届时基准已知。 */
        return BT_MCP_OK;
    }
    uint32_t off = (uint32_t)idx * s->chunk;
    if (off + plen > BT_MCP_CFG_MAX_MSG) {
        s->in_use = 0;
        return BT_MCP_E_TOOBIG;
    }

    uint8_t bit = (uint8_t)(1u << (idx & 7u));
    int already = (s->got[idx >> 3] & bit) != 0;
    if (!already) {
        memcpy(s->buf + off, pl, plen);
        s->got[idx >> 3] = (uint8_t)(s->got[idx >> 3] | bit);
        s->len_total = (uint16_t)(s->len_total + plen);
        s->t_last = f->now_ms;
    } else {
        s->t_last = f->now_ms;      /* 重复片：只刷新活跃时间 */
        /* 无进展的重发也计入缺失等待：否则「对端反复重发同一片」
         * 这种真·失联场景永远不会弃槽（且不依赖 pump 是否被调用）。 */
        if (++s->gap_wait > BT_MCP_GAP_WAIT_MAX) {
            s->in_use = 0;
            f->dead_valid = 1;
            f->dead_seq = seq;
            return BT_MCP_E_SYNC;
        }
    }
    f->dead_valid = 0;          /* 有分片成功落位 → 解除放弃标记 */
    if (flags & BT_MCP_FLAG_LAST) {
        s->have_last = 1;
        s->last_idx = idx;
        s->last_len = plen;
    }

    /* 缺片统计：从 0 扫到 max(已见最高 idx, last_idx) */
    uint8_t hi = idx;
    if (s->have_last && s->last_idx > hi) hi = s->last_idx;
    uint8_t missing[BT_MCP_CFG_NACK_MAX_IDX];
    uint8_t nmiss = 0;
    uint32_t n_missing_total = 0;
    for (uint8_t i = 0; i <= hi; i++) {
        if (s->got[i >> 3] & (uint8_t)(1u << (i & 7u))) continue;
        n_missing_total++;
        if (nmiss < BT_MCP_CFG_NACK_MAX_IDX) missing[nmiss++] = i;
    }

    if (n_missing_total) {
        /* 有缺口：登记 NACK（一次报多个）。
         * gap_wait 的语义是「连续多少轮 NACK **没有任何进展**」——
         * 不能按「收到带缺口的片」计数：乱序到达时缺口是常态，
         * 那样会把还在正常推进的消息误判为对端失联（实测交付率 34→16）。 */
        if (!f->nack_pending) {
            if (s->len_total == s->nack_len) {
                if (++s->gap_wait > BT_MCP_GAP_WAIT_MAX) {
                    s->in_use = 0;                /* 真补不齐 → 弃槽 */
                    f->dead_valid = 1;
                    f->dead_seq = seq;
                    return BT_MCP_E_SYNC;
                }
            } else {
                s->gap_wait = 0;                  /* 有进展：重置 */
                s->nack_len = s->len_total;
            }
            f->nack_pending = 1;
            f->nack_seq = seq;
            f->nack_count = nmiss;
            memcpy(f->nack_idx, missing, nmiss);
        }
        return BT_MCP_OK;
    }

    /* 无缺口且已收到 LAST → 完整 */
    if (s->have_last) {
        uint32_t total = (uint32_t)s->last_idx * s->chunk + s->last_len;
        if (total > BT_MCP_CFG_MAX_MSG) { s->in_use = 0; return BT_MCP_E_TOOBIG; }
        sink(user, s->ctrl ? BT_MCP_KIND_CTRL : BT_MCP_KIND_MCP,
             seq, s->buf, (uint16_t)total);
        s->in_use = 0;
    }
    return BT_MCP_OK;
}

/* ---- 接收侧：流通道（任意字节段） ---- */

/* 流通道畸形帧：丢弃当前帧状态并重新找 magic（v0.3 修「一次坏帧永久卡死」） */
static int stream_desync(bt_mcp_framer_t *f)
{
    f->s_active = 0;
    f->s_hdr_got = 0;
    f->s_len_got = 0;
    f->s_fcs_got = 0;
    return BT_MCP_E_SYNC;
}

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
            f->s_fcs_got = 0;
            f->s_active = 1;
        }

        uint8_t  flags = f->s_hdr[1];
        uint8_t  seq   = f->s_hdr[2];
        uint16_t plen  = (uint16_t)(f->s_hdr[4] | ((uint16_t)f->s_hdr[5] << 8));
        /* 流通道：单帧承载整条消息（CHUNKED=0），plen 上限 = MAX_MSG；
           CTRL 帧仍恒 ≤255（判例）。 */
        if ((flags & BT_MCP_FLAG_CTRL) && plen > BT_MCP_CTRL_MAX_PAYLOAD)
            return stream_desync(f);
        if (plen > BT_MCP_CFG_MAX_MSG) return stream_desync(f);
        if (f->s_len_got + (uint32_t)plen > BT_MCP_CFG_MAX_MSG)
            return stream_desync(f);
        if (flags & BT_MCP_FLAG_CHUNK) return stream_desync(f);

#if BT_MCP_CFG_FCS16
        while (f->s_fcs_got < 2 && len) {
            f->s_fcs[f->s_fcs_got++] = *in++;
            len--;
        }
        if (f->s_fcs_got < 2) return BT_MCP_OK;
#endif

        uint16_t take = (uint16_t)(plen - f->s_len_got);
        if (take > len) take = len;
        memcpy(f->s_cur + f->s_len_got, in, take);
        f->s_len_got = (uint16_t)(f->s_len_got + take);
        in += take; len -= take;
        if (f->s_len_got < plen) return BT_MCP_OK;

#if BT_MCP_CFG_FCS16
        {
            uint16_t want = crc_cont(f->s_hdr, BT_MCP_HDR_LEN, 0xFFFF);
            want = crc_cont(f->s_cur, plen, want);
            if (f->s_fcs[0] != (want & 0xFF) || f->s_fcs[1] != (want >> 8))
                return stream_desync(f);
        }
#endif
        sink(user, (flags & BT_MCP_FLAG_CTRL) ? BT_MCP_KIND_CTRL
                                              : BT_MCP_KIND_MCP,
             seq, f->s_cur, plen);
        f->s_active = 0; f->s_hdr_got = 0; f->s_len_got = 0; f->s_fcs_got = 0;
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

int bt_mcp_framer_take_nack(bt_mcp_framer_t *f, uint8_t *seq,
                            uint8_t *idx, uint8_t max, uint8_t *count_out)
{
    if (!f || !f->nack_pending) {
        if (count_out) *count_out = 0;
        return 0;
    }
    uint8_t n = f->nack_count;
    if (n > max) n = max;
    if (seq) *seq = f->nack_seq;
    if (idx && n) memcpy(idx, f->nack_idx, n);
    if (count_out) *count_out = n;
    f->nack_pending = 0;
    f->nack_count = 0;
    return n;
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
