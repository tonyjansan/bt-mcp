/* bt_mcp/tier.h —— 内存档位（编译期一键选容量，替代逐项调 config.h）。
 *
 * 用法（编译命令里选一档，或在自己的编译前导里定义 BT_MCP_TIER）：
 *     cc -DBT_MCP_TIER=BT_MCP_TIER_COMPACT  ...
 *
 * 档位只影响「容量」不改变「协议语义」：线上字节完全一致，
 * 差异只在于单消息上限、重组槽数与重传保护范围。
 *
 * | 档位 | MAX_MSG | 槽 | 重传槽 | 实测静态 RAM(framer+session+server) | 适用 |
 * |------|---------|----|--------|-----------------------------------|------|
 * | COMPACT  | 1024 | 2 | 2×256 | 4048 B  | 64KB SRAM 级 BLE SoC（8/16 位 MCU）|
 * | STANDARD | 2048 | 4 | 2×512 | 11760 B | 128KB SRAM 级，主流可穿戴 |
 * | FULL     | 8192 | 4 | 2×512 | 42480 B | 256KB+ SRAM / 网关 / 宿主 |
 * （宿主侧再加 mcp_server 的响应缓冲 g_rbuf[MAX_MSG]，档位内已含在 server 之外的
 *   静态区；应用工具自己的缓冲不计。）
 *
 * RAM 大头是「每槽一整条消息缓冲」（MAX_MSG），所以 MAX_MSG 是首要旋钮；
 * 超过档位上限的消息按草案 §5.3 走 resources 分页，协议不失效。
 */
#ifndef BT_MCP_TIER_H
#define BT_MCP_TIER_H

#define BT_MCP_TIER_COMPACT  1
#define BT_MCP_TIER_STANDARD 2
#define BT_MCP_TIER_FULL     3

#ifndef BT_MCP_TIER
#define BT_MCP_TIER BT_MCP_TIER_FULL
#endif

#if BT_MCP_TIER == BT_MCP_TIER_COMPACT
#  define BT_MCP_CFG_MAX_MSG        1024
#  define BT_MCP_CFG_MAX_REASM      2
#  define BT_MCP_CFG_TX_RETRY_BYTES 256
#  define BT_MCP_CFG_MAX_FRAG       32
#  define BT_MCP_CFG_PAGE_SIZE      4
#  define BT_MCP_CFG_NOTIFY_BUF     256
#elif BT_MCP_TIER == BT_MCP_TIER_STANDARD
#  define BT_MCP_CFG_MAX_MSG        2048
#  define BT_MCP_CFG_MAX_REASM      4
#  define BT_MCP_CFG_TX_RETRY_BYTES 512
#  define BT_MCP_CFG_MAX_FRAG       32
#  define BT_MCP_CFG_PAGE_SIZE      6
#  define BT_MCP_CFG_NOTIFY_BUF     384
#elif BT_MCP_TIER == BT_MCP_TIER_FULL
   /* 走 config.h 的默认值（8KB / 4 槽 / 512B 重传槽） */
#else
#  error "unknown BT_MCP_TIER"
#endif

#endif
