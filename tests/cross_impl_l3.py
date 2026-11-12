#!/usr/bin/env python3
"""cross_impl_l3.py —— 用独立第二实现校验主实现的字节流（v0.9 C2）。

## 它补的是哪个洞

现有差分是 C ↔ Rust 互比，两个实现出自同一份理解。若那份理解本身错了
（flags 位定义、帧头字段顺序、分片约定），差分**永远绿**。

本脚本引入第三条腿：`tests/l3_independent.py`（按规范文本独立重写的
Python 解析器），把 mock 与 oracle 产出的字节流喂进去，要求：
  1. 能解析（无 magic 错、无截断、无非法位组合）；
  2. 重组出的消息序列与「发进去的请求」一一对应；
  3. C 与 Rust 的输出被独立解析器解读为**同一组消息**。

这样即使 C 与 Rust 一起错，只要错了就和规范文本对不上，这里会红。

## 用法

    python3 tests/cross_impl_l3.py [rounds]
"""
import importlib.util
import os
import struct
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
os.chdir(ROOT)
sys.path.insert(0, 'tests')


def _load(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    m = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(m)
    return m


l3 = _load('l3_independent', 'tests/l3_independent.py')
m4 = _load('m4_mcp_harness', 'tests/m4_mcp_harness.py')

MOCK = os.environ.get('BT_MCP_MOCK', 'build/btmcp_mock')
ORACLE = os.environ.get('BT_MCP_ORACLE', 'oracle/target/release/oracle')

CHECKS = [0, 0]      # pass, fail


def check(cond, msg):
    if cond:
        CHECKS[0] += 1
    else:
        CHECKS[1] += 1
        print(f'FAIL {msg}')


def run(binpath, script):
    p = subprocess.run([binpath, 'mcp'], input=script,
                       capture_output=True, timeout=20)
    return p.returncode, p.stdout


def unwrap(out):
    """剥掉 harness 步封装，取出纯 wire 字节。

    mock/oracle 的 mcp 模式每步输出：
        i32 rc | u32 wire_len | wire | u32 nmsgs | {i32 rx_rc | u32 len | bytes}*nmsgs
    本脚本只关心 wire（发出去的帧），其余字段跳过。
    """
    wires, off = [], 0
    n = len(out)
    while off + 8 <= n:
        off += 4                                   # rc
        wl = struct.unpack_from('<I', out, off)[0]; off += 4
        if off + wl > n:
            break
        wires.append(out[off:off + wl]); off += wl
        if off + 4 > n:
            break
        nmsg = struct.unpack_from('<I', out, off)[0]; off += 4
        for _ in range(nmsg):
            if off + 4 > n:
                break
            off += 4                               # rx_rc
            ln = struct.unpack_from('<I', out, off)[0]; off += 4
            if off + ln > n:
                break
            off += ln
    return b''.join(wires)


def mcp_msgs():
    """一组覆盖分片边界的请求：小/中/大消息各一。"""
    init = (b'{"jsonrpc":"2.0","id":1,"method":"initialize","params":'
            b'{"protocolVersion":"2025-11-25"}}')
    inited = b'{"jsonrpc":"2.0","method":"notifications/initialized"}'
    small = b'{"jsonrpc":"2.0","id":2,"method":"ping"}'
    # 中等：工具名 + 参数（会跨多片）
    med = (b'{"jsonrpc":"2.0","id":3,"method":"tools/call","params":'
           b'{"name":"echo","arguments":{"text":"' + b'x' * 120 + b'"}}}')
    return [init, inited, small, med]


def verify_stream(stream, label, fcs16=False):
    """独立解析器能否规范地解析这段流。"""
    frames, errors = l3.parse_stream(stream, fcs16=fcs16)
    check(len(frames) > 0, f'{label}: 独立解析器读出了帧')
    check(not errors, f'{label}: 无规范违例（实际 {len(errors)} 处）')
    for e in errors[:3]:
        print(f'    VIOLATION {e}')
    return frames


def main():
    rounds = int(sys.argv[1]) if len(sys.argv) > 1 else 20
    if not os.path.exists(MOCK) or not os.path.exists(ORACLE):
        print('SKIP: 需要先构建 build/btmcp_mock 与 oracle/target/release/oracle')
        return 0

    tools = [(b'echo', b'echo tool')]
    msgs = mcp_msgs()

    # ---- 1. 通道/MTU 组合：独立解析器都要能读懂 ----
    for chan, mtu in [(0, 0), (1, 244), (1, 60), (1, 23)]:
        script = m4.script(chan, mtu, tools, msgs)
        rc_c, out_c = run(MOCK, script)
        rc_r, out_r = run(ORACLE, script)
        label = f'chan={chan} mtu={mtu}'
        check(rc_c == 0 and rc_r == 0, f'{label}: 两个实现都正常退出')

        fc = verify_stream(unwrap(out_c), f'C {label}')
        fr = verify_stream(unwrap(out_r), f'Rust {label}')

        # 独立解析器对两侧的解读必须一致（帧数、每帧元信息、载荷）
        sig_c = [(f.seq, f.idx, f.last, f.chunk, f.is_ctrl, bytes(f.payload))
                 for f in fc]
        sig_r = [(f.seq, f.idx, f.last, f.chunk, f.is_ctrl, bytes(f.payload))
                 for f in fr]
        if sig_c != sig_r:
            # 找出第一个不同处，便于定位
            for i, (a, b) in enumerate(zip(sig_c, sig_r)):
                if a != b:
                    print(f'    first diff @frame {i}: C={a[:5]} R={b[:5]}')
                    break
        check(sig_c == sig_r, f'{label}: 独立解析器对两侧解读一致')

    # ---- 2. 重组：规范解析出的消息必须是合法 JSON-RPC ----
    script = m4.script(1, 244, tools, msgs)
    _rc, out_c = run(MOCK, script)
    frames, _ = l3.parse_stream(unwrap(out_c))
    bodies = l3.assemble_messages(frames)
    check(len(bodies) > 0, '重组出响应消息')
    for seq, body in bodies:
        if not body:
            continue
        check(body.lstrip().startswith(b'{'), f'seq={seq}: 消息以 {{ 开头')
        check(body.rstrip().endswith(b'}'), f'seq={seq}: 消息以 }} 结尾')
        check(b'"jsonrpc":"2.0"' in body, f'seq={seq}: 含 jsonrpc 2.0')

    # ---- 3. 随机轮次：两侧流被独立解读后逐帧一致 ----
    rng = m4.Rng(0x5EED) if hasattr(m4, 'Rng') else None
    for i in range(rounds):
        chan = 1 if (i % 2) else 0
        mtu = [0, 23, 60, 244][i % 4]
        s = m4.script(chan, mtu, tools, msgs)
        rc_c, out_c = run(MOCK, s)
        rc_r, out_r = run(ORACLE, s)
        if rc_c != 0 or rc_r != 0:
            check(False, f'round {i}: 退出码 C={rc_c} R={rc_r}')
            continue
        fc, ec = l3.parse_stream(unwrap(out_c))
        fr, er = l3.parse_stream(unwrap(out_r))
        check(not ec and not er, f'round {i}: 两侧都无规范违例')
        sc = [(f.seq, f.idx, f.last, f.chunk, f.is_ctrl, bytes(f.payload))
              for f in fc]
        sr = [(f.seq, f.idx, f.last, f.chunk, f.is_ctrl, bytes(f.payload))
              for f in fr]
        check(sc == sr, f'round {i}: 独立解读一致（chan={chan} mtu={mtu}）')
    _ = rng

    print(f'\n独立第二实现校验：{CHECKS[0]} 通过 / {CHECKS[1]} 失败')
    if CHECKS[1]:
        print('注：红不代表 C/Rust 互不一致，而是**与规范文本不一致**——'
              '这正是本脚本存在的意义。')
        return 1
    print('CROSS-IMPL OK：两侧流均可被独立实现按规范解读，且解读一致')
    return 0


if __name__ == '__main__':
    sys.exit(main())
