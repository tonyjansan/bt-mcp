/* trace.c —— 追踪钩子实现（v0.4）。未设置钩子时零副作用。
 *
 * v0.8：BT_MCP_CFG_TRACE=0 时 trace.h 把 bt_mcp_trace 定义成空宏，
 * 调用点零代码；因此本文件在关闭档只需提供 bt_mcp_trace_set 的空实现，
 * 不再定义 bt_mcp_trace 函数（原实现定义一个空函数，调用点仍要付 call 成本，
 * 与头文件「零代码」的承诺不符）。 */
#include "bt_mcp/trace.h"

/* 编译关闭档：宏会改写下面的函数定义，先撤销（仅本编译单元） */
#if !BT_MCP_CFG_TRACE
#  undef bt_mcp_trace
#endif

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

#else  /* BT_MCP_CFG_TRACE == 0：bt_mcp_trace 由头文件的空宏替代 */

void bt_mcp_trace_set(void *user, bt_mcp_trace_fn fn)
{
    (void)user; (void)fn;
}

#endif
