/* test_conformance.c —— MCP 规范结构符合性（v0.4 G4）。
 *
 * 与差分测试的分工：
 *  - 差分回答「C 与 Rust 是否一致」（两边一起错也发现不了）；
 *  - 本测试回答「响应是否符合 MCP 规范的**结构要求**」。
 *
 * 做法：驱动真实 server，用 core 的 JSON 解析器读取响应，断言规范要求的
 * 必需字段存在且类型正确、可选字段不越界、错误码落在规范区间。
 * 这样即使两侧实现一起改错，只要违反规范形状就会失败。
 *
 * 注：用 core 自己的解析器做断言有轻微自引用，但该解析器本身由 1339 项差分
 * 与 fuzz 覆盖，风险可接受；若要更独立可换第三方解析器（记为 v0.5 改进）。
 */
#include <stdio.h>
#include <string.h>

#include "bt_mcp/frame.h"
#include "bt_mcp/session.h"
#include "bt_mcp/mcp_server.h"
#include "bt_mcp/json.h"
#include "bt_mcp/err.h"

/* memmem 不是 C99 标准函数：自备一个（GNU 扩展在 -std=c99 下不可见） */
static const void *mem_find(const void *hay, size_t hn, const char *needle,
                            size_t nn)
{
    if (nn == 0 || hn < nn) return NULL;
    const unsigned char *h = hay;
    for (size_t i = 0; i + nn <= hn; i++)
        if (!memcmp(h + i, needle, nn)) return h + i;
    return NULL;
}

static int g_pass, g_fail;
#define CHECK(c, m) do { if (c) g_pass++; else { g_fail++; \
    printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, m); } } while (0)

typedef struct { uint8_t b[65536]; size_t n; } vec_t;
static int vput(void *io, const uint8_t *d, uint16_t n)
{
    vec_t *v = io;
    if (v->n + n > sizeof v->b) return BT_MCP_E_NOMEM;
    memcpy(v->b + v->n, d, n);
    v->n += n;
    return 0;
}

/* ---- 工具表 ---- */
static int tool_text(const bt_mcp_json_req_t *r, bt_mcp_writer_t *w,
                     bt_mcp_call_t *c, void *user)
{
    (void)r; (void)c; (void)user;
    return bt_mcp_w_raw(w, "hi", 2);
}
static int tool_rich(const bt_mcp_json_req_t *r, bt_mcp_writer_t *w,
                     bt_mcp_call_t *c, void *user)
{
    (void)r; (void)c; (void)user;
    int rc = bt_mcp_w_content_text(w, "a", 1);
    if (!rc) rc = bt_mcp_w_content_image(w, "aGk=", 4, "image/png");
    if (!rc) rc = bt_mcp_w_content_resource(w, "bt-mcp://dev/r", "text/plain",
                                            "x", 1);
    {
        static const char M[] = "\"io.btmcp/ver\":\"0.5\"";
        if (!rc) rc = bt_mcp_call_set_meta(c, M, (uint32_t)(sizeof M - 1));
    }
    return rc;
}
static int tool_fail(const bt_mcp_json_req_t *r, bt_mcp_writer_t *w,
                     bt_mcp_call_t *c, void *user)
{
    (void)r; (void)w; (void)c; (void)user;
    return BT_MCP_E_ARGS;              /* 失败且不写内容 */
}
static int g_consent_allow;
static int consent_cb(void *u, const char *n, const bt_mcp_json_req_t *r)
{
    (void)u; (void)n; (void)r;
    return g_consent_allow ? 0 : -1;
}

static const bt_mcp_tool_def_t TOOLS[] = {
    { .name = "t_text", .desc = "text tool", .title = "Text tool",
      .input_schema_json = "{\"type\":\"object\",\"properties\":{}}",
      .output_schema_json = "{\"type\":\"object\"}",
      .annotations = BT_MCP_TOOL_READONLY, .authz = BT_MCP_AUTHZ_OPEN,
      .fn = tool_text },
    { .name = "t_rich", .desc = "rich", 
      .input_schema_json = "{\"type\":\"object\",\"properties\":{}}",
      .authz = BT_MCP_AUTHZ_OPEN, .fn = tool_rich },
    { .name = "t_fail", .desc = "fails",
      .input_schema_json = "{\"type\":\"object\",\"properties\":{}}",
      .authz = BT_MCP_AUTHZ_OPEN, .fn = tool_fail },
    { .name = "t_consent", .desc = "consent",
      .input_schema_json = "{\"type\":\"object\",\"properties\":{}}",
      .authz = BT_MCP_AUTHZ_CONSENT, .fn = tool_text },
    { .name = "t_restricted", .desc = "restricted",
      .input_schema_json = "{\"type\":\"object\",\"properties\":{}}",
      .authz = BT_MCP_AUTHZ_RESTRICTED, .fn = tool_text },
};

static int res_read(void *u, uint32_t cur, const char **t, uint32_t *l,
                    uint32_t *next)
{
    (void)u; (void)cur;
    *t = "hello"; *l = 5; *next = BT_MCP_CURSOR_END;
    return 0;
}
static int res_sub(void *u, int sub) { (void)u; (void)sub; return 0; }

static const bt_mcp_resource_def_t RES[] = {
    { .name = "r", .desc = "res", .title = "Res", .mime_type = "text/plain",
      .size = 5, .authz = BT_MCP_AUTHZ_OPEN, .read = res_read, .sub = res_sub },
};

static int prompt_get(const bt_mcp_json_req_t *r, bt_mcp_writer_t *w, void *u)
{
    (void)r; (void)u;
    int rc = bt_mcp_w_key(w, "messages");
    if (!rc) rc = bt_mcp_w_raw(w,
        "[{\"role\":\"user\",\"content\":{\"type\":\"text\",\"text\":\"hi\"}}]",
        sizeof "[{\"role\":\"user\",\"content\":{\"type\":\"text\",\"text\":\"hi\"}}]" - 1);
    w->need_comma = 1;
    /* v0.6：显式验证 bt_mcp_w_meta()——它接受**JSON 片段原文**（这里传字符串值
     * "conf"），产出 "k":"conf"，而不是再套一层引号。 */
    if (!rc) rc = bt_mcp_w_meta(w, "io.btmcp/src", "\"conf\"",
                               (uint32_t)(sizeof "\"conf\"" - 1));
    w->need_comma = 1;
    return rc;
}
static const bt_mcp_prompt_def_t PROMPTS[] = {
    { .name = "p", .desc = "prompt", .title = "P", .get = prompt_get },
};

/* v0.7（A5）：资源模板与补全 */
static const bt_mcp_resource_template_t TPLS[] = {
    { .uri_template = "bt-mcp://dev/{n}/log", .name = "tpl",
      .desc = "tpl desc", .title = "T", .mime_type = "text/plain" },
};
static int conf_complete(void *u, const char *rt, uint16_t rtl,
                         const char *rn, uint16_t rnl, const char *an,
                         uint16_t anl, const char *av, uint16_t avl,
                         const char *const **out, uint16_t *n)
{
    (void)u; (void)rt; (void)rtl; (void)rn; (void)rnl; (void)an; (void)anl;
    static const char *const ALL[] = { "alpha", "beta" };
    static const char *F[2];
    int k = 0;
    for (int i = 0; i < 2; i++)
        if (!avl || !strncmp(ALL[i], av, avl)) F[k++] = ALL[i];
    *out = F; *n = (uint16_t)k;
    return 0;
}

static bt_mcp_framer_t  g_fr;
static bt_mcp_session_t g_se;
static bt_mcp_server_t  g_sv;
static vec_t            g_wire;

/* 最近一条响应的「裸 JSON」（从帧里剥掉 6B 头后拼接） */
static uint8_t g_json[65536];
static uint32_t g_json_len;

/* 把 g_wire 里的帧拆出载荷（cap.reliable=1 → 无 FCS） */
static void unwrap_wire(void)
{
    g_json_len = 0;
    size_t off = 0;
    while (off + 6 <= g_wire.n) {
        uint16_t plen = (uint16_t)(g_wire.b[off + 4] | (g_wire.b[off + 5] << 8));
        if (off + 6 + plen > g_wire.n) break;
        if (g_json_len + plen <= sizeof g_json) {
            memcpy(g_json + g_json_len, g_wire.b + off + 6, plen);
            g_json_len += plen;
        }
        off += 6 + plen;
    }
}

/* 发一条请求，返回解析后的响应（无响应时返回 E_AGAIN） */
static int rpc(const char *req, bt_mcp_json_req_t *out)
{
    g_wire.n = 0;
    int rc = bt_mcp_server_rx(&g_sv, (const uint8_t *)req, (uint32_t)strlen(req));
    if (rc) return rc;
    unwrap_wire();
    if (!g_json_len) return BT_MCP_E_AGAIN;
    return bt_mcp_json_parse_msg((const char *)g_json, g_json_len, out);
}

/* 断言 result 是对象且含指定 key */
/* 先在顶层取 result 片段，再把该片段当独立对象解析并在其中找键。
 * 注意：result 是响应的**顶层**键（不在 params 里），所以不能用 get_sub。 */
static int has(const bt_mcp_json_req_t *r, const char *key,
               const char **v, uint32_t *vl)
{
    const char *res; uint32_t rl;
    if (bt_mcp_json_top_raw(r, "result", &res, &rl)) return BT_MCP_E_PARSE;
    if (rl < 2 || res[0] != '{') return BT_MCP_E_PARSE;
    bt_mcp_json_req_t sub;
    if (bt_mcp_json_parse_msg(res, rl, &sub)) return BT_MCP_E_PARSE;
    return bt_mcp_json_top_raw(&sub, key, v, vl);
}

int main(void)
{
    static bt_mcp_chan_cap_t cap = { .kind = BT_MCP_CHAN_PACKET, .mtu = 244,
                                     .reliable = 1, .flow_ctrl = 1,
                                     .max_pending = 4 };
    static bt_mcp_frame_cfg_t fcfg = { .max_reasm = 4, .frag_tout_ms = 2000 };
    bt_mcp_session_cfg_t sc = { .req_timeout_ms = 30000, .resume_tout_ms = 5000,
                                .tx_window = 8, .role = BT_MCP_ROLE_SERVER };

    bt_mcp_framer_init(&g_fr, &fcfg, &cap);
    bt_mcp_session_init(&g_se, &sc);
    bt_mcp_session_bind(&g_se, &g_fr, vput, &g_wire);
    bt_mcp_session_link_up(&g_se, 0);
    bt_mcp_session_mark_ready(&g_se);

    bt_mcp_server_cfg_t cfg = { .tools = TOOLS,
        .tools_count = (uint16_t)(sizeof TOOLS / sizeof TOOLS[0]),
        .resources = RES, .resources_count = 1,
        .prompts = PROMPTS, .prompts_count = 1,
        .consent = consent_cb, .consent_user = NULL,
        .templates = TPLS, .templates_count = 1,
        .complete = conf_complete };
    CHECK(bt_mcp_server_init(&g_sv, &g_se, &cfg, "conf-dev", "0.4") == 0,
          "server init");

    bt_mcp_json_req_t r;

    /* ---- 1. initialize 的结构符合规范 ---- */
    {
        int rc = rpc("{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"initialize\","
                     "\"params\":{\"protocolVersion\":\"2025-06-18\","
                     "\"capabilities\":{},\"clientInfo\":{\"name\":\"h\","
                     "\"version\":\"1\"}}}", &r);
        if (rc) printf("[dbg] initialize rc=%d wire=%zu\n", rc, g_wire.n);
        CHECK(rc == 0, "initialize responds");
        const char *v; uint32_t vl;
        {
            const char *rs; uint32_t rsl;
            CHECK(!bt_mcp_json_top_raw(&r, "result", &rs, &rsl)
                  && rsl > 2 && rs[0] == '{', "result is object");
            /* v0.7（A5）：能力声明（就在这条 initialize 响应里断言） */
            CHECK(mem_find(g_json, g_json_len, "\"templates\":{}",
                           sizeof "\"templates\":{}" - 1) != NULL,
                  "capabilities declares resources.templates");
            CHECK(mem_find(g_json, g_json_len, "\"completions\":{}",
                           sizeof "\"completions\":{}" - 1) != NULL,
                  "capabilities declares completions");
        }
        CHECK(!has(&r, "protocolVersion", &v, &vl) && vl >= 2 && v[0] == '"',
              "protocolVersion is string");
        CHECK(!has(&r, "serverInfo", &v, &vl) && vl > 2 && v[0] == '{',
              "serverInfo is object");
        CHECK(!has(&r, "capabilities", &v, &vl) && vl > 2 && v[0] == '{',
              "capabilities is object");
    }

    /* 1c. 未初始化时发请求 → -32001（不是 -32002，那个留给资源不存在） */
    {
        static bt_mcp_framer_t fr2;
        static bt_mcp_session_t se2;
        static bt_mcp_server_t sv2;
        static vec_t wire2;
        memset(&wire2, 0, sizeof wire2);
        bt_mcp_framer_init(&fr2, &fcfg, &cap);
        bt_mcp_session_init(&se2, &sc);
        bt_mcp_session_bind(&se2, &fr2, vput, &wire2);
        bt_mcp_session_link_up(&se2, 0);
        CHECK(bt_mcp_server_init(&sv2, &se2, &cfg, "d", "0.4") == 0, "sv2 init");
        const char *q = "{\"jsonrpc\":\"2.0\",\"id\":77,"
                        "\"method\":\"tools/list\"}";
        CHECK(bt_mcp_server_rx(&sv2, (const uint8_t *)q,
                              (uint32_t)strlen(q)) == 0, "pre-init request");
        /* 响应在 wire2 里：找 -32001 */
        int hit = 0;
        const char *needle = "\"code\":-32001";
        for (size_t i = 0; i + 13 <= wire2.n; i++)
            if (!memcmp(wire2.b + i, needle, 13)) { hit = 1; break; }
        CHECK(hit, "uninitialized -> -32001");
    }

    /* 之后的方法需要 initialized */
    {
        const char *ini = "{\"jsonrpc\":\"2.0\","
                          "\"method\":\"notifications/initialized\"}";
        CHECK(bt_mcp_server_rx(&g_sv, (const uint8_t *)ini,
                              (uint32_t)strlen(ini)) == 0, "initialized ack");
    }

    /* ---- 1b. ping 必须是空结果对象（规范要求） ---- */
    {
        CHECK(rpc("{\"jsonrpc\":\"2.0\",\"id\":99,\"method\":\"ping\"}",
                  &r) == 0, "ping responds");
        const char *rs; uint32_t rsl;
        CHECK(!bt_mcp_json_top_raw(&r, "result", &rs, &rsl) && rsl == 2
              && rs[0] == '{' && rs[1] == '}', "ping result is empty object");
    }

    /* ---- 2. tools/list：每项必须有 name/description/inputSchema ---- */
    {
        CHECK(rpc("{\"jsonrpc\":\"2.0\",\"id\":2,\"method\":\"tools/list\"}",
                  &r) == 0, "tools/list responds");
        const char *arr; uint32_t al;
        CHECK(!has(&r, "tools", &arr, &al) && al > 2 && arr[0] == '[',
              "tools is array");
        /* 抽查：必须有 inputSchema 字样，且不出现裸 name 缺失 */
        CHECK(mem_find(g_json, g_json_len, "\"inputSchema\"", sizeof "\"inputSchema\"" - 1) != NULL,
              "tools contain inputSchema");
        CHECK(mem_find(g_json, g_json_len, "\"name\"", sizeof "\"name\"" - 1) != NULL,
              "tools contain name");
        CHECK(mem_find(g_json, g_json_len, "\"annotations\"", sizeof "\"annotations\"" - 1) != NULL,
              "annotations emitted when set");
        CHECK(mem_find(g_json, g_json_len, "\"outputSchema\"", sizeof "\"outputSchema\"" - 1) != NULL,
              "outputSchema emitted when set");
        CHECK(mem_find(g_json, g_json_len, "readOnlyHint", sizeof "readOnlyHint" - 1) != NULL,
              "annotation hint spelled per spec");
    }

    /* ---- 3. tools/call 成功：content[] 每项有 type ---- */
    {
        CHECK(rpc("{\"jsonrpc\":\"2.0\",\"id\":3,\"method\":\"tools/call\","
                  "\"params\":{\"name\":\"t_text\"}}", &r) == 0, "call text");
        const char *c; uint32_t cl;
        CHECK(!has(&r, "content", &c, &cl) && cl > 2 && c[0] == '[',
              "content is array");
        CHECK(mem_find(g_json, g_json_len, "\"type\":\"text\"", sizeof "\"type\":\"text\"" - 1) != NULL,
              "content item has type");
        CHECK(mem_find(g_json, g_json_len, "\"isError\":false", sizeof "\"isError\":false" - 1) != NULL,
              "isError present and false");
    }

    /* ---- 4. 富内容：image 需 data+mimeType，resource 需 uri+mimeType+text ---- */
    {
        CHECK(rpc("{\"jsonrpc\":\"2.0\",\"id\":4,\"method\":\"tools/call\","
                  "\"params\":{\"name\":\"t_rich\"}}", &r) == 0, "call rich");
        CHECK(mem_find(g_json, g_json_len, "\"type\":\"image\"", sizeof "\"type\":\"image\"" - 1) != NULL,
              "image item");
        CHECK(mem_find(g_json, g_json_len, "\"mimeType\":\"image/png\"", sizeof "\"mimeType\":\"image/png\"" - 1) != NULL,
              "image mimeType");
        CHECK(mem_find(g_json, g_json_len, "\"type\":\"resource\"", sizeof "\"type\":\"resource\"" - 1) != NULL,
              "resource item");
        CHECK(mem_find(g_json, g_json_len, "\"uri\":\"bt-mcp://dev/r\"", sizeof "\"uri\":\"bt-mcp://dev/r\"" - 1) != NULL,
              "resource uri");
        /* v0.5 V5：_meta 必须在 result 层（content 数组闭合之后），
         * 不能掉进 content[] 里变成文本项 */
        const char *meta = mem_find(g_json, g_json_len, "\"_meta\":{",
                                   sizeof "\"_meta\":{" - 1);
        const char *close = mem_find(g_json, g_json_len, "\"isError\"",
                                    sizeof "\"isError\"" - 1);
        CHECK(meta && close && meta < close, "_meta 在 result 层（isError 之前）");
        /* 且 content 数组必须已闭合：content 的 ']' 出现在 _meta 之前 */
        const char *carr = mem_find(g_json, g_json_len, "\"content\":[",
                                   sizeof "\"content\":[" - 1);
        const char *cend = carr ? mem_find(carr, g_json_len
                                          - (size_t)(carr - (const char *)g_json),
                                          "]", 1) : NULL;
        CHECK(carr && meta && cend && cend < meta,
              "content 数组在 _meta 之前闭合");
    }

    /* ---- 5. 工具失败：isError=true 且仍有 content（Host 才能展示原因）---- */
    {
        CHECK(rpc("{\"jsonrpc\":\"2.0\",\"id\":5,\"method\":\"tools/call\","
                  "\"params\":{\"name\":\"t_fail\"}}", &r) == 0, "call fail");
        CHECK(mem_find(g_json, g_json_len, "\"isError\":true", sizeof "\"isError\":true" - 1) != NULL,
              "isError true on tool failure");
        CHECK(mem_find(g_json, g_json_len, "\"content\":[", sizeof "\"content\":[" - 1) != NULL,
              "content still present on failure");
    }

    /* ---- 6. 授权：CONSENT 未放行 → -32003；RESTRICTED 恒拒 ---- */
    {
        g_consent_allow = 0;
        CHECK(rpc("{\"jsonrpc\":\"2.0\",\"id\":6,\"method\":\"tools/call\","
                  "\"params\":{\"name\":\"t_consent\"}}", &r) == 0, "consent deny");
        CHECK(mem_find(g_json, g_json_len, "\"code\":-32003", sizeof "\"code\":-32003" - 1) != NULL,
              "consent denial maps to -32003");
        g_consent_allow = 1;
        CHECK(rpc("{\"jsonrpc\":\"2.0\",\"id\":7,\"method\":\"tools/call\","
                  "\"params\":{\"name\":\"t_consent\"}}", &r) == 0, "consent allow");
        CHECK(mem_find(g_json, g_json_len, "\"code\":-32003", sizeof "\"code\":-32003" - 1) == NULL,
              "consent allow passes");
        CHECK(rpc("{\"jsonrpc\":\"2.0\",\"id\":8,\"method\":\"tools/call\","
                  "\"params\":{\"name\":\"t_restricted\"}}", &r) == 0,
              "restricted responds");
        CHECK(mem_find(g_json, g_json_len, "\"code\":-32003", sizeof "\"code\":-32003" - 1) != NULL,
              "restricted always denied");
    }

    /* ---- 7. 错误码：未知方法 -32601、坏参数 -32602、未初始化 -32001、
     *         资源不存在 -32002、权限不足 -32003（V7 语义化） ---- */
    {
        CHECK(rpc("{\"jsonrpc\":\"2.0\",\"id\":9,\"method\":\"nope\"}", &r) == 0,
              "unknown method responds");
        CHECK(mem_find(g_json, g_json_len, "\"code\":-32601", sizeof "\"code\":-32601" - 1) != NULL,
              "unknown method -> -32601");
        CHECK(rpc("{\"jsonrpc\":\"2.0\",\"id\":10,\"method\":\"tools/call\","
                  "\"params\":{\"name\":\"missing\"}}", &r) == 0, "bad tool");
        CHECK(mem_find(g_json, g_json_len, "\"code\":-32602", sizeof "\"code\":-32602" - 1) != NULL,
              "unknown tool -> -32602");
        CHECK(rpc("{\"jsonrpc\":\"2.0\",\"id\":11,\"method\":\"resources/read\","
                  "\"params\":{\"uri\":\"bt-mcp://dev/nope\"}}", &r) == 0,
              "missing resource responds");
        CHECK(mem_find(g_json, g_json_len, "\"code\":-32002", sizeof "\"code\":-32002" - 1) != NULL,
              "missing resource -> -32002");
        (void)rpc("garbage{", &r);   /* 语法错：只要求不崩 */
    }

    /* ---- 8. resources/list 与 prompts/list 的字段 ---- */
    {
        CHECK(rpc("{\"jsonrpc\":\"2.0\",\"id\":12,\"method\":\"resources/list\"}",
                  &r) == 0, "resources/list responds");
        CHECK(mem_find(g_json, g_json_len, "\"uri\":\"bt-mcp://dev/r\"", sizeof "\"uri\":\"bt-mcp://dev/r\"" - 1) != NULL,
              "resource has uri");
        CHECK(mem_find(g_json, g_json_len, "\"size\":5", sizeof "\"size\":5" - 1) != NULL,
              "resource size emitted");
        CHECK(rpc("{\"jsonrpc\":\"2.0\",\"id\":13,\"method\":\"prompts/list\"}",
                  &r) == 0, "prompts/list responds");
        CHECK(mem_find(g_json, g_json_len, "\"prompts\":[", sizeof "\"prompts\":[" - 1) != NULL,
              "prompts array");
        CHECK(rpc("{\"jsonrpc\":\"2.0\",\"id\":14,\"method\":\"prompts/get\","
                  "\"params\":{\"name\":\"p\"}}", &r) == 0, "prompts/get responds");
        CHECK(mem_find(g_json, g_json_len, "\"messages\":[", sizeof "\"messages\":[" - 1) != NULL,
              "prompt has messages");
        CHECK(mem_find(g_json, g_json_len, "\"role\":\"user\"", sizeof "\"role\":\"user\"" - 1) != NULL,
              "prompt message has role");
        /* w_meta 的正确形态：一次引号、无重复包裹 */
        CHECK(mem_find(g_json, g_json_len, "\"_meta\":{\"io.btmcp/src\":\"conf\"}",
                       sizeof "\"_meta\":{\"io.btmcp/src\":\"conf\"}" - 1) != NULL,
              "prompt _meta via w_meta (single-quoted value)");
    }

    /* ---- 9. 订阅与日志：result 为空对象、通知形状 ---- */
    {
        CHECK(rpc("{\"jsonrpc\":\"2.0\",\"id\":15,\"method\":\"resources/subscribe\","
                  "\"params\":{\"uri\":\"bt-mcp://dev/r\"}}", &r) == 0,
              "subscribe responds");
        CHECK(mem_find(g_json, g_json_len, "\"result\":{}", sizeof "\"result\":{}" - 1) != NULL,
              "subscribe result is empty object");

        g_wire.n = 0;
        CHECK(bt_mcp_server_notify_resource_updated(&g_sv, "bt-mcp://dev/r") == 0,
              "notify_resource_updated ok");
        unwrap_wire();
        CHECK(mem_find(g_json, g_json_len, "notifications/resources/updated", sizeof "notifications/resources/updated" - 1) != NULL,
              "resource updated notification method");

        g_wire.n = 0;
        CHECK(bt_mcp_server_log(&g_sv, BT_MCP_LOG_WARNING, "drv", "warn", 4) == 0,
              "log emits");
        unwrap_wire();
        CHECK(mem_find(g_json, g_json_len, "notifications/message", sizeof "notifications/message" - 1) != NULL,
              "log message method");
        if (mem_find(g_json, g_json_len, "\"level\":\"warning\"",
                     sizeof "\"level\":\"warning\"" - 1) == NULL) {
            printf("  (实际 log 响应: %.*s)\n", (int)g_json_len, (char *)g_json);
        }
        CHECK(mem_find(g_json, g_json_len, "\"level\":\"warning\"",
                       sizeof "\"level\":\"warning\"" - 1) != NULL,
              "log level per spec");
        /* 低于阈值应被丢弃 */
        g_wire.n = 0;
        CHECK(bt_mcp_server_log(&g_sv, BT_MCP_LOG_DEBUG, "drv", "dbg", 3) == 0,
              "below-threshold log returns ok");
        CHECK(g_wire.n == 0, "below-threshold log is dropped");
    }

    /* ---- 10. list_changed 通知方法名 ---- */
    {
        g_wire.n = 0;
        CHECK(bt_mcp_server_notify_list_changed(&g_sv, "tools") == 0, "list_changed");
        unwrap_wire();
        CHECK(mem_find(g_json, g_json_len, "notifications/tools/list_changed", sizeof "notifications/tools/list_changed" - 1)
              != NULL, "tools list_changed method name");
    }

    printf("\n%d pass, %d fail\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
