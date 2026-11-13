//! bt-mcp-oracle：独立 Rust 参照实现（差分 oracle），与 C core 逐字节对齐。
//! CLI 模式（stdin→stdout 二进制）：
//!   encode   : u32 kind | u16 mtu | u32 len | payload → 帧字节
//!   decode-p : [u16 片长 + 片]* → [u32 kind | u32 len | msg]*
//!   decode-s : 流字节           → 同 decode-p
//!   session  : u8 chan | u16 mtu | 指令序列（L3+L4）
//!   mcp      : u8 chan | u16 mtu | u8 ntools | 工具* | 0x07 MSG*
//!   e2e      : 同上信封 + client/server 全栈对话指令
//! 帧头（小端）: 0xB7 | flags | seq | idx | u16 len；flags: bit0 LAST bit1 CHUNK bit2 CTRL
use std::io::{Read, Write};

const MAGIC: u8 = 0xB7;
const HDR: usize = 6;

/* FCS16（可选）：线序 hdr | fcs(2, LE) | payload。由环境变量 BT_MCP_FCS16=1 开启，
 * 必须与 C 侧 -DBT_MCP_CFG_FCS16=1 的构建配对使用（差分只比同配置）。 */
static FCS16_ON: std::sync::OnceLock<bool> = std::sync::OnceLock::new();

fn fcs16_on() -> bool {
    *FCS16_ON.get_or_init(|| std::env::var("BT_MCP_FCS16").map(|v| v == "1")
        .unwrap_or(false))
}

/* CRC16-CCITT，与 C 的 crc_cont 逐位一致 */
fn crc16_cont(p: &[u8], mut crc: u16) -> u16 {
    for &b in p {
        crc ^= (b as u16) << 8;
        for _ in 0..8 {
            crc = if crc & 0x8000 != 0 { (crc << 1) ^ 0x1021 } else { crc << 1 };
        }
    }
    crc
}
const FLAG_LAST: u8 = 0x01;
const FLAG_CHUNK: u8 = 0x02;
const FLAG_CTRL: u8 = 0x04;
const MAX_FRAG: usize = 255;  /* == BT_MCP_CFG_MAX_FRAG（u8 idx 上限） */
/* v0.9.1：一次 NACK 最多报几个缺片（== BT_MCP_CFG_NACK_MAX_IDX） */
const NACK_MAX_IDX: usize = 16;
const MAX_MSG: usize = 8192;

const E_AGAIN: i32 = -1;
const E_ARGS: i32 = -2;
const E_PENDING: i32 = -9;   /* 与 C 的 BT_MCP_E_PENDING 对应（oracle 无异步工具） */
const E_STATE: i32 = -3;
const E_TOOBIG: i32 = -4;
const E_NOMEM: i32 = -5;
const E_SYNC: i32 = -6;
const E_PARSE: i32 = -7;
const E_LOCKED: i32 = -11;

const K_MCP: u8 = 0;
const K_CTRL: u8 = 1;
const C_CREDIT: u8 = 1;
const C_PING: u8 = 2;
const C_RESUME_REQ: u8 = 3;
const C_RESUME_OK: u8 = 4;
const C_RESUME_REJ: u8 = 5;
const C_ACK: u8 = 6;
const C_NACK: u8 = 7;

const CTRL_MAX: usize = 255;
/* v0.7（A2）：与 C 同——NACK 尾部重放上限与单消息重放预算 */
const NACK_REPLAY_MAX: u8 = 16;
const NACK_REPLAY_BUDGET_X: u16 = 16;
/* 重组槽数：默认 4；C 侧用 -DBT_MCP_CFG_SHARED_REASM=1 时是 1，
 * 差分必须同配置，因此这里也允许用环境变量覆盖（与 FCS16 同模式）。 */
static MAX_REASM_V: std::sync::OnceLock<usize> = std::sync::OnceLock::new();

fn max_reasm() -> usize {
    *MAX_REASM_V.get_or_init(|| std::env::var("BT_MCP_MAX_REASM")
        .ok().and_then(|v| v.parse().ok()).unwrap_or(4))
}
const GAP_WAIT_MAX: u8 = 4;
const FRAG_TOUT: u32 = 2000;
const RESUME_TOUT: u32 = 5000;
const REQ_TOUT: u32 = 30000;
const TX_SLOTS: usize = 2;
const TX_RETRY_BYTES: usize = 512;
const RX_FP_SLOTS: usize = 8;   /* v0.6：与 C 的 BT_MCP_CFG_RX_FP_SLOTS 对齐 */
const DEDUPE_MS: u32 = 20000;   /* v0.7：覆盖完整重传寿命（与 C 同） */
const ACK_TOUT: u32 = 600;
const RTO_MAX: u32 = 4800;         /* v0.5 V9：指数退避上限 */
const CREDIT_REFRESH: u32 = 600;   /* v0.6：空闲信用刷新周期 */
const MAX_RETRY: u8 = 3;
const RESUME_MAX_FAIL: u8 = 3;
const RESUME_LOCK_MS: u32 = 30000;
const PAGE_SIZE: usize = 8;
const KEY_MAX: usize = 40;
const IDSTR_MAX: usize = 32;
const NOTIFY_BUF: usize = 512;

fn crc32_of(data: &[u8]) -> u32 {
    let mut c: u32 = 0xFFFFFFFF;
    for &b in data {
        c ^= b as u32;
        for _ in 0..8 {
            c = (c >> 1) ^ (0xEDB88320u32 & (0u32.wrapping_sub(c & 1)));
        }
    }
    !c
}

/* ---------------- 帧层：encode / decode（纯 L3，无会话） ---------------- */

fn encode(kind: u32, mtu: u16, payload: &[u8]) -> Option<Vec<u8>> {
    if payload.len() > MAX_MSG { return None; }
    let fcs = if fcs16_on() { 2 } else { 0 };
    let chunk = if mtu == 0 {
        MAX_MSG
    } else {
        let m = mtu.min(261) as usize;
        if m <= HDR + fcs { return None; }
        m - HDR - fcs
    };
    if kind == 1 && payload.len() > CTRL_MAX { return None; }
    let nfrag = if payload.is_empty() { 1 } else { payload.len().div_ceil(chunk) };
    if kind == 1 && nfrag > 1 { return None; }
    if nfrag > MAX_FRAG { return None; }
    let mut out = Vec::with_capacity(payload.len() + nfrag * HDR);
    for idx in 0..nfrag {
        let done = idx * chunk;
        let plen = (payload.len() - done).min(chunk);
        let mut flags = 0u8;
        if nfrag > 1 { flags |= FLAG_CHUNK; }
        if idx == nfrag - 1 { flags |= FLAG_LAST; }
        if kind == 1 { flags |= FLAG_CTRL; }
        let hdr = [MAGIC, flags, 0, idx as u8,
                   (plen & 0xFF) as u8, (plen >> 8) as u8];
        out.extend_from_slice(&hdr);
        if fcs != 0 {
            let c = crc16_cont(&hdr, 0xFFFF);
            let c = crc16_cont(&payload[done..done + plen], c);
            out.extend_from_slice(&[(c & 0xFF) as u8, (c >> 8) as u8]);
        }
        out.extend_from_slice(&payload[done..done + plen]);
    }
    Some(out)
}

struct DecSlot { in_use: bool, msg_seq: u8, gap_wait: u8,
                 chunk: u16, len_total: u16, last_len: u16, last_idx: u8,
                 have_last: bool, got: [u8; (MAX_FRAG + 7) / 8], buf: Vec<u8> }

fn decode_packet(input: &[u8]) -> Option<Vec<(u8, Vec<u8>)>> {
    let mut msgs = Vec::new();
    let mut slots: Vec<DecSlot> = Vec::new();
    let mut off = 0usize;
    while off + 2 <= input.len() {
        let flen = u16::from_le_bytes([input[off], input[off + 1]]) as usize;
        off += 2;
        if off + flen > input.len() { return None; }
        let frag = &input[off..off + flen];
        off += flen;
        if frag.len() < HDR { continue; }
        let (flags, seq, idx) = (frag[1], frag[2], frag[3]);
        let plen = u16::from_le_bytes([frag[4], frag[5]]) as usize;
        let fcs = if fcs16_on() { 2 } else { 0 };
        if frag.len() < HDR + fcs + plen || idx as usize >= MAX_FRAG { continue; }
        if fcs != 0 {
            let want = crc16_cont(&frag[..HDR], 0xFFFF);
            let want = crc16_cont(&frag[HDR + 2..HDR + 2 + plen], want);
            if want != u16::from_le_bytes([frag[HDR], frag[HDR + 1]]) { continue; }
        }
        let pl = &frag[HDR + fcs..HDR + fcs + plen];
        let ctrl = flags & FLAG_CTRL != 0;
        if flags & FLAG_CHUNK == 0 {
            msgs.push((ctrl as u8, pl.to_vec()));
            continue;
        }
        let pos = match slots.iter().position(|s| s.in_use && s.msg_seq == seq) {
            Some(p) => p,
            None => {
                if slots.len() >= max_reasm() { continue; }
                slots.push(DecSlot { in_use: true, msg_seq: seq, gap_wait: 0,
                                     chunk: 0, len_total: 0, last_len: 0,
                                     last_idx: 0, have_last: false,
                                     got: [0u8; (MAX_FRAG + 7) / 8],
                                     buf: Vec::new() });
                slots.len() - 1
            }
        };
        let s = &mut slots[pos];
        /* v0.9.1（与 C 同）：乱序缓存 + 位图。解码侧无法知道发送方 MTU，
         * 因此基准从分片自身推导：非 LAST 片长度恒等于 chunk。 */
        if !s.in_use || (s.len_total == 0 && !s.have_last && s.chunk == 0
                         && s.got[0] == 0) {
            s.got = [0u8; (MAX_FRAG + 7) / 8];
            s.have_last = false; s.last_len = 0; s.last_idx = 0;
            s.len_total = 0;
            let cc = if flags & FLAG_LAST == 0 { plen as u16 } else { 0 };
            s.chunk = cc;                       /* 0 = 未知 */
        }
        if s.chunk == 0 { continue; }            /* 基准未知：等下一片 */
        let chunk = s.chunk as usize;
        let off = (idx as usize).saturating_mul(chunk);
        if off + plen > MAX_MSG { s.in_use = false; return None; }
        let bit = 1u8 << (idx & 7);
        let already = (s.got[(idx >> 3) as usize] & bit) != 0;
        if !already {
            s.buf.resize(MAX_MSG, 0);
            s.buf[off..off + plen].copy_from_slice(pl);
            s.got[(idx >> 3) as usize] |= bit;
            s.len_total = s.len_total.wrapping_add(plen as u16);
        }
        if flags & FLAG_LAST != 0 {
            s.have_last = true; s.last_idx = idx; s.last_len = plen as u16;
        }
        let mut hi = idx;
        if s.have_last && s.last_idx > hi { hi = s.last_idx; }
        let mut complete = true;
        for i in 0..=hi {
            if (s.got[(i >> 3) as usize] & (1u8 << (i & 7))) == 0 { complete = false; break; }
        }
        if complete && s.have_last {
            let total = (s.last_idx as usize) * chunk + s.last_len as usize;
            let msg = s.buf[..total].to_vec();
            let k = ctrl as u8;
            s.in_use = false;
            msgs.push((k, msg));
        }
    }
    Some(msgs)
}

fn decode_stream(input: &[u8]) -> Option<Vec<(u8, Vec<u8>)>> {
    let mut msgs = Vec::new();
    let mut off = 0usize;
    while off < input.len() {
        if input[off] != MAGIC { off += 1; continue; }
        if off + HDR > input.len() { break; }
        let (flags, plen) = (input[off + 1],
            u16::from_le_bytes([input[off + 4], input[off + 5]]) as usize);
        if (flags & FLAG_CTRL != 0 && plen > CTRL_MAX) || plen > MAX_MSG
            || flags & FLAG_CHUNK != 0 { return None; }
        let fcs = if fcs16_on() { 2 } else { 0 };
        if off + HDR + fcs + plen > input.len() { break; }
        if fcs != 0 {
            let want = crc16_cont(&input[off..off + HDR], 0xFFFF);
            let want = crc16_cont(&input[off + HDR + 2..off + HDR + 2 + plen], want);
            if want != u16::from_le_bytes([input[off + HDR], input[off + HDR + 1]])
                { return None; }
        }
        msgs.push(((flags & FLAG_CTRL != 0) as u8,
                   input[off + HDR + fcs..off + HDR + fcs + plen].to_vec()));
        off += HDR + fcs + plen;
    }
    Some(msgs)
}

/* ---------------- 会话层（L3+L4） ---------------- */

#[derive(PartialEq, Clone, Copy)]
enum SState { Idle, LinkUp, Ready }

enum RxPre { Rc(i32), Deliver }

#[derive(Clone)]
struct SSlot {
    /* v0.9.1（A2 根因修复，与 C 同）：乱序缓存 + 缺片位图。
     * 偏移 = idx * chunk（非末尾片长度恒为 chunk）。 */
    in_use: bool, ctrl: bool, msg_seq: u8, gap_wait: u8,
    chunk: u16, len_total: u16, last_len: u16, last_idx: u8, have_last: bool,
    nack_len: u16, t_first: u32, t_last: u32,
    got: [u8; (MAX_FRAG + 7) / 8], buf: Vec<u8>,
}

#[derive(Clone)]
struct TxSlot { valid: bool, seq: u8, n: u8, len: u16, t0: u32,
                msg_no: u32, nack_replay: u16, buf: Vec<u8> }

#[derive(Clone, Copy)]
struct RxFp { valid: bool, seq: u8, len: u16, crc: u32, t: u32, msg_no: u32 }

struct Sess {
    packet: bool, mtu: u16,
    /* v0.7：MTU 热更新后的当前值（0 表示回退到 mtu） */
    mtu_cur: u16,
    reliable: bool, flow_ctrl: bool,
    /* L3 */
    tx_seq: u8, fnow: u32,
    nack_pending: bool, nack_seq: u8,
    nack_idx: [u8; NACK_MAX_IDX], nack_count: u8,
    dead_seq: u8, dead_valid: bool,
    slots: Vec<SSlot>,
    s_hdr: [u8; 6], s_hdr_got: usize, s_len_got: u16,
    s_fcs: [u8; 2], s_fcs_got: usize,
    s_active: bool, s_cur: Vec<u8>,
    /* L4 */
    state: SState, credits: u32, tx_window: u32,
    resume_token: u32, resume_pending: bool, resume_t0: u32,
    resume_fail: u8, resume_locked: bool, resume_lock_t0: u32, resume_lock_ms: u32,
    req_inflight: bool, req_t0: u32,
    ping_pending: bool, ping_epoch: u8, ping_t0: u32, rtt_ms: u32, rtt_valid: bool,
    txslot: Vec<TxSlot>, txslot_next: usize,
    rx_fp: Vec<RxFp>, rx_fp_next: usize, rx_msg_no: u32,
    tx_msg_no: u32, stat_tx_stale_drop: u32,
    consumed: u16, peer_consumed: u16,
    retry_max: u8, ack_tout: u32, rto_cur: u32, rto_base: u32,
    last_ctrl_ms: u32,
    stat_tx_retransmit: u32, stat_tx_dropped: u32, stat_rx_dup: u32,
    now_ms: u32,
    /* 差分输出累积 */
    wire: Vec<u8>,
    msgs: Vec<(i32, Vec<u8>)>,
}

impl Sess {
    fn new(packet: bool, mtu: u16) -> Self {
        Sess {
            packet, mtu,
            /* 与 mock 的 chan_cap 一致：GATT 包通道不可靠、无通道流控；
               流通道有序可靠且自带流控 */
            reliable: !packet, flow_ctrl: !packet,
            tx_seq: 0, fnow: 0,
            nack_pending: false, nack_seq: 0, nack_idx: [0; NACK_MAX_IDX], nack_count: 0,
            dead_seq: 0, dead_valid: false,
            slots: (0..max_reasm()).map(|_| SSlot {
                in_use: false, ctrl: false, msg_seq: 0, gap_wait: 0, chunk: 0,
                len_total: 0, last_len: 0, last_idx: 0, have_last: false,
                nack_len: 0, got: [0u8; (MAX_FRAG + 7) / 8],
                t_first: 0, t_last: 0, buf: vec![0; MAX_MSG],
            }).collect(),
            s_hdr: [0; 6], s_hdr_got: 0, s_len_got: 0,
            s_fcs: [0; 2], s_fcs_got: 0,
            s_active: false, s_cur: vec![0; MAX_MSG],
            state: SState::Idle, credits: 0, tx_window: 4,
            resume_token: 0, resume_pending: false, resume_t0: 0,
            resume_fail: 0, resume_locked: false, resume_lock_t0: 0, resume_lock_ms: 0,
            req_inflight: false, req_t0: 0,
            ping_pending: false, ping_epoch: 0, ping_t0: 0, rtt_ms: 0, rtt_valid: false,
            txslot: (0..TX_SLOTS).map(|_| TxSlot { valid: false, seq: 0, n: 0, msg_no: 0, nack_replay: 0,
                len: 0, t0: 0, buf: Vec::new() }).collect(),
            txslot_next: 0,
            rx_fp: vec![RxFp { valid: false, seq: 0, len: 0, crc: 0, t: 0, msg_no: 0 };
                        RX_FP_SLOTS],
            rx_fp_next: 0, rx_msg_no: 0, tx_msg_no: 0, stat_tx_stale_drop: 0,
            consumed: 0, peer_consumed: 0,
            retry_max: MAX_RETRY, ack_tout: ACK_TOUT,
            rto_cur: ACK_TOUT, rto_base: ACK_TOUT,
            last_ctrl_ms: 0,
            stat_tx_retransmit: 0, stat_tx_dropped: 0, stat_rx_dup: 0,
            now_ms: 0,
            wire: Vec::new(), msgs: Vec::new(), mtu_cur: 0,
        }
    }

    fn eff_mtu(&self) -> u16 { if self.mtu_cur != 0 { self.mtu_cur } else { self.mtu } }
    fn chunk(&self) -> usize {
        if !self.packet { return MAX_MSG; }
        let m = self.eff_mtu().min(261) as usize;
        let fcs = if fcs16_on() { 2 } else { 0 };
        if m <= HDR + fcs { return 0; }
        m - HDR - fcs
    }
    fn credit_on(&self) -> bool { self.packet && !self.flow_ctrl }
    fn unreliable(&self) -> bool { self.packet && !self.reliable }

    fn alloc_seq(&mut self) -> u8 {
        let s = self.tx_seq;
        self.tx_seq = self.tx_seq.wrapping_add(1);
        s
    }

    /* 以指定 seq 发整条消息（判例 #1：seq 为消息键，重传不推进 tx_seq） */
    /* 一条 len 字节消息在当前 MTU 下会被切成几片（与 C 的 frag_count 一致） */
    fn frag_count(&self, len: usize) -> u8 {
        if self.packet {
            let mtu = self.eff_mtu();
            let mut chunk = if mtu > 261 { 261u32 } else { mtu as u32 };
            let ov = HDR as u32 + if fcs16_on() { 2 } else { 0 };
            if chunk <= ov { return 1; }
            chunk -= ov;
            let n = if len == 0 { 1 } else { (len as u32 + chunk - 1) / chunk };
            return n.min(255) as u8;
        }
        1
    }

    fn frame_seq(&mut self, kind: u8, seq: u8, payload: &[u8]) -> i32 {
        let chunk = self.chunk();
        if chunk == 0 { return E_STATE; }
        if payload.len() > MAX_MSG { return E_TOOBIG; }
        if kind == K_CTRL && payload.len() > CTRL_MAX { return E_TOOBIG; }
        let nfrag = if payload.is_empty() { 1 } else { payload.len().div_ceil(chunk) };
        if kind == K_CTRL && nfrag > 1 { return E_TOOBIG; }
        if nfrag > MAX_FRAG { return E_TOOBIG; }
        for idx in 0..nfrag {
            let done = idx * chunk;
            let plen = (payload.len() - done).min(chunk);
            let mut flags = if kind == K_CTRL { FLAG_CTRL } else { 0 };
            if nfrag > 1 { flags |= FLAG_CHUNK; }
            if idx == nfrag - 1 { flags |= FLAG_LAST; }
            let mut hdr = [MAGIC, flags, seq, idx as u8,
                           plen as u8, (plen >> 8) as u8];
            if fcs16_on() {
                let c = crc16_cont(&hdr, 0xFFFF);
                let c = crc16_cont(&payload[done..done + plen], c);
                hdr = hdr;  /* 头本身不变 */
                self.wire.extend_from_slice(&hdr);
                self.wire.extend_from_slice(&[(c & 0xFF) as u8, (c >> 8) as u8]);
                self.wire.extend_from_slice(&payload[done..done + plen]);
                continue;
            }
            self.wire.extend_from_slice(&hdr);
            self.wire.extend_from_slice(&payload[done..done + plen]);
        }
        0
    }

    fn frame_idx(&mut self, kind: u8, seq: u8, idx: u8, payload: &[u8]) -> i32 {
        let chunk = self.chunk();
        if chunk == 0 { return E_STATE; }
        if payload.len() > MAX_MSG { return E_TOOBIG; }
        if kind == K_CTRL && payload.len() > CTRL_MAX { return E_TOOBIG; }
        let nfrag = if payload.is_empty() { 1 } else { payload.len().div_ceil(chunk) };
        if kind == K_CTRL && nfrag > 1 { return E_TOOBIG; }
        if nfrag > MAX_FRAG { return E_TOOBIG; }
        if idx as usize >= nfrag { return -2; }
        let done = idx as usize * chunk;
        let plen = (payload.len() - done).min(chunk);
        let mut flags = if kind == K_CTRL { FLAG_CTRL } else { 0 };
        if nfrag > 1 { flags |= FLAG_CHUNK; }
        if idx as usize == nfrag - 1 { flags |= FLAG_LAST; }
        let hdr = [MAGIC, flags, seq, idx, plen as u8, (plen >> 8) as u8];
        self.wire.extend_from_slice(&hdr);
        if fcs16_on() {
            let c = crc16_cont(&hdr, 0xFFFF);
            let c = crc16_cont(&payload[done..done + plen], c);
            self.wire.extend_from_slice(&[(c & 0xFF) as u8, (c >> 8) as u8]);
        }
        self.wire.extend_from_slice(&payload[done..done + plen]);
        0
    }

    fn send_mcp(&mut self, kind: u8, payload: &[u8]) -> i32 {
        let seq = self.alloc_seq();
        self.frame_seq(kind, seq, payload)
    }

    fn feed_packet(&mut self, frag: &[u8],
                   msgs: &mut Vec<(u8, u8, Vec<u8>)>) -> i32 {
        if frag.len() < HDR { return E_SYNC; }
        let (flags, seq, idx) = (frag[1], frag[2], frag[3]);
        let plen = u16::from_le_bytes([frag[4], frag[5]]) as usize;
        let fcs = if fcs16_on() { 2 } else { 0 };
        if frag.len() < HDR + fcs + plen { return E_SYNC; }
        if idx as usize >= MAX_FRAG { return E_SYNC; }
        if fcs != 0 {
            let want = crc16_cont(&frag[..HDR], 0xFFFF);
            let want = crc16_cont(&frag[HDR + 2..HDR + 2 + plen], want);
            let got = u16::from_le_bytes([frag[HDR], frag[HDR + 1]]);
            if want != got { return E_SYNC; }
        }
        let pl = &frag[HDR + fcs..HDR + fcs + plen];
        let ctrl = flags & FLAG_CTRL != 0;
        if flags & FLAG_CHUNK == 0 {
            msgs.push((if ctrl { K_CTRL } else { K_MCP }, seq, pl.to_vec()));
            return 0;
        }
        if self.dead_valid && self.dead_seq == seq { return 0; }
        let pos = match self.slots.iter().position(|s| s.in_use && s.msg_seq == seq) {
            Some(p) => p,
            None => match self.slots.iter().position(|s| !s.in_use) {
                Some(p) => {
                    let s = &mut self.slots[p];
                    s.in_use = true; s.ctrl = false; s.msg_seq = seq;
                    s.gap_wait = 0; s.chunk = 0; s.len_total = 0;
                    s.have_last = false; s.last_len = 0; s.nack_len = 0;
                    s.got = [0u8; (MAX_FRAG + 7) / 8];
                    s.t_first = 0; s.t_last = 0;
                    p
                }
                None => return E_NOMEM,
            },
        };
        {
            let frag_chunk = self.chunk();
        let s = &mut self.slots[pos];
            /* v0.9.1（A2 根因修复，与 C 逐分支同）：乱序缓存 + 位图 */
            if s.len_total == 0 && !s.have_last && s.chunk == 0 && s.got[0] == 0 {
                s.ctrl = ctrl;
                s.t_first = self.fnow;
                s.got = [0u8; (MAX_FRAG + 7) / 8];
                s.have_last = false;
                s.last_len = 0;
                s.len_total = 0;
                s.gap_wait = 0;
                s.nack_len = 0;
                /* 与 C 同：非 LAST 片长度恒等于 chunk（最权威）；
                 * LAST 片较短，退回 MTU 推导值；都没有则记 0 = 未知。 */
                let cc = if flags & FLAG_LAST == 0 { plen as u16 } else { 0 };
                let cc = if cc == 0 { frag_chunk as u16 } else { cc };
                s.chunk = cc;
            }
            if s.chunk == 0 { return 0; }   /* 基准未知：丢弃，等 ARQ 重发 */
            let chunk = s.chunk as usize;
            let off = (idx as usize).saturating_mul(chunk);
            if off + plen > MAX_MSG {
                s.in_use = false;
                return E_TOOBIG;
            }
            let bit = 1u8 << (idx & 7);
            let already = (s.got[(idx >> 3) as usize] & bit) != 0;
            if !already {
                s.buf.resize(MAX_MSG, 0);
                s.buf[off..off + plen].copy_from_slice(pl);
                s.got[(idx >> 3) as usize] |= bit;
                s.len_total = s.len_total.wrapping_add(plen as u16);
                s.t_last = self.fnow;
            } else {
                s.t_last = self.fnow;
            }
            self.dead_valid = false;
            if flags & FLAG_LAST != 0 {
                s.have_last = true;
                s.last_idx = idx;
                s.last_len = plen as u16;
            }
            /* 缺片统计 */
            let mut hi = idx;
            if s.have_last && s.last_idx > hi { hi = s.last_idx; }
            let mut missing: [u8; NACK_MAX_IDX] = [0; NACK_MAX_IDX];
            let mut nmiss = 0usize;
            let mut total_missing = 0usize;
            for i in 0..=hi {
                if (s.got[(i >> 3) as usize] & (1u8 << (i & 7))) != 0 { continue; }
                total_missing += 1;
                if nmiss < NACK_MAX_IDX { missing[nmiss] = i; nmiss += 1; }
            }
            let len_total = s.len_total;
            if total_missing > 0 {
                if !self.nack_pending {
                    /* gap_wait = 连续无进展的 NACK 轮数（与 C 同语义） */
                    if len_total == s.nack_len {
                        s.gap_wait += 1;
                        if s.gap_wait > GAP_WAIT_MAX {
                            s.in_use = false;
                            self.dead_valid = true;
                            self.dead_seq = seq;
                            return E_SYNC;
                        }
                    } else {
                        s.gap_wait = 0;
                        s.nack_len = len_total;
                    }
                    self.nack_pending = true;
                    self.nack_seq = seq;
                    self.nack_count = nmiss as u8;
                    self.nack_idx = missing;
                }
            } else if s.have_last {
                let total = (s.last_idx as usize) * chunk + s.last_len as usize;
                let msg = s.buf[..total].to_vec();
                let k = if s.ctrl { K_CTRL } else { K_MCP };
                s.in_use = false;
                s.gap_wait = 0;
                msgs.push((k, seq, msg));
            }
        }
        0
    }

    fn take_nack(&mut self) -> Option<(u8, Vec<u8>)> {
        if !self.nack_pending { return None; }
        self.nack_pending = false;
        let n = self.nack_count as usize;
        Some((self.nack_seq, self.nack_idx[..n].to_vec()))
    }

    fn feed_stream(&mut self, data: &[u8], msgs: &mut Vec<(u8, u8, Vec<u8>)>) -> i32 {
        let mut data = data;
        while !data.is_empty() {
            if !self.s_active {
                while !data.is_empty() && self.s_hdr_got < HDR {
                    self.s_hdr[self.s_hdr_got] = data[0];
                    self.s_hdr_got += 1;
                    data = &data[1..];
                }
                if self.s_hdr_got < HDR { return 0; }
                if self.s_hdr[0] != MAGIC {
                    let mut i = 1;
                    while i < HDR && self.s_hdr[i] != MAGIC { i += 1; }
                    self.s_hdr.copy_within(i.., 0);
                    self.s_hdr_got = HDR - i;
                    continue;
                }
                self.s_len_got = 0;
                self.s_fcs_got = 0;
                self.s_active = true;
            }
            let flags = self.s_hdr[1];
            let seq = self.s_hdr[2];
            let plen = u16::from_le_bytes([self.s_hdr[4], self.s_hdr[5]]) as usize;
            /* 判例同 C：三处畸形在「读载荷之前」判定并复位解析器
             * （CHUNKED 在流通道上本身违规，无需等数据到齐） */
            if (flags & FLAG_CTRL != 0 && plen > CTRL_MAX) {
                self.s_active = false; self.s_hdr_got = 0; self.s_len_got = 0;
                return E_SYNC;
            }
            if plen > MAX_MSG || self.s_len_got as usize + plen > MAX_MSG {
                self.s_active = false; self.s_hdr_got = 0; self.s_len_got = 0;
                return E_SYNC;
            }
            if flags & FLAG_CHUNK != 0 {
                self.s_active = false; self.s_hdr_got = 0; self.s_len_got = 0;
                return E_SYNC;
            }
            if fcs16_on() {
                while self.s_fcs_got < 2 && !data.is_empty() {
                    self.s_fcs[self.s_fcs_got] = data[0];
                    self.s_fcs_got += 1;
                    data = &data[1..];
                }
                if self.s_fcs_got < 2 { return 0; }
            }
            let mut take = plen - self.s_len_got as usize;
            if take > data.len() { take = data.len(); }
            self.s_cur[self.s_len_got as usize..self.s_len_got as usize + take]
                .copy_from_slice(&data[..take]);
            self.s_len_got += take as u16;
            data = &data[take..];
            if (self.s_len_got as usize) < plen { return 0; }
            if fcs16_on() {
                let want = crc16_cont(&self.s_hdr[..HDR], 0xFFFF);
                let want = crc16_cont(&self.s_cur[..plen], want);
                let got = u16::from_le_bytes([self.s_fcs[0], self.s_fcs[1]]);
                if want != got {
                    self.s_active = false; self.s_hdr_got = 0;
                    self.s_len_got = 0; self.s_fcs_got = 0;
                    return E_SYNC;
                }
            }
            let msg = self.s_cur[..plen].to_vec();
            msgs.push((if flags & FLAG_CTRL != 0 { K_CTRL } else { K_MCP }, seq, msg));
            self.s_active = false; self.s_hdr_got = 0; self.s_len_got = 0;
        }
        0
    }

    fn clear_reliability(&mut self) {
        for s in self.txslot.iter_mut() { s.valid = false; }
        for f in self.rx_fp.iter_mut() { f.valid = false; }
        self.txslot_next = 0;
        self.rx_fp_next = 0;
        self.rx_msg_no = 0;
        self.consumed = 0;
        self.peer_consumed = 0;
        for s in self.slots.iter_mut() { s.in_use = false; s.gap_wait = 0; }
        self.s_hdr_got = 0; self.s_len_got = 0; self.s_active = false;
        self.nack_pending = false;
    }

    fn link_up(&mut self, token: u32) {
        self.state = SState::LinkUp;
        self.credits = self.tx_window;
        self.resume_pending = false;
        self.req_inflight = false;
        self.resume_token = token;
        self.ping_pending = false;
        self.clear_reliability();
    }
    fn link_down(&mut self, bond: bool) {
        self.state = SState::Idle;
        self.req_inflight = false;
        self.resume_pending = false;
        self.ping_pending = false;
        if !bond { self.resume_token = 0; }
        self.clear_reliability();
    }
    fn mark_ready(&mut self) {
        if self.state == SState::LinkUp { self.state = SState::Ready; }
    }

    /* ---- 重传槽 ---- */
    fn slot_by_seq(&mut self, seq: u8) -> Option<usize> {
        self.txslot.iter().position(|s| s.valid && s.seq == seq)
    }
    fn slot_claim(&mut self) -> usize {
        if let Some(p) = self.txslot.iter().position(|s| !s.valid) { return p; }
        let p = self.txslot_next % TX_SLOTS;
        self.txslot_next += 1;
        p
    }

    /* ---- 去重指纹 ---- */
    fn rx_dup_check(&mut self, seq: u8, pl: &[u8]) -> bool {
        let crc = crc32_of(pl);
        for f in self.rx_fp.iter() {
            if !f.valid { continue; }
            if f.seq != seq || f.len != pl.len() as u16 || f.crc != crc { continue; }
            if self.now_ms.wrapping_sub(f.t) > DEDUPE_MS { continue; }
            if self.rx_msg_no.wrapping_sub(f.msg_no) > 8 { continue; }
            return true;
        }
        let p = self.rx_fp_next % RX_FP_SLOTS;
        self.rx_fp_next += 1;
        self.rx_fp[p] = RxFp { valid: true, seq, len: pl.len() as u16, crc,
                               t: self.now_ms, msg_no: self.rx_msg_no };
        self.rx_msg_no = self.rx_msg_no.wrapping_add(1);
        false
    }

    /* ---- 控制帧 ---- */
    fn ctrl_frame(&mut self, type_: u8, args: &[u8]) -> i32 {
        if 1 + args.len() > CTRL_MAX { return E_TOOBIG; }
        let mut buf = Vec::with_capacity(1 + args.len());
        buf.push(type_);
        buf.extend_from_slice(args);
        self.send_mcp(K_CTRL, &buf)
    }
    fn send_ack(&mut self, seq: u8) -> i32 {
        self.ctrl_frame(C_ACK, &[seq])
    }

    fn ct_eq32(a: u32, b: u32) -> bool {
        let mut x = a ^ b;
        x |= x >> 16; x |= x >> 8; x |= x >> 4; x |= x >> 2; x |= x >> 1;
        (x & 1) == 0
    }

    fn resume_locked_now(&mut self) -> bool {
        if !self.resume_locked { return false; }
        if self.now_ms.wrapping_sub(self.resume_lock_t0) >= self.resume_lock_ms {
            self.resume_locked = false;
            self.resume_fail = 0;
            self.resume_lock_ms = 0;
            return false;
        }
        true
    }

    fn resume_fail_note(&mut self) {
        if self.resume_fail < 0xFF { self.resume_fail += 1; }
        if self.resume_fail >= RESUME_MAX_FAIL {
            let mut backoff = RESUME_LOCK_MS;
            let mut i = RESUME_MAX_FAIL;
            while i < self.resume_fail && backoff < RESUME_LOCK_MS * 8 {
                backoff <<= 1;
                i += 1;
            }
            self.resume_locked = true;
            self.resume_lock_t0 = self.now_ms;
            self.resume_lock_ms = backoff;
        }
    }

    fn rx_ctrl(&mut self, pl: &[u8]) -> i32 {
        if pl.is_empty() { return E_SYNC; }
        match pl[0] {
            C_CREDIT => {
                if pl.len() < 3 { return E_SYNC; }
                let total = u16::from_le_bytes([pl[1], pl[2]]);
                let delta = total.wrapping_sub(self.peer_consumed);
                self.peer_consumed = total;
                let cur = self.credits + delta as u32;
                self.credits = if cur > 255 { 255 } else { cur };
                0
            }
            C_PING => {
                let epoch = if pl.len() > 1 { pl[1] } else { 0 };
                if self.ping_pending && epoch == self.ping_epoch {
                    self.rtt_ms = self.now_ms.wrapping_sub(self.ping_t0);
                    self.rtt_valid = true;
                    self.ping_pending = false;
                    return 0;
                }
                self.ctrl_frame(C_PING, &[epoch])
            }
            C_ACK => {
                if pl.len() < 2 { return E_SYNC; }
                if let Some(p) = self.slot_by_seq(pl[1]) { self.txslot[p].valid = false; }
                self.rto_cur = self.rto_base;   /* V9：任何合法 ACK 都回落基准 */
                0
            }
            C_NACK => {
                if pl.len() < 3 { return E_SYNC; }
                let Some(p) = self.slot_by_seq(pl[1]) else { return 0; };
                let (seq, buf) = (self.txslot[p].seq, self.txslot[p].buf.clone());
                /* v0.9.1（与 C 同）：只为真正缺的片重发，不再「补到末尾」。
                 * 旧格式 len==3 → count=1（兼容老对端）。 */
                if pl.len() == 3 {
                    let rc = self.frame_idx(K_MCP, seq, pl[2], &buf);
                    if rc == 0 { self.stat_tx_retransmit += 1; }
                    return rc;
                }
                let cnt = pl[2] as usize;
                if cnt == 0 || cnt + 3 > pl.len() { return E_SYNC; }
                if self.txslot[p].nack_replay as usize >= cnt { return 0; }
                let mut rc = 0;
                for i in 0..cnt {
                    rc = self.frame_idx(K_MCP, seq, pl[3 + i], &buf);
                    if rc != 0 { break; }
                    self.txslot[p].nack_replay += 1;
                    self.stat_tx_retransmit += 1;
                }
                rc
            }
            C_RESUME_REQ => {
                if self.state != SState::LinkUp { return E_STATE; }
                if self.resume_locked_now() {
                    return self.ctrl_frame(C_RESUME_REJ, &[3]);
                }
                if pl.len() < 5 || self.resume_token == 0 {
                    self.resume_fail_note();
                    return self.ctrl_frame(C_RESUME_REJ, &[1]);
                }
                let tok = u32::from_le_bytes(pl[1..5].try_into().unwrap());
                if !Sess::ct_eq32(tok, self.resume_token) {
                    self.resume_fail_note();
                    return self.ctrl_frame(C_RESUME_REJ, &[2]);
                }
                self.resume_fail = 0;
                self.resume_locked = false;
                self.state = SState::Ready;
                self.ctrl_frame(C_RESUME_OK, &tok.to_le_bytes())
            }
            C_RESUME_OK => {
                if self.state != SState::LinkUp || !self.resume_pending {
                    return E_STATE;
                }
                self.resume_pending = false;
                self.state = SState::Ready;
                0
            }
            C_RESUME_REJ => {
                self.resume_pending = false;
                self.state = SState::LinkUp;
                0
            }
            _ => E_SYNC,
        }
    }

    /* 阶段一：状态门 / 控制帧终结 / 去重。Deliver = 应上抛给 L5。 */
    fn rx_pre(&mut self, kind: u8, seq: u8, payload: &[u8]) -> RxPre {
        if self.state == SState::Idle { return RxPre::Rc(E_STATE); }
        if kind == K_CTRL { return RxPre::Rc(self.rx_ctrl(payload)); }
        if self.state == SState::LinkUp {
            let p = payload;
            let ok = p.len() >= 9
                && (p.starts_with(b"initialize")
                    || (p.len() >= 11 && p.starts_with(b"\"initialize"))
                    || p.starts_with(b"{\"jsonrpc"));
            if !ok { return RxPre::Rc(E_STATE); }
        }
        if self.unreliable() && self.rx_dup_check(seq, payload) {
            self.stat_rx_dup += 1;
            if self.state == SState::Ready { self.send_ack(seq); }
            return RxPre::Rc(0);
        }
        RxPre::Deliver
    }

    /* 阶段二：上抛之后回补信用 / 回 ACK（与 C session_rx 顺序一致） */
    fn rx_post(&mut self, seq: u8) {
        if self.credit_on() {
            self.consumed = self.consumed.wrapping_add(1);
            let cum = self.consumed.to_le_bytes();
            let _ = self.ctrl_frame(C_CREDIT, &cum);
        }
        if self.unreliable() { self.send_ack(seq); }
    }

    fn rx(&mut self, kind: u8, seq: u8, payload: &[u8]) -> i32 {
        match self.rx_pre(kind, seq, payload) {
            RxPre::Rc(rc) => rc,
            RxPre::Deliver => {
                if self.msgs.len() < 8 { self.msgs.push((0, payload.to_vec())); }
                self.rx_post(seq);
                0
            }
        }
    }

    fn tx(&mut self, json: &[u8]) -> i32 { self.tx_common(json, false) }

    /* V8：引用式发送。C 侧只记指针、重传时读调用方缓冲；oracle 侧没有
     * 「调用方缓冲生命周期」的概念（指令流里缓冲立刻失效），因此等价语义是
     * 「不受 512B 限制地保存一份副本」——线上行为与 C 完全一致。 */
    fn tx_ref(&mut self, json: &[u8]) -> i32 { self.tx_common(json, true) }

    fn tx_common(&mut self, json: &[u8], ref_mode: bool) -> i32 {
        if self.state != SState::Ready && self.state != SState::LinkUp {
            return E_STATE;
        }
        if self.credit_on() && self.credits == 0 { return E_AGAIN; }
        let seq = self.alloc_seq();
        let rc = self.frame_seq(K_MCP, seq, json);
        if rc != 0 { return rc; }
        self.last_ctrl_ms = self.now_ms;   /* v0.6：有实际流量就不需要空闲刷新 */
        if self.credit_on() { self.credits -= 1; }
        if self.unreliable() && !json.is_empty()
            && (ref_mode || json.len() <= TX_RETRY_BYTES) {
            let p = self.slot_claim();
            self.txslot[p].buf.clear();
            self.txslot[p].buf.extend_from_slice(json);
            self.txslot[p].len = json.len() as u16;
            self.txslot[p].seq = seq;
            self.txslot[p].msg_no = self.tx_msg_no;
            self.txslot[p].nack_replay = 0;
            self.tx_msg_no = self.tx_msg_no.wrapping_add(1);
            self.txslot[p].n = 0;
            self.txslot[p].t0 = self.now_ms;
            self.txslot[p].valid = true;
        }
        0
    }

    fn ctrl(&mut self, type_: u8, args: &[u8]) -> i32 {
        if self.state == SState::Idle { return E_STATE; }
        if type_ == C_RESUME_REQ {
            if self.state != SState::LinkUp { return E_STATE; }
            if self.resume_locked_now() { return E_LOCKED; }
            self.resume_pending = true;
            self.resume_t0 = self.now_ms;
        }
        self.ctrl_frame(type_, args)
    }

    fn ping(&mut self, epoch: u8) -> i32 {
        if self.state != SState::Ready { return E_STATE; }
        let rc = self.ctrl_frame(C_PING, &[epoch]);
        if rc == 0 {
            self.ping_pending = true;
            self.ping_epoch = epoch;
            self.ping_t0 = self.now_ms;
        }
        rc
    }

    fn pump(&mut self) -> i32 {
        let mut rc = 0;
        if self.unreliable() {
            if let Some((seq, idxs)) = self.take_nack() {
                /* v0.9.1：type|seq|count|idx[count]；count==1 与旧格式字节等价 */
                let mut args = Vec::with_capacity(2 + idxs.len());
                args.push(seq);
                args.push(idxs.len() as u8);
                args.extend_from_slice(&idxs);
                let r = self.ctrl_frame(C_NACK, &args);
                if r != 0 { rc = r; }
            }
        }
        for i in 0..TX_SLOTS {
            if !self.txslot[i].valid { continue; }
            if self.now_ms.wrapping_sub(self.txslot[i].t0) < self.rto_cur { continue; }
            if self.txslot[i].n >= self.retry_max {
                self.txslot[i].valid = false;
                self.stat_tx_dropped += 1;
                if self.credit_on() && self.credits < 255 { self.credits += 1; }
                continue;
            }
            /* v0.7 A1（与 C 同）：对端去重窗口容不下这条消息时，重传会被
             * 当成新消息二次上抛。宁可放弃，也不制造重复。 */
            if self.tx_msg_no.wrapping_sub(self.txslot[i].msg_no) > RX_FP_SLOTS as u32 {
                self.txslot[i].valid = false;
                self.stat_tx_dropped += 1;
                self.stat_tx_stale_drop += 1;
                if self.credit_on() && self.credits < 255 { self.credits += 1; }
                continue;
            }
            let (seq, buf) = (self.txslot[i].seq, self.txslot[i].buf.clone());
            let r = self.frame_seq(K_MCP, seq, &buf);
            self.txslot[i].n += 1;
            self.txslot[i].t0 = self.now_ms;
            if r != 0 { rc = r; continue; }
            self.stat_tx_retransmit += 1;
            /* V9：指数退避（与 C 同） */
            let nx = (self.rto_cur as u64 * 2).min(RTO_MAX as u64) as u32;
            self.rto_cur = nx;
        }
        rc
    }

    fn tick(&mut self, now: u32) {
        self.now_ms = now;
        if self.state == SState::LinkUp && self.resume_pending
            && now.wrapping_sub(self.resume_t0) > RESUME_TOUT {
            self.resume_pending = false;
        }
        if self.state == SState::Ready && self.req_inflight
            && now.wrapping_sub(self.req_t0) > REQ_TOUT {
            self.req_inflight = false;
        }
        /* framer_tick */
        self.fnow = now;
        for s in self.slots.iter_mut() {
            if s.in_use {
                if now.wrapping_sub(s.t_first) > FRAG_TOUT {
                    s.in_use = false;
                } else {
                    s.t_last = now;
                }
            }
        }
        let _ = self.pump();

        /* v0.6：空闲信用刷新（与 C 同）。累计 CREDIT 幂等，重发可自愈「丢一个」。 */
        if self.credit_on()
            && (self.state == SState::Ready || self.state == SState::LinkUp)
            && now.wrapping_sub(self.last_ctrl_ms) >= CREDIT_REFRESH {
            let cum = self.consumed.to_le_bytes();
            if self.ctrl_frame(C_CREDIT, &cum) == 0 {
                self.last_ctrl_ms = now;
            }
        }
    }
}

fn wr_i32s(out: &mut impl Write, v: i32) {
    out.write_all(&v.to_le_bytes()).unwrap();
}
fn wr_u32s(out: &mut impl Write, v: usize) {
    out.write_all(&(v as u32).to_le_bytes()).unwrap();
}

fn run_session(input: &[u8], out: &mut impl Write) -> Result<(), i32> {
    if input.len() < 3 { return Err(2); }
    let packet = input[0] != 0;
    let mtu = u16::from_le_bytes([input[1], input[2]]);
    if packet && mtu.min(261) <= HDR as u16 { return Err(2); }
    let mut s = Sess::new(packet, mtu);

    let mut off = 3usize;
    while off < input.len() {
        let op = input[off]; off += 1;
        let mut rc = 0i32;
        match op {
            0x01 => {
                if off + 4 > input.len() { return Err(2); }
                let tok = u32::from_le_bytes(input[off..off + 4].try_into().unwrap());
                off += 4;
                s.link_up(tok);
            }
            0x02 => {
                if off >= input.len() { return Err(2); }
                let bond = input[off] != 0; off += 1;
                s.link_down(bond);
            }
            0x08 => {
                if off + 2 > input.len() { return Err(2); }
                let l = u16::from_le_bytes(input[off..off + 2].try_into().unwrap()) as usize;
                off += 2;
                if off + l > input.len() { return Err(2); }
                rc = s.tx_ref(&input[off..off + l]);
                off += l;
            }
            0x03 => {
                if off + 2 > input.len() { return Err(2); }
                let l = u16::from_le_bytes(input[off..off + 2].try_into().unwrap()) as usize;
                off += 2;
                if off + l > input.len() { return Err(2); }
                rc = s.tx(&input[off..off + l]);
                off += l;
            }
            0x04 => {
                if off + 2 > input.len() { return Err(2); }
                let l = u16::from_le_bytes(input[off..off + 2].try_into().unwrap()) as usize;
                off += 2;
                if off + l > input.len() { return Err(2); }
                let wire = &input[off..off + l];
                off += l;
                rc = rx_wire_s(&mut s, wire);
            }
            0x05 => {
                if off + 2 > input.len() { return Err(2); }
                let type_ = input[off];
                let alen = input[off + 1] as usize;
                off += 2;
                if off + alen > input.len() { return Err(2); }
                rc = s.ctrl(type_, &input[off..off + alen]);
                off += alen;
            }
            0x06 => {
                if off + 4 > input.len() { return Err(2); }
                let now = u32::from_le_bytes(input[off..off + 4].try_into().unwrap());
                off += 4;
                s.tick(now);
            }
            0x09 => {   /* SET_MTU u16（v0.7 进差分） */
                if off + 2 > input.len() { return Err(2); }
                let m = u16::from_le_bytes(input[off..off + 2].try_into().unwrap());
                off += 2;
                if s.packet {
                    let old = s.chunk();
                    s.mtu_cur = m;
                    /* 与 C 同：MTU 变了 → 在途重组槽的偏移失效，弃掉 */
                    if s.chunk() != old {
                        for sl in s.slots.iter_mut() {
                            sl.in_use = false;
                            sl.got = [0u8; (MAX_FRAG + 7) / 8];
                            sl.chunk = 0; sl.len_total = 0; sl.have_last = false;
                        }
                        s.dead_valid = false;
                        s.nack_pending = false;
                    }
                    rc = 0;
                } else { rc = E_ARGS; }
            }
            _ => return Err(2),
        }
        wr_i32s(out, rc);
        wr_u32s(out, s.wire.len());
        out.write_all(&s.wire).unwrap();
        wr_u32s(out, s.msgs.len());
        for (mrc, m) in s.msgs.iter() {
            wr_i32s(out, *mrc);
            wr_u32s(out, m.len());
            out.write_all(m).unwrap();
        }
        s.wire.clear();
        s.msgs.clear();
    }
    Ok(())
}

fn rx_wire_s(s: &mut Sess, wire: &[u8]) -> i32 {
    let mut last_rc = 0i32;
    let mut poff = 0usize;
    while poff < wire.len() {
        let mut msgs: Vec<(u8, u8, Vec<u8>)> = Vec::new();
        if s.packet {
            /* 判例同 C：帧长解析失败直接返回（不泵 NACK） */
            if poff + 2 > wire.len() { return E_PARSE; }
            let flen = u16::from_le_bytes([wire[poff], wire[poff + 1]]) as usize;
            poff += 2;
            if poff + flen > wire.len() { return E_PARSE; }
            let rc = s.feed_packet(&wire[poff..poff + flen], &mut msgs);
            poff += flen;
            /* 判例同 C：feed 出错跳出循环，但仍要泵 NACK */
            if rc != 0 { last_rc = rc; break; }
        } else {
            let rc = s.feed_stream(wire, &mut msgs);
            if rc != 0 { last_rc = rc; break; }
            poff = wire.len();
        }
        for (kind, seq, payload) in msgs {
            last_rc = s.rx(kind, seq, &payload);
        }
    }
    let _ = s.pump();
    last_rc
}

/* ---------------- JSON：C bt_mcp_json_* 的严格镜像 ---------------- */

fn is_digit(c: u8) -> bool { (b'0'..=b'9').contains(&c) }
fn is_hex(c: u8) -> bool {
    is_digit(c) || ((c | 0x20) >= b'a' && (c | 0x20) <= b'f')
}

fn ws_at(json: &[u8], p: &mut usize) {
    while *p < json.len() && matches!(json[*p], b' ' | b'\t' | b'\n' | b'\r') {
        *p += 1;
    }
}
fn peek_at(json: &[u8], p: &mut usize, ch: u8) -> bool {
    ws_at(json, p);
    *p < json.len() && json[*p] == ch
}

fn scan_number(json: &[u8], p: &mut usize) -> Option<()> {
    let mut i = *p;
    if i < json.len() && json[i] == b'-' { i += 1; }
    if i >= json.len() { return None; }
    if json[i] == b'0' { i += 1; }
    else if is_digit(json[i]) { while i < json.len() && is_digit(json[i]) { i += 1; } }
    else { return None; }
    if i < json.len() && json[i] == b'.' {
        i += 1;
        if i >= json.len() || !is_digit(json[i]) { return None; }
        while i < json.len() && is_digit(json[i]) { i += 1; }
    }
    if i < json.len() && (json[i] == b'e' || json[i] == b'E') {
        i += 1;
        if i < json.len() && (json[i] == b'+' || json[i] == b'-') { i += 1; }
        if i >= json.len() || !is_digit(json[i]) { return None; }
        while i < json.len() && is_digit(json[i]) { i += 1; }
    }
    *p = i;
    Some(())
}

fn scan_literal(json: &[u8], p: &mut usize, lit: &[u8]) -> Option<()> {
    if json.len() - *p < lit.len() || &json[*p..*p + lit.len()] != lit { return None; }
    *p += lit.len();
    Some(())
}

/* C skip_value 镜像：返回片段 [v0, v1) 与是否字符串 */
fn skip_value_c(json: &[u8], p: &mut usize, depth: usize)
    -> Option<(usize, usize, bool)> {
    ws_at(json, p);
    if *p >= json.len() { return None; }
    let start = *p;
    match json[*p] {
        b'"' => {
            *p += 1;
            loop {
                if *p >= json.len() { return None; }
                if json[*p] == b'\\' {
                    *p += 1;
                    if *p >= json.len() { return None; }
                    if json[*p] == b'u' {
                        if json.len() - *p < 5 { return None; }
                        for i in 1..=4 {
                            if !is_hex(json[*p + i]) { return None; }
                        }
                        *p += 4;
                    } else if !b"\"\\/bfnrt".contains(&json[*p]) {
                        return None;
                    }
                    *p += 1;
                    continue;
                }
                if json[*p] == b'"' {
                    *p += 1;
                    return Some((start, *p, true));
                }
                *p += 1;
            }
        }
        b'{' | b'[' => {
            if depth + 1 > 6 { return None; }
            let open = json[*p];
            let close = if open == b'{' { b'}' } else { b']' };
            *p += 1;
            loop {
                if peek_at(json, p, close) {
                    *p += 1;
                    return Some((start, *p, false));
                }
                if *p >= json.len() { return None; }
                if open == b'{' {
                    if !peek_at(json, p, b'"') { return None; }
                    skip_value_c(json, p, depth + 1)?;
                    if !peek_at(json, p, b':') { return None; }
                    *p += 1;
                }
                skip_value_c(json, p, depth + 1)?;
                if peek_at(json, p, b',') { *p += 1; }
                else if !peek_at(json, p, close) && *p < json.len() { return None; }
            }
        }
        b't' => { scan_literal(json, p, b"true")?; }
        b'f' => { scan_literal(json, p, b"false")?; }
        b'n' => { scan_literal(json, p, b"null")?; }
        c if c == b'-' || is_digit(c) => { scan_number(json, p)?; }
        _ => return None,
    }
    Some((start, *p, false))
}

fn hex4(s: &[u8]) -> u32 {
    let mut v = 0u32;
    for i in 0..4 {
        let c = s[i];
        v <<= 4;
        v |= if c <= b'9' { (c - b'0') as u32 }
             else { ((c | 0x20) - b'a' + 10) as u32 };
    }
    v
}

fn put_utf8(cp: u32, dst: &mut Vec<u8>) {
    if cp < 0x80 { dst.push(cp as u8); }
    else if cp < 0x800 {
        dst.push((0xC0 | (cp >> 6)) as u8);
        dst.push((0x80 | (cp & 0x3F)) as u8);
    } else if cp < 0x10000 {
        dst.push((0xE0 | (cp >> 12)) as u8);
        dst.push((0x80 | ((cp >> 6) & 0x3F)) as u8);
        dst.push((0x80 | (cp & 0x3F)) as u8);
    } else {
        dst.push((0xF0 | (cp >> 18)) as u8);
        dst.push((0x80 | ((cp >> 12) & 0x3F)) as u8);
        dst.push((0x80 | ((cp >> 6) & 0x3F)) as u8);
        dst.push((0x80 | (cp & 0x3F)) as u8);
    }
}

/* C bt_mcp_json_decode_str 镜像：[start,end) 为含引号片段；解码转义（含代理对） */
fn decode_str(buf: &[u8], mut p: usize, mut end: usize) -> Vec<u8> {
    let mut out = Vec::new();
    if p < end && buf[p] == b'"' { p += 1; }
    if end > p && buf[end - 1] == b'"' { end -= 1; }
    while p < end {
        if buf[p] == b'\\' {
            p += 1;
            if p >= end { break; }
            match buf[p] {
                b'n' => { out.push(b'\n'); p += 1; }
                b't' => { out.push(b'\t'); p += 1; }
                b'r' => { out.push(b'\r'); p += 1; }
                b'b' => { out.push(0x08); p += 1; }
                b'f' => { out.push(0x0C); p += 1; }
                b'u' => {
                    if end - p < 5 { p = end; continue; }
                    let mut cp = hex4(&buf[p + 1..]);
                    p += 5;
                    if (0xD800..=0xDBFF).contains(&cp) && end - p >= 6
                        && buf[p] == b'\\' && buf[p + 1] == b'u' {
                        let lo = hex4(&buf[p + 2..]);
                        if (0xDC00..=0xDFFF).contains(&lo) {
                            cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                            p += 6;
                        }
                    }
                    if (0xD800..=0xDFFF).contains(&cp) { cp = 0xFFFD; }
                    put_utf8(cp, &mut out);
                }
                c => { out.push(c); p += 1; }
            }
        } else {
            out.push(buf[p]);
            p += 1;
        }
    }
    out
}

fn decode_str_full(s: &[u8], total_out: &mut usize) -> Vec<u8> {
    /* 用于键名：返回解码结果（C 用 need 判断是否超长） */
    let v = decode_str(s, 0, s.len());
    *total_out = v.len();
    v
}

fn parse_i64_sat(p: &[u8]) -> i64 {
    let mut i = 0usize;
    let mut neg = false;
    if i < p.len() && (p[i] == b'-' || p[i] == b'+') {
        neg = p[i] == b'-';
        i += 1;
    }
    let lim: u64 = if neg { 9223372036854775808 } else { 9223372036854775807 };
    let mut acc: u64 = 0;
    while i < p.len() && is_digit(p[i]) {
        let d = (p[i] - b'0') as u64;
        if acc > (lim - d) / 10 {
            acc = lim;
            while i < p.len() && is_digit(p[i]) { i += 1; }
            break;
        }
        acc = acc * 10 + d;
        i += 1;
    }
    if neg {
        if acc >= 9223372036854775808 { i64::MIN } else { -(acc as i64) }
    } else {
        acc as i64
    }
}

struct Req {
    method: Vec<u8>,
    has_method: bool,
    has_id: bool,
    id_is_str: bool,
    id_num: i64,
    id_str: Vec<u8>,
    params: Vec<u8>,
}

fn id_seen(r: &Req) -> Option<Req> {
    if !r.has_id { return None; }
    Some(Req { method: Vec::new(), has_method: false, has_id: true,
               id_is_str: r.id_is_str, id_num: r.id_num,
               id_str: r.id_str.clone(), params: Vec::new() })
}

fn parse_req(json: &[u8]) -> Result<Req, Option<Req>> {
    let mut p = 0usize;
    if !peek_at(json, &mut p, b'{') { return Err(None); }
    p += 1;
    let mut r = Req { method: Vec::new(), has_method: false, has_id: false,
                      id_is_str: false, id_num: 0, id_str: Vec::new(),
                      params: Vec::new() };
    while !peek_at(json, &mut p, b'}') {
        if p >= json.len() { return Err(id_seen(&r)); }
        if !peek_at(json, &mut p, b'"') { return Err(id_seen(&r)); }
        let (k0, k1, _) = skip_value_c(json, &mut p, 0).ok_or_else(|| id_seen(&r))?;
        let mut need = 0usize;
        let key = decode_str_full(&json[k0..k1], &mut need);
        let key_known = need < KEY_MAX;
        if !peek_at(json, &mut p, b':') { return Err(id_seen(&r)); }
        p += 1;
        let (v0, v1, is_str) = skip_value_c(json, &mut p, 0).ok_or_else(|| id_seen(&r))?;
        if key_known && key == b"method" {
            if v1 <= v0 || json[v0] != b'"' { return Err(id_seen(&r)); }
            r.method = json[v0 + 1..v1 - 1].to_vec();
            r.has_method = true;
        } else if key_known && key == b"id" {
            r.has_id = true;
            if is_str {
                r.id_is_str = true;
                r.id_str = json[v0 + 1..v1 - 1].to_vec();
            } else {
                r.id_is_str = false;
                r.id_num = parse_i64_sat(&json[v0..v1]);
            }
        } else if key_known && key == b"params" {
            r.params = json[v0..v1].to_vec();
        }
        if peek_at(json, &mut p, b',') {
            p += 1;
            /* 严格 JSON：逗号后不允许直接闭合（{"a":1,} 是语法错） */
            if peek_at(json, &mut p, b'}') { return Err(id_seen(&r)); }
        } else if !peek_at(json, &mut p, b'}') && p < json.len() {
            return Err(id_seen(&r));
        }
    }
    /* 闭合对象后只允许空白（与 C 同：尾随垃圾视为报文损坏） */
    p += 1;
    while p < json.len() && matches!(json[p], b' ' | b'\t' | b'\r' | b'\n') {
        p += 1;
    }
    if p < json.len() { return Err(id_seen(&r)); }
    if !r.has_method { return Err(id_seen(&r)); }
    Ok(r)
}

/* params 内顶层键查找（值片段） */
fn find_key(params: &[u8], key: &[u8]) -> Option<(usize, usize)> {
    if params.first() != Some(&b'{') { return None; }
    let mut p = 1usize;
    loop {
        if peek_at(params, &mut p, b'}') { return None; }
        if p >= params.len() { return None; }
        if !peek_at(params, &mut p, b'"') { return None; }
        let (k0, k1, _) = skip_value_c(params, &mut p, 0)?;
        let mut need = 0usize;
        let kb = decode_str_full(&params[k0..k1], &mut need);
        let known = need < KEY_MAX;
        if !peek_at(params, &mut p, b':') { return None; }
        p += 1;
        let (v0, v1, _) = skip_value_c(params, &mut p, 0)?;
        if known && kb == key { return Some((v0, v1)); }
        if peek_at(params, &mut p, b',') { p += 1; }
        else if !peek_at(params, &mut p, b'}') && p < params.len() { return None; }
    }
}

/* v0.7（A5）：取 params.<obj>.<key> 的字符串值（不含引号，转义保留） */
/* 取任意类型值的原文（对象/数组/字符串/数字都可）——get_str_value 只认字符串，
 * 取对象必须用它，否则嵌套 params 会被误判为失败。 */
fn get_raw_value(params: &[u8], key: &[u8]) -> Option<Vec<u8>> {
    let (v0, v1) = find_key(params, key)?;
    if v1 <= v0 { return None; }
    Some(params[v0..v1].to_vec())
}

fn sub_get_str(params: &[u8], obj: &[u8], key: &[u8]) -> Option<Vec<u8>> {
    let inner = get_raw_value(params, obj)?;
    if inner.len() < 2 { return None; }
    /* 子对象形态是 {...}（不带引号）；只有字符串值才需要去两侧引号 */
    let body: &[u8] = if inner[0] == b'"' { &inner[1..inner.len() - 1] } else { &inner[..] };
    let v = get_str_value(body, key)?;
    if v.len() < 2 || v[0] != b'"' { return None; }
    Some(v[1..v.len() - 1].to_vec())
}

fn get_str_value(params: &[u8], key: &[u8]) -> Option<Vec<u8>> {
    let (v0, v1) = find_key(params, key)?;
    if v1 < v0 + 2 || params[v0] != b'"' { return None; }
    Some(params[v0..v1].to_vec())      /* 含引号的原文段 */
}

fn get_u32_value(params: &[u8], key: &[u8]) -> Option<u32> {
    let (v0, v1) = find_key(params, key)?;
    if v1 == v0 { return None; }
    let mut acc: u64 = 0;
    for i in v0..v1 {
        if !is_digit(params[i]) { return None; }
        acc = acc * 10 + (params[i] - b'0') as u64;
        if acc > 0xFFFFFFFF { acc = 0xFFFFFFFF; }
    }
    Some(acc as u32)
}

/* ---------------- L5：MCP server ---------------- */

struct McpTool {
    name: String,
    desc: String,
    title: Option<String>,
    output_schema: Option<String>,
    annotations: u8,       /* 1=readOnly 2=destructive 4=idempotent 8=openWorld */
    authz: u8,             /* 0=open 1=consent 2=restricted */
    kind: u8,              /* 0=echo 1=allow 2=rich */
}

struct ResDef {
    name: &'static str,
    desc: &'static str,
    title: Option<&'static str>,
    mime: &'static str,
    text: &'static [u8],
    page: usize,
    size: u32,             /* 0 = 不输出 size */
    sub: bool,             /* 是否可订阅 */
}

const RES_HR: &[u8] = b"hello resource";
const RES_LOG: &[u8] = b"0123456789ABCDEFGHIJ";
const RESOURCES: [ResDef; 2] = [
    ResDef { name: "hr_window", desc: "heart rate window",
             title: Some("Heart rate"), mime: "text/plain",
             text: RES_HR, page: 0, size: 0, sub: false },
    ResDef { name: "log", desc: "device log", title: None, mime: "text/plain",
             text: RES_LOG, page: 8, size: 20, sub: true },
];

struct PromptDef { name: &'static str, desc: &'static str, title: &'static str }
const PROMPTS: [PromptDef; 1] = [
    PromptDef { name: "greet", desc: "greeting prompt", title: "Greet" },
];

struct McpSrv {
    sess: Sess,
    tools: Vec<McpTool>,
    initialized: bool,
    log_level: u8,
    subs: Vec<u16>,
    proto: String,          /* 协商到的协议版本（v0.5 V6） */
    batching: bool,
    bout: Vec<u8>,
}

fn json_escape(s: &[u8]) -> Vec<u8> {
    let mut v = Vec::with_capacity(s.len() + 2);
    v.push(b'"');
    for &c in s {
        match c {
            b'"' => v.extend_from_slice(b"\\\""),
            b'\\' => v.extend_from_slice(b"\\\\"),
            b'\n' => v.extend_from_slice(b"\\n"),
            b'\r' => v.extend_from_slice(b"\\r"),
            b'\t' => v.extend_from_slice(b"\\t"),
            _ if c < 0x20 => v.extend_from_slice(
                format!("\\u00{:02x}", c).as_bytes()),
            _ => v.push(c),
        }
    }
    v.push(b'"');
    v
}

fn put_id(out: &mut Vec<u8>, id: i64, id_str: Option<&[u8]>) {
    match id_str {
        Some(s) => {
            out.push(b'"');
            out.extend_from_slice(s);
            out.push(b'"');
        }
        None => out.extend_from_slice(id.to_string().as_bytes()),
    }
}

/* C respond()：head + body + "}}" 必须 ≤ 8192（memsink cap） */
fn respond_json(id: i64, id_str: Option<&[u8]>, body: &[u8]) -> Option<Vec<u8>> {
    let mut v = Vec::with_capacity(64 + body.len());
    v.extend_from_slice(b"{\"jsonrpc\":\"2.0\",\"id\":");
    put_id(&mut v, id, id_str);
    v.extend_from_slice(b",\"result\":{");
    if v.len() + body.len() > MAX_MSG { return None; }
    v.extend_from_slice(body);
    if v.len() + 2 > MAX_MSG { return None; }
    v.extend_from_slice(b"}}");
    Some(v)
}

/* C respond_error()：ebuf 288 */
fn error_json(id: i64, id_str: Option<&[u8]>, code: i32, msg: &str) -> Option<Vec<u8>> {
    let mut v = Vec::new();
    v.extend_from_slice(b"{\"jsonrpc\":\"2.0\",\"id\":");
    put_id(&mut v, id, id_str);
    v.extend_from_slice(format!(",\"error\":{{\"code\":{},\"message\":", code).as_bytes());
    v.extend_from_slice(&json_escape(msg.as_bytes()));
    v.extend_from_slice(b"}}");
    if v.len() > 288 { return None; }
    Some(v)
}

fn respond(sv: &mut McpSrv, r: &Req, body: &[u8]) -> i32 {
    let id_str = if r.id_is_str { Some(&r.id_str[..]) } else { None };
    let Some(j) = respond_json(r.id_num, id_str, body) else { return E_TOOBIG; };
    if sv.batching {
        if !sv.bout.is_empty() { sv.bout.push(b','); }
        sv.bout.extend_from_slice(&j);
        return 0;
    }
    sv.sess.tx(&j)
}

fn respond_err(sv: &mut McpSrv, r: &Req, code: i32, msg: &str) -> i32 {
    let id_str = if r.id_is_str { Some(&r.id_str[..]) } else { None };
    let Some(j) = error_json(r.id_num, id_str, code, msg) else { return E_TOOBIG; };
    if sv.batching {
        if !sv.bout.is_empty() { sv.bout.push(b','); }
        sv.bout.extend_from_slice(&j);
        return 0;
    }
    sv.sess.tx(&j)
}

fn version_supported(v: &[u8]) -> bool {
    ["2025-11-25", "2025-06-18", "2025-03-26", "2024-11-05", "2024-10-07"]
        .iter().any(|s| s.as_bytes() == v)
}

fn body_initialize(sv: &mut McpSrv, r: &Req) -> Vec<u8> {
    let mut b = Vec::new();
    let echo = get_str_value(&r.params, b"protocolVersion")
        .map(|v| v[1..v.len() - 1].to_vec())
        .filter(|v| version_supported(v));
    b.extend_from_slice(b"\"protocolVersion\":");
    let eff: Vec<u8> = match &echo {
        Some(v) => v.clone(),
        None => b"2025-11-25".to_vec(),
    };
    match echo {
        Some(v) => b.extend_from_slice(&json_escape(&v)),
        None => b.extend_from_slice(b"\"2025-11-25\""),
    }
    sv.proto = String::from_utf8_lossy(&eff).into_owned();
    b.extend_from_slice(br#","capabilities":{"tools":{"listChanged":true},"resources":{"subscribe":true,"listChanged":true"#);
    /* v0.7（A5）：templates 是 resources 对象内部的键（与 C 同） */
    b.extend_from_slice(b",\"templates\":{}}");
    /* prompts 恒存在（mock 固定注册 1 个） */
    b.extend_from_slice(b",\"prompts\":{\"listChanged\":true}");
    /* v0.7（A5）：completions 与 logging 同级 */
    b.extend_from_slice(b",\"completions\":{}");
    b.extend_from_slice(b",\"logging\":{}}");
    b.extend_from_slice(b",\"serverInfo\":{\"name\":");
    b.extend_from_slice(&json_escape(sv.sess_fw().0.as_bytes()));
    b.extend_from_slice(b",\"version\":");
    b.extend_from_slice(&json_escape(sv.sess_fw().1.as_bytes()));
    b.extend_from_slice(b"},\"_meta\":{\"io.btmcp/ver\":\"0.5\",\"io.btmcp/chan\":{");
    let cap = sv.sess_cap();
    b.extend_from_slice(format!("\"kind\":{},", cap.0).as_bytes());
    b.extend_from_slice(format!("\"mtu\":{},", cap.1).as_bytes());
    b.extend_from_slice(format!("\"window\":{},", cap.2).as_bytes());
    b.extend_from_slice(format!("\"reliable\":{},", cap.3).as_bytes());
    b.extend_from_slice(format!("\"flow\":{}", cap.4).as_bytes());
    b.extend_from_slice(b"}}");
    b
}

impl McpSrv {
    fn sess_fw(&self) -> (String, String) {
        ("bt-mcp-device".to_string(), "0.5".to_string())
    }
    /* (kind, mtu, window, reliable, flow) */
    fn sess_cap(&self) -> (u32, u32, u32, u32, u32) {
        let kind = if self.sess.packet { 0 } else { 1 };
        let mtu = if self.sess.packet { self.sess.mtu as u32 } else { 0 };
        let rel = if self.sess.reliable { 1 } else { 0 };
        let flow = if self.sess.flow_ctrl { 1 } else { 0 };
        (kind, mtu, self.sess.tx_window, rel, flow)
    }
    fn tool_by_name(&self, name: &[u8]) -> Option<&McpTool> {
        self.tools.iter().find(|t| t.name.as_bytes() == name)
    }
}

fn parse_digits(p: &[u8]) -> u32 {
    let mut v: u32 = 0;
    for &c in p {
        if !is_digit(c) { break; }
        if v > 0x0FFFFFFF { return 0xFFFFFFFF; }
        v = v * 10 + (c - b'0') as u32;
    }
    v
}

fn page_of(r: &Req, total: u16) -> (usize, usize) {
    let mut s = 0usize;
    if let Some(cur) = get_str_value(&r.params, b"cursor") {
        s = parse_digits(&cur[1..cur.len() - 1]) as usize;
    }
    if s > total as usize { s = total as usize; }
    let mut e = s + PAGE_SIZE;
    if e > total as usize { e = total as usize; }
    (s, e)
}

fn put_next_cursor(body: &mut Vec<u8>, next: usize, total: u16) {
    if next >= total as usize { return; }
    body.extend_from_slice(b",\"nextCursor\":");
    body.extend_from_slice(&json_escape(next.to_string().as_bytes()));
}

fn tools_list(sv: &McpSrv, r: &Req) -> Vec<u8> {
    let total = sv.tools.len() as u16;
    let (start, end) = page_of(r, total);
    let mut b = Vec::new();
    b.extend_from_slice(b"\"tools\":[");
    for (i, t) in sv.tools.iter().enumerate().take(end).skip(start) {
        if i != start { b.push(b','); }
        b.extend_from_slice(b"{\"name\":");
        b.extend_from_slice(&json_escape(t.name.as_bytes()));
        if let Some(ti) = &t.title {
            b.extend_from_slice(b",\"title\":");
            b.extend_from_slice(&json_escape(ti.as_bytes()));
        }
        b.extend_from_slice(b",\"description\":");
        b.extend_from_slice(&json_escape(t.desc.as_bytes()));
        b.extend_from_slice(b",\"inputSchema\":");
        b.extend_from_slice(b"{\"type\":\"object\",\"properties\":{}}");
        if let Some(os) = &t.output_schema {
            b.extend_from_slice(b",\"outputSchema\":");
            b.extend_from_slice(os.as_bytes());
        }
        if t.annotations != 0 {
            b.extend_from_slice(b",\"annotations\":{");
            let mut first = true;
            for (bit, name) in [(1u8, "readOnlyHint"), (2, "destructiveHint"),
                                (4, "idempotentHint"), (8, "openWorldHint")] {
                if t.annotations & bit == 0 { continue; }
                if !first { b.push(b','); }
                first = false;
                b.push(b'"');
                b.extend_from_slice(name.as_bytes());
                b.extend_from_slice(b"\":true");
            }
            b.push(b'}');
        }
        b.push(b'}');
    }
    b.extend_from_slice(b"]");
    put_next_cursor(&mut b, end, total);
    b
}

const URI_PFX: &[u8] = b"bt-mcp://dev/";

fn parse_resource_uri(uri: &[u8]) -> Option<(Vec<u8>, u32)> {
    if uri.len() <= URI_PFX.len() || &uri[..URI_PFX.len()] != URI_PFX { return None; }
    let mut i = URI_PFX.len();
    let ns = i;
    while i < uri.len() && uri[i] != b'?' && uri[i] != b'/' { i += 1; }
    if i == ns { return None; }
    let name = uri[ns..i].to_vec();
    let mut cursor = 0u32;
    if i < uri.len() && uri[i] == b'?' {
        i += 1;
        while i + 7 <= uri.len() {
            if &uri[i..i + 7] == b"cursor=" {
                cursor = parse_digits(&uri[i + 7..]);
                break;
            }
            i += 1;
        }
    }
    Some((name, cursor))
}

/* v0.7（A5）：资源模板（与 mock 的 MOCK_TPL 一致） */
struct Tpl { uri: &'static [u8], name: &'static [u8], desc: &'static [u8],
             title: &'static [u8], mime: &'static [u8] }
const TEMPLATES: &[Tpl] = &[
    Tpl { uri: b"bt-mcp://dev/{name}/log", name: b"log_tpl",
          desc: b"device log", title: b"Log", mime: b"text/plain" },
];

fn templates_list(_sv: &McpSrv, r: &Req) -> Vec<u8> {
    let total = TEMPLATES.len() as u16;
    let (start, end) = page_of(r, total);
    let mut b = Vec::new();
    b.extend_from_slice(b"\"resourceTemplates\":[");
    for (i, t) in TEMPLATES.iter().enumerate().take(end).skip(start) {
        if i != start { b.push(b','); }
        b.extend_from_slice(b"{\"uriTemplate\":");
        b.extend_from_slice(&json_escape(t.uri));
        b.extend_from_slice(b",\"name\":");
        b.extend_from_slice(&json_escape(t.name));
        b.extend_from_slice(b",\"title\":");
        b.extend_from_slice(&json_escape(t.title));
        b.extend_from_slice(b",\"description\":");
        b.extend_from_slice(&json_escape(t.desc));
        b.extend_from_slice(b",\"mimeType\":");
        b.extend_from_slice(&json_escape(t.mime));
        b.push(b'}');
    }
    b.push(b']');
    put_next_cursor(&mut b, end, total);
    b
}

/* 补全：与 mock 的 mock_complete 同语义（alpha/beta/gamma 前缀过滤） */
fn complete_body(r: &Req) -> Vec<u8> {
    const ALL: [&[u8]; 3] = [b"alpha", b"beta", b"gamma"];
    let av = sub_get_str(&r.params, b"argument", b"value").unwrap_or_default();
    let mut b = Vec::new();
    b.extend_from_slice(b"\"completion\":{\"values\":[");
    let mut first = true;
    for v in ALL.iter() {
        if !av.is_empty() && !v.starts_with(&av[..]) { continue; }
        if !first { b.push(b','); }
        first = false;
        b.extend_from_slice(&json_escape(v));
    }
    b.extend_from_slice(b"]}");
    b
}

fn resources_list(sv: &McpSrv, r: &Req) -> Vec<u8> {
    let total = RESOURCES.len() as u16;
    let _ = sv;
    let (start, end) = page_of(r, total);
    let mut b = Vec::new();
    b.extend_from_slice(b"\"resources\":[");
    for (i, rd) in RESOURCES.iter().enumerate().take(end).skip(start) {
        if i != start { b.push(b','); }
        b.extend_from_slice(b"{\"name\":");
        b.extend_from_slice(&json_escape(rd.name.as_bytes()));
        if let Some(ti) = rd.title {
            b.extend_from_slice(b",\"title\":");
            b.extend_from_slice(&json_escape(ti.as_bytes()));
        }
        b.extend_from_slice(b",\"uri\":\"bt-mcp://dev/");
        b.extend_from_slice(rd.name.as_bytes());
        b.extend_from_slice(b"\",\"description\":");
        b.extend_from_slice(&json_escape(rd.desc.as_bytes()));
        b.extend_from_slice(b",\"mimeType\":");
        b.extend_from_slice(&json_escape(rd.mime.as_bytes()));
        if rd.size != 0 {
            b.extend_from_slice(b",\"size\":");
            b.extend_from_slice(rd.size.to_string().as_bytes());
        }
        b.push(b'}');
    }
    b.extend_from_slice(b"]");
    put_next_cursor(&mut b, end, total);
    b
}

fn resources_read(r: &Req) -> Result<Vec<u8>, i32> {
    let Some(uri_raw) = get_str_value(&r.params, b"uri") else { return Err(E_PARSE); };
    let uri = &uri_raw[1..uri_raw.len() - 1];
    let Some((name, mut cursor)) = parse_resource_uri(uri) else { return Err(E_PARSE); };
    if cursor == 0 {
        if let Some(cur) = get_str_value(&r.params, b"cursor") {
            cursor = parse_digits(&cur[1..cur.len() - 1]);
        }
    }
    let Some(rd) = RESOURCES.iter().find(|d| d.name.as_bytes() == &name[..]) else {
        return Err(-2);
    };
    let (text, next) = if cursor as usize >= rd.text.len() {
        (&b""[..], u32::MAX)
    } else {
        let mut n = rd.text.len() - cursor as usize;
        if rd.page != 0 && n > rd.page { n = rd.page; }
        let slice = &rd.text[cursor as usize..cursor as usize + n];
        let nx = cursor as usize + n;
        (slice, if nx < rd.text.len() { nx as u32 } else { u32::MAX })
    };
    let mut b = Vec::new();
    b.extend_from_slice(b"\"contents\":[{\"uri\":\"bt-mcp://dev/");
    b.extend_from_slice(rd.name.as_bytes());
    b.extend_from_slice(b"\",\"mimeType\":");
    b.extend_from_slice(&json_escape(rd.mime.as_bytes()));
    b.extend_from_slice(b",\"text\":\"");
    b.extend_from_slice(&json_escape_inner(text));
    b.extend_from_slice(b"\"}]");
    if next != u32::MAX {
        b.extend_from_slice(b",\"nextCursor\":");
        b.extend_from_slice(&json_escape(next.to_string().as_bytes()));
    }
    Ok(b)
}

fn prompts_list(r: &Req) -> Vec<u8> {
    let total = PROMPTS.len() as u16;
    let (start, end) = page_of(r, total);
    let mut b = Vec::new();
    b.extend_from_slice(b"\"prompts\":[");
    for (i, p) in PROMPTS.iter().enumerate().take(end).skip(start) {
        if i != start { b.push(b','); }
        b.extend_from_slice(b"{\"name\":");
        b.extend_from_slice(&json_escape(p.name.as_bytes()));
        b.extend_from_slice(b",\"title\":");
        b.extend_from_slice(&json_escape(p.title.as_bytes()));
        b.extend_from_slice(b",\"description\":");
        b.extend_from_slice(&json_escape(p.desc.as_bytes()));
        b.push(b'}');
    }
    b.extend_from_slice(b"]");
    put_next_cursor(&mut b, end, total);
    b
}

/* prompts/get：与 mock 的 prompt_greet 逐字节一致 */
fn prompts_get(r: &Req) -> Result<Vec<u8>, i32> {
    let Some(name_raw) = get_str_value(&r.params, b"name") else { return Err(E_PARSE); };
    let name = &name_raw[1..name_raw.len() - 1];
    if !PROMPTS.iter().any(|p| p.name.as_bytes() == name) { return Err(-2); }
    let mut b = Vec::new();
    b.extend_from_slice(b"\"description\":");
    b.extend_from_slice(&json_escape(b"A friendly greeting"));
    b.extend_from_slice(b",\"messages\":[{\"role\":\"user\",\"content\":{\"type\":\"text\",\"text\":\"hello\"}}]");
    /* 与 C 的 bt_mcp_w_meta 逐字节一致（v0.6：让该 API 走真实路径） */
    b.extend_from_slice(b",\"_meta\":{\"io.btmcp/prompt\":\"greet\"}");
    Ok(b)
}

/* ---- 内容项拼装（对应 C 的 cs_t） ---- */
struct Cs { b: Vec<u8>, items: u8, text_open: bool, err: bool }

impl Cs {
    fn new() -> Self { Cs { b: Vec::new(), items: 0, text_open: false, err: false } }
    fn close_text(&mut self) {
        if self.text_open { self.text_open = false; self.b.extend_from_slice(b"\"}"); }
    }
    fn open_text(&mut self) {
        if self.text_open { return; }
        if self.items != 0 { self.b.push(b','); }
        self.b.extend_from_slice(b"{\"type\":\"text\",\"text\":\"");
        self.text_open = true;
        self.items = self.items.wrapping_add(1);
    }
    fn put(&mut self, d: &[u8]) {
        if self.err { return; }
        for &ch in d {
            if !self.text_open { self.open_text(); }
            match ch {
                b'"' => self.b.extend_from_slice(b"\\\""),
                b'\\' => self.b.extend_from_slice(b"\\\\"),
                b'\n' => self.b.extend_from_slice(b"\\n"),
                b'\r' => self.b.extend_from_slice(b"\\r"),
                b'\t' => self.b.extend_from_slice(b"\\t"),
                _ if ch < 0x20 => self.b.extend_from_slice(
                    format!("\\u00{:02x}", ch).as_bytes()),
                _ => self.b.push(ch),
            }
        }
    }
    fn escape_inline(&mut self, d: &[u8]) {
        for &ch in d {
            match ch {
                b'"' => self.b.extend_from_slice(b"\\\""),
                b'\\' => self.b.extend_from_slice(b"\\\\"),
                b'\n' => self.b.extend_from_slice(b"\\n"),
                b'\r' => self.b.extend_from_slice(b"\\r"),
                b'\t' => self.b.extend_from_slice(b"\\t"),
                _ if ch < 0x20 => self.b.extend_from_slice(
                    format!("\\u00{:02x}", ch).as_bytes()),
                _ => self.b.push(ch),
            }
        }
    }
    fn item_prefix(&mut self) {
        self.close_text();
        if self.items != 0 { self.b.push(b','); }
    }
    fn content_text(&mut self, t: &[u8]) {
        self.close_text();
        self.open_text();
        self.put(t);
    }
    fn content_opaque(&mut self, ty: &str, data: &[u8], mime: &str) {
        self.close_text();
        self.item_prefix();
        self.b.extend_from_slice(b"{\"type\":\"");
        self.b.extend_from_slice(ty.as_bytes());
        self.b.extend_from_slice(b"\",\"data\":\"");
        self.b.extend_from_slice(data);
        self.b.extend_from_slice(b"\",\"mimeType\":\"");
        self.b.extend_from_slice(mime.as_bytes());
        self.b.extend_from_slice(b"\"}");
        self.items = self.items.wrapping_add(1);
    }
    fn content_resource(&mut self, uri: &str, mime: &str, text: &[u8]) {
        self.close_text();
        self.item_prefix();
        self.b.extend_from_slice(b"{\"type\":\"resource\",\"resource\":{\"uri\":\"");
        self.b.extend_from_slice(uri.as_bytes());
        self.b.extend_from_slice(b"\",\"mimeType\":\"");
        self.b.extend_from_slice(mime.as_bytes());
        self.b.extend_from_slice(b"\",\"text\":\"");
        self.escape_inline(text);
        self.b.extend_from_slice(b"\"}}");
        self.items = self.items.wrapping_add(1);
    }
    fn end(&mut self) { self.close_text(); self.b.push(b']'); }
}

fn tool_result_body(text: &[u8], is_error: bool) -> Vec<u8> {
    let mut c = Cs::new();
    c.b.extend_from_slice(b"\"content\":[");
    c.put(text);
    c.end();
    c.b.extend_from_slice(b",\"isError\":");
    c.b.extend_from_slice(if is_error { b"true" } else { b"false" });
    c.b
}

fn tool_rich_body() -> Vec<u8> {
    let mut c = Cs::new();
    c.b.extend_from_slice(b"\"content\":[");
    c.put(b"plain");
    c.content_text(b"second");
    c.content_opaque("image", b"aGVsbG8=", "image/png");
    c.content_opaque("audio", b"AAEC", "audio/wav");
    c.content_resource("bt-mcp://dev/log", "text/plain", b"res");
    c.end();
    /* v0.5 V5：_meta 在 isError 之前（与 C 侧同字节） */
    c.b.extend_from_slice(b",\"_meta\":{\"io.btmcp/ver\":\"0.5\"}");
    c.b.extend_from_slice(b",\"isError\":false");
    c.b
}

fn json_escape_inner(s: &[u8]) -> Vec<u8> {
    let full = json_escape(s);
    full[1..full.len() - 1].to_vec()
}

fn tool_echo(params_raw: &[u8]) -> Result<Vec<u8>, ()> {
    let Some(v) = get_str_value(params_raw, b"text") else { return Err(()); };
    let mut t = decode_str(&v, 0, v.len());
    t.truncate(MAX_MSG - 1);
    Ok(t)
}

/* mock 的 consent_cb：arguments 里出现 "allow":true 才放行 */
fn consent_allows(params: &[u8]) -> bool {
    let args = match find_key(params, b"arguments") {
        Some((v0, v1)) => &params[v0..v1],
        None => return false,
    };
    let needle = b"\"allow\":true";
    args.windows(needle.len()).any(|w| w == needle)
}

fn rx_one(sv: &mut McpSrv, json: &[u8]) -> i32 {
    let r = match parse_req(json) {
        Ok(r) => r,
        Err(Some(part)) => {
            return respond_err(sv, &part, -32700, "Parse error");
        }
        Err(None) => return E_PARSE,
    };
    if r.method == b"initialize" {
        let body = body_initialize(sv, &r);
        return respond(sv, &r, &body);
    }
    if r.method == b"notifications/initialized" {
        sv.initialized = true;
        sv.sess.mark_ready();
        return 0;
    }
    if !sv.initialized {
        if !r.has_id { return E_STATE; }
        return respond_err(sv, &r, -32001, "Server not initialized");
    }
    if r.method == b"notifications/cancelled" { return 0; }
    if r.method == b"ping" { return respond(sv, &r, b""); }   /* 规范：空 result */
    if r.method == b"tools/list" {
        let body = tools_list(sv, &r);
        return respond(sv, &r, &body);
    }
    if r.method == b"tools/call" {
        let Some(name_raw) = get_str_value(&r.params, b"name") else {
            return respond_err(sv, &r, -32602, "Invalid params");
        };
        let name_val = name_raw[1..name_raw.len() - 1].to_vec();
        let Some(tool) = sv.tool_by_name(&name_val) else {
            return respond_err(sv, &r, -32602, "Invalid params");
        };
        let authz = tool.authz;
        let kind = tool.kind;
        if authz == 2 { return respond_err(sv, &r, -32003, "Permission denied"); }
        if authz == 1 && !consent_allows(&r.params) {
            return respond_err(sv, &r, -32003, "Permission denied");
        }
        let body = if kind == 2 {
            tool_rich_body()
        } else if kind == 1 {
            tool_result_body(b"allowed", false)
        } else {
            let args: Vec<u8> = match find_key(&r.params, b"arguments") {
                Some((v0, v1)) if r.params[v0] == b'{' => r.params[v0..v1].to_vec(),
                _ => r.params.clone(),
            };
            match tool_echo(&args) {
                Ok(text) => tool_result_body(&text, false),
                Err(()) => tool_result_body(b"tool error", true),
            }
        };
        let rc = respond(sv, &r, &body);
        if rc == E_TOOBIG {
            return respond_err(sv, &r, -32603, "Result too large");
        }
        return rc;
    }
    if r.method == b"resources/list" {
        let body = resources_list(sv, &r);
        return respond(sv, &r, &body);
    }
    if r.method == b"resources/templates/list" {
        let body = templates_list(sv, &r);
        return respond(sv, &r, &body);
    }
    if r.method == b"completion/complete" {
        let body = complete_body(&r);
        return respond(sv, &r, &body);
    }
    if r.method == b"resources/read" {
        match resources_read(&r) {
            Ok(body) => return respond(sv, &r, &body),
            Err(-2) => return respond_err(sv, &r, -32002, "Resource not found"),
            Err(_) => return respond_err(sv, &r, -32602, "Invalid params"),
        }
    }
    if r.method == b"resources/subscribe" || r.method == b"resources/unsubscribe" {
        let sub = r.method == b"resources/subscribe";
        let Some(uri_raw) = get_str_value(&r.params, b"uri") else {
            return respond_err(sv, &r, -32602, "Invalid params");
        };
        let uri = &uri_raw[1..uri_raw.len() - 1];
        let Some((name, _)) = parse_resource_uri(uri) else {
            return respond_err(sv, &r, -32602, "Invalid params");
        };
        let Some(idx) = RESOURCES.iter().position(|d| d.name.as_bytes() == &name[..])
            else { return respond_err(sv, &r, -32602, "Invalid params"); };
        if !RESOURCES[idx].sub { return respond_err(sv, &r, -32602, "Invalid params"); }
        let have = sv.subs.iter().position(|&x| x as usize == idx);
        if sub {
            if have.is_none() {
                if sv.subs.len() >= 8 { return respond_err(sv, &r, -32603, "Subscribe failed"); }
                sv.subs.push(idx as u16);
            }
        } else if let Some(h) = have {
            sv.subs.remove(h);
        }
        return respond(sv, &r, b"");
    }
    if r.method == b"prompts/list" {
        let body = prompts_list(&r);
        return respond(sv, &r, &body);
    }
    if r.method == b"prompts/get" {
        match prompts_get(&r) {
            Ok(body) => return respond(sv, &r, &body),
            Err(-2) => return respond_err(sv, &r, -32602, "Prompt not found"),
            Err(_) => return respond_err(sv, &r, -32602, "Invalid params"),
        }
    }
    if r.method == b"logging/setLevel" {
        let Some(lv) = get_str_value(&r.params, b"level") else {
            return respond_err(sv, &r, -32602, "Invalid params");
        };
        let name = &lv[1..lv.len() - 1];
        let names: [&[u8]; 8] = [b"debug", b"info", b"notice", b"warning",
                                 b"error", b"critical", b"alert", b"emergency"];
        let Some(pos) = names.iter().position(|n| *n == name) else {
            return respond_err(sv, &r, -32602, "Invalid params");
        };
        sv.log_level = pos as u8;
        return respond(sv, &r, b"");
    }
    if r.has_id {
        return respond_err(sv, &r, -32601, "Method not found");
    }
    0
}

fn batch_supported(sv: &McpSrv) -> bool {
    matches!(sv.proto.as_str(), "2025-03-26" | "2024-11-05" | "2024-10-07")
}

/* 与 C 的 batch_next 等价：从 pos 起切一个完整 JSON 值（跳过字符串内括号） */
fn batch_next(buf: &[u8], pos: usize) -> Option<usize> {
    let mut p = pos;
    while p < buf.len() && matches!(buf[p], b' ' | b'\t' | b'\r' | b'\n') { p += 1; }
    if p >= buf.len() { return None; }
    let start = p;
    let mut depth = 0i32;
    let mut instr = false;
    let mut esc = false;
    while p < buf.len() {
        let ch = buf[p];
        if instr {
            if esc { esc = false; }
            else if ch == b'\\' { esc = true; }
            else if ch == b'"' { instr = false; }
            p += 1;
            continue;
        }
        match ch {
            b'"' => instr = true,
            b'{' | b'[' => depth += 1,
            b'}' | b']' => depth -= 1,
            b',' if depth == 0 => break,
            _ => {}
        }
        p += 1;
    }
    if p == start { None } else { Some(p - start) }
}

fn server_rx(sv: &mut McpSrv, json: &[u8]) -> i32 {
    let mut i = 0usize;
    while i < json.len() && matches!(json[i], b' ' | b'\t' | b'\r' | b'\n') { i += 1; }
    if i < json.len() && json[i] == b'[' {
        return rx_batch(sv, &json[i..]);
    }
    rx_one(sv, json)
}

fn rx_batch(sv: &mut McpSrv, buf: &[u8]) -> i32 {
    if !batch_supported(sv) {
        let Some(j) = error_json(0, None, -32600, "Batch not supported")
            else { return E_TOOBIG; };
        /* id 用 null：error_json(0,None) 会写 0，这里手工构造 null 版本 */
        let mut v = Vec::new();
        v.extend_from_slice(b"{\"jsonrpc\":\"2.0\",\"id\":null,\"error\":{\"code\":-32600,\"message\":");
        v.extend_from_slice(&json_escape(b"Batch not supported"));
        v.extend_from_slice(b"}}");
        let _ = j;
        return sv.sess.tx(&v);
    }
    sv.batching = true;
    sv.bout.clear();
    let mut pos = 0usize;
    while pos < buf.len() && matches!(buf[pos], b' ' | b'\t' | b'\r' | b'\n' | b'[') { pos += 1; }
    let mut rc = 0;
    loop {
        let Some(elen) = batch_next(buf, pos) else { break; };
        let r1 = rx_one(sv, &buf[pos..pos + elen]);
        if r1 != 0 && r1 != E_PENDING { rc = r1; }
        pos += elen;
        while pos < buf.len() && matches!(buf[pos], b',' | b' ' | b'\t' | b'\r' | b'\n') { pos += 1; }
        if pos >= buf.len() || buf[pos] == b']' { break; }
    }
    sv.batching = false;
    if rc != 0 { return rc; }
    if sv.bout.is_empty() { return 0; }
    let mut out = Vec::with_capacity(sv.bout.len() + 2);
    out.push(b'[');
    out.extend_from_slice(&sv.bout);
    out.push(b']');
    sv.sess.tx(&out)
}

/* ---------------- 驱动模式 ---------------- */

fn load_tools(input: &[u8], off: &mut usize) -> Result<(Vec<McpTool>, usize), i32> {
    if *off >= input.len() { return Err(2); }
    let tools_count = input[*off] as usize; *off += 1;
    if tools_count > 16 { return Err(2); }
    let mut tools = Vec::with_capacity(tools_count);
    for _ in 0..tools_count {
        if *off + 2 > input.len() { return Err(2); }
        let nl = input[*off] as usize; *off += 1;
        if nl > 63 || *off + nl + 1 > input.len() { return Err(2); }
        let name = input[*off..*off + nl].to_vec(); *off += nl;
        let dl = input[*off] as usize; *off += 1;
        if dl > 127 || *off + dl > input.len() { return Err(2); }
        let desc = input[*off..*off + dl].to_vec(); *off += dl;
        tools.push(McpTool { name: String::from_utf8_lossy(&name).into_owned(),
                             desc: String::from_utf8_lossy(&desc).into_owned(),
                             title: None, output_schema: None,
                             annotations: 0, authz: 0, kind: 0 });
    }
    /* 与 mock 一致：追加 consent_tool / restricted_tool / rich_tool */
    let n = tools.len();
    if n + 2 <= 16 {
        tools.push(McpTool { name: "consent_tool".to_string(),
            desc: "needs consent".to_string(),
            title: Some("Consent tool".to_string()),
            output_schema: Some("{\"type\":\"object\"}".to_string()),
            annotations: 1 | 4, authz: 1, kind: 1 });
        tools.push(McpTool { name: "restricted_tool".to_string(),
            desc: "always denied".to_string(),
            title: None, output_schema: None,
            annotations: 2, authz: 2, kind: 1 });
    }
    if tools.len() + 1 <= 16 {
        tools.push(McpTool { name: "rich_tool".to_string(),
            desc: "multi content".to_string(),
            title: None, output_schema: None,
            annotations: 0, authz: 0, kind: 2 });
    }
    Ok((tools, *off))
}

fn run_mcp(input: &[u8], out: &mut impl Write) -> Result<(), i32> {
    if input.len() < 3 { return Err(2); }
    let packet = input[0] != 0;
    let mtu = u16::from_le_bytes([input[1], input[2]]);
    let mut off = 3usize;
    let (tools, o) = load_tools(input, &mut off)?;
    off = o;
    if packet {
        let m = mtu.min(261) as usize;
        if m <= HDR { return Err(2); }
    }
    let mut s = Sess::new(packet, mtu);
    s.link_up(0);
    let mut srv = McpSrv { sess: s, tools, initialized: false,
                           log_level: 1, subs: Vec::new(),
                           proto: String::new(), batching: false,
                           bout: Vec::new() };

    while off < input.len() {
        let op = input[off]; off += 1;
        if op != 0x07 { return Err(2); }
        if off + 2 > input.len() { return Err(2); }
        let l = u16::from_le_bytes([input[off], input[off + 1]]) as usize;
        off += 2;
        if off + l > input.len() { return Err(2); }
        let msg = &input[off..off + l];
        off += l;

        srv.sess.wire.clear();
        let rc = server_rx(&mut srv, msg);
        wr_i32s(out, rc);
        wr_u32s(out, srv.sess.wire.len());
        out.write_all(&srv.sess.wire).unwrap();
        wr_u32s(out, 0usize);
        srv.sess.wire.clear();
    }
    Ok(())
}

/* ---- e2e ---- */
struct E2e {
    cl: Sess,
    sv: McpSrv,
    c2s: Vec<u8>,
    s2c: Vec<u8>,
}

impl E2e {
    fn pump_line(&mut self, wire: &[u8], client_side: bool,
                 recs: &mut Vec<(i32, Vec<u8>)>) -> i32 {
        let mut msgs: Vec<(u8, u8, Vec<u8>)> = Vec::new();
        let mut off = 0usize;
        while off < wire.len() {
            msgs.clear();
            if self.cl.packet {
                if off + 2 > wire.len() { return E_PARSE; }
                let flen = u16::from_le_bytes([wire[off], wire[off + 1]]) as usize;
                off += 2;
                if off + flen > wire.len() { return E_PARSE; }
                let frag = &wire[off..off + flen];
                off += flen;
                let rc = if client_side {
                    self.cl.feed_packet(frag, &mut msgs)
                } else {
                    self.sv.sess.feed_packet(frag, &mut msgs)
                };
                if rc != 0 { return rc; }
            } else {
                let rc = if client_side {
                    self.cl.feed_stream(wire, &mut msgs)
                } else {
                    self.sv.sess.feed_stream(wire, &mut msgs)
                };
                if rc != 0 { return rc; }
                off = wire.len();
            }
            for (kind, seq, payload) in msgs.iter() {
                let rc = if client_side {
                    let rc = self.cl.rx(*kind, *seq, payload);
                    if rc == 0 && *kind == K_MCP && recs.len() < 8 {
                        recs.push((rc, payload.clone()));
                    }
                    rc
                } else {
                    /* server 侧：门/去重 → 上抛 server_rx → 回补（判例同 C） */
                    match self.sv.sess.rx_pre(*kind, *seq, payload) {
                        RxPre::Rc(rc) => rc,
                        RxPre::Deliver => {
                            let sink_rc = server_rx(&mut self.sv, payload);
                            self.sv.sess.rx_post(*seq);
                            sink_rc
                        }
                    }
                };
                if rc != 0 { return rc; }
            }
        }
        if client_side { let _ = self.cl.pump(); } else { let _ = self.sv.sess.pump(); }
        0
    }

    fn pump_all(&mut self, recs: &mut Vec<(i32, Vec<u8>)>) -> i32 {
        for _ in 0..16 {
            if !self.c2s.is_empty() {
                let w = std::mem::take(&mut self.c2s);
                let rc = self.pump_line(&w, false, recs);
                self.c2s.extend_from_slice(&std::mem::take(&mut self.cl.wire));
                self.s2c.extend_from_slice(&std::mem::take(&mut self.sv.sess.wire));
                if rc != 0 { return rc; }
                continue;
            }
            if !self.s2c.is_empty() {
                let w = std::mem::take(&mut self.s2c);
                let rc = self.pump_line(&w, true, recs);
                self.c2s.extend_from_slice(&std::mem::take(&mut self.cl.wire));
                self.s2c.extend_from_slice(&std::mem::take(&mut self.sv.sess.wire));
                if rc != 0 { return rc; }
                continue;
            }
            return 0;
        }
        E_AGAIN
    }
}

fn run_e2e(input: &[u8], out: &mut impl Write) -> Result<(), i32> {
    if input.len() < 3 { return Err(2); }
    let packet = input[0] != 0;
    let mtu = u16::from_le_bytes([input[1], input[2]]);
    let mut off = 3usize;
    let (tools, o) = load_tools(input, &mut off)?;
    off = o;
    if packet {
        let m = mtu.min(261) as usize;
        if m <= HDR { return Err(2); }
    }
    let mut e = E2e {
        cl: Sess::new(packet, mtu),
        sv: McpSrv { sess: Sess::new(packet, mtu), tools, initialized: false,
                     log_level: 1, subs: Vec::new(), proto: String::new(),
                     batching: false, bout: Vec::new() },
        c2s: Vec::new(),
        s2c: Vec::new(),
    };

    while off < input.len() {
        let op = input[off]; off += 1;
        let mut rc = 0i32;
        let mut recs: Vec<(i32, Vec<u8>)> = Vec::new();
        match op {
            0x08 => {
                if off + 2 > input.len() { return Err(2); }
                let l = u16::from_le_bytes(input[off..off + 2].try_into().unwrap()) as usize;
                off += 2;
                if off + l > input.len() { return Err(2); }
                let msg = input[off..off + l].to_vec(); off += l;
                e.cl.wire.clear();
                rc = e.cl.tx(&msg);
                let w = std::mem::take(&mut e.cl.wire);
                e.c2s.extend_from_slice(&w);
                let prc = e.pump_all(&mut recs);
                if prc != 0 { rc = prc; }
            }
            0x09 => {
                if off + 2 > input.len() { return Err(2); }
                let l = u16::from_le_bytes(input[off..off + 2].try_into().unwrap()) as usize;
                off += 2;
                if off + l > input.len() { return Err(2); }
                let msg = input[off..off + l].to_vec(); off += l;
                e.sv.sess.wire.clear();
                rc = server_rx(&mut e.sv, &msg);
                let w = std::mem::take(&mut e.sv.sess.wire);
                e.s2c.extend_from_slice(&w);
                let prc = e.pump_all(&mut recs);
                if prc != 0 { rc = prc; }
            }
            0x0A => {
                if off + 4 > input.len() { return Err(2); }
                let tok = u32::from_le_bytes(input[off..off + 4].try_into().unwrap());
                off += 4;
                e.cl.link_up(0);
                e.sv.sess.link_up(tok);
                e.c2s.extend_from_slice(&std::mem::take(&mut e.cl.wire));
                e.s2c.extend_from_slice(&std::mem::take(&mut e.sv.sess.wire));
            }
            0x0B => {
                if off >= input.len() { return Err(2); }
                let bond = input[off] != 0; off += 1;
                e.cl.link_down(bond);
                e.sv.sess.link_down(bond);
                e.c2s.extend_from_slice(&std::mem::take(&mut e.cl.wire));
                e.s2c.extend_from_slice(&std::mem::take(&mut e.sv.sess.wire));
            }
            0x0C => {
                if off + 4 > input.len() { return Err(2); }
                let now = u32::from_le_bytes(input[off..off + 4].try_into().unwrap());
                off += 4;
                e.cl.tick(now);
                e.sv.sess.tick(now);
                /* C 侧 emit 直通交叉线；Rust 需显式搬运 tick 产生的重传帧 */
                e.c2s.extend_from_slice(&std::mem::take(&mut e.cl.wire));
                e.s2c.extend_from_slice(&std::mem::take(&mut e.sv.sess.wire));
            }
            _ => return Err(2),
        }
        wr_i32s(out, rc);
        wr_u32s(out, 0usize);
        wr_u32s(out, recs.len());
        for (mrc, m) in recs.iter() {
            wr_i32s(out, *mrc);
            wr_u32s(out, m.len());
            out.write_all(m).unwrap();
        }
    }
    Ok(())
}

fn main() {
    let mode = std::env::args().nth(1).unwrap_or_default();
    let mut input = Vec::new();
    std::io::stdin().read_to_end(&mut input).unwrap();
    let mut out = std::io::stdout().lock();

    match mode.as_str() {
        "config" => {
            /* v0.6 N3：配置签名（与 C 侧同形，供差分矩阵前置比对） */
            let fcs = if fcs16_on() { 1 } else { 0 };
            println!(concat!(
                "{{\"impl\":\"rust\",\"fcs16\":{},\"max_reasm\":{},",
                "\"shared\":{},\"max_msg\":{},\"tx_slots\":{},",
                "\"rx_fp\":{},\"max_frag\":{},\"page\":{},",
                "\"pending\":{},\"subs\":{},\"s2c\":{},\"conn\":{},",
                "\"rto_max\":{},\"batch_buf\":{},\"meta_max\":{}}}"),
                fcs, max_reasm(),
                if max_reasm() == 1 { 1 } else { 0 },
                MAX_MSG, TX_SLOTS, RX_FP_SLOTS, MAX_FRAG, 8,
                2, 8, 2, 4, RTO_MAX, 4096, 256);
            return;
        }
        "session" => {
            if let Err(rc) = run_session(&input, &mut out) {
                std::process::exit(rc);
            }
        }
        "mcp" => {
            if let Err(rc) = run_mcp(&input, &mut out) {
                std::process::exit(rc);
            }
        }
        "e2e" => {
            if let Err(rc) = run_e2e(&input, &mut out) {
                std::process::exit(rc);
            }
        }
        "encode" => {
            let kind = u32::from_le_bytes(input[0..4].try_into().unwrap());
            let mtu = u16::from_le_bytes(input[4..6].try_into().unwrap());
            let len = u32::from_le_bytes(input[6..10].try_into().unwrap()) as usize;
            if 10 + len > input.len() { std::process::exit(2); }
            match encode(kind, mtu, &input[10..10 + len]) {
                Some(frames) => out.write_all(&frames).unwrap(),
                None => std::process::exit(3),
            }
        }
        "decode-p" | "decode-s" => {
            let msgs = if mode == "decode-p" {
                decode_packet(&input)
            } else {
                decode_stream(&input)
            };
            for (kind, msg) in msgs.unwrap_or_default() {
                out.write_all(&(kind as u32).to_le_bytes()).unwrap();
                out.write_all(&(msg.len() as u32).to_le_bytes()).unwrap();
                out.write_all(&msg).unwrap();
            }
        }
        _ => eprintln!("usage: oracle encode|decode-p|decode-s|session|mcp|e2e"),
    }
}
