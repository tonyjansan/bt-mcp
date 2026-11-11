/* test_fault.c —— 故障注入（v0.5 V3 分支覆盖率）。
 *
 * 覆盖率缺口的最大一块是「发送/写入失败后的错误传播链」（if (rc) return rc）。
 * 正常测试里这些分支永不执行。这里把各层的 sink/emit 换成**受控失败**的版本，
 * 让失败发生在不同的点，从而把整条错误传播链跑起来，并验证：
 *  1) 失败时返回确定错误码（不崩溃、不产生半截报文）；
 *  2) 失败后状态可恢复（换回正常 sink 仍能工作）。
 */
#include <stdio.h>
#include <string.h>

#include "bt_mcp/frame.h"
#include "bt_mcp/session.h"
#include "bt_mcp/mcp_server.h"
#include "bt_mcp/mcp_client.h"
#include "bt_mcp/err.h"

static int g_pass, g_fail;
#define CHECK(c, m) do { if (c) g_pass++; else { g_fail++; \
    printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, m); } } while (0)

typedef struct { uint8_t b[65536]; size_t n; int fail_on; int calls; } sinkx_t;

static int failing_emit(void *io, const uint8_t *d, uint16_t n)
{
    sinkx_t *s = io;
    s->calls++;
    if (s->fail_on >= 0 && s->calls > s->fail_on) return BT_MCP_E_IO;
    if (s->n + n > sizeof s->b) return BT_MCP_E_TOOBIG;
    memcpy(s->b + s->n, d, n); s->n += n;
    return BT_MCP_OK;
}

static int t_ok(const bt_mcp_json_req_t *r, bt_mcp_writer_t *w,
                bt_mcp_call_t *c, void *u)
{
    (void)r; (void)c; (void)u;
    return bt_mcp_w_raw(w, "ok", 2);
}

/* 写满型 sink：用于逼出 writer 的 E_IO 传播 */
typedef struct { int budget; } wfail_t;
static int w_put(void *io, const char *d, uint32_t n)
{
    (void)d;
    wfail_t *w = io;
    if ((int)n > w->budget) { w->budget = -1; return BT_MCP_E_TOOBIG; }
    w->budget -= (int)n;
    return BT_MCP_OK;
}

static const bt_mcp_tool_def_t TOOLS[] = {
    { .name = "ok", .desc = "d", .input_schema_json = "{}", .fn = t_ok },
};

int main(void)
{
    static bt_mcp_chan_cap_t cap = { .kind = BT_MCP_CHAN_PACKET, .mtu = 64,
                                     .reliable = 0, .flow_ctrl = 0,
                                     .max_pending = 2 };
    static bt_mcp_frame_cfg_t fcfg = { .max_reasm = 2, .frag_tout_ms = 2000 };
    static bt_mcp_framer_t fr;
    static bt_mcp_session_t se;
    static bt_mcp_server_t sv;
    static sinkx_t sk;

    /* ---- 1. 发送路径在各点失败：返回确定错误码 ---- */
    memset(&sk, 0, sizeof sk);
    sk.fail_on = 0;                       /* 第一次发送就失败 */
    bt_mcp_framer_init(&fr, &fcfg, &cap);
    bt_mcp_session_cfg_t sc = { .req_timeout_ms = 30000, .resume_tout_ms = 5000,
                                .tx_window = 4, .role = BT_MCP_ROLE_SERVER };
    bt_mcp_session_init(&se, &sc);
    bt_mcp_session_bind(&se, &fr, failing_emit, &sk);
    bt_mcp_session_link_up(&se, 0);
    bt_mcp_server_cfg_t cfg = { .tools = TOOLS, .tools_count = 1 };
    bt_mcp_server_init(&sv, &se, &cfg, "d", "0.5");

    CHECK(bt_mcp_session_tx(&se, (const uint8_t *)"x", 1) == BT_MCP_E_IO,
          "tx propagates emit failure");
    /* 失败不应扣信用（v0.3 已修的语义，这里回归它） */
    int credits_before = se.credits;
    (void)bt_mcp_session_tx(&se, (const uint8_t *)"x", 1);
    CHECK(se.credits == credits_before, "failed tx does not consume credit");

    /* 控制帧失败 */
    CHECK(bt_mcp_session_ctrl(&se, BT_MCP_CTRL_PING, (const uint8_t *)"\x01", 1)
          == BT_MCP_E_IO, "ctrl propagates emit failure");

    /* 恢复 sink → 应能正常工作 */
    sk.fail_on = -1; sk.calls = 0; sk.n = 0;
    CHECK(bt_mcp_session_tx(&se, (const uint8_t *)"y", 1) == 0,
          "tx works after sink recovery");

    /* ---- 2. L5 响应发送失败：server_rx 必须把错误码透出 ---- */
    {
        sk.fail_on = -1; sk.calls = 0; sk.n = 0;
        const char *q = "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"initialize\","
                        "\"params\":{\"protocolVersion\":\"2025-11-25\"}}";
        CHECK(bt_mcp_server_rx(&sv, (const uint8_t *)q, (uint32_t)strlen(q)) == 0,
              "initialize ok with working sink");
        sk.fail_on = 0; sk.calls = 0;      /* 让响应发送失败 */
        const char *q2 = "{\"jsonrpc\":\"2.0\",\"id\":2,\"method\":\"tools/list\"}";
        int rc = bt_mcp_server_rx(&sv, (const uint8_t *)q2, (uint32_t)strlen(q2));
        CHECK(rc == BT_MCP_E_IO, "tools/list surfaces send failure");
    }

    /* ---- 3. writer 预算耗尽：各构造器都要把失败传上去 ---- */
    {
        wfail_t wf;
        bt_mcp_writer_t w;
        const char *m = "{\"jsonrpc\":\"2.0\",\"id\":1,\"result\":{}}";
        (void)m;
        wf.budget = 8;                    /* 只够写一小段 */
        w.put = w_put; w.sink = &wf; w.need_comma = 0;
        CHECK(bt_mcp_w_begin_result(&w, 1) == BT_MCP_E_TOOBIG, "begin_result -> E_TOOBIG on budget");
        wf.budget = 200;
        w.put = w_put; w.sink = &wf; w.need_comma = 0;
        CHECK(bt_mcp_w_begin_result(&w, 1) == BT_MCP_OK, "begin_result ok");
        CHECK(bt_mcp_w_end(&w) == BT_MCP_OK, "end ok");
        /* 关键路径：kv 系列在预算不足时也要报错 */
        wf.budget = 3;
        w.put = w_put; w.sink = &wf; w.need_comma = 0;
        CHECK(bt_mcp_w_kv_str(&w, "k", "v") == BT_MCP_E_TOOBIG, "kv_str -> E_TOOBIG on budget");
        wf.budget = 3;
        w.put = w_put; w.sink = &wf; w.need_comma = 0;
        CHECK(bt_mcp_w_kv_u32(&w, "k", 1) == BT_MCP_E_TOOBIG, "kv_u32 -> E_TOOBIG on budget");
        wf.budget = 3;
        w.put = w_put; w.sink = &wf; w.need_comma = 0;
        CHECK(bt_mcp_w_kv_u64(&w, "k", 1) == BT_MCP_E_TOOBIG, "kv_u64 -> E_TOOBIG on budget");
        wf.budget = 3;
        w.put = w_put; w.sink = &wf; w.need_comma = 0;
        CHECK(bt_mcp_w_kv_raw(&w, "k", "1", 1) == BT_MCP_E_TOOBIG, "kv_raw -> E_TOOBIG on budget");
        wf.budget = 3;
        w.put = w_put; w.sink = &wf; w.need_comma = 0;
        CHECK(bt_mcp_w_str_lit(&w, "abc") == BT_MCP_E_TOOBIG, "str_lit -> E_TOOBIG on budget");
        wf.budget = 3;
        w.put = w_put; w.sink = &wf; w.need_comma = 0;
        CHECK(bt_mcp_w_key(&w, "k") == BT_MCP_E_TOOBIG, "w_key -> E_TOOBIG on budget");
        wf.budget = 3;
        w.put = w_put; w.sink = &wf; w.need_comma = 0;
        CHECK(bt_mcp_w_begin_error(&w, 1, -32603, "x") == BT_MCP_E_TOOBIG,
              "begin_error -> E_TOOBIG on budget");
        wf.budget = 3;
        w.put = w_put; w.sink = &wf; w.need_comma = 0;
        CHECK(bt_mcp_w_begin_error_id(&w, 1, NULL, 0, -32603, "x")
              == BT_MCP_E_TOOBIG, "begin_error_id -> E_TOOBIG on budget");
        wf.budget = 3;
        w.put = w_put; w.sink = &wf; w.need_comma = 0;
        CHECK(bt_mcp_w_begin_result_id(&w, 1, "s", 1) == BT_MCP_E_TOOBIG,
              "begin_result_id -> E_TOOBIG on budget");
    }

    /* ---- 4. 帧层发送失败：MTU 非法 / 超 MAX_FRAG ---- */
    {
        static sinkx_t s2;
        memset(&s2, 0, sizeof s2); s2.fail_on = -1;
        static bt_mcp_framer_t f2;
        bt_mcp_chan_cap_t c2 = { .kind = BT_MCP_CHAN_PACKET, .mtu = 8,
                                 .reliable = 1, .flow_ctrl = 0, .max_pending = 1 };
        bt_mcp_framer_init(&f2, &fcfg, &c2);
        static uint8_t big[8192];
        memset(big, 'q', sizeof big);
        /* 单片载荷只有 2B → 8192B 需要 >255 片 → E_TOOBIG */
        int rc = bt_mcp_framer_send(&f2, BT_MCP_KIND_MCP, big, sizeof big,
                                    failing_emit, &s2);
        CHECK(rc == BT_MCP_E_TOOBIG, "framer rejects over MAX_FRAG");
        /* 控制帧超长（>255）→ E_TOOBIG */
        uint8_t ctl[300];
        memset(ctl, 0, sizeof ctl);
        rc = bt_mcp_framer_send(&f2, BT_MCP_KIND_CTRL, ctl, sizeof ctl,
                                failing_emit, &s2);
        CHECK(rc == BT_MCP_E_TOOBIG, "framer rejects oversized control frame");
        /* 载荷先超 MAX_MSG */
        rc = bt_mcp_framer_send(&f2, BT_MCP_KIND_MCP, big, BT_MCP_CFG_MAX_MSG + 1,
                                failing_emit, &s2);
        CHECK(rc == BT_MCP_E_TOOBIG, "framer rejects over MAX_MSG");
    }

    printf("\n%d pass, %d fail\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
