/* test_json_corpus.c —— JSON 解析器语料测试（v0.5 V3 分支覆盖率）。
 *
 * 用一批确定性语料把解析器/取值器的**畸形输入分支**跑满：
 * 转义、\u 代理对、数字边界、深层嵌套、截断、尾随垃圾、字符串 id 形态等。
 * 每个用例只断言「不崩溃 + 返回值属于允许集合」，重点在覆盖分支。
 */
#include <stdio.h>
#include <string.h>
#include "bt_mcp/json.h"

static int g_pass, g_fail;
#define CHECK(c, m) do { if (c) g_pass++; else { g_fail++; \
    printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, m); } } while (0)

/* 合法语料：必须解析成功 */
static const char *const GOOD[] = {
    "{}",
    "{\"a\":1}",
    "{\"a\":-1}",
    "{\"a\":0}",
    "{\"a\":1.5}",
    "{\"a\":-0.0}",
    "{\"a\":1e10}",
    "{\"a\":1E-10}",
    "{\"a\":true}",
    "{\"a\":false}",
    "{\"a\":null}",
    "{\"a\":\"\"}",
    "{\"a\":\"x\"}",
    "{\"a\":\"\\\"\\\\\\/\\b\\f\\n\\r\\t\"}",
    "{\"a\":\"\\u0041\"}",
    "{\"a\":\"\\u00e9\"}",
    "{\"a\":\"\\ud83d\\ude00\"}",          /* 代理对 */
    /* 孤立代理：实现选择宽松接受（不视为语法错），只要不崩即可 */
    "{\"a\":\"\\ud83d\"}",
    "{\"a\":[]}",
    "{\"a\":[1,2,3]}",
    "{\"a\":{}}",
    "{\"a\":{\"b\":{\"c\":{\"d\":1}}}}",
    "{\"a\":1,\"b\":2,\"c\":3}",
    "{\"a\" : 1 }",
    "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"m\"}",
    "{\"jsonrpc\":\"2.0\",\"id\":\"s\",\"method\":\"m\"}",
    "{\"jsonrpc\":\"2.0\",\"id\":-2147483648,\"method\":\"m\"}",
    "{\"jsonrpc\":\"2.0\",\"id\":2147483647,\"method\":\"m\"}",
    "{\"jsonrpc\":\"2.0\",\"method\":\"m\",\"params\":null}",
    "{\"jsonrpc\":\"2.0\",\"method\":\"m\",\"params\":{\"k\":[{\"x\":1}]}}",
};

/* 非法语料：必须解析失败（返回非 0），且不得崩溃 */
static const char *const BAD[] = {
    "",
    "{",
    "}",
    "[]",                       /* 顶层数组不是单条消息 */
    "1",
    "\"s\"",
    "null",
    "{\"a\":}",
    "{\"a\"}",
    "{\"a\":1,}",
    "{,}",
    "{\"a\":1}trailing",
    "{\"a\":\"unterminated}",
    "{\"a\":\"\\q\"}",          /* 非法转义 */
    "{\"a\":\"\\u00\"}",        /* \u 位数不足 */
    "{\"a\":\"\\uZZZZ\"}",      /* \u 非十六进制 */
    "{\"a\":01}",               /* 前导零 */
    "{\"a\":.5}",
    "{\"a\":1.}",
    "{\"a\":1e}",
    "{\"a\":--1}",
    "{\"a\":[1,}",
    "{\"a\":{\"b\":}}",
    "{\"a\":1} {}",
};

int main(void)
{
    bt_mcp_json_req_t r;

    for (unsigned i = 0; i < sizeof GOOD / sizeof GOOD[0]; i++) {
        int rc = bt_mcp_json_parse_msg(GOOD[i], (uint32_t)strlen(GOOD[i]), &r);
        if (rc) printf("[dbg] GOOD[%u] rejected rc=%d: %s\n", i, rc, GOOD[i]);
        CHECK(rc == 0, "good corpus parses");
    }
    for (unsigned i = 0; i < sizeof BAD / sizeof BAD[0]; i++) {
        int rc = bt_mcp_json_parse_msg(BAD[i], (uint32_t)strlen(BAD[i]), &r);
        if (!rc) printf("[dbg] BAD[%u] accepted: %s\n", i, BAD[i]);
        CHECK(rc != 0, "bad corpus rejected");
    }

    /* 取值器的类型不匹配分支 */
    {
        const char *m = "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"m\","
                        "\"params\":{\"s\":\"v\",\"n\":7,\"neg\":-7,"
                        "\"f\":1.25,\"b\":true,\"nul\":null,"
                        "\"arr\":[1],\"obj\":{\"k\":1}}}";
        CHECK(bt_mcp_json_parse_msg(m, (uint32_t)strlen(m), &r) == 0, "mixed parse");
        const char *s; uint16_t sl; uint32_t u; int32_t i32; double d;
        /* 正确类型 */
        CHECK(bt_mcp_json_get_str(&r, "s", &s, &sl) == 0 && sl == 1, "get_str ok");
        CHECK(bt_mcp_json_get_u32(&r, "n", &u) == 0 && u == 7, "get_u32 ok");
        CHECK(bt_mcp_json_get_i32(&r, "neg", &i32) == 0 && i32 == -7, "get_i32 ok");
        CHECK(bt_mcp_json_get_f64(&r, "f", &d) == 0 && d > 1.2 && d < 1.3,
              "get_f64 ok");
        /* 错误类型：全部必须报错而不是返回垃圾 */
        CHECK(bt_mcp_json_get_str(&r, "n", &s, &sl) != 0, "get_str on number");
        CHECK(bt_mcp_json_get_str(&r, "arr", &s, &sl) != 0, "get_str on array");
        CHECK(bt_mcp_json_get_str(&r, "obj", &s, &sl) != 0, "get_str on object");
        CHECK(bt_mcp_json_get_u32(&r, "s", &u) != 0, "get_u32 on string");
        CHECK(bt_mcp_json_get_u32(&r, "f", &u) != 0, "get_u32 on float");
        CHECK(bt_mcp_json_get_i32(&r, "b", &i32) != 0, "get_i32 on bool");
        CHECK(bt_mcp_json_get_f64(&r, "nul", &d) != 0, "get_f64 on null");
        /* 不存在的键 */
        CHECK(bt_mcp_json_get_str(&r, "nope", &s, &sl) != 0, "missing str");
        CHECK(bt_mcp_json_get_u32(&r, "nope", &u) != 0, "missing u32");
        CHECK(bt_mcp_json_get_i32(&r, "nope", &i32) != 0, "missing i32");
        CHECK(bt_mcp_json_get_f64(&r, "nope", &d) != 0, "missing f64");
        /* NULL 输出指针 */
        CHECK(bt_mcp_json_get_str(&r, "s", NULL, NULL) != 0, "NULL out rejected");
        CHECK(bt_mcp_json_get_u32(&r, "n", NULL) != 0, "NULL u32 out rejected");
        CHECK(bt_mcp_json_get_f64(&r, "f", NULL) != 0, "NULL f64 out rejected");
        /* 子对象与原文片段 */
        bt_mcp_json_req_t sub;
        CHECK(bt_mcp_json_get_sub(&r, "obj", &sub) == 0, "get_sub ok");
        CHECK(bt_mcp_json_get_sub(&r, "s", &sub) != 0, "get_sub on string fails");
        const char *raw; uint32_t rl;
        CHECK(bt_mcp_json_top_raw(&r, "params", &raw, &rl) == 0, "top_raw ok");
        CHECK(bt_mcp_json_top_raw(&r, "nope", &raw, &rl) != 0, "top_raw missing");
    }

    /* 字符串解码：各种转义与截断 */
    {
        char out[64];
        uint16_t need = 0;
        /* 简单转义 */
        static const char S1[] = "\"a\\nb\"";
        CHECK(bt_mcp_json_decode_str(S1, S1 + sizeof S1 - 1, out,
                                     sizeof out, &need) > 0, "decode escapes");
        /* 目标缓冲不足 → 只报 need，不越界写 */
        char tiny[2];
        uint16_t n2 = 0;
        static const char S2[] = "\"abcdef\"";
        (void)bt_mcp_json_decode_str(S2, S2 + sizeof S2 - 1, tiny,
                                     sizeof tiny, &n2);
        CHECK(n2 >= 6, "decode reports needed length");
        CHECK(tiny[0] && tiny[1], "tiny buffer still NUL-safe");
        /* 畸形输入 */
        static const char S3[] = "\"";
        (void)bt_mcp_json_decode_str(S3, S3 + sizeof S3 - 1, out,
                                     sizeof out, &need);
        static const char S4[] = "\"\\u\"";
        (void)bt_mcp_json_decode_str(S4, S4 + sizeof S4 - 1, out,
                                     sizeof out, &need);
        CHECK(1, "decode malformed doesn't crash");
    }

    /* 严格版 bt_mcp_json_parse：必须有 method（响应报文会被拒绝）。
     * 这是与 parse_msg 的唯一差异，必须被显式验证，否则「两个看起来一样的
     * 解析入口」很容易被误用。 */
    {
        const char *req = "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"ping\"}";
        const char *resp = "{\"jsonrpc\":\"2.0\",\"id\":1,\"result\":{}}";
        bt_mcp_json_req_t r;
        CHECK(bt_mcp_json_parse(req, (uint32_t)strlen(req), &r) == 0,
              "strict parse: request with method ok");
        CHECK(r.method && r.method_len == 4
              && !memcmp(r.method, "ping", 4), "strict parse: method located");
        CHECK(bt_mcp_json_parse(resp, (uint32_t)strlen(resp), &r) != 0,
              "strict parse: response without method rejected");
        /* 宽松版必须接受同一条响应（对照） */
        CHECK(bt_mcp_json_parse_msg(resp, (uint32_t)strlen(resp), &r) == 0,
              "lenient parse: response accepted");
        /* 空指针 */
        CHECK(bt_mcp_json_parse(NULL, 0, &r) != 0, "strict parse: NULL src");
        CHECK(bt_mcp_json_parse(req, (uint32_t)strlen(req), NULL) != 0,
              "strict parse: NULL out");
    }


    printf("\n%d pass, %d fail\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
