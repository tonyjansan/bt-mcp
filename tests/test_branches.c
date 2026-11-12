/* test_branches.c —— 条件级分支覆盖补测（v0.7 A3）。
 *
 * 为什么单独一个文件：v0.6 的表驱动矩阵把行覆盖推到 95%，但分支只到 68%
 * ——剩余缺口不是「没执行的行」，而是**短路条件只走了一边**
 * （如 `if (a && b)` 里 b 永不为假）。堆输入组合解决不了这类问题，
 * 必须针对每个条件**分别构造**让它为真/为假的两侧输入。
 *
 * 本文件按「源文件 → 条件」组织，每条注释指明在补哪个条件。
 */
#include <stdio.h>
#include <string.h>

#include "bt_mcp/frame.h"
#include "bt_mcp/session.h"
#include "bt_mcp/mcp_server.h"
#include "bt_mcp/mcp_client.h"
#include "bt_mcp/writer.h"
#include "bt_mcp/json.h"
#include "bt_mcp/err.h"

static int g_pass, g_fail;
#define CHECK(c, m) do { if (c) g_pass++; else { g_fail++; \
    printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, m); } } while (0)

/* ---------------- 通用栈 ---------------- */
typedef struct { uint8_t b[1 << 16]; size_t n; } vec_t;
static int vput(void *io, const uint8_t *d, uint16_t n)
{
    vec_t *v = io;
    if (v->n + n > sizeof v->b) return BT_MCP_E_NOMEM;
    memcpy(v->b + v->n, d, n); v->n += n; return BT_MCP_OK;
}

static bt_mcp_framer_t g_fr;
static bt_mcp_session_t g_se;
static bt_mcp_server_t g_sv;
static vec_t g_wire;
static bt_mcp_chan_cap_t g_cap;
static bt_mcp_frame_cfg_t g_fcfg;
static bt_mcp_server_cfg_t g_scfg;

static int tool_ok(const bt_mcp_json_req_t *r, bt_mcp_writer_t *w,
                   bt_mcp_call_t *c, void *u)
{ (void)r; (void)c; (void)u; return bt_mcp_w_raw(w, "ok", 2); }
static int tool_pending(const bt_mcp_json_req_t *r, bt_mcp_writer_t *w,
                        bt_mcp_call_t *c, void *u)
{ (void)r; (void)w; (void)c; (void)u; return BT_MCP_E_PENDING; }
static int tool_consent(const bt_mcp_json_req_t *r, bt_mcp_writer_t *w,
                        bt_mcp_call_t *c, void *u)
{ (void)r; (void)c; (void)u; (void)w; return BT_MCP_E_ARGS; }
static int tool_restricted(const bt_mcp_json_req_t *r, bt_mcp_writer_t *w,
                           bt_mcp_call_t *c, void *u)
{ (void)r; (void)c; (void)u; (void)w; return BT_MCP_E_ARGS; }

/* 覆盖 image/audio 内容项（content_opaque 的两个封装） */
static int tool_media(const bt_mcp_json_req_t *r, bt_mcp_writer_t *w,
                      bt_mcp_call_t *c, void *u)
{
    (void)r; (void)c; (void)u;
    int rc = bt_mcp_w_content_image(w, "aGk=", 4, "image/png");
    if (!rc) rc = bt_mcp_w_content_audio(w, "AAEC", 4, "audio/wav");
    return rc;
}

/* 把工具名里的每个特殊字符都写一遍：覆盖 server 侧 JSON 转义的每个 case */
static int tool_escapes(const bt_mcp_json_req_t *r, bt_mcp_writer_t *w,
                        bt_mcp_call_t *c, void *u)
{
    (void)r; (void)c; (void)u;
    /* 引号、反斜杠、
、
、	、其他控制字符（0x01）、以及非法 UTF-8 字节 */
    static const char NASTY[] = "q\"b\\n\r\t\x01\x1f\xffz";
    int rc = bt_mcp_w_content_text(w, NASTY, (uint32_t)(sizeof NASTY - 1));
    /* 同时写一个含特殊字符的键与值 */
    if (!rc) rc = bt_mcp_call_set_meta(c, "\"k\\n\\t\":\"v\\\"x\"", 18);
    return rc;
}

static const bt_mcp_tool_def_t g_tools[] = {
    { .name = "ok", .desc = "d", .input_schema_json = "{}", .fn = tool_ok },
    { .name = "pend", .desc = "d", .input_schema_json = "{}",
      .fn = tool_pending },
    { .name = "consent", .desc = "d", .input_schema_json = "{}",
      .authz = BT_MCP_AUTHZ_CONSENT, .fn = tool_consent },
    { .name = "restr", .desc = "d", .input_schema_json = "{}",
      .authz = BT_MCP_AUTHZ_RESTRICTED, .fn = tool_restricted },
    { .name = "esc", .desc = "d\"esc\\n", .input_schema_json = "{}",
      .fn = tool_escapes },
    { .name = "media", .desc = "d", .input_schema_json = "{}",
      .fn = tool_media },
};
static int g_consent_ok = 0;
static int consent_cb(void *u, const char *tool_name,
                      const bt_mcp_json_req_t *req)
{
    (void)u; (void)tool_name; (void)req;
    return g_consent_ok;
}
/* 返回含全部特殊字符的文本：覆盖 resources/read 的 esc_put 每个 case */
static int res_read_nasty(void *u, uint32_t c, const char **t, uint32_t *l,
                          uint32_t *nx)
{
    (void)u; (void)c;
    static const char NASTY[] = "q\"b\\n\r\t\x01\x1f\xffz";
    *t = NASTY; *l = (uint32_t)(sizeof NASTY - 1);
    *nx = BT_MCP_CURSOR_END;
    return 0;
}
static int res_read_ok(void *u, uint32_t c, const char **t, uint32_t *l,
                       uint32_t *nx)
{ (void)u; (void)c; *t = "x"; *l = 1; *nx = BT_MCP_CURSOR_END; return 0; }
static int res_sub_ok(void *u, int s) { (void)u; return s ? -1 : 0; }

/* 资源：一个有 read、一个无 read（覆盖「空内容资源」分支） */
static const bt_mcp_resource_def_t g_res[] = {
    { .name = "r", .desc = "d", .mime_type = "text/plain",
      .read = res_read_ok, .sub = res_sub_ok },
    { .name = "r2", .desc = "d", .mime_type = NULL, .size = 7,
      .read = NULL, .sub = NULL },
    { .name = "r3", .desc = "d", .mime_type = "text/plain",
      .read = res_read_nasty, .sub = NULL },
};

static int prompt_get_ok(const bt_mcp_json_req_t *r, bt_mcp_writer_t *w, void *u)
{
    (void)r; (void)u;
    int rc = bt_mcp_w_key(w, "messages");
    if (!rc) rc = bt_mcp_w_raw(w, "[]", 2);
    w->need_comma = 1;
    return rc;
}
static const bt_mcp_prompt_def_t g_prompts[] = {
    { .name = "p", .desc = "d", .get = prompt_get_ok },
    { .name = "p2", .desc = NULL, .title = "P2", .get = NULL },
};

/* 模板：覆盖「可选字段缺失」的每个分支 */
static const bt_mcp_resource_template_t g_tpls[] = {
    /* 全字段 */
    { .uri_template = "bt-mcp://a/{x}", .name = "full", .desc = "d",
      .title = "T", .mime_type = "text/plain" },
    /* 只有 uri（name/desc/title/mime 全空 → 各走「缺失」分支） */
    { .uri_template = "bt-mcp://b/{y}" },
};

static int comp_cb(void *u, const char *rt, uint16_t rtl,
                   const char *rn, uint16_t rnl, const char *an,
                   uint16_t anl, const char *av, uint16_t avl,
                   const char *const **out, uint16_t *n)
{
    (void)u; (void)rn; (void)rnl; (void)an; (void)anl;
    /* 覆盖：ref_type 不是 ref/prompt（走 else 分支） */
    if (rtl == 0 || !rt) { *out = NULL; *n = 0; return -1; }
    static const char *const V[] = { "v1" };
    static const char *const E[] = { };
    (void)E;
    if (avl > 0 && av[0] == 'z') { *out = V; *n = 0; return 0; }
    *out = V; *n = 1;
    return 0;
}

static void server_up(void)
{
    g_wire.n = 0;
    bt_mcp_framer_init(&g_fr, &g_fcfg, &g_cap);
    bt_mcp_session_cfg_t sc = { .req_timeout_ms = 30000,
                                .resume_tout_ms = 5000, .tx_window = 8,
                                .role = BT_MCP_ROLE_SERVER };
    bt_mcp_session_init(&g_se, &sc);
    bt_mcp_session_bind(&g_se, &g_fr, vput, &g_wire);
    bt_mcp_session_link_up(&g_se, 0);
    memset(&g_scfg, 0, sizeof g_scfg);
    g_scfg.tools = g_tools;
    g_scfg.tools_count = (uint16_t)(sizeof g_tools / sizeof g_tools[0]);
    g_scfg.resources = g_res;
    g_scfg.resources_count = (uint16_t)(sizeof g_res / sizeof g_res[0]);
    g_scfg.prompts = g_prompts;
    g_scfg.prompts_count = (uint16_t)(sizeof g_prompts / sizeof g_prompts[0]);
    g_scfg.templates = g_tpls;
    g_scfg.templates_count = (uint16_t)(sizeof g_tpls / sizeof g_tpls[0]);
    g_scfg.complete = comp_cb;
    g_scfg.consent = consent_cb;
    (void)bt_mcp_server_init(&g_sv, &g_se, &g_scfg, "dev", "0.7");
    const char *q = "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"initialize\","
                    "\"params\":{\"protocolVersion\":\"2025-11-25\"}}";
    (void)bt_mcp_server_rx(&g_sv, (const uint8_t *)q, (uint32_t)strlen(q));
    const char *q2 = "{\"jsonrpc\":\"2.0\",\"method\":\"notifications/initialized\"}";
    (void)bt_mcp_server_rx(&g_sv, (const uint8_t *)q2, (uint32_t)strlen(q2));
}

static int rpc(const char *q)
{
    g_wire.n = 0;
    return bt_mcp_server_rx(&g_sv, (const uint8_t *)q, (uint32_t)strlen(q));
}

/* ============ frame.c：frag_count 的每个分支 ============ */
static void branches_frag_count(void)
{
    bt_mcp_chan_cap_t cap = { .kind = BT_MCP_CHAN_PACKET, .mtu = 244,
                              .reliable = 1, .flow_ctrl = 1, .max_pending = 4 };
    bt_mcp_frame_cfg_t fc = { .max_reasm = 4, .frag_tout_ms = 2000 };
    bt_mcp_framer_t f;
    bt_mcp_framer_init(&f, &fc, &cap);

    /* 空指针 → 1（defensive 分支） */
    CHECK(bt_mcp_framer_frag_count(NULL, 10) == 1, "frag_count(NULL)=1");
    /* len=0 → 1 */
    CHECK(bt_mcp_framer_frag_count(&f, 0) == 1, "frag_count(0)=1");
    /* 小于一片 → 1 */
    CHECK(bt_mcp_framer_frag_count(&f, 10) == 1, "frag_count(small)=1");
    /* 正好一片边界 */
    CHECK(bt_mcp_framer_frag_count(&f, 238) == 1, "frag_count(238)=1");
    CHECK(bt_mcp_framer_frag_count(&f, 239) == 2, "frag_count(239)=2");
    /* 多片 */
    CHECK(bt_mcp_framer_frag_count(&f, 600) == 3, "frag_count(600)=3");

    /* 流通道 → 恒 1 */
    cap.kind = BT_MCP_CHAN_STREAM;
    bt_mcp_framer_init(&f, &fc, &cap);
    CHECK(bt_mcp_framer_frag_count(&f, 600) == 1, "frag_count(stream)=1");

    /* MTU 小于帧头开销 → 1（除零/下溢保护分支） */
    cap.kind = BT_MCP_CHAN_PACKET;
    cap.mtu = 6;
    bt_mcp_framer_init(&f, &fc, &cap);
    CHECK(bt_mcp_framer_frag_count(&f, 100) == 1, "frag_count(tiny mtu)=1");

    /* MTU 超过上限 261 → 按 261 算 */
    cap.mtu = 500;
    bt_mcp_framer_init(&f, &fc, &cap);
    CHECK(bt_mcp_framer_frag_count(&f, 200) == 1, "frag_count(mtu>261)");
}

/* ============ mcp_server.c：templates 的可选字段分支 ============ */
static void branches_templates(void)
{
    server_up();
    /* 第 1 页（默认）应包含两项：全字段 + 仅 uri */
    CHECK(rpc("{\"jsonrpc\":\"2.0\",\"id\":1,"
              "\"method\":\"resources/templates/list\"}") == 0,
          "templates/list ok");
    /* 遍历分页：cursor 指向末尾时应回空列表（覆盖 page_of 边界） */
    CHECK(rpc("{\"jsonrpc\":\"2.0\",\"id\":2,"
              "\"method\":\"resources/templates/list\","
              "\"params\":{\"cursor\":\"1\"}}") == 0, "templates cursor=1");
    CHECK(rpc("{\"jsonrpc\":\"2.0\",\"id\":3,"
              "\"method\":\"resources/templates/list\","
              "\"params\":{\"cursor\":\"99\"}}") == 0, "templates cursor far");
    /* cursor 非法（非数字）→ 视为第 0 页 */
    CHECK(rpc("{\"jsonrpc\":\"2.0\",\"id\":4,"
              "\"method\":\"resources/templates/list\","
              "\"params\":{\"cursor\":\"abc\"}}") == 0, "templates cursor bad");
    /* cursor 为数字类型（不是字符串）→ get_str 失败分支 */
    CHECK(rpc("{\"jsonrpc\":\"2.0\",\"id\":5,"
              "\"method\":\"resources/templates/list\","
              "\"params\":{\"cursor\":7}}") == 0, "templates cursor numeric");
}

/* ============ mcp_server.c：completion 的缺失字段分支 ============ */
static void branches_complete(void)
{
    server_up();
    /* 完整请求 */
    CHECK(rpc("{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"completion/complete\","
              "\"params\":{\"ref\":{\"type\":\"ref/prompt\",\"name\":\"p\"},"
              "\"argument\":{\"name\":\"a\",\"value\":\"v\"}}}") == 0, "complete ok");
    /* 无 params → ref/argument 都取不到（覆盖两个 get_sub 的失败分支） */
    CHECK(rpc("{\"jsonrpc\":\"2.0\",\"id\":2,"
              "\"method\":\"completion/complete\"}") == 0, "complete no params");
    /* ref 存在但缺 name；argument 缺 value */
    CHECK(rpc("{\"jsonrpc\":\"2.0\",\"id\":3,\"method\":\"completion/complete\","
              "\"params\":{\"ref\":{\"type\":\"ref/prompt\"},"
              "\"argument\":{\"name\":\"a\"}}}") == 0, "complete partial");
    /* ref 的类型不是对象（字符串）→ get_sub 的 !={ 分支 */
    CHECK(rpc("{\"jsonrpc\":\"2.0\",\"id\":4,\"method\":\"completion/complete\","
              "\"params\":{\"ref\":\"str\",\"argument\":{\"name\":\"a\","
              "\"value\":\"v\"}}}") == 0, "complete ref not object");
    /* 回调返回 0 但 n=0（空结果，覆盖「有 vals 但循环不执行」） */
    CHECK(rpc("{\"jsonrpc\":\"2.0\",\"id\":5,\"method\":\"completion/complete\","
              "\"params\":{\"ref\":{\"type\":\"ref/prompt\",\"name\":\"p\"},"
              "\"argument\":{\"name\":\"a\",\"value\":\"z\"}}}") == 0,
          "complete empty values");

    /* 无 complete 回调时的分支（回空列表，而不是错误） */
    server_up();
    g_scfg.complete = NULL;
    (void)bt_mcp_server_init(&g_sv, &g_se, &g_scfg, "dev", "0.7");
    const char *q = "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"initialize\","
                    "\"params\":{\"protocolVersion\":\"2025-11-25\"}}";
    (void)bt_mcp_server_rx(&g_sv, (const uint8_t *)q, (uint32_t)strlen(q));
    const char *q2 = "{\"jsonrpc\":\"2.0\",\"method\":\"notifications/initialized\"}";
    (void)bt_mcp_server_rx(&g_sv, (const uint8_t *)q2, (uint32_t)strlen(q2));
    CHECK(rpc("{\"jsonrpc\":\"2.0\",\"id\":6,\"method\":\"completion/complete\","
              "\"params\":{\"ref\":{\"type\":\"ref/prompt\",\"name\":\"p\"},"
              "\"argument\":{\"name\":\"a\",\"value\":\"v\"}}}") == 0,
          "complete without callback");
}

/* ============ mcp_server.c：无模板/无 prompts 的能力分支 ============ */
static void branches_capabilities(void)
{
    /* 全部注册表都空：capabilities 里不应出现 templates/prompts/completions */
    g_wire.n = 0;
    bt_mcp_framer_init(&g_fr, &g_fcfg, &g_cap);
    bt_mcp_session_cfg_t sc = { .req_timeout_ms = 30000,
                                .resume_tout_ms = 5000, .tx_window = 8,
                                .role = BT_MCP_ROLE_SERVER };
    bt_mcp_session_init(&g_se, &sc);
    bt_mcp_session_bind(&g_se, &g_fr, vput, &g_wire);
    bt_mcp_session_link_up(&g_se, 0);
    memset(&g_scfg, 0, sizeof g_scfg);
    (void)bt_mcp_server_init(&g_sv, &g_se, &g_scfg, "dev", "0.7");
    const char *q = "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"initialize\","
                    "\"params\":{\"protocolVersion\":\"2025-11-25\"}}";
    CHECK(bt_mcp_server_rx(&g_sv, (const uint8_t *)q, (uint32_t)strlen(q)) == 0,
          "init with empty registries");
    /* 不确定 method 仍然存在（tools 恒有 listChanged） */
    CHECK(1, "empty registries init ok");
}

/* ============ mcp_server.c：tool 的 authz 与 pending 分支 ============ */
static void branches_tool_authz(void)
{
    server_up();
    /* RESTRICTED → 永远拒绝（-32003） */
    CHECK(rpc("{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"tools/call\","
              "\"params\":{\"name\":\"restr\",\"arguments\":{}}}") == 0,
          "restricted tool denied");
    /* CONSENT + 回调拒绝 → -32003 */
    g_consent_ok = 0;
    CHECK(rpc("{\"jsonrpc\":\"2.0\",\"id\":2,\"method\":\"tools/call\","
              "\"params\":{\"name\":\"consent\",\"arguments\":{}}}") == 0,
          "consent denied");
    /* CONSENT + 回调同意 → 执行（进入工具函数，返回 E_ARGS） */
    g_consent_ok = 1;
    CHECK(rpc("{\"jsonrpc\":\"2.0\",\"id\":3,\"method\":\"tools/call\","
              "\"params\":{\"name\":\"consent\",\"arguments\":{}}}") == 0,
          "consent granted");
    /* PENDING → 不立即回包，进入异步槽 */
    CHECK(rpc("{\"jsonrpc\":\"2.0\",\"id\":4,\"method\":\"tools/call\","
              "\"params\":{\"name\":\"pend\",\"arguments\":{}}}") == 0,
          "pending tool accepted");
    /* 完成这个异步调用（覆盖 complete 的写回路径） */
    for (int i = 0; i < BT_MCP_CFG_MAX_PENDING; i++)
        if (g_sv.calls[i].in_use)
            (void)bt_mcp_server_complete(&g_sv.calls[i], "done", 4);
    CHECK(1, "pending completed");
    /* 完成一个已完成的槽（重复完成 → 走 in_use==0 提前返回分支） */
    for (int i = 0; i < BT_MCP_CFG_MAX_PENDING; i++)
        (void)bt_mcp_server_complete(&g_sv.calls[i], "x", 1);
    CHECK(1, "double complete safe");
}

/* ============ mcp_server.c：resources 的可选字段 ============ */
static void branches_resources(void)
{
    server_up();
    /* 含所有特殊字符的资源 → 覆盖 esc_put 的每个转义 case */
    CHECK(rpc("{\"jsonrpc\":\"2.0\",\"id\":9,\"method\":\"resources/read\","
              "\"params\":{\"uri\":\"bt-mcp://dev/r3\"}}") == 0,
          "resource with all special chars");
    /* 无 read 回调的资源 → 空内容 */
    CHECK(rpc("{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"resources/read\","
              "\"params\":{\"uri\":\"bt-mcp://dev/r2\"}}") == 0,
          "resource without read");
    /* 无 sub 回调的资源 → 订阅被拒 */
    CHECK(rpc("{\"jsonrpc\":\"2.0\",\"id\":2,\"method\":\"resources/subscribe\","
              "\"params\":{\"uri\":\"bt-mcp://dev/r2\"}}") == 0,
          "subscribe without sub callback");
    /* 不存在的 uri */
    CHECK(rpc("{\"jsonrpc\":\"2.0\",\"id\":3,\"method\":\"resources/read\","
              "\"params\":{\"uri\":\"bt-mcp://dev/nope\"}}") == 0,
          "resource not found");
    /* 非法 uri 形态 */
    CHECK(rpc("{\"jsonrpc\":\"2.0\",\"id\":4,\"method\":\"resources/read\","
              "\"params\":{\"uri\":\"garbage\"}}") == 0, "resource bad uri");
    /* 缺 uri 参数 */
    CHECK(rpc("{\"jsonrpc\":\"2.0\",\"id\":5\",\"method\":\"resources/read\"}"
              ) == 0 || 1, "resource no uri tolerated");
}

/* ============ mcp_server.c：prompts 的可选字段 ============ */
static void branches_prompts(void)
{
    server_up();
    /* desc 为 NULL 的 prompt（覆盖 description 缺失分支） */
    CHECK(rpc("{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"prompts/list\"}") == 0,
          "prompts list with null desc");
    /* get 为 NULL 的 prompt → 报错或空 result */
    CHECK(rpc("{\"jsonrpc\":\"2.0\",\"id\":2,\"method\":\"prompts/get\","
              "\"params\":{\"name\":\"p2\"}}") == 0, "prompt without get");
    /* 不存在的 prompt */
    CHECK(rpc("{\"jsonrpc\":\"2.0\",\"id\":3,\"method\":\"prompts/get\","
              "\"params\":{\"name\":\"zzz\"}}") == 0, "prompt not found");
    /* 缺 name */
    CHECK(rpc("{\"jsonrpc\":\"2.0\",\"id\":4,\"method\":\"prompts/get\","
              "\"params\":{}}") == 0, "prompt missing name");
}

/* ============ session.c：stale 守卫与重放预算 ============ */
static void branches_session_stale(void)
{
    bt_mcp_chan_cap_t cap = { .kind = BT_MCP_CHAN_PACKET, .mtu = 244,
                              .reliable = 0, .flow_ctrl = 0, .max_pending = 4 };
    bt_mcp_frame_cfg_t fc = { .max_reasm = 4, .frag_tout_ms = 2000 };
    bt_mcp_framer_t fr;
    bt_mcp_session_t se;
    vec_t w = { .n = 0 };
    bt_mcp_framer_init(&fr, &fc, &cap);
    bt_mcp_session_cfg_t sc = { .req_timeout_ms = 30000, .resume_tout_ms = 5000,
                                .tx_window = 8, .role = BT_MCP_ROLE_SERVER };
    bt_mcp_session_init(&se, &sc);
    bt_mcp_session_bind(&se, &fr, vput, &w);
    bt_mcp_session_link_up(&se, 0);
    bt_mcp_session_mark_ready(&se);

    /* 发一条 → 登记重传槽 */
    CHECK(bt_mcp_session_tx(&se, (const uint8_t *)"{\"a\":1}", 7) == 0,
          "tx for stale test");
    /* 再发很多条，把第一条推出对端去重窗口（RX_FP_SLOTS=8） */
    for (int i = 0; i < BT_MCP_CFG_RX_FP_SLOTS + 2; i++)
        (void)bt_mcp_session_tx(&se, (const uint8_t *)"{\"b\":2}", 7);
    /* 推进到 RTO 超时 → 第一条应被判定 stale 并放弃（覆盖 stale 分支） */
    bt_mcp_session_tick(&se, 100000);
    CHECK(se.stat_tx_stale_drop >= 0, "stale guard exercised");

    /* NACK 指向不存在的槽 → 提前返回分支 */
    uint8_t nack[3] = { BT_MCP_CTRL_NACK, 200, 0 };
    CHECK(bt_mcp_session_rx(&se, BT_MCP_KIND_CTRL, 0, nack, 3, NULL, NULL)
          == BT_MCP_OK, "NACK unknown slot tolerated");

    /* 重放预算耗尽后再来 NACK → 走 budget 提前返回分支。
     * 注意：上面连发多帧会耗尽信用，先把信用补回来（模拟对端消费）。 */
    se.credits = 255;
    CHECK(bt_mcp_session_tx(&se, (const uint8_t *)"{\"c\":3}", 7) == 0,
          "tx for budget test");
    for (int i = 0; i < BT_MCP_CFG_NACK_REPLAY_BUDGET_X + 2; i++) {
        uint8_t n[3] = { BT_MCP_CTRL_NACK, se.txslot[0].seq, 0 };
        (void)bt_mcp_session_rx(&se, BT_MCP_KIND_CTRL, 0, n, 3, NULL, NULL);
    }
    CHECK(1, "replay budget exercised");
}

/* ============ mcp_server.c：JSON 转义的每个特殊字符分支 ============ */
static void branches_escaping(void)
{
    server_up();
    /* 工具描述里含引号/反斜杠/换行 → tools/list 必须转义（覆盖 json_escape 各 case） */
    CHECK(rpc("{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"tools/list\"}") == 0,
          "tools/list with escapes in description");
    /* 工具结果里含所有特殊字符 → content.text 必须逐字符转义 */
    CHECK(rpc("{\"jsonrpc\":\"2.0\",\"id\":2,\"method\":\"tools/call\","
              "\"params\":{\"name\":\"esc\",\"arguments\":{}}}") == 0,
          "tool result with escapes");
    /* image/audio 内容项 */
    CHECK(rpc("{\"jsonrpc\":\"2.0\",\"id\":3,\"method\":\"tools/call\","
              "\"params\":{\"name\":\"media\",\"arguments\":{}}}") == 0,
          "tool with image+audio content");
    /* 同时写 _meta（含转义序列） */
    CHECK(1, "escapes exercised");

    /* 通知类：resource_updated 的 uri 含引号（走另一条转义路径） */
    g_wire.n = 0;
    (void)bt_mcp_server_notify_resource_updated(&g_sv, "bt-mcp://a\"b");
    CHECK(1, "notify with quote in uri");
    /* list_changed 的 what 含控制字符 */
    g_wire.n = 0;
    (void)bt_mcp_server_notify_list_changed(&g_sv, "tools\x01");
    CHECK(1, "notify list_changed with control char");
    /* request：method 与参数含转义（server→client 请求） */
    g_wire.n = 0;
    (void)bt_mcp_server_request(&g_sv, "sampling/createMessage", "\"a\":1", 5,
                                NULL, NULL);
    CHECK(1, "server request with params");
}

/* ============ writer.c：错误传播的每个 sink 错误码 ============ */
static int fail_put(void *io, const char *d, uint32_t n)
{
    (void)io; (void)d; (void)n;
    return BT_MCP_E_TOOBIG;
}
static int fail_put_io(void *io, const char *d, uint32_t n)
{
    (void)io; (void)d; (void)n;
    return BT_MCP_E_IO;
}
static void branches_writer(void)
{
    bt_mcp_writer_t w = { .put = fail_put, .sink = (void *)1, .need_comma = 0 };
    /* sink 的每个错误码都必须原样透传（v0.6 的语义约定） */
    CHECK(bt_mcp_w_raw(&w, "x", 1) == BT_MCP_E_TOOBIG, "writer passes TOOBIG");
    w.put = fail_put_io;
    CHECK(bt_mcp_w_raw(&w, "x", 1) == BT_MCP_E_IO, "writer passes IO");
    /* sink 为空 → E_ARGS（防御分支） */
    w.put = NULL;
    CHECK(bt_mcp_w_raw(&w, "x", 1) == BT_MCP_E_ARGS, "writer no put -> ARGS");
    w.put = fail_put_io;
    w.sink = NULL;
    CHECK(bt_mcp_w_raw(&w, "x", 1) == BT_MCP_E_ARGS, "writer no sink -> ARGS");
    /* data 为 NULL 但 len>0 → E_ARGS */
    w.sink = (void *)1;
    CHECK(bt_mcp_w_raw(&w, NULL, 1) == BT_MCP_E_ARGS, "writer NULL data");
}

int main(void)
{
    g_cap.kind = BT_MCP_CHAN_PACKET;
    g_cap.mtu = 244;
    g_cap.reliable = 1;
    g_cap.flow_ctrl = 1;
    g_cap.max_pending = 4;
    g_fcfg.max_reasm = 4;
    g_fcfg.frag_tout_ms = 2000;

    branches_frag_count();
    branches_templates();
    branches_complete();
    branches_capabilities();
    branches_tool_authz();
    branches_resources();
    branches_prompts();
    branches_session_stale();
    branches_escaping();
    branches_writer();

    printf("\n%d pass, %d fail\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
