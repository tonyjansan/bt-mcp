/* probe_verify.c — independent probes for the v0.3 fix verification.
 * cc -O2 -std=c99 -Wall -Wextra -Werror -Icore/include -Iport \
 *    tests/probe_verify.c core sources -o build/probe_verify
 * FCS variant: add -DBT_MCP_CFG_FCS16=1
 */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "bt_mcp/frame.h"
#include "bt_mcp/session.h"
#include "bt_mcp/mcp_server.h"
#include "bt_mcp/mcp_client.h"
#include "bt_mcp/err.h"

static int g_pass, g_fail;
#define CHECK(c, m) do { if (c) g_pass++; else { g_fail++; \
    printf("FAIL line %d: %s\n", __LINE__, m); } } while (0)

/* ---------- stream harness ---------- */
typedef struct { uint8_t b[1 << 14]; size_t n; } vsink_t;
static int vemit(void *io, const uint8_t *d, uint16_t n)
{
    vsink_t *v = io;
    if (n && v->n + n <= sizeof v->b) { memcpy(v->b + v->n, d, n); v->n += n; }
    return 0;
}
static bt_mcp_chan_cap_t cap_stream = { BT_MCP_CHAN_STREAM, 0, 1, 1, 4 };

static void stream_setup(bt_mcp_framer_t *f, vsink_t *v, bt_mcp_session_t *s)
{
    bt_mcp_frame_cfg_t fc = { 0, 2000 };
    bt_mcp_framer_init(f, &fc, &cap_stream);
    memset(v, 0, sizeof *v);
    bt_mcp_session_cfg_t cfg = { 30000, 5000, 4, BT_MCP_ROLE_SERVER, 3, 600 };
    bt_mcp_session_init(s, &cfg);
    bt_mcp_session_bind(s, f, vemit, v);
    bt_mcp_session_link_up(s, 0);
    bt_mcp_session_mark_ready(s);
}

static int g_msgs;
static char g_last[9000]; static uint32_t g_last_len;
static void sink(void *u, const uint8_t *json, uint32_t len)
{ (void)u; g_msgs++; g_last_len = len < sizeof g_last ? len : sizeof g_last - 1;
  memcpy(g_last, json, g_last_len); g_last[g_last_len] = 0; }

/* L3 sink wrapper: framer → session_rx → sink */
static void fr_sink(void *user, uint8_t kind, uint8_t seq,
                    const uint8_t *payload, uint16_t len)
{
    bt_mcp_session_t *s = user;
    bt_mcp_session_rx(s, kind, seq, payload, len, sink, NULL);
}

/* ---------- A: bad frame mid-feed must not swallow following frames ---------- */
static void probeA(void)
{
    bt_mcp_framer_t f; vsink_t v; bt_mcp_session_t s;
    stream_setup(&f, &v, &s);
    const char *g1 = "{\"m\":1}", *g2 = "{\"m\":2}";
    uint8_t all[512]; size_t n = 0;

    bt_mcp_session_tx(&s, (const uint8_t *)g1, strlen(g1));
    memcpy(all, v.b, v.n); n = v.n;

    /* bad frame: stream-forbidden CHUNK flag, valid magic + plausible len */
    uint8_t bad[14] = { 0xB7, BT_MCP_FLAG_CHUNK, 0x09, 0, 0x05, 0x00, 'x','x','x','x','x' };
    size_t bl = 11;
    memcpy(all + n, bad, bl); n += bl;

    bt_mcp_session_tx(&s, (const uint8_t *)g2, strlen(g2));
    memcpy(all + n, v.b, v.n); n += v.n;

    g_msgs = 0;
    int rc = bt_mcp_framer_feed(&f, all, (uint16_t)n, fr_sink, &s);
    CHECK(rc != 0, "A: bad frame reports E_SYNC");
    CHECK(g_msgs == 2, "A: both good frames parsed despite bad frame between them");

    /* A2: desync must not wedge parser: frames in a NEW feed still parse */
    g_msgs = 0;
    bt_mcp_framer_feed(&f, v.b, (uint16_t)v.n, fr_sink, &s);  /* g1+g2 bytes again */
    CHECK(g_msgs == 2, "A2: parser not wedged after desync");
}

/* ---------- B: FCS16 stream round-trip incl >255B (two emits) ---------- */
static void probeB(void)
{
    bt_mcp_framer_t f; vsink_t v; bt_mcp_session_t s;
    stream_setup(&f, &v, &s);
    static char big[900]; memset(big, 'Q', sizeof big);
    for (int i = 0; i < 700; i++) big[i] = (char)('a' + (i % 26));
    g_msgs = 0;
    int rc = bt_mcp_session_tx(&s, (const uint8_t *)big, 700);
    CHECK(rc == 0, "B: 700B stream tx ok");
    rc = bt_mcp_framer_feed(&f, v.b, (uint16_t)v.n, fr_sink, &s);
    CHECK(rc == 0, "B: 700B stream frame feeds clean");
    CHECK(g_msgs == 1, "B: 700B message delivered");
    CHECK(g_last_len == 700 && !memcmp(g_last, big, 700), "B: payload intact");

    /* corrupt one payload byte → rejected (only meaningful with FCS16;
     * non-FCS has no integrity check by design — link layer assumed) */
    v.b[40] ^= 0x55;
    g_msgs = 0;
    rc = bt_mcp_framer_feed(&f, v.b, (uint16_t)v.n, fr_sink, &s);
#if BT_MCP_CFG_FCS16
    CHECK(rc != 0, "B: corrupted frame rejected");
    CHECK(g_msgs == 0, "B: corrupted frame not delivered");
#else
    CHECK(g_msgs == 1, "B(non-FCS): no link-level check by design");
#endif

    /* B2: framer_reset clears FCS state mid-frame — use a FRESH clean frame */
    size_t vn0 = v.n;
    bt_mcp_session_tx(&s, (const uint8_t *)big, 700);   /* fresh clean frame */
    uint8_t partial[16]; memcpy(partial, v.b + vn0, 16);
    bt_mcp_framer_feed(&f, partial, 16, fr_sink, &s);   /* partial header+fcs+payload */
    bt_mcp_framer_reset(&f);
    g_msgs = 0;
    bt_mcp_framer_feed(&f, v.b + vn0, (uint16_t)(v.n - vn0), fr_sink, &s);
    CHECK(g_msgs == 1, "B2: framer_reset clears FCS stream state");
}

/* ---------- C: packet channel FCS round trip ---------- */
static void probeC(void)
{
    static bt_mcp_chan_cap_t cap = { BT_MCP_CHAN_PACKET, 53, 0, 0, 4 }; /* chunk=45-2 */
    bt_mcp_framer_t f; vsink_t v; bt_mcp_session_t s;
    bt_mcp_frame_cfg_t fc = { 4, 2000 };
    bt_mcp_framer_init(&f, &fc, &cap);
    memset(&v, 0, sizeof v);
    bt_mcp_session_cfg_t cfg = { 30000, 5000, 4, 0, 3, 600 };
    bt_mcp_session_init(&s, &cfg);
    bt_mcp_session_bind(&s, &f, vemit, &v);
    bt_mcp_session_link_up(&s, 0);
    bt_mcp_session_mark_ready(&s);
    static char msg[200]; memset(msg, 'k', sizeof msg);
    g_msgs = 0;
    CHECK(bt_mcp_session_tx(&s, (const uint8_t *)msg, 200) == 0, "C: tx ok");
    g_msgs = 0;
    /* feed each captured emit as one notify */
    size_t off = 0;
    while (off < v.n) {
        uint8_t hdr[6]; memcpy(hdr, v.b + off, 6);
        uint16_t plen = (uint16_t)(hdr[4] | (hdr[5] << 8));
#if BT_MCP_CFG_FCS16
        uint16_t flen = (uint16_t)(6 + 2 + plen);
#else
        uint16_t flen = (uint16_t)(6 + plen);
#endif
        bt_mcp_framer_feed(&f, v.b + off, flen, fr_sink, &s);
        off += flen;
    }
    CHECK(g_msgs == 1, "C: fragmented packet msg reassembled with FCS");
}

/* ---------- D: credit tail-loss deadlock probe ---------- */
/* Scenario: sender (client) & receiver (server) over unreliable packet channel.
 * Receiver consumes request; its CREDIT is lost; ACK arrives.
 * Sender is left at 0 credits with nothing in flight. No new traffic can flow. */
static void probeD(void)
{
    static bt_mcp_chan_cap_t cap = { BT_MCP_CHAN_PACKET, 244, 0, 0, 4 };
    bt_mcp_framer_t ftx, frx; vsink_t v; /* v = tx->wire */
    bt_mcp_session_t stx, srx;
    bt_mcp_frame_cfg_t fc = { 4, 2000 };
    bt_mcp_framer_init(&ftx, &fc, &cap);
    bt_mcp_framer_init(&frx, &fc, &cap);
    memset(&v, 0, sizeof v);
    bt_mcp_session_cfg_t cfg = { 30000, 5000, 1, 0, 3, 600 };
    bt_mcp_session_init(&stx, &cfg);
    bt_mcp_session_init(&srx, &cfg);
    bt_mcp_session_bind(&stx, &ftx, vemit, &v);
    bt_mcp_session_bind(&srx, &frx, vemit, &(vsink_t){0}); /* placeholder rebind below */
    static vsink_t vr;
    memset(&vr, 0, sizeof vr);
    bt_mcp_session_bind(&srx, &frx, vemit, &vr);
    bt_mcp_session_link_up(&stx, 0);
    bt_mcp_session_link_up(&srx, 0);
    bt_mcp_session_mark_ready(&stx);
    bt_mcp_session_mark_ready(&srx);

    /* drain helper: move v → frx, capturing control frames; selectively drop CREDIT */
    struct { int drop_credit; int drop_ack; } flags = { 0, 0 };
    uint8_t ctrl_log[256]; uint32_t ctrl_n = 0;

    /* Send the only message. Receiver consumes it, sends ACK+CREDIT back. */
    CHECK(bt_mcp_session_tx(&stx, (const uint8_t *)"{\"q\":1}", 7) == 0, "D: tx");
    CHECK(stx.credits == 0, "D: window=1 exhausted");
    /* deliver to receiver fragment-by-fragment (one message, one emit: 7B < 238) */
    g_msgs = 0;
    bt_mcp_framer_feed(&frx, v.b, (uint16_t)v.n, fr_sink, &srx);
    CHECK(g_msgs == 1, "D: receiver got it");
    /* receiver produced ACK + CREDIT into vr; deliver ACK, DROP the CREDIT */
    size_t off = 0; int got_credit = 0;
    while (off < vr.n) {
        uint8_t hdr[6]; memcpy(hdr, vr.b + off, 6);
        uint16_t plen = (uint16_t)(hdr[4] | (hdr[5] << 8));
#if BT_MCP_CFG_FCS16
        uint16_t flen = (uint16_t)(6 + 2 + plen);
#else
        uint16_t flen = (uint16_t)(6 + plen);
#endif
        int is_ctrl = (hdr[1] & BT_MCP_FLAG_CTRL) != 0;
#if BT_MCP_CFG_FCS16
        uint8_t type = vr.b[off + 8];
#else
        uint8_t type = vr.b[off + 6];
#endif
        if (is_ctrl && type == BT_MCP_CTRL_CREDIT) {
            got_credit = 1; flags.drop_credit = 1; (void)flags.drop_credit;
            off += flen; continue;              /* CREDIT LOST */
        }
        uint8_t buf[512]; memcpy(buf, vr.b + off, flen);
        bt_mcp_framer_feed(&ftx, buf, flen, fr_sink, &stx);
        off += flen;
    }
    CHECK(got_credit, "D: receiver did send CREDIT (we dropped it)");
    CHECK(stx.txslot[0].valid == 0, "D: ACK cleared tx slot (no retransmit)");

    /* Sender now: credits==0, no pending tx, receiver has consumed everything.
     * Nothing will ever replenish credits: can the sender ever send again? */
    uint32_t t;
    for (t = 0; t < 30000; t += 100) {
        bt_mcp_session_tick(&stx, t);
        bt_mcp_session_tick(&srx, t);
    }
    CHECK(stx.credits == 0, "D: sender still has zero credits (deadlock present)");
    int can = bt_mcp_session_tx(&stx, (const uint8_t *)"{\"q\":2}", 7);
    printf("  D: after tail-loss, tx returns %d (credits=%u)  [E_AGAIN=-1 => deadlock confirmed]\n",
           can, stx.credits);
    CHECK(can == BT_MCP_E_AGAIN, "D: tail-lost CREDIT = permanent stall");
    (void)ctrl_log; (void)ctrl_n;
}

/* ---------- E: reconnect asymmetry over-credit probe ---------- */
static void probeE(void)
{
    bt_mcp_session_t s; bt_mcp_framer_t f; vsink_t v;
    stream_setup(&f, &v, &s);
    /* simulate: peer consumed 300 messages in an old connection */
    s.peer_consumed = 300;
    /* new connection on the receiver side: it announces cumulative 1 */
    uint8_t credit[3] = { BT_MCP_CTRL_CREDIT, 1, 0 };
    bt_mcp_session_rx(&s, BT_MCP_KIND_CTRL, 0, credit, 3, NULL, NULL);
    printf("  E: credits after reconnection CREDIT total=1 with stale peer_consumed=300: %u\n",
           s.credits);
    CHECK(s.credits <= 8, "E: stale peer_consumed must not explode credits (window=4)");
}

/* ---------- F: gap_wait never resets across healed gaps ---------- */
static void probeF(void)
{
    static bt_mcp_chan_cap_t cap = { BT_MCP_CHAN_PACKET, 53, 0, 0, 4 }; /* chunk=47 */
    bt_mcp_framer_t ftx, frx; vsink_t v;
    bt_mcp_frame_cfg_t fc = { 4, 2000 };
    bt_mcp_framer_init(&ftx, &fc, &cap);
    bt_mcp_framer_init(&frx, &fc, &cap);
    memset(&v, 0, sizeof v);
    bt_mcp_session_cfg_t cfg = { 30000, 5000, 4, 0, 3, 600 };
    bt_mcp_session_t stx, srx;
    bt_mcp_session_init(&stx, &cfg);
    bt_mcp_session_init(&srx, &cfg);
    static vsink_t vr; memset(&vr, 0, sizeof vr);
    bt_mcp_session_bind(&stx, &ftx, vemit, &v);
    bt_mcp_session_bind(&srx, &frx, vemit, &vr);
    bt_mcp_session_link_up(&stx, 0); bt_mcp_session_link_up(&srx, 0);
    bt_mcp_session_mark_ready(&stx); bt_mcp_session_mark_ready(&srx);

    static char msg[400];
    for (int i = 0; i < 400; i++) msg[i] = (char)('a' + i % 26);
    CHECK(bt_mcp_session_tx(&stx, (const uint8_t *)msg, 400) == 0, "F: tx");  /* 9 frags (chunk 47) */
    size_t off = 0; int idxn = 0;
    static uint8_t frag[64][64]; static uint16_t flen[64];
    while (off < v.n) {
        uint8_t hdr[6]; memcpy(hdr, v.b + off, 6);
        uint16_t pl = (uint16_t)(hdr[4] | (hdr[5] << 8));
#if BT_MCP_CFG_FCS16
        uint16_t fl = (uint16_t)(6 + 2 + pl);
#else
        uint16_t fl = (uint16_t)(6 + pl);
#endif
        if (idxn < 64 && fl <= sizeof frag[0]) {
            flen[idxn] = fl;
            memcpy(frag[idxn], v.b + off, fl);
            idxn++;
        }
        off += fl;
    }
    g_msgs = 0;
    /* 5 gap episodes, each healed: episodes 1-4 deliverable, 5th should still work */
    for (int ep = 0; ep < 5; ep++) {
        /* deliver out-of-order: frag 8 (last) first to trigger gap, then frag 1..8 */
        bt_mcp_framer_feed(&frx, frag[8], flen[8], fr_sink, &srx);   /* gap registered */
        for (int i = 1; i <= 8; i++)
            bt_mcp_framer_feed(&frx, frag[i], flen[i], fr_sink, &srx); /* heal */
        if (g_msgs) break;   /* completed on some episode */
    }
    printf("  F: completed after %d healed gap episodes (gap_wait=%u)\n",
           g_msgs ? (5 - 0) : 5, frx.reasm[0].gap_wait);
    /* With current code, 5th gap on same slot hits GAP_WAIT_MAX=4 → slot freed →
     * message lost. Expect it to complete: message had all fragments delivered. */
    CHECK(g_msgs == 1, "F: message survives 5 healed gaps (gap_wait must reset per-episode)");
}

/* ---------- G: dedupe false-accept/false-drop ---------- */
static void probeG(void)
{
    static bt_mcp_chan_cap_t cap = { BT_MCP_CHAN_PACKET, 244, 0, 0, 4 };
    bt_mcp_framer_t ftx, frx; vsink_t v, vr;
    bt_mcp_frame_cfg_t fc = { 4, 2000 };
    bt_mcp_framer_init(&ftx, &fc, &cap);
    bt_mcp_framer_init(&frx, &fc, &cap);
    memset(&v, 0, sizeof v); memset(&vr, 0, sizeof vr);
    bt_mcp_session_cfg_t cfg = { 30000, 5000, 4, 0, 3, 600 };
    bt_mcp_session_t stx, srx;
    bt_mcp_session_init(&stx, &cfg); bt_mcp_session_init(&srx, &cfg);
    bt_mcp_session_bind(&stx, &ftx, vemit, &v);
    bt_mcp_session_bind(&srx, &frx, vemit, &vr);
    bt_mcp_session_link_up(&stx, 0); bt_mcp_session_link_up(&srx, 0);
    bt_mcp_session_mark_ready(&stx); bt_mcp_session_mark_ready(&srx);

    /* 4 identical-ish messages in flight, ALL ACKs lost, sender retransmits all */
    static char msg[100]; memset(msg, 'w', sizeof msg);
    static uint8_t wire[4][512]; static uint16_t wl[4]; static uint8_t seqs[4];
    for (int k = 0; k < 4; k++) {
        size_t n0 = v.n;
        seqs[k] = bt_mcp_framer_alloc_seq(&ftx);
        CHECK(bt_mcp_framer_send_seq(&ftx, BT_MCP_KIND_MCP, seqs[k],
            (const uint8_t *)msg, 100, vemit, &v) == 0, "G: send");
        wl[k] = (uint16_t)(v.n - n0);
        memcpy(wire[k], v.b + n0, wl[k]);
    }
    /* deliver all 4 to receiver */
    g_msgs = 0;
    for (int k = 0; k < 4; k++)
        bt_mcp_framer_feed(&frx, wire[k], wl[k], fr_sink, &srx);
    int first = g_msgs;
    CHECK(first == 4, "G: 4 in-flight all delivered");
    /* replay all 4 (ACKs lost → sender retransmitted) */
    for (int k = 0; k < 4; k++)
        bt_mcp_framer_feed(&frx, wire[k], wl[k], fr_sink, &srx);
    CHECK(g_msgs == 4, "G: replay of all 4 deduped (no false accept)");

    /* false-drop check: two DIFFERENT messages with identical content must both pass */
    size_t n0 = v.n;
    uint8_t s1 = bt_mcp_framer_alloc_seq(&ftx);
    bt_mcp_framer_send_seq(&ftx, BT_MCP_KIND_MCP, s1, (const uint8_t *)msg, 100, vemit, &v);
    uint8_t w2[512]; uint16_t wl2 = (uint16_t)(v.n - n0); memcpy(w2, v.b + n0, wl2);
    n0 = v.n;
    uint8_t s2 = bt_mcp_framer_alloc_seq(&ftx);
    bt_mcp_framer_send_seq(&ftx, BT_MCP_KIND_MCP, s2, (const uint8_t *)msg, 100, vemit, &v);
    uint8_t w3[512]; uint16_t wl3 = (uint16_t)(v.n - n0); memcpy(w3, v.b + n0, wl3);
    g_msgs = 0;
    bt_mcp_framer_feed(&frx, w2, wl2, fr_sink, &srx);
    bt_mcp_framer_feed(&frx, w3, wl3, fr_sink, &srx);
    CHECK(g_msgs == 2, "G: two identical-content messages with different seq both pass");
}

/* ---------- H: u16 cumulative wraparound ---------- */
static void probeH(void)
{
    bt_mcp_session_t s; bt_mcp_framer_t f; vsink_t v;
    stream_setup(&f, &v, &s);
    s.peer_consumed = 0xFFF0;
    uint8_t credit[3] = { BT_MCP_CTRL_CREDIT, 0xF0, 0xFF };  /* total 0xFFF0 → delta 0 */
    bt_mcp_session_rx(&s, BT_MCP_KIND_CTRL, 0, credit, 3, NULL, NULL);
    uint32_t c0 = s.credits;
    /* wrap: total becomes 0x0010 (receiver wrapped) */
    uint8_t c2[3] = { BT_MCP_CTRL_CREDIT, 0x10, 0x00 };
    bt_mcp_session_rx(&s, BT_MCP_KIND_CTRL, 0, c2, 3, NULL, NULL);
    CHECK(s.credits == c0 + 0x20, "H: u16 wraparound delta math safe");
}

/* ---------- I: client init gate ---------- */
static void probeI(void)
{
    bt_mcp_session_t s; bt_mcp_framer_t f; vsink_t v;
    stream_setup(&f, &v, &s);
    bt_mcp_client_t cl;
    bt_mcp_client_init(&cl, &s, NULL, NULL);
    /* app sends initialize via client */
    CHECK(bt_mcp_client_initialize(&cl, "probe", "1") == 0, "I: initialize sent");
    uint32_t init_id = cl.init_id;
    CHECK(init_id != 0, "I: init_id stored");
    /* a response arrives with a DIFFERENT id */
    const char *other = "{\"jsonrpc\":\"2.0\",\"id\":999,\"result\":{\"x\":1}}";
    bt_mcp_client_rx(&cl, (const uint8_t *)other, strlen(other));
    CHECK(cl.initialized == 0, "I: non-init response does not flip initialized");
    /* now the real init response with init_id */
    char good[128];
    int n = snprintf(good, sizeof good,
        "{\"jsonrpc\":\"2.0\",\"id\":%u,\"result\":{\"protocolVersion\":\"2025-11-25\"}}",
        init_id);
    bt_mcp_client_rx(&cl, (const uint8_t *)good, n);
    CHECK(cl.initialized == 1, "I: matching response flips initialized");
    /* duplicate late response must not double-send notifications/initialized */
    CHECK(bt_mcp_client_rx(&cl, (const uint8_t *)good, n) == 0, "I: late dup accepted silently");
}

/* ---------- J: over-long string id → id:null + -32600 ---------- */
static char j_out[4096]; static uint32_t j_out_n;
static int j_emit(void *io, const uint8_t *d, uint16_t n)
{ (void)io; if (j_out_n + n < sizeof j_out) { memcpy(j_out + j_out_n, d, n); j_out_n += n; } return 0; }

static void probeJ(void)
{
    bt_mcp_session_t s; bt_mcp_framer_t f; vsink_t v;
    stream_setup(&f, &v, &s);
    /* rebind emit to capture */
    bt_mcp_session_bind(&s, &f, j_emit, NULL);
    j_out_n = 0;
    /* 40-char string id (> IDSTR_MAX=32) */
    const char *req = "{\"jsonrpc\":\"2.0\",\"id\":\"0123456789012345678901234567890123456789\","
                      "\"method\":\"ping\",\"params\":{}}";
    bt_mcp_session_rx(&s, BT_MCP_KIND_MCP, 1, (const uint8_t *)req,
                      (uint16_t)strlen(req), (void*)0, NULL);
    /* sink NULL → use server? simpler: check j_out contains "id":null and -32600 */
    /* NOTE: session_rx with NULL mcp_sink still processes… need mcp_server. */
}

int main(void)
{
    printf("A\n"); fflush(stdout); probeA();
    printf("B\n"); fflush(stdout); probeB();
    printf("C\n"); fflush(stdout); probeC();
    printf("D\n"); fflush(stdout); probeD();
    printf("E\n"); fflush(stdout); probeE();
    printf("F\n"); fflush(stdout); probeF();
    printf("G\n"); fflush(stdout); probeG();
    printf("H\n"); fflush(stdout); probeH();
    printf("I\n"); fflush(stdout); probeI();
    probeJ();
    printf("\n%d pass, %d fail\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
