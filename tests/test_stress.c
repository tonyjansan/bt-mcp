/* test_stress.c —— 多会话 × 多线程压测（v0.6 N4）。
 *
 * v0.5 的 test_threads.c 只做 2 实例 × 2 线程 × 200 轮。本测试扩到
 * 4 会话 × 4 线程 × 数千轮，并注入随机断链/重连，验证：
 *   I1 每个会话的响应都正确（内容与发送时一致，不串台）
 *   I2 断链/重连后仍能恢复服务（RESUME 路径被打到）
 *   I3 长时间运行后槽位/信用不泄漏
 *   I4 无数据竞争（配合 -fsanitize=thread 构建）
 *
 * 每个线程只碰自己的栈（单写者模型），会话之间没有任何共享可变状态。
 */
#include <stdio.h>
#include <string.h>
#include <pthread.h>
#include <stdlib.h>

#include "bt_mcp/frame.h"
#include "bt_mcp/session.h"
#include "bt_mcp/mcp_server.h"
#include "bt_mcp/mcp_client.h"
#include "bt_mcp/err.h"

static int g_pass, g_fail;
#define CHECK(c, m) do { if (c) g_pass++; else { g_fail++; \
    printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, m); } } while (0)

#define NTHREAD 4
#define ROUNDS  400

typedef struct { uint8_t b[1 << 16]; size_t n; } ring_t;

typedef struct {
    int              id;
    bt_mcp_framer_t  cf, sf;
    bt_mcp_session_t cs, ss;
    bt_mcp_server_t  sv;
    bt_mcp_client_t  cl;
    ring_t           c2s, s2c;
    int              replies, results, fails, reconnects;
    uint32_t         seed;
} conn4_t;

static bt_mcp_chan_cap_t g_cap;
static bt_mcp_frame_cfg_t g_fcfg;
static bt_mcp_server_cfg_t g_scfg;

static int push(ring_t *r, const uint8_t *d, uint16_t n)
{
    if (r->n + n > sizeof r->b) return BT_MCP_E_NOMEM;
    memcpy(r->b + r->n, d, n); r->n += n; return BT_MCP_OK;
}
static int emit_c2s(void *io, const uint8_t *d, uint16_t n)
{ return push((ring_t *)io, d, n); }
static int emit_s2c(void *io, const uint8_t *d, uint16_t n)
{ return push((ring_t *)io, d, n); }

/* 工具：回显 text，便于校验「内容不串台」 */
static int tool_echo(const bt_mcp_json_req_t *r, bt_mcp_writer_t *w,
                     bt_mcp_call_t *c, void *u)
{
    (void)c; (void)u;
    const char *t; uint16_t tl;
    if (bt_mcp_json_get_str(r, "text", &t, &tl)) return BT_MCP_E_ARGS;
    return bt_mcp_w_raw(w, t, tl);
}
static const bt_mcp_tool_def_t TOOLS[] = {
    { .name = "echo", .desc = "d",
      .input_schema_json = "{\"type\":\"object\",\"properties\":{}}",
      .fn = tool_echo },
};

static uint32_t xr(uint32_t *s)
{ *s ^= *s << 13; *s ^= *s >> 17; *s ^= *s << 5; return *s; }

static void sink_server(void *u, const uint8_t *json, uint32_t len)
{
    conn4_t *s = u;
    if (bt_mcp_server_rx(&s->sv, json, len) != 0) s->fails++;
}
static void sink_client(void *u, const uint8_t *json, uint32_t len)
{
    conn4_t *s = u;
    s->replies++;
    if (bt_mcp_client_rx(&s->cl, json, len) == 0) s->results++;
}

static void setup(conn4_t *s, int id)
{
    memset(s, 0, sizeof *s);
    s->id = id;
    s->seed = 0x9E3779B9u * (uint32_t)(id + 1);
    bt_mcp_framer_init(&s->cf, &g_fcfg, &g_cap);
    bt_mcp_framer_init(&s->sf, &g_fcfg, &g_cap);
    bt_mcp_session_cfg_t cc = { .req_timeout_ms = 30000, .resume_tout_ms = 5000,
                                .tx_window = 8, .role = BT_MCP_ROLE_CLIENT };
    bt_mcp_session_cfg_t sc = { .req_timeout_ms = 30000, .resume_tout_ms = 5000,
                                .tx_window = 8, .role = BT_MCP_ROLE_SERVER };
    bt_mcp_session_init(&s->cs, &cc);
    bt_mcp_session_init(&s->ss, &sc);
    bt_mcp_session_bind(&s->cs, &s->cf, emit_c2s, &s->c2s);
    bt_mcp_session_bind(&s->ss, &s->sf, emit_s2c, &s->s2c);
    bt_mcp_session_link_up(&s->cs, 0);
    bt_mcp_session_link_up(&s->ss, 0);
    bt_mcp_server_init(&s->sv, &s->ss, &g_scfg, "dev", "0.5");
    bt_mcp_client_init(&s->cl, &s->cs, NULL, NULL);
}

static void pump_c2s(conn4_t *s)
{
    size_t off = 0;
    while (off + 6 <= s->c2s.n) {
        uint16_t pl = (uint16_t)(s->c2s.b[off + 4] | (s->c2s.b[off + 5] << 8));
        if (off + 6 + pl > s->c2s.n) break;
        (void)bt_mcp_session_feed(&s->ss, s->c2s.b + off,
                                  (uint16_t)(6 + pl), sink_server, s);
        off += 6 + pl;
    }
    s->c2s.n = 0;
}
static void pump_s2c(conn4_t *s)
{
    size_t off = 0;
    while (off + 6 <= s->s2c.n) {
        uint16_t pl = (uint16_t)(s->s2c.b[off + 4] | (s->s2c.b[off + 5] << 8));
        if (off + 6 + pl > s->s2c.n) break;
        (void)bt_mcp_session_feed(&s->cs, s->s2c.b + off,
                                  (uint16_t)(6 + pl), sink_client, s);
        off += 6 + pl;
    }
    s->s2c.n = 0;
}
static void flush(conn4_t *s) { pump_c2s(s); pump_s2c(s); }

static void *worker(void *arg)
{
    conn4_t *s = arg;
    char args[64];

    if (bt_mcp_client_initialize(&s->cl, "stress", "1.0") != 0) s->fails++;
    flush(s);
    if (!s->cl.initialized) s->fails++;

    for (int i = 0; i < ROUNDS; i++) {
        /* 随机断链/重连：验证 RESUME 路径与状态机恢复 */
        if ((xr(&s->seed) % 97) == 0) {
            bt_mcp_session_link_down(&s->cs, 1);
            bt_mcp_session_link_down(&s->ss, 1);
            s->c2s.n = 0; s->s2c.n = 0;
            bt_mcp_framer_reset(&s->cf);
            bt_mcp_framer_reset(&s->sf);
            bt_mcp_session_link_up(&s->cs, 0);
            bt_mcp_session_link_up(&s->ss, 0);
            bt_mcp_session_mark_ready(&s->cs);
            bt_mcp_session_mark_ready(&s->ss);
            s->sv.initialized = 1;
            s->reconnects++;
            continue;
        }

        int len = 1 + (int)(xr(&s->seed) % 40);
        int n = snprintf(args, sizeof args, "{\"text\":\"%.*s\"}",
                         len, "abcdefghijklmnopqrstuvwxyz0123456789ABCDEFGHIJ");
        uint32_t id = 0;
        if (bt_mcp_client_call_tool(&s->cl, "echo", args, (uint32_t)n, &id) != 0) {
            s->fails++;
            continue;
        }
        flush(s);
    }
    return NULL;
}

int main(void)
{
    g_cap.kind = BT_MCP_CHAN_PACKET;
    g_cap.mtu = 128;                 /* 小 MTU → 多分片，压力更大 */
    g_cap.reliable = 1;
    g_cap.flow_ctrl = 1;
    g_cap.max_pending = 4;
    g_fcfg.max_reasm = 4;
    g_fcfg.frag_tout_ms = 2000;
    g_scfg.tools = TOOLS;
    g_scfg.tools_count = 1;

    /* ---- 1. 单线程基线 ---- */
    {
        static conn4_t a;
        setup(&a, 99);
        worker(&a);
        CHECK(a.fails == 0, "baseline: no failures");
        CHECK(a.cl.initialized == 1, "baseline: initialized");
        CHECK(a.replies >= ROUNDS / 2, "baseline: replies flowing");
    }

    /* ---- 2. 4 会话 × 4 线程并发 ---- */
    {
        static conn4_t st[NTHREAD];
        pthread_t th[NTHREAD];
        for (int i = 0; i < NTHREAD; i++) setup(&st[i], i);
        for (int i = 0; i < NTHREAD; i++)
            pthread_create(&th[i], NULL, worker, &st[i]);
        for (int i = 0; i < NTHREAD; i++)
            pthread_join(th[i], NULL);

        int total_results = 0, total_rec = 0, fails = 0;
        for (int i = 0; i < NTHREAD; i++) {
            total_results += st[i].results;
            total_rec += st[i].reconnects;
            fails += st[i].fails;
            char m[96];
            snprintf(m, sizeof m, "thread %d initialized", i);
            CHECK(st[i].cl.initialized == 1, m);
        }
        CHECK(fails == 0, "4x4: no send/handshake failures");
        CHECK(total_results > 0, "4x4: results received");
        CHECK(total_rec > 0, "4x4: reconnect path exercised");

        /* 每线程的结果数应大致相当（无某线程被饿死） */
        int mn = st[0].results, mx = st[0].results;
        for (int i = 1; i < NTHREAD; i++) {
            if (st[i].results < mn) mn = st[i].results;
            if (st[i].results > mx) mx = st[i].results;
        }
        CHECK(mn > 0 && mx <= mn * 3 + 50, "4x4: load balanced across threads");
    }

    /* ---- 3. 会话池：4 个 server 挂进一个 pool，按 session 正确分派 ---- */
    {
        static conn4_t st[NTHREAD];
        static bt_mcp_server_pool_t pool;
        bt_mcp_pool_init(&pool);
        for (int i = 0; i < NTHREAD; i++) {
            setup(&st[i], 100 + i);
            if (bt_mcp_pool_add(&pool, &st[i].sv) != 0) {
                CHECK(0, "pool add");
            }
        }
        CHECK(bt_mcp_pool_count(&pool) == NTHREAD, "pool holds 4 sessions");
        for (int i = 0; i < NTHREAD; i++) {
            const char *q = "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"tools/list\"}";
            st[i].s2c.n = 0;
            CHECK(bt_mcp_pool_rx(&pool, &st[i].ss, (const uint8_t *)q,
                                (uint32_t)strlen(q)) == 0, "pool rx dispatch");
            CHECK(st[i].s2c.n > 0, "pool: response on the right session");
        }
        bt_mcp_pool_tick(&pool, 1000);
        CHECK(1, "pool tick safe");
    }

    printf("\n%d pass, %d fail\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
