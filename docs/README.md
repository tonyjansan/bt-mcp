# BT-MCP 文档索引

> **从这里开始**：只想接入 → [`bt-mcp-interface-spec.html`](bt-mcp-interface-spec.html)；
> 要自己实现一份 → [`bt-mcp-protocol-spec.md`](bt-mcp-protocol-spec.md)（规范）；
> 要在本仓改代码 → [`bt-mcp-core-draft.md`](bt-mcp-core-draft.md)（实现说明）。

## 活文档（随代码持续更新）

| 文档 | 内容 | 权威性 |
|---|---|---|
| [`bt-mcp-protocol-spec.md`](bt-mcp-protocol-spec.md) | **协议规范（normative）**：通道抽象、分帧格式、控制帧、会话语义、MCP 方法面、安全、兼容性矩阵 | 给**第三方实现者**；线上格式与语义以此为准 |
| [`bt-mcp-core-draft.md`](bt-mcp-core-draft.md) | **实现说明（informative）**：设计原则、模块划分、完整 C API 骨架、port 清单、配置与内存档位、验证体系 | 给**本仓维护者**；与公开头文件同步（`tools/check_docs.py` 会检查） |
| [`bt-mcp-interface-spec.html`](bt-mcp-interface-spec.html) | 集成者规约：port 如何接、字节流怎么喂、控制帧语义、最小 E2E 验收 | 面向移植/接入者 |
| [`porting-guide.md`](porting-guide.md) | **移植指南**：4 个接入点（emit/feed/cap/tick）、通道语义对照表、初始化顺序、上板 6 步验收清单、两个必须知道的实现约束 | 面向移植者；含「已知未验证项」清单 |
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

**v0.7 / v0.8 / v0.9.x 的记录在哪**（这三个版本**没有**独立的 roadmap/validation 文件——
那段时间的规划与执行合并进了同一份滚动文档）：

| 版本 | 规划与执行结果 | 对外变更 | 标签 |
|---|---|---|---|
| v0.7（清欠账） | [`roadmap-v0.7.md`](roadmap-v0.7.md) §2 的「执行结果」表 | CHANGELOG 0.7.0 | `v0.7.0` |
| v0.8（真实平台落地） | [`roadmap-v0.7.md`](roadmap-v0.7.md) §3 | CHANGELOG 0.8.0 | `v0.8.0` |
| v0.9（可信度） | [`roadmap-v0.7.md`](roadmap-v0.7.md) §4 | CHANGELOG 0.9.0 | `v0.9.0` |
| v0.9.1（清遗留） | [`roadmap-v0.7.md`](roadmap-v0.7.md) §4.5 | CHANGELOG 0.9.1 | `v0.9.1` |

> 为什么不再单独建文件：v0.7 起的迭代是**在同一个里程碑（M1）下连续推进**的，
> 拆成四份 roadmap 会有大量重复的出口判据与风险表。滚动文档 + CHANGELOG
> 已经能回答「当时为什么这么决定」和「当时测得多少」，这是历史文档的唯一价值。

## 工具（`tools/`，CI 门禁与测量）

| 工具 | 作用 | 是否门禁 |
|---|---|---|
| `check_docs.py` | 文档↔代码一致性：符号存在性、基线数字、版本标记 | ✅ 失败即红 |
| `ram_baseline.py` + `ram_baseline.json` | 12 组合静态 RAM 基线比对 | ✅ 回归即红 |
| `static_analysis.py` + `static_analysis.json` | `clang --analyze` 零告警基线 + **6 配置 × 8 文件 × 10 严开关**矩阵；**自带自测**（喂已知缺陷必须报出） | ✅ |
| `baselines.json` | **回归基线数字的单一来源**（差分/HIL/单元/矩阵/SDK/golden/覆盖率） | 被 `check_docs.py` 读取 |
| `check_example_ports.py` | 用 stub 头校验参考移植实现是否只用真实存在的 core API | ✅ |
| `footprint.py` | 代码体积：`--arm` 用真 Thumb-2 交叉编译（`tools/arm_freestanding/` 提供最小 libc 声明） | 报告 |
| `coverage.sh` | 行/分支覆盖率（门禁 95/69） | ✅ |
| `bench.c` | 封帧开销基线（MTU 放大、流通道固定 +6B） | 报告 |

## 未来规划

| 文档 | 内容 |
|---|---|
| [`roadmap-v0.7.md`](roadmap-v0.7.md) | **v0.7 → 中期里程碑 M1「可交付」（v1.0-rc）**：v0.7 清欠账、v0.8 真实平台落地、v0.9 可信度与文档，含出口判据 E1–E8、明确不做的事、依赖与检查点 |

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

1. **改公开头文件 → 同步 `bt-mcp-core-draft.md` 的 §7（C API 骨架）**，否则 `tools/check_docs.py` 会失败。
   **改线格式或语义 → 同步 `bt-mcp-protocol-spec.md`。**
2. **改回归基线数字 → 只改 `tools/baselines.json`，再让 README 与草案跟随它**
   （`tools/check_docs.py` 从该文件读取期望值后逐处比对）。
   **不要**在文档或检查器里分别硬编码数字——v0.9.1 前正是这么做的，
   结果 v0.7–v0.9 三次发布后文档仍写着旧值而门禁照样通过。
   版本号同理，单一来源是仓库根的 `VERSION`。
3. **新增测试 → 在 `build.sh` 的对应分组里登记**，并按需加入 `tools/coverage.sh`。
4. **截图/大文件不入库**；`build/`、`oracle/target/`、`node_modules/` 已在 `.gitignore`。
