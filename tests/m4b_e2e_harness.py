#!/usr/bin/env python3
"""bt-mcp M4b 差分 harness：e2e 全栈（L2~L5）client/server 背靠背对话
C mock ↔ Rust oracle。

脚本信封与 mcp 模式一致：u8 chan | u16 mtu | u8 tools_count | [工具]* | 指令*
指令：0x08 CMSG u16+MCP → client session_tx
      0x09 SMSG u16+MCP → server bt_mcp_server_rx 直投
      0x0A LUP u32 token → 双方 link_up
      0x0B LDN u8 bond   → 双方 link_down
      0x0C TICK u32 now  → 双方 tick
两侧 e2e 模式跑同一脚本，stdout 逐字节对拍。

场景覆盖：完整 initialize 握手、SMSG 直投预置、包通道分片（mtu=23）、
断链重连（LDN→LUP）、粗门越权（LINK_UP 期非 initialize → E_STATE）、
credit 耗尽 E_AGAIN、TICK 半帧超时、随机脚本。

用法：python3 m4b_e2e_harness.py [rounds]
"""
import random
import struct
import subprocess
import sys
import os

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
MOCK = os.path.join(ROOT, "build", "btmcp_mock")
ORACLE = os.path.join(ROOT, "oracle", "target", "release", "oracle")

SCENARIOS = []


def scenario(fn):
    SCENARIOS.append(fn)
    return fn


CMSG, SMSG, LUP, LDN, TICK = b"\x08", b"\x09", b"\x0a", b"\x0b", b"\x0c"


def cmsg(m):
    return CMSG + struct.pack("<H", len(m)) + m


def smsg(m):
    return SMSG + struct.pack("<H", len(m)) + m


def lup(tok):
    return LUP + struct.pack("<I", tok)


def ldn(bond):
    return LDN + bytes([bond])


def tick(now):
    return TICK + struct.pack("<I", now)


def script(chan, mtu, tools, ops):
    s = bytes([chan]) + struct.pack("<H", mtu) + bytes([len(tools)])
    for n, d in tools:
        s += bytes([len(n)]) + n + bytes([len(d)]) + d
    return s + b"".join(ops)


def run(binpath, sc):
    p = subprocess.run([binpath, "e2e"], input=sc,
                       capture_output=True, timeout=10)
    return p.returncode, p.stdout


INIT = b'{"jsonrpc":"2.0","id":1,"method":"initialize","params":{}}'
INITED = b'{"jsonrpc":"2.0","method":"notifications/initialized"}'
LIST = b'{"jsonrpc":"2.0","id":2,"method":"tools/list"}'
CALL = b'{"jsonrpc":"2.0","id":3,"method":"tools/call","params":{"name":"echo","arguments":{"text":"hi"}}}'
PING = b'{"jsonrpc":"2.0","id":4,"method":"ping"}'


@scenario
def s_full_handshake(rng):
    """流通道完整握手：LUP→CMSG initialize→client 收 serverInfo 响应
    →SMSG notifications/initialized→tools/list→tools/call→ping。"""
    ops = [lup(0x11223344),
           cmsg(INIT),
           smsg(INITED),
           cmsg(LIST),
           cmsg(CALL),
           cmsg(PING)]
    return script(0, 0, [(b"echo", b"echo tool")], ops)


@scenario
def s_smsg_preset(rng):
    """LUP 前后 SMSG 直投：未 link_up 期 server 粗门拒绝（E_STATE），
    LUP 后 SMSG 才生效。"""
    ops = [smsg(INIT),
           smsg(INITED),
           lup(1),
           smsg(INIT),
           smsg(INITED),
           cmsg(LIST)]
    return script(0, 0, [(b"echo", b"e")], ops)


@scenario
def s_packet_frag(rng):
    """包通道小 mtu=23：分片重组全链路（tools/list 长响应跨片）。"""
    tools = [(b"echo", b"desc-0123456789" * 6)]
    ops = [lup(0xA5A5A5A5),
           cmsg(INIT),
           smsg(INITED),
           cmsg(b'{"jsonrpc":"2.0","id":2,"method":"tools/list"}')]
    return script(1, 23, tools, ops)


@scenario
def s_credit_exhaust(rng):
    """包通道 credit 窗口烧干 → E_AGAIN（响应泵空后 client 发不动）。"""
    ops = [lup(7)] + [cmsg(LIST)] * 6
    return script(1, 244, [(b"echo", b"e")], ops)


@scenario
def s_link_down_up(rng):
    """断链重连：LDN→LUP 重置 credit/清 pending；LUP 前 CMSG→E_STATE。"""
    ops = [lup(0xDEADBEEF),
           cmsg(INIT),
           cmsg(LIST),
           ldn(1),              # bond 保留 token
           cmsg(LIST),          # IDLE → E_STATE
           lup(0xDEADBEEF),
           cmsg(LIST)]
    return script(0, 0, [(b"echo", b"e")], ops)


@scenario
def s_gate_violation(rng):
    """LINK_UP 期粗门：非 initialize 形态 CMSG → E_STATE；CTRL 不受限。"""
    ops = [lup(0x100),
           cmsg(LIST),          # 粗门拒绝
           cmsg(INIT),          # initialize 形态放行
           smsg(INITED),
           cmsg(LIST)]
    return script(0, 0, [(b"echo", b"e")], ops)


@scenario
def s_tick_timeout(rng):
    """TICK 推进：包通道分片半帧超时回收（framer_tick 路径）。"""
    tools = [(b"echo", b"d" * 40)]
    ops = [lup(3),
           cmsg(INIT),
           smsg(INITED),
           cmsg(b'{"jsonrpc":"2.0","id":9,"method":"tools/list"}'),
           tick(90000),         # 超 30000 超时
           cmsg(PING)]
    return script(1, 23, tools, ops)


@scenario
def s_error_payloads(rng):
    """乱输入：垃圾 CMSG/SMSG、无 method、超长 text、未知方法。"""
    ops = [lup(1),
           cmsg(b"garbage"),
           cmsg(b'{"jsonrpc":"2.0"}'),
           cmsg(INIT),
           smsg(b'{"jsonrpc":"2.0","method":"notifications/initialized"}'),
           cmsg(b'{"jsonrpc":"2.0","id":2,"method":"bogus"}'),
           cmsg(b'{"jsonrpc":"2.0","id":3,"method":"tools/call","params":{"name":"nope"}}')]
    return script(0, 0, [(b"echo", b"e")], ops)


@scenario
def s_bond_token_loss(rng):
    """LDN(bond=0)：bond 丢失 → server token 清零，同 token 重连也 REJ。
    （session 层 RESUME 判例在 e2e 面上的投影由 link_up 重置语义覆盖。）"""
    ops = [lup(0xCAFEBABE),
           cmsg(INIT),
           ldn(0),             # bond 丢
           lup(0xCAFEBABE),
           cmsg(LIST)]
    return script(0, 0, [(b"echo", b"e")], ops)


def rand_str(rng, n):
    pool = b"abcXYZ019 .,_-\"\\/\n\t{}[]:;<>=+*()&^%$#@!~`|?ae"
    return bytes(pool[rng.randrange(len(pool))] for _ in range(n))


def rand_mcp(rng):
    """随机 MCP 消息：合法握手系 / 乱串 / 变体 params 混合。"""
    r = rng.random()
    if r < 0.08:
        return rand_str(rng, rng.randint(0, 60))
    if r < 0.14:
        return b'{"jsonrpc":"2.0"}'
    ms = [b"initialize", b"notifications/initialized", b"tools/list",
          b"tools/call", b"ping", b"bogus", b"resources/list"]
    m = b'{"jsonrpc":"2.0",'
    if rng.random() < 0.8:
        m += b'"id":' + rng.choice([b"1", b"42", b"-5", b"9007199254740993",
                                    b'"s"', b"0"]) + b","
    m += b'"method":"' + rng.choice(ms) + b'"'
    rp = rng.random()
    if rp < 0.3:
        m += b',"params":{}'
    elif rp < 0.55:
        m += b',"params":{"name":"echo","arguments":{"text":"' + \
             rand_str(rng, rng.randint(1, 30)) + b'"}}'
    elif rp < 0.7:
        m += b',"params":{"name":"' + rand_str(rng, rng.randint(0, 10)) + \
             b'","arguments":{}}'
    elif rp < 0.85:
        m += b',"params":' + rand_str(rng, rng.randint(0, 20))
    m += b"}"
    return m


def rand_ops(rng):
    ops = []
    # 概率性先 link_up（随机 token）
    if rng.random() < 0.7:
        ops.append(lup(rng.randrange(1 << 32)))
    for _ in range(rng.randint(1, 18)):
        r = rng.random()
        if r < 0.55:
            ops.append(cmsg(rand_mcp(rng)))
        elif r < 0.75:
            ops.append(smsg(rand_mcp(rng)))
        elif r < 0.85:
            ops.append(tick(rng.choice([0, 100, 31000, 90000, rng.randrange(1 << 30)])))
        elif r < 0.95:
            ops.append(lup(rng.randrange(1 << 32)))
        else:
            ops.append(ldn(rng.randrange(2)))
    return ops


def rand_script(rng):
    packet = rng.random() < 0.4
    mtu = 0 if not packet else rng.choice([23, 31, 53, 100, 244, 261])
    tools = []
    for i in range(rng.randint(0, 4)):
        nm = b"tool%d" % i if rng.random() < 0.6 else rand_str(rng, rng.randint(1, 10))
        tools.append((nm, rand_str(rng, rng.randint(0, 30))))
    return script(1 if packet else 0, mtu, tools, rand_ops(rng))


def diff_check(name, sc, stats):
    rc_c, out_c = run(MOCK, sc)
    rc_o, out_o = run(ORACLE, sc)
    stats["checks"] += 1
    if (rc_c, out_c) != (rc_o, out_o):
        print(f"FAIL {name}: rc {rc_c} vs {rc_o}, "
              f"{len(out_c)}B vs {len(out_o)}B")
        for j, (a, b) in enumerate(zip(out_c, out_o)):
            if a != b:
                print(f"  diff at {j}: {a:02x} vs {b:02x}")
                print(f"  C   : {out_c[max(0,j-40):j+60]!r}")
                print(f"  Rust: {out_o[max(0,j-40):j+60]!r}")
                break
        stats["fails"] += 1
        return False
    return True


def main():
    rounds = int(sys.argv[1]) if len(sys.argv) > 1 else 200
    rng = random.Random(0xE2E2C0DE)   # 固定种子：可复现
    stats = {"checks": 0, "fails": 0}

    for fn in SCENARIOS:
        diff_check(f"scenario {fn.__name__}", fn(rng), stats)

    for i in range(rounds):
        if not diff_check(f"random [{i}]", rand_script(rng), stats):
            if stats["fails"] >= 3:
                print("too many failures, stopping early")
                break

    if stats["fails"]:
        print(f"\nFAILED: {stats['fails']}/{stats['checks']} checks")
        sys.exit(1)
    print(f"ALL GREEN: {stats['checks']} checks (scenarios={len(SCENARIOS)} "
          f"random={rounds})")


if __name__ == "__main__":
    main()
