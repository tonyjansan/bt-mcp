#!/usr/bin/env python3
"""bt-mcp M4a 差分 harness：L5 mcp_server 全栈（L3+L4+L5）C mock ↔ Rust oracle。

脚本：u8 chan | u16 mtu | u8 tools_count | [工具]* | [0x07 MSG u16 + MCP]*
工具记录：u8 name_len+name | u8 desc_len+desc（fn 固定 echo）
两侧 mcp 模式跑同一脚本，stdout 逐字节对拍。

场景覆盖：initialize/notifications/initialized 握手、-32002 门、
tools/list（多工具、特殊字符 name/desc）、tools/call（echo、无 text→tool error、
未知工具→-32602、name 缺失）、ping、未知方法 -32601/-32700、
id 边界（i64 极值、字符串 id）、双层转义、包通道 mtu 边界与信用耗尽、
超大响应（E_TOOBIG 路径）、并发 URL 转义字符。

用法：python3 m4_mcp_harness.py [rounds]
"""
import random
import struct
import subprocess
import sys
import os

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
MOCK = os.environ.get("BT_MCP_MOCK") or os.path.join(ROOT, "build", "btmcp_mock")
ORACLE = os.path.join(ROOT, "oracle", "target", "release", "oracle")

SCENARIOS = []


def scenario(fn):
    SCENARIOS.append(fn)
    return fn


def op_msg(m):
    return b"\x07" + struct.pack("<H", len(m)) + m


def script(chan, mtu, tools, msgs):
    s = bytes([chan]) + struct.pack("<H", mtu) + bytes([len(tools)])
    for n, d in tools:
        s += bytes([len(n)]) + n + bytes([len(d)]) + d
    for m in msgs:
        s += op_msg(m)
    return s


def run(binpath, script):
    p = subprocess.run([binpath, "mcp"], input=script,
                       capture_output=True, timeout=10)
    return p.returncode, p.stdout


INIT = b'{"jsonrpc":"2.0","id":1,"method":"initialize","params":{}}'
INITED = b'{"jsonrpc":"2.0","method":"notifications/initialized"}'


def init_seq(id_start=1):
    return [INIT, INITED]


@scenario
def s_happy_path(rng):
    msgs = init_seq() + [
        b'{"jsonrpc":"2.0","id":2,"method":"tools/list"}',
        b'{"jsonrpc":"2.0","id":3,"method":"ping"}',
        b'{"jsonrpc":"2.0","id":4,"method":"tools/call","params":{"name":"echo","arguments":{"text":"hello"}}}',
    ]
    return script(0, 0, [(b"echo", b"echo tool")], msgs)


@scenario
def s_not_initialized(rng):
    msgs = [
        b'{"jsonrpc":"2.0","id":2,"method":"tools/list"}',
        b'{"jsonrpc":"2.0","method":"tools/list"}',
        INIT, INITED,
        b'{"jsonrpc":"2.0","id":3,"method":"tools/list"}',
    ]
    return script(0, 0, [(b"echo", b"d")], msgs)


@scenario
def s_tool_call_variants(rng):
    msgs = init_seq() + [
        b'{"jsonrpc":"2.0","id":2,"method":"tools/call","params":{"name":"echo","arguments":{"text":"hi"}}}',
        b'{"jsonrpc":"2.0","id":3,"method":"tools/call","params":{"name":"echo","arguments":{}}}',
        b'{"jsonrpc":"2.0","id":4,"method":"tools/call","params":{"name":"nope"}}',
        b'{"jsonrpc":"2.0","id":5,"method":"tools/call"}',
        b'{"jsonrpc":"2.0","id":6,"method":"tools/call","params":{"name":"echo"}}',
        b'{"jsonrpc":"2.0","id":7,"method":"tools/call","params":{"name":"echo","arguments":{"text":"a\\"b"}}}',
        b'{"jsonrpc":"2.0","id":8,"method":"tools/call","params":{"name":"echo","arguments":{"text":"tab\\there"}}}',
    ]
    return script(0, 0, [(b"echo", b"echo")], msgs)


@scenario
def s_multi_tools(rng):
    tools = [(b"echo", b"first tool"),
             (b"add", b"second tool with, comma"),
             (b"weird name", b'desc with "quotes" and \\ backslash')]
    msgs = init_seq() + [
        b'{"jsonrpc":"2.0","id":2,"method":"tools/list"}',
        b'{"jsonrpc":"2.0","id":3,"method":"tools/call","params":{"name":"add","arguments":{"text":"A"}}}',
        b'{"jsonrpc":"2.0","id":4,"method":"tools/call","params":{"name":"weird name","arguments":{"text":"B"}}}',
    ]
    return script(0, 0, tools, msgs)


@scenario
def s_error_paths(rng):
    msgs = init_seq() + [
        b'{"jsonrpc":"2.0","id":2,"method":"bogus"}',
        b'{"jsonrpc":"2.0","method":"notifications/other"}',
        b'garbage',
        b'{"jsonrpc":"2.0","id":3,"method":"tools/call","params":{"name":"echo","arguments":"notobj"}}',
        b'{"jsonrpc":"2.0","id":4,"method":"tools/call","params":"scalar"}',
        b'{"jsonrpc":"2.0"}',
        b'{"method":"nomethod","id":5}',
    ]
    return script(0, 0, [(b"echo", b"e")], msgs)


@scenario
def s_v05_batch(rng):
    """v0.5 V6：JSON-RPC batch 按协商版本分支。
    先协商 2025-03-26（支持 batch）→ batch 逐条响应；
    再协商 2025-06-18（规范已移除）→ 明确回 -32600。"""
    old = (b'{"jsonrpc":"2.0","id":1,"method":"initialize","params":'
           b'{"protocolVersion":"2025-03-26","capabilities":{}}}')
    inited = b'{"jsonrpc":"2.0","method":"notifications/initialized"}'
    batch = (b'[{"jsonrpc":"2.0","id":2,"method":"ping"},'
             b'{"jsonrpc":"2.0","id":3,"method":"tools/list"}]')
    only_notif = b'[{"jsonrpc":"2.0","method":"notifications/initialized"}]'
    bad = b'[{"jsonrpc":"2.0","id":4,"method":"nope"},{"bogus":1}]'
    return script(0, 0, [(b"echo", b"e")],
                  [old, inited, batch, only_notif, bad])


@scenario
def s_v05_batch_new(rng):
    """v0.5 V6：协商到新版本后，batch 必须被明确拒绝（-32600）。"""
    new = (b'{"jsonrpc":"2.0","id":1,"method":"initialize","params":'
           b'{"protocolVersion":"2025-06-18","capabilities":{}}}')
    inited = b'{"jsonrpc":"2.0","method":"notifications/initialized"}'
    batch = (b'[{"jsonrpc":"2.0","id":2,"method":"ping"},'
             b'{"jsonrpc":"2.0","id":3,"method":"ping"}]')
    return script(0, 0, [(b"echo", b"e")], [new, inited, batch])


@scenario
def s_v04_meta(rng):
    """v0.4：工具元数据 / prompts / 订阅 / 日志分级。"""
    msgs = init_seq() + [
        b'{"jsonrpc":"2.0","id":2,"method":"tools/list"}',
        b'{"jsonrpc":"2.0","id":3,"method":"prompts/list"}',
        b'{"jsonrpc":"2.0","id":4,"method":"prompts/get","params":{"name":"greet"}}',
        b'{"jsonrpc":"2.0","id":5,"method":"prompts/get","params":{"name":"nope"}}',
        b'{"jsonrpc":"2.0","id":6,"method":"resources/list"}',
        b'{"jsonrpc":"2.0","id":7,"method":"resources/subscribe","params":{"uri":"bt-mcp://dev/log"}}',
        b'{"jsonrpc":"2.0","id":8,"method":"resources/subscribe","params":{"uri":"bt-mcp://dev/hr_window"}}',
        b'{"jsonrpc":"2.0","id":9,"method":"resources/unsubscribe","params":{"uri":"bt-mcp://dev/log"}}',
        b'{"jsonrpc":"2.0","id":10,"method":"resources/subscribe","params":{"uri":"bogus"}}',
        b'{"jsonrpc":"2.0","id":11,"method":"logging/setLevel","params":{"level":"debug"}}',
        b'{"jsonrpc":"2.0","id":12,"method":"logging/setLevel","params":{"level":"nope"}}',
        b'{"jsonrpc":"2.0","id":13,"method":"logging/setLevel","params":{}}',
    ]
    return script(0, 0, [(b"echo", b"e")], msgs)


@scenario
def s_v04_authz(rng):
    """v0.4：授权（consent 放行/拒绝、restricted）与富内容。"""
    msgs = init_seq() + [
        b'{"jsonrpc":"2.0","id":2,"method":"tools/call","params":{"name":"restricted_tool"}}',
        b'{"jsonrpc":"2.0","id":3,"method":"tools/call","params":{"name":"consent_tool","arguments":{}}}',
        b'{"jsonrpc":"2.0","id":4,"method":"tools/call","params":{"name":"consent_tool","arguments":{"allow":true}}}',
        b'{"jsonrpc":"2.0","id":5,"method":"tools/call","params":{"name":"rich_tool"}}',
        b'{"jsonrpc":"2.0","id":6,"method":"tools/call","params":{"name":"consent_tool"}}',
        b'{"jsonrpc":"2.0","id":7,"method":"tools/call","params":{"name":"missing"}}',
    ]
    return script(0, 0, [(b"echo", b"e")], msgs)


@scenario
def s_id_edge(rng):
    msgs = init_seq() + [
        b'{"jsonrpc":"2.0","id":9223372036854775807,"method":"tools/list"}',
        b'{"jsonrpc":"2.0","id":-9223372036854775808,"method":"tools/list"}',
        b'{"jsonrpc":"2.0","id":9007199254740993,"method":"tools/list"}',
        b'{"jsonrpc":"2.0","id":"string-id","method":"tools/list"}',
        b'{"jsonrpc":"2.0","id":0,"method":"tools/list"}',
    ]
    return script(0, 0, [(b"echo", b"e")], msgs)


@scenario
def s_packet_credit(rng):
    """包通道：mtu=244，窗口烧干后 E_AGAIN。"""
    tools = [(b"echo", b"e")]
    msgs = init_seq() + [
        b'{"jsonrpc":"2.0","id":2,"method":"tools/list"}',
        b'{"jsonrpc":"2.0","id":3,"method":"tools/list"}',
        b'{"jsonrpc":"2.0","id":4,"method":"tools/list"}',
        b'{"jsonrpc":"2.0","id":5,"method":"tools/list"}',   # 窗口 4 → 这条 E_AGAIN
        b'{"jsonrpc":"2.0","id":6,"method":"tools/list"}',
    ]
    return script(1, 244, tools, msgs)


@scenario
def s_packet_fragment(rng):
    """包通道小 mtu：响应帧分片（tools/list 多工具长响应）。"""
    tools = [(b"echo", b"desc-0123456789" * 6)]
    msgs = init_seq() + [
        b'{"jsonrpc":"2.0","id":2,"method":"tools/list"}',
    ]
    return script(1, 23, tools, msgs)


@scenario
def s_resources(rng):
    """resources/list 与 resources/read（含 cursor 分页与未知 uri）。"""
    msgs = init_seq() + [
        b'{"jsonrpc":"2.0","id":2,"method":"resources/list"}',
        b'{"jsonrpc":"2.0","id":3,"method":"resources/read","params":{"uri":"bt-mcp://dev/log"}}',
        b'{"jsonrpc":"2.0","id":4,"method":"resources/read","params":{"uri":"bt-mcp://dev/log?cursor=8"}}',
        b'{"jsonrpc":"2.0","id":5,"method":"resources/read","params":{"uri":"bt-mcp://dev/log?cursor=16"}}',
        b'{"jsonrpc":"2.0","id":6,"method":"resources/read","params":{"uri":"bt-mcp://dev/hr_window"}}',
        b'{"jsonrpc":"2.0","id":7,"method":"resources/read","params":{"uri":"bt-mcp://dev/nope"}}',
        b'{"jsonrpc":"2.0","id":8,"method":"resources/read","params":{}}',
        b'{"jsonrpc":"2.0","id":9,"method":"resources/read","params":{"uri":123}}',
        b'{"jsonrpc":"2.0","id":10,"method":"resources/read","params":{"uri":"bt-mcp://dev/log","cursor":"8"}}',
    ]
    return script(0, 0, [(b"echo", b"e")], msgs)


@scenario
def s_tools_pagination(rng):
    """工具数超过一页（PAGE_SIZE=8）→ nextCursor 分页。"""
    tools = [(b"t%02d" % i, b"desc%d" % i) for i in range(10)]
    msgs = init_seq() + [
        b'{"jsonrpc":"2.0","id":2,"method":"tools/list"}',
        b'{"jsonrpc":"2.0","id":3,"method":"tools/list","params":{"cursor":"8"}}',
        b'{"jsonrpc":"2.0","id":4,"method":"tools/list","params":{"cursor":"99"}}',
    ]
    return script(0, 0, tools, msgs)


@scenario
def s_string_ids(rng):
    """字符串 id 原样回显（含转义与边界）。"""
    msgs = init_seq() + [
        b'{"jsonrpc":"2.0","id":"a","method":"ping"}',
        b'{"jsonrpc":"2.0","id":"","method":"ping"}',
        b'{"jsonrpc":"2.0","id":"a\"b","method":"ping"}',
        b'{"jsonrpc":"2.0","id":"\\u4e2d","method":"tools/list"}',
        b'{"jsonrpc":"2.0","id":"x","method":"bogus"}',
    ]
    return script(0, 0, [(b"echo", b"e")], msgs)


@scenario
def s_channel_meta(rng):
    """initialize 的 _meta 通道声明在包/流通道下的取值。"""
    msgs = init_seq() + [b'{"jsonrpc":"2.0","id":2,"method":"tools/list"}']
    return script(1, 244, [(b"echo", b"e")], msgs)


@scenario
def s_toobig(rng):
    """echo text 超限 → 响应超 8KB → E_TOOBIG 路径（工具成功但 tx 失败）。"""
    msgs = init_seq() + [
        b'{"jsonrpc":"2.0","id":2,"method":"tools/call","params":{"name":"echo","arguments":{"text":"' +
        b"T" * 8300 + b'"}}}',
    ]
    return script(0, 0, [(b"echo", b"e")], msgs)


def rand_str(rng, n):
    pool = (b"abcXYZ019 .,_-\"\\/\n\t{}[]:;<>=+*()&^%$#@!~`|?ae")
    return bytes(rng.choice(pool) if isinstance(pool, bytes) else pool[rng.randrange(len(pool))]
                 for _ in range(n))


def rand_json_method(rng):
    ms = [b"initialize", b"notifications/initialized", b"tools/list",
          b"tools/call", b"ping", b"bogus", b"notifications/x",
          b"resources/list", b"resources/read", b"notifications/cancelled", b""]
    return rng.choice(ms)


def rand_params(rng):
    r = rng.random()
    if r < 0.2:
        return b"{}"
    if r < 0.4:
        return b'{"text":"' + rand_str(rng, rng.randint(1, 40)) + b'"}'
    if r < 0.55:
        return b'{"name":"echo","arguments":{"text":"' + rand_str(rng, rng.randint(1, 40)) + b'"}}}'
    if r < 0.7:
        return b'{"name":"' + rand_str(rng, rng.randint(1, 12)) + b'","arguments":{"text":"x"}}'
    if r < 0.8:
        return b'{"name":"echo","arguments":{}}'
    if r < 0.92:
        uri = rng.choice([b"bt-mcp://dev/log", b"bt-mcp://dev/hr_window",
                          b"bt-mcp://dev/nope", b"bt-mcp://dev/log?cursor=8",
                          b"garbage", b""])
        return b'{"uri":"' + uri + b'"}'
    return rand_str(rng, rng.randint(0, 30))   # 乱 params


def rand_msg(rng):
    r = rng.random()
    if r < 0.05:
        return rand_str(rng, rng.randint(0, 60))   # 纯垃圾
    if r < 0.1:
        return b'{"jsonrpc":"2.0"}'                 # 无 method
    method = rand_json_method(rng)
    has_id = rng.random() < 0.8
    m = b'{"jsonrpc":"2.0",'
    if has_id:
        idv = rng.choice([b"1", b"42", b"999", b"-5", b"9007199254740993",
                          b'"str"', b"0", b"12345678"])
        m += b'"id":' + idv + b","
    m += b'"method":"' + method + b'"'
    if rng.random() < 0.9:
        m += b',"params":' + rand_params(rng)
    m += b"}"
    return m


def rand_tools(rng):
    n = rng.randint(0, 4)
    tools = []
    for i in range(n):
        nm = b"tool%d" % i if rng.random() < 0.6 else rand_str(rng, rng.randint(1, 10))
        tools.append((nm, rand_str(rng, rng.randint(0, 30))))
    return tools


def rand_script(rng):
    packet = rng.random() < 0.4
    mtu = 0 if not packet else rng.choice([23, 53, 100, 244, 261])
    tools = rand_tools(rng)
    nmsg = rng.randint(1, 15)
    msgs = [rand_msg(rng) for _ in range(nmsg)]
    return script(1 if packet else 0, mtu, tools, msgs)


def main():
    rounds = int(sys.argv[1]) if len(sys.argv) > 1 else 200
    rng = random.Random(0xB707C0)   # 固定种子：可复现
    fails = 0
    checks = 0

    for fn in SCENARIOS:
        sc = fn(rng)
        rc_c, out_c = run(MOCK, sc)
        rc_o, out_o = run(ORACLE, sc)
        checks += 1
        if (rc_c, out_c) != (rc_o, out_o):
            print(f"FAIL scenario {fn.__name__}: rc {rc_c} vs {rc_o}, "
                  f"{len(out_c)}B vs {len(out_o)}B")
            for j, (a, b) in enumerate(zip(out_c, out_o)):
                if a != b:
                    print(f"  diff at {j}: {a:02x} vs {b:02x}")
                    print(f"  C   : {out_c[max(0,j-40):j+60]}")
                    print(f"  Rust: {out_o[max(0,j-40):j+60]}")
                    break
            fails += 1

    for i in range(rounds):
        sc = rand_script(rng)
        rc_c, out_c = run(MOCK, sc)
        rc_o, out_o = run(ORACLE, sc)
        checks += 1
        if (rc_c, out_c) != (rc_o, out_o):
            print(f"FAIL random [{i}]: rc {rc_c} vs {rc_o}, "
                  f"{len(out_c)}B vs {len(out_o)}B")
            for j, (a, b) in enumerate(zip(out_c, out_o)):
                if a != b:
                    print(f"  diff at {j}: {a:02x} vs {b:02x}")
                    print(f"  C   : {out_c[max(0,j-40):j+60]}")
                    print(f"  Rust: {out_o[max(0,j-40):j+60]}")
                    break
            fails += 1

    if fails:
        print(f"\nFAILED: {fails}/{checks} checks")
        sys.exit(1)
    print(f"ALL GREEN: {checks} checks (scenarios={len(SCENARIOS)} "
          f"random={rounds})")


if __name__ == "__main__":
    main()
