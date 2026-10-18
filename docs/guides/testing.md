# 测试

使用 CTest 进行自动化测试。

```sh
ctest --preset ninja-debug --output-on-failure
```

命令详情和生成器相关标志见 `cmake.md`。

## 测试框架

测试使用 vendored 的 [Unity](https://github.com/ThrowTheSwitch/Unity) 框架（MIT，位于 `tests/vendor/unity/` 下）。每个测试可执行文件在 `UNITY_BEGIN()` 和 `UNITY_END()` 之间用 `RUN_TEST()` 注册其测试用例，并且必须提供 `setUp()` 和 `tearDown()`，即使它们为空。vendored 的 Unity 源码不参与 `clang-format`、`clang-tidy` 和 `cppcheck` 检查。

## 期望

- 缺陷修复应包含回归测试。
- 新增公共 API 应包含成功与失败用例。
- 数值类 API 应覆盖相关的边界与溢出用例。
- 测试应断言行为，而非行覆盖率。
- 对于小型模块，优先编写聚焦的测试程序，而非宽泛的集成测试。
- 可安装的库应保留一个小型的下游包冒烟测试。

## 静态分析

CI 运行 clang-format、clang-tidy、cppcheck 和 YAML lint 检查。本地 clang-tidy 检查是可选的，因为工具可用性因平台而异：

**Linux/macOS**

```sh
./scripts/check.sh --enable-tidy
```

**Windows PowerShell**

```powershell
./scripts/check.ps1 -EnableTidy
```

## 需要运行的检查

| 变更类型 | 最低限度的有效检查 |
| --- | --- |
| C 源码或公共头文件 | `./scripts/check.ps1` 或 `./scripts/check.sh` |
| 动态库或导出宏变更 | `ninja-shared` 预设与动态库包冒烟测试 |
| 安装/包元数据变更 | 静态与动态包冒烟测试 |
| 子项目行为变更 | `tests/subproject_smoke/` |
| CI 或 YAML 变更 | `git diff --check` 与 CI 静态分析 |
| 发布敏感变更 | `ninja-release`、`ninja-shared`、`ninja-asan` 与包冒烟测试 |

## 包冒烟测试

`tests/package_smoke/` 是一个下游消费者，用于验证已安装的包元数据。它应保持精简，只断言已安装的头文件、导出的目标、C 链接以及基本库调用能够通过 `find_package` 正常工作。

在支持动态库时，为静态与动态两种安装运行包冒烟测试。

## 子项目冒烟测试

`tests/subproject_smoke/` 验证项目可以通过 `add_subdirectory()` 被消费，且默认不启用顶层示例、测试或安装规则。

## 质量门禁（CTest 注册）

除单元测试外，CTest 还注册了以下门禁。门禁的验收口径（扫描面下限、牙齿测试、空目录探针、四步注册）见 [C_PROJECT_STANDARD.md](../../C_PROJECT_STANDARD.md) 的「验证标准」一节。

文档门禁的扫描面是 **git 视角的活文档**（tracked ∪ untracked 且未被 ignore，排除 `build/` 与版本库内部目录）：被忽略的本地草稿（`.hermes/`、`dist/`）不会影响判定，同一提交在开发机与 CI 检出上结论一致；非 git 工作副本退回盘上遍历。`tokenprobe_gate_behaviors` 的 `TestDocGatesScanSurface` 锁定两个方向——忽略目录不进扫描面，活文档（含未入库但未被忽略的新文档）仍必须被判。

| CTest 名称 | 脚本 | 守护什么 | 失败输出怎么读 |
| --- | --- | --- | --- |
| `tokenprobe_decl_def_check` | `scripts/check_decl_def.py` | include/ 每个公共声明在 src/ 有定义；绝对扫描面下限 | `ERROR: 文件:行: public declaration without definition: 符号`；下限不足时首行 `FAIL: 解析失败：…仅 N 项（下限 M）` |
| `tokenprobe_layers_check` | `scripts/check_layers.py` | 公共头不引入第三方依赖；provider 不包含 OS/传输层头 | `FAIL: 文件:行: …不得包含/不得包含 …` |
| `tokenprobe_gate_behaviors` | `scripts/tests/test_gate_behaviors.py` | 门禁脚本自身「能失败」的性质（unittest） | unittest 标准输出，FAIL 行指名测试与断言 |
| `tokenprobe_version_consistency` | `scripts/check_version_consistency.py` | CMakeLists VERSION、生成头、CLI --version、CHANGELOG 最新段四处一致 | 首行 `FAIL:` 指名缺失/异常源，随后逐源列出四个值 |
| `tokenprobe_docs_check` | `scripts/check_docs.py` | 相对链接/锚点可达（URL 解码后判定）、代码围栏配对、README.md 文档索引与 docs/ 双向覆盖 | `ERROR: 文件:行: …`（死链/锚点/围栏/孤儿/索引死链） |
| `tokenprobe_doc_paths_check` | `scripts/check_doc_paths.py` | 活文档反引号路径在 git 视角（已入库 ∪ 已忽略 ∪ 历史并集）可达 | `ERROR: 文件:行: …未入库/不存在`；非 git 或浅克隆上下文报 `FAIL: 无法取证`（CI 需 `fetch-depth: 0`） |

门禁脚本的诊断输出固定为 UTF-8：Windows 控制台默认 GBK/cp1252，中文判定写到 stdout 会 `UnicodeEncodeError` 崩溃、写到 stderr 会退化成转义序列；脚本读取的 git 子进程输出同样显式按 UTF-8 解码（`git show` 回传 blob 原始字节）。`tokenprobe_gate_behaviors` 用 `PYTHONIOENCODING=cp1252` 复现旧控制台，锁定判定不随宿主编码变化。口径见 [C_PROJECT_STANDARD.md](../../C_PROJECT_STANDARD.md) 的「验证标准」一节。

依赖 git 取证的门禁（`tokenprobe_doc_paths_check`）要求工作副本是完整的 git 仓库；在非 git 目录或浅克隆中它会明确报 `FAIL: 无法取证`，而不是降级为跳过。

可选入口（默认不接入 CI 与 check.sh）：

- `scripts/coverage_ratchet.py` —— 覆盖率棘轮（基线只升不降；基线口径唯一平台，当前为 Linux/GCC）。
- `scripts/gate_floor_probe.sh` —— 把全部 `scripts/check_*.py` 指向空目录跑一遍，验证每条门禁都能失败（VACUOUS-PASS 探测器）。
- `scripts/static_analysis.sh` —— clang-format / clang-tidy / cppcheck 的单一实现入口，CI 与 `scripts/check.sh` 调同一份。

## 手动检查

当行为无法通过 CTest 验证时，在 PR 中记录手动检查。手动检查应包含命令、平台与观察到的结果。
