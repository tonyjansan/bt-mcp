/* M4c HIL：port 层 ring buffer transport——gatt_port / spp_port 适配器
 * 驱动 core L3+L4 背靠背全链路（core 零改动验证）。
 * 编：cc -O2 -std=c99 -Wall -Wextra -Werror -Icore/include -Iport \
 *      tests/test_m4c_port.c core/src/frame.c core/src/session.c \
 *      -o build/test_m4c_port
 * 断言全过输出 ALL GREEN + 检查数。 */
#include "bt_mcp/gatt_port.h"
#include "bt_mcp/spp_port.h"
#include "bt_mcp/session.h"
#include <stdio.h>
#include <string.h>

#define CHECK(cond) do { \
    if (!(cond)) { printf("FAIL line %d: %s\n", __LINE__, #cond); return 1; } \
    checks++; \
} while (0)

/* 片旁路记录：framer 每片 emit 两次（hdr=HDR_LEN、payload），而 GATT
 * notify = hdr+payload 一体到达。聚合：hdr emit 开新 notify 槽，payload
 * 追加同槽；槽长入 frag_log，包通道按槽直通对端 feed。 */
static uint8_t  frag_buf[64][BT_MCP_HDR_LEN + 256];
static uint16_t frag_log[64];
static int      frag_n;

static int emit_log_gatt(void *io, const uint8_t *d, uint16_t n)
{
    if (n == BT_MCP_HDR_LEN) {              /* 新片 hdr：开新槽 */
        if (frag_n < 64) {
            memcpy(frag_buf[frag_n], d, n);
            frag_log[frag_n] = n;
        }
        frag_n++;
    } else if (frag_n > 0 && frag_n <= 64) {  /* payload 追加当前槽 */
        if (frag_log[frag_n - 1] + n <= BT_MCP_HDR_LEN + 256) {
            memcpy(frag_buf[frag_n - 1] + frag_log[frag_n - 1], d, n);
            frag_log[frag_n - 1] += n;
        }
    }
    return bt_mcp_gatt_emit(io, d, n);
}

static int msg_count;
static void count_sink(void *u, uint8_t kind, uint8_t seq,
                       const uint8_t *b, uint16_t l)
{
    (void)u; (void)kind; (void)seq; (void)b; (void)l;
    msg_count++;
}

struct endpoint {
    bt_mcp_framer_t  fr;
    bt_mcp_session_t sess;
    bt_mcp_gatt_port_t gp;
    bt_mcp_spp_port_t sp;
    int is_gatt;
};

static int ep_init(struct endpoint *e, int is_gatt, uint16_t att_mtu,
                   int role)
{
    memset(e, 0, sizeof *e);
    e->is_gatt = is_gatt;
    bt_mcp_frame_cfg_t fcfg = { .max_reasm = 4, .frag_tout_ms = 2000 };
    bt_mcp_session_cfg_t scfg = { .req_timeout_ms = 30000, .resume_tout_ms = 5000,
                                  .tx_window = 4, .role = (uint8_t)role };
    int rc;
    if (is_gatt) {
        if ((rc = bt_mcp_gatt_port_init(&e->gp, att_mtu)) != 0) return rc;
        if ((rc = bt_mcp_framer_init(&e->fr, &fcfg, &e->gp.cap)) != 0) return rc;
        if (bt_mcp_session_init(&e->sess, &scfg)) return -100;
        bt_mcp_session_bind(&e->sess, &e->fr, bt_mcp_gatt_emit, &e->gp);
    } else {
        if ((rc = bt_mcp_spp_port_init(&e->sp)) != 0) return rc;
        if ((rc = bt_mcp_framer_init(&e->fr, &fcfg, &e->sp.cap)) != 0) return rc;
        if (bt_mcp_session_init(&e->sess, &scfg)) return -101;
        bt_mcp_session_bind(&e->sess, &e->fr, bt_mcp_spp_emit, &e->sp);
    }
    return 0;
}

static const char *INIT =
    "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"initialize\",\"params\":{}}";

int main(void)
{
    int checks = 0;
    uint8_t tmp[BT_MCP_CFG_MAX_MSG];

    /* ---- GATT port 基础：init 参数门 + ring FIFO 语义 ---- */
    {
        bt_mcp_gatt_port_t p;
        CHECK(bt_mcp_gatt_port_init(&p, 20) == BT_MCP_E_ARGS); /* mtu-3 < 20 */
        CHECK(bt_mcp_gatt_port_init(&p, 247) == BT_MCP_OK);
        CHECK(p.cap.mtu == 244);
        CHECK(p.cap.kind == BT_MCP_CHAN_PACKET);
        uint8_t d[8] = {1,2,3,4,5,6,7,8};
        CHECK(bt_mcp_gatt_emit(&p, d, 8) == BT_MCP_OK);
        CHECK(bt_mcp_gatt_port_used(&p) == 8);
        uint8_t out[8];
        CHECK(bt_mcp_gatt_drain(&p, out, 100) == 8);
        CHECK(memcmp(out, d, 8) == 0);
        CHECK(bt_mcp_gatt_port_used(&p) == 0);
        /* 溢出路径：8192 池灌满后 E_NOMEM + drop 计数 */
        memset(tmp, 0xAB, 8192);
        CHECK(bt_mcp_gatt_emit(&p, tmp, 8192) == BT_MCP_OK);
        CHECK(bt_mcp_gatt_emit(&p, d, 1) == BT_MCP_E_NOMEM);
        CHECK(p.drop == 1);
        CHECK(bt_mcp_gatt_drain(&p, tmp, 8192) == 8192);
        CHECK(bt_mcp_gatt_port_used(&p) == 0);
    }

    /* ---- SPP port 基础 ---- */
    {
        bt_mcp_spp_port_t p;
        CHECK(bt_mcp_spp_port_init(&p) == BT_MCP_OK);
        CHECK(p.cap.kind == BT_MCP_CHAN_STREAM);
        CHECK(p.cap.mtu == 0);
        uint8_t d[4] = {9,8,7,6};
        CHECK(bt_mcp_spp_emit(&p, d, 4) == BT_MCP_OK);
        uint8_t out[4];
        CHECK(bt_mcp_spp_drain(&p, out, 4) == 4);
        CHECK(memcmp(out, d, 4) == 0);
        CHECK(bt_mcp_spp_emit(&p, d, 0) == BT_MCP_OK);
        CHECK(bt_mcp_spp_port_used(&p) == 0);
    }

    /* ---- GATT 全链路 HIL：client tx → 逐片直通 → server 重组 ---- */
    {
        struct endpoint c, s;
        CHECK(ep_init(&c, 1, 247, BT_MCP_ROLE_CLIENT) == 0);
        CHECK(ep_init(&s, 1, 247, BT_MCP_ROLE_SERVER) == 0);
        bt_mcp_session_link_up(&c.sess, 0);
        bt_mcp_session_link_up(&s.sess, 1);
        /* tx 前换上片旁路 emit（包通道按片直通对端须片边界） */
        bt_mcp_session_bind(&c.sess, &c.fr, emit_log_gatt, &c.gp);
        frag_n = 0;
        CHECK(bt_mcp_session_tx(&c.sess, (const uint8_t *)INIT,
                                (uint32_t)strlen(INIT)) == 0);
        CHECK(frag_n == 1);                       /* 84B 单片（mtu=244） */
        CHECK(bt_mcp_gatt_port_used(&c.gp) > 0);
        msg_count = 0;
        for (int i = 0; i < frag_n; i++) {
            uint32_t got = bt_mcp_gatt_drain(&c.gp, tmp, frag_log[i]);
            CHECK(got == frag_log[i]);
            CHECK(bt_mcp_framer_feed(&s.fr, tmp, (uint16_t)got,
                                     count_sink, NULL) == 0);
        }
        CHECK(msg_count == 1);
        CHECK(bt_mcp_gatt_port_used(&c.gp) == 0);
    }

    /* ---- GATT 分片链路：mtu=23 长消息跨片 + framer 重组 ---- */
    {
        struct endpoint c, s;
        CHECK(ep_init(&c, 1, 26, BT_MCP_ROLE_CLIENT) == 0);   /* mtu=23 */
        CHECK(ep_init(&s, 1, 26, BT_MCP_ROLE_SERVER) == 0);
        bt_mcp_session_link_up(&c.sess, 0);
        bt_mcp_session_link_up(&s.sess, 1);
        bt_mcp_session_bind(&c.sess, &c.fr, emit_log_gatt, &c.gp);
        frag_n = 0;
        CHECK(bt_mcp_session_tx(&c.sess, (const uint8_t *)INIT,
                                (uint32_t)strlen(INIT)) == 0);
        CHECK(frag_n >= 4);                       /* 84B/17B 片 ≥ 4 notify */
        msg_count = 0;
        for (int i = 0; i < frag_n; i++) {
            uint32_t got = bt_mcp_gatt_drain(&c.gp, tmp, frag_log[i]);
            CHECK(got == frag_log[i]);
            CHECK(bt_mcp_framer_feed(&s.fr, tmp, (uint16_t)got,
                                     count_sink, NULL) == 0);
        }
        CHECK(msg_count == 1);                    /* 末片触发重组 */
        CHECK(bt_mcp_gatt_port_used(&c.gp) == 0);
    }

    /* ---- SPP 全链路 HIL：流通道任意字节段喂入（1B~7B 混合切分） ---- */
    {
        struct endpoint c, s;
        CHECK(ep_init(&c, 0, 0, BT_MCP_ROLE_CLIENT) == 0);
        CHECK(ep_init(&s, 0, 0, BT_MCP_ROLE_SERVER) == 0);
        bt_mcp_session_link_up(&c.sess, 0);
        bt_mcp_session_link_up(&s.sess, 1);
        CHECK(bt_mcp_session_tx(&c.sess, (const uint8_t *)INIT,
                                (uint32_t)strlen(INIT)) == 0);
        CHECK(bt_mcp_spp_port_used(&c.sp) > 0);
        msg_count = 0;
        uint32_t total = bt_mcp_spp_port_used(&c.sp);
        uint32_t fed = 0;
        while (fed < total) {
            uint32_t chunk = (total - fed) < 7 ? (total - fed) : 7;
            uint32_t got = bt_mcp_spp_drain(&c.sp, tmp, chunk);
            CHECK(bt_mcp_framer_feed(&s.fr, tmp, (uint16_t)got,
                                     count_sink, NULL) == 0);
            fed += got;
        }
        CHECK(msg_count == 1);
        CHECK(bt_mcp_spp_port_used(&c.sp) == 0);
    }

    /* ---- SPP ring 回绕链路：跨回绕 drain 不丢字节 ---- */
    {
        bt_mcp_spp_port_t p;
        CHECK(bt_mcp_spp_port_init(&p) == 0);
        uint8_t pat[300], out[300];
        for (int i = 0; i < 300; i++) pat[i] = (uint8_t)(i * 7);
        for (int round = 0; round < 40; round++) {   /* head 单调推进过回绕点 */
            CHECK(bt_mcp_spp_emit(&p, pat, 300) == BT_MCP_OK);
            CHECK(bt_mcp_spp_drain(&p, out, 300) == 300);
            CHECK(memcmp(out, pat, 300) == 0);
        }
        CHECK(bt_mcp_spp_port_used(&p) == 0);
        CHECK(p.drop == 0);
    }

    printf("ALL GREEN: %d checks\n", checks);
    return 0;
}
