/* bt_mcp/trace.h —— 可选运行期追踪钩子（v0.4，草案 §8）。
 *
 * 目的：定位「这条消息为什么没上抛 / 为什么被丢」时不必改代码加 printf。
 * 设计约束：
 *  - **编译期可完全裁掉**：BT_MCP_CFG_TRACE=0 时为**宏**展开成空语句，
 *    调用点不生成任何代码（v0.8 修正：原先是函数声明，调用点仍会生成
 *    call 指令，与「零代码」的说法不符——现改为宏）；
 *  - 全局单例（调试设施，非协议状态）：默认不设置即无开销；
 *  - 回调内**禁止**调用任何 bt_mcp_* API（会重入），只允许记录/打印。
 */
#ifndef BT_MCP_TRACE_H
#define BT_MCP_TRACE_H

#include <stdint.h>
#include "config.h"

/* 事件级别：越大越严重 */
#define BT_MCP_TRACE_DBG   0   /* 帧收发/状态迁移细节 */
#define BT_MCP_TRACE_INFO  1   /* 会话状态变化、方法分派 */
#define BT_MCP_TRACE_WARN  2   /* 丢帧/重传/拒绝/授权失败 */
#define BT_MCP_TRACE_ERROR 3   /* 协议错误、槽耗尽 */

typedef void (*bt_mcp_trace_fn)(void *user, uint8_t level,
                                const char *what,
                                const void *data, uint16_t len);

/* 设置/清除追踪钩子（fn=NULL 清除）。 */
void bt_mcp_trace_set(void *user, bt_mcp_trace_fn fn);

#if BT_MCP_CFG_TRACE
/* 发出一个事件（未设置钩子时也是空操作）。 */
void bt_mcp_trace(uint8_t level, const char *what,
                  const void *data, uint16_t len);
#else
/* 编译期关闭：宏展开为空，调用点零代码（这是本头文件承诺的核心性质）。 */
#  define bt_mcp_trace(level, what, data, len)  ((void)0)
#endif

#endif
