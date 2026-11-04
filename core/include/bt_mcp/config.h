/* bt_mcp/config.h —— 编译期配置集中地，无 vendor ifdef（草案 §7.2） */
#ifndef BT_MCP_CONFIG_H
#define BT_MCP_CONFIG_H

#ifndef BT_MCP_CFG_MAX_REASM
#define BT_MCP_CFG_MAX_REASM     4      /* 重组槽数 */
#endif
#ifndef BT_MCP_CFG_MAX_MSG
#define BT_MCP_CFG_MAX_MSG       8192   /* 单消息字节上限 */
#endif
#ifndef BT_MCP_CFG_FCS16
#define BT_MCP_CFG_FCS16         0      /* 通道无链路校验时置 1（追加 CRC16-CCITT） */
#endif
#ifndef BT_MCP_CFG_FRAG_TOUT_MS
#define BT_MCP_CFG_FRAG_TOUT_MS  2000
#endif
#ifndef BT_MCP_CFG_MAX_FRAG
#define BT_MCP_CFG_MAX_FRAG      32     /* 单消息分片上限（8KB/244B≈34→32 片封顶需分页） */
#endif

#endif
