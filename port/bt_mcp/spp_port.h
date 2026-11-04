/* bt_mcp/spp_port.h —— M4c：SPP/RFCOMM（流语义）通道适配器。
 * core 零改动：framer emit 回调直通 port_send；feed 由 SPP socket
 * recv 到达驱动（流通道任意字节段）。流通道无 MTU 上限（cap.mtu=0）、
 * 无丢序，环形缓冲模拟 SPP 线路缓冲。 */
#ifndef BT_MCP_SPP_PORT_H
#define BT_MCP_SPP_PORT_H

#include "bt_mcp/frame.h"
#include "bt_mcp/err.h"
#include <string.h>

typedef struct {
    bt_mcp_chan_cap_t cap;             /* kind=STREAM，mtu=0 无限 */
    uint8_t  ring[BT_MCP_CFG_MAX_MSG]; /* 线路缓冲底（字节流） */
    uint32_t head, tail;                /* 单调递增，模容量 */
    uint32_t drop;                      /* 溢出丢弃（= 线路缓冲满后丢段） */
} bt_mcp_spp_port_t;

static inline int bt_mcp_spp_port_init(bt_mcp_spp_port_t *p)
{
    memset(p, 0, sizeof *p);
    p->cap.kind = BT_MCP_CHAN_STREAM;
    p->cap.mtu = 0;                   /* 流通道：无 MTU 上限（判例 §3） */
    p->cap.reliable = 1;
    p->cap.flow_ctrl = 0;
    p->cap.max_pending = 4;
    return BT_MCP_OK;
}

#define BT_MCP_SPP_RING_CAP (BT_MCP_CFG_MAX_MSG)

static inline uint32_t bt_mcp_spp_port_used(const bt_mcp_spp_port_t *p)
{
    return p->head - p->tail;
}

/* framer emit 回调：帧字节流进线路缓冲；满则计数 drop 并报 E_NOMEM
 * （等价 SPP 线路缓冲满，write 返回 EAGAIN 的 mock 简化语义）。 */
static inline int bt_mcp_spp_emit(void *io, const uint8_t *data, uint16_t len)
{
    bt_mcp_spp_port_t *p = io;
    if (bt_mcp_spp_port_used(p) + len > BT_MCP_SPP_RING_CAP) {
        p->drop++;
        return BT_MCP_E_NOMEM;
    }
    for (uint16_t i = 0; i < len; i++)
        p->ring[(p->head + i) % BT_MCP_SPP_RING_CAP] = data[i];
    p->head += len;
    return BT_MCP_OK;
}

/* 模拟 SPP socket recv：一次读 len 字节到 out，供对端 framer_feed。
 * 返回实际读出数（≤len）。 */
static inline uint32_t bt_mcp_spp_drain(bt_mcp_spp_port_t *p,
                                        uint8_t *out, uint32_t len)
{
    uint32_t used = bt_mcp_spp_port_used(p);
    if (len > used) len = used;
    for (uint32_t i = 0; i < len; i++)
        out[i] = p->ring[(p->tail + i) % BT_MCP_SPP_RING_CAP];
    p->tail += len;
    return len;
}

#endif
