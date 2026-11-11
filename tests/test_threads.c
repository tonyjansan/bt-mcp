/* test_threads.c —— 多实例并发安全（v0.4 G5 的 TSan 用例）。
 *
 * 线程模型：bt-mcp 的约定是「一个实例只被一个线程使用」（单写者）。
 * 桥接场景里一个进程会同时持有**多个**实例（同时连多台设备），
 * 每个实例在自己的线程里跑。只要实例之间没有共享可变状态就是安全的。
 *
 * 本测试建两组完全独立的端到端栈（各自的 framer/session/server/信道），
 * 两个线程并发、各跑 ROUND 轮「工具调用往返」；断言：
 *  1) 两边都能完成握手并正确应答每一条请求（互不干扰）；
 *  2) 在 -fsanitize=thread 下无数据竞争报告（见 build.sh 的 tsan 步骤）。
 *
 * 注：bt_mcp_trace 的钩子是**进程级**全局（调试设施）。多线程应用要么不安装、
 * 要么自行加锁；本测试以 BT_MCP_CFG_TRACE=0（默认）构建。
 */
#include <stdio.h>
#include <string.h>
#include <pthread.h>

#include "bt_mcp/frame.h"
#include "bt_mcp/session.h"
#include "bt_mcp/mcp_server.h"
#include "bt_mcp/mcp_client.h"
#include "bt_mcp/err.h"

static int g_pass, g_fail;
#define CHECK(c, m) do { if (c) g_pass++; else { g_fail++; \
    printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, m); } } while (0)

#define ROUND 200

typedef struct { uint8_t b[1 << 15]; size_t n; } ring_t;

typedef struct {
    int              id;
    bt_mcp_framer_t  cf, sf;
    bt_mcp_session_t cs, ss;
    bt_mcp_server_t  sv;
    bt_mcp_client_t  cl;
    ring_t           c2s, s2c;
    int              replies;      /* 收到的响应条数 */
    int              tool_results; /* client 收到多少次 result */
    int              fails;
} stack_t;

static bt_mcp_chan_cap_t g_cap;
static bt_mcp_frame_cfg_t g_fcfg;

static int push(ring_t *r, const uint8_t *d, uint16_t n)
{
    if (r->n + n > sizeof r->b) return BT_MCP_E_NOMEM;
    memcpy(r->b + r->n, d, n);
    r->n += n;
    return BT_MCP_OK;
}
static int emit_c2s(void *io, const uint8_t *d, uint16_t n)
{ return push((ring_t *)io, d, n); }
static int emit_s2c(void *io, const uint8_t *d, uint16_t n)
{ return push((ring_t *)io, d, n); }

/* 工具：返回参数 text 的长度（纯计算，不碰共享内存） */
static int tool_len(const bt_mcp_json_req_t *r, bt_mcp_writer_t *w,
                    bt_mcp_call_t *c, void *user)
{
    (void)c; (void)user;
    const char *t; uint16_t tl;
    if (bt_mcp_json_get_str(r, "text", &t, &tl)) return BT_MCP_E_ARGS;
    char buf[16];
    int n = 0, v = (int)tl;
    if (v == 0) buf[n++] = '0';
    else {
        char tmp[12]; int k = 0;
        while (v) { tmp[k++] = (char)('0' + v % 10); v /= 10; }
        while (k) buf[n++] = tmp[--k];
    }
    return bt_mcp_w_raw(w, buf, (uint32_t)n);
}

static const bt_mcp_tool_def_t TOOLS[] = {
    { .name = "len", .desc = "length of text",
      .input_schema_json = "{\"type\":\"object\",\"properties\":{}}",
      .authz = BT_MCP_AUTHZ_OPEN, .fn = tool_len },
};

static void stack_init(stack_t *s, int id)
{
    memset(s, 0, sizeof *s);
    s->id = id;
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

    bt_mcp_server_cfg_t svc = { .tools = TOOLS, .tools_count = 1 };
    bt_mcp_server_init(&s->sv, &s->ss, &svc, "dev", "0.4");
    bt_mcp_client_init(&s->cl, &s->cs, NULL, NULL);
}

/* L4 上抛 → L5（server）：与 mock 的 e2e 驱动同形 */
static void sink_server(void *u, const uint8_t *json, uint32_t len)
{
    stack_t *s = u;
    if (bt_mcp_server_rx(&s->sv, json, len) != 0) s->fails++;
}

/* 把帧交给 server 侧 session：framer 重组 → 状态门 → L5 回调 */
static void pump_c2s(stack_t *s)
{
    if (!s->c2s.n) return;
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

/* L4 上抛 → L5（client） */
static void sink_client(void *u, const uint8_t *json, uint32_t len)
{
    stack_t *s = u;
    s->replies++;
    if (bt_mcp_client_rx(&s->cl, json, len) == 0) s->tool_results++;
}

/* 把帧交给 client 侧 session（同样走完整重组） */
static void pump_s2c(stack_t *s)
{
    if (!s->s2c.n) return;
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

/* 一次往返：把 client 刚发出的帧送到 server，再把 server 的响应当回 client */
static void flush(stack_t *s)
{
    pump_c2s(s);
    pump_s2c(s);
}

/* 一个栈的完整使用序列：握手 + ROUND 轮工具调用 */
static void drive(stack_t *s)
{
    /* 握手：initialize → 等响应 → client 自己发 initialized */
    if (bt_mcp_client_initialize(&s->cl, "conf-tester", "1.0") != 0) s->fails++;
    flush(s);
    if (!s->cl.initialized) s->fails++;

    /* ROUND 轮工具调用：每轮 request → flush → 结果回到 client */
    for (int i = 0; i < ROUND; i++) {
        char args[48];
        int an = snprintf(args, sizeof args, "{\"text\":\"%.*s\"}",
                          (i % 20) + 1, "abcdefghijklmnopqrst");
        uint32_t id = 0;
        if (bt_mcp_client_call_tool(&s->cl, "len", args, (uint32_t)an, &id) != 0) {
            s->fails++;
            continue;
        }
        flush(s);
    }
}

static void *worker(void *arg)
{
    drive((stack_t *)arg);
    return NULL;
}

int main(void)
{
    g_cap.kind = BT_MCP_CHAN_PACKET;
    g_cap.mtu = 244;
    g_cap.reliable = 1;
    g_cap.flow_ctrl = 1;
    g_cap.max_pending = 8;
    g_fcfg.max_reasm = 4;
    g_fcfg.frag_tout_ms = 2000;

    /* ---- 1. 单线程基线：流程本身必须通 ---- */
    {
        static stack_t a;
        stack_init(&a, 0);
        drive(&a);
        CHECK(a.fails == 0, "baseline: no failures");
        CHECK(a.cs.state == BT_MCP_S_READY, "baseline: client READY");
        CHECK(a.sv.initialized == 1, "baseline: server initialized");
        CHECK(a.replies >= ROUND, "baseline: got a reply per request");
    }

    /* ---- 2. 两线程并发、各自一套栈 ---- */
    {
        static stack_t ta, tb;
        stack_init(&ta, 11);
        stack_init(&tb, 22);
        pthread_t th[2];
        pthread_create(&th[0], NULL, worker, &ta);
        pthread_create(&th[1], NULL, worker, &tb);
        pthread_join(th[0], NULL);
        pthread_join(th[1], NULL);

        CHECK(ta.fails == 0, "thread A: no failures");
        CHECK(tb.fails == 0, "thread B: no failures");
        CHECK(ta.cs.state == BT_MCP_S_READY, "thread A: READY");
        CHECK(tb.cs.state == BT_MCP_S_READY, "thread B: READY");
        CHECK(ta.replies >= ROUND, "thread A: reply per request");
        CHECK(tb.replies >= ROUND, "thread B: reply per request");
    }

    printf("\n%d pass, %d fail\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
