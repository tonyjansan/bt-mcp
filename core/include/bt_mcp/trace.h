/* bt_mcp/trace.h —— 可选运行期追踪钩子（v0.4，草案 §8）。
 *
 * 目的：定位「这条消息为什么没上抛 / 为什么被丢」时不必改代码加 printf。
 * 设计约束：
 *  - **编译期可完全裁掉**：BT_MCP_CFG_TRACE=0 时 bt_mcp_trace() 展开为空，
 *    零代码零 RAM；
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

/* 发出一个事件（未设置钩子或编译期关闭时是空操作）。 */
void bt_mcp_trace(uint8_t level, const char *what,
                  const void *data, uint16_t len);

#endif
