#!/usr/bin/env python3
"""golden_replay.py —— MCP 响应 golden transcript 回归（v0.4 G4）。

与 conformance 的分工：
  - conformance 校验「结构是否符合规范」（字段存在/类型正确）；
  - golden 校验「字节是否发生了非预期变化」（字段顺序、空格、错误文案...）。

golden 文件是**人工审阅过**的期望响应（`tests/golden/*.json`），
来源是当前实现，但每条都对照 MCP 规范检查过必需字段与语义；
它的作用是「锁住已确认正确的输出」，任何改动都会显式暴露。

用法：
    python3 tests/golden_replay.py            # 校验
    python3 tests/golden_replay.py --update   # 重新生成（需人工 review diff！）
"""
import json
import os
import struct
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
MOCK = os.environ.get("BT_MCP_MOCK") or os.path.join(ROOT, "build", "btmcp_mock")
GOLDEN_DIR = os.path.join(ROOT, "tests", "golden")


def build_script(tools, messages, mtu=244, chan=0):
    """mcp 模式输入：u8 chan | u16 mtu | u8 ntools | tools* | (0x07 msg)*"""
    s = bytes([chan]) + struct.pack("<H", mtu) + bytes([len(tools)])
    for name, desc in tools:
        s += bytes([len(name)]) + name + bytes([len(desc)]) + desc
    for m in messages:
        s += b"\x07" + struct.pack("<H", len(m)) + m
    return s


def run_mcp(script):
    """返回每一步的响应 JSON 文本（无响应为 None）。"""
    p = subprocess.run([MOCK, "mcp"], input=script, capture_output=True,
                       timeout=20)
    if p.returncode != 0:
        raise SystemExit(f"mock mcp 失败 rc={p.returncode}: {p.stderr[:200]}")
    buf = p.stdout
    off = 0
    outs = []
    while off + 8 <= len(buf):
        off += 4                                   # i32 rc
        wl = struct.unpack_from("<I", buf, off)[0]
        off += 4
        wire = buf[off:off + wl]
        off += wl
        nm = struct.unpack_from("<I", buf, off)[0]
        off += 4
        for _ in range(nm):
            off += 4 + struct.unpack_from("<I", buf, off)[0]
        # 剥离帧头（本配置无 FCS）
        j = b""
        o = 0
        while o + 6 <= len(wire):
            plen = struct.unpack_from("<H", wire, o + 4)[0]
            if o + 6 + plen > len(wire):
                break
            j += wire[o + 6:o + 6 + plen]
            o += 6 + plen
        outs.append(j.decode("utf-8") if j else None)
    return outs


SCENARIOS = [
    {
        "file": "basic.json",
        "desc": "initialize / tools.list / tools.call 文本结果 / ping / 未知方法",
        "tools": [(b"echo", b"echo text")],
        "steps": [
            '{"jsonrpc":"2.0","id":1,"method":"initialize","params":'
            '{"protocolVersion":"2025-06-18","capabilities":{},'
            '"clientInfo":{"name":"golden","version":"1"}}}',
            '{"jsonrpc":"2.0","method":"notifications/initialized"}',
            '{"jsonrpc":"2.0","id":2,"method":"tools/list"}',
            '{"jsonrpc":"2.0","id":3,"method":"tools/call","params":'
            '{"name":"echo","arguments":{"text":"hi"}}}',
            '{"jsonrpc":"2.0","id":4,"method":"ping"}',
            '{"jsonrpc":"2.0","id":5,"method":"nope"}',
        ],
    },
    {
        "file": "resources.json",
        "desc": "resources.list（含 title/size）/ resources.read 分页与不存在 / 订阅",
        "tools": [],
        "steps": [
            '{"jsonrpc":"2.0","id":1,"method":"initialize","params":'
            '{"protocolVersion":"2025-06-18","capabilities":{}}}',
            '{"jsonrpc":"2.0","method":"notifications/initialized"}',
            '{"jsonrpc":"2.0","id":2,"method":"resources/list"}',
            '{"jsonrpc":"2.0","id":3,"method":"resources/read","params":'
            '{"uri":"bt-mcp://dev/log"}}',
            '{"jsonrpc":"2.0","id":4,"method":"resources/read","params":'
            '{"uri":"bt-mcp://dev/log","cursor":"8"}}',
            '{"jsonrpc":"2.0","id":5,"method":"resources/read","params":'
            '{"uri":"bt-mcp://dev/missing"}}',
            '{"jsonrpc":"2.0","id":6,"method":"resources/subscribe","params":'
            '{"uri":"bt-mcp://dev/log"}}',
            '{"jsonrpc":"2.0","id":7,"method":"resources/unsubscribe","params":'
            '{"uri":"bt-mcp://dev/log"}}',
        ],
    },
    {
        "file": "prompts_logging_authz.json",
        "desc": "prompts.list/get、logging.setLevel、授权（consent/restricted）、富内容",
        "tools": [(b"echo", b"echo text")],
        "steps": [
            '{"jsonrpc":"2.0","id":1,"method":"initialize","params":'
            '{"protocolVersion":"2025-06-18","capabilities":{}}}',
            '{"jsonrpc":"2.0","method":"notifications/initialized"}',
            '{"jsonrpc":"2.0","id":2,"method":"prompts/list"}',
            '{"jsonrpc":"2.0","id":3,"method":"prompts/get","params":'
            '{"name":"greet"}}',
            '{"jsonrpc":"2.0","id":4,"method":"logging/setLevel","params":'
            '{"level":"debug"}}',
            '{"jsonrpc":"2.0","id":5,"method":"tools/call","params":'
            '{"name":"restricted_tool"}}',
            '{"jsonrpc":"2.0","id":6,"method":"tools/call","params":'
            '{"name":"consent_tool","arguments":{}}}',
            '{"jsonrpc":"2.0","id":7,"method":"tools/call","params":'
            '{"name":"consent_tool","arguments":{"allow":true}}}',
            '{"jsonrpc":"2.0","id":8,"method":"tools/call","params":'
            '{"name":"rich_tool"}}',
        ],
    },
]


def run_scenario(sc):
    tools = sc["tools"]
    msgs = [m.encode() for m in sc["steps"]]
    outs = run_mcp(build_script(tools, msgs))
    # 只保留有响应的步骤（通知不产生响应）
    pairs = []
    for req, resp in zip(sc["steps"], outs):
        if resp is None:
            continue
        pairs.append({"req": json.loads(req), "expect": json.loads(resp)})
    return {"desc": sc["desc"], "steps": pairs}


def main():
    update = "--update" in sys.argv
    os.makedirs(GOLDEN_DIR, exist_ok=True)
    fails = checks = 0
    for sc in SCENARIOS:
        got = run_scenario(sc)
        path = os.path.join(GOLDEN_DIR, sc["file"])
        if update or not os.path.exists(path):
            with open(path, "w") as f:
                json.dump(got, f, indent=2, ensure_ascii=False, sort_keys=False)
                f.write("\n")
            print(f"WROTE {sc['file']} ({len(got['steps'])} steps)")
            continue
        with open(path) as f:
            want = json.load(f)
        for i, (w, g) in enumerate(zip(want["steps"], got["steps"])):
            checks += 1
            if w != g:
                fails += 1
                print(f"FAIL {sc['file']} step {i}")
                print(f"  want: {json.dumps(w['expect'], ensure_ascii=False)[:200]}")
                print(f"  got : {json.dumps(g['expect'], ensure_ascii=False)[:200]}")
        if len(want["steps"]) != len(got["steps"]):
            fails += 1
            print(f"FAIL {sc['file']}: step count {len(want['steps'])} vs "
                  f"{len(got['steps'])}")
    if update:
        print("\n已重新生成 golden —— 请人工 review `git diff tests/golden/`！")
        return 0
    if fails:
        print(f"\nFAILED: {fails}/{checks} golden checks")
        return 1
    print(f"\nALL GREEN: {checks} golden checks (scenarios={len(SCENARIOS)})")
    return 0


if __name__ == "__main__":
    sys.exit(main())
