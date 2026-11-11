/* test_reliability.c —— 不可靠包通道可靠性子系统的定向压力测试。
 *
 * 与差分 harness 互补：这里不做 C↔Rust 对拍，而是用「已知正确答案」的
 * 端到端场景验证语义性质（丢片可补、重复不重放、超时重传、槽位回收）。
 *
 * 编：cc -O2 -std=c99 -Wall -Wextra -Werror [-fsanitize=address,undefined] \
 *        -Icore/include tests/test_reliability.c core/src/frame.c \
 *        core/src/session.c -o build/test_reliability
 */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "bt_mcp/frame.h"
#include "bt_mcp/session.h"
#include "bt_mcp/err.h"

static int g_pass, g_fail;
#define CHECK(c, m) do { if (c) g_pass++; else { g_fail++; \
    printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, m); } } while (0)

/* 帧记录队列（保留片边界，便于刻意丢片/复制/乱序） */
#define MAXF 256
typedef struct {
    uint8_t  buf[16384];
    uint16_t len[MAXF];
    size_t   off[MAXF];
    int      n;
} fq_t;

static int q_emit(void *io, const uint8_t *d, uint16_t n)
{
    fq_t *q = io;
    if (q->n >= MAXF) return BT_MCP_E_NOMEM;
    size_t tail = q->n ? q->off[q->n - 1] + q->len[q->n - 1] : 0;
    if (tail + n > sizeof q->buf) return BT_MCP_E_NOMEM;
    memcpy(q->buf + tail, d, n);
    q->off[q->n] = tail;
    q->len[q->n] = n;
    q->n++;
    return 0;
}

static size_t q_bytes(const fq_t *q)
{
    return q->n ? q->off[q->n - 1] + q->len[q->n - 1] : 0;
}

/* 统计上抛消息数（L4 → L5 形态） */
static int g_msgs;
static void sink(void *u, const uint8_t *json, uint32_t len)
{
    (void)u; (void)json; (void)len;
    g_msgs++;
}

static bt_mcp_framer_t  fr_tx, fr_rx;
static bt_mcp_session_t se_tx, se_rx;
static fq_t q, q_rx;   /* q=发送侧帧；q_rx=接收侧应答帧（隔离，别污染片索引） */
/* framer 只存 cap 指针：生命周期须覆盖 framer（API 约定），故用静态 */
static bt_mcp_chan_cap_t g_cap;
static bt_mcp_frame_cfg_t g_fcfg;

static void setup(uint16_t mtu)
{
    g_cap.kind = BT_MCP_CHAN_PACKET;
    g_cap.mtu = mtu;
    g_cap.reliable = 0;
    g_cap.flow_ctrl = 0;
    g_cap.max_pending = 4;
    g_fcfg.max_reasm = 4;
    g_fcfg.frag_tout_ms = 2000;
    bt_mcp_session_cfg_t cfg = { .req_timeout_ms = 30000, .resume_tout_ms = 5000,
                                 .tx_window = 4, .role = BT_MCP_ROLE_CLIENT };
    bt_mcp_framer_init(&fr_tx, &g_fcfg, &g_cap);
    bt_mcp_framer_init(&fr_rx, &g_fcfg, &g_cap);
    bt_mcp_session_init(&se_tx, &cfg);
    bt_mcp_session_init(&se_rx, &cfg);
    memset(&q, 0, sizeof q);
    memset(&q_rx, 0, sizeof q_rx);
    bt_mcp_session_bind(&se_tx, &fr_tx, q_emit, &q);
    bt_mcp_session_bind(&se_rx, &fr_rx, q_emit, &q_rx); /* 接收侧也需绑定 cap/清态 */
    bt_mcp_session_link_up(&se_tx, 0);
    bt_mcp_session_link_up(&se_rx, 0);
    bt_mcp_session_mark_ready(&se_tx);
    bt_mcp_session_mark_ready(&se_rx);
    g_msgs = 0;
}

/* 把队列里第 idx 片喂给 rx（走完整栈：framer → session_rx → 去重/ACK） */
static void feed_frag(int idx)
{
    bt_mcp_session_feed(&se_rx, q.buf + q.off[idx], q.len[idx], sink, NULL);
}

int main(void)
{
    static char msg[1024];
    memset(msg, 'A', sizeof msg);

    /* ---- 1. 正常路径：3 片全到 → 1 条消息 ---- */
    {
        setup(244);
        CHECK(bt_mcp_session_tx(&se_tx, (const uint8_t *)msg, 400) == 0, "tx ok");
        CHECK(q.n == 2, "400B / 238B → 2 fragments");
        for (int i = 0; i < q.n; i++) feed_frag(i);
        CHECK(g_msgs == 1, "all fragments → 1 message");
    }

    /* ---- 2. 丢中间片：NACK 登记 + 补发后仍能重组 ---- */
    {
        setup(244);
        bt_mcp_session_tx(&se_tx, (const uint8_t *)msg, 550);   /* 3 片 */
        CHECK(q.n == 3, "550B → 3 fragments");
        feed_frag(0);
        feed_frag(2);                       /* 跳过第 1 片 */
        CHECK(g_msgs == 0, "gap → no message yet");
        uint8_t nseq, nidx;
        CHECK(bt_mcp_framer_take_nack(&fr_rx, &nseq, &nidx) == 1,
              "NACK registered for missing chunk");
        CHECK(nidx == 1, "NACK points at missing index 1");
        feed_frag(1);                       /* 补发 */
        feed_frag(2);                       /* 重发末片 */
        CHECK(g_msgs == 1, "filled gap → message delivered");

        /* 发送侧能按 NACK 精确补发该片 */
        fq_t q2; memset(&q2, 0, sizeof q2);
        uint8_t seq = se_tx.txslot[0].seq;
        int rc = bt_mcp_framer_send_idx(&fr_tx, BT_MCP_KIND_MCP, seq, 1,
                                        (const uint8_t *)msg, 550, q_emit, &q2);
        CHECK(rc == 0 && q2.n == 1, "send_idx emits exactly one chunk");
        CHECK(q2.len[0] == 244, "full chunk = hdr + 238");
    }

    /* ---- 3. 重复片不污染槽位（迟到重片静默忽略） ---- */
    {
        setup(244);
        bt_mcp_session_tx(&se_tx, (const uint8_t *)msg, 400);
        feed_frag(0);
        feed_frag(0);                       /* 重片 */
        feed_frag(1);
        feed_frag(2);
        CHECK(g_msgs == 1, "duplicate chunk ignored, message intact");
    }

    /* ---- 4. 整条消息级去重：同 seq 同内容只上抛一次 ---- */
    {
        setup(244);
        bt_mcp_session_tx(&se_tx, (const uint8_t *)msg, 400);
        for (int i = 0; i < q.n; i++) feed_frag(i);
        for (int i = 0; i < q.n; i++) feed_frag(i);   /* 整条重放 */
        CHECK(g_msgs == 1, "whole-message replay deduped at L4");
    }

    /* ---- 5. 超时重传：tick 推进后自动重发同 seq ---- */
    {
        setup(244);
        /* 注：槽容量 512B，>512B 的消息按设计不享重传保护 */
        CHECK(bt_mcp_session_tx(&se_tx, (const uint8_t *)msg, 400) == 0, "tx 400");
        size_t before = q_bytes(&q);
        CHECK(se_tx.txslot[0].valid == 1, "slot registered for <=512B message");
        bt_mcp_session_tick(&se_tx, 1000);
        CHECK(q_bytes(&q) > before, "timeout retransmits whole message");
        CHECK(se_tx.stat_tx_retransmit >= 1, "retransmit counter");
        /* 重传次数到顶后放弃 */
        for (int i = 0; i < 10; i++) bt_mcp_session_tick(&se_tx, 1000 + (uint32_t)i * 1000);
        CHECK(se_tx.stat_tx_dropped >= 1, "gives up after retry_max");
    }

    /* ---- 6. ACK 释放重传槽，之后不再重传 ---- */
    {
        setup(244);
        bt_mcp_session_tx(&se_tx, (const uint8_t *)msg, 400);
        uint8_t ack[2] = { BT_MCP_CTRL_ACK, se_tx.txslot[0].seq };
        bt_mcp_session_rx(&se_tx, BT_MCP_KIND_CTRL, 0, ack, 2, NULL, NULL);
        size_t before = q_bytes(&q);
        bt_mcp_session_tick(&se_tx, 5000);
        CHECK(q_bytes(&q) == before, "ACKed message never retransmitted");
    }

    /* ---- 7. 槽满后补不齐 → 最终弃槽（不永久占死缓冲） ---- */
    {
        setup(53);                          /* chunk=47 → 600B 需 13 片 > MAX_FRAG */
        bt_mcp_session_tx(&se_tx, (const uint8_t *)msg, 400);   /* 9 片 */
        int n = q.n;
        CHECK(n >= 9, "many fragments");
        feed_frag(0);
        /* 反复喂越序片，让 gap_wait 增长到上限 */
        for (int i = 0; i < 10; i++) feed_frag(2);
        CHECK(g_msgs == 0, "never completed");
        /* 槽应已释放：再喂完整序列（从 0 起）能重新开始 */
        int slots_in_use = 0;
        for (int i = 0; i < fr_rx.cfg.max_reasm; i++)
            slots_in_use += fr_rx.reasm[i].in_use;
        CHECK(slots_in_use == 0, "abandoned slot freed");
    }

    /* ---- 8. 断链清空在途重组状态 ---- */
    {
        setup(244);
        bt_mcp_session_tx(&se_tx, (const uint8_t *)msg, 400);
        feed_frag(0);
        bt_mcp_session_link_down(&se_rx, 1);
        int in_use = 0;
        for (int i = 0; i < fr_rx.cfg.max_reasm; i++)
            in_use += fr_rx.reasm[i].in_use;
        CHECK(in_use == 0, "link_down clears half-reassembled frames");
    }

    printf("\n%d pass, %d fail\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
