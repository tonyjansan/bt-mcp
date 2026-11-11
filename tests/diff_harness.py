#!/usr/bin/env python3
"""bt-mcp M1 差分 harness：C mock ↔ Rust oracle 双向字节差分。

三条路线全做（各 N 轮随机样本）：
  A. encode 方向差分：随机消息 → C encode vs Rust encode → 字节必须全等
  B. decode 方向差分：随机帧流 → C decode vs oracle decode → 消息序列全等
  C. 交叉解码：C encode 产物喂 oracle decode（反向亦然）→ 互通语义等

用法：python3 diff_harness.py [rounds_per_route]
"""
import random
import struct
import subprocess
import sys
import os

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
MOCK = os.environ.get("BT_MCP_MOCK") or os.path.join(ROOT, "build", "btmcp_mock")
ORACLE = os.path.join(ROOT, "oracle", "target", "release", "oracle")

MTUS = [0, 23, 27, 53, 128, 185, 244, 247, 261, 517]  # 0 = 流通道


def run(binpath, mode, data):
    p = subprocess.run([binpath, mode], input=data,
                       capture_output=True, timeout=10)
    return p.returncode, p.stdout


def c_encode(kind, mtu, payload):
    data = struct.pack("<IHI", kind, mtu, len(payload)) + payload
    rc, out = run(MOCK, "encode", data)
    return rc, out


def oracle_encode(kind, mtu, payload):
    data = struct.pack("<IHI", kind, mtu, len(payload)) + payload
    rc, out = run(ORACLE, "encode", data)
    return rc, out


def parse_msgs(blob):
    """[u32 kind | u32 len | msg]* → [(kind, bytes)]"""
    out, off = [], 0
    while off + 8 <= len(blob):
        kind, ln = struct.unpack_from("<II", blob, off)
        off += 8
        if off + ln > len(blob):
            raise ValueError("truncated msg record")
        out.append((kind, blob[off:off + ln]))
        off += ln
    if off != len(blob):
        raise ValueError("trailing garbage in msg record stream")
    return out


def decode_both(stream, c_mode, o_mode):
    rc_c, out_c = run(MOCK, c_mode, stream)
    rc_o, out_o = run(ORACLE, o_mode, stream)
    return (rc_c, out_c), (rc_o, out_o)


def rand_msg(rng):
    n = rng.choice([0, 1, 7, 20, 100, 500, 1000, 2000, 4000, 8192])
    return bytes(rng.getrandbits(8) for _ in range(n))


def main():
    rounds = int(sys.argv[1]) if len(sys.argv) > 1 else 200
    rng = random.Random(0xB7)  # 固定种子：可复现
    fails = 0

    for i in range(rounds):
        payload = rand_msg(rng)
        kind = rng.choice([0, 1])
        mtu = rng.choice(MTUS)
        note = f"[{i}] kind={kind} mtu={mtu} len={len(payload)}"

        # --- 路线 A：encode 字节差分 ---
        rc_c, out_c = c_encode(kind, mtu, payload)
        rc_o, out_o = oracle_encode(kind, mtu, payload)
        # 8192 消息在小 MTU 下可能超 MAX_FRAG=32 → 两边都必须以同一错误拒收
        if rc_c != 0 or rc_o != 0:
            if rc_c != rc_o:
                print(f"FAIL A-err {note}: c_rc={rc_c} o_rc={rc_o}")
                fails += 1
            continue
        if out_c != out_o:
            print(f"FAIL A {note}: {len(out_c)}B vs {len(out_o)}B")
            print("  c:", out_c[:48].hex())
            print("  o:", out_o[:48].hex())
            fails += 1
            continue

        # --- 路线 B：decode 各自解码同一帧流 ---
        frames = out_c
        if mtu == 0:
            # 流通道
            (rc_c2, out_c2), (rc_o2, out_o2) = decode_both(
                frames, "decode-s", "decode-s")
        else:
            # 包通道：帧流 → [u16 片长+片]*。解析 C 发送产物：6B 头 + plen
            pk = b""
            off = 0
            while off < len(frames):
                plen = struct.unpack_from("<H", frames, off + 4)[0]
                pk += struct.pack("<H", 6 + plen) + frames[off:off + 6 + plen]
                off += 6 + plen
            frames_in = pk
            (rc_c2, out_c2), (rc_o2, out_o2) = decode_both(
                frames_in, "decode-p", "decode-p")

        msgs_c = parse_msgs(out_c2) if rc_c2 == 0 else None
        msgs_o = parse_msgs(out_o2) if rc_o2 == 0 else None
        expect = [(kind, payload)]
        if msgs_c != expect:
            print(f"FAIL B-c {note}: c decoded {msgs_c and len(msgs_c)} msgs")
            fails += 1
            continue
        if msgs_o != expect:
            print(f"FAIL B-o {note}: oracle decoded {msgs_o and len(msgs_o)} msgs")
            fails += 1
            continue

    # --- 路线 C：交叉解码（用固定几组代表性样本） ---
    for kind, mtu, payload in [
        (0, 0, b"hello-stream"),
        (1, 0, b"\x01\x10\x00"),           # 控制帧（CREDIT）
        (0, 23, bytes(range(64)) * 3),      # 小 MTU 多片
        (0, 244, b"x" * 4000),              # 大消息
        (1, 128, b"\x03\xaa\xbb\xcc\xdd"), # RESUME_REQ
    ]:
        _, frames_c = c_encode(kind, mtu, payload)
        _, frames_o = oracle_encode(kind, mtu, payload)
        assert frames_c == frames_o
        if mtu == 0:
            (_, out_oc) = decode_both(frames_c, "decode-s", "decode-s")  # C 解 C 帧
            rc_co, out_co = run(ORACLE, "decode-s", frames_c)
            rc_oo, out_oo = run(MOCK, "decode-s", frames_o)
            assert rc_co == 0 and rc_oo == 0
            if parse_msgs(out_co) != parse_msgs(out_oo):
                print(f"FAIL C-stream kind={kind} len={len(payload)}")
                fails += 1
        else:
            pk = b""
            off = 0
            while off < len(frames_c):
                plen = struct.unpack_from("<H", frames_c, off + 4)[0]
                pk += struct.pack("<H", 6 + plen) + frames_c[off:off + 6 + plen]
                off += 6 + plen
            rc_co, out_co = run(ORACLE, "decode-p", pk)
            rc_cc, out_cc = run(MOCK, "decode-p", pk)
            if rc_co or rc_cc:
                print(f"FAIL C-rc kind={kind} mtu={mtu} rc_co={rc_co} rc_cc={rc_cc}")
                fails += 1
                continue
            m_o, m_c = parse_msgs(out_co), parse_msgs(out_cc)
            if m_o != m_c or m_o[0][1] != payload:
                print(f"FAIL C-pkt kind={kind} mtu={mtu}")
                fails += 1

    total = rounds * 2 + 5
    if fails:
        print(f"\nFAILED: {fails}/{total} checks")
        sys.exit(1)
    print(f"ALL GREEN: {total} checks (A={rounds} B={rounds} C=5)")


if __name__ == "__main__":
    main()
