/* bt_mcp/config.h —— 编译期配置集中地，无 vendor ifdef（草案 §7.2）
 *
 * v0.3 起所有容量项都是「可缩档」的：把 MAX_MSG/MAX_REASM/TX_RETRY_BYTES
 * 调小即可把静态 RAM 从 ~50KB 压到 ~3KB（见 docs 内存档位表）。 */
#ifndef BT_MCP_CONFIG_H
#define BT_MCP_CONFIG_H

/* 档位（可选）：定义 BT_MCP_TIER 即一键设定容量，逐项 #ifndef 仍可覆盖 */
#include "tier.h"

/* ---- 接收面 ---- */
/* v0.5 V11：共享重组缓冲。
 * 0 = 每槽独立 buf[MAX_MSG]（默认，支持多消息交织重组）
 * 1 = 单槽 + 单个 MAX_MSG 缓冲（RAM 从 MAX_REASM×MAX_MSG 降到 MAX_MSG）
 *
 * 代价（明确记录）：共享模式只允许**一条**消息在重组中。对端若交织发送多条
 * 消息，后到的按「槽已占用」丢弃（协议允许，吞吐下降）。适合 RAM 紧张、
 * 实际只有一个大消息在途的场景（如单连接传感器）。 */
#ifndef BT_MCP_CFG_SHARED_REASM
#define BT_MCP_CFG_SHARED_REASM  0
#endif
#ifndef BT_MCP_CFG_MAX_REASM
#define BT_MCP_CFG_MAX_REASM     4      /* 重组槽数（共享模式下强制为 1） */
#endif
#if BT_MCP_CFG_SHARED_REASM && BT_MCP_CFG_MAX_REASM != 1
#  undef  BT_MCP_CFG_MAX_REASM
#  define BT_MCP_CFG_MAX_REASM   1     /* 共享缓冲只能承载一条消息 */
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
/* 去重指纹窗口：必须 ≥ 发送窗口（tx_window），否则「N 条在途 + ACK 全丢」
 * 时前几条的指纹已被挤出，重传会被当作新消息二次上抛（重复执行工具）。
 * v0.6：默认 4 → 8。v0.6 的链路仿真 soak 实测：在 10%~50% 丢包 + 乱序下，
 * 4 个槽不足以覆盖「放弃重试后仍在途的副本」的到达延迟，出现重复投递；
 * 提到 8 后大幅收敛（剩余缺口见 docs/roadmap-v0.6.md 的 N6 遗留项）。
 * 代价：每槽约 16B（seq+len+crc+代次+时间），8 槽约 128B。 */
#define BT_MCP_CFG_RX_FP_SLOTS   8      /* 指纹项：seq+len+CRC32+代次，不存消息体 */
#endif
#ifndef BT_MCP_CFG_ACK_TOUT_MS
#define BT_MCP_CFG_ACK_TOUT_MS   600    /* 单次等待 ACK 时限 */
#endif
#ifndef BT_MCP_CFG_MAX_RETRY
#define BT_MCP_CFG_MAX_RETRY     3      /* 重传次数上限（超过即放弃并计数） */
#endif
/* v0.7（A2）：收到 NACK 时一次最多重放多少片（尾部重放上限）。
 * 必须 ≥ 一条消息的典型分片数，否则一轮补不齐、要多轮才能收敛。
 * 实测（MTU 23 / 200B = 12 片 / 20% 丢包）：8 片 → 交付 23%，
 * 16 片 → 57%（一次覆盖全部分片）。取 16。 */
/* v0.9.1（A2 根因修复）：一次 NACK 最多报几个缺片。
 * 位图让接收侧能精确知道缺哪些，于是只需补这些——不再「补到尾」。
 * 上限同时决定控制帧大小（1 + 1 + 1 + N ≤ 255，N ≤ 252）。 */
#ifndef BT_MCP_CFG_NACK_MAX_IDX
#define BT_MCP_CFG_NACK_MAX_IDX   16
#endif
#ifndef BT_MCP_CFG_NACK_REPLAY_MAX
#define BT_MCP_CFG_NACK_REPLAY_MAX  16
#endif
/* v0.7（A2）：单条消息因 NACK 累计重放的片数上限。
 * 没有它时，尾部重放会反复触发（实测小 MTU 场景 retx 达 1.3 万次/60 条 ≈ 每条
 * 重放 19 遍），带宽被无效重放吃掉。预算 = 分片数 × 该倍数。
 * 实测倍数的拐点：3 → 交付 20%，8 → 22%，16 → **57%**，32 → 55%（饱和）。
 * 取 16。注意这仍是「重放已到达的片」的浪费做法，根因修法见 roadmap A2 遗留：
 * 接收侧乱序缓存 + 缺片位图，只补真正缺的片。 */
#ifndef BT_MCP_CFG_NACK_REPLAY_BUDGET_X
#define BT_MCP_CFG_NACK_REPLAY_BUDGET_X  16
#endif
#ifndef BT_MCP_CFG_DEDUPE_MS
/* 接收去重时间窗。**必须覆盖发送侧的完整重传寿命**，否则迟到副本落在窗外
 * 会被当作新消息二次上抛（工具重复执行）。
 * 推导：指数退避下重传寿命 ≈ ACK_TOUT × (2^MAX_RETRY − 1) = 600 × 7 = 4200ms，
 * 再加最后一次重传的在途时间与调度抖动 → 取 4200 × 4 ≈ 20s 为安全值。
 * v0.7 实测（docs/roadmap-v0.7.md A1）：5000ms 时 30%~50% 丢包仍有 2~5/120 条
 * 重复；20000ms 后全部归零，代价是放弃重试的消息更少被误当"已送达"。
 * RAM 代价：零（只多存一个时间戳比较）。 */
#define BT_MCP_CFG_DEDUPE_MS       20000
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
#ifndef BT_MCP_CFG_MAX_CONN
#define BT_MCP_CFG_MAX_CONN      4      /* 一个 server pool 可挂的并发会话数 */
#endif
/* v0.6：信用刷新周期。累计 CREDIT 是幂等的，空闲时周期性重发可让对端从
 * 「丢了一个 CREDIT」中自愈。没有它，发送方信用耗尽且无在途消息时无法恢复
 * （v0.6 soak 实测：30% 丢包下会永久卡在 E_AGAIN）。0 = 关闭刷新。 */
#ifndef BT_MCP_CFG_CREDIT_REFRESH_MS
#define BT_MCP_CFG_CREDIT_REFRESH_MS  600
#endif
#ifndef BT_MCP_CFG_RTO_MAX_MS
#define BT_MCP_CFG_RTO_MAX_MS    4800   /* 指数退避后的 RTO 上限（=8×600 基准） */
#endif
#ifndef BT_MCP_CFG_BATCH_BUF
#define BT_MCP_CFG_BATCH_BUF     4096   /* JSON-RPC batch 响应聚合缓冲 */
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
