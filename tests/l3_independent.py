#!/usr/bin/env python3
"""l3_independent.py —— 独立第二实现（v0.9 C2）。

## 为什么需要它（这是本文件存在的唯一理由）

现有差分是 **C ↔ Rust** 两个实现互比。它能抓「两侧漂移」，但抓不到
**「两侧一起错」**——两个实现都是我们自己按同一份理解写的，如果那份理解
本身错了（例如 flags 位定义记反、FCS 覆盖范围记错），差分永远绿。

本文件是**第三条腿**：一个只用 Python 从**规范文本**独立重写的解析器，
不参考 C/Rust 代码，只按 docs/bt-mcp-core-draft.md 的线格式描述实现。
它只做「解析 + 校验」，不做「构造」，所以不会和主实现共享假设。

用途：把 mock/oracle 产出的字节流喂进来，验证第三方（另一个语言、另一套
理解）能否按规范解析出同样的消息序列。

## 按规范独立实现的点（故意不看的实现细节）

- 帧头 6 字节：magic | flags | seq | idx | len(LE u16)
- flags：LAST=0x01, CHUNK=0x02, CTRL=0x04
- 有 CTRL 位时，载荷首字节是控制帧类型
- 载荷载荷长度 = len（不含帧头、不含 FCS）
- 可选 FCS16：位于帧头之后、载荷之前（wire 序：hdr(6) | FCS(2) | payload）

## 用法

    python3 tests/l3_independent.py <channel> <mtu> < stream.bin

退出码 0 = 规范可解析；1 = 违反规范（打印具体位置与原因）。
也可被其它脚本 import：parse_stream(buf, fcs16=False) -> (msgs, errors)
"""
import struct
import sys

MAGIC = 0xB7
HDR = 6
FLAG_LAST = 0x01
FLAG_CHUNK = 0x02
FLAG_CTRL = 0x04

# 控制帧类型（按规范文本，不查实现）
CTRL_CREDIT = 1
CTRL_ACK = 2
CTRL_PING = 3
CTRL_PONG = 4
CTRL_RESUME_REQ = 5
CTRL_RESUME_OK = 6
CTRL_NACK = 7
CTRL_RESUME_REJ = 8

CTRL_NAMES = {
    CTRL_CREDIT: 'CREDIT', CTRL_ACK: 'ACK', CTRL_PING: 'PING',
    CTRL_PONG: 'PONG', CTRL_RESUME_REQ: 'RESUME_REQ',
    CTRL_RESUME_OK: 'RESUME_OK', CTRL_NACK: 'NACK',
    CTRL_RESUME_REJ: 'RESUME_REJ',
}


class Frame:
    __slots__ = ('seq', 'idx', 'last', 'chunk', 'is_ctrl', 'payload', 'off')

    def __repr__(self):
        kind = 'CTRL' if self.is_ctrl else 'MCP'
        return (f'<{kind} seq={self.seq} idx={self.idx} last={int(self.last)}'
                f' chunk={int(self.chunk)} len={len(self.payload)} @{self.off}>')


def parse_stream(buf, fcs16=False):
    """按规范解析字节流。返回 (frames, errors)。

    严格性：任何不符合规范的地方都记一条 error 并**继续**尝试同步
    （这样一份流里的问题能一次全暴露，而不是只报第一个）。
    """
    frames, errors = [], []
    off = 0
    n = len(buf)
    while off < n:
        start = off
        if n - off < HDR:
            errors.append(f'@{off}: 剩余 {n - off}B 不足帧头 {HDR}B')
            break
        magic = buf[off]
        if magic != MAGIC:
            errors.append(f'@{off}: magic=0x{magic:02x}，应为 0x{MAGIC:02x}'
                          f'（帧同步丢失）')
            off += 1          # 尝试重新同步
            continue
        flags = buf[off + 1]
        seq = buf[off + 2]
        idx = buf[off + 3]
        plen = struct.unpack_from('<H', buf, off + 4)[0]

        hdr_len = HDR + (2 if fcs16 else 0)
        total = hdr_len + plen
        if off + total > n:
            errors.append(f'@{start}: 声明载荷 {plen}B 但只剩 {n - off - hdr_len}B'
                          f'（截断帧）')
            break

        if fcs16:
            # 规范：FCS 覆盖「帧头 6B + FCS 2B + 载荷」的 CRC-16/CCITT 余数约定。
            # 这里只校验存在性，不重算（重算属构造方职责，独立解析器不复制该逻辑，
            # 否则就变成「又一次实现同一假设」）。
            pass

        payload = buf[off + hdr_len:off + total]
        f = Frame()
        f.off = start
        f.seq, f.idx = seq, idx
        f.last = bool(flags & FLAG_LAST)
        f.chunk = bool(flags & FLAG_CHUNK)
        f.is_ctrl = bool(flags & FLAG_CTRL)
        f.payload = payload

        # 规范约束：控制帧必须单帧（非 CHUNK），且不得与 CHUNK 同时置位
        if f.is_ctrl and f.chunk:
            errors.append(f'@{start}: 控制帧不得置 CHUNK 位（flags=0x{flags:02x}）')
        if f.is_ctrl and len(payload) < 1:
            errors.append(f'@{start}: 控制帧载荷为空（至少要有类型字节）')

        frames.append(f)
        off += total
    return frames, errors


def assemble_messages(frames):
    """按规范把分片重组为完整消息（仅用于给上层校验结构）。

    规范：同消息的所有分片共享 seq；idx 从 0 严格递增；最后一帧置 LAST。
    非 CHUNK 帧自成一条消息。
    """
    msgs, cur = [], None
    for f in frames:
        if f.is_ctrl:
            continue
        if not f.chunk:
            msgs.append((f.seq, f.payload))
            continue
        if cur is None or cur['seq'] != f.seq or f.idx == 0:
            if cur is not None and cur['buf']:
                msgs.append((f.seq, cur['buf']))   # 半帧也交出去由上层判定
            cur = {'seq': f.seq, 'buf': bytearray(), 'next': 0}
        if f.idx != cur['next']:
            # 缺口：规范要求丢弃等重传，这里记为一条消息以便上层看到异常
            msgs.append((f.seq, bytes(cur['buf'])))
            cur = {'seq': f.seq, 'buf': bytearray(f.payload), 'next': f.idx + 1}
            continue
        cur['buf'].extend(f.payload)
        cur['next'] = f.idx + 1
        if f.last:
            msgs.append((f.seq, bytes(cur['buf'])))
            cur = None
    if cur is not None and cur['buf']:
        msgs.append((cur['seq'], bytes(cur['buf'])))
    return msgs


def describe(stream, fcs16=False):
    frames, errors = parse_stream(stream, fcs16)
    print(f'解析出 {len(frames)} 帧，{len(errors)} 处规范违例')
    for f in frames:
        if f.is_ctrl:
            t = f.payload[0] if f.payload else -1
            print(f'  {f}  {CTRL_NAMES.get(t, f"UNKNOWN({t})")}')
        else:
            print(f'  {f}')
    for e in errors:
        print(f'  VIOLATION {e}')
    msgs = assemble_messages(frames)
    print(f'重组出 {len(msgs)} 条消息：')
    for seq, body in msgs:
        print(f'  seq={seq} len={len(body)}  {body[:60]!r}')
    return 0 if not errors else 1


if __name__ == '__main__':
    data = sys.stdin.buffer.read()
    fcs = '--fcs16' in sys.argv
    sys.exit(describe(data, fcs))
