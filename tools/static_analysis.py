#!/usr/bin/env python3
"""static_analysis.py —— 静态分析门禁（v0.9 C3）。

## 为什么是 `clang --analyze` 而不是 clang-tidy / cppcheck

计划里写的是 clang-tidy + cppcheck，但**本机两者都没装**（也没有包管理器
可装）。能用的是编译器自带的静态分析器 `clang --analyze`，
它覆盖的是「路径敏感的真实缺陷」（空指针解引用、越界、泄漏、未初始化），
而不是风格规则。

**如实说明局限**：它不做 MISRA 检查、不做命名/类型风格检查、
不做跨编译单元的调用图分析。所以它**不能**替代 clang-tidy/cppcheck，
只能覆盖其中最有价值的一部分。

## 关键设计：门禁必须能自证有效

静态分析门禁最常见的问题是「跑了个空命令然后报 OK」。所以本脚本
**每次都先做一次自测**：把一段已知有缺陷的代码喂给分析器，
要求它**必须报出缺陷**。自测不过就说明门禁已失效，直接 fail。

除了 `clang --analyze`（路径敏感缺陷），本工具还做**严格告警矩阵**：
把 core 在「每个编译期配置组合」下用一组很严的告警开关编译一遍。

为什么需要矩阵：单一配置下干净的代码，换个配置就可能冒出未用变量、
指针窄化、隐式符号转换——因为这些分支在默认配置里根本没参与编译。
v0.9.1 加这个矩阵时，立刻就抓到 `c->id_is_str = r->id_is_str`
（int → uint8_t 隐式窄化），默认告警集看不见。

## 用法

    python3 tools/static_analysis.py          # 门禁（自测 + 分析 + 告警矩阵）
    python3 tools/static_analysis.py --update # 更新基线（需 review diff）
"""
import json
import os
import subprocess
import sys
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
os.chdir(ROOT)

BASELINE = 'tools/static_analysis.json'
CC = os.environ.get('CC', 'cc')
SCAN_DIRS = ['core/src']

# 自测样本：必须能被检出。若这条不过，说明分析器/参数已失效。
SELFTEST = r'''
#include <string.h>
int selftest_bad(const char *s)
{
    int *q = 0;
    *q = 1;                      /* 空指针解引用：必须被检出 */
    char buf[4];
    strcpy(buf, s);              /* 越界写：多数分析器也能检出 */
    return *q;
}
'''


def analyze(path, extra_flags=()):
    """对单个文件跑 clang 静态分析，返回 [(file, line, msg)]。"""
    cmd = [CC, '--analyze', '-std=c99', '-Icore/include',
           '-Xanalyzer', '-analyzer-output=text'] + list(extra_flags) + [path]
    r = subprocess.run(cmd, capture_output=True, text=True)
    out = []
    for line in (r.stderr + r.stdout).splitlines():
        if 'warning:' not in line:
            continue
        # 形如: /path/file.c:123:45: warning: ...
        parts = line.split(':')
        try:
            line_no = int(parts[1])
        except (IndexError, ValueError):
            line_no = 0
        msg = line.split('warning:', 1)[1].strip()
        out.append((os.path.basename(parts[0]), line_no, msg))
    return out


def selftest():
    """证明分析器真的在工作——否则后面的「零告警」毫无意义。"""
    tmp = tempfile.NamedTemporaryFile('w', suffix='.c', delete=False)
    try:
        tmp.write(SELFTEST)
        tmp.close()
        found = analyze(tmp.name)
        if not found:
            print('FAIL: 静态分析器自测未通过——喂了已知缺陷却没报出来。')
            print('      这说明门禁已失效（分析器不可用或参数不对），')
            print('      此时「零告警」不代表代码干净。')
            return False
        print(f'自测通过：分析器检出了注入的缺陷（{len(found)} 条）')
        return True
    finally:
        os.unlink(tmp.name)


# 严格告警集：比 -Wall -Wextra 更严，专门抓「跨配置才会暴露」的问题
STRICT_WARNINGS = [
    '-Wall', '-Wextra', '-Wpedantic', '-Wshadow', '-Wconversion',
    '-Wsign-conversion', '-Wcast-qual', '-Wstrict-prototypes',
    '-Wmissing-prototypes', '-Wundef',
]

# 配置矩阵：每个组合都要零告警
CONFIG_MATRIX = [
    ('default', []),
    ('fcs16', ['-DBT_MCP_CFG_FCS16=1']),
    ('trace', ['-DBT_MCP_CFG_TRACE=1']),
    ('shared_reasm', ['-DBT_MCP_CFG_SHARED_REASM=1']),
    ('compact', ['-DBT_MCP_TIER=BT_MCP_TIER_COMPACT']),
    ('full', ['-DBT_MCP_TIER=BT_MCP_TIER_FULL']),
]


def strict_warning_matrix():
    """在配置矩阵下用严格告警集编译 core，返回 [(cfg, file, line, msg)]。"""
    findings = []
    srcs = sorted(f for f in os.listdir('core/src') if f.endswith('.c'))
    for cfg_name, cfg_flags in CONFIG_MATRIX:
        for f in srcs:
            cmd = ([CC, '-c', '-O2', '-std=c99', '-Icore/include']
                   + STRICT_WARNINGS + cfg_flags + [f'core/src/{f}', '-o', os.devnull])
            r = subprocess.run(cmd, capture_output=True, text=True)
            for line in r.stderr.splitlines():
                if 'warning:' not in line:
                    continue
                try:
                    fname = os.path.basename(line.split(':')[0])
                    line_no = int(line.split(':')[1])
                except (IndexError, ValueError):
                    fname, line_no = f, 0
                msg = line.split('warning:', 1)[1].strip()
                findings.append((cfg_name, fname, line_no, msg))
    return findings


def scan():
    findings = []
    for d in SCAN_DIRS:
        for f in sorted(os.listdir(d)):
            if not f.endswith('.c'):
                continue
            for base, line, msg in analyze(os.path.join(d, f)):
                findings.append({'file': base, 'line': line, 'msg': msg})
    return findings


def key(f):
    """用于比对的稳定键（忽略行号漂移：改代码会让行号变，但不该算新缺陷）。"""
    return f'{f["file"]}::{f["msg"]}'


def main():
    update = '--update' in sys.argv

    if not selftest():
        return 1

    # ---- 严格告警矩阵（跨配置）----
    sw = strict_warning_matrix()
    if sw:
        print(f'严格告警矩阵发现 {len(sw)} 条告警（这些在默认告警集下看不见）：')
        for cfg, fname, line, msg in sw:
            print(f'  [{cfg}] {fname}:{line}  {msg}')
        print('\n提示：用 -Wconversion/-Wshadow 等开关暴露的是真实类型问题，'
              '\n      不要用强制转换糊过去——先确认语义再显式收敛。')
        return 1
    print(f'严格告警矩阵通过：{len(CONFIG_MATRIX)} 个配置 × '
          f'{len([f for f in os.listdir("core/src") if f.endswith(".c")])} 个文件，零告警')

    findings = scan()
    cur = {}
    for f in findings:
        cur[key(f)] = cur.get(key(f), 0) + 1

    if update or not os.path.exists(BASELINE):
        with open(BASELINE, 'w') as fh:
            json.dump(cur, fh, indent=2, sort_keys=True)
            fh.write('\n')
        print(f'WROTE {BASELINE}（{len(cur)} 类告警，共 {len(findings)} 条）')
        if cur:
            print('注意：基线里已有告警，属于「接受现状」而不是「代码干净」。')
            for k in sorted(cur):
                print(f'  {k}  x{cur[k]}')
        return 0

    with open(BASELINE) as fh:
        base = json.load(fh)

    new, gone = [], []
    for k, n in cur.items():
        if k not in base:
            new.append((k, n))
        elif n > base[k]:
            new.append((f'{k}（{base[k]}→{n}）', n))
    for k in base:
        if k not in cur:
            gone.append(k)

    if gone:
        print('已有告警消失（代码变好，可用 --update 收紧基线）：')
        for k in gone:
            print(f'  - {k}')
    if new:
        print('\n新增静态分析告警：')
        for k, n in new:
            print(f'  + {k} x{n}')
        for f in findings:
            if key(f) not in base:
                print(f'    {f["file"]}:{f["line"]}  {f["msg"]}')
        return 1
    print(f'STATIC ANALYSIS OK：自测有效，{len(findings)} 条告警均在基线内')
    return 0


if __name__ == '__main__':
    sys.exit(main())
