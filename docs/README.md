# BT-MCP 文档索引

> **从这里开始**：只想接入 → [`bt-mcp-interface-spec.html`](bt-mcp-interface-spec.html)；
> 想改协议/看设计 → [`bt-mcp-core-draft.md`](bt-mcp-core-draft.md)。

## 活文档（随代码持续更新）

| 文档 | 内容 | 权威性 |
|---|---|---|
| [`bt-mcp-core-draft.md`](bt-mcp-core-draft.md) | **协议与接口权威文档**：设计原则、线格式、控制帧、会话语义、MCP 方法面、完整 C API 骨架、配置项、内存档位、实测开销 | 与公开头文件同步，改 API 必须同步本文件（`tools/check_docs.py` 会检查） |
| [`bt-mcp-interface-spec.html`](bt-mcp-interface-spec.html) | 集成者规约：port 如何接、字节流怎么喂、控制帧语义、最小 E2E 验收 | 面向移植/接入者 |
| [`../README.md`](../README.md) | 项目概览、当前状态、构建与测试入口、能力覆盖表 | 门面 |
| [`../CHANGELOG.md`](../CHANGELOG.md) | 按版本记录对外变更（含破坏性变更与迁移方式） | 变更事实的单一来源 |

## 各版本记录（历史，不再修改）

每个版本三个角度：**规划**（当时的判断）→ **发布说明**（对外口径）→ **验证报告**（实测数据）。

| 版本 | 规划 | 发布说明 | 验证报告 | 标签 |
|---|---|---|---|---|
| v0.3 | — | [`release-notes-v0.3.md`](release-notes-v0.3.md) | [`v0.3-validation.md`](v0.3-validation.md) | `v0.3.0` |
| v0.4 | [`roadmap-v0.4.md`](roadmap-v0.4.md) | [`release-notes-v0.4.md`](release-notes-v0.4.md) | [`v0.4-validation.md`](v0.4-validation.md) | `v0.4.0` / `v0.4.1` |
| v0.5 | [`roadmap-v0.5.md`](roadmap-v0.5.md) | — （见 CHANGELOG 0.5.0） | [`v0.5-validation.md`](v0.5-validation.md) | `v0.5.0` |
| v0.6 | [`roadmap-v0.6.md`](roadmap-v0.6.md) | — （见 CHANGELOG 0.6.0） | — （数据并入本文档与本版 CHANGELOG） | `v0.6.0` |

**为什么这些历史文档不删**：它们记录了「当时为什么这么决定」以及「当时的实测数字」，
是复盘后续问题的唯一依据。例如 v0.5 写下的「分支覆盖目标 75%」和 v0.6 记录的
「该预测被证伪」，只有对照着看才有价值。**不要用当前认知去改写历史文档**；
有被取代的结论时，在原文追加「注（vX 整理）」说明即可。

## 本版已合并/删除的文档

| 曾经存在的文件 | 处置 |
|---|---|
| `tests/probe_verify.c` | **删除**。v0.3 期的临时验证脚本，不被任何构建引用，且断言在 v0.4+ 后已失效（跑起来 3 项失败）。其覆盖内容已被 `test_m2` / `test_reliability` / `test_client` 完整取代。 |
| `tests/test_threads.c` | **删除**。2×2 并发用例，已被 `test_stress.c`（4×4 + 断链重连 + 池分派 + TSan）取代。 |
| `tests/test_l5_fuzz.c` | **删除**。确定性随机输入，已被 `fuzz/fuzz_l5.c`（结构变异 + 四条不变量 + 更大规模）取代。 |
| `tests/test_m4c_port.c` → 保留 | 仍是 port 层 HIL 的唯一来源（198 项）。 |

## 维护约定

1. **改公开头文件 → 同步 `bt-mcp-core-draft.md` 的 §7**，否则 `tools/check_docs.py` 会失败。
2. **改回归基线数字 → 同步 README / 草案 / 验证报告三处**（同一脚本检查）。
3. **新增测试 → 在 `build.sh` 的对应分组里登记**，并按需加入 `tools/coverage.sh`。
4. **截图/大文件不入库**；`build/`、`oracle/target/`、`node_modules/` 已在 `.gitignore`。
