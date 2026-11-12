#!/usr/bin/env python3
"""bt-mcp M3 差分 harness：L4 session 全栈（L3+L4）C mock ↔ Rust oracle。

路线 D：随机生成指令脚本（LINK_UP/TX/RX/CTRL/TICK/LINK_DOWN），
两侧 session 模式跑同一脚本，stdout 字节必须全等。

场景覆盖：正常握手+echo、READY 前越权流量、信用耗尽/CREDIT 回补、
RESUME token 正/误、断链 bond 作废、包通道乱序分片、流通道半帧跨指令、
PING/PONG、TICK 超时回收。

用法：python3 m3_session_harness.py [rounds]
"""
import random
import struct
import subprocess
import sys
import os

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
MOCK = os.environ.get("BT_MCP_MOCK") or os.path.join(ROOT, "build", "btmcp_mock")
ORACLE = os.path.join(ROOT, "oracle", "target", "release", "oracle")

CTRL_CREDIT = 1
CTRL_PING = 2
CTRL_RESUME_REQ = 3
CTRL_RESUME_OK = 4
CTRL_RESUME_REJ = 5
CTRL_ACK = 6
CTRL_NACK = 7


def frame_msg(seq, flags, idx, payload):
    """构造一帧（小端 6B 头）。"""
    return bytes([0xB7, flags, seq, idx, len(payload) & 0xFF,
                  len(payload) >> 8]) + payload


def run(binpath, script):
    p = subprocess.run([binpath, "session"], input=script,
                       capture_output=True, timeout=10)
    return p.returncode, p.stdout


def op_link_up(token):
    return b"\x01" + struct.pack("<I", token)


def op_link_down(bond):
    return b"\x02" + bytes([bond])


def op_tx(payload):
    return b"\x03" + struct.pack("<H", len(payload)) + payload


def op_tx_ref(payload):
    """V8：引用式发送（大消息也享重传保护）。"""
    return b"\x08" + struct.pack("<H", len(payload)) + payload


def op_rx(wire):
    return b"\x04" + struct.pack("<H", len(wire)) + wire


def op_ctrl(type_, args):
    return b"\x05" + bytes([type_, len(args)]) + args


def op_tick(now):
    return b"\x06" + struct.pack("<I", now)


def pkt_wire(frames):
    """包通道 RX 输入格式：[u16 片长+片]*。"""
    out = b""
    for fr in frames:
        out += struct.pack("<H", len(fr)) + fr
    return out


def mcp(s):
    """近似 MCP JSON 的消息体。"""
    return s.encode()



def op_set_mtu(mtu):
    """v0.7（A7）：MTU 热更新。"""
    return b"\x09" + struct.pack("<H", mtu)



SCENARIOS = []


def scenario(fn):
    SCENARIOS.append(fn)
    return fn


@scenario
def happy_path(rng):
    """流通道：LINK_UP → initialize（经 RX 粗门）→ TX → PING/PONG。"""
    init = mcp('{"jsonrpc":"2.0","id":1,"method":"initialize","params":{}}')
    wire = frame_msg(9, 0x01, 0, init)  # LAST
    s = op_link_up(0x100)
    s += op_rx(wire)
    s += op_tx(mcp("hello"))
    s += op_rx(frame_msg(10, 0x05, 0, bytes([CTRL_PING, 7])))  # ctrl 帧
    s += op_tick(100)
    return 0, 0, s


@scenario
def gate_before_ready(rng):
    """LINK_UP 期非 initialize MCP → E_STATE；initialize 直拼形态放行。"""
    wire = frame_msg(0, 0x01, 0, mcp("tools/list"))
    s = op_link_up(0x100)
    s += op_rx(wire)
    s += op_tx(mcp("still allowed at LINK_UP"))
    s += op_link_up(0x101)
    s += op_rx(frame_msg(1, 0x01, 0, b'{"jsonrpc'))
    return 0, 0, s


@scenario
def credit_packet(rng):
    """包通道：烧干窗口 → E_AGAIN → CREDIT 回补 → 再发。"""
    mtu = 261
    s = op_link_up(0x200)
    for i in range(5):  # 窗口 4，第 5 条 E_AGAIN
        s += op_tx(mcp(f"m{i}"))
    # RX 回补 CREDIT 3（单帧 ctrl 消息）
    s += op_rx(pkt_wire([frame_msg(50, 0x05, 0,
                 bytes([CTRL_CREDIT, 3, 0]))]))
    s += op_tx(mcp("after"))
    s += op_tx(mcp("again"))  # 补 3 只够 3 条 → 第 4 条又 E_AGAIN? （补后 3，用 1 剩 2）
    s += op_tx(mcp("x"))      # 再用 1 剩 1
    s += op_tx(mcp("y"))      # 剩 0 后这条 E_AGAIN
    return 1, mtu, s


@scenario
def credit_before_ready(rng):
    """READY 前 CREDIT → E_STATE。"""
    s = op_link_up(0)
    s += op_rx(pkt_wire([frame_msg(0, 0x05, 0,
                 bytes([CTRL_CREDIT, 1, 0]))]))
    return 1, 261, s


@scenario
def resume_ok_and_rej(rng):
    """RESUME_REQ：token 匹配 → OK 且直达 READY；不匹配 → REJ。"""
    # 第一次：server 侧（LINK_UP 持 token）
    s = op_link_up(0xABCD1234)
    s += op_ctrl(CTRL_RESUME_REQ, struct.pack("<I", 0xABCD1234))
    # 收到 OK（RX）→ READY
    s += op_rx(pkt_wire([frame_msg(0, 0x05, 0,
                 bytes([CTRL_RESUME_OK]) + struct.pack("<I", 0xABCD1234))]))
    s += op_tx(mcp("ready now"))
    # 断链（bond 保住）→ 再 RESUME 误 token → REJ
    s += op_link_down(1)
    s += op_link_up(0xABCD1234)
    s += op_rx(pkt_wire([frame_msg(1, 0x05, 0,
                 bytes([CTRL_RESUME_REQ]) + struct.pack("<I", 0xDEAD))]))
    return 1, 261, s


@scenario
def resume_timeout(rng):
    """RESUME_REQ 发出后超时 → resume_pending 清（后续行为一致即可）。"""
    s = op_link_up(0x77)
    s += op_ctrl(CTRL_RESUME_REQ, struct.pack("<I", 0x77))
    s += op_tick(1)   # t0=0? t0 取 now_ms=0 → tick(1)
    s += op_tick(4999)
    s += op_tick(5001)  # > 5000：清 pending
    s += op_tick(9999)
    return 1, 261, s


@scenario
def bond_lost_token(rng):
    """bond 丢 → token 作废：RESUME_REQ → REJ(1)。"""
    s = op_link_up(0xFACE)
    s += op_link_down(0)   # bond 丢
    s += op_link_up(0)     # token 已清零
    s += op_rx(pkt_wire([frame_msg(0, 0x05, 0,
                 bytes([CTRL_RESUME_REQ]) + struct.pack("<I", 0xFACE))]))
    return 1, 261, s


@scenario
def frag_packet(rng):
    """包通道多片消息：RX 重组后上抛；乱序分片 → E_SYNC。"""
    mtu = 53
    big = mcp("Z" * 300)  # 300B，chunk=47 → 7 片
    seq = 3
    frames = []
    off = 0
    idx = 0
    while off < len(big) or idx == 0:
        chunk = big[off:off + 47]
        last = off + len(chunk) >= len(big)
        frames.append(frame_msg(seq, 0x02 | (0x01 if last else 0),
                                idx, chunk))
        off += 47
        idx += 1
    s = op_link_up(0x1)
    s += op_rx(pkt_wire(frames))
    # 乱序：直接喂 idx=1 开头的两条
    bad = [frame_msg(9, 0x02, 1, b"aa"), frame_msg(9, 0x03, 0, b"bb")]
    s += op_rx(pkt_wire(bad))
    s += op_rx(pkt_wire([frame_msg(10, 0x03, 0, mcp("ok"))]))
    return 1, mtu, s


@scenario
def frag_timeout(rng):
    """半帧超时回收：TICK 超时后同 seq 可重新开槽。"""
    s = op_link_up(0x1)
    s += op_rx(pkt_wire([frame_msg(5, 0x02, 0, b"first frag")]))
    s += op_tick(1999)   # 未超时
    s += op_tick(2001)   # > 2000：回收
    s += op_rx(pkt_wire([frame_msg(5, 0x02, 0, b"again"),
                        frame_msg(5, 0x03, 1, b"done")]))
    return 1, 100, s


@scenario
def stream_half_frame(rng):
    """流通道半帧：一条 RX 指令只喂半帧，下一指令补齐 → 上抛。"""
    msg = frame_msg(0, 0x01, 0, mcp("split message"))
    half = msg[:8]
    rest = msg[8:]
    s = op_link_up(0x1)
    s += op_rx(half)
    s += op_rx(rest)
    # 失步滑窗：垃圾字节里藏 magic 帧头
    m2 = frame_msg(1, 0x01, 0, mcp("after garbage"))
    s += op_rx(b"\xff\xfe\xfd" + m2)
    # 半帧垃圾收尾（无完整帧）
    s += op_rx(b"\x00B7\x01")
    return 0, 0, s


@scenario
def ctrl_oversize_and_ping(rng):
    """PING 应答；控制帧超 255B → E_TOOBIG；流通道 CTRL 超 255 → E_SYNC。"""
    s = op_link_up(0x1)
    s += op_rx(frame_msg(0, 0x05, 0, bytes([CTRL_PING, 3])))
    s += op_ctrl(CTRL_PING, b"\x99")
    s += op_tx(b"")   # 空消息：单帧零载荷
    s += op_tx(b"Q" * 8193)  # E_TOOBIG
    return 0, 0, s


@scenario
def idle_gate(rng):
    """IDLE 期一切拒绝：TX/CTRL/RX 均 E_STATE。"""
    s = op_tx(mcp("nope"))
    s += op_ctrl(CTRL_PING, b"")
    s += op_rx(frame_msg(0, 0x01, 0, mcp("nope")))
    s += op_link_up(0x5)
    return 0, 0, s


@scenario
def s_v05_tx_ref(rng):
    """V8：>512B 消息用 tx_ref 发送 → 登记重传槽，超时后整条重发。"""
    mtu = 244
    body = (op_link_up(0) + op_tx_ref(b"x" * 900) +
            op_tick(1000) + op_tick(2000) + op_tick(3000))
    return 1, mtu, body


@scenario
def s_v05_tx_ref_vs_tx(rng):
    """V8 对照：同样 900B，普通 tx 不登记重传（超时后不重发），
    tx_ref 登记（超时后重发）——两者输出必须与各自语义一致。"""
    mtu = 244
    body = (op_link_up(0) + op_tx(b"y" * 900) +
            op_tick(1000) + op_tick(2000))
    return 1, mtu, body


def rand_script(rng, packet):
    """随机脚本：随机指令序列（含随机 MCP payload 与 wire 帧）。"""
    mtu = 0 if not packet else rng.choice([23, 53, 100, 244, 261])
    s = bytes([1 if packet else 0]) + struct.pack("<H", mtu)
    state_ready = False
    now = 0
    for _ in range(rng.randint(8, 30)):
        r = rng.random()
        if r < 0.08:
            s += op_link_up(rng.getrandbits(32))
        elif r < 0.14:
            s += op_link_down(rng.randint(0, 1))
        elif r < 0.45:
            n = rng.choice([0, 5, 20, 100, 800, 2000])
            s += op_tx(bytes(rng.getrandbits(8) for _ in range(n)))
        elif r < 0.75:
            # 随机 wire：一半概率合法帧、一半纯垃圾
            if rng.random() < 0.5:
                n = rng.randint(0, 4)
                frames = []
                for k in range(n):
                    if packet:
                        pl = bytes(rng.getrandbits(8)
                                   for _ in range(rng.randint(0, 50)))
                        frames.append(frame_msg(rng.getrandbits(8),
                                                rng.choice([0x00, 0x01, 0x02,
                                                            0x03, 0x04, 0x05,
                                                            0x07]),
                                                rng.getrandbits(8), pl))
                    else:
                        pl = bytes(rng.getrandbits(8)
                                   for _ in range(rng.randint(0, 50)))
                        frames.append(frame_msg(rng.getrandbits(8),
                                                rng.choice([0x00, 0x01, 0x04,
                                                                            0x05,
                                                                            0x06]),
                                                0, pl))
                wire = pkt_wire(frames) if packet else b"".join(frames)
            else:
                wire = bytes(rng.getrandbits(8)
                             for _ in range(rng.randint(0, 60)))
            s += op_rx(wire)
        elif r < 0.9:
            t = rng.choice([CTRL_CREDIT, CTRL_PING, CTRL_RESUME_REQ,
                            CTRL_RESUME_OK, CTRL_RESUME_REJ,
                            CTRL_ACK, CTRL_NACK, 0x77])
            args = bytes(rng.getrandbits(8)
                         for _ in range(rng.choice([0, 1, 2, 4, 8])))
            s += op_ctrl(t, args)
        else:
            now += rng.randint(0, 3000)
            s += op_tick(now)
    return (1 if packet else 0), mtu, s




def main():
    rounds = int(sys.argv[1]) if len(sys.argv) > 1 else 200
    rng = random.Random(0xB70710C0)  # 固定种子：可复现
    fails = 0
    checks = 0

    for i, fn in enumerate(SCENARIOS):
        pkt_f, mtu, body = fn(rng)
        script = bytes([pkt_f]) + struct.pack("<H", mtu) + body
        rc_c, out_c = run(MOCK, script)
        rc_o, out_o = run(ORACLE, script)
        checks += 1
        if (rc_c, out_c) != (rc_o, out_o):
            print(f"FAIL scenario {fn.__name__}: rc {rc_c} vs {rc_o}, "
                  f"{len(out_c)}B vs {len(out_o)}B")
            for j, (a, b) in enumerate(zip(out_c, out_o)):
                if a != b:
                    print(f"  first diff at byte {j}: {a:02x} vs {b:02x}")
                    break
            fails += 1

    for i in range(rounds):
        packet = rng.random() < 0.5
        _, _, script = rand_script(rng, packet)
        rc_c, out_c = run(MOCK, script)
        rc_o, out_o = run(ORACLE, script)
        checks += 1
        if (rc_c, out_c) != (rc_o, out_o):
            print(f"FAIL random [{i}] packet={packet}: rc {rc_c} vs {rc_o}, "
                  f"{len(out_c)}B vs {len(out_o)}B")
            for j, (a, b) in enumerate(zip(out_c, out_o)):
                if a != b:
                    print(f"  first diff at byte {j}: {a:02x} vs {b:02x}")
                    break
            fails += 1

    if fails:
        print(f"\nFAILED: {fails}/{checks} checks")
        sys.exit(1)
    print(f"ALL GREEN: {checks} checks (scenarios={len(SCENARIOS)} "
          f"random={rounds})")



@scenario
def v07_set_mtu(rng):
    """v0.7（A7）：MTU 热更新。

    小 MTU → 切大 MTU → 切更小 → 回退 0（用通道能力）。
    每次 set_mtu 后的 TX 都必须按**当时的** MTU 切片，两侧逐字节一致。
    """
    s = op_link_up(0)
    s += op_set_mtu(60)
    s += op_tx(mcp("small mtu message"))
    s += op_set_mtu(244)
    s += op_tx(mcp("big mtu message"))
    s += op_set_mtu(23)
    s += op_tx(mcp("tiny mtu message"))
    s += op_set_mtu(0)              # 0 = 回退到通道能力
    s += op_tx(mcp("after fallback"))
    return 1, 244, s                # 包通道 + 初始 MTU 244

if __name__ == "__main__":
    main()

