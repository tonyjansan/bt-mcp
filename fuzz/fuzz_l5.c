/* fuzz_l5.c —— 语法感知的 L5 fuzz（v0.6 N2）。
 *
 * 与 fuzz/fuzz_frame.c 的分工：
 *   - fuzz_frame.c 变异**字节**，验证 L3 编解码器；
 *   - 本文件变异**JSON 结构**，验证 L5 语义层（mcp_server / mcp_client）。
 *
 * 为什么需要「语法感知」：纯字节变异几乎都会在第一步就被 JSON 解析器拒绝，
 * 于是永远测不到解析之后的逻辑。本变异器保留 `{"jsonrpc":"2.0", ... }` 外壳，
 * 只变异字段值、类型、嵌套、键序、尾部结构，因此大多数用例能进到 L5 深处。
 *
 * 四条不变量（任何一条被违反都视为 bug）：
 *   I1 不崩溃：进程必须活着走完全部迭代。
 *   I2 返回码合法：rc ∈ 已知 BT_MCP_E* 集合。
 *   I3 状态不粘：喂完垃圾后再喂一条合法 ping，仍必须正常响应。
 *   I4 不泄漏：静态占用的池在迭代间必须回到基线（槽位不残留）。
 *
 * 用法：
 *   ./build/fuzz_l5 [iters] [seed]
 *   ./build/fuzz_l5 2000000          # 200 万次
 *   ./build/fuzz_l5 1000000 42       # 固定种子可复现
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "bt_mcp/frame.h"
#include "bt_mcp/session.h"
#include "bt_mcp/mcp_server.h"
#include "bt_mcp/mcp_client.h"
#include "bt_mcp/err.h"

/* ---------------- RNG（xorshift32，确定性） ---------------- */
static uint32_t g_rng;
static uint32_t rnd(void)
{
    g_rng ^= g_rng << 13;
    g_rng ^= g_rng >> 17;
    g_rng ^= g_rng << 5;
    return g_rng;
}
static uint32_t rnd_n(uint32_t n) { return n ? rnd() % n : 0; }

/* ---------------- 被驱动对象 ---------------- */
typedef struct { uint8_t b[1 << 17]; size_t n; } vec_t;
static int vput(void *io, const uint8_t *d, uint16_t n)
{
    vec_t *v = io;
    if (v->n + n > sizeof v->b) return BT_MCP_E_NOMEM;
    memcpy(v->b + v->n, d, n); v->n += n; return BT_MCP_OK;
}
static int t_ok(const bt_mcp_json_req_t *r, bt_mcp_writer_t *w,
                bt_mcp_call_t *c, void *u)
{ (void)r; (void)c; (void)u; return bt_mcp_w_raw(w, "ok", 2); }
static int t_rich(const bt_mcp_json_req_t *r, bt_mcp_writer_t *w,
                  bt_mcp_call_t *c, void *u)
{
    (void)r; (void)u;
    int rc = bt_mcp_w_content_text(w, "a", 1);
    if (!rc) rc = bt_mcp_w_content_image(w, "aGk=", 4, "image/png");
    if (!rc) rc = bt_mcp_w_content_audio(w, "AAEC", 4, "audio/wav");
    if (!rc) rc = bt_mcp_w_content_resource(w, "bt-mcp://dev/r", "text/plain",
                                            "x", 1);
    if (!rc) rc = bt_mcp_call_set_meta(c, "k\":1", 4);
    return rc;
}
static int t_pending(const bt_mcp_json_req_t *r, bt_mcp_writer_t *w,
                     bt_mcp_call_t *c, void *u)
{ (void)r; (void)w; (void)c; (void)u; return BT_MCP_E_PENDING; }

static const bt_mcp_tool_def_t TOOLS[] = {
    { .name = "ok", .desc = "d", .input_schema_json = "{}", .fn = t_ok },
    { .name = "rich", .desc = "d", .input_schema_json = "{}", .fn = t_rich },
    { .name = "pending", .desc = "d", .input_schema_json = "{}", .fn = t_pending },
    { .name = "consent", .desc = "d", .input_schema_json = "{}",
      .authz = BT_MCP_AUTHZ_CONSENT, .fn = t_ok },
};
static int res_read(void *u, uint32_t c, const char **t, uint32_t *l,
                    uint32_t *nx)
{ (void)u; (void)c; *t = "x"; *l = 1; *nx = BT_MCP_CURSOR_END; return 0; }
static int res_sub(void *u, int s) { (void)u; return s ? -1 : 0; }
static const bt_mcp_resource_def_t RES[] = {
    { .name = "r", .desc = "d", .mime_type = "text/plain",
      .read = res_read, .sub = res_sub },
};
static int prompt_get(const bt_mcp_json_req_t *r, bt_mcp_writer_t *w, void *u)
{
    (void)r; (void)u;
    int rc = bt_mcp_w_key(w, "messages");
    if (!rc) rc = bt_mcp_w_raw(w, "[]", 2);
    w->need_comma = 1;
    return rc;
}
static const bt_mcp_prompt_def_t PROMPTS[] = {
    { .name = "p", .desc = "d", .get = prompt_get },
};

static bt_mcp_framer_t g_fr;
static bt_mcp_session_t g_se;
static bt_mcp_server_t g_sv;
static bt_mcp_client_t g_cl;
static vec_t g_wire;
static bt_mcp_chan_cap_t g_cap;
static bt_mcp_frame_cfg_t g_fcfg;
static bt_mcp_server_cfg_t g_scfg;

static int server_up(const char *proto)
{
    g_wire.n = 0;
    bt_mcp_framer_init(&g_fr, &g_fcfg, &g_cap);
    bt_mcp_session_cfg_t sc = { .req_timeout_ms = 30000, .resume_tout_ms = 5000,
                                .tx_window = 8, .role = BT_MCP_ROLE_SERVER };
    bt_mcp_session_init(&g_se, &sc);
    bt_mcp_session_bind(&g_se, &g_fr, vput, &g_wire);
    bt_mcp_session_link_up(&g_se, 0);
    if (bt_mcp_server_init(&g_sv, &g_se, &g_scfg, "d", "0.5")) return -1;
    char q[192];
    int n = snprintf(q, sizeof q,
        "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"initialize\","
        "\"params\":{\"protocolVersion\":\"%s\"}}", proto);
    (void)bt_mcp_server_rx(&g_sv, (const uint8_t *)q, (uint32_t)n);
    const char *q2 = "{\"jsonrpc\":\"2.0\",\"method\":\"notifications/initialized\"}";
    (void)bt_mcp_server_rx(&g_sv, (const uint8_t *)q2, (uint32_t)strlen(q2));
    return 0;
}

/* ---------------- 结构感知变异器 ---------------- */

static const char *const METHODS[] = {
    "initialize", "notifications/initialized", "ping", "tools/list",
    "tools/call", "resources/list", "resources/read", "resources/subscribe",
    "resources/unsubscribe", "prompts/list", "prompts/get",
    "logging/setLevel", "notifications/cancelled", "completion/complete", "",
    "\xff\xfe", "tools/call\x00tail",
};
static const char *const TOOLNAMES[] = {
    "ok", "rich", "pending", "consent", "zzz", "", "ok\x01",
};
static const char *const URIS[] = {
    "bt-mcp://dev/r", "bt-mcp://dev/zzz", "bt-mcp://", "nope", "",
    "bt-mcp://dev/r?cursor=99", "bt-mcp://dev/r?cursor=-1",
    "bt-mcp://dev/r?cursor=abc",
};
static const char *const LEVELS[] = {
    "debug", "info", "notice", "warning", "error", "critical", "alert",
    "emergency", "bogus", "",
};
/* 值形态：每种代表一类 JSON 类型 */
static const char *const VALUES[] = {
    "null", "true", "false", "0", "-1", "2147483647", "-2147483648",
    "1.5", "1e999", "\"\"", "\"s\"", "[]", "{}", "[1,2]", "{\"k\":1}",
    "\"\\u0041\"", "\"\\ud83d\\ude00\"", "\"unterminated",
};

static size_t gen(char *out, size_t cap)
{
    size_t n = 0;
    char tmp[192];

    /* 偶尔生成完全非 JSON 的垃圾（覆盖解析拒绝路径） */
    if (rnd_n(32) == 0) {
        size_t k = rnd_n(cap - 1);
        for (size_t i = 0; i < k; i++)
            out[i] = (char)(rnd() & 0xFF ? (rnd() % 128) : 0);
        out[k] = 0;
        return k;
    }

    n += (size_t)snprintf(out + n, cap - n, "{\"jsonrpc\":\"2.0\"");
    /* id：数字 / 字符串 / 缺失 / 怪值 */
    switch (rnd_n(5)) {
    case 0: n += (size_t)snprintf(out + n, cap - n, ",\"id\":%u", rnd()); break;
    case 1: n += (size_t)snprintf(out + n, cap - n, ",\"id\":\"s%u\"", rnd_n(9)); break;
    case 2: n += (size_t)snprintf(out + n, cap - n, ",\"id\":%s",
                                  VALUES[rnd_n(sizeof VALUES / sizeof VALUES[0])]); break;
    default: break;
    }
    /* method */
    if (rnd_n(16) == 0) {
        n += (size_t)snprintf(out + n, cap - n, ",\"method\":%s",
                              VALUES[rnd_n(sizeof VALUES / sizeof VALUES[0])]);
    } else {
        n += (size_t)snprintf(out + n, cap - n, ",\"method\":\"%s\"",
                              METHODS[rnd_n(sizeof METHODS / sizeof METHODS[0])]);
    }
    /* params：多层结构，字段名/值类型随机 */
    switch (rnd_n(10)) {
    case 0: break;
    case 1: n += (size_t)snprintf(out + n, cap - n, ",\"params\":%s",
                                  VALUES[rnd_n(sizeof VALUES / sizeof VALUES[0])]); break;
    default: {
        n += (size_t)snprintf(out + n, cap - n, ",\"params\":{");
        int nk = 1 + (int)rnd_n(4);
        int first = 1;
        for (int i = 0; i < nk; i++) {
            const char *key;
            switch (rnd_n(7)) {
            case 0: key = "name"; break;
            case 1: key = "uri"; break;
            case 2: key = "cursor"; break;
            case 3: key = "level"; break;
            case 4: key = "arguments"; break;
            case 5: key = "requestId"; break;
            default: key = "_meta"; break;
            }
            const char *val;
            if (!strcmp(key, "name"))      val = TOOLNAMES[rnd_n(7)];
            else if (!strcmp(key, "uri"))  val = URIS[rnd_n(8)];
            else if (!strcmp(key, "level"))val = LEVELS[rnd_n(10)];
            else                           val = NULL;
            if (!first) n += (size_t)snprintf(out + n, cap - n, ",");
            first = 0;
            if (val)
                n += (size_t)snprintf(out + n, cap - n, "\"%s\":\"%s\"", key, val);
            else if (!strcmp(key, "_meta")) {
                if (rnd_n(2))
                    n += (size_t)snprintf(out + n, cap - n,
                        "\"_meta\":{\"progressToken\":%u}", rnd());
                else
                    n += (size_t)snprintf(out + n, cap - n, "\"_meta\":%s",
                        VALUES[rnd_n(sizeof VALUES / sizeof VALUES[0])]);
            } else
                n += (size_t)snprintf(out + n, cap - n, "\"%s\":%s", key,
                    VALUES[rnd_n(sizeof VALUES / sizeof VALUES[0])]);
        }
        n += (size_t)snprintf(out + n, cap - n, "}");
        break;
    }
    }
    /* 偶尔加额外顶层键 / 深嵌套 */
    if (rnd_n(8) == 0) {
        int depth = 1 + (int)rnd_n(10);
        n += (size_t)snprintf(out + n, cap - n, ",\"x\":");
        for (int i = 0; i < depth && n + 2 < cap; i++) out[n++] = '[';
        out[n++] = '1';
        for (int i = 0; i < depth && n + 2 < cap; i++) out[n++] = ']';
        out[n] = 0;
    }
    n += (size_t)snprintf(out + n, cap - n, "}");
    /* 尾部：合法空白 / 垃圾 / 第二个对象 / 截断 */
    switch (rnd_n(16)) {
    case 0: if (n + 1 < cap) out[n++] = ' '; break;
    case 1: if (n + 3 < cap) n += (size_t)snprintf(out + n, cap - n, "xyz"); break;
    case 2: if (n + 3 < cap) n += (size_t)snprintf(out + n, cap - n, "{}"); break;
    case 3: if (n > 3) n -= 1 + rnd_n(3); break;   /* 截断 */
    default: break;
    }
    out[n] = 0;
    (void)tmp;
    return n;
}

/* ---------------- 不变量检查 ---------------- */
static int rc_known(int rc)
{
    switch (rc) {
    case BT_MCP_OK: case BT_MCP_E_AGAIN: case BT_MCP_E_ARGS:
    case BT_MCP_E_STATE: case BT_MCP_E_TOOBIG: case BT_MCP_E_NOMEM:
    case BT_MCP_E_SYNC: case BT_MCP_E_PARSE: case BT_MCP_E_IO:
    case BT_MCP_E_PENDING: case BT_MCP_E_CANCEL: case BT_MCP_E_LOCKED:
    case BT_MCP_E_AUTHZ: case BT_MCP_E_BUSY:
        return 1;
    default:
        return 0;
    }
}

/* 槽位基线：迭代间不得残留（I4） */
static int slots_used(void)
{
    int n = 0;
    for (int i = 0; i < BT_MCP_CFG_MAX_PENDING; i++)
        if (g_sv.calls[i].in_use) n++;
    return n;
}

int main(int argc, char **argv)
{
    uint32_t iters = (argc > 1) ? (uint32_t)strtoul(argv[1], NULL, 10) : 1000000u;
    g_rng = (argc > 2) ? (uint32_t)strtoul(argv[2], NULL, 10) : 0xC0FFEEu;

    g_cap.kind = BT_MCP_CHAN_PACKET;
    g_cap.mtu = 244;
    g_cap.reliable = 1;
    g_cap.flow_ctrl = 1;
    g_cap.max_pending = 4;
    g_fcfg.max_reasm = 4;
    g_fcfg.frag_tout_ms = 2000;
    g_scfg.tools = TOOLS;
    g_scfg.tools_count = (uint16_t)(sizeof TOOLS / sizeof TOOLS[0]);
    g_scfg.resources = RES;
    g_scfg.resources_count = 1;
    g_scfg.prompts = PROMPTS;
    g_scfg.prompts_count = 1;

    if (server_up("2025-11-25")) { printf("setup failed\n"); return 2; }

    char msg[512];
    uint32_t i2 = 0, i3 = 0, i4 = 0;

    for (uint32_t it = 0; it < iters; it++) {
        size_t n = gen(msg, sizeof msg);
        g_wire.n = 0;
        int rc = bt_mcp_server_rx(&g_sv, (const uint8_t *)msg, (uint32_t)n);

        /* I2：返回码合法 */
        if (!rc_known(rc)) {
            if (++i2 <= 5)
                printf("I2 VIOLATION it=%u rc=%d msg=%.80s\n", it, rc, msg);
        }

        /* I3：每 512 轮验证状态未粘死 */
        if ((it & 511) == 0) {
            const char *ping = "{\"jsonrpc\":\"2.0\",\"id\":999,\"method\":\"ping\"}";
            g_wire.n = 0;
            int prc = bt_mcp_server_rx(&g_sv, (const uint8_t *)ping,
                                       (uint32_t)strlen(ping));
            if (prc != BT_MCP_OK && prc != BT_MCP_E_TOOBIG) {
                if (++i3 <= 5)
                    printf("I3 VIOLATION it=%u ping rc=%d\n", it, prc);
            }
        }

        /* I4：每 1024 轮清空可能的异步槽并检查基线 */
        if ((it & 1023) == 0) {
            for (int k = 0; k < BT_MCP_CFG_MAX_PENDING; k++)
                if (g_sv.calls[k].in_use)
                    (void)bt_mcp_server_complete(&g_sv.calls[k], "z", 0);
            if (slots_used() != 0) {
                if (++i4 <= 5)
                    printf("I4 VIOLATION it=%u slots=%d\n", it, slots_used());
            }
        }

        /* 定期 tick，覆盖超时回收分支 */
        if ((it & 2047) == 0) bt_mcp_server_tick(&g_sv, it * 10u);

        /* 极少数情况下重建栈，避免长期累积半开状态掩盖问题 */
        if ((it & 0xFFFF) == 0xFFFF)
            (void)server_up((rnd() & 1) ? "2025-03-26" : "2025-06-18");
    }

    /* 客户端侧也打一轮（同一变异器，喂给 client_rx） */
    {
        g_wire.n = 0;
        bt_mcp_framer_init(&g_fr, &g_fcfg, &g_cap);
        bt_mcp_session_cfg_t cc = { .req_timeout_ms = 30000,
                                    .resume_tout_ms = 5000, .tx_window = 8,
                                    .role = BT_MCP_ROLE_CLIENT };
        bt_mcp_session_init(&g_se, &cc);
        bt_mcp_session_bind(&g_se, &g_fr, vput, &g_wire);
        bt_mcp_session_link_up(&g_se, 0);
        bt_mcp_client_init(&g_cl, &g_se, NULL, NULL);
        uint32_t citers = iters / 4 ? iters / 4 : 1;
        for (uint32_t it = 0; it < citers; it++) {
            size_t n = gen(msg, sizeof msg);
            int rc = bt_mcp_client_rx(&g_cl, (const uint8_t *)msg, (uint32_t)n);
            if (rc != BT_MCP_OK && rc != BT_MCP_E_PARSE) {
                if (++i2 <= 10)
                    printf("I2 VIOLATION (client) it=%u rc=%d\n", it, rc);
            }
        }
    }

    printf("FUZZ_L5 DONE: %u server + %u client iters, seed=%u\n",
           iters, (iters / 4 ? iters / 4 : 1), g_rng);
    printf("  I2(rc合法)=%u  I3(状态不粘)=%u  I4(槽位不泄漏)=%u\n", i2, i3, i4);
    if (i2 || i3 || i4) {
        printf("  INVARIANT VIOLATIONS: %u\n", i2 + i3 + i4);
        return 1;
    }
    printf("  ZERO VIOLATION\n");
    return 0;
}
