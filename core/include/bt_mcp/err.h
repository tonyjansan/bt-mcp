/* bt_mcp/err.h —— 统一错误码（草案 §7） */
#ifndef BT_MCP_ERR_H
#define BT_MCP_ERR_H

enum {
    BT_MCP_OK      = 0,
    BT_MCP_E_AGAIN = -1,  /* 缓冲/窗口暂满，稍后 tick 重试 */
    BT_MCP_E_ARGS  = -2,
    BT_MCP_E_STATE = -3,  /* 当前会话状态不接受此操作 */
    BT_MCP_E_TOOBIG = -4, /* 超 8KB 消息上限或分片超限 */
    BT_MCP_E_NOMEM = -5,  /* 静态池耗尽 */
    BT_MCP_E_SYNC = -6,  /* 流失步/重组校验失败 */
    BT_MCP_E_PARSE = -7, /* JSON/参数解析失败 */
    BT_MCP_E_IO   = -8  /* 底层 sink/transport 写失败 */
};

#endif
