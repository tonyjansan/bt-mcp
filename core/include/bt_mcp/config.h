/* bt_mcp/config.h —— 编译期配置集中地，无 vendor ifdef（草案 §7.2）
 *
 * v0.3 起所有容量项都是「可缩档」的：把 MAX_MSG/MAX_REASM/TX_RETRY_BYTES
 * 调小即可把静态 RAM 从 ~50KB 压到 ~3KB（见 docs 内存档位表）。 */
#ifndef BT_MCP_CONFIG_H
#define BT_MCP_CONFIG_H

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
#define BT_MCP_CFG_MAX_FRAG      32     /* 单消息分片上限（8KB/244B≈34→32 片封顶需分页） */
#endif

/* ---- 发送面：不可靠包通道的消息级重传 ---- */
#ifndef BT_MCP_CFG_TX_SLOTS
#define BT_MCP_CFG_TX_SLOTS      2      /* 重传槽个数（覆盖在途消息条数） */
#endif
#ifndef BT_MCP_CFG_TX_RETRY_BYTES
#define BT_MCP_CFG_TX_RETRY_BYTES 512   /* 每槽消息体容量；超出者不享重传保护 */
#endif
#ifndef BT_MCP_CFG_RX_FP_SLOTS
#define BT_MCP_CFG_RX_FP_SLOTS   2      /* 去重指纹窗口（seq+len+CRC32，不存消息体） */
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

/* 协商协议版本（initialize 回显与支持集判定见 mcp_server.c） */
#define BT_MCP_PROTO_VERSION     "2025-11-25"

#endif
