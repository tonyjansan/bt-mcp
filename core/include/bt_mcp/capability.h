/* bt_mcp/capability.h —— core 对通道的全部假设仅此一个结构体（草案 §3） */
#ifndef BT_MCP_CAPABILITY_H
#define BT_MCP_CAPABILITY_H

typedef enum {
    BT_MCP_CHAN_PACKET = 0, /* 包语义：有 MTU、可能丢、无跨包边界（GATT、CoC） */
    BT_MCP_CHAN_STREAM = 1  /* 流语义：字节流有序可靠（SPP、RFCOMM、UART） */
} bt_mcp_chan_kind_t;

typedef struct bt_mcp_chan_cap {
    bt_mcp_chan_kind_t kind;
    uint16_t mtu;             /* 单次 send() payload 上限；流通道填 0 = 无限 */
    uint8_t  reliable   : 1;  /* 1 = 送达有序可靠 */
    uint8_t  flow_ctrl  : 1;  /* 1 = 通道自带流控，L4 免信用 */
    uint8_t  max_pending : 4; /* 建议同时在途消息数（= 对端重组槽数） */
} bt_mcp_chan_cap_t;

#endif
