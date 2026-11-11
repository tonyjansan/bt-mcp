/* bench.c —— 吞吐/开销基线（草案 §10「吞吐与功耗基线」的协议侧部分）。
 *
 * 测三件事：
 *  1. 帧开销比：不同 MTU 下，一条 N 字节 MCP 消息实际占用多少线缆字节；
 *  2. 分片成本：小 MTU 的分片放大系数；
 *  3. 可靠性开销：不可靠包通道上 ACK/CREDIT 带来的额外帧占比。
 *
 * 全部用同一份 core（framer/session）真实编码，非估算。
 *
 * 编：cc -O2 -std=c99 -Wall -Wextra -Werror -Icore/include \
 *        tools/bench.c core/src/frame.c core/src/session.c -o build/bench && ./build/bench
 */
#include <stdio.h>
#include <string.h>

#include "bt_mcp/frame.h"
#include "bt_mcp/session.h"
#include "bt_mcp/err.h"

typedef struct { size_t len; size_t frames; } counter_t;

static int count_emit(void *io, const uint8_t *d, uint16_t n)
{
    (void)d;
    counter_t *c = io;
    c->len += n;
    c->frames++;
    return 0;
}

static bt_mcp_framer_t g_fr;
static uint8_t g_payload[BT_MCP_CFG_MAX_MSG];

/* 返回线缆字节数 + 帧数 */
static void measure(uint16_t mtu, int packet, size_t msg_len,
                    counter_t *out)
{
    bt_mcp_chan_cap_t cap = { .kind = packet ? BT_MCP_CHAN_PACKET : BT_MCP_CHAN_STREAM,
                              .mtu = mtu,
                              .reliable = packet ? 0 : 1,
                              .flow_ctrl = packet ? 0 : 1,
                              .max_pending = 4 };
    memset(out, 0, sizeof *out);
    memset(g_payload, 'x', msg_len);
    if (bt_mcp_framer_init(&g_fr, NULL, &cap)) return;
    bt_mcp_framer_send(&g_fr, BT_MCP_KIND_MCP, g_payload, (uint32_t)msg_len,
                       count_emit, out);
}

/* 可靠性开销：一条请求 + 一条响应在不可靠包通道上的控制帧占比 */
static size_t g_ctrl_bytes, g_ctrl_frames;

static void measure_reliability_overhead(uint16_t mtu, size_t req_len,
                                         size_t resp_len, size_t *data_bytes,
                                         size_t *data_frames)
{
    static bt_mcp_framer_t fr;
    static bt_mcp_session_t se;
    bt_mcp_chan_cap_t cap = { .kind = BT_MCP_CHAN_PACKET, .mtu = mtu,
                              .reliable = 0, .flow_ctrl = 0, .max_pending = 4 };
    bt_mcp_session_cfg_t cfg = { .req_timeout_ms = 30000, .resume_tout_ms = 5000,
                                 .tx_window = 4, .role = BT_MCP_ROLE_SERVER };
    bt_mcp_framer_init(&fr, NULL, &cap);
    bt_mcp_session_init(&se, &cfg);
    bt_mcp_session_bind(&se, &fr, count_emit, &(counter_t){0});
    /* 上面 bind 的 io 用完即弃；这里重新绑定到统计器 */
    static counter_t data;
    memset(&data, 0, sizeof data);
    bt_mcp_session_bind(&se, &fr, count_emit, &data);
    bt_mcp_session_link_up(&se, 0);
    bt_mcp_session_mark_ready(&se);

    /* 本地消费对端一条消息 → 产生 CREDIT + ACK（这两帧是可靠性开销） */
    static uint8_t req[2048];
    memset(req, 'q', req_len);
    bt_mcp_session_rx(&se, BT_MCP_KIND_MCP, 1, req, (uint16_t)req_len,
                      NULL, NULL);
    g_ctrl_bytes = data.len;
    g_ctrl_frames = data.frames;

    /* 本端发一条响应 → 数据帧 */
    data.len = 0; data.frames = 0;
    memset(g_payload, 'r', resp_len);
    bt_mcp_session_tx(&se, g_payload, (uint32_t)resp_len);

    *data_bytes = data.len;
    *data_frames = data.frames;
}

int main(void)
{
    static const uint16_t mtus[] = { 23, 53, 100, 185, 244, 261 };
    const size_t msg = 512;

    printf("== 分片开销：%zuB MCP 消息（包通道）==\n", msg);
    printf("%-6s %-8s %-8s %-8s %-8s\n", "MTU", "片数", "线缆B", "开销%", "放大x");
    for (size_t i = 0; i < sizeof mtus / sizeof mtus[0]; i++) {
        counter_t c;
        measure(mtus[i], 1, msg, &c);
        printf("%-6u %-8zu %-8zu %-8.1f %-8.2f\n", mtus[i], c.frames, c.len,
               100.0 * (double)(c.len - msg) / (double)msg,
               (double)c.len / (double)msg);
    }

    printf("\n== 流通道（SPP/UART）：整条消息单帧，固定 +6B ==\n");
    printf("（>261B 的内部实现会拆成「头+载荷」两次 emit，字节流下等价）\n");
    printf("%-8s %-10s %-8s\n", "消息B", "emit次数", "线缆B");
    for (size_t len = 0; len <= 4096; len = len ? len * 2 : 64) {
        counter_t c;
        measure(0, 0, len, &c);
        printf("%-8zu %-10zu %-8zu（+%zu）\n", len, c.frames, c.len, c.len - len);
    }

    printf("\n== 可靠性控制帧开销（不可靠包通道，MTU=244）==\n");
    {
        size_t db = 0, df = 0;
        measure_reliability_overhead(244, 128, 512, &db, &df);
        printf("场景：消费 128B 请求 + 发 512B 响应\n");
        printf("  数据帧: %zu 帧 / %zuB\n", df, db);
        printf("  控制帧: %zu 帧 / %zuB（CREDIT+ACK）\n", g_ctrl_frames, g_ctrl_bytes);
        printf("  控制开销占比: %.1f%%\n",
               100.0 * (double)g_ctrl_bytes / (double)(db + g_ctrl_bytes));
    }

    printf("\n== 典型 MCP 消息规模下的每消息开销 ==\n");
    printf("%-10s %-10s %-10s\n", "消息B", "线缆B(MTU244)", "开销");
    static const size_t sizes[] = { 32, 64, 128, 256, 512, 1024, 2048, 4096 };
    for (size_t i = 0; i < sizeof sizes / sizeof sizes[0]; i++) {
        counter_t c;
        measure(244, 1, sizes[i], &c);
        printf("%-10zu %-10zu %+.1f%%\n", sizes[i], c.len,
               100.0 * (double)(c.len - sizes[i]) / (double)sizes[i]);
    }
    return 0;
}
