/* bt_mcp/gatt_port.h —— M4c：GATT（BTLE 包语义）通道适配器。
 * core 零改动：framer 的 emit 回调直通 port_send，feed 由
 * GATT 写特性值分片到达驱动。port 拥有一个深度受限的发送 FIFO，
 * ring buffer 模拟 GATT notify 队列（EIO=写入通知挂起时的 ENOMEM 语义）。 */
#ifndef BT_MCP_GATT_PORT_H
#define BT_MCP_GATT_PORT_H

#include "bt_mcp/frame.h"
#include "bt_mcp/err.h"
#include <string.h>

typedef struct {
    bt_mcp_chan_cap_t cap;         /* kind=PACKET，mtu=对端 MTU-3 */
    uint8_t  ring[BT_MCP_CFG_MAX_MSG]; /* 发送 FIFO 底（分片片字节） */
    uint32_t head, tail;           /* 单调递增，模容量 */
    uint32_t drop;                 /* 溢出丢弃计数（=GATT buffer pool 耗尽） */
} bt_mcp_gatt_port_t;

static inline int bt_mcp_gatt_port_init(bt_mcp_gatt_port_t *p, uint16_t att_mtu)
{
    memset(p, 0, sizeof *p);
    p->cap.kind = BT_MCP_CHAN_PACKET;
    p->cap.mtu = (uint16_t)(att_mtu - 3);   /* ATT 头 3B 预扣（GATT 判例） */
    p->cap.reliable = 1;
    p->cap.flow_ctrl = 0;
    p->cap.max_pending = 4;
    return p->cap.mtu < 20 ? BT_MCP_E_ARGS : BT_MCP_OK;
}

/* ring 容量查询：单调 head/tail 判满判空不取模（判例：无绕回歧义） */
static inline uint32_t bt_mcp_gatt_port_used(const bt_mcp_gatt_port_t *p)
{
    return p->head - p->tail;
}
#define BT_MCP_GATT_RING_CAP (BT_MCP_CFG_MAX_MSG)

/* framer emit 回调：片字节进 ring FIFO；满则计入 drop 并报 E_NOMEM
 * （framer_send 透传该 rc，session_tx 上抛——背压语义与真实 GATT 一致）。 */
static inline int bt_mcp_gatt_emit(void *io, const uint8_t *data, uint16_t len)
{
    bt_mcp_gatt_port_t *p = io;
    if (bt_mcp_gatt_port_used(p) + len > BT_MCP_GATT_RING_CAP) {
        p->drop++;
        return BT_MCP_E_NOMEM;
    }
    for (uint16_t i = 0; i < len; i++)
        p->ring[(p->head + i) % BT_MCP_GATT_RING_CAP] = data[i];
    p->head += len;
    return BT_MCP_OK;
}

/* 模拟 GATT subsystem 送达：FIFO 一次冲出 len 字节到 out（=central 已收），
 * 供对端 framer_feed。返回实际冲出字节数（≤len）。 */
static inline uint32_t bt_mcp_gatt_drain(bt_mcp_gatt_port_t *p,
                                         uint8_t *out, uint32_t len)
{
    uint32_t used = bt_mcp_gatt_port_used(p);
    if (len > used) len = used;
    for (uint32_t i = 0; i < len; i++)
        out[i] = p->ring[(p->tail + i) % BT_MCP_GATT_RING_CAP];
    p->tail += len;
    return len;
}

#endif
