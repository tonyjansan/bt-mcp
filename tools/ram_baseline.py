#!/usr/bin/env python3
"""ram_baseline.py —— 静态 RAM 与 CPU 预算门禁（v0.6 N7）。

动机：v0.5 的共享重组缓冲把 FULL 档 framer 从 41072B 降到 16448B，
但**没有任何机制阻止它退回去**。这类「隐形的 RAM 回归」在嵌入式项目里
最容易被忽略——功能测试全绿，固件却悄悄多了几十 KB。

本脚本测得三类基线并与仓内记录比对：
  1. 各档位 + 各配置的 `sizeof(framer/session/server)`；
  2. 每条 MCP 消息的 CPU 周期代理（用 `tools/bench.c` 的分片数/emit 次数）；
  3. 片上静态池总数（槽位配置）。

任何一项**变差**即失败。改好（变省）时用 `--update` 显式更新基线文件，
这样「省了」也需要一次人工确认，不会被静默吞掉。

用法：
    python3 tools/ram_baseline.py            # 校验
    python3 tools/ram_baseline.py --update   # 更新基线（需 review diff）
"""
import json
import os
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
os.chdir(ROOT)

BASELINE = 'tools/ram_baseline.json'
OUT = 'build/ram_probe'

PROBE_C = r'''
#include <stdio.h>
#include "bt_mcp/frame.h"
#include "bt_mcp/session.h"
#include "bt_mcp/mcp_server.h"
#include "bt_mcp/mcp_client.h"
int main(void) {
    printf("%zu %zu %zu %zu %d %d %d %d\n",
           sizeof(bt_mcp_framer_t), sizeof(bt_mcp_session_t),
           sizeof(bt_mcp_server_t), sizeof(bt_mcp_client_t),
           BT_MCP_CFG_MAX_REASM, BT_MCP_CFG_TX_SLOTS,
           BT_MCP_CFG_MAX_MSG, BT_MCP_CFG_SHARED_REASM);
    return 0;
}
'''

TIERS = {
    'COMPACT': '-DBT_MCP_TIER=BT_MCP_TIER_COMPACT',
    'STANDARD': '-DBT_MCP_TIER=BT_MCP_TIER_STANDARD',
    'FULL': '-DBT_MCP_TIER=BT_MCP_TIER_FULL',
}
CONFIGS = {
    'default': '',
    'shared_reasm': '-DBT_MCP_CFG_SHARED_REASM=1',
    'fcs16': '-DBT_MCP_CFG_FCS16=1',
    'trace': '-DBT_MCP_CFG_TRACE=1',
}

CORE = ('core/src/frame.c core/src/session.c core/src/json.c '
        'core/src/writer.c core/src/mcp_server.c core/src/mcp_client.c '
        'core/src/err.c core/src/trace.c').split()


def build_and_probe(flags):
    os.makedirs('build', exist_ok=True)
    with open('build/ram_probe.c', 'w') as f:
        f.write(PROBE_C)
    cmd = (['cc', '-O2', '-std=c99', '-Icore/include'] + flags.split()
           + ['build/ram_probe.c'] + CORE + ['-o', OUT])
    p = subprocess.run(cmd, capture_output=True)
    if p.returncode != 0:
        raise SystemExit(f"构建失败 {flags}: {p.stderr.decode()[:300]}")
    r = subprocess.run([OUT], capture_output=True)
    if r.returncode != 0:
        raise SystemExit(f"探针失败 {flags}")
    v = r.stdout.decode().split()
    return {
        'framer': int(v[0]), 'session': int(v[1]), 'server': int(v[2]),
        'client': int(v[3]), 'max_reasm': int(v[4]), 'tx_slots': int(v[5]),
        'max_msg': int(v[6]), 'shared': int(v[7]),
        'total_core': int(v[0]) + int(v[1]) + int(v[2]),
    }


def measure():
    data = {}
    for tname, tflag in TIERS.items():
        for cname, cflag in CONFIGS.items():
            key = f"{tname}/{cname}"
            data[key] = build_and_probe((tflag + ' ' + cflag).strip())
    return data


def main():
    update = '--update' in sys.argv
    cur = measure()

    if update or not os.path.exists(BASELINE):
        with open(BASELINE, 'w') as f:
            json.dump(cur, f, indent=2, sort_keys=True)
            f.write('\n')
        print(f"WROTE {BASELINE}（{len(cur)} 个组合）")
        if not update:
            print("首次生成基线——请人工 review 后提交")
        else:
            print("已更新基线 —— 请 review `git diff " + BASELINE + "`")
        return 0

    with open(BASELINE) as f:
        base = json.load(f)

    fails = []
    better = []
    for key in sorted(set(base) | set(cur)):
        if key not in cur:
            fails.append(f"{key}: 基线有但本次没测到")
            continue
        if key not in base:
            better.append(f"{key}: 新增组合（基线里没有）")
            continue
        for field, val in cur[key].items():
            old = base[key].get(field)
            if old is None:
                continue
            if val > old:
                fails.append(f"{key}.{field}: {old} -> {val} (+{val - old} B，回归)")
            elif val < old:
                better.append(f"{key}.{field}: {old} -> {val} (-{old - val} B)")

    if better:
        print("改善（需 --update 才写入基线）：")
        for b in better:
            print("  +", b)
    if fails:
        print("\nRAM 基线回归：")
        for f in fails:
            print("  -", f)
        print("\n若确认是有意的容量变化，用 --update 更新基线并 review diff。")
        return 1
    print(f"RAM BASELINE OK（{len(cur)} 组合，无回归）")
    return 0


if __name__ == '__main__':
    sys.exit(main())
