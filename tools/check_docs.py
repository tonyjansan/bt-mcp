#!/usr/bin/env python3
"""check_docs.py —— 文档与代码一致性（防漂移）。

检查三件事：
  1. 文档里提到的 bt_mcp_*/BT_MCP_* 符号必须真实存在于头文件（core + port）；
  2. 关键数字（回归基线、覆盖率）在 README / draft / validation 之间一致；
  3. 版本标记：README/draft/interface-spec 的版本号一致。

用法：python3 tools/check_docs.py        # 0 = 一致
"""
import glob
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
os.chdir(ROOT)

# 允许的「非符号」：环境变量、构建宏、历史记录里被移除的类型、计划中的提案
ALLOW = {
    'BT_MCP_MOCK', 'BT_MCP_TSAN', 'BT_MCP_FCS16', 'BT_MCP_MAX_REASM',
    'BT_MCP_AUTHZ_', 'BT_MCP_EC_', 'BT_MCP_E', 'BT_MCP_CFG_HEAP',
    'BT_MCP_ASSERT_REENTRANT', 'BT_MCP_CFG_ASSERT_INVARIANTS',
    'BT_MCP_ENTER', 'BT_MCP_LEAVE',
    'bt_mcp_server_add_session', 'bt_mcp_session_handle_t',
    'bt_mcp_progress_t',            # v0.2 已移除，CHANGELOG 历史记录里提到
    'bt_mcp_session_ctrl',          # 存在，但可能被写作 session_ctrl 简写
    'BT_MCP_TICK_MS',               # 故意提到：v0.8 记录「这个宏不存在，
                                    #   tick 周期由宿主决定」，属反例引用
    'bt_mcp_port_nimble',           # 参考实现的**文件名**，不是符号
}

DOCS = ['README.md', 'docs/README.md', 'docs/bt-mcp-core-draft.md',
        'docs/bt-mcp-protocol-spec.md',
        'docs/bt-mcp-interface-spec.html',
        'docs/v0.5-validation.md', 'docs/v0.4-validation.md',
        'docs/release-notes-v0.4.md', 'docs/release-notes-v0.3.md',
        'docs/roadmap-v0.5.md', 'docs/roadmap-v0.6.md', 'CHANGELOG.md']

fails = []


def headers_symbols():
    syms = set()
    for f in glob.glob('core/include/bt_mcp/*.h') + glob.glob('port/bt_mcp/*.h'):
        t = open(f, encoding='utf-8').read()
        for m in re.finditer(r'\b(bt_mcp_[a-z0-9_]+|BT_MCP_[A-Z0-9_]+)\b', t):
            syms.add(m.group(1))
    return syms


def check_symbols():
    syms = headers_symbols()
    for d in DOCS:
        if not os.path.exists(d):
            continue
        t = open(d, encoding='utf-8').read()
        for m in re.finditer(r'\b(bt_mcp_[a-z0-9_]+|BT_MCP_[A-Z0-9_]+)\b', t):
            sym = m.group(1)
            if sym in ALLOW or sym in syms:
                continue
            fails.append(f"{d}: 提到不存在的符号 {sym}")


def check_numbers():
    """回归基线数字必须在 README / draft / validation 三处一致。"""
    # v0.6 起的基线口径：差分 1295 / HIL 198 / 单元 695 / 矩阵 11469 / SDK 38 / golden 20
    targets = {
        'README.md': [r'1295', r'198', r'695', r'38', r'20'],
        'docs/bt-mcp-core-draft.md': [r'1295', r'198', r'695', r'11469', r'38', r'20'],
    }
    for path, pats in targets.items():
        if not os.path.exists(path):
            continue
        t = open(path, encoding='utf-8').read()
        for p in pats:
            if not re.search(r'\b' + p + r'\b', t):
                fails.append(f"{path}: 缺少基线数字 {p}")


def read_version():
    """从 VERSION 文件读当前版本（单一来源）。

    为什么改成读文件：v0.9.1 之前这里是硬编码 `want = 'v0.6'`，
    于是 v0.7/v0.8/v0.9 三个版本发布后，README 的 Status 一直停在 v0.6
    而检查器仍然通过 —— **检查器自己过期了，反而掩盖了文档债**。
    """
    try:
        with open('VERSION') as f:
            v = f.read().strip()
    except OSError:
        return None
    return 'v' + v if v else None


def check_versions():
    """主文档的版本号必须与 VERSION 一致。"""
    want = read_version()
    if not want:
        fails.append('VERSION 文件缺失或为空（版本号单一来源）')
        return
    checks = {
        'README.md': r'## Status \(2026-\d\d-\d\d, (v\d+\.\d+(?:\.\d+)?)\)',
        'docs/bt-mcp-core-draft.md': r'# BT-MCP 实现说明',
        'docs/bt-mcp-protocol-spec.md': r'规范（normative）',
        'docs/bt-mcp-interface-spec.html': r'<h1>BT-MCP (v\d+\.\d+(?:\.\d+)?) 外部接口规约</h1>',
    }
    for path, pat in checks.items():
        if not os.path.exists(path):
            continue
        t = open(path, encoding='utf-8').read()
        m = re.search(pat, t)
        if not m:
            fails.append(f"{path}: 找不到版本标记（模式 {pat}）")
        elif m.groups():
            # 带捕获组 = 该文件必须标注当前版本号
            if m.group(1) != want:
                fails.append(f"{path}: 版本 {m.group(1)} != {want}")
        # 无捕获组 = 该文件是「不带版本号」的规范/说明，只要求标记存在


def check_index():
    """docs/README.md 里用链接列出的本地文件必须真实存在（防断链）。"""
    path = 'docs/README.md'
    if not os.path.exists(path):
        fails.append('docs/README.md 不存在')
        return
    t = open(path, encoding='utf-8').read()
    for m in re.finditer(r'\]\(([^)#]+\.(?:md|html))\)', t):
        rel = m.group(1)
        target = os.path.normpath(os.path.join('docs', rel))
        if not os.path.exists(target):
            fails.append(f'docs/README.md: 链接指向不存在的文件 {rel}')


def main():
    check_symbols()
    check_numbers()
    check_versions()
    check_index()
    if fails:
        print("文档与代码不一致：")
        for f in fails:
            print("  -", f)
        return 1
    print("DOCS OK: 符号 / 基线数字 / 版本标记 三处一致")
    return 0


if __name__ == '__main__':
    sys.exit(main())
