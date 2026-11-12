# BT-MCP 实现说明 v0.6 —— 平台无关设计

日期：2026-11-12 · 状态：**实现说明（informative）**。本文件面向**在本仓维护代码的人**：
设计原则、模块划分、C API 骨架、配置与内存、port 清单、验证体系。

> **协议线上格式与语义**见 [`bt-mcp-protocol-spec.md`](bt-mcp-protocol-spec.md)。
> 二者冲突时以规范文件为准。



日期：2026-11-11 · 状态：草案。v0.6 实现已验证：**1295 项差分 + 198 项 port HIL
+ 695 项单元/定向 + 11469 项表驱动矩阵断言 + 38 项官方 MCP SDK 互验 + 20 项 golden**，
全绿；行覆盖 **95.1%** / 分支 **68.2%**；ASan/UBSan/TSan 干净。

> 本文是**唯一的协议与接口权威文档**（与公开头文件同步）。版本演进见下方摘要；
> 各版本的发布说明与验证报告见 [`docs/README.md`](README.md) 的索引。

## 版本演进摘要

**v0.6 相对 v0.5 的变化（主要是验证与健壮性，协议语义仅两处）：**

- **协议语义 1：空闲信用刷新**。累计 CREDIT 帧不受 ARQ 保护，丢一个就少一格；
  发送方信用耗尽且管道已空时，对端无新消费可用来补发 → 永久 `E_AGAIN`。
  修复为 `session_tick` 在空闲 ≥ `BT_MCP_CFG_CREDIT_REFRESH_MS` 时重发累计
  CREDIT（载荷是绝对值，重发幂等）（§5.4）；
- **协议语义 2：去重窗口 4 → 8**（`RX_FP_SLOTS`）。同时把「恰好一次」的表述
  从**无条件保证**改为**有界保证**，并给出实测对照表（§5.4）；
- **writer 去掉 libc `snprintf` 依赖**：改为自研整数格式化（`fmt_u64`/`fmt_i64`，
  含 INT64_MIN 溢出边界），使 JSON 写出层可在无 printf 的裸机环境构建；
- **writer 错误语义**：透传 sink 错误码（区分 `E_TOOBIG` 与 `E_IO`）；
- **验证引擎换代**：表驱动组合矩阵（3141 生成用例）、语法感知 fuzz（四条不变量）、
  正交差分矩阵（配置签名比对）、链路仿真 soak、多会话压测、RAM 预算门禁、
  文档一致性检查。详见 `roadmap-v0.6.md`。

**v0.5 相对 v0.4 的变化：**
- **规范语义修正**：`ping` 响应改为空结果对象 `{}`（原 `{"pong":true}` 不符规范）；
  未初始化错误码改为 `-32001`，与「资源不存在」`-32002` 分离（§5.2）；
- **`_meta` 透传**：工具经 `bt_mcp_call_set_meta()` 声明，结果层输出；
  进度通知增加规范可选字段 `message`（§5.6）；
- **JSON-RPC batch 按协商版本分支**：`2025-03-26` 及更早聚合为数组，
  `2025-06-18+` 明确回 `-32600`（§5.2）；
- **大消息重传保护**：`bt_mcp_session_tx_ref()` 引用式保护，突破 512B 限制（§5.4）；
- **拥塞退避**：RTO 指数退避（上限 `BT_MCP_CFG_RTO_MAX_MS`），ACK 到达即回落（§5.4）；
- **多会话**：`bt_mcp_server_pool_t` 按 session 分派（§7.1）；
- **共享重组缓冲**：`BT_MCP_CFG_SHARED_REASM=1` 把 framer 从 41072B 降到 16448B（§7.3）；
- **解析器收紧**：拒绝尾随垃圾与尾随逗号（帧长度已界定边界，尾随字节即损坏）（§4.5）；
- **健壮性**：writer 校验 sink、client/JSON getter 校验空指针，全部 fail-closed（§7.4）。

**v0.4 相对 v0.3 的变化：**
- **授权真正实施**：`AUTHZ_CONSENT` 走 consent 回调、`AUTHZ_RESTRICTED` 一律拒绝，
  拒绝映射 `-32003`（§6.3）；
- **prompts / 资源订阅 / 日志分级 / list_changed** 四项能力落地（§5.2）；
- **富内容结果**：`text` / `image` / `audio` / `embedded resource` 多内容项（§5.6）；
- **server→client 请求**：`sampling`/`roots`/`elicitation` 的承载能力，
  在途槽 + id 匹配 + 超时（§5.7）；
- **MTU 热更新**：`bt_mcp_framer_set_mtu()`，BLE MTU 交换后无需重建 framer（§4.1）；
- **NVM 持久化**：resume token / 绑定标志可落盘，掉电可恢复（§5.8）；
- **keepalive**：`KEEPALIVE_MS` / `IDLE_DEAD_MS`（默认关闭）（§5.8）；
- **响应缓冲去静态化**：server 与 client 的拼装缓冲移入实例，多设备桥接才正确（§7.4）。

---

## 1. 设计原则

1. **协议核心不感知蓝牙**：core/ 只看到「带 MTU 语义的发送通道 + 字节/包回调 + 连接状态事件」。GATT、SPP、CoC、甚至 USB-CDC/UART 都是同一抽象的一个 port。
2. **两类通道，一套帧格式**：
   - **包通道**（GATT notify/write、L2CAP CoC）：有 MTU、可能丢包/无跨包边界保证 → 帧层带序号与分片重组；
   - **流通道**（SPP/RFCOMM、UART、USB-CDC）：字节流有序可靠 → 同一帧格式直接连发，序号退化为对齐/丢弃用途。
   两种通道**共用同一帧格式**（流通道多付 4 字节/帧头的成本，换取实现唯一性与互操作性）。
3. **最大平台兼容**：core/ 为纯 C99、无 OS 调用、无芯片头文件、无动态分配（编译期可切静态池）、无 `#ifdef <vendor>`。RTOS 与裸机（主循环轮询 `*_tick()`）均可驱动。
4. **MCP 语义严格子集**：线上字节与标准 MCP（JSON-RPC 2.0）互通，裁剪面在 §5。
5. **不锚定任何手机 OS**：手机/网关侧 Bridge 是配套件而非协议一部分；协议只定义设备端与 Bridge 之间的空中字节。

## 2. 层次与模块总览

```
L6  provider      设备业务（传感器/工具/资源表，编译期注册）
L5  mcp_server    JSON-RPC 编解码、方法分派、能力声明、超时/取消/进度
L4  session       会话状态机、请求-响应关联、信用流控、断链恢复
L3  frame         分帧/分片/重组、序号去重、通道类型自适应
L2  transport      通道抽象（port 实现：GATT / SPP / CoC / UART / host-mock）
L1  （任意蓝牙协议栈：vendor host、Zephyr、bluedroid…仅 port 层接触）
```

依赖方向严格单向向下：L6→L5→L4→L3→L2；反向通信只经注册的回调（下行数据、事件）。
**所有模块为非阻塞、事件驱动、单写者模型**：任意时刻只有一个执行上下文进入协议栈（裸机主循环或单一协议线程），port 层负责把 ISR/栈回调投递进该上下文（队列/信号量由 port 提供）。

## 7. C99 模块接口骨架

> 本节是**公开头文件的镜像**（v0.5）。改动 API 时必须同步更新本节；
> §7.5 给出防漂移的自动检查方法。

约定：所有类型以 `bt_mcp_` 前缀；所有函数非阻塞、返回 `int`（0 成功，
负值 = `BT_MCP_E*`）；时间一律由调用方传入 `now_ms`（无 OS 时钟依赖）；
无动态分配；所有实例状态都在调用方持有的结构体里（无模块级可变状态，
trace 钩子除外且仅调试用）。

### 7.0 错误码（`bt_mcp/err.h`）

```c
enum {
    BT_MCP_OK        =  0,
    BT_MCP_E_AGAIN   = -1,   /* 缓冲/窗口暂满，稍后 tick 重试 */
    BT_MCP_E_ARGS    = -2,   /* 参数非法（含空指针） */
    BT_MCP_E_STATE   = -3,   /* 当前会话状态不接受此操作 */
    BT_MCP_E_TOOBIG  = -4,   /* 超 MAX_MSG / 分片超限 / 结果过大 */
    BT_MCP_E_NOMEM   = -5,   /* 静态池耗尽（槽满） */
    BT_MCP_E_SYNC    = -6,   /* 流失步 / 重组校验失败 */
    BT_MCP_E_PARSE   = -7,   /* JSON 语法/类型不匹配 */
    BT_MCP_E_IO      = -8,   /* 底层 emit/sink 失败 */
    BT_MCP_E_PENDING = -9,   /* 工具已受理，结果稍后经 complete 送达 */
    BT_MCP_E_CANCEL  = -10,  /* 已被 notifications/cancelled 取消 */
    BT_MCP_E_LOCKED  = -11,  /* RESUME 退避锁定中 */
    BT_MCP_E_AUTHZ   = -12,  /* 授权拒绝（consent 未放行 / RESTRICTED） */
    BT_MCP_E_BUSY    = -13,  /* 在途请求槽已满 */
};
const char *bt_mcp_strerror(int rc);   /* 稳定字符串，便于日志 */
```

### 7.1 L3 分帧（`bt_mcp/frame.h`）

```c
typedef void (*bt_mcp_msg_sink_t)(void *user, uint8_t kind, uint8_t seq,
                                  const uint8_t *payload, uint16_t len);
    /* kind: BT_MCP_KIND_MCP=0（完整重组后的 MCP 消息）
             BT_MCP_KIND_CTRL=1（控制帧，原样上抛 L4） */

typedef struct {
    uint8_t  max_reasm;      /* 重组槽数；0 = 用编译期默认 */
    uint16_t frag_tout_ms;
} bt_mcp_frame_cfg_t;

typedef struct bt_mcp_framer bt_mcp_framer_t;   /* 定长，大小编译期定 */

int  bt_mcp_framer_init(bt_mcp_framer_t *f, const bt_mcp_frame_cfg_t *cfg,
                        const bt_mcp_chan_cap_t *cap);
/* 发送：整条消息按 cap.mtu 切片，经 emit 逐片吐出（一片 = 一次回调） */
int  bt_mcp_framer_send(bt_mcp_framer_t *f, uint8_t kind,
                        const uint8_t *payload, uint32_t len,
                        int (*emit)(void*, const uint8_t*, uint16_t), void *io);
/* 指定 seq / 指定单片的发送（重传与 NACK 补片用；v0.3） */
int  bt_mcp_framer_send_seq(bt_mcp_framer_t *f, uint8_t kind, uint8_t seq,
                            const uint8_t *payload, uint32_t len,
                            int (*emit)(void*, const uint8_t*, uint16_t), void *io);
int  bt_mcp_framer_send_idx(bt_mcp_framer_t *f, uint8_t kind, uint8_t seq,
                            uint8_t idx, const uint8_t *payload, uint32_t len,
                            int (*emit)(void*, const uint8_t*, uint16_t), void *io);
uint8_t bt_mcp_framer_alloc_seq(bt_mcp_framer_t *f);
/* 接收：喂原始字节（包通道喂整片；流通道喂任意字节段），重组完成经 sink 上抛 */
int  bt_mcp_framer_feed(bt_mcp_framer_t *f, const uint8_t *in, uint16_t len,
                        bt_mcp_msg_sink_t sink, void *user);
/* 取走待发 NACK 登记：1=有（seq/idx 已填），0=无 */
int  bt_mcp_framer_take_nack(bt_mcp_framer_t *f, uint8_t *seq, uint8_t *idx);
void bt_mcp_framer_tick(bt_mcp_framer_t *f, uint32_t now_ms);
/* 链路 MTU 热更新（v0.4）：0 = 回退 cap->mtu；流通道返回 E_ARGS */
int  bt_mcp_framer_set_mtu(bt_mcp_framer_t *f, uint16_t mtu);
void bt_mcp_framer_reset(bt_mcp_framer_t *f);   /* 断链丢弃半帧 */
```

### 7.2 L4 会话（`bt_mcp/session.h`）

```c
typedef enum {
    BT_MCP_S_IDLE = 0, BT_MCP_S_LINK_UP, BT_MCP_S_READY
} bt_mcp_state_t;

typedef void (*bt_mcp_mcp_sink_t)(void *user, const uint8_t *json, uint32_t len);

typedef struct {
    uint32_t req_timeout_ms;   /* 在途请求超时（0 = 30000） */
    uint32_t resume_tout_ms;
    uint8_t  tx_window;        /* 包通道应用层信用初值 */
    uint8_t  retry_max;        /* 重传上限（0 = 默认 3） */
    uint16_t ack_tout_ms;      /* ACK 时限基准（0 = 默认 600），也是 RTO 回落值 */
    uint8_t  role;             /* BT_MCP_ROLE_CLIENT / _SERVER（RESUME 裁决方） */
} bt_mcp_session_cfg_t;

typedef struct {               /* NVM 持久化钩子（v0.4） */
    int (*save)(void *user, const void *buf, uint16_t len);
    int (*load)(void *user, void *buf, uint16_t len);
    void *user;
} bt_mcp_storage_t;

typedef struct {               /* 落盘快照布局 */
    uint32_t magic;            /* 'BTM1' */
    uint16_t version;          /* 1 */
    uint16_t flags;            /* bit0 = 已绑定 */
    uint32_t resume_token;
} bt_mcp_session_state_t;

typedef struct bt_mcp_session bt_mcp_session_t;   /* 定长 */

int  bt_mcp_session_init(bt_mcp_session_t *s, const bt_mcp_session_cfg_t *cfg);
void bt_mcp_session_bind(bt_mcp_session_t *s, bt_mcp_framer_t *fr,
                         int (*emit)(void*, const uint8_t*, uint16_t), void *io);
bt_mcp_state_t bt_mcp_session_state(const bt_mcp_session_t *s);

/* 状态迁移（由 port / 应用驱动） */
void bt_mcp_session_link_up(bt_mcp_session_t *s, uint32_t resume_token);
void bt_mcp_session_mark_ready(bt_mcp_session_t *s);
void bt_mcp_session_link_down(bt_mcp_session_t *s, int bond_intact);

/* 下行（来自 L5 以外的管道）；上行（来自 L5），受信用窗口约束 */
int  bt_mcp_session_rx(bt_mcp_session_t *s, uint8_t kind, uint8_t seq,
                       const uint8_t *payload, uint32_t len,
                       bt_mcp_mcp_sink_t sink, void *user);
int  bt_mcp_session_tx(bt_mcp_session_t *s, const uint8_t *json, uint32_t len);
/* v0.5：引用式发送——槽里只记指针，重传时读调用方缓冲，突破 512B 限制。
 * 契约：缓冲必须在 ACK/放弃之前有效。 */
int  bt_mcp_session_tx_ref(bt_mcp_session_t *s, const uint8_t *json, uint32_t len);

/* 便捷路径：喂字节 → framer 重组 → 状态门 → 上抛 sink（推荐 port 用这个） */
int  bt_mcp_session_feed(bt_mcp_session_t *s, const uint8_t *in, uint16_t len,
                         bt_mcp_mcp_sink_t sink, void *user);
int  bt_mcp_session_pump(bt_mcp_session_t *s);       /* 冲待发 NACK/控制帧 */
int  bt_mcp_session_ctrl(bt_mcp_session_t *s, uint8_t type,
                         const uint8_t *args, uint8_t alen);
int  bt_mcp_session_ping(bt_mcp_session_t *s, uint8_t epoch);
uint32_t bt_mcp_session_rtt(const bt_mcp_session_t *s, int *valid);
void bt_mcp_session_tick(bt_mcp_session_t *s, uint32_t now_ms);

/* 持久化与 keepalive（v0.4） */
void     bt_mcp_session_set_storage(bt_mcp_session_t *s, const bt_mcp_storage_t *st);
int      bt_mcp_session_save_state(const bt_mcp_session_t *s);
int      bt_mcp_session_load_state(bt_mcp_session_t *s);
uint32_t bt_mcp_session_resume_token(const bt_mcp_session_t *s);
```

### 7.3 L5 MCP server（`bt_mcp/mcp_server.h`）

注册表（**全部建议用指定初始化器**，字段顺序在 v0.4 变过）：

```c
/* 授权级别 */
enum { BT_MCP_AUTHZ_OPEN = 0, BT_MCP_AUTHZ_CONSENT, BT_MCP_AUTHZ_RESTRICTED };
/* 工具注解位（对应 MCP annotations） */
#define BT_MCP_TOOL_READONLY     0x01
#define BT_MCP_TOOL_DESTRUCTIVE  0x02
#define BT_MCP_TOOL_IDEMPOTENT   0x04
#define BT_MCP_TOOL_OPENWORLD    0x08
/* 日志级别 */
enum { BT_MCP_LOG_DEBUG = 0, BT_MCP_LOG_INFO, BT_MCP_LOG_NOTICE,
       BT_MCP_LOG_WARNING, BT_MCP_LOG_ERROR, BT_MCP_LOG_CRITICAL,
       BT_MCP_LOG_ALERT, BT_MCP_LOG_EMERGENCY };

typedef int (*bt_mcp_tool_fn)(const bt_mcp_json_req_t *args, bt_mcp_writer_t *out,
                             bt_mcp_call_t *call, void *user);
typedef struct {
    const char *name; const char *desc; const char *title;
    const char *input_schema_json; const char *output_schema_json;
    uint8_t     annotations;    /* BT_MCP_TOOL_* 位或 */
    uint8_t     authz;          /* BT_MCP_AUTHZ_* */
    bt_mcp_tool_fn fn; void *user;
} bt_mcp_tool_def_t;

typedef int (*bt_mcp_resource_read_fn)(void *user, uint32_t cursor,
                                       const char **text, uint32_t *len,
                                       uint32_t *next_cursor);
typedef int (*bt_mcp_resource_sub_fn)(void *user, int subscribe);
typedef struct {
    const char *name; const char *desc; const char *title;
    const char *mime_type; uint32_t size;
    uint8_t     authz;
    bt_mcp_resource_read_fn read; bt_mcp_resource_sub_fn sub; void *user;
} bt_mcp_resource_def_t;

typedef int (*bt_mcp_prompt_get_fn)(const bt_mcp_json_req_t *args,
                                    bt_mcp_writer_t *out, void *user);
typedef struct {
    const char *name; const char *desc; const char *title;
    const char *arguments_schema_json;
    bt_mcp_prompt_get_fn get; void *user;
} bt_mcp_prompt_def_t;

/* consent 回调：返回 0 = 放行，非 0 = 拒绝 */
typedef int (*bt_mcp_consent_fn)(void *user, const char *tool_name,
                                 const bt_mcp_json_req_t *req);

typedef struct {
    const bt_mcp_tool_def_t     *tools;     uint16_t tools_count;
    const bt_mcp_resource_def_t *resources; uint16_t resources_count;
    const bt_mcp_prompt_def_t   *prompts;   uint16_t prompts_count;
    bt_mcp_consent_fn consent; void *consent_user;
} bt_mcp_server_cfg_t;
```

生命周期与入口：

```c
int  bt_mcp_server_init(bt_mcp_server_t *sv, bt_mcp_session_t *s,
                        const bt_mcp_server_cfg_t *cfg,
                        const char *fw_name, const char *fw_version);
/* 统一入口：单条消息、JSON-RPC batch（按协商版本分支）都在这里分派 */
int  bt_mcp_server_rx(bt_mcp_server_t *sv, const uint8_t *json, uint32_t len);
void bt_mcp_server_tick(bt_mcp_server_t *sv, uint32_t now_ms);
```

异步调用与进度：

```c
int  bt_mcp_server_complete(bt_mcp_call_t *call, const char *text, int is_error);
int  bt_mcp_call_progress(bt_mcp_call_t *call, uint64_t done, uint64_t total);
int  bt_mcp_call_progress_msg(bt_mcp_call_t *call, uint64_t done, uint64_t total,
                              const char *message);      /* v0.5：规范可选字段 */
int  bt_mcp_call_cancelled(const bt_mcp_call_t *call);
int  bt_mcp_call_set_meta(bt_mcp_call_t *call, const char *json, uint32_t len);
                            /* v0.5：声明结果级 _meta（"k":v 片段原文） */
```

富内容写入（工具回调内，v0.4）：

```c
int  bt_mcp_w_content_text(bt_mcp_writer_t *w, const char *text, uint32_t len);
int  bt_mcp_w_content_image(bt_mcp_writer_t *w, const char *b64, uint32_t b64len,
                            const char *mime);
int  bt_mcp_w_content_audio(bt_mcp_writer_t *w, const char *b64, uint32_t b64len,
                            const char *mime);
int  bt_mcp_w_content_resource(bt_mcp_writer_t *w, const char *uri,
                               const char *mime, const char *text, uint32_t len);
/* _meta 追加：name 是 _meta 内的键，json 是该键的 **JSON 片段原文**
 * （不额外加引号）：传 "\"0.5\"" 得到 "k":"0.5"，传 {"a":1} 得到 "k":{"a":1}。
 * 仅适用于**结果对象级** writer（如 prompts/get）；工具回调请用
 * bt_mcp_call_set_meta()（那边传的是整个 "k":v 片段）。 */
int  bt_mcp_w_meta(bt_mcp_writer_t *w, const char *name,
                   const char *json, uint32_t len);
```

下行通知与 server→client 请求：

```c
int  bt_mcp_server_notify(bt_mcp_server_t *sv, const char *method,
                          const char *params_json, uint32_t params_len);
int  bt_mcp_server_notify_resource_updated(bt_mcp_server_t *sv, const char *uri);
int  bt_mcp_server_notify_list_changed(bt_mcp_server_t *sv, const char *what);
int  bt_mcp_server_log(bt_mcp_server_t *sv, uint8_t level, const char *logger,
                       const char *text, uint32_t len);

typedef void (*bt_mcp_server_resp_fn)(void *user, int rc,
                                      const char *result_json, uint32_t result_len);
int  bt_mcp_server_request(bt_mcp_server_t *sv, const char *method,
                           const char *params_json, uint32_t params_len,
                           bt_mcp_server_resp_fn cb, void *user);
int  bt_mcp_server_inflight(const bt_mcp_server_t *sv);
```

多会话（v0.5）：

```c
typedef struct {
    bt_mcp_server_t *sv[BT_MCP_CFG_MAX_CONN];
    uint8_t          n;
} bt_mcp_server_pool_t;

void    bt_mcp_pool_init(bt_mcp_server_pool_t *pool);
int     bt_mcp_pool_add(bt_mcp_server_pool_t *pool, bt_mcp_server_t *sv);
int     bt_mcp_pool_rx(bt_mcp_server_pool_t *pool, bt_mcp_session_t *sess,
                       const uint8_t *json, uint32_t len);
void    bt_mcp_pool_tick(bt_mcp_server_pool_t *pool, uint32_t now_ms);
uint8_t bt_mcp_pool_count(const bt_mcp_server_pool_t *pool);
```

> `bt_mcp_server_t` 本身就是**每会话**上下文（`sess`/`initialized`/`calls[]`/
> `subs[]`/`log_level`/`proto`/batch 状态都在实例内，cfg 是值拷贝）。
> 因此多会话不需要拆分单实例逻辑，只需要按 session 找实例的分派器。
> 约束与单会话一致：**每个实例只被一个线程使用**。

### 7.4 L5 MCP client（`bt_mcp/mcp_client.h`）

```c
typedef struct {
    int         is_notification; int is_error; int32_t error_code;
    uint32_t    id;
    const char *method; uint16_t method_len;
    const char *result; uint32_t result_len;
} bt_mcp_client_event_t;
typedef void (*bt_mcp_client_cb_t)(void *user, const bt_mcp_client_event_t *ev);

int  bt_mcp_client_init(bt_mcp_client_t *c, bt_mcp_session_t *s,
                        bt_mcp_client_cb_t cb, void *user);
int  bt_mcp_client_initialize(bt_mcp_client_t *c, const char *client_name,
                              const char *client_version);
int  bt_mcp_client_request(bt_mcp_client_t *c, const char *method,
                           const char *params_json, uint32_t params_len,
                           uint32_t *out_id);
int  bt_mcp_client_list_tools(bt_mcp_client_t *c, const char *cursor,
                              uint32_t *out_id);
int  bt_mcp_client_call_tool(bt_mcp_client_t *c, const char *name,
                             const char *args_json, uint32_t args_len,
                             uint32_t *out_id);
int  bt_mcp_client_read_resource(bt_mcp_client_t *c, const char *uri,
                                 uint32_t *out_id);
int  bt_mcp_client_rx(bt_mcp_client_t *c, const uint8_t *json, uint32_t len);
int  bt_mcp_client_inflight(const bt_mcp_client_t *c);
void bt_mcp_client_tick(bt_mcp_client_t *c, uint32_t now_ms);
```

### 7.5 JSON / writer / trace（内部但对 port 可见）

```c
/* bt_mcp/json.h —— 零分配解析器 */
int  bt_mcp_json_parse(const char *src, uint32_t len, bt_mcp_json_req_t *out);
int  bt_mcp_json_parse_msg(const char *src, uint32_t len, bt_mcp_json_req_t *out);
int  bt_mcp_json_get_str(const bt_mcp_json_req_t*, const char*, const char**, uint16_t*);
int  bt_mcp_json_get_u32/i32/f64(...);
int  bt_mcp_json_get_raw(const bt_mcp_json_req_t*, const char*, const char**, uint32_t*);
int  bt_mcp_json_top_raw(const bt_mcp_json_req_t*, const char*, const char**, uint32_t*);
int  bt_mcp_json_get_sub(const bt_mcp_json_req_t*, const char*, bt_mcp_json_req_t*);
uint16_t bt_mcp_json_decode_str(const char *s, const char *e,
                                char *out, uint16_t cap, uint16_t *need);

/* bt_mcp/writer.h —— 零分配 JSON 写出 */
int bt_mcp_w_raw(bt_mcp_writer_t*, const char *data, uint32_t len);
int bt_mcp_w_str_n(w, const char *s, uint32_t n);
int bt_mcp_w_str_lit(w, const char *s);
int bt_mcp_w_key(w, const char *key);
int bt_mcp_w_kv_str / kv_raw / kv_u32 / kv_u64(...);
int bt_mcp_w_begin_result / begin_result_id / begin_error / begin_error_id(...);
int bt_mcp_w_end(w);

/* bt_mcp/trace.h —— 编译期可裁的调试钩子（BT_MCP_CFG_TRACE=0 时全部消失） */
typedef void (*bt_mcp_trace_fn)(int level, const char *what,
                                const void *data, uint16_t len);
void bt_mcp_trace_set(void *unused, bt_mcp_trace_fn fn);
void bt_mcp_trace(int level, const char *what, const void *data, uint16_t len);
```

**健壮性契约（v0.5 收紧）**：所有 writer 入口校验 `w`/`put`/`sink`，所有 JSON
getter 校验输出指针——传 NULL 一律返回 `E_ARGS`，**绝不崩溃**（嵌入式应
fail-closed）。v0.5 之前这两处都会解引用空指针。

### 7.6 驱动模型（裸机与 RTOS 同构）

```c
/* 裸机主循环 */
for (;;) {
    bt_mcp_framer_tick(&framer, now());
    bt_mcp_session_tick(&sess, now());
    bt_mcp_server_tick(&server, now());
    platform_run_bt_stack();        /* 产生 on_rx/on_conn → core 内部状态推进 */
    idle_sleep();
}
/* RTOS：单一协议任务把同样的三个 tick + 队列 drain 包成 while(1)。
   core 本身不含任何队列/信号量原语 —— 并发归一由 port 层完成。 */
```

### 7.7 配置（编译期，全部集中在 bt_mcp/config.h，无 vendor ifdef）

```c
/* ---- 容量（可被 BT_MCP_TIER 一键覆盖，§7.8；逐项仍可单独 #ifndef 覆盖）---- */
#define BT_MCP_CFG_SHARED_REASM     0      /* 1 = 单槽共享缓冲（v0.5，见 §4.3） */
#define BT_MCP_CFG_MAX_REASM        4      /* 重组槽数（共享模式下强制为 1） */
#define BT_MCP_CFG_MAX_MSG          8192   /* 单消息字节上限（≤ u16 可承载） */
#define BT_MCP_CFG_MAX_FRAG         255    /* 单消息分片数上限（u8 idx 决定） */
#define BT_MCP_CFG_FRAG_TOUT_MS     2000

/* ---- 不可靠包通道的消息级可靠（§5.4）---- */
#define BT_MCP_CFG_TX_SLOTS         2      /* 重传槽数 */
#define BT_MCP_CFG_TX_RETRY_BYTES   512    /* 普通 tx 的逐槽副本容量 */
#define BT_MCP_CFG_RX_FP_SLOTS      8      /* 接收去重指纹槽（≥ TX_SLOTS；v0.6: 4→8） */
#define BT_MCP_CFG_ACK_TOUT_MS      600    /* ACK 时限基准，也是 RTO 回落值 */
#define BT_MCP_CFG_MAX_RETRY        3      /* 每消息重传上限 */
#define BT_MCP_CFG_DEDUPE_MS        20000  /* 必须覆盖重传寿命（v0.7: 5000→20000） */
#define BT_MCP_CFG_RTO_MAX_MS       4800   /* 指数退避上限（v0.5） */
#define BT_MCP_CFG_CREDIT_REFRESH_MS 600   /* 空闲信用刷新周期（v0.6） */
#define BT_MCP_CFG_NACK_REPLAY_MAX   16    /* NACK 尾部重放一次最多几片（v0.7） */
#define BT_MCP_CFG_NACK_REPLAY_BUDGET_X 16 /* 单消息重放预算 = 分片数 × 该值（v0.7） */

/* ---- 安全（§6）---- */
#define BT_MCP_CFG_RESUME_MAX_FAIL  3
#define BT_MCP_CFG_RESUME_LOCK_MS   30000

/* ---- L5 ---- */
#define BT_MCP_CFG_PAGE_SIZE        8      /* tools/resources/prompts 每页条数 */
#define BT_MCP_CFG_MAX_PENDING      2      /* 并发 tools/call 槽 */
#define BT_MCP_CFG_IDSTR_MAX        32     /* 字符串 request id 复制上限 */
#define BT_MCP_CFG_KEY_MAX          40     /* JSON 键名比较缓冲 */
#define BT_MCP_CFG_JSON_MAX_DEPTH   6      /* 嵌套深度上限（防栈爆） */
#define BT_MCP_CFG_NOTIFY_BUF       512    /* 单条通知缓冲（≥192 有编译期断言） */
#define BT_MCP_CFG_MAX_SUBS         8      /* 同时订阅的资源数（v0.4） */
#define BT_MCP_CFG_MAX_S2C_REQ      2      /* server→client 在途请求（v0.4） */
#define BT_MCP_CFG_S2C_TOUT_MS      30000  /* S→C 请求超时（v0.4） */
#define BT_MCP_CFG_MAX_CONN         4      /* 一个 pool 的并发会话数（v0.5） */
#define BT_MCP_CFG_BATCH_BUF        4096   /* JSON-RPC batch 响应聚合（v0.5） */
#define BT_MCP_CFG_META_MAX         256    /* 单次调用可携带的 _meta 原文（v0.5） */

/* ---- 链路活性（v0.4，默认关闭 = 行为不变）---- */
#define BT_MCP_CFG_KEEPALIVE_MS     0      /* >0：空闲该毫秒数后主动 PING */
#define BT_MCP_CFG_IDLE_DEAD_MS     0      /* >0：超该毫秒无入站即判链路失效 */

/* ---- 可选链路校验 ---- */
#define BT_MCP_CFG_FCS16            0      /* 1 = hdr|FCS16|payload（§4.1） */

/* ---- 调试 ---- */
#define BT_MCP_CFG_TRACE            0      /* 1 = 编译进 trace 钩子（§7.5） */
```

**编译期一致性断言**（`config.h` 末尾，违反即编译失败，不是运行期才发现）：
`MAX_MSG ≤ 65535`、`MAX_REASM ∈ 1..8`、`TX_SLOTS ∈ 1..8`、`RX_FP_SLOTS ≥ TX_SLOTS`、
`MAX_FRAG ∈ 1..255`、`NOTIFY_BUF ≥ 192`、`MAX_FRAG × 255 ≥ MAX_MSG`、
`TX_RETRY_BYTES ≥ 32`（或 retry_max=0 显式关闭）。

（全静态：无 `BT_MCP_CFG_HEAP`。宿主机侧 mock/oracle 自行用动态内存，core 不用。）

### 7.8 内存档位（`bt_mcp/tier.h`）

`-DBT_MCP_TIER=BT_MCP_TIER_COMPACT|STANDARD|FULL` 一键选容量；线上字节完全一致，
只有单消息上限/槽数/分页大小不同。

**实测静态 RAM**（`sizeof(framer) + sizeof(session) + sizeof(server)`，
2026-11-11 v0.5 实测，含 v0.4/v0.5 新增的订阅表、S2C 在途槽、batch 聚合缓冲、
`_meta` 缓冲、多会话池字段）：

| 档位 | MAX_MSG | 重组槽 | framer | session | server | **合计** |
|---|---|---|---|---|---|---|
| COMPACT | 1024 | 2 | 3152 | 808 | 6624 | **10584 B** |
| STANDARD | 2048 | 4 | 10352 | 1320 | 7776 | **19448 B** |
| FULL | 8192 | 4 | 41072 | 1320 | 14048 | **56440 B** |

**开启 `BT_MCP_CFG_SHARED_REASM=1`（§4.3）后**：

| 档位 | framer | 合计 | 相对默认 |
|---|---|---|---|
| COMPACT | 2112 | **9544 B** | −1040 B（−10%） |
| STANDARD | 4160 | **13256 B** | −6192 B（−32%） |
| FULL | 16448 | **31816 B** | **−24624 B（−44%）** |

档位选择建议：单消息上限决定 `MAX_MSG`，`MAX_MSG × MAX_REASM` 决定接收缓冲
主体。RAM 紧张且同时只有一个大消息在途时，优先开共享模式。
（`tools/bench.c` 可复现上表；CI 里应把这三行做成基线，回归即失败——v0.6 N7。）

> 注：v0.4 及更早的文档给的是 4048/11760/42480，那是**只算 framer+session、
> 且当时的 server 结构更小**的数字。v0.4 新增订阅表/S2C 槽、v0.5 新增 batch
> 聚合与 `_meta` 后，含 server 的完整占用如上表。

## 8. Port 清单（协议核心之外的全部工作）

| port | 平台无关度 | 实现要点 |
|---|---|---|
| GATT | 任意 BLE 栈（vendor host/Zephyr/bluedroid/NimBLE…）：定义一个 3 特征服务（Ctrl notify+write / Dn notify / Up write-no-rsp），ATT_MTU−3 填 cap.mtu，**reliable=0**（开启 §5.4 重传）；栈回调转投协议上下文 | 唯一必选 |
| SPP | 任意 Classic 栈：SDP 注册 SPP、RFCOMM 通道，直接流通道（reliable=1、flow_ctrl=1）| 双模设备可选 |
| L2CAP CoC | 支持 LE Credit Based 的栈：`coc_port.h`，cap = PACKET + reliable=1 + flow_ctrl=1（只保留分片，免信用免重传）| 网关场景可选 |
| host-mock | POSIX：socketpair 仿真包/流通道 | CI/golden/fuzz 必备 |

SPP 与 GATT 之上协议核心**零代码差异**：唯一区别是 port 填的 `bt_mcp_chan_cap_t`。

## 10. 路线与验证

- M1 ✅：frame.c + host-mock + 独立 Rust oracle 做差分 + 独立 fuzz 引擎喂帧
  （截断/乱序/重复/失步流）；出口 = 双向字节 100% 一致、fuzz 零 crash。
- M2 ✅：session+mcp_server 闭环；出口 = initialize→tools/list→tools/call E2E + 单元检查。
- M3 ✅：GATT（包/不可靠）与 SPP（流/可靠）两套 port 同一份 core 背靠背对照（198 项 HIL）；
  断链注入/窗口压满/RESUME/去重/重传全覆盖。
- M4 ✅：CoC port 落地（`coc_port.h`）；差分矩阵扩到四套 harness。
- M5 ✅（协议侧）：开销基线实测（`tools/bench.c`，见下表）。待办：真实芯片移植示例
  （Zephyr/NimBLE）、手机 Bridge 代码、真实硬件上的功耗实测。

**v0.4/v0.5 的验证体系**（四层互补，每层都抓到过其他层发现不了的缺陷）：

| 层 | 手段 | 规模 | 能发现 | 看不见 |
|---|---|---|---|---|
| L1 | C ↔ 独立 Rust oracle 差分 | 1295 项（主 1045 + FCS16 配对 196 + 共享模式 54） | 两侧**不一致** | 两侧一起错 |
| L2 | 结构符合性（`test_conformance.c`） | 64 项 | 响应**不符合规范结构** | 规范之外的语义错 |
| L3 | golden transcript（人工按规范核对） | 20 条 | 字节**非预期漂移** | 规范理解本身错 |
| L4 | 官方 MCP SDK 互验 | 38 项 | 第三方**无法正确理解**输出 | 传输层之外的实现缺陷 |
| 附加 | 有损通道 / TSan / 覆盖率 / 故障注入 | 17 + 10 + 门禁 + 21 | ARQ 收敛性 / 并发安全 / 倒退 / 错误传播 | — |

实测战果（v0.4+v0.5 共 10 个真缺陷）：L1 抓到 2 个（重写引入的引号/花括号）、
L2 抓到 2 个（`level` 缺引号、内嵌资源多一个右花括号导致**非法 JSON**）、
L3 抓到 1 个（`ping` 不是空对象）、L4 抓到 1 个（transport 未重组分片）、
边界/故障注入抓到 4 个（writer sink、client 空指针、JSON getter 空指针、
client 模块级缓冲）。**每一个都是其他层发现不了的。**

### 10.1 开销基线（`tools/bench.c` 实测，非估算）

包通道分片开销（512B MCP 消息）：

| MTU | 分片 | 线缆字节 | 开销 | 放大 |
|---|---|---|---|---|
| 23 | 31 | 698 | +36.3% | 1.36× |
| 53 | 11 | 578 | +12.9% | 1.13× |
| 100 | 6 | 548 | +7.0% | 1.07× |
| 185 | 3 | 530 | +3.5% | 1.04× |
| 244 | 3 | 530 | +3.5% | 1.04× |

流通道（SPP/UART）：整条消息单帧，固定 **+6B**（与消息长度无关；MTU 无关）。
可靠性控制帧开销（不可靠包通道，MTU=244）：消费 128B 请求 + 发 512B 响应时，
控制帧 2 帧 / 17B，占链路总字节 **3.1%**（即每条被消费消息约 +8.5B）。

工程含义：**MTU 是首要旋钮**——ATT_MTU 23 的默认档放大 1.36×，而交换到 244
后降到 1.04×。所以 GATT port 应尽早做 MTU 交换；若不能，可考虑用流通道
（SPP/CoC）承载大 payload 类工具。
