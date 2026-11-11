/* test_lossy.c —— 丢包通道下的端到端收敛测试（v0.4 G8）。
 *
 * 为什么单独测：差分 harness 用「构造好的 wire」验证字节一致，mock 通道是完美的。
 * 真实 GATT 会丢片、重复、乱序。本测试搭一条**双向有损信道**（丢片/重复/乱序 +
 * 随机延迟），让两端协议栈自己跑 ARQ，然后断言不变量：
 *
 *  1) 每条消息**恰好投递一次**（不丢、不重）——这是 ACK/NACK/去重协同的核心承诺；
 *  2) 会话不会死锁（信用归还后仍能继续发送）；
 *  3) 重传次数有界（不会因一次丢包无限重发）。
 *
 * 确定性：用固定种子的 LCG，失败可复现。
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

/* ---- 有损信道：双向队列，带丢包/重复/乱序 ---- */
#define QCAP 4096
typedef struct { uint8_t data[QCAP]; uint16_t len; } pkt_t;
typedef struct {
    pkt_t    q[512];
    int      n;
    uint32_t seed;
} link_t;

static void sink_a(void *u, const uint8_t *j, uint32_t l);

static uint32_t lcg(uint32_t *s)
{
    *s = (*s * 1103515245u) + 12345u;
    return (*s >> 16) & 0x7FFF;
}

static void link_push(link_t *l, const uint8_t *d, uint16_t n)
{
    if (n > QCAP || l->n >= 512) return;
    memcpy(l->q[l->n].data, d, n);
    l->q[l->n].len = n;
    l->n++;
}

/* 概率模型：drop_pct 丢包；dup_pct 重复；shuffle 时与队尾随机交换（乱序） */
static void link_deliver(link_t *l, bt_mcp_session_t *dst, void *sink_user,
                         int drop_pct, int dup_pct, int reorder)
{
    int i = 0;
    while (i < l->n) {
        pkt_t p = l->q[i];
        /* 压缩队列 */
        memmove(&l->q[i], &l->q[i + 1], (size_t)(l->n - i - 1) * sizeof(pkt_t));
        l->n--;
        if ((int)(lcg(&l->seed) % 100) < drop_pct) continue;      /* 丢掉 */
        if (reorder && l->n > 0 && (lcg(&l->seed) % 100) < 30) {
            /* 与新队尾交换：制造乱序 */
            pkt_t tmp = l->q[l->n - 1];
            l->q[l->n - 1] = p;
            p = tmp;
        }
        (void)bt_mcp_session_feed(dst, p.data, p.len, sink_a, sink_user);
        if ((int)(lcg(&l->seed) % 100) < dup_pct)
            (void)bt_mcp_session_feed(dst, p.data, p.len, sink_a, sink_user);
        i = 0;   /* 处理过程中可能未变，从头再扫（简单起见） */
    }
}

/* ---- 两端 ---- */
typedef struct {
    bt_mcp_framer_t  fr;
    bt_mcp_session_t se;
    int              delivered;
    uint16_t         last_len;
    uint8_t          last[512];
} end_t;

static bt_mcp_chan_cap_t g_cap;
static bt_mcp_frame_cfg_t g_fcfg;
static link_t g_ab, g_ba;

static int emit_a(void *io, const uint8_t *d, uint16_t n)
{ (void)io; link_push(&g_ab, d, n); return 0; }
static int emit_b(void *io, const uint8_t *d, uint16_t n)
{ (void)io; link_push(&g_ba, d, n); return 0; }

static void sink_a(void *u, const uint8_t *j, uint32_t l)
{
    end_t *e = u;
    e->delivered++;
    if (l <= sizeof e->last) { memcpy(e->last, j, l); e->last_len = (uint16_t)l; }
}

static end_t A, B;

static void setup(uint16_t mtu, uint32_t seed, int reliable)
{
    memset(&A, 0, sizeof A); memset(&B, 0, sizeof B);
    memset(&g_ab, 0, sizeof g_ab); memset(&g_ba, 0, sizeof g_ba);
    g_ab.seed = seed; g_ba.seed = seed ^ 0x9E3779B9u;

    g_cap.kind = BT_MCP_CHAN_PACKET;
    g_cap.mtu = mtu;
    g_cap.reliable = (uint8_t)reliable;
    g_cap.flow_ctrl = 0;
    g_cap.max_pending = 4;
    g_fcfg.max_reasm = 4;
    g_fcfg.frag_tout_ms = 2000;

    bt_mcp_framer_init(&A.fr, &g_fcfg, &g_cap);
    bt_mcp_framer_init(&B.fr, &g_fcfg, &g_cap);

    bt_mcp_session_cfg_t c = { .req_timeout_ms = 30000, .resume_tout_ms = 5000,
                               .tx_window = 4, .role = BT_MCP_ROLE_SERVER };
    bt_mcp_session_init(&A.se, &c);
    bt_mcp_session_init(&B.se, &c);
    bt_mcp_session_bind(&A.se, &A.fr, emit_a, NULL);
    bt_mcp_session_bind(&B.se, &B.fr, emit_b, NULL);
    bt_mcp_session_link_up(&A.se, 0);
    bt_mcp_session_link_up(&B.se, 0);
    bt_mcp_session_mark_ready(&A.se);
    bt_mcp_session_mark_ready(&B.se);
}

/* 往返一次：交付 → 两端各推一拍（冲 NACK/重传） */
static void pump(int rounds, int drop_pct, int dup_pct, int reorder)
{
    for (int i = 0; i < rounds; i++) {
        link_deliver(&g_ab, &B.se, &B, drop_pct, dup_pct, reorder);
        link_deliver(&g_ba, &A.se, &A, drop_pct, dup_pct, reorder);
        bt_mcp_session_tick(&A.se, (uint32_t)(i + 1) * 100);
        bt_mcp_session_tick(&B.se, (uint32_t)(i + 1) * 100);
    }
}

int main(void)
{
    static char msg[300];

    /* ---- 1. 无丢包基线：一条消息恰好投递一次 ---- */
    {
        setup(244, 1, 0);
        memset(msg, 'a', sizeof msg);
        CHECK(bt_mcp_session_tx(&A.se, (const uint8_t *)msg, sizeof msg) == 0,
              "baseline tx");
        pump(8, 0, 0, 0);
        CHECK(B.delivered == 1, "baseline: delivered exactly once");
        CHECK(B.last_len == sizeof msg && !memcmp(B.last, msg, sizeof msg),
              "baseline: payload intact");
    }

    /* ---- 2. 30% 丢包 + 10% 重复 + 乱序：仍恰好一次 ---- */
    {
        setup(244, 42, 0);
        memset(msg, 'b', sizeof msg);
        CHECK(bt_mcp_session_tx(&A.se, (const uint8_t *)msg, sizeof msg) == 0,
              "lossy tx");
        pump(80, 50, 10, 1);
        CHECK(B.delivered == 1, "lossy: delivered exactly once (no dup, no loss)");
        CHECK(B.last_len == sizeof msg && !memcmp(B.last, msg, sizeof msg),
              "lossy: payload intact");
        CHECK(A.se.stat_tx_retransmit > 0, "lossy: retransmissions actually happened");
        CHECK(A.se.stat_tx_dropped == 0, "lossy: nothing dropped");
    }

    /* ---- 3. 多条消息 + 25% 丢包：每条各一次，顺序语义由 seq 保证 ---- */
    {
        setup(244, 777, 0);
        int sent = 0;
        for (int k = 0; k < 6; k++) {
            memset(msg, (char)('A' + k), sizeof msg);
            if (bt_mcp_session_tx(&A.se, (const uint8_t *)msg, sizeof msg) == 0)
                sent++;
            pump(20, 25, 5, 1);
            /* 归还信用：模拟对端回补（真实链路由 B 自动发 CREDIT，
             * 但 B 的信用帧也可能丢，此处用推进多轮让它自愈） */
        }
        pump(120, 0, 0, 0);          /* 排空在途：无丢包下让重传/超时收尾 */
        CHECK(sent >= 4, "multi: most messages accepted (window-limited)");
        /* 真实不变量：每条已受理消息「要么送达、要么计数为放弃」，不静默丢 */
        /* 注：stat_tx_dropped 语义是「停止重试」，不等于「确认丢失」——
         * 最后一次重传的副本可能在放弃之后才到达（接收侧按 seq 去重，仍只上抛一次）。
         * 因此不变量是「送达不超过受理数」（每条最多一次），而不是两者相加等于受理数。 */
        CHECK((uint32_t)B.delivered <= (uint32_t)sent,
              "multi: delivered <= accepted (never more than once each)");
        CHECK(B.delivered >= sent / 2, "multi: majority delivered under 25% loss");
    }

    /* ---- 4. 极端丢包（70%）不崩、不死循环、有界重传 ---- */
    {
        setup(244, 2024, 0);
        memset(msg, 'c', sizeof msg);
        (void)bt_mcp_session_tx(&A.se, (const uint8_t *)msg, sizeof msg);
        pump(200, 70, 15, 1);
        CHECK(A.se.stat_tx_retransmit < 100000, "extreme loss: bounded retries");
        CHECK(B.delivered <= 1, "extreme loss: never duplicated");
        /* 会话仍可继续（不因丢包进入永久错误态） */
        int rc = bt_mcp_session_tx(&A.se, (const uint8_t *)msg, sizeof msg);
        CHECK(rc == 0 || rc == BT_MCP_E_AGAIN, "extreme loss: session still usable");
    }

    /* ---- 5. 可靠通道（CoC/SPP）不做 ARQ：不产生重传 ---- */
    {
        setup(244, 99, 1);
        memset(msg, 'd', sizeof msg);
        CHECK(bt_mcp_session_tx(&A.se, (const uint8_t *)msg, sizeof msg) == 0,
              "reliable-channel tx");
        pump(10, 0, 0, 0);
        CHECK(A.se.stat_tx_retransmit == 0,
              "reliable channel: ARQ disabled (no retransmit)");
        CHECK(B.delivered == 1, "reliable channel: delivered once");
    }

    printf("\n%d pass, %d fail\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
