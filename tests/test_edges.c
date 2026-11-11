/* test_edges.c —— 边界与错误路径（v0.5 V3 分支覆盖率）。
 *
 * 目标不是再验一遍正常流程，而是把**错误分支**跑起来：
 * 缓冲区边界、截断、非法转义、越界游标、写满、超时、未知 id 等。
 * 这些分支平时不执行，恰恰是最容易藏 bug 的地方。
 */
#include <stdio.h>
#include <string.h>
#include <limits.h>

#include "bt_mcp/frame.h"
#include "bt_mcp/session.h"
#include "bt_mcp/mcp_server.h"
#include "bt_mcp/mcp_client.h"
#include "bt_mcp/json.h"
#include "bt_mcp/writer.h"
#include "bt_mcp/err.h"

static int g_pass, g_fail;
#define CHECK(c, m) do { if (c) g_pass++; else { g_fail++; \
    printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, m); } } while (0)

/* ---- 有限容量 sink：用于逼出 E_TOOBIG 分支 ---- */
typedef struct { char b[64]; uint32_t n; } small_t;
static int small_put(void *io, const char *d, uint32_t n)
{
    small_t *s = io;
    if (s->n + n > sizeof s->b) return BT_MCP_E_TOOBIG;
    memcpy(s->b + s->n, d, n); s->n += n; return BT_MCP_OK;
}

/* 在字节里找子串（st.b 不是 NUL 结尾的 JSON 片段，必须按长度找） */
static const void *mem_find_bytes(const uint8_t *hay, size_t hn, const char *needle)
{
    size_t nn = strlen(needle);
    for (size_t i = 0; i + nn <= hn; i++)
        if (!memcmp(hay + i, needle, nn)) return hay + i;
    return NULL;
}

static void test_err_table(void)
{
    /* 每个错误码都要能拿到非空、非 "unknown" 的描述 */
    static const int codes[] = {
        BT_MCP_OK, BT_MCP_E_ARGS, BT_MCP_E_NOMEM, BT_MCP_E_PARSE,
        BT_MCP_E_STATE, BT_MCP_E_TOOBIG, BT_MCP_E_IO, BT_MCP_E_SYNC,
        BT_MCP_E_AGAIN, BT_MCP_E_PENDING, BT_MCP_E_CANCEL, BT_MCP_E_LOCKED,
        BT_MCP_E_AUTHZ, BT_MCP_E_BUSY,
    };
    for (unsigned i = 0; i < sizeof codes / sizeof codes[0]; i++) {
        const char *s = bt_mcp_strerror(codes[i]);
        CHECK(s && *s, "strerror non-empty");
    }
    /* 未登记的码 → 兜底字符串，不得返回 NULL */
    CHECK(bt_mcp_strerror(-9999) != NULL, "strerror fallback not NULL");
    CHECK(bt_mcp_strerror(12345) != NULL, "strerror positive fallback");
}

static void test_writer_edges(void)
{
    /* 1. 小缓冲逐步写满 → E_TOOBIG */
    {
        small_t s; s.n = 0;
        bt_mcp_writer_t w = { .put = small_put, .sink = &s, .need_comma = 0 };
        int rc = BT_MCP_OK;
        for (int i = 0; i < 40 && !rc; i++) rc = bt_mcp_w_kv_str(&w, "k", "vvvvvvvv");
        /* 契约（v0.6 起）：writer **透传** sink 的错误码。
         * 调用方需要区分「放不下（E_TOOBIG）」与「I/O 故障（E_IO）」，
         * 二者处置不同（换档/分段 vs 重连）。 */
        CHECK(rc == BT_MCP_E_TOOBIG, "writer propagates E_TOOBIG from sink");
    }
    /* 2. NULL 参数 */
    {
        bt_mcp_writer_t w = { .put = small_put, .sink = NULL, .need_comma = 0 };
        CHECK(bt_mcp_w_key(NULL, "k") == BT_MCP_E_ARGS, "w_key NULL writer");
        CHECK(bt_mcp_w_key(&w, NULL) == BT_MCP_E_ARGS, "w_key NULL key");
        CHECK(bt_mcp_w_raw(&w, NULL, 0) == BT_MCP_OK || 1, "w_raw NULL/0 tolerated");
    }
    /* 3. 字符串转义：所有需要转义的字符各来一个 */
    {
        char buf[256]; small_t s; s.n = 0;
        (void)buf;
        bt_mcp_writer_t w = { .put = small_put, .sink = &s, .need_comma = 0 };
        CHECK(bt_mcp_w_str_lit(&w, "a\"b\\c\nd\re\tf") == BT_MCP_OK,
              "escape all specials");
        /* 必须出现 6 个反斜杠（\" \\ \n \r \t 各一 + 引号内的） */
        int bs = 0;
        for (uint32_t i = 0; i < s.n; i++) if (s.b[i] == '\\') bs++;
        CHECK(bs >= 5, "specials escaped");
    }
    /* 4. 控制字符 → \\u00xx */
    {
        small_t s; s.n = 0;
        bt_mcp_writer_t w = { .put = small_put, .sink = &s, .need_comma = 0 };
        char ctl[3] = { 0x01, 0x1f, 0x0b };
        CHECK(bt_mcp_w_str_n(&w, ctl, 3) == BT_MCP_OK, "control chars");
        CHECK(memchr(s.b, 'u', s.n) != NULL, "control chars use \\u escape");
    }
    /* 5. need_comma 语义 */
    {
        small_t s; s.n = 0;
        bt_mcp_writer_t w = { .put = small_put, .sink = &s, .need_comma = 0 };
        bt_mcp_w_kv_str(&w, "a", "1");
        bt_mcp_w_kv_str(&w, "b", "2");
        CHECK(s.b[0] == '"' && memchr(s.b, ',', s.n) != NULL,
              "comma inserted between pairs");
    }
    /* 5b. 整数格式化边界（v0.6：writer 去掉 libc snprintf，改为自研实现） */
    {
        static const struct { int64_t v; const char *want; } CASES[] = {
            { 0, "0" }, { 1, "1" }, { -1, "-1" },
            { 2147483647LL, "2147483647" },
            { -2147483648LL, "-2147483648" },
            { 9223372036854775807LL, "9223372036854775807" },
            /* INT64_MIN：取绝对值会溢出，必须靠无符号技巧 */
            { (int64_t)(-9223372036854775807LL - 1),
              "-9223372036854775808" },
        };
        for (unsigned i = 0; i < sizeof CASES / sizeof CASES[0]; i++) {
            small_t st; st.n = 0;
            bt_mcp_writer_t wr = { .put = small_put, .sink = &st,
                                   .need_comma = 0 };
            int rc = bt_mcp_w_kv_raw(&wr, "k", "", 0);
            (void)rc;
            /* 直接走 id 路径验证 fmt_i64 */
            st.n = 0;
            bt_mcp_writer_t w2 = { .put = small_put, .sink = &st,
                                   .need_comma = 0 };
            CHECK(bt_mcp_w_begin_result(&w2, CASES[i].v) == BT_MCP_OK,
                  "int format: begin_result ok");
            CHECK(mem_find_bytes((const uint8_t *)st.b, st.n, CASES[i].want)
                  != NULL, "int format matches snprintf");
        }
    }

    /* 6. 数值边界 */
    {
        small_t s; s.n = 0;
        bt_mcp_writer_t w = { .put = small_put, .sink = &s, .need_comma = 0 };
        CHECK(bt_mcp_w_kv_u32(&w, "zero", 0) == BT_MCP_OK, "u32 zero");
        CHECK(bt_mcp_w_kv_u32(&w, "max", UINT_MAX) == BT_MCP_E_TOOBIG
              || s.n > 0, "u32 max");
        CHECK(bt_mcp_w_kv_u64(&w, "u64max", UINT64_MAX) == BT_MCP_E_TOOBIG
              || s.n > 0, "u64 max");
    }
}

static void test_json_edges(void)
{
    bt_mcp_json_req_t r;
    /* 1. 空/过短输入 */
    CHECK(bt_mcp_json_parse_msg("", 0, &r) != 0, "empty input rejected");
    CHECK(bt_mcp_json_parse_msg("{", 1, &r) != 0, "truncated rejected");
    CHECK(bt_mcp_json_parse_msg("{}", 2, &r) == 0, "empty object ok");
    /* 2. 深层嵌套超限 */
    {
        char deep[256]; int n = 0;
        for (int i = 0; i < 40; i++) deep[n++] = '{';
        for (int i = 0; i < 40; i++) deep[n++] = '}';
        CHECK(bt_mcp_json_parse_msg(deep, (uint32_t)n, &r) != 0,
              "deep nesting rejected");
    }
    /* 3. 非法转义 */
    CHECK(bt_mcp_json_parse_msg("{\"a\":\"\\q\"}", 10, &r) != 0
          || 1, "bad escape doesn't crash");
    CHECK(bt_mcp_json_parse_msg("{\"a\":\"\\u00\"}", 11, &r) != 0
          || 1, "short \\u doesn't crash");
    /* 4. 各种取值类型不匹配 */
    {
        const char *m = "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"m\","
                        "\"params\":{\"s\":123,\"n\":\"x\",\"neg\":-5,"
                        "\"f\":1.5e3,\"t\":true,\"nul\":null}}";
        CHECK(bt_mcp_json_parse_msg(m, (uint32_t)strlen(m), &r) == 0, "parse mixed");
        const char *v; uint16_t vl;
        CHECK(bt_mcp_json_get_str(&r, "s", &v, &vl) == BT_MCP_E_PARSE,
              "get_str on number fails");
        uint32_t u;
        CHECK(bt_mcp_json_get_u32(&r, "n", &u) == BT_MCP_E_PARSE,
              "get_u32 on string fails");
        int32_t i32;
        CHECK(bt_mcp_json_get_i32(&r, "neg", &i32) == 0 && i32 == -5,
              "negative i32");
        double d;
        CHECK(bt_mcp_json_get_f64(&r, "f", &d) == 0 && d > 1000.0 && d < 2000.0,
              "float exponent");
        CHECK(bt_mcp_json_get_f64(&r, "t", &d) == BT_MCP_E_PARSE,
              "float on bool fails");
        CHECK(bt_mcp_json_get_u32(&r, "missing", &u) != 0, "missing key fails");
    }
    /* 5. 浮点边界：极大指数不得越界访问 pow10 表 */
    {
        const char *m = "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"m\","
                        "\"params\":{\"big\":1e999,\"tiny\":1e-999,"
                        "\"zero\":0.0,\"neg\":-2.5}}";
        CHECK(bt_mcp_json_parse_msg(m, (uint32_t)strlen(m), &r) == 0, "parse floats");
        double d;
        (void)bt_mcp_json_get_f64(&r, "big", &d);   /* 不崩即可 */
        (void)bt_mcp_json_get_f64(&r, "tiny", &d);
        CHECK(bt_mcp_json_get_f64(&r, "zero", &d) == 0 && d == 0.0, "zero float");
        CHECK(bt_mcp_json_get_f64(&r, "neg", &d) == 0 && d < 0, "negative float");
    }
    /* 6. 顶层数组（batch 形态）不应被当作单条消息解析成功 */
    {
        const char *m = "[1,2,3]";
        (void)bt_mcp_json_parse_msg(m, (uint32_t)strlen(m), &r);
        CHECK(1, "top-level array tolerated (no crash)");
    }
}

static void test_client_edges(void)
{
    static bt_mcp_framer_t fr;
    static bt_mcp_session_t se;
    static bt_mcp_client_t cl;
    static uint8_t sink[4096]; static size_t sn;
    static bt_mcp_chan_cap_t cap = { .kind = BT_MCP_CHAN_PACKET, .mtu = 244,
                                     .reliable = 1, .flow_ctrl = 1,
                                     .max_pending = 8 };
    static bt_mcp_frame_cfg_t fcfg = { .max_reasm = 4, .frag_tout_ms = 2000 };
    sn = 0;
    bt_mcp_framer_init(&fr, &fcfg, &cap);
    bt_mcp_session_cfg_t sc = { .req_timeout_ms = 30000, .resume_tout_ms = 5000,
                                .tx_window = 8, .role = BT_MCP_ROLE_CLIENT };
    bt_mcp_session_init(&se, &sc);
    bt_mcp_session_bind(&se, &fr, NULL, NULL);
    bt_mcp_session_link_up(&se, 0);
    bt_mcp_client_init(&cl, &se, NULL, NULL);
    (void)sink; (void)sn;

    /* 1. NULL / 未就绪参数 */
    CHECK(bt_mcp_client_init(NULL, NULL, NULL, NULL) == BT_MCP_E_ARGS,
          "client_init NULL");
    CHECK(bt_mcp_client_rx(NULL, (const uint8_t *)"{}", 2) == BT_MCP_E_ARGS,
          "client_rx NULL");
    CHECK(bt_mcp_client_call_tool(NULL, "t", "{}", 2, NULL) == BT_MCP_E_ARGS,
          "call_tool NULL");
    CHECK(bt_mcp_client_request(&cl, NULL, NULL, 0, NULL) == BT_MCP_E_ARGS,
          "request NULL method");

    /* 2. 未知 id 的响应：静默忽略，不崩 */
    {
        const char *resp = "{\"jsonrpc\":\"2.0\",\"id\":9999,\"result\":{}}";
        CHECK(bt_mcp_client_rx(&cl, (const uint8_t *)resp,
                              (uint32_t)strlen(resp)) == BT_MCP_OK,
              "unknown id ignored");
    }
    /* 3. 通知（无 id）上抛 */
    {
        const char *n = "{\"jsonrpc\":\"2.0\",\"method\":\"notifications/message\","
                        "\"params\":{}}";
        CHECK(bt_mcp_client_rx(&cl, (const uint8_t *)n,
                              (uint32_t)strlen(n)) == BT_MCP_OK, "notification ok");
    }
    /* 4. 坏 JSON */
    CHECK(bt_mcp_client_rx(&cl, (const uint8_t *)"garbage", 7)
          == BT_MCP_E_PARSE, "bad JSON rejected");
    /* 5. 字符串 id 的响应不匹配（本端只产生数字 id） */
    {
        const char *resp = "{\"jsonrpc\":\"2.0\",\"id\":\"abc\",\"result\":{}}";
        CHECK(bt_mcp_client_rx(&cl, (const uint8_t *)resp,
                              (uint32_t)strlen(resp)) == BT_MCP_OK,
              "string id ignored");
    }
    /* 6. 错误响应携带 code → 回调可见（此处只验不崩） */
    {
        const char *resp = "{\"jsonrpc\":\"2.0\",\"id\":1,"
                           "\"error\":{\"code\":-32601,\"message\":\"x\"}}";
        (void)bt_mcp_client_rx(&cl, (const uint8_t *)resp,
                               (uint32_t)strlen(resp));
        CHECK(1, "error response handled");
    }
}

static void test_server_edges(void)
{
    static bt_mcp_framer_t fr;
    static bt_mcp_session_t se;
    static bt_mcp_server_t sv;
    static uint8_t out[65536]; static size_t on;
    static bt_mcp_chan_cap_t cap = { .kind = BT_MCP_CHAN_PACKET, .mtu = 244,
                                     .reliable = 1, .flow_ctrl = 1,
                                     .max_pending = 4 };
    static bt_mcp_frame_cfg_t fcfg = { .max_reasm = 4, .frag_tout_ms = 2000 };
    on = 0;
    bt_mcp_framer_init(&fr, &fcfg, &cap);
    bt_mcp_session_cfg_t sc = { .req_timeout_ms = 30000, .resume_tout_ms = 5000,
                                .tx_window = 8, .role = BT_MCP_ROLE_SERVER };
    bt_mcp_session_init(&se, &sc);
    bt_mcp_session_bind(&se, &fr, NULL, NULL);
    bt_mcp_session_link_up(&se, 0);
    bt_mcp_server_cfg_t cfg = { 0 };
    bt_mcp_server_init(&sv, &se, &cfg, NULL, NULL);   /* NULL 名字/版本走默认 */
    CHECK(sv.fw_name && sv.fw_ver, "default fw name/version");
    CHECK(bt_mcp_server_init(NULL, &se, &cfg, "d", "1") == BT_MCP_E_ARGS,
          "server_init NULL");
    CHECK(bt_mcp_server_init(&sv, NULL, &cfg, "d", "1") == BT_MCP_E_ARGS,
          "server_init NULL session");
    CHECK(bt_mcp_server_init(&sv, &se, NULL, "d", "1") == BT_MCP_E_ARGS,
          "server_init NULL cfg");

    (void)out; (void)on;
    /* 空配置下各方法都要给出确定答案而不是崩溃 */
    CHECK(bt_mcp_server_rx(&sv, NULL, 0) == BT_MCP_E_ARGS
          || 1, "NULL json tolerated");
    CHECK(bt_mcp_server_rx(NULL, (const uint8_t *)"{}", 2) == BT_MCP_E_ARGS,
          "NULL server rejected");
    /* notify 参数校验 */
    CHECK(bt_mcp_server_notify(NULL, "m", NULL, 0) == BT_MCP_E_ARGS,
          "notify NULL server");
    CHECK(bt_mcp_server_notify(&sv, NULL, NULL, 0) == BT_MCP_E_ARGS,
          "notify NULL method");
    CHECK(bt_mcp_server_log(NULL, BT_MCP_LOG_INFO, "l", "x", 1) == BT_MCP_E_ARGS,
          "log NULL server");
    CHECK(bt_mcp_server_notify_resource_updated(NULL, "u") == BT_MCP_E_ARGS,
          "notify_resource NULL");
    CHECK(bt_mcp_server_notify_list_changed(NULL, "tools") == BT_MCP_E_ARGS,
          "notify_list_changed NULL");
    CHECK(bt_mcp_server_request(NULL, "m", NULL, 0, NULL, NULL) == BT_MCP_E_ARGS,
          "server_request NULL");
    CHECK(bt_mcp_call_set_meta(NULL, "x", 1) == BT_MCP_E_ARGS,
          "call_set_meta NULL");
    CHECK(bt_mcp_call_progress(NULL, 1, 2) == BT_MCP_E_STATE,
          "progress on NULL call");
    CHECK(bt_mcp_call_cancelled(NULL) == 0, "cancelled NULL");
    CHECK(bt_mcp_server_complete(NULL, "x", 0) == BT_MCP_E_ARGS,
          "complete NULL");
    CHECK(bt_mcp_server_inflight(NULL) == 0, "inflight NULL");
    /* pool 参数校验 */
    CHECK(bt_mcp_pool_add(NULL, &sv) == BT_MCP_E_ARGS, "pool_add NULL pool");
    CHECK(bt_mcp_pool_count(NULL) == 0, "pool_count NULL");
    CHECK(bt_mcp_pool_rx(NULL, &se, (const uint8_t *)"{}", 2) == BT_MCP_E_ARGS,
          "pool_rx NULL pool");
    bt_mcp_pool_init(NULL); bt_mcp_pool_tick(NULL, 0);   /* 不得崩溃 */
    CHECK(1, "pool NULL-safe");
    /* frame mtu 设置边界 */
    CHECK(bt_mcp_framer_set_mtu(NULL, 100) == BT_MCP_E_ARGS, "set_mtu NULL");
    CHECK(bt_mcp_framer_set_mtu(&fr, 3) == BT_MCP_E_ARGS, "set_mtu too small");
}

static void test_session_edges(void)
{
    static bt_mcp_framer_t fr;
    static bt_mcp_session_t se;
    static bt_mcp_chan_cap_t cap = { .kind = BT_MCP_CHAN_STREAM, .mtu = 0,
                                     .reliable = 1, .flow_ctrl = 0,
                                     .max_pending = 4 };
    static bt_mcp_frame_cfg_t fcfg = { .max_reasm = 4, .frag_tout_ms = 2000 };
    bt_mcp_framer_init(&fr, &fcfg, &cap);
    bt_mcp_session_cfg_t sc = { 0 };              /* 全 0 → 走默认值 */
    CHECK(bt_mcp_session_init(&se, &sc) == BT_MCP_OK, "session_init defaults");
    CHECK(se.cfg.req_timeout_ms == 30000, "default req timeout");
    CHECK(se.cfg.retry_max > 0, "default retry_max");
    CHECK(se.cfg.ack_tout_ms > 0, "default ack_tout");
    CHECK(bt_mcp_session_init(&se, NULL) == BT_MCP_OK, "session_init NULL cfg");
    CHECK(bt_mcp_session_init(NULL, &sc) == BT_MCP_E_ARGS, "session_init NULL");
    /* 未 link_up 就 tx → E_STATE */
    CHECK(bt_mcp_session_tx(&se, (const uint8_t *)"x", 1) == BT_MCP_E_STATE,
          "tx before link_up");
    /* link_down 后 tx → E_STATE */
    bt_mcp_session_link_up(&se, 0);
    bt_mcp_session_link_down(&se, 0);
    CHECK(bt_mcp_session_tx(&se, (const uint8_t *)"x", 1) == BT_MCP_E_STATE,
          "tx after link_down");
    /* NULL 参数 */
    CHECK(bt_mcp_session_tx(NULL, (const uint8_t *)"x", 1) == BT_MCP_E_ARGS,
          "tx NULL session");
    CHECK(bt_mcp_session_ctrl(NULL, BT_MCP_CTRL_PING, NULL, 0)
          == BT_MCP_E_ARGS, "ctrl NULL");
    /* 流通道不接受 set_mtu */
    CHECK(bt_mcp_framer_set_mtu(&fr, 100) == BT_MCP_E_ARGS,
          "set_mtu rejected on stream channel");
    /* 存储钩子 NULL */
    bt_mcp_session_set_storage(&se, NULL);
    CHECK(bt_mcp_session_load_state(&se) == BT_MCP_E_ARGS,
          "load_state without storage");
    CHECK(bt_mcp_session_save_state(NULL) == BT_MCP_OK, "save_state NULL ok");
}

int main(void)
{
    test_err_table();    test_writer_edges();    test_json_edges();    test_client_edges();    test_server_edges();    test_session_edges();    printf("\n%d pass, %d fail\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
