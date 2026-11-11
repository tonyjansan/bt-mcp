/* test_fcs16.c —— FCS16（CRC16-CCITT）编解码往返测试。
 *
 * 为什么单独测：FCS16 是「通道无链路级校验」时才开的可选模式（自定义 UART），
 * 默认差分矩阵跑的是 FCS16=0，所以它坏了差分发现不了（v0.3 重构就踩过这个坑：
 * 发送侧改成 hdr|payload|FCS、接收侧仍按 hdr|FCS|payload 解析）。
 *
 * 本测试用 -DBT_MCP_CFG_FCS16=1 编译，覆盖：
 *   - 包通道：单帧 / 多帧（含 255B 满片）
 *   - 流通道：小帧（≤255，一次 emit）/ 大帧（>255，分两次 emit）
 *   - 校验错误必须被拒绝（单 bit 翻转）
 *
 * 编：cc -O2 -std=c99 -Wall -Wextra -Werror -DBT_MCP_CFG_FCS16=1 \
 *        -Icore/include tests/test_fcs16.c core/src/frame.c -o build/test_fcs16
 */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "bt_mcp/frame.h"
#include "bt_mcp/err.h"

#if !BT_MCP_CFG_FCS16
#error "test_fcs16.c must be built with -DBT_MCP_CFG_FCS16=1"
#endif

static int g_pass, g_fail;
#define CHECK(c, m) do { if (c) g_pass++; else { g_fail++; \
    printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, m); } } while (0)

typedef struct { uint8_t buf[16384]; size_t len; uint16_t frag[256]; int nfrag; } q_t;

static int q_emit(void *io, const uint8_t *d, uint16_t n)
{
    q_t *q = io;
    if (q->len + n > sizeof q->buf || q->nfrag >= 256) return BT_MCP_E_NOMEM;
    memcpy(q->buf + q->len, d, n);
    q->frag[q->nfrag++] = n;
    q->len += n;
    return 0;
}

static int g_msgs;
static uint8_t g_last[8192];
static uint16_t g_last_len;
static void sink(void *u, uint8_t kind, uint8_t seq,
                 const uint8_t *p, uint16_t n)
{
    (void)u; (void)kind; (void)seq;
    g_msgs++;
    if (n <= sizeof g_last) { memcpy(g_last, p, n); g_last_len = n; }
}

static bt_mcp_framer_t tx, rx;

static void setup(int packet, uint16_t mtu)
{
    static bt_mcp_chan_cap_t cap;
    static bt_mcp_frame_cfg_t cfg;
    cap.kind = packet ? BT_MCP_CHAN_PACKET : BT_MCP_CHAN_STREAM;
    cap.mtu = mtu;
    cap.reliable = packet ? 0 : 1;
    cap.flow_ctrl = packet ? 0 : 1;
    cap.max_pending = 4;
    cfg.max_reasm = 4;
    cfg.frag_tout_ms = 2000;
    bt_mcp_framer_init(&tx, &cfg, &cap);
    bt_mcp_framer_init(&rx, &cfg, &cap);
    g_msgs = 0;
    g_last_len = 0;
}

/* 包通道：逐片喂；流通道：整段喂 */
static int deliver(const q_t *q, int packet)
{
    if (!packet) return bt_mcp_framer_feed(&rx, q->buf, (uint16_t)q->len, sink, NULL);
    size_t off = 0;
    for (int i = 0; i < q->nfrag; i++) {
        int rc = bt_mcp_framer_feed(&rx, q->buf + off, q->frag[i], sink, NULL);
        off += q->frag[i];
        if (rc) return rc;
    }
    return BT_MCP_OK;
}

int main(void)
{
    static uint8_t msg[8192];

    /* ---- 包通道往返 ---- */
    {
        static const uint16_t mtus[] = { 20, 23, 100, 244, 261 };
        static const size_t lens[] = { 0, 1, 17, 238, 255, 512, 1024, 4000 };
        for (size_t mi = 0; mi < sizeof mtus / sizeof mtus[0]; mi++) {
            for (size_t li = 0; li < sizeof lens / sizeof lens[0]; li++) {
                setup(1, mtus[mi]);
                for (size_t i = 0; i < lens[li]; i++) msg[i] = (uint8_t)(i * 31 + 7);
                q_t q; memset(&q, 0, sizeof q);
                int rc = bt_mcp_framer_send(&tx, BT_MCP_KIND_MCP, msg,
                                            (uint32_t)lens[li], q_emit, &q);
                if (rc) {
                    /* 小 MTU 下分片数超 u8 idx 上限（255）→ 必须明确 E_TOOBIG */
                    size_t chunk = (size_t)mtus[mi] - BT_MCP_HDR_LEN - 2;
                    size_t nfrag = (lens[li] + chunk - 1) / chunk;
                    char m[96];
                    snprintf(m, sizeof m, "packet mtu=%u len=%zu → E_TOOBIG expected (nfrag=%zu)",
                             mtus[mi], lens[li], nfrag);
                    CHECK(rc == BT_MCP_E_TOOBIG && nfrag > 255, m);
                    continue;
                }
                rc = deliver(&q, 1);
                char m[96];
                snprintf(m, sizeof m, "packet mtu=%u len=%zu rc=%d",
                         mtus[mi], lens[li], rc);
                CHECK(rc == 0 && g_msgs == 1 && g_last_len == lens[li]
                      && !memcmp(g_last, msg, lens[li]), m);
            }
        }
    }

    /* ---- 流通道往返（含 >255B 的两段 emit） ---- */
    {
        static const size_t lens[] = { 0, 1, 100, 255, 256, 512, 1024, 8192 };
        for (size_t li = 0; li < sizeof lens / sizeof lens[0]; li++) {
            setup(0, 0);
            for (size_t i = 0; i < lens[li]; i++) msg[i] = (uint8_t)(i * 17 + 3);
            q_t q; memset(&q, 0, sizeof q);
            int rc = bt_mcp_framer_send(&tx, BT_MCP_KIND_MCP, msg,
                                        (uint32_t)lens[li], q_emit, &q);
            if (rc) { CHECK(0, "stream send failed"); continue; }
            rc = deliver(&q, 0);
            char m[96];
            snprintf(m, sizeof m, "stream len=%zu rc=%d msgs=%d", lens[li], rc, g_msgs);
            CHECK(rc == 0 && g_msgs == 1 && g_last_len == lens[li]
                  && !memcmp(g_last, msg, lens[li]), m);
        }
    }

    /* ---- 校验失败必须拒绝（包通道 / 流通道各一） ---- */
    {
        size_t n = 300;
        for (size_t i = 0; i < n; i++) msg[i] = (uint8_t)i;

        /* 包通道：翻转载荷一位 */
        setup(1, 244);
        q_t q; memset(&q, 0, sizeof q);
        bt_mcp_framer_send(&tx, BT_MCP_KIND_MCP, msg, (uint32_t)n, q_emit, &q);
        q.buf[BT_MCP_HDR_LEN + 2 + 5] ^= 0x40;      /* hdr|fcs|payload 的载荷区 */
        g_msgs = 0;
        int rc = deliver(&q, 1);
        CHECK(rc != 0 || g_msgs == 0, "packet: corrupted FCS rejected");

        /* 流通道：翻转载荷一位 */
        setup(0, 0);
        memset(&q, 0, sizeof q);
        bt_mcp_framer_send(&tx, BT_MCP_KIND_MCP, msg, (uint32_t)n, q_emit, &q);
        q.buf[BT_MCP_HDR_LEN + 2 + 7] ^= 0x01;
        g_msgs = 0;
        rc = bt_mcp_framer_feed(&rx, q.buf, (uint16_t)q.len, sink, NULL);
        CHECK(rc != 0 && g_msgs == 0, "stream: corrupted FCS rejected");
    }

    /* ---- 流通道坏帧后能自恢复（不卡死） ---- */
    {
        setup(0, 0);
        uint8_t junk[6] = { BT_MCP_MAGIC, 0x02, 1, 0, 0, 0 };  /* CHUNKED 违规 */
        bt_mcp_framer_feed(&rx, junk, 6, sink, NULL);
        for (size_t i = 0; i < 50; i++) msg[i] = (uint8_t)i;
        q_t q; memset(&q, 0, sizeof q);
        bt_mcp_framer_send(&tx, BT_MCP_KIND_MCP, msg, 50, q_emit, &q);
        g_msgs = 0;
        int rc = bt_mcp_framer_feed(&rx, q.buf, (uint16_t)q.len, sink, NULL);
        CHECK(rc == 0 && g_msgs == 1, "stream recovers after bad frame");
    }

    printf("\n%d pass, %d fail\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
