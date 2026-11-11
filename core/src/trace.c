/* trace.c —— 追踪钩子实现（v0.4）。未设置钩子时零副作用。 */
#include "bt_mcp/trace.h"

#if BT_MCP_CFG_TRACE

static void        *g_trace_user;
static bt_mcp_trace_fn g_trace_fn;

void bt_mcp_trace_set(void *user, bt_mcp_trace_fn fn)
{
    g_trace_user = user;
    g_trace_fn = fn;
}

void bt_mcp_trace(uint8_t level, const char *what,
                  const void *data, uint16_t len)
{
    if (g_trace_fn) g_trace_fn(g_trace_user, level, what, data, len);
}

#else  /* BT_MCP_CFG_TRACE == 0：完全裁掉，连符号都不生成 */

void bt_mcp_trace_set(void *user, bt_mcp_trace_fn fn)
{
    (void)user; (void)fn;
}

void bt_mcp_trace(uint8_t level, const char *what,
                  const void *data, uint16_t len)
{
    (void)level; (void)what; (void)data; (void)len;
}

#endif
