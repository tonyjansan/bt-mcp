# bt-mcp

Bluetooth-carried MCP (Model Context Protocol) stack — a platform-agnostic
C99 protocol core that runs MCP over BLE GATT / Classic SPP / any byte
channel, with zero vendor ifdefs and zero OS dependencies.

## Status (2026-11-04)

Project M1–M4 complete, all milestones closed:

| Layer | Module | Notes |
|---|---|---|
| L3 | `core/src/frame.c` | 0xB7 framing / reassembly, PACKET vs STREAM channels |
| L4 | `core/src/session.c` | IDLE→LINK_UP→READY state gates, credit window, RESUME token bound to bond |
| L5 | `core/src/mcp_server.c` | JSON-RPC dispatch (initialize / tools/list / tools/call / ping) |
| port | `port/bt_mcp/` | GATT + SPP adapters; core unchanged across both (M4c) |

Regression baseline: **1335 protocol diff checks + 184 port HIL checks, all
green**; differential fuzzing: **1e9 iterations, ZERO CRASH**.

## Layout

```
core/include/bt_mcp/  public headers (capability, config, err, frame, session, json, writer, mcp_server)
core/src/             protocol core (C99, no OS deps)
port/bt_mcp/          channel adapters: gatt_port.h, spp_port.h (header-only, ring-buffer transport)
mock/                 C mock binary speaking the differential-harness instruction protocol
oracle/               independent Rust oracle for differential testing
tests/                diff harnesses (M1/M3/M4a/M4b) + HIL unit tests
fuzz/                 standalone differential fuzz engine
docs/                 bt-mcp-interface-spec.html — Proxy/Host integration spec
build.sh              build C mock + Rust oracle, run all diff suites
```

## Build & test

```
./build.sh            # runs M1 diff + M3 session + M4 mcp + M4b e2e harnesses
```

Port-layer HIL test (compiled separately, see `tests/test_m4c_port.c`):

```
cc -O2 -std=c99 -Wall -Wextra -Werror -Icore/include -Iport \
   tests/test_m4c_port.c core/src/frame.c core/src/session.c \
   -o build/test_m4c_port && ./build/test_m4c_port
```

Protocol draft: [`docs/bt-mcp-core-draft.md`](docs/bt-mcp-core-draft.md) (BT-MCP v0.2).
