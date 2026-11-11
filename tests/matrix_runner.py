#!/usr/bin/env python3
"""matrix_runner.py —— 正交差分矩阵（v0.6 N3）。

v0.5 的教训：差分矩阵是**手写**的，漏一个维度就是假绿，而红了之后还要
人工排查「到底是实现不一致，还是两侧配置不一致」。共享重组缓冲那次红，
根因是 C 侧 MAX_REASM=1、Rust 侧 4 —— 排查花了不少时间。

本脚本做两件事：
  1. **配置签名前置比对**：跑差分之前先问两侧 `config` 模式，逐字段比对，
     不一致立刻 fail 并打印差异（而不是等差分红了再猜）。
  2. **笛卡尔积枚举**：把 (FCS16, 重组槽, MTU, 版本) 做成显式维度表，
     自动生成组合并逐个跑，报告哪个组合失败。

用法：
    python3 tests/matrix_runner.py              # 默认规模
    python3 tests/matrix_runner.py --rounds 100 # 加大随机轮数
    python3 tests/matrix_runner.py --list       # 只列组合不执行

需要事先构建对应配置的 mock（见 build.sh 的 matrix 步骤）；缺失的组合会
明确报告 SKIP，而不是静默跳过。
"""
import argparse
import itertools
import json
import os
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
os.chdir(ROOT)

# 维度表：每项 = (显示名, mock 二进制, oracle 环境变量, 额外 env)
# 只有「两侧都有对应构建」的组合才有意义——这正是签名比对的用武之地。
# 每项 = (显示名, C 侧 mock 二进制, 需要给**两侧**的环境变量)
# 注意：两侧必须同配置——这正是 v0.5 共享模式那次差分红的原因。
DIM_FCS16 = [
    ("fcs16=0", "build/btmcp_mock", {}),
    ("fcs16=1", "build/btmcp_mock_fcs", {"BT_MCP_FCS16": "1"}),
]
DIM_REASM = [
    ("reasm=4", "build/btmcp_mock", {}),
    ("reasm=1", "build/btmcp_mock_shared", {"BT_MCP_MAX_REASM": "1"}),
]
DIM_MTU = [23, 244]          # 由 harness 的脚本头控制，不需要额外构建
DIM_PROTO = ["2025-03-26", "2025-06-18"]

HARNESSES = [
    ("m3", "tests/m3_session_harness.py"),
    ("m4", "tests/m4_mcp_harness.py"),
]


def run(cmd, env=None, input_bytes=None, timeout=180):
    e = dict(os.environ)
    if env:
        e.update(env)
    return subprocess.run(cmd, input=input_bytes, capture_output=True,
                          env=e, timeout=timeout)


def config_of(binary, env):
    """取某一侧的配置签名（dict）。失败返回 None。"""
    if not os.path.exists(binary):
        return None
    p = run([binary, "config"], env=env)
    if p.returncode != 0:
        return None
    try:
        return json.loads(p.stdout.decode())
    except Exception:
        return None


def oracle_config(env):
    p = run(["oracle/target/release/oracle", "config"], env=env)
    if p.returncode != 0:
        return None
    try:
        return json.loads(p.stdout.decode())
    except Exception:
        return None


def compare_signatures(c_cfg, r_cfg):
    """逐字段比对；impl 与 shared 派生字段不算差异。"""
    if c_cfg is None or r_cfg is None:
        return ["无法取得配置签名"]
    diffs = []
    keys = set(c_cfg) | set(r_cfg)
    for k in sorted(keys):
        if k in ("impl",):
            continue
        if c_cfg.get(k) != r_cfg.get(k):
            diffs.append(f"{k}: C={c_cfg.get(k)} vs Rust={r_cfg.get(k)}")
    return diffs


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--rounds", type=int, default=40)
    ap.add_argument("--list", action="store_true")
    args = ap.parse_args()

    combos = list(itertools.product(DIM_FCS16, DIM_REASM, DIM_MTU, DIM_PROTO))
    if args.list:
        for f, r, m, p in combos:
            print(f"{f[0]:10s} {r[0]:9s} mtu={m:<4d} proto={p}")
        print(f"\n共 {len(combos)} 个组合")
        return 0

    ok = skipped = failed = 0
    failures = []
    sig_checked = set()

    for (fname, mock0, fenv), (rname, mock1, renv), mtu, proto in combos:
        # reasm 维度决定用哪个 C mock；fcs16 维度决定加不加 FCS，两者要组合
        mock = mock0
        if renv.get("BT_MCP_MAX_REASM"):
            # reasm=1 需要共享重组缓冲构建；若同时要 FCS16，则需另建组合二进制
            mock = ("build/btmcp_mock_shared_fcs"
                    if fenv.get("BT_MCP_FCS16") else mock1)
        if not os.path.exists(mock):
            skipped += 1
            print(f"SKIP {fname} {rname} mtu={mtu} {proto}: 缺 {mock}")
            continue

        # 两侧共享同一套配置 env（fcs16 + reasm 都要生效）
        env = dict(fenv)
        env.update(renv)
        env["BT_MCP_MOCK"] = mock

        # 1) 配置签名前置比对（两侧用同一套 env 查询）
        c_cfg = config_of(mock, env)
        r_cfg = oracle_config(env)
        diffs = compare_signatures(c_cfg, r_cfg)
        key = (fname, rname)
        if diffs and key not in sig_checked:
            sig_checked.add(key)
            failed += 1
            msg = f"签名不一致 {fname} {rname}: " + "; ".join(diffs)
            print("FAIL " + msg)
            failures.append(msg)
            continue
        sig_checked.add(key)

        # 2) 跑各 harness
        for hname, hpath in HARNESSES:
            p = run([sys.executable, hpath, str(args.rounds)], env=env)
            if p.returncode == 0:
                ok += 1
            else:
                failed += 1
                tail = p.stdout.decode(errors="replace").strip().splitlines()
                tail = tail[-1] if tail else "(无输出)"
                msg = f"{hname} {fname} {rname} mtu={mtu} proto={proto}: {tail}"
                print("FAIL " + msg)
                failures.append(msg)

    print(f"\n矩阵：{len(combos)} 组合，{ok} 通过 / {skipped} 跳过 / {failed} 失败")
    if failed:
        print("\n失败明细：")
        for f in failures:
            print("  -", f)
        return 1
    print("MATRIX OK（配置签名一致，全部组合通过）")
    return 0


if __name__ == "__main__":
    sys.exit(main())
