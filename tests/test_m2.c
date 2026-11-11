/* test_m2.c —— L2~L5 全栈单元测试（v0.3）。
 *
 * 覆盖：
 *  1. initialize 握手：protocolVersion 协商回显、serverInfo、_meta 通道声明
 *  2. tools/list 分页与 cursor
 *  3. tools/call 同步路径（转义、isError、未知工具/缺参）
 *  4. 字符串 id 原样回显
 *  5. 信用闭环：TX 扣减、消费自动回补、饱和
 *  6. 不可靠包通道：ACK、NACK 单片重传、超时整条重传、接收去重
 *  7. resources/list + resources/read（含 cursor 分页）
 *  8. 异步 tools/call：E_PENDING → 进度通知 → 完成/取消
 *  9. RESUME：token 匹配/不匹配/防爆破锁定
 * 10. PING RTT 测量
 * 11. notifications/cancelled
 *
 * 编：cc -O2 -std=c99 -Wall -Wextra -Werror -Icore/include \
 *        tests/test_m2.c core/src/frame.c core/src/session.c \
 *        core/src/json.c core/src/writer.c core/src/mcp_server.c \
 *        -o build/test_m2 && ./build/test_m2
 */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "bt_mcp/frame.h"
#include "bt_mcp/session.h"
#include "bt_mcp/mcp_server.h"
#include "bt_mcp/err.h"

static int g_pass, g_fail;

#define CHECK(cond, msg) do { \
    if (cond) { g_pass++; } \
    else { g_fail++; printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, msg); } \
} while (0)

/* ---- 线缆缓冲 ---- */
typedef struct { uint8_t buf[65536]; size_t len; } vec_t;

static int vec_put(void *io, const uint8_t *d, uint16_t n)
{
    vec_t *v = io;
    if (v->len + n > sizeof v->buf) return BT_MCP_E_NOMEM;
    memcpy(v->buf + v->len, d, n);
    v->len += n;
    return 0;
}

static int vec_has(const vec_t *v, const char *needle)
{
    size_t nl = strlen(needle);
    if (nl > v->len) return 0;
    for (size_t i = 0; i + nl <= v->len; i++)
        if (!memcmp(v->buf + i, needle, nl)) return 1;
    return 0;
}

/* ---- 工具/资源实现 ---- */

/* echo：把 arguments.text 解转义后原样输出 */
static int tool_echo(const bt_mcp_json_req_t *req, bt_mcp_writer_t *out,
                     bt_mcp_call_t *call, void *user)
{
    (void)call; (void)user;
    const char *txt; uint16_t tl;
    if (bt_mcp_json_get_str(req, "text", &txt, &tl)) {
        bt_mcp_w_raw(out, "missing text", 12);
        return 1;                       /* isError=true */
    }
    char buf[128]; uint16_t need = 0;
    uint16_t n = bt_mcp_json_decode_str(txt - 1, txt + tl + 1, buf,
                                        sizeof buf - 1, &need);
    (void)need;
    return bt_mcp_w_raw(out, buf, n);
}

/* 异步工具：首调返回 E_PENDING 并在受理时发一次进度 */
static bt_mcp_call_t *g_async_call;
static int g_async_progress;
static int tool_async(const bt_mcp_json_req_t *req, bt_mcp_writer_t *out,
                      bt_mcp_call_t *call, void *user)
{
    (void)req; (void)out; (void)user;
    g_async_call = call;
    g_async_progress = bt_mcp_call_progress(call, 1, 3) == 0;
    return BT_MCP_E_PENDING;
}

static const char RES_TEXT[] = "0123456789ABCDEFGHIJ";   /* 20B，页宽 8 */

static int res_read(void *user, uint32_t cursor, const char **text,
                    uint32_t *len, uint32_t *next)
{
    (void)user;
    const uint32_t total = sizeof RES_TEXT - 1;
    if (cursor >= total) {
        *text = ""; *len = 0; *next = BT_MCP_CURSOR_END;
        return 0;
    }
    uint32_t n = total - cursor;
    if (n > 8) n = 8;
    *text = RES_TEXT + cursor;
    *len = n;
    uint32_t nx = cursor + n;
    *next = (nx < total) ? nx : BT_MCP_CURSOR_END;
    return 0;
}

static const bt_mcp_tool_def_t TOOLS[] = {
    { "echo", "Echo back the text parameter",
      "{\"type\":\"object\",\"properties\":{\"text\":{\"type\":\"string\"}}}",
      BT_MCP_AUTHZ_OPEN, tool_echo, NULL },
    { "slow", "Async tool", "{\"type\":\"object\",\"properties\":{}}",
      BT_MCP_AUTHZ_OPEN, tool_async, NULL },
};

static const bt_mcp_resource_def_t RES[] = {
    { "log", "device log", "text/plain", BT_MCP_AUTHZ_OPEN, res_read, NULL },
};

/* 简化测试拓扑：server 侧 framer+session+server，客户端用 encode 手工造帧 */
static bt_mcp_framer_t  g_fr_s;
static bt_mcp_session_t g_se_s;
static bt_mcp_server_t  g_sv;
static vec_t            g_wire_s2c;

/* 模拟 Host 侧消费完响应后回补信用（真实链路由对端 session 自动发）。
 * CREDIT 载荷是「累计已消费条数」，故必须单调递增——发固定值会让 delta=0。 */
static uint16_t g_host_consumed;
static void host_topup(void)
{
    g_host_consumed = (uint16_t)(g_host_consumed + 8);
    uint8_t a[3] = { BT_MCP_CTRL_CREDIT,
                     (uint8_t)(g_host_consumed & 0xFF),
                     (uint8_t)(g_host_consumed >> 8) };
    bt_mcp_session_rx(&g_se_s, BT_MCP_KIND_CTRL, 0, a, 3, NULL, NULL);
}

static void reset_wires(void) { g_wire_s2c.len = 0; host_topup(); }

int main(void)
{
    bt_mcp_chan_cap_t cap = { BT_MCP_CHAN_PACKET, 244, 0, 0, 4 };
    bt_mcp_frame_cfg_t fcfg = { .max_reasm = 4, .frag_tout_ms = 2000 };
    bt_mcp_session_cfg_t scfg = { .req_timeout_ms = 30000, .resume_tout_ms = 5000,
                                  .tx_window = 4, .role = BT_MCP_ROLE_SERVER };

    bt_mcp_framer_init(&g_fr_s, &fcfg, &cap);
    bt_mcp_session_init(&g_se_s, &scfg);
    bt_mcp_session_bind(&g_se_s, &g_fr_s, vec_put, &g_wire_s2c);
    bt_mcp_session_link_up(&g_se_s, 0);

    bt_mcp_server_cfg_t svcfg = { TOOLS, 2, RES, 1 };
    bt_mcp_server_init(&g_sv, &g_se_s, &svcfg, "test-device", "0.3");

    reset_wires();

    /* ===== 1. initialize 协商 ===== */
    {
        const char *req =
            "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"initialize\","
            "\"params\":{\"protocolVersion\":\"2025-06-18\",\"capabilities\":{},"
            "\"clientInfo\":{\"name\":\"bridge\",\"version\":\"1\"}}}";
        int rc = bt_mcp_server_rx(&g_sv, (const uint8_t *)req, (uint32_t)strlen(req));
        CHECK(rc == 0, "initialize rc");
        CHECK(vec_has(&g_wire_s2c, "\"protocolVersion\":\"2025-06-18\""),
              "protocolVersion echoed from client");
        CHECK(vec_has(&g_wire_s2c, "\"serverInfo\":{\"name\":\"test-device\""),
              "serverInfo nested object");
        CHECK(vec_has(&g_wire_s2c, "\"io.btmcp/chan\":{\"kind\":0,\"mtu\":244,\"window\":4"),
              "_meta channel declaration");
        CHECK(vec_has(&g_wire_s2c, "\"reliable\":0"), "_meta reliable flag");
        reset_wires();
    }

    /* 不支持的版本 → 回最新 */
    {
        const char *req =
            "{\"jsonrpc\":\"2.0\",\"id\":2,\"method\":\"initialize\","
            "\"params\":{\"protocolVersion\":\"1999-01-01\"}}";
        bt_mcp_server_rx(&g_sv, (const uint8_t *)req, (uint32_t)strlen(req));
        CHECK(vec_has(&g_wire_s2c, "\"protocolVersion\":\"2025-11-25\""),
              "unsupported version → latest");
        reset_wires();
    }

    /* 握手完成 → session 推 READY */
    {
        const char *n = "{\"jsonrpc\":\"2.0\",\"method\":\"notifications/initialized\"}";
        bt_mcp_server_rx(&g_sv, (const uint8_t *)n, (uint32_t)strlen(n));
        CHECK(g_sv.initialized == 1, "initialized flag");
        CHECK(bt_mcp_session_state(&g_se_s) == BT_MCP_S_READY,
              "session READY after notifications/initialized");
        reset_wires();
    }

    /* ===== 2. tools/list 分页 ===== */
    {
        const char *req = "{\"jsonrpc\":\"2.0\",\"id\":3,\"method\":\"tools/list\"}";
        bt_mcp_server_rx(&g_sv, (const uint8_t *)req, (uint32_t)strlen(req));
        CHECK(vec_has(&g_wire_s2c, "\"name\":\"echo\""), "tools/list has echo");
        CHECK(vec_has(&g_wire_s2c, "inputSchema"), "tools/list inputSchema");
        CHECK(!vec_has(&g_wire_s2c, "nextCursor"), "single page → no nextCursor");
        reset_wires();
    }

    /* ===== 3. tools/call 同步 ===== */
    {
        const char *c = "{\"jsonrpc\":\"2.0\",\"id\":4,\"method\":\"tools/call\","
            "\"params\":{\"name\":\"echo\",\"arguments\":{\"text\":\"hi\\tthere\"}}}";
        bt_mcp_server_rx(&g_sv, (const uint8_t *)c, (uint32_t)strlen(c));
        CHECK(vec_has(&g_wire_s2c, "\"text\":\"hi\\tthere\""),
              "tool text escaped once (tab)");
        CHECK(vec_has(&g_wire_s2c, "\"isError\":false"), "isError=false");
        reset_wires();
    }
    {
        const char *c = "{\"jsonrpc\":\"2.0\",\"id\":5,\"method\":\"tools/call\","
            "\"params\":{\"name\":\"echo\",\"arguments\":{}}}";
        bt_mcp_server_rx(&g_sv, (const uint8_t *)c, (uint32_t)strlen(c));
        CHECK(vec_has(&g_wire_s2c, "\"text\":\"missing text\""), "tool error text");
        CHECK(vec_has(&g_wire_s2c, "\"isError\":true"), "isError=true");
        reset_wires();
    }
    {
        const char *c = "{\"jsonrpc\":\"2.0\",\"id\":6,\"method\":\"tools/call\","
            "\"params\":{\"name\":\"nope\"}}";
        bt_mcp_server_rx(&g_sv, (const uint8_t *)c, (uint32_t)strlen(c));
        CHECK(vec_has(&g_wire_s2c, "-32602"), "unknown tool → -32602");
        reset_wires();
    }

    /* ===== 4. 字符串 id 回显 ===== */
    {
        const char *c = "{\"jsonrpc\":\"2.0\",\"id\":\"req-9\",\"method\":\"ping\"}";
        bt_mcp_server_rx(&g_sv, (const uint8_t *)c, (uint32_t)strlen(c));
        CHECK(vec_has(&g_wire_s2c, "\"id\":\"req-9\""), "string id echoed as string");
        reset_wires();
    }

    /* ===== 5. 信用闭环 ===== */
    {
        bt_mcp_session_t s;
        bt_mcp_session_cfg_t cfg = { .req_timeout_ms = 30000, .resume_tout_ms = 5000,
                                     .tx_window = 2, .role = BT_MCP_ROLE_SERVER };
        bt_mcp_session_init(&s, &cfg);
        vec_t w = { .len = 0 };
        bt_mcp_session_bind(&s, &g_fr_s, vec_put, &w);
        bt_mcp_session_link_up(&s, 0);
        bt_mcp_session_mark_ready(&s);
        CHECK(s.credits == 2, "credits init = tx_window");
        CHECK(bt_mcp_session_tx(&s, (const uint8_t *)"a", 1) == 0, "tx 1 ok");
        CHECK(bt_mcp_session_tx(&s, (const uint8_t *)"b", 1) == 0, "tx 2 ok");
        CHECK(bt_mcp_session_tx(&s, (const uint8_t *)"c", 1) == BT_MCP_E_AGAIN,
              "credits exhausted → E_AGAIN");
        /* 对端消费 1 条 → 自动回补 */
        uint8_t msg[] = "{\"jsonrpc\":\"2.0\",\"method\":\"ping\"}";
        size_t before = w.len;
        int rc = bt_mcp_session_rx(&s, BT_MCP_KIND_MCP, 7, msg,
                                   (uint16_t)sizeof msg - 1, NULL, NULL);
        CHECK(rc == 0, "rx delivered");
        /* 消费一条消息 → 自动回补对端 1 credit（CREDIT 帧），并回 ACK */
        CHECK(w.len > before, "consume emits CREDIT/ACK frames");
        CHECK(w.buf[before + BT_MCP_HDR_LEN] == BT_MCP_CTRL_CREDIT,
              "first emitted control frame is CREDIT");
        /* 饱和：手动灌满 + 回补不溢出 */
        uint8_t cr[3] = { BT_MCP_CTRL_CREDIT, 0xFF, 0x00 };
        bt_mcp_session_rx(&s, BT_MCP_KIND_CTRL, 0, cr, 3, NULL, NULL);
        CHECK(s.credits == 255, "credits saturated at 255");
        reset_wires();
    }

    /* ===== 6. 不可靠包通道：ACK / 重传 / 去重 ===== */
    {
        bt_mcp_session_t s;
        bt_mcp_session_cfg_t cfg = { .req_timeout_ms = 30000, .resume_tout_ms = 5000,
                                     .tx_window = 4, .role = BT_MCP_ROLE_CLIENT };
        bt_mcp_session_init(&s, &cfg);
        vec_t w = { .len = 0 };
        bt_mcp_session_bind(&s, &g_fr_s, vec_put, &w);
        bt_mcp_session_link_up(&s, 0x11223344);
        bt_mcp_session_mark_ready(&s);

        /* 发一条 → 登记重传槽 */
        CHECK(bt_mcp_session_tx(&s, (const uint8_t *)"hello", 5) == 0, "tx hello");
        CHECK(s.txslot[0].valid == 1, "tx slot registered");
        uint8_t seq0 = s.txslot[0].seq;

        /* 收到 ACK → 释放槽 */
        uint8_t ack[2] = { BT_MCP_CTRL_ACK, seq0 };
        w.len = 0;
        bt_mcp_session_rx(&s, BT_MCP_KIND_CTRL, 0, ack, 2, NULL, NULL);
        CHECK(s.txslot[0].valid == 0, "ACK releases tx slot");

        /* 超时重传：再发一条，tick 越过 ack_tout → 自动重发 */
        CHECK(bt_mcp_session_tx(&s, (const uint8_t *)"again", 5) == 0, "tx again");
        w.len = 0;
        bt_mcp_session_tick(&s, 1000);
        CHECK(w.len > 0, "timed-out message retransmitted");
        CHECK(s.stat_tx_retransmit >= 1, "retransmit counted");

        /* NACK 单片重传 */
        {
            bt_mcp_session_t n;
            bt_mcp_session_init(&n, &cfg);
            vec_t wn = { .len = 0 };
            bt_mcp_session_bind(&n, &g_fr_s, vec_put, &wn);
            bt_mcp_session_link_up(&n, 0);
            bt_mcp_session_mark_ready(&n);
            static char big[600];
            memset(big, 'x', sizeof big);
            CHECK(bt_mcp_session_tx(&n, (const uint8_t *)big, 600) == 0, "tx big");
            CHECK(n.txslot[0].valid == 0, "oversize message not retransmit-protected");
            n.txslot[0].valid = 1; n.txslot[0].seq = 42; n.txslot[0].len = 600;
            memcpy(n.txslot[0].buf, big, BT_MCP_CFG_TX_RETRY_BYTES);
            n.txslot[0].buf[0] = 'y';
            uint8_t nack[3] = { BT_MCP_CTRL_NACK, 42, 1 };
            wn.len = 0;
            bt_mcp_session_rx(&n, BT_MCP_KIND_CTRL, 0, nack, 3, NULL, NULL);
            CHECK(wn.len == 244, "NACK replays exactly one chunk (6B hdr + 238B)");
        }

        /* 接收去重：同 seq 同内容窗口内只上抛一次 */
        {
            bt_mcp_session_t d;
            bt_mcp_session_init(&d, &cfg);
            vec_t wd = { .len = 0 };
            bt_mcp_session_bind(&d, &g_fr_s, vec_put, &wd);
            bt_mcp_session_link_up(&d, 0);
            bt_mcp_session_mark_ready(&d);
            int cnt = 0;
            (void)cnt;
            uint8_t m[] = "{\"jsonrpc\":\"2.0\",\"method\":\"ping\"}";
            uint16_t ml = (uint16_t)(sizeof m - 1);
            bt_mcp_session_rx(&d, BT_MCP_KIND_MCP, 3, m, ml, NULL, NULL);
            bt_mcp_session_rx(&d, BT_MCP_KIND_MCP, 3, m, ml, NULL, NULL);
            CHECK(d.stat_rx_dup == 1, "duplicate message dropped");
            CHECK(wd.len > 0, "duplicate still ACKed");
        }
        reset_wires();
    }

    /* ===== 7. resources ===== */
    {
        const char *lr = "{\"jsonrpc\":\"2.0\",\"id\":7,\"method\":\"resources/list\"}";
        bt_mcp_server_rx(&g_sv, (const uint8_t *)lr, (uint32_t)strlen(lr));
        CHECK(vec_has(&g_wire_s2c, "\"uri\":\"bt-mcp://dev/log\""), "resource uri");
        reset_wires();

        const char *rr = "{\"jsonrpc\":\"2.0\",\"id\":8,\"method\":\"resources/read\","
            "\"params\":{\"uri\":\"bt-mcp://dev/log\"}}";
        bt_mcp_server_rx(&g_sv, (const uint8_t *)rr, (uint32_t)strlen(rr));
        CHECK(vec_has(&g_wire_s2c, "\"text\":\"01234567\""), "first page text");
        CHECK(vec_has(&g_wire_s2c, "\"nextCursor\":\"8\""), "nextCursor=8");
        reset_wires();

        const char *r2 = "{\"jsonrpc\":\"2.0\",\"id\":9,\"method\":\"resources/read\","
            "\"params\":{\"uri\":\"bt-mcp://dev/log?cursor=8\"}}";
        bt_mcp_server_rx(&g_sv, (const uint8_t *)r2, (uint32_t)strlen(r2));
        CHECK(vec_has(&g_wire_s2c, "\"text\":\"89ABCDEF\""), "second page text");
        reset_wires();

        const char *bad = "{\"jsonrpc\":\"2.0\",\"id\":10,\"method\":\"resources/read\","
            "\"params\":{\"uri\":\"bt-mcp://dev/nope\"}}";
        bt_mcp_server_rx(&g_sv, (const uint8_t *)bad, (uint32_t)strlen(bad));
        CHECK(vec_has(&g_wire_s2c, "-32002"), "unknown resource → -32002");
        reset_wires();
    }

    /* ===== 8. 异步调用 + 进度 + 取消 ===== */
    {
        const char *c = "{\"jsonrpc\":\"2.0\",\"id\":11,\"method\":\"tools/call\","
            "\"params\":{\"name\":\"slow\",\"arguments\":{},"
            "\"_meta\":{\"progressToken\":77}}}";
        int rc = bt_mcp_server_rx(&g_sv, (const uint8_t *)c, (uint32_t)strlen(c));
        CHECK(rc == 0, "pending call returns OK without response");
        CHECK(g_async_call != NULL, "call handle provided");
        CHECK(g_async_progress == 1, "progress notification emitted");
        CHECK(vec_has(&g_wire_s2c, "\"progressToken\":77"), "progress token echoed");
        CHECK(!vec_has(&g_wire_s2c, "\"result\""), "no result yet (async)");
        reset_wires();

        /* 完成 → 发响应（先回补信用：真实链路由对端自动发 CREDIT） */
        host_topup();
        rc = bt_mcp_server_complete(g_async_call, "done", 0);
        CHECK(rc == 0, "complete sends result");
        CHECK(vec_has(&g_wire_s2c, "\"text\":\"done\""), "async result text");
        CHECK(vec_has(&g_wire_s2c, "\"id\":11"), "async result id");
        reset_wires();

        /* 取消：再挂起一次，cancelled 后 complete 不发任何字节 */
        host_topup();
        bt_mcp_server_rx(&g_sv, (const uint8_t *)c, (uint32_t)strlen(c));
        reset_wires();
        const char *cx = "{\"jsonrpc\":\"2.0\",\"method\":\"notifications/cancelled\","
            "\"params\":{\"requestId\":11}}";
        bt_mcp_server_rx(&g_sv, (const uint8_t *)cx, (uint32_t)strlen(cx));
        CHECK(bt_mcp_call_cancelled(g_async_call) == 1, "call marked cancelled");
        bt_mcp_server_complete(g_async_call, "late", 0);
        CHECK(g_wire_s2c.len == 0, "cancelled call emits nothing");
        reset_wires();
    }

    /* ===== 9. RESUME ===== */
    {
        bt_mcp_session_t s;
        bt_mcp_session_cfg_t cfg = { .req_timeout_ms = 30000, .resume_tout_ms = 5000,
                                     .tx_window = 4, .role = BT_MCP_ROLE_SERVER };
        bt_mcp_session_init(&s, &cfg);
        vec_t w = { .len = 0 };
        bt_mcp_session_bind(&s, &g_fr_s, vec_put, &w);

        bt_mcp_session_link_up(&s, 0xDEADBEEF);
        uint8_t ok[5] = { BT_MCP_CTRL_RESUME_REQ, 0xEF, 0xBE, 0xAD, 0xDE };
        bt_mcp_session_rx(&s, BT_MCP_KIND_CTRL, 0, ok, 5, NULL, NULL);
        CHECK(bt_mcp_session_state(&s) == BT_MCP_S_READY, "RESUME match → READY");

        /* 连续错误 → 锁定 */
        bt_mcp_session_link_up(&s, 0xDEADBEEF);
        uint8_t bad[5] = { BT_MCP_CTRL_RESUME_REQ, 1, 2, 3, 4 };
        for (int i = 0; i < BT_MCP_CFG_RESUME_MAX_FAIL; i++) {
            w.len = 0;
            bt_mcp_session_rx(&s, BT_MCP_KIND_CTRL, 0, bad, 5, NULL, NULL);
            CHECK(vec_has(&w, "\x02"), "mismatch → REJ reason 2");
        }
        CHECK(s.resume_locked == 1, "locked after repeated failures");
        w.len = 0;
        bt_mcp_session_rx(&s, BT_MCP_KIND_CTRL, 0, ok, 5, NULL, NULL);
        CHECK(vec_has(&w, "\x03"), "locked → REJ reason 3 even with good token");
        /* 锁定过期后恢复：tick 越过锁定窗口，正确 token 应重新被接受 */
        bt_mcp_session_tick(&s, BT_MCP_CFG_RESUME_LOCK_MS + 1);
        w.len = 0;
        bt_mcp_session_rx(&s, BT_MCP_KIND_CTRL, 0, ok, 5, NULL, NULL);
        CHECK(s.resume_locked == 0, "lock expires");
        CHECK(bt_mcp_session_state(&s) == BT_MCP_S_READY, "RESUME accepted after lock");
        reset_wires();
    }

    /* ===== 10. PING RTT ===== */
    {
        bt_mcp_session_t s;
        bt_mcp_session_cfg_t cfg = { .req_timeout_ms = 30000, .resume_tout_ms = 5000,
                                     .tx_window = 4, .role = BT_MCP_ROLE_CLIENT };
        bt_mcp_session_init(&s, &cfg);
        vec_t w = { .len = 0 };
        bt_mcp_session_bind(&s, &g_fr_s, vec_put, &w);
        bt_mcp_session_link_up(&s, 0);
        bt_mcp_session_mark_ready(&s);
        bt_mcp_session_tick(&s, 100);
        CHECK(bt_mcp_session_ping(&s, 9) == 0, "ping sent");
        bt_mcp_session_tick(&s, 115);
        uint8_t pong[2] = { BT_MCP_CTRL_PING, 9 };
        bt_mcp_session_rx(&s, BT_MCP_KIND_CTRL, 0, pong, 2, NULL, NULL);
        int valid = 0;
        CHECK(bt_mcp_session_rtt(&s, &valid) == 15, "RTT measured 15ms");
        CHECK(valid == 1, "RTT valid flag");
        reset_wires();
    }

    /* ===== 11. 状态门 ===== */
    {
        bt_mcp_session_t s;
        bt_mcp_session_cfg_t cfg = { .req_timeout_ms = 30000, .resume_tout_ms = 5000,
                                     .tx_window = 4, .role = BT_MCP_ROLE_SERVER };
        bt_mcp_session_init(&s, &cfg);
        CHECK(bt_mcp_session_rx(&s, BT_MCP_KIND_MCP, 0, (const uint8_t *)"{}", 2,
                                NULL, NULL) == BT_MCP_E_STATE,
              "IDLE session rx → E_STATE");
    }

    printf("\n%d pass, %d fail\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
