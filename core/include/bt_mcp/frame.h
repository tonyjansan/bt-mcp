/* bt_mcp/frame.h —— L3 分帧/重组（草案 §4/§7）。纯协议、无 OS 依赖。
 *
 * v0.3 新增：
 *  - msg_sink 携带本消息的帧头 seq（L4 用它做 ACK/去重）；
 *  - 缺片保持重组槽存活并登记 NACK，等对端单片重传补齐（不再弃整消息）；
 *  - send_seq / send_idx 支持「原序号重传」，配合 L4 重传槽实现丢片恢复。
 */
#ifndef BT_MCP_FRAME_H
#define BT_MCP_FRAME_H

#include <stdint.h>
#include <stddef.h>
#include "capability.h"
#include "config.h"

/* 帧头（草案 §4.1，小端 6B）：
   0 magic=0xB7 | 1 flags | 2 seq | 3 idx | 4..5 len(LE u16)
   flags: bit0 FRAG_LAST, bit1 CHUNKED, bit2 CTRL */
#define BT_MCP_MAGIC      0xB7
#define BT_MCP_HDR_LEN    6
#define BT_MCP_FLAG_LAST  0x01
#define BT_MCP_FLAG_CHUNK 0x02
#define BT_MCP_FLAG_CTRL  0x04
#define BT_MCP_MAX_CHUNK_PAYLOAD 255

/* 上抛消息 kind（msg_sink 第二参） */
#define BT_MCP_KIND_MCP  0
#define BT_MCP_KIND_CTRL 1

/* 控制帧类型（草案 §4.2） */
#define BT_MCP_CTRL_CREDIT     0x01 /* u16 credits */
#define BT_MCP_CTRL_PING       0x02 /* u8 epoch */
#define BT_MCP_CTRL_RESUME_REQ 0x03 /* u32 token */
#define BT_MCP_CTRL_RESUME_OK  0x04 /* u32 token */
#define BT_MCP_CTRL_RESUME_REJ 0x05 /* u8 reason */
#define BT_MCP_CTRL_ACK        0x06 /* u8 seq         —— 消息级确认 */
#define BT_MCP_CTRL_NACK       0x07 /* u8 seq|u8 idx  —— 请求重传缺失片 */

/* 控制帧 payload 上限（控制帧恒单片） */
#define BT_MCP_CTRL_MAX_PAYLOAD 255

/* 上抛回调：kind=BT_MCP_KIND_*；seq=该消息的帧头序号（控制帧/流通道同样带） */
typedef void (*bt_mcp_msg_sink_t)(void *user, uint8_t kind, uint8_t seq,
                                  const uint8_t *payload, uint16_t len);

typedef struct {
    uint8_t  max_reasm;      /* 重组槽数（包通道）；默认 4 */
    uint16_t frag_tout_ms;   /* 重组超时，0 = 用 BT_MCP_CFG_FRAG_TOUT_MS */
} bt_mcp_frame_cfg_t;

/* v0.9.1（A2 根因修复）：**乱序缓存 + 缺片位图**。
 *
 * v0.8 的实现是严格按序：idx 跳号就丢弃该片、只 NACK 一个 next_idx，
 * 于是发送端只能「从缺片补到末尾」——把已经到达的片又发一遍。
 * 实测 60 条消息产生了 9045 次重传（约 19 倍带宽放大）。
 *
 * 现在改为：乱序片按 `idx * chunk` 落位并置位图；只有真正缺的片会被 NACK。
 * 所有非末尾片的长度都等于 chunk，所以偏移可以直接算，不必记录每片长度。
 */
struct bt_mcp_reasm_slot {
    uint8_t  in_use;
    uint8_t  ctrl;         /* 1 = 控制帧消息 */
    uint8_t  msg_seq;      /* 消息键：同消息全部分片共享的帧头 seq */
    uint8_t  next_idx;     /* 兼容字段：当前最小的未收到分片号（供旧路径阅读） */
    uint8_t  gap_wait;     /* 缺片等待次数（防对端彻底丢帧后槽位长期占用） */
    uint16_t chunk;        /* 本槽当前的分片载荷大小（偏移计算用） */
    uint16_t len_total;    /* 已收到字节数累计 */
    uint16_t last_len;     /* LAST 片的载荷长度（0 = 尚未收到 LAST） */
    uint16_t nack_len;     /* 上次发 NACK 时的 len_total（判定「有无进展」） */
    uint8_t  last_idx;     /* LAST 片的下标 */
    uint8_t  have_last;    /* 1 = 已收到 LAST */
    uint32_t t_first;      /* 槽位开通时刻（超时回收基准） */
    uint32_t t_last;
    /* 位图：bit i = 已收到分片 i（最多 BT_MCP_CFG_MAX_FRAG=255 片 → 32B） */
    uint8_t  got[(BT_MCP_CFG_MAX_FRAG + 7) / 8];
    uint8_t  buf[BT_MCP_CFG_MAX_MSG];
};

struct bt_mcp_framer {
    const bt_mcp_chan_cap_t *cap;
    uint16_t                 mtu_override;  /* 0=用 cap->mtu；链路 MTU 热更新用 */
    bt_mcp_frame_cfg_t       cfg;
    uint8_t                  tx_seq;      /* 发送序号 mod 256 */
    uint32_t                 now_ms;      /* 最近一次 tick 的时间戳缓存 */
    /* 待发 NACK（缺片登记；L4 经 take_nack 取走后发控制帧） */
    uint8_t  nack_pending;
    uint8_t  nack_seq;
    uint8_t  nack_idx[BT_MCP_CFG_NACK_MAX_IDX];  /* v0.9.1：一次报多个缺片 */
    uint8_t  nack_count;
    struct bt_mcp_reasm_slot reasm[BT_MCP_CFG_MAX_REASM];
    /* v0.9.1：刚被放弃的消息 seq。同一 seq 的后续分片一律忽略，
     * 直到出现别的 seq —— 否则「对端反复重发同一片」会让同一条消息
     * 无限「放弃→重建」，既浪费又永远释放不掉槽。 */
    uint8_t  dead_seq;
    uint8_t  dead_valid;
    /* 流通道解析器状态 */
    uint8_t  s_hdr[BT_MCP_HDR_LEN];
    uint8_t  s_hdr_got;
    uint16_t s_len_got;
    uint8_t  s_fcs[2];                 /* FCS16：紧跟帧头的 2 字节校验（流通道状态） */
    uint8_t  s_fcs_got;
    uint8_t  s_cur[BT_MCP_CFG_MAX_MSG]; /* 仅流通道用于非 CHUNKED 整帧缓存 */
    int      s_active;                 /* 流通道当前帧接收中 */
};

typedef struct bt_mcp_framer bt_mcp_framer_t;

int  bt_mcp_framer_init(bt_mcp_framer_t *f, const bt_mcp_frame_cfg_t *cfg,
                        const bt_mcp_chan_cap_t *cap);
/* 取走一个发送序号（L4 需先知道 seq 才能登记重传槽） */
uint8_t bt_mcp_framer_alloc_seq(bt_mcp_framer_t *f);
/* 一条 len 字节的消息在当前 MTU 下会被切成几片（NACK 尾部重放需要知道） */
uint8_t bt_mcp_framer_frag_count(const bt_mcp_framer_t *f, uint32_t len);
/* 发送：整条消息 → emit 按片吐出（帧头+payload，FCS 可选）。
   kind: BT_MCP_KIND_MCP / BT_MCP_KIND_CTRL。返回 0 或负错误码。 */
int  bt_mcp_framer_send(bt_mcp_framer_t *f, uint8_t kind,
                        const uint8_t *payload, uint32_t len,
                        int (*emit)(void *, const uint8_t *, uint16_t),
                        void *io);
/* 以指定 seq 重发整条消息（L4 重传槽用；不推进 tx_seq） */
int  bt_mcp_framer_send_seq(bt_mcp_framer_t *f, uint8_t kind, uint8_t seq,
                            const uint8_t *payload, uint32_t len,
                            int (*emit)(void *, const uint8_t *, uint16_t),
                            void *io);
/* 以指定 seq 只重发第 idx 片（NACK 单片补齐；不推进 tx_seq） */
int  bt_mcp_framer_send_idx(bt_mcp_framer_t *f, uint8_t kind, uint8_t seq,
                            uint8_t idx, const uint8_t *payload, uint32_t len,
                            int (*emit)(void *, const uint8_t *, uint16_t),
                            void *io);
/* 接收：喂原始字节（包通道喂完整片；流通道喂任意字节段），重组完成经 sink 上抛 */
int  bt_mcp_framer_feed(bt_mcp_framer_t *f, const uint8_t *in, uint16_t len,
                        bt_mcp_msg_sink_t sink, void *user);
/* 取走待发 NACK 登记：返回缺片个数（0 = 无）。
 * v0.9.1：一次可报多个缺片（分片级 SACK），idx 数组由调用方提供、容量 ≥
 * BT_MCP_CFG_NACK_MAX_IDX。旧的「单缺片」语义等价于 count==1。 */
int  bt_mcp_framer_take_nack(bt_mcp_framer_t *f, uint8_t *seq,
                             uint8_t *idx, uint8_t max, uint8_t *count_out);
void bt_mcp_framer_tick(bt_mcp_framer_t *f, uint32_t now_ms);
/* 丢弃在途重组状态（断链时调用，防跨连接的半帧串味） */
/* 链路 MTU 变化（BLE 连接后 MTU 交换、CoC 重配）：立即生效于后续分片。
 * mtu=0 表示回退到 cap->mtu；已发出的帧不受影响（接收侧按其头部 len 解析）。 */
int  bt_mcp_framer_set_mtu(bt_mcp_framer_t *f, uint16_t mtu);

void bt_mcp_framer_reset(bt_mcp_framer_t *f);

#endif
