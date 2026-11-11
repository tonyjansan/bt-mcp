/* err.c —— 错误码到稳定字符串（v0.4）。静态常量表，无分配。 */
#include "bt_mcp/err.h"

const char *bt_mcp_strerror(int rc)
{
    switch (rc) {
    case BT_MCP_OK:        return "OK";
    case BT_MCP_E_AGAIN:   return "E_AGAIN";    /* 暂时性，稍后重试 */
    case BT_MCP_E_ARGS:    return "E_ARGS";
    case BT_MCP_E_STATE:   return "E_STATE";
    case BT_MCP_E_TOOBIG:  return "E_TOOBIG";
    case BT_MCP_E_NOMEM:   return "E_NOMEM";
    case BT_MCP_E_SYNC:    return "E_SYNC";
    case BT_MCP_E_PARSE:   return "E_PARSE";
    case BT_MCP_E_IO:      return "E_IO";
    case BT_MCP_E_PENDING: return "E_PENDING";
    case BT_MCP_E_CANCEL:  return "E_CANCEL";
    case BT_MCP_E_LOCKED:  return "E_LOCKED";
    case BT_MCP_E_AUTHZ:   return "E_AUTHZ";
    case BT_MCP_E_BUSY:    return "E_BUSY";
    default:               return "E_UNKNOWN";
    }
}
