/* bt_mcp/config.h —— 编译期配置集中地，无 vendor ifdef（草案 §7.2）
 *
 * v0.3 起所有容量项都是「可缩档」的：把 MAX_MSG/MAX_REASM/TX_RETRY_BYTES
 * 调小即可把静态 RAM 从 ~50KB 压到 ~3KB（见 docs 内存档位表）。 */
#ifndef BT_MCP_CONFIG_H
#define BT_MCP_CONFIG_H

/* 档位（可选）：定义 BT_MCP_TIER 即一键设定容量，逐项 #ifndef 仍可覆盖 */
#include "tier.h"

/* ---- 接收面 ---- */
#ifndef BT_MCP_CFG_MAX_REASM
#define BT_MCP_CFG_MAX_REASM     4      /* 重组槽数 */
#endif
#ifndef BT_MCP_CFG_MAX_MSG
#define BT_MCP_CFG_MAX_MSG       8192   /* 单消息字节上限（≤ u16 len 可承载） */
#endif
#ifndef BT_MCP_CFG_FRAG_TOUT_MS
#define BT_MCP_CFG_FRAG_TOUT_MS  2000   /* 半帧滞留回收时限 */
#endif
#ifndef BT_MCP_CFG_MAX_FRAG
/* 单消息分片上限：idx 是 u8，故上限 255。v0.2 的 32 会让 8161..8192B 的消息
 * 在 MTU 261 时假性 E_TOOBIG；v0.3 起放到 255，使「MAX_MSG 可达」与
 * 「小 MTU 通道也能装下档位消息」同时成立（8KB 在 23B ATT_MTU 下需 586 片，
 * 仍受 255 限制——这是 u8 idx 的硬上限，文档已注明）。 */
#define BT_MCP_CFG_MAX_FRAG      255
#endif

/* ---- 发送面：不可靠包通道的消息级重传 ---- */
#ifndef BT_MCP_CFG_TX_SLOTS
#define BT_MCP_CFG_TX_SLOTS      2      /* 重传槽个数（覆盖在途消息条数） */
#endif
#ifndef BT_MCP_CFG_TX_RETRY_BYTES
#define BT_MCP_CFG_TX_RETRY_BYTES 512   /* 每槽消息体容量；超出者不享重传保护 */
#endif
#ifndef BT_MCP_CFG_RX_FP_SLOTS
/* 去重指纹窗口：必须 ≥ 发送窗口（tx_window），否则「4 条在途 + ACK 全丢」
 * 时前几条的指纹已被挤出，重传会被当作新消息二次上抛（重复执行工具）。
 * 默认 4 与 tx_window 默认值对齐。 */
#define BT_MCP_CFG_RX_FP_SLOTS   4      /* 指纹项：seq+len+CRC32+代次，不存消息体 */
#endif
#ifndef BT_MCP_CFG_ACK_TOUT_MS
#define BT_MCP_CFG_ACK_TOUT_MS   600    /* 单次等待 ACK 时限 */
#endif
#ifndef BT_MCP_CFG_MAX_RETRY
#define BT_MCP_CFG_MAX_RETRY     3      /* 重传次数上限（超过即放弃并计数） */
#endif
#ifndef BT_MCP_CFG_DEDUPE_MS
#define BT_MCP_CFG_DEDUPE_MS     5000   /* 重复消息判定时间窗（覆盖重传horizon） */
#endif

/* ---- 安全：RESUME 防爆破 ---- */
#ifndef BT_MCP_CFG_RESUME_MAX_FAIL
#define BT_MCP_CFG_RESUME_MAX_FAIL 3    /* 超过此次数进入锁定 */
#endif
#ifndef BT_MCP_CFG_RESUME_LOCK_MS
#define BT_MCP_CFG_RESUME_LOCK_MS  30000 /* 首次锁定时长（之后指数翻倍，上限 8×） */
#endif

/* ---- L5 面 ---- */
#ifndef BT_MCP_CFG_PAGE_SIZE
#define BT_MCP_CFG_PAGE_SIZE     8      /* tools/list、resources/list 每页条数 */
#endif
#ifndef BT_MCP_CFG_MAX_PENDING
#define BT_MCP_CFG_MAX_PENDING   2      /* 异步 tools/call 在途槽数 */
#endif
#ifndef BT_MCP_CFG_IDSTR_MAX
#define BT_MCP_CFG_IDSTR_MAX     32     /* 字符串 request id 复制上限（超长按 -32600 处理） */
#endif
#ifndef BT_MCP_CFG_KEY_MAX
#define BT_MCP_CFG_KEY_MAX       40     /* JSON 键名解码缓冲（超长键静默跳过该键） */
#endif
#ifndef BT_MCP_CFG_JSON_MAX_DEPTH
#define BT_MCP_CFG_JSON_MAX_DEPTH 6
#endif
#ifndef BT_MCP_CFG_NOTIFY_BUF
#define BT_MCP_CFG_NOTIFY_BUF    512    /* 设备主动通知拼装缓冲 */
#endif
#ifndef BT_MCP_CFG_FCS16
#define BT_MCP_CFG_FCS16         0      /* 通道无链路校验时置 1（追加 CRC16-CCITT） */
#endif

/* ---- 配置合法性编译期断言（配错直接编不过，而不是运行期出错） ---- */
/* MAX_MSG 用 u16 承载片长与重组计数，必须 ≤ 65535 */
#if BT_MCP_CFG_MAX_MSG > 65535
#  error "BT_MCP_CFG_MAX_MSG must be <= 65535 (u16 framing/len_total)"
#endif
#if BT_MCP_CFG_MAX_MSG < 64
#  error "BT_MCP_CFG_MAX_MSG too small to carry a minimal MCP message"
#endif
#if BT_MCP_CFG_RX_FP_SLOTS < 1 || BT_MCP_CFG_RX_FP_SLOTS > 16
#  error "BT_MCP_CFG_RX_FP_SLOTS must be 1..16"
#endif
#if BT_MCP_CFG_NOTIFY_BUF < 192
#  error "BT_MCP_CFG_NOTIFY_BUF must be >= 192 (progress notification worst case)"
#endif
#if BT_MCP_CFG_MAX_FRAG < 1 || BT_MCP_CFG_MAX_FRAG > 255
#  error "BT_MCP_CFG_MAX_FRAG must be 1..255 (u8 idx)"
#endif
#if BT_MCP_CFG_MAX_REASM < 1 || BT_MCP_CFG_MAX_REASM > 8
#  error "BT_MCP_CFG_MAX_REASM must be 1..8 (max_pending is a 4-bit field)"
#endif
#if BT_MCP_CFG_TX_SLOTS < 1 || BT_MCP_CFG_TX_SLOTS > 8
#  error "BT_MCP_CFG_TX_SLOTS must be 1..8"
#endif
#if BT_MCP_CFG_TX_RETRY_BYTES < 32
#  error "BT_MCP_CFG_TX_RETRY_BYTES must be >= 32 (or set retry_max=0 to disable)"
#endif
/* 单片载荷 ≤255（u8 idx 分片数 × 255 需覆盖 MAX_MSG） */
#if BT_MCP_CFG_MAX_FRAG * 255 < BT_MCP_CFG_MAX_MSG
#  error "BT_MCP_CFG_MAX_FRAG too small for BT_MCP_CFG_MAX_MSG at 255B chunks"
#endif

#ifndef BT_MCP_CFG_MAX_SUBS
#define BT_MCP_CFG_MAX_SUBS      8      /* 同时订阅的资源数上限 */
#endif
#ifndef BT_MCP_CFG_MAX_S2C_REQ
#define BT_MCP_CFG_MAX_S2C_REQ   2      /* server→client 在途请求上限 */
#endif
#ifndef BT_MCP_CFG_S2C_TOUT_MS
#define BT_MCP_CFG_S2C_TOUT_MS   30000  /* S→C 请求超时 */
#endif
#ifndef BT_MCP_CFG_META_MAX
#define BT_MCP_CFG_META_MAX      256    /* 单次 tools/call 可携带的 _meta 原文上限 */
#endif
#ifndef BT_MCP_CFG_KEEPALIVE_MS
#define BT_MCP_CFG_KEEPALIVE_MS  0      /* >0 时空闲该毫秒数后自动 PING（0=关闭） */
#endif
#ifndef BT_MCP_CFG_IDLE_DEAD_MS
#define BT_MCP_CFG_IDLE_DEAD_MS  0      /* >0 时超过该毫秒无入站即判链路失效（0=关闭） */
#endif
#ifndef BT_MCP_CFG_TRACE
#define BT_MCP_CFG_TRACE         0      /* 1 = 编译进追踪钩子（debug 构建用） */
#endif

/* 协商协议版本（initialize 回显与支持集判定见 mcp_server.c） */
#define BT_MCP_PROTO_VERSION     "2025-11-25"

#endif
