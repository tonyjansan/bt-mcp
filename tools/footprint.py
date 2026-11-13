#!/usr/bin/env python3
"""footprint.py —— 核心代码的 Flash/RAM 占用估算（v0.8 B3）。

## 这个工具解决什么问题

`tools/ram_baseline.py` 报的是 **sizeof(结构体)**，那是「静态 RAM 需求」。
但真实固件占用还包括**代码体积（Flash）**，两者量级完全不同：
FULL 档结构体 56KB，而代码约 27KB —— 只报前者会让人误判。
本工具量后者。

## 诚实的前提（重要）

本机是 macOS，**没有 ARM 工具链**（arm-none-eabi-size 缺失）。
所以这里用宿主 clang 编译并用 `size -m` 解析分段。含义：

- **能信**：各档位之间的**相对关系**、以及分片开关（FCS16/TRACE/共享重组）
  对代码体积的影响方向与幅度；
- **不能信**：绝对值。ARM/Thumb 指令密度与 x86-64 差异可达 1.5~2 倍，
  且真实固件还要加 libc、栈、port 胶水。

M1 里程碑的 E4 要求「真实固件增量，与预测误差 ≤15%」—— 那必须用
`arm-none-eabi-size` 量 demo 固件。本工具是 E4 之前能做到的最好近似，
并且**明确标注它不是 E4**。

## v0.9.1：ARM/Thumb-2 真交叉编译（大幅提升保真度）

发现 clang 自带 Thumb-2 后端（`-target thumbv7m-none-eabi`），只是缺 ARM libc 头。
core 总共只依赖 8 个 libc 函数，于是把最小声明放进 `tools/arm_freestanding/`，
就能**真的编译出 Thumb-2 目标文件**并用 `llvm-size` 量出字节数。

这比"宿主 x86-64 近似"可信得多。仍然不能替代 `arm-none-eabi-gcc` 的精确
链接后体积（真实固件还要加 libc 实现、启动代码、栈），但已经能回答
「哪个开关贵、贵多少、绝对量级是多少」——E4 的实质问题。

## 用法

    python3 tools/footprint.py            # 宿主（x86-64）表
    python3 tools/footprint.py --arm      # ARM/Thumb-2 真交叉编译
    python3 tools/footprint.py --json     # 机器可读
"""
import json
import os
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
os.chdir(ROOT)

OUT = 'build/footprint'
CC = os.environ.get('CC', 'cc')

TIERS = ['COMPACT', 'STANDARD', 'FULL']
SRCS = ['frame', 'session', 'mcp_server', 'mcp_client', 'json', 'writer', 'err']
# 这些开关会改变代码体积（不是 RAM）
VARIANTS = {
    'default': '',
    'fcs16': '-DBT_MCP_CFG_FCS16=1',
    'trace': '-DBT_MCP_CFG_TRACE=1',
    'shared_reasm': '-DBT_MCP_CFG_SHARED_REASM=1',
}


def parse_size_m(obj):
    """解析 `size -m` 输出，返回 {section: bytes}。"""
    r = subprocess.run(['size', '-m', obj], capture_output=True, text=True)
    if r.returncode != 0:
        return None
    secs = {}
    for line in r.stdout.splitlines():
        line = line.strip()
        if not line.startswith('Section ('):
            continue
        # 形如: Section (__TEXT, __text): 25104
        # 注意括号里有 **两段**（segname, sectname），要取后一段。
        try:
            inside = line.split('(')[1].split(')')[0]
            name = inside.split(',')[-1].strip()
            size = int(line.rsplit(':', 1)[1].strip())
        except (IndexError, ValueError):
            continue
        secs[name] = secs.get(name, 0) + size
    return secs


def build_and_measure(tier, variant_flags):
    os.makedirs(OUT, exist_ok=True)
    objs = []
    for s in SRCS:
        o = f'{OUT}/{tier}_{s}.o'
        cmd = (f'{CC} -O2 -c -std=c99 -ffunction-sections -fdata-sections '
               f'-Icore/include -DBT_MCP_TIER=BT_MCP_TIER_{tier} '
               f'{variant_flags} core/src/{s}.c -o {o}').split()
        r = subprocess.run(cmd, capture_output=True, text=True)
        if r.returncode != 0:
            raise SystemExit(f'编译失败 {s}@{tier}: {r.stderr[:300]}')
        objs.append(o)

    total = {}
    for o in objs:
        secs = parse_size_m(o)
        if secs is None:
            raise SystemExit(f'size -m 失败: {o}')
        for k, v in secs.items():
            total[k] = total.get(k, 0) + v

    # 归类：
    #   Flash ≈ 代码 + 只读常量 + 字符串字面量
    #   RAM   ≈ 零初始化(.bss) + 已初始化(.data) + 常量（MCU 上 const 可能在 RAM 或 flash）
    flash = sum(v for k, v in total.items()
                if k in ('__text', '__const', '__cstring', '__literal8',
                         '__const_cfstring'))
    bss = sum(v for k, v in total.items() if k == '__bss')
    data = sum(v for k, v in total.items() if k == '__data')
    return {'flash': flash, 'bss': bss, 'data': data, 'sections': total}


ARM_TARGET = 'thumbv7m-none-eabi'   # Cortex-M3 类（nRF52/SAMD2x 常见档）
ARM_INC = 'tools/arm_freestanding'


def find_llvm_size():
    for cand in ('llvm-size',):
        r = subprocess.run(['which', cand], capture_output=True, text=True)
        if r.returncode == 0:
            return cand
    # Xcode 里通常不在 PATH
    import glob
    for pat in ('/Applications/Xcode*.app/Contents/Developer/Toolchains/*/usr/bin/llvm-size',
                '/usr/local/opt/llvm/bin/llvm-size', '/opt/homebrew/opt/llvm/bin/llvm-size'):
        hits = glob.glob(pat)
        if hits:
            return hits[0]
    return None


def arm_measure(tier, extra_flags=()):
    """真交叉编译到 Thumb-2，返回 {file: text}。"""
    sz = find_llvm_size()
    if not sz:
        return None
    os.makedirs(OUT, exist_ok=True)
    out = {}
    for src in sorted(f for f in os.listdir('core/src') if f.endswith('.c')):
        obj = f'{OUT}/arm_{tier}_{src}.o'
        cmd = [CC, '-target', ARM_TARGET, '-mthumb', '-c', '-O2', '-std=c99',
               '-ffreestanding', '-nostdlib', f'-I{ARM_INC}', '-Icore/include',
               f'-DBT_MCP_TIER=BT_MCP_TIER_{tier}'] + list(extra_flags)
        cmd += [f'core/src/{src}', '-o', obj]
        r = subprocess.run(cmd, capture_output=True, text=True)
        if r.returncode != 0:
            raise SystemExit(f'ARM 编译失败 {src}@{tier}: {r.stderr[:300]}')
        r = subprocess.run([sz, obj], capture_output=True, text=True)
        parts = r.stdout.splitlines()[1].split()
        out[src] = int(parts[0]) + int(parts[1])      # text + data
    return out


def main_arm():
    if not find_llvm_size():
        print('SKIP: 找不到 llvm-size（需要 LLVM 工具链；macOS 自带 size 读不了 ELF）')
        return 0
    print(f'ARM/Thumb-2 目标代码体积（真交叉编译 -target {ARM_TARGET}）\n')
    print(f'{"档位/配置":<26} {"text+data":>10}')
    print('-' * 40)
    rows = {}
    for tier in TIERS:
        for vname, vflags in VARIANTS.items():
            m = arm_measure(tier, vflags.split() if vflags else [])
            tot = sum(m.values())
            rows[f'{tier}/{vname}'] = tot
            print(f'{tier}/{vname:<26} {tot:>10}')
    print('\n相对关系（同档位下各开关的增量）：')
    for tier in TIERS:
        base = rows[f'{tier}/default']
        parts = [f'{v} {rows[f"{tier}/{v}"] - base:+d}'
                 for v in VARIANTS if v != 'default']
        print(f'  {tier:<9} base={base:>7}  ' + '  '.join(parts))
    print('\n注意：这是**目标代码**体积，不含 libc 实现、启动代码、栈与 port 胶水。')
    print('      它可信地回答「哪个开关贵、绝对量级多少」，但仍不是链接后的固件尺寸。')
    return 0


def main():
    if '--arm' in sys.argv:
        return main_arm()
    as_json = '--json' in sys.argv
    result = {}
    for tier in TIERS:
        for vname, vflags in VARIANTS.items():
            key = f'{tier}/{vname}'
            result[key] = build_and_measure(tier, vflags)

    if as_json:
        print(json.dumps(result, indent=2, sort_keys=True))
        return 0

    print('核心代码体积估算（宿主 clang，仅供相对比较；ARM 实测见 M1-E4）\n')
    print(f'{"档位/配置":<26} {"Flash":>8} {"bss":>8} {"data":>7}')
    print('-' * 54)
    for key in sorted(result):
        r = result[key]
        print(f'{key:<26} {r["flash"]:>8} {r["bss"]:>8} {r["data"]:>7}')

    print('\n相对关系（同一档位下各开关的影响）：')
    for tier in TIERS:
        base = result[f'{tier}/default']['flash']
        parts = []
        for vname in VARIANTS:
            if vname == 'default':
                continue
            d = result[f'{tier}/{vname}']['flash'] - base
            parts.append(f'{vname} {d:+d}')
        print(f'  {tier:<9} base={base:>6}  ' + '  '.join(parts))

    print('\n注意：')
    print('  - Flash 含 __text/__const/__cstring/__literal8（MCU 上都在 Flash）。')
    print('  - bss 是零初始化静态量；core 的 session/server 由**调用方**提供，')
    print('    不计入这里（详见 tools/ram_baseline.py 的 sizeof 口径）。')
    print('  - 绝对值不可用于选型：ARM/Thumb 与 x86-64 指令密度差异可达 1.5~2x。')
    return 0


if __name__ == '__main__':
    sys.exit(main())
