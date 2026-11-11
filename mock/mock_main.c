/* mock_main.c —— host-mock 差分驱动器（草案 §8 host-mock port）。
 * 协议与 oracle 完全一致：
 *   encode   : stdin = u32 kind | u16 mtu | u32 len | payload → stdout = 帧字节
 *   decode-p : stdin = [u16 片长 + 片]*  → stdout = [u32 kind | u32 len | msg]*
 *   decode-s : stdin = 流字节            → stdout 同 decode-p
 *   session  : stdin = u8 chan | u16 mtu | 指令序列（M3 差分，L3+L4 全栈）
 *              每条指令输出一条记录：
 *                i32 rc | u32 wire_len + wire | u32 nmsgs
 *                  + 每条上抛消息 { i32 rx_rc | u32 len + bytes }
 *              指令：
 *                0x01 LINK_UP  u32 token
 *                0x02 LINK_DOWN u8 bond_intact
 *                0x03 TX       u16 len + MCP 原文（session_tx）
 *                0x04 RX       u16 len + wire 帧字节（喂 framer→session_rx）
 *                0x05 CTRL     u8 type | u8 alen + args（session_ctrl）
 *                0x06 TICK     u32 now_ms
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdlib.h>
#include "bt_mcp/frame.h"
#include "bt_mcp/session.h"
#include "bt_mcp/mcp_server.h"
#include "bt_mcp/err.h"

static uint32_t rd_u32(const uint8_t *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 |
           (uint32_t)p[3] << 24;
}

/* emit sink：把片字节追加到动态缓冲（宿主机 mock 允许 malloc） */
typedef struct { uint8_t *buf; size_t len, cap; } vec_t;

static int vec_emit(void *io, const uint8_t *data, uint16_t len)
{
    vec_t *v = io;
    if (v->len + len > v->cap) {
        size_t nc = v->cap ? v->cap * 2 : 256;
        while (nc < v->len + len) nc *= 2;
        v->buf = realloc(v->buf, nc);
        v->cap = nc;
    }
    memcpy(v->buf + v->len, data, len);
    v->len += len;
    return 0;
}

/* msg sink：包通道差分时每片需带「u16 片长前缀」——由 harness 在两片之间插。
   这里只把消息记入数组（差分比对的输出格式在 main 里拼）。 */
#define MAX_OUT_MSGS 64
static struct { uint8_t kind; uint8_t seq; uint16_t len; uint8_t buf[BT_MCP_CFG_MAX_MSG]; }
    g_msgs[MAX_OUT_MSGS];
static int g_nmsgs;

static void sink_msg(void *user, uint8_t kind, uint8_t seq,
                     const uint8_t *payload, uint16_t len)
{
    (void)user;
    if (g_nmsgs >= MAX_OUT_MSGS || len > BT_MCP_CFG_MAX_MSG) return;
    g_msgs[g_nmsgs].kind = kind;
    g_msgs[g_nmsgs].seq = seq;
    g_msgs[g_nmsgs].len = len;
    if (len) memcpy(g_msgs[g_nmsgs].buf, payload, len);
    g_nmsgs++;
}

static uint8_t *read_all(size_t *out_len)
{
    size_t cap = 65536, len = 0;
    uint8_t *buf = malloc(cap);
    size_t n;
    while ((n = fread(buf + len, 1, cap - len, stdin)) > 0) {
        len += n;
        if (len == cap) { cap *= 2; buf = realloc(buf, cap); }
    }
    *out_len = len;
    return buf;
}

static void write_msgs(void)
{
    for (int i = 0; i < g_nmsgs; i++) {
        uint8_t h[8];
        uint32_t kind = g_msgs[i].kind, len = g_msgs[i].len;
        h[0] = (uint8_t)kind; h[1] = (uint8_t)(kind >> 8);
        h[2] = (uint8_t)(kind >> 16); h[3] = (uint8_t)(kind >> 24);
        h[4] = (uint8_t)len;  h[5] = (uint8_t)(len >> 8);
        h[6] = (uint8_t)(len >> 16); h[7] = (uint8_t)(len >> 24);
        fwrite(h, 1, 8, stdout);
        fwrite(g_msgs[i].buf, 1, g_msgs[i].len, stdout);
    }
}

static int run_session(const uint8_t *in, size_t in_len);

/* ---- M4a：mcp 模式（L5 差分驱动） ----
 * 输入：u8 chan | u16 mtu | u8 tools_count | [工具]* | 指令序列
 * 工具记录：u8 name_len + name | u8 desc_len + desc（fn 固定 echo）
 * 指令 0x07 MSG u16 len + MCP 消息原文 → bt_mcp_server_rx
 * 每条指令输出（与 session 模式同形）：i32 rc | u32 wire_len + wire
 *   | u32 nmsgs + {i32 rx_rc | u32 len + bytes}*
 * wire = server 响应经 framer 发出的帧字节；nmsgs 恒 0。 */
static int run_mcp(const uint8_t *in, size_t in_len);
static int run_e2e(const uint8_t *in, size_t in_len);

/* echo 工具：把 params.text 原文写进 content（server 负责转义），缺失回错 */
static int tool_echo(const bt_mcp_json_req_t *r, bt_mcp_writer_t *w,
                     bt_mcp_call_t *call, void *user)
{
    (void)call; (void)user;
    const char *text; uint16_t tl;
    if (bt_mcp_json_get_str(r, "text", &text, &tl))
        return BT_MCP_E_PARSE;
    /* 解转义后按原文输出（server 负责 JSON 转义）；超 MAX_MSG 截断 */
    static char buf[BT_MCP_CFG_MAX_MSG];
    uint16_t need = 0;
    uint16_t n = bt_mcp_json_decode_str(text - 1, text + tl + 1, buf,
                                        sizeof buf - 1, &need);
    (void)need;
    return bt_mcp_w_raw(w, buf, n);
}

/* ---- 资源表（差分固定面：与 oracle 逐字节同形） ---- */
typedef struct { const char *text; uint32_t len; uint32_t page; } res_src_t;

static int res_read(void *user, uint32_t cursor, const char **text,
                    uint32_t *len, uint32_t *next)
{
    const res_src_t *s = user;
    if (cursor >= s->len) {
        *text = ""; *len = 0; *next = BT_MCP_CURSOR_END;
        return 0;
    }
    uint32_t n = s->len - cursor;
    if (s->page && n > s->page) n = s->page;
    *text = s->text + cursor;
    *len = n;
    uint32_t nx = cursor + n;
    *next = (nx < s->len) ? nx : BT_MCP_CURSOR_END;
    return 0;
}

static const char RES_HR[]  = "hello resource";
static const char RES_LOG[] = "0123456789ABCDEFGHIJ";
static const res_src_t RES_HR_SRC  = { RES_HR,  sizeof RES_HR  - 1, 0 };
static const res_src_t RES_LOG_SRC = { RES_LOG, sizeof RES_LOG - 1, 8 };

/* 订阅回调：只记录，用于让 notifications/resources/updated 可被触发 */
static int g_sub_log;
static int res_sub(void *user, int subscribe)
{
    (void)user;
    g_sub_log = subscribe;
    return 0;
}

static const bt_mcp_resource_def_t MOCK_RES[] = {
    { .name = "hr_window", .desc = "heart rate window", .title = "Heart rate",
      .mime_type = "text/plain", .authz = BT_MCP_AUTHZ_OPEN,
      .read = res_read, .user = (void *)&RES_HR_SRC },
    { .name = "log", .desc = "device log", .mime_type = "text/plain",
      .size = (uint32_t)(sizeof RES_LOG - 1),   /* 20B，见 RES_LOG */
      .authz = BT_MCP_AUTHZ_OPEN, .read = res_read, .sub = res_sub,
      .user = (void *)&RES_LOG_SRC },
};

/* proposals/授权测试工具：consent 放行规则 = arguments.allow 为 true */
static int consent_cb(void *user, const char *tool_name,
                      const bt_mcp_json_req_t *req)
{
    (void)user; (void)tool_name;
    const char *v; uint32_t vl;
    if (bt_mcp_json_get_raw(req, "arguments", &v, &vl)) return -1;
    /* 在 arguments 片段里找 allow:true（简化判定，仅 mock 用） */
    for (uint32_t i = 0; i + 12 <= vl; i++)
        if (!memcmp(v + i, "\"allow\":true", 12)) return 0;
    return -1;
}

static int tool_allow(const bt_mcp_json_req_t *r, bt_mcp_writer_t *w,
                      bt_mcp_call_t *c, void *user)
{
    (void)r; (void)c; (void)user;
    return bt_mcp_w_content_text(w, "allowed", 7);
}

static int g_rich_tool_present;

/* 富内容工具：text + image + audio + resource 四个内容项 */
static int tool_rich(const bt_mcp_json_req_t *r, bt_mcp_writer_t *w,
                     bt_mcp_call_t *c, void *user)
{
    (void)r; (void)c; (void)user;
    int rc = bt_mcp_w_raw(w, "plain", 5);            /* 裸写 → 文本项 */
    if (!rc) rc = bt_mcp_w_content_text(w, "second", 6);
    if (!rc) rc = bt_mcp_w_content_image(w, "aGVsbG8=", 8, "image/png");
    if (!rc) rc = bt_mcp_w_content_audio(w, "AAEC", 4, "audio/wav");
    if (!rc) rc = bt_mcp_w_content_resource(w, "bt-mcp://dev/log", "text/plain",
                                            "res", 3);
    return rc;
}

/* prompts：一个静态 prompt */
static int prompt_greet(const bt_mcp_json_req_t *r, bt_mcp_writer_t *w, void *user)
{
    (void)r; (void)user;
    int rc = bt_mcp_w_key(w, "description");
    if (!rc) rc = bt_mcp_w_str_lit(w, "A friendly greeting");
    w->need_comma = 1;
    if (!rc) rc = bt_mcp_w_key(w, "messages");
    static const char MSGS[] =
        "[{\"role\":\"user\",\"content\":"
        "{\"type\":\"text\",\"text\":\"hello\"}}]";
    if (!rc) rc = bt_mcp_w_raw(w, MSGS, (uint32_t)(sizeof MSGS - 1));
    w->need_comma = 1;
    return rc;
}

static const bt_mcp_prompt_def_t MOCK_PROMPTS[] = {
    { .name = "greet", .desc = "greeting prompt", .title = "Greet",
      .get = prompt_greet },
};
#define MOCK_PROMPT_COUNT ((uint16_t)(sizeof MOCK_PROMPTS / sizeof MOCK_PROMPTS[0]))
#define MOCK_RES_COUNT ((uint16_t)(sizeof MOCK_RES / sizeof MOCK_RES[0]))

int main(int argc, char **argv)
{
    if (argc < 2) {
        fprintf(stderr, "usage: %s encode|decode-p|decode-s|session|mcp|e2e\n",
                argv[0]);
        return 2;
    }

    size_t in_len;
    uint8_t *in = read_all(&in_len);

    if (!strcmp(argv[1], "encode")) {
        if (in_len < 10) return 2;
        uint32_t kind = rd_u32(in);
        uint16_t mtu = (uint16_t)(in[4] | (in[5] << 8));
        uint32_t plen = rd_u32(in + 6);
        if ((size_t)10 + plen > in_len) return 2;

        bt_mcp_chan_cap_t cap = { .kind = (mtu ? BT_MCP_CHAN_PACKET
                                               : BT_MCP_CHAN_STREAM),
                                  .mtu = mtu, .reliable = 1, .flow_ctrl = 0,
                                  .max_pending = 4 };
        static bt_mcp_framer_t fr; /* 8KB+ 静态，避免栈溢出 */
        bt_mcp_frame_cfg_t cfg = { .max_reasm = 4, .frag_tout_ms = 2000 };
        if (bt_mcp_framer_init(&fr, &cfg, &cap)) return 2;

        vec_t v = { 0 };
        int rc = bt_mcp_framer_send(&fr, (uint8_t)kind, in + 10, plen,
                                    vec_emit, &v);
        if (rc) return 3;
        fwrite(v.buf, 1, v.len, stdout);
    } else if (!strcmp(argv[1], "decode-p")) {
        static bt_mcp_framer_t fr;
        bt_mcp_chan_cap_t cap = { .kind = BT_MCP_CHAN_PACKET, .mtu = 244,
                                  .reliable = 1, .flow_ctrl = 0, .max_pending = 4 };
        bt_mcp_frame_cfg_t cfg = { .max_reasm = 4, .frag_tout_ms = 2000 };
        if (bt_mcp_framer_init(&fr, &cfg, &cap)) return 2;
        size_t off = 0;
        while (off + 2 <= in_len) {
            uint16_t flen = (uint16_t)(in[off] | (in[off + 1] << 8));
            off += 2;
            if (off + flen > in_len) return 3;
            if (bt_mcp_framer_feed(&fr, in + off, flen, sink_msg, NULL)) return 3;
            off += flen;
        }
        write_msgs();
    } else if (!strcmp(argv[1], "decode-s")) {
        static bt_mcp_framer_t fr;
        bt_mcp_chan_cap_t cap = { .kind = BT_MCP_CHAN_STREAM, .mtu = 0,
                                  .reliable = 1, .flow_ctrl = 1, .max_pending = 4 };
        bt_mcp_frame_cfg_t cfg = { .max_reasm = 4, .frag_tout_ms = 2000 };
        if (bt_mcp_framer_init(&fr, &cfg, &cap)) return 2;
        /* 任意字节喂入：整段一次 feed */
        if (bt_mcp_framer_feed(&fr, in, (uint16_t)in_len, sink_msg, NULL)) return 3;
        write_msgs();
    } else if (!strcmp(argv[1], "session")) {
        return run_session(in, in_len);
    } else if (!strcmp(argv[1], "mcp")) {
        return run_mcp(in, in_len);
    } else if (!strcmp(argv[1], "e2e")) {
        return run_e2e(in, in_len);
    } else {
        return 2;
    }
    return 0;
}

/* run_mcp 实现在文件尾（依赖 M3 段的 g_step/wr_step）。 */

/* ---- M3：session 模式（L3+L4 全栈差分驱动） ----
 * 输入：u8 chan(0=stream 1=packet) | u16 mtu | [指令]*
 * 每条指令输出：i32 rc | u32 wire_len | wire | u32 nmsgs
 *   + 每条上抛消息 i32 rx_rc | u32 len | bytes
 * TX 指令的 wire = session_tx 经 framer 发出的帧字节。
 * RX 指令的 wire = 控制帧自动应答（PONG/RESUME_OK/REJ）写出的帧字节；
 *   上抛消息 = LINK_UP 门放行后转 mcp_sink 的 MCP 消息（RX 指令 wire 输入里
 *   的 MCP 帧 payload）。控制帧终结不上抛（判例：终结在 L4）。
 * nmsgs 语义：RX 指令 = 通过状态门进入 mcp_sink 的 MCP 消息条数；
 *   其余指令 = 0。 */

typedef struct {
    int32_t rc;
    vec_t  wire;          /* 本指令写出的帧字节 */
    int32_t nmsgs;
    struct { int32_t rx_rc; uint32_t len; const uint8_t *p; } msgs[8];
} step_out_t;

static step_out_t g_step;

static void mcp_up(void *user, const uint8_t *json, uint32_t len)
{
    step_out_t *st = user;
    if (st->nmsgs < 8) {
        st->msgs[st->nmsgs].rx_rc = 0;
        st->msgs[st->nmsgs].len = len;
        /* json 指向 framer 内部重组缓冲，指令处理完前有效 */
        st->msgs[st->nmsgs].p = json;
        st->nmsgs++;
    }
}

static void wr_step(const step_out_t *st)
{
    uint32_t wl = (uint32_t)st->wire.len;
    fwrite(&st->rc, 4, 1, stdout);
    fwrite(&wl, 4, 1, stdout);
    if (wl) fwrite(st->wire.buf, 1, wl, stdout);
    fwrite(&st->nmsgs, 4, 1, stdout);
    for (int32_t i = 0; i < st->nmsgs && i < 8; i++) {
        fwrite(&st->msgs[i].rx_rc, 4, 1, stdout);
        fwrite(&st->msgs[i].len, 4, 1, stdout);
        if (st->msgs[i].len) fwrite(st->msgs[i].p, 1, st->msgs[i].len, stdout);
    }
}

/* RX 指令：wire 帧字节喂 framer，上抛消息转 session_rx。
 * 逐帧独立调 session_rx（kind 由 framer 标记），按消息序输出。 */
static int rx_wire(bt_mcp_session_t *se, bt_mcp_framer_t *fr,
                   const uint8_t *in, size_t in_len)
{
    size_t off = 0;
    int32_t last_rx_rc = 0;
    while (off < in_len) {
        g_nmsgs = 0;                        /* 借用 sink_msg 记数组 */
        if (fr->cap->kind == BT_MCP_CHAN_PACKET) {
            if (off + 2 > in_len) return BT_MCP_E_PARSE;
            uint16_t flen = (uint16_t)(in[off] | (in[off + 1] << 8));
            off += 2;
            if ((size_t)off + flen > in_len) return BT_MCP_E_PARSE;
            int rc = bt_mcp_framer_feed(fr, in + off, flen, sink_msg, NULL);
            off += flen;
            if (rc) { last_rx_rc = rc; break; }
        } else {
            int rc = bt_mcp_framer_feed(fr, in, (uint16_t)in_len, sink_msg, NULL);
            if (rc) { last_rx_rc = rc; break; }
            off = in_len;
        }
        /* framer 重组完成的每条消息 → session_rx。
         * 控制帧终结在 L4（自动 PONG/RESUME 应答已进 g_step.wire）；
         * MCP 消息经状态门后上抛（mcp_up 记录）。 */
        for (int i = 0; i < g_nmsgs; i++) {
            step_out_t *st = &g_step;
            int rc = bt_mcp_session_rx(se, g_msgs[i].kind, g_msgs[i].seq,
                                       g_msgs[i].buf, g_msgs[i].len,
                                       g_msgs[i].kind == BT_MCP_KIND_CTRL
                                           ? NULL : mcp_up, st);
            last_rx_rc = rc;
            if (rc == BT_MCP_OK && g_msgs[i].kind == BT_MCP_KIND_CTRL) {
                /* 控制帧应答字节在 session_rx 内部已写 wire——但 session 的
                   emit 直通 vec_emit(&g_step.wire)，此处无需补写。 */
            }
        }
        g_nmsgs = 0;
    }
    bt_mcp_session_pump(se);     /* 冲待发 NACK（缺片） */
    return last_rx_rc;
}

static int run_session(const uint8_t *in, size_t in_len)
{
    if (in_len < 3) return 2;
    uint8_t chan = in[0];
    uint16_t mtu = (uint16_t)(in[1] | (in[2] << 8));

    static bt_mcp_framer_t fr;
    static bt_mcp_session_t se;
    bt_mcp_chan_cap_t cap = { .kind = chan ? BT_MCP_CHAN_PACKET
                                          : BT_MCP_CHAN_STREAM,
                              .mtu = mtu,
                              .reliable = chan ? 0 : 1,   /* GATT 包通道不可靠 */
                              .flow_ctrl = chan ? 0 : 1,
                              .max_pending = 4 };
    bt_mcp_frame_cfg_t fcfg = { .max_reasm = 4, .frag_tout_ms = 2000 };
    if (bt_mcp_framer_init(&fr, &fcfg, &cap)) return 2;
    bt_mcp_session_cfg_t scfg = { .req_timeout_ms = 30000, .resume_tout_ms = 5000,
                                  .tx_window = 4, .role = BT_MCP_ROLE_CLIENT };
    if (bt_mcp_session_init(&se, &scfg)) return 2;
    bt_mcp_session_bind(&se, &fr, vec_emit, &g_step.wire);

    size_t off = 3;
    int32_t rc = 0;
    while (off < in_len) {
        memset(&g_step, 0, sizeof g_step);
        uint8_t op = in[off++];
        switch (op) {
        case 0x01: {                        /* LINK_UP u32 token */
            uint32_t tok = rd_u32(in + off); off += 4;
            bt_mcp_session_link_up(&se, tok);
            rc = 0;
            break;
        }
        case 0x02: {                        /* LINK_DOWN u8 bond */
            int bond = in[off++];
            bt_mcp_session_link_down(&se, bond);
            rc = 0;
            break;
        }
        case 0x03: {                        /* TX u16 len + MCP */
            uint16_t l = (uint16_t)(in[off] | (in[off + 1] << 8)); off += 2;
            if ((size_t)off + l > in_len) return 2;
            rc = bt_mcp_session_tx(&se, in + off, l);
            off += l;
            break;
        }
        case 0x04: {                        /* RX u16 len + wire 字节 */
            uint16_t l = (uint16_t)(in[off] | (in[off + 1] << 8)); off += 2;
            if ((size_t)off + l > in_len) return 2;
            rc = rx_wire(&se, &fr, in + off, l);
            off += l;
            break;
        }
        case 0x05: {                        /* CTRL u8 type u8 alen args */
            uint8_t type = in[off++];
            uint8_t alen = in[off++];
            rc = bt_mcp_session_ctrl(&se, type, in + off, alen);
            off += alen;
            break;
        }
        case 0x06: {                        /* TICK u32 now_ms */
            uint32_t now = rd_u32(in + off); off += 4;
            bt_mcp_session_tick(&se, now);
            rc = 0;
            break;
        }
        default:
            return 2;
        }
        g_step.rc = rc;
        wr_step(&g_step);
        if (g_step.wire.buf) { /* vec 复用前清零长度，缓冲保留复用 */
            /* 不能 free：msgs[].p 指向 framer 重组缓冲，与 wire 无关，安全 */
        }
        g_step.wire.len = 0;
    }
    return 0;
}

/* ---- M4a：run_mcp 实现（须位于 g_step/wr_step 定义之后） ---- */
static int run_mcp(const uint8_t *in, size_t in_len)
{
    if (in_len < 4) return 2;
    uint8_t chan = in[0];
    uint16_t mtu = (uint16_t)(in[1] | (in[2] << 8));
    uint8_t tools_count = in[3];

    /* 工具表：脚本内嵌（name/desc；fn=echo；inputSchema 固定） */
    static bt_mcp_tool_def_t tools[16];
    static char names[16][64], descs[16][128];
    if (tools_count > 16) return 2;
    size_t off = 4;
    for (uint8_t i = 0; i < tools_count; i++) {
        if (off + 2 > in_len) return 2;
        uint8_t nl = in[off++];
        if (nl > 63 || off + nl + 1 > in_len) return 2;
        memcpy(names[i], in + off, nl); names[i][nl] = 0; off += nl;
        uint8_t dl = in[off++];
        if (dl > 127 || off + dl > in_len) return 2;
        memcpy(descs[i], in + off, dl); descs[i][dl] = 0; off += dl;
        tools[i].name = names[i];
        tools[i].desc = descs[i];
        tools[i].input_schema_json =
            "{\"type\":\"object\",\"properties\":{}}";
        tools[i].authz = BT_MCP_AUTHZ_OPEN;
        tools[i].fn = tool_echo;
        tools[i].user = NULL;
    }
    /* 追加固定工具：授权路径可被差分覆盖（名字固定，Host 可见） */
    if (tools_count + 2 <= 16) {
        bt_mcp_tool_def_t *ct = &tools[tools_count++];
        ct->name = "consent_tool";
        ct->desc = "needs consent";
        ct->title = "Consent tool";
        ct->input_schema_json = "{\"type\":\"object\",\"properties\":{}}";
        ct->output_schema_json = "{\"type\":\"object\"}";
        ct->annotations = BT_MCP_TOOL_READONLY | BT_MCP_TOOL_IDEMPOTENT;
        ct->authz = BT_MCP_AUTHZ_CONSENT;
        ct->fn = tool_allow;
        ct->user = NULL;

        bt_mcp_tool_def_t *rt = &tools[tools_count++];
        rt->name = "restricted_tool";
        rt->desc = "always denied";
        rt->title = NULL;
        rt->input_schema_json = "{\"type\":\"object\",\"properties\":{}}";
        rt->output_schema_json = NULL;
        rt->annotations = BT_MCP_TOOL_DESTRUCTIVE;
        rt->authz = BT_MCP_AUTHZ_RESTRICTED;
        rt->fn = tool_allow;
        rt->user = NULL;
    }
    if (!g_rich_tool_present) {
        /* 追加一个富内容工具：验证 text/image/audio/resource 混排 */
        if (tools_count + 1 <= 16) {
            bt_mcp_tool_def_t *rt = &tools[tools_count++];
            rt->name = "rich_tool";
            rt->desc = "multi content";
            rt->title = NULL;
            rt->input_schema_json = "{\"type\":\"object\",\"properties\":{}}";
            rt->output_schema_json = NULL;
            rt->annotations = 0;
            rt->authz = BT_MCP_AUTHZ_OPEN;
            rt->fn = tool_rich;
            rt->user = NULL;
        }
        g_rich_tool_present = 1;
    }

    static bt_mcp_framer_t fr;
    static bt_mcp_session_t se;
    static bt_mcp_server_t sv;
    bt_mcp_chan_cap_t cap = { .kind = chan ? BT_MCP_CHAN_PACKET
                                           : BT_MCP_CHAN_STREAM,
                              .mtu = mtu,
                              .reliable = chan ? 0 : 1,   /* GATT 包通道不可靠 */
                              .flow_ctrl = chan ? 0 : 1,
                              .max_pending = 4 };
    bt_mcp_frame_cfg_t fcfg = { .max_reasm = 4, .frag_tout_ms = 2000 };
    if (bt_mcp_framer_init(&fr, &fcfg, &cap)) return 2;
    bt_mcp_session_cfg_t scfg = { .req_timeout_ms = 30000, .resume_tout_ms = 5000,
                                  .tx_window = 4, .role = BT_MCP_ROLE_SERVER };
    if (bt_mcp_session_init(&se, &scfg)) return 2;
    bt_mcp_session_bind(&se, &fr, vec_emit, &g_step.wire);
    bt_mcp_session_link_up(&se, 0);   /* server 端：LINK_UP 等 initialize */
    bt_mcp_server_cfg_t sv_cfg = { .tools = tools, .tools_count = tools_count,
                                  .resources = MOCK_RES,
                                  .resources_count = MOCK_RES_COUNT,
                                  .prompts = MOCK_PROMPTS,
                                  .prompts_count = MOCK_PROMPT_COUNT,
                                  .consent = consent_cb };
    if (bt_mcp_server_init(&sv, &se, &sv_cfg, "bt-mcp-device", "0.4")) return 2;

    int32_t rc = 0;
    while (off < in_len) {
        memset(&g_step, 0, sizeof g_step);
        uint8_t op = in[off++];
        if (op != 0x07) return 2;      /* 目前只有 MSG 一条指令 */
        if (off + 2 > in_len) return 2;
        uint16_t l = (uint16_t)(in[off] | (in[off + 1] << 8)); off += 2;
        if ((size_t)off + l > in_len) return 2;
        rc = bt_mcp_server_rx(&sv, in + off, l);
        off += l;
        g_step.rc = rc;
        wr_step(&g_step);
        g_step.wire.len = 0;
    }
    return 0;
}

/* ---- M4b：e2e 模式（L2~L5 全栈 client/server 对话差分） ----
 * 输入信封与 mcp 模式一致：u8 chan | u16 mtu | u8 tools_count
 *   | [工具记录]* | 指令序列。指令：
 *   0x08 CMSG  u16 len + MCP 原文 → client session_tx
 *   0x09 SMSG  u16 len + MCP 原文 → server 侧直投 bt_mcp_server_rx
 *   0x0A LUP   u32 token          → 双方 link_up（server 记 token）
 *   0x0B LDN   u8 bond            → 双方 link_down(bond)
 *   0x0C TICK  u32 now_ms         → 双方 tick
 * 每条指令输出记录（i32 rc | u32 nmsgs）：
 *   0x08/0x0A/0x0B/0x0C：wire 自动泵空后 rc=client session_tx/link/tick rc
 *   0x09：nmsgs=泵空产生的 client 上抛消息数，随后每条
 *          {i32 rx_rc | u32 len + bytes}；rx_rc=消息进 client session_rx 的 rc
 *   0x08/0x0A/0x0B/0x0C 的 nmsgs 恒 0。
 * 交叉链路：Cmsg→(server rx)→server 响应→(client rx)→client 上抛消息
 * 经 sink 记录后直接丢弃——客户端应用层逻辑不在差分面内。 */

/* e2e server 侧桥：session 上抛的 MCP 消息转 server_rx（保持 ACK/信用闭环） */
static bt_mcp_server_t *g_pump_sv;
static int g_pump_sink_rc;
static void pump_srv_sink(void *user, const uint8_t *json, uint32_t len)
{
    (void)user;
    g_pump_sink_rc = bt_mcp_server_rx(g_pump_sv, json, len);
}

static vec_t e_c2s, e_s2c;   /* 交叉线缓冲：c2s=client→server，s2c=server→client */

/* 泵空一条线：wire → 对端 framer → session_rx；上抛 MCP 消息逐条记录。
 * 返回最后一条消息的 session_rx rc（无消息=0）。帧序号 client/server
 * 各自独立自增，背靠背不交换 tx_seq——判例与单侧 encode/decode 一致。 */
static int pump_line(bt_mcp_session_t *dst, bt_mcp_framer_t *fr, vec_t *w)
{
    size_t off = 0;
    int32_t last_rc = 0;
    while (off < w->len) {
        g_nmsgs = 0;
        if (fr->cap->kind == BT_MCP_CHAN_PACKET) {
            if (off + 2 > w->len) return BT_MCP_E_PARSE;
            uint16_t flen = (uint16_t)(w->buf[off] | (w->buf[off + 1] << 8));
            off += 2;
            if (off + flen > w->len) return BT_MCP_E_PARSE;
            int rc = bt_mcp_framer_feed(fr, w->buf + off, flen, sink_msg, NULL);
            off += flen;
            if (rc) return rc;
        } else {
            int rc = bt_mcp_framer_feed(fr, w->buf, (uint16_t)w->len,
                                        sink_msg, NULL);
            if (rc) return rc;
            off = w->len;
        }
        for (int i = 0; i < g_nmsgs; i++) {
            int rc = bt_mcp_session_rx(dst, g_msgs[i].kind, g_msgs[i].seq,
                                      g_msgs[i].buf, g_msgs[i].len,
                                      g_msgs[i].kind == BT_MCP_KIND_CTRL
                                          ? NULL : mcp_up, &g_step);
            last_rc = rc;
            if (rc) break;   /* rx 出错：判例=M3 session 同形（E_STATE/E_SYNC） */
        }
        g_nmsgs = 0;
    }
    bt_mcp_session_pump(dst);
    return last_rc;
}

/* 泵空两条线直到静止：c2s→server、s2c→client 交替，直到双线零字节
 * 或步数上限（防活锁）。server 侧上抛 MCP 消息须再进 server_rx（判例：
 * 0x08 TX 指令 → server 收到的是 MCP 请求，server_rx 产响应）。
 * 返回：0=静止；负=泵内部错 rc 上抛。 */
static int pump_all(bt_mcp_session_t *cs, bt_mcp_framer_t *cfr,
                    bt_mcp_server_t *sv, bt_mcp_session_t *ss,
                    bt_mcp_framer_t *sfr)
{
    for (int guard = 0; guard < 16; guard++) {
        if (e_c2s.len) {
            vec_t tmp = e_c2s; e_c2s.len = 0;
            /* server 侧：上抛 MCP 消息（kind=MCP）须转 server_rx；控制帧
               终结在 session（判例：判例 M3）。用 sink_msg 数组转投。 */
            size_t off = 0;
            int32_t last_rc = 0;
            while (off < tmp.len) {
                g_nmsgs = 0;
                if (sfr->cap->kind == BT_MCP_CHAN_PACKET) {
                    if (off + 2 > tmp.len) return BT_MCP_E_PARSE;
                    uint16_t flen = (uint16_t)(tmp.buf[off] | (tmp.buf[off + 1] << 8));
                    off += 2;
                    if (off + flen > tmp.len) return BT_MCP_E_PARSE;
                    int rc = bt_mcp_framer_feed(sfr, tmp.buf + off, flen,
                                                sink_msg, NULL);
                    off += flen;
                    if (rc) return rc;
                } else {
                    int rc = bt_mcp_framer_feed(sfr, tmp.buf, (uint16_t)tmp.len,
                                                sink_msg, NULL);
                    if (rc) return rc;
                    off = tmp.len;
                }
                for (int i = 0; i < g_nmsgs; i++) {
                    g_pump_sink_rc = 0;
                    last_rc = bt_mcp_session_rx(ss, g_msgs[i].kind,
                                                g_msgs[i].seq,
                                                g_msgs[i].buf, g_msgs[i].len,
                                                pump_srv_sink, sv);
                    if (g_pump_sink_rc) last_rc = g_pump_sink_rc;
                    if (last_rc) break;
                }
                g_nmsgs = 0;
                if (last_rc && last_rc != BT_MCP_OK) break;
            }
            bt_mcp_session_pump(ss);
            if (last_rc && last_rc != BT_MCP_OK) return last_rc;
            continue;
        }
        if (e_s2c.len) {
            int rc = pump_line(cs, cfr, &e_s2c);
            e_s2c.len = 0;
            if (rc) return rc;
            continue;
        }
        return 0;                       /* 静止 */
    }
    return BT_MCP_E_AGAIN;              /* 活锁护栏 */
}

static int run_e2e(const uint8_t *in, size_t in_len)
{
    if (in_len < 4) return 2;
    uint8_t chan = in[0];
    uint16_t mtu = (uint16_t)(in[1] | (in[2] << 8));
    uint8_t tools_count = in[3];

    static bt_mcp_tool_def_t tools[16];
    static char names[16][64], descs[16][128];
    if (tools_count > 16) return 2;
    size_t off = 4;
    for (uint8_t i = 0; i < tools_count; i++) {
        if (off + 2 > in_len) return 2;
        uint8_t nl = in[off++];
        if (nl > 63 || off + nl + 1 > in_len) return 2;
        memcpy(names[i], in + off, nl); names[i][nl] = 0; off += nl;
        uint8_t dl = in[off++];
        if (dl > 127 || off + dl > in_len) return 2;
        memcpy(descs[i], in + off, dl); descs[i][dl] = 0; off += dl;
        tools[i].name = names[i];
        tools[i].desc = descs[i];
        tools[i].input_schema_json =
            "{\"type\":\"object\",\"properties\":{}}";
        tools[i].authz = BT_MCP_AUTHZ_OPEN;
        tools[i].fn = tool_echo;
        tools[i].user = NULL;
    }
    /* 追加固定工具：授权路径可被差分覆盖（名字固定，Host 可见） */
    if (tools_count + 2 <= 16) {
        bt_mcp_tool_def_t *ct = &tools[tools_count++];
        ct->name = "consent_tool";
        ct->desc = "needs consent";
        ct->title = "Consent tool";
        ct->input_schema_json = "{\"type\":\"object\",\"properties\":{}}";
        ct->output_schema_json = "{\"type\":\"object\"}";
        ct->annotations = BT_MCP_TOOL_READONLY | BT_MCP_TOOL_IDEMPOTENT;
        ct->authz = BT_MCP_AUTHZ_CONSENT;
        ct->fn = tool_allow;
        ct->user = NULL;

        bt_mcp_tool_def_t *rt = &tools[tools_count++];
        rt->name = "restricted_tool";
        rt->desc = "always denied";
        rt->title = NULL;
        rt->input_schema_json = "{\"type\":\"object\",\"properties\":{}}";
        rt->output_schema_json = NULL;
        rt->annotations = BT_MCP_TOOL_DESTRUCTIVE;
        rt->authz = BT_MCP_AUTHZ_RESTRICTED;
        rt->fn = tool_allow;
        rt->user = NULL;
    }
    if (!g_rich_tool_present) {
        /* 追加一个富内容工具：验证 text/image/audio/resource 混排 */
        if (tools_count + 1 <= 16) {
            bt_mcp_tool_def_t *rt = &tools[tools_count++];
            rt->name = "rich_tool";
            rt->desc = "multi content";
            rt->title = NULL;
            rt->input_schema_json = "{\"type\":\"object\",\"properties\":{}}";
            rt->output_schema_json = NULL;
            rt->annotations = 0;
            rt->authz = BT_MCP_AUTHZ_OPEN;
            rt->fn = tool_rich;
            rt->user = NULL;
        }
        g_rich_tool_present = 1;
    }

    /* client 侧（L4 client 角色）+ server 侧（L4 server 角色 + L5） */
    static bt_mcp_framer_t cfr, sfr;
    static bt_mcp_session_t cs, ss;
    static bt_mcp_server_t sv;
    bt_mcp_chan_cap_t cap = { .kind = chan ? BT_MCP_CHAN_PACKET
                                          : BT_MCP_CHAN_STREAM,
                              .mtu = mtu,
                              .reliable = chan ? 0 : 1,   /* GATT 包通道不可靠 */
                              .flow_ctrl = chan ? 0 : 1,
                              .max_pending = 4 };
    bt_mcp_frame_cfg_t fcfg = { .max_reasm = 4, .frag_tout_ms = 2000 };
    if (bt_mcp_framer_init(&cfr, &fcfg, &cap)) return 2;
    if (bt_mcp_framer_init(&sfr, &fcfg, &cap)) return 2;
    bt_mcp_session_cfg_t ccfg = { .req_timeout_ms = 30000, .resume_tout_ms = 5000,
                                  .tx_window = 4, .role = BT_MCP_ROLE_CLIENT };
    bt_mcp_session_cfg_t scfg = { .req_timeout_ms = 30000, .resume_tout_ms = 5000,
                                  .tx_window = 4, .role = BT_MCP_ROLE_SERVER };
    if (bt_mcp_session_init(&cs, &ccfg)) return 2;
    if (bt_mcp_session_init(&ss, &scfg)) return 2;
    /* client emit → c2s 线；server emit → s2c 线 */
    bt_mcp_session_bind(&cs, &cfr, vec_emit, &e_c2s);
    bt_mcp_session_bind(&ss, &sfr, vec_emit, &e_s2c);
    bt_mcp_server_cfg_t sv_cfg = { .tools = tools, .tools_count = tools_count,
                                  .resources = MOCK_RES,
                                  .resources_count = MOCK_RES_COUNT,
                                  .prompts = MOCK_PROMPTS,
                                  .prompts_count = MOCK_PROMPT_COUNT,
                                  .consent = consent_cb };
    if (bt_mcp_server_init(&sv, &ss, &sv_cfg, "bt-mcp-device", "0.4")) return 2;

    g_pump_sv = &sv;
    memset(&e_c2s, 0, sizeof e_c2s);
    memset(&e_s2c, 0, sizeof e_s2c);
    int32_t rc = 0;
    while (off < in_len) {
        memset(&g_step, 0, sizeof g_step);
        uint8_t op = in[off++];
        switch (op) {
        case 0x08: {                      /* CMSG：client session_tx */
            if (off + 2 > in_len) return 2;
            uint16_t l = (uint16_t)(in[off] | (in[off + 1] << 8)); off += 2;
            if ((size_t)off + l > in_len) return 2;
            rc = bt_mcp_session_tx(&cs, in + off, l);
            off += l;
            {
                int prc = pump_all(&cs, &cfr, &sv, &ss, &sfr);
                if (prc) rc = prc;
            }
            break;
        }
        case 0x09: {                      /* SMSG：server_rx 直投 */
            if (off + 2 > in_len) return 2;
            uint16_t l = (uint16_t)(in[off] | (in[off + 1] << 8)); off += 2;
            if ((size_t)off + l > in_len) return 2;
            rc = bt_mcp_server_rx(&sv, in + off, l);
            off += l;
            {
                int prc = pump_all(&cs, &cfr, &sv, &ss, &sfr);
                if (prc) rc = prc;
            }
            break;
        }
        case 0x0A: {                      /* LUP：双方 link_up */
            if (off + 4 > in_len) return 2;
            uint32_t tok = rd_u32(in + off); off += 4;
            bt_mcp_session_link_up(&cs, 0);
            bt_mcp_session_link_up(&ss, tok);
            rc = 0;
            break;
        }
        case 0x0B: {                      /* LDN：双方 link_down */
            if (off >= in_len) return 2;
            int bond = in[off++];
            bt_mcp_session_link_down(&cs, bond);
            bt_mcp_session_link_down(&ss, bond);
            rc = 0;
            break;
        }
        case 0x0C: {                      /* TICK：双方 tick */
            if (off + 4 > in_len) return 2;
            uint32_t now = rd_u32(in + off); off += 4;
            bt_mcp_session_tick(&cs, now);
            bt_mcp_session_tick(&ss, now);
            rc = 0;
            break;
        }
        default:
            return 2;
        }
        /* 输出记录：i32 rc | u32 nmsgs | [i32 rx_rc | u32 len + bytes]*
         * g_step.nmsgs 由 mcp_up 在泵空 client 侧时累积（0x09 泵路径）；
         * 其余指令归零。 */
        g_step.rc = rc;
        wr_step(&g_step);
        g_step.wire.len = 0;
    }
    return 0;
}
