/* test_shared_reasm.c —— 共享重组缓冲（v0.5 V11）。
 *
 * 编译期开关 BT_MCP_CFG_SHARED_REASM=1 时，重组只用一个 MAX_MSG 缓冲
 * （framer 从 41072B 降到 16448B）。本测试验证：
 *  1) 共享模式仍能正确重组一条跨多片的消息（含 >单片 的载荷完整性）；
 *  2) 乱序/缺片在共享模式下仍按同一套规则处理；
 *  3) 共享模式确实只保留一条在重组中（交织的第二条按协议允许被丢弃，
 *     不能导致状态机崩溃或串台）。
 *
 * 同一个源文件在两种配置下都能编译：默认配置下第 3 条断言自动跳过。
 */
#include <stdio.h>
#include <string.h>

#include "bt_mcp/frame.h"
#include "bt_mcp/err.h"

static int g_pass, g_fail;
#define CHECK(c, m) do { if (c) g_pass++; else { g_fail++; \
    printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, m); } } while (0)

typedef struct { uint8_t b[65536]; size_t n; } vec_t;
static int vput(void *io, const uint8_t *d, uint16_t n)
{
    vec_t *v = io;
    if (v->n + n > sizeof v->b) return BT_MCP_E_NOMEM;
    memcpy(v->b + v->n, d, n); v->n += n; return 0;
}

typedef struct { int count; uint8_t last[8192]; uint16_t last_len; } sink_t;
static void on_msg(void *u, uint8_t kind, uint8_t seq, const uint8_t *pl,
                   uint16_t len)
{
    sink_t *s = u;
    (void)kind; (void)seq;
    s->count++;
    if (len <= sizeof s->last) { memcpy(s->last, pl, len); s->last_len = len; }
}

int main(void)
{
    static bt_mcp_chan_cap_t cap = { .kind = BT_MCP_CHAN_PACKET, .mtu = 64,
                                     .reliable = 1, .flow_ctrl = 0,
                                     .max_pending = 4 };
    static bt_mcp_frame_cfg_t fcfg = { .max_reasm = BT_MCP_CFG_MAX_REASM,
                                       .frag_tout_ms = 2000 };
    static bt_mcp_framer_t tx, rx;
    static vec_t wire;
    static sink_t sk;
    static uint8_t payload[1000];

    bt_mcp_framer_init(&tx, &fcfg, &cap);
    bt_mcp_framer_init(&rx, &fcfg, &cap);

    for (size_t i = 0; i < sizeof payload; i++) payload[i] = (uint8_t)(i * 7);

    /* ---- 1. 跨多片消息完整重组 ---- */
    {
        wire.n = 0; sk.count = 0; sk.last_len = 0;
        CHECK(bt_mcp_framer_send(&tx, BT_MCP_KIND_MCP, payload,
                                 sizeof payload, vput, &wire) == 0, "send 1000B");
        int frags = 0;
        size_t off = 0;
        while (off + BT_MCP_HDR_LEN <= wire.n) {
            uint16_t pl = (uint16_t)(wire.b[off + 4] | (wire.b[off + 5] << 8));
            if (off + BT_MCP_HDR_LEN + pl > wire.n) break;
            (void)bt_mcp_framer_feed(&rx, wire.b + off,
                                     (uint16_t)(BT_MCP_HDR_LEN + pl),
                                     on_msg, &sk);
            off += BT_MCP_HDR_LEN + pl;
            frags++;
        }
        CHECK(frags > 1, "payload spans multiple fragments");
        CHECK(sk.count == 1, "reassembled exactly once");
        CHECK(sk.last_len == sizeof payload, "assembled length matches");
        CHECK(!memcmp(sk.last, payload, sizeof payload), "assembled bytes match");
    }

    /* ---- 2. 缺片语义：缺口不完成，过期片按重复丢弃（真实语义） ----
     * 恢复路径是「接收侧 NACK 缺片 → 发送侧 send_idx 定点补发」，
     * 由 test_reliability.c 覆盖。这里只验证帧层的两条不变量：
     *  a) 有缺口时绝不上抛半条消息；
     *  b) idx < next_idx 的迟到分片按重复丢弃，不破坏已收内容。 */
    {
        bt_mcp_framer_init(&rx, &fcfg, &cap);
        wire.n = 0; sk.count = 0; sk.last_len = 0;
        bt_mcp_framer_send(&tx, BT_MCP_KIND_MCP, payload, sizeof payload,
                           vput, &wire);
        size_t offs[64]; uint16_t plens[64]; int nf = 0;
        size_t off = 0;
        while (off + BT_MCP_HDR_LEN <= wire.n && nf < 64) {
            uint16_t pl = (uint16_t)(wire.b[off + 4] | (wire.b[off + 5] << 8));
            if (off + BT_MCP_HDR_LEN + pl > wire.n) break;
            offs[nf] = off; plens[nf] = pl; nf++;
            off += BT_MCP_HDR_LEN + pl;
        }
        if (nf >= 3) {
            /* 喂第 0 片与第 2..n 片，跳过第 1 片 */
            (void)bt_mcp_framer_feed(&rx, wire.b + offs[0],
                                     (uint16_t)(BT_MCP_HDR_LEN + plens[0]),
                                     on_msg, &sk);
            for (int i = 2; i < nf; i++)
                (void)bt_mcp_framer_feed(&rx, wire.b + offs[i],
                                         (uint16_t)(BT_MCP_HDR_LEN + plens[i]),
                                         on_msg, &sk);
            CHECK(sk.count == 0, "gap: no premature delivery");
            CHECK(rx.reasm[0].in_use == 1, "gap: slot still holding partial msg");
            /* 迟到重发第 0 片（idx < next_idx）→ 按重复丢弃，仍不完成 */
            (void)bt_mcp_framer_feed(&rx, wire.b + offs[0],
                                     (uint16_t)(BT_MCP_HDR_LEN + plens[0]),
                                     on_msg, &sk);
            CHECK(sk.count == 0, "stale fragment dropped as duplicate");
            /* 接收侧应登记了待补的缺片（NACK 队列） */
            uint8_t ns = 0, ni[BT_MCP_CFG_NACK_MAX_IDX], nc = 0;
            CHECK(bt_mcp_framer_take_nack(&rx, &ns, ni,
                                          (uint8_t)BT_MCP_CFG_NACK_MAX_IDX,
                                          &nc) >= 1,
                  "missing fragment registered for NACK");
            CHECK(nc >= 1 && ni[0] == 1, "NACK points at the missing index");
        } else {
            CHECK(nf >= 3, "expected >=3 fragments");
        }
    }

    /* ---- 3. 单槽限制的真实语义：受限于**分片**消息，单片消息不受影响 ---- */
    {
        bt_mcp_framer_init(&rx, &fcfg, &cap);
        sk.count = 0;
        static bt_mcp_framer_t ta, tb;
        static vec_t wa, wb;
        bt_mcp_framer_init(&ta, &fcfg, &cap);
        bt_mcp_framer_init(&tb, &fcfg, &cap);
        /* 关键：两个 framer 默认都从 seq=0 开始，若不推进 B 的序号，
         * 接收侧会把 B 当成 A 的续片（同一消息）。推进一次即可区分。 */
        (void)bt_mcp_framer_alloc_seq(&tb);
        wa.n = 0; wb.n = 0;
        /* A、B 都是需要重组的多片消息 */
        bt_mcp_framer_send(&ta, BT_MCP_KIND_MCP, payload, sizeof payload,
                           vput, &wa);
        bt_mcp_framer_send(&tb, BT_MCP_KIND_MCP, payload, sizeof payload,
                           vput, &wb);
        /* 交错：A 第 0 片 → B 第 0 片 */
        uint16_t a0 = (uint16_t)(wa.b[4] | (wa.b[5] << 8));
        uint16_t b0 = (uint16_t)(wb.b[4] | (wb.b[5] << 8));
        (void)bt_mcp_framer_feed(&rx, wa.b,
                                 (uint16_t)(BT_MCP_HDR_LEN + a0), on_msg, &sk);
        (void)bt_mcp_framer_feed(&rx, wb.b,
                                 (uint16_t)(BT_MCP_HDR_LEN + b0), on_msg, &sk);
        /* 关键断言：两条分片消息不可能同时在重组中 */
        int active = 0;
        for (int k = 0; k < BT_MCP_CFG_MAX_REASM; k++)
            if (rx.reasm[k].in_use) active++;
        CHECK(active <= BT_MCP_CFG_MAX_REASM, "active slots within capacity");
#if BT_MCP_CFG_SHARED_REASM
        CHECK(active == 1, "shared: only one reassembly in flight");
#else
        CHECK(active == 2, "multi-slot: two reassemblies in flight");
#endif
        /* 无论哪种配置：喂完 A 的其余片都必须正确完成 A */
        size_t off = BT_MCP_HDR_LEN + a0;
        int done_a = 0;
        while (off + BT_MCP_HDR_LEN <= wa.n) {
            uint16_t pl = (uint16_t)(wa.b[off + 4] | (wa.b[off + 5] << 8));
            if (off + BT_MCP_HDR_LEN + pl > wa.n) break;
            (void)bt_mcp_framer_feed(&rx, wa.b + off,
                                     (uint16_t)(BT_MCP_HDR_LEN + pl),
                                     on_msg, &sk);
            off += BT_MCP_HDR_LEN + pl;
        }
        done_a = (sk.count >= 1 && sk.last_len == sizeof payload
                  && !memcmp(sk.last, payload, sizeof payload));
        CHECK(done_a, "A completed after interleaving (both configs)");
    }

    printf("\n%d pass, %d fail (SHARED=%d, MAX_REASM=%d)\n", g_pass, g_fail,
           BT_MCP_CFG_SHARED_REASM, BT_MCP_CFG_MAX_REASM);
    return g_fail ? 1 : 0;
}
