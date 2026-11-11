/* bt_mcp/coc_port.h —— L2CAP LE Credit Based Connection（CoC）通道适配器。
 *
 * CoC 语义（草案 §3 表）：PACKET + reliable=1 + flow_ctrl=1
 *  - L2CAP 自带 FCS 与重传 → 不需要 L3 的 NACK/重传；
 *  - LE credit 机制自带流控 → 不需要 L4 的应用层信用窗口；
 *  - 但 CoC MTU 仍可能小于整条 MCP 消息 → 保留 L3 分片/重组。
 * 所以 CoC 是「只花分片成本」的通道：core 零改动，只有 cap 取值不同。
 *
 * 与 gatt_port 的差异只在这三个 bit，正是「能力描述符收敛」的实证。 */
#ifndef BT_MCP_COC_PORT_H
#define BT_MCP_COC_PORT_H

#include "bt_mcp/frame.h"
#include "bt_mcp/err.h"
#include <string.h>

typedef struct {
    bt_mcp_chan_cap_t cap;               /* kind=PACKET, reliable=1, flow=1 */
    uint8_t  ring[BT_MCP_CFG_MAX_MSG];   /* 发送 SDU 队列底 */
    uint32_t head, tail;
    uint32_t drop;
    uint16_t credits;                    /* 对端授予的 LE credits（模拟） */
} bt_mcp_coc_port_t;

#define BT_MCP_COC_RING_CAP (BT_MCP_CFG_MAX_MSG)

/* coc_mtu：对端协商的 L2CAP CoC MTU（SDU 上限，非 ATT_MTU）。
 * 典型 512B（默认）~ 64KB（高吞吐）。 */
static inline int bt_mcp_coc_port_init(bt_mcp_coc_port_t *p, uint16_t coc_mtu)
{
    memset(p, 0, sizeof *p);
    p->cap.kind = BT_MCP_CHAN_PACKET;
    p->cap.mtu = coc_mtu;
    p->cap.reliable = 1;      /* L2CAP 层可靠 */
    p->cap.flow_ctrl = 1;     /* LE credit 自带流控 */
    p->cap.max_pending = 4;
    p->credits = 4;
    return coc_mtu <= BT_MCP_HDR_LEN ? BT_MCP_E_ARGS : BT_MCP_OK;
}

static inline uint32_t bt_mcp_coc_port_used(const bt_mcp_coc_port_t *p)
{
    return p->head - p->tail;
}

/* framer emit 回调：一个分片 = 一个 SDU；队列满计入 drop 并背压 */
static inline int bt_mcp_coc_emit(void *io, const uint8_t *data, uint16_t len)
{
    bt_mcp_coc_port_t *p = io;
    if (bt_mcp_coc_port_used(p) + len > BT_MCP_COC_RING_CAP) {
        p->drop++;
        return BT_MCP_E_NOMEM;
    }
    for (uint16_t i = 0; i < len; i++)
        p->ring[(p->head + i) % BT_MCP_COC_RING_CAP] = data[i];
    p->head += len;
    return BT_MCP_OK;
}

/* 模拟对端取走 SDU（一次 drain 一个分片边界内的字节） */
static inline uint32_t bt_mcp_coc_drain(bt_mcp_coc_port_t *p,
                                        uint8_t *out, uint32_t len)
{
    uint32_t used = bt_mcp_coc_port_used(p);
    if (len > used) len = used;
    for (uint32_t i = 0; i < len; i++)
        out[i] = p->ring[(p->tail + i) % BT_MCP_COC_RING_CAP];
    p->tail += len;
    return len;
}

#endif
