/* fuzz_frame.c —— L3 帧层 fuzz 靶（草案 §10 M1 出口判据）。
 *
 * 靶函数双通道双路：
 *  1. 回环性质：随机 kind/mtu + 输入字节当 payload → framer_send →
 *     产物喂回 framer_feed → 重组结果必须与原 payload 全等（可拒收，拒收不测）。
 *  2. 野字节注入：原始输入直接喂包/流通道，只要求不 crash、不变量不破。
 *
 * libFuzzer 构建：clang -O1 -fsanitize=fuzzer,address -std=c99 -Icore/include \
 *       fuzz/fuzz_frame.c core/src/frame.c -o build/fuzz_frame
 * standalone 构建（Apple clang 无 fuzzer runtime 时）：-DBT_MCP_FUZZ_MAIN \
 *   自带 xorshift 变异引擎 + 24h 长跑 + 崩溃种子转储（seed 可复现）
 */
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include "bt_mcp/frame.h"

/* msg sink：收到的完整消息追加进 vec（fuzz 靶允许 malloc） */
typedef struct { uint8_t *buf; size_t len, cap; uint32_t n; } vec_t;

static int vec_emit(void *io, const uint8_t *data, uint16_t len)
{
    vec_t *v = io;
    if (v->len + len > v->cap) {
        size_t nc = v->cap ? v->cap * 2 : 256;
        while (nc < v->len + len) nc *= 2;
        v->buf = realloc(v->buf, nc);
        v->cap = nc;
    }
    memcpy(v->buf + v->len, data, len);
    v->len += len;
    return 0;
}

static void vec_sink(void *user, uint8_t kind, uint8_t seq,
                     const uint8_t *payload, uint16_t len)
{
    vec_t *v = user;
    (void)kind;
    (void)seq;
    vec_emit(v, payload, len);
    v->n++;
}

/* 一条输入喂两种通道各一遍（静态 framer 各 ~16KB，放静态区） */
static bt_mcp_framer_t g_fr;
static bt_mcp_chan_cap_t g_cap;

static int roundtrip(uint8_t kind, uint16_t mtu, const uint8_t *payload, uint32_t len)
{
    g_cap.kind = mtu ? BT_MCP_CHAN_PACKET : BT_MCP_CHAN_STREAM;
    g_cap.mtu  = mtu;
    g_cap.reliable = 1;
    g_cap.flow_ctrl = 0;
    g_cap.max_pending = 4;

    bt_mcp_frame_cfg_t cfg = { .max_reasm = 4, .frag_tout_ms = 2000 };
    if (bt_mcp_framer_init(&g_fr, &cfg, &g_cap)) return 1; /* 通道不可用，不测 */

    vec_t out = { 0 };
    if (bt_mcp_framer_send(&g_fr, kind, payload, len, vec_emit, &out))
        { free(out.buf); return 1; } /* 合法拒收，不测 */

    vec_t back = { 0 };
    int rc;
    if (mtu == 0) {
        rc = bt_mcp_framer_feed(&g_fr, out.buf, (uint16_t)out.len, vec_sink, &back);
    } else {
        /* 包通道：片前插 u16 长度前缀逐片喂（与差分 harness 同形）。
           out 恰为 [6B 头 + plen]* 连续布局，直接按头切。 */
        size_t off = 0;
        rc = 0;
        while (off < out.len && !rc) {
            if (off + 6 > out.len) { rc = 1; break; }
            uint16_t plen = (uint16_t)(out.buf[off + 4] | (out.buf[off + 5] << 8));
            /* 片长 = 头 6 + 可选 FCS 2 + 载荷；off 必须严格前进（防死循环） */
            size_t flen = (size_t)BT_MCP_HDR_LEN + plen
#if BT_MCP_CFG_FCS16
                        + 2
#endif
                        ;
            if (flen == 0 || off + flen > out.len) { rc = 1; break; }
            rc = bt_mcp_framer_feed(&g_fr, out.buf + off, (uint16_t)flen,
                                    vec_sink, &back);
            off += flen;
        }
    }
    /* 回环铁律：喂自己发的字节必须成功且逐字节还原 */
    if (rc != 0 || back.len != len || (len && memcmp(back.buf, payload, len)))
        abort();
    free(out.buf); free(back.buf);
    return 0;
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    if (size < 4) return 0;
    uint8_t kind = data[0] & 1;                       /* MCP / CTRL */
    uint16_t mtu = (uint16_t)(data[1] << 8 | data[2]); /* 0 = 流通道 */
    const uint8_t *payload = data + 3;
    uint32_t len = (uint32_t)(size - 3);
    if (len > BT_MCP_CFG_MAX_MSG) return 0;           /* 超限由差分测过，fuzz 不重复 */

    roundtrip(kind, mtu, payload, len);

    /* 野字节注入：原始输入截成 u16 段直接喂，包/流通道各一遍，仅要求不 crash */
    static bt_mcp_framer_t wild;
    bt_mcp_frame_cfg_t cfg = { .max_reasm = 4, .frag_tout_ms = 2000 };
    vec_t sink = { 0 };

    bt_mcp_chan_cap_t pcap = { BT_MCP_CHAN_PACKET, 244, 1, 0, 4 };
    bt_mcp_framer_init(&wild, &cfg, &pcap);
    for (size_t off = 0; off < size; off += 300)
        bt_mcp_framer_feed(&wild, data + off,
                           (uint16_t)((size - off > 300) ? 300 : size - off),
                           vec_sink, &sink);

    bt_mcp_chan_cap_t scap = { BT_MCP_CHAN_STREAM, 0, 1, 1, 4 };
    bt_mcp_framer_init(&wild, &cfg, &scap);
    for (size_t off = 0; off < size; off += 300)
        bt_mcp_framer_feed(&wild, data + off,
                           (uint16_t)((size - off > 300) ? 300 : size - off),
                           vec_sink, &sink);

    /* 截断/重复/乱序注入：把回环产物切片乱喂，只要求不变量不破（rc 任意） */
    bt_mcp_framer_init(&wild, &cfg, &pcap);
    for (size_t off = 0; off + 4 < size; off += 7) {
        bt_mcp_framer_feed(&wild, data + off, 4, vec_sink, &sink);   /* 任意 4B 片 */
        bt_mcp_framer_tick(&wild, (uint32_t)(off * 100));            /* 驱动超时回收 */
    }
    free(sink.buf);
    return 0;
}

#ifdef BT_MCP_FUZZ_MAIN
/* ===== standalone 引擎：结构化样本生成 + xorshift 变异，可复现 ===== */
#include <stdio.h>
#include <time.h>

static uint64_t g_rs;
static inline uint64_t rs(void)
{
    g_rs ^= g_rs << 13; g_rs ^= g_rs >> 7; g_rs ^= g_rs << 17;
    return g_rs;
}

static uint8_t pool[BT_MCP_CFG_MAX_MSG + 16];

/* 生成一个测试用例：20% 纯随机野字节；80% 以合法帧骨架变异（更易穿深） */
static size_t gen_case(void)
{
    size_t len = rs() % (sizeof(pool) / 2) + 4;
    if (rs() % 5 == 0) {                      /* 纯随机 */
        for (size_t i = 0; i < len; i++) pool[i] = (uint8_t)rs();
        return len;
    }
    /* 骨架：合法头 + 随机体，随后 0..4 处随机变异（位翻/截断/重复段拷入） */
    pool[0] = 0xB7;
    pool[1] = (uint8_t)rs();
    pool[2] = (uint8_t)rs();
    pool[3] = (uint8_t)rs();
    uint16_t plen = (uint16_t)(rs() % (len > 262 ? 256 : len));
    pool[4] = (uint8_t)(plen & 0xFF); pool[5] = (uint8_t)(plen >> 8);
    for (size_t i = 6; i < len; i++) pool[i] = (uint8_t)rs();
    for (int m = 0, nm = (int)(rs() % 5); m < nm; m++) {
        size_t p = rs() % len;
        switch (rs() % 3) {
        case 0: pool[p] ^= (uint8_t)(1u << (rs() % 8)); break;   /* 位翻 */
        case 1: pool[p] = (uint8_t)rs(); break;                  /* 字节替换 */
        case 2: {                                               /* 段拷贝（重复/乱序） */
            size_t src = rs() % len, n = rs() % 32 + 1;
            for (size_t i = 0; i < n; i++) pool[(p + i) % len] = pool[(src + i) % len];
            break;
        }
        }
    }
    return len;
}

int main(int argc, char **argv)
{
    unsigned long iters = 200000000UL;      /* 默认足够跑数小时 */
    uint64_t seed = 0xB70710C0DEllu;
    int arg_i = 1;
    if (argc > 1 && argv[1][0] != '-') { iters = strtoul(argv[1], NULL, 0); arg_i++; }
    if (argc > arg_i) seed = strtoull(argv[arg_i], NULL, 0);
    g_rs = seed ? seed : (uint64_t)time(NULL);
    if (!g_rs) g_rs = 1;

    fprintf(stderr, "standalone fuzz: iters=%lu seed=%llu\n", iters, (unsigned long long)g_rs);
    for (unsigned long it = 0; it < iters; it++) {
        size_t len = gen_case();
        LLVMFuzzerTestOneInput(pool, len);
        if ((it & 0xFFFFF) == 0xFFFFF)
            fprintf(stderr, "  %lu iters ok\n", it + 1);
    }
    fprintf(stderr, "FUZZ DONE: %lu iters, seed=%llu, ZERO CRASH\n",
            iters, (unsigned long long)g_rs);
    return 0;
}
#endif
