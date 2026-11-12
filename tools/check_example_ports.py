#!/usr/bin/env python3
"""check_example_ports.py —— 校验参考移植实现用的是**真实存在**的 core API。

动机（v0.8 B2 实测）：手写 Zephyr 参考实现时，我犯了 3 个错误，
全都被「用 stub 头做语法检查」抓到：
  1. `bt_mcp_framer_feed` 需要 5 个参数，我只传了 4 个（真 API 误用）；
  2. 我编了一个不存在的 `BT_MCP_TICK_MS` 宏（core 不规定 tick 周期）；
  3. `bt_gatt_attr[2]` 式句柄查找在真实 NimBLE 里不成立。

本机没有 Zephyr SDK，无法真编译；但「API 名与参数个数是否真实存在」
是可以静态校验的——这正是最容易错、也最误导下游的部分。
所以本脚本用 stub 头 + `cc -fsyntax-only` 把这类错误挡在提交前。

用法：python3 tools/check_example_ports.py
"""
import os
import shutil
import subprocess
import sys
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
os.chdir(ROOT)

ZEPHYR_DIR = 'examples/zephyr'

# 最小 stub：只声明参考实现用到的 NimBLE/Zephyr 符号，**不求语义正确**，
# 只求「名字存在 + 参数个数对得上」，这样拼错的 core API 会暴露出来。
STUBS = {
    'zephyr/kernel.h': r'''
#pragma once
#include <stdint.h>
#include <stddef.h>
typedef uint32_t k_timeout_t;
#define K_MSEC(x) ((k_timeout_t)(x))
struct k_work { int _x; };
struct os_mbuf;
static inline uint64_t k_uptime_get(void) { return 0; }
#define K_WORK_DEFINE(n, f) struct k_work n
static inline int k_work_schedule(struct k_work *w, k_timeout_t t)
{ (void)w; (void)t; return 0; }
#define OS_MBUF_PKTLEN(o) 0
static inline struct os_mbuf *os_msys_get_pkthdr(uint16_t a, uint16_t b)
{ (void)a; (void)b; return 0; }
static inline int os_mbuf_append(struct os_mbuf *o, const void *d, uint16_t n)
{ (void)o; (void)d; (void)n; return 0; }
static inline void os_mbuf_free_chain(struct os_mbuf *o) { (void)o; }
''',
    'zephyr/bluetooth/bluetooth.h': '#pragma once\n#include <stdint.h>\n',
    'zephyr/bluetooth/gatt.h': r'''
#pragma once
#include <stdint.h>
#include <stddef.h>
#include <zephyr/bluetooth/bluetooth.h>
struct bt_conn;
struct bt_gatt_attr;
struct bt_gatt_service { struct bt_gatt_attr *attrs; size_t n; };
#define BT_UUID_DECLARE_16(x) ((void *)(uintptr_t)(x))
#define BT_GATT_PRIMARY_SERVICE(u) { ._x = 0 }
#define BT_GATT_CHARACTERISTIC(u, p, perm, rd, wr, usr) { ._x = 0 }
#define BT_GATT_CCC(c, p) { ._x = 0 }
#define BT_GATT_CHRC_WRITE 1
#define BT_GATT_CHRC_WRITE_WITHOUT_RESP 2
#define BT_GATT_CHRC_NOTIFY 4
#define BT_GATT_PERM_WRITE 1
#define BT_GATT_PERM_READ 2
#define BT_GATT_SERVICE_DEFINE(n, ...) struct bt_gatt_service n = { 0, 0 }
static inline uint16_t bt_gatt_attr_get_handle(struct bt_gatt_attr *a)
{ (void)a; return 1; }
static inline int bt_gatt_service_register(struct bt_gatt_service *s)
{ (void)s; return 0; }
static inline int bt_gatt_notify_cb(struct bt_conn *c, void *p, size_t o,
                                    void *d, struct os_mbuf *m)
{ (void)c; (void)p; (void)o; (void)d; (void)m; return 0; }
''',
    'zephyr/bluetooth/conn.h': r'''
#pragma once
#include <stdint.h>
struct bt_conn;
struct bt_conn_interval { uint16_t interval; };
struct bt_conn_le_info { struct bt_conn_interval data; };
struct bt_conn_info { struct { struct bt_conn_le_info data; } le; };
static inline struct bt_conn *bt_conn_ref(struct bt_conn *c) { return c; }
static inline void bt_conn_unref(struct bt_conn *c) { (void)c; }
static inline int bt_conn_get_info(struct bt_conn *c, struct bt_conn_info *i)
{ (void)c; (void)i; return 0; }
struct bt_conn_cb { void (*connected)(struct bt_conn *, uint8_t);
                    void (*disconnected)(struct bt_conn *, uint8_t); };
#define BT_CONN_CB_DEFINE(n) struct bt_conn_cb n
''',
    'zephyr/sys/byteorder.h': '#pragma once\n',
}


def main():
    cc = os.environ.get('CC', 'cc')
    if not os.path.isdir(ZEPHYR_DIR):
        print(f'SKIP: 没有 {ZEPHYR_DIR}')
        return 0
    srcs = [os.path.join(ZEPHYR_DIR, f) for f in sorted(os.listdir(ZEPHYR_DIR))
            if f.endswith('.c')]
    if not srcs:
        print(f'SKIP: {ZEPHYR_DIR} 里没有 .c')
        return 0

    tmp = tempfile.mkdtemp(prefix='btmcp_stubs_')
    try:
        for rel, body in STUBS.items():
            p = os.path.join(tmp, rel)
            os.makedirs(os.path.dirname(p), exist_ok=True)
            with open(p, 'w') as f:
                f.write(body)

        bad = []
        for src in srcs:
            cmd = [cc, '-fsyntax-only', '-std=c99', f'-I{tmp}',
                   '-Icore/include', '-Iport', src]
            r = subprocess.run(cmd, capture_output=True, text=True)
            if r.returncode != 0:
                errs = [l for l in r.stderr.splitlines() if 'error' in l]
                bad.append((src, errs[:6]))

        if bad:
            print('参考移植实现里有对 core API 的错误用法：')
            for src, errs in bad:
                print(f'  {src}:')
                for e in errs:
                    print(f'    {e}')
            print('\n提示：这些通常是「参数个数不对」或「用了不存在的宏/字段」。')
            return 1
        print(f'EXAMPLE PORTS OK: {len(srcs)} 个参考实现，core API 用法均可解析')
        return 0
    finally:
        shutil.rmtree(tmp, ignore_errors=True)


if __name__ == '__main__':
    sys.exit(main())
