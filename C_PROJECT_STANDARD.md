# C Project Standard

本文档为中小型 C 项目定义一套务实的基线。

## 目录

- [核心原则](#核心原则)
- [必备项目形状](#必备项目形状)
- [CMake 标准](#cmake-标准)
- [测试标准](#测试标准)
- [头文件边界标准](#头文件边界标准)
- [CI 标准](#ci-标准)
- [验证标准](#验证标准)
- [仓库维护标准](#仓库维护标准)
- [文档标准](#文档标准)
- [默认避免](#默认避免)


## 核心原则

- 从简单开始，让接缝保持可见。
- 公共 API 放在 `include/`；实现细节留在 `src/`。
- 只有在重复的具体使用之后才引入抽象。
- 从第一个提交起就让构建、测试和 CI 可靠。
- 保持文档简短、就近，并绑定在影响贡献者的决策上。

## 必备项目形状

```text
include/<project>/   公共头文件
src/<project>/       库实现
src/main.c           可选的 CLI 或应用入口
tests/               聚焦的 CTest 测试
cmake/               CMake 模块
scripts/             本地引导与检查脚本
docs/                简短文档
docs/guides/         主题指南（中文）
docs/adr/            架构决策记录（中文）
.github/workflows/   CI
```

## CMake 标准

使用一个小的顶层 `CMakeLists.txt`，把工作委托给模块：

- `cmake/CompilerOptions.cmake` 负责警告、sanitizer 和覆盖率。
- `cmake/Dependencies.cmake` 负责第三方依赖。
- `cmake/Sources.cmake` 负责源码清单。
- `cmake/Tests.cmake` 负责测试目标。
- `cmake/Packaging.cmake` 负责安装与打包规则。

可安装的库应生成 CMake 包配置文件，让下游项目能使用
`find_package(<Project> CONFIG REQUIRED)`。

除非项目有理由只发布静态或只发布共享制品，库应支持 `BUILD_SHARED_LIBS`。
公共头文件应暴露一个小的导出宏，并通过 `extern "C"` guard 可供 C++ 调用方
使用。

只属于顶层的便利设施（如示例、测试和安装规则）在常规仓库构建中应默认开启，
在项目被作为子项目引入时应默认关闭。

同时支持安装和子项目消费的项目，应为两条路径保留小的冒烟测试。

语言与工具链策略应显式：

- 在 CMake 中把项目语言标准设为目标级要求，例如
  `target_compile_features(<library> PUBLIC c_std_11)`。
- 可移植 C 库默认以 C11 为基线，除非项目书面说明了要求更新标准的理由。
- 支持编译器族而不是某一个本地编译器路径：MSVC、GCC、Clang 和 AppleClang
  是常规基线。
- 除非某个 preset 就是为了记录依赖 generator 的工作流而存在，提交的 preset
  应保持 generator 中立。
- 当项目需要稳定的 `compile_commands.json` 供 clangd 使用时，为默认的编辑器
  与 CI 工作流提供显式的 `ninja-*` preset。
- 编译器安装路径、SDK 路径和开发者特定的 CMake kit 放在用户 preset 或本地
  IDE 配置中，不放在共享项目文件里。
- 当仓库支持常见的本地 generator 替代方案时，提供
  `CMakeUserPresets.example.json`。
- 当首次运行的失败更可能是工具链或环境问题而不是源码问题时，提供引导脚本。
- 记录哪些工具是用户必须安装的，哪些只是可选的编辑器便利。仓库不应假设
  CMake、编译器、Ninja、VS Code 扩展、clangd 或静态分析工具已经存在。
- 只提交可移植的编辑器模板文件。VS Code 推荐、通用 CMake 任务、`.clangd`
  后备标志和可选后备示例是有用的；绝对编译器路径和个人工作区设置不是。

推荐的主 preset：

- `ninja-debug`
- `ninja-release`
- `ninja-shared`
- `ninja-asan`
- `ninja-coverage`

推荐的 generator 中立兼容 preset：

- `debug`
- `release`
- `shared`
- `asan`
- `coverage`

## 测试标准

- 使用 CTest 作为通用测试运行器。
- 使用 vendored Unity 框架（MIT，`tests/vendor/unity/`）做测试断言。
  每个测试可执行文件提供 `setUp()`/`tearDown()`。
- 优先为每个模块或子系统提供聚焦的测试可执行文件。
- 除非细节是有书面记录的内部契约，否则不测试实现细节。
- Bug 修复应包含回归测试，除非该行为只能人工观察。
- 可安装的库应包含一个包冒烟测试，用 `find_package` 构建一个小的下游项目。

## 头文件边界标准

公共：

```text
include/<project>/feature.h
```

内部：

```text
src/<project>/feature_internal.h
```

规则：

- 公共头文件只包含它需要的内容。
- 公共类型应稳定且有文档。
- 内部头文件可以暴露实现状态，但必须与公共消费者隔离。

## CI 标准

每个项目应有：

- 在受支持编译器族（MSVC、GCC、Clang、AppleClang）上的 debug 构建与测试。
- release 构建与测试。
- 支持 `BUILD_SHARED_LIBS` 时的共享库构建与测试。
- 在支持的平台上做 sanitizer 构建，覆盖所有支持 sanitizer 标志的平台与编译器。
- 至少使用 `cppcheck` 的静态分析。
- 项目提供 `.clang-tidy` 配置时运行 clang-tidy。
- 工作流与自动化文件的 YAML lint。
- 提供覆盖率 preset 的项目做覆盖率 preset 验证。
- 可安装库在各平台上的包冒烟测试。

## 验证标准

门禁（quality gate）的价值在于它**能失败**。本节定义门禁本身的验收口径。

### 门禁扫描面下限

每条扫描型门禁（解析源码、文档、配置的脚本）必须声明扫描面下限：解析出的条目数低于下限即报「解析失败」并以非 0 退出，而不是打印 OK。

理由：「解析到 0 项却打印 OK」是最危险的假绿——扫描路径写错、目录改名、正则失配都会让门禁静默变成空操作。下限取实测基线 × 0.8（在注释里写明口径与日期）；结构性删除确实会降低计数时，必须在同一笔改动里下调下限并写明原因。

模板与生成项目的扫描面天然不同（wizard 生成项目只留桩代码），因此下限分两档：脚本默认是**绝对下限**（≥1，模板与生成项目都适用），模板自身的**严格下限**（基线 × 0.8）由 CTest 注册时显式传入，并放在 wizard markers 内，生成项目不继承。

扫描面本身也必须与工作目录解耦：判定对象是 **git 视角的活文件**（tracked ∪ untracked 且未被 ignore），被忽略的本地草稿（`.hermes/`、`dist/`、`Okk/`）不得影响结论。理由同上——扫描面取自工作目录时，同一提交在开发机（有草稿）与 CI 检出（无草稿）会给出不同结论，这属于环境依赖缺陷，不是本地噪声。非 git 工作副本下没有 ignore 语义可用，退回盘上遍历，并在脚本的扫描面对齐一节写明。

### 门禁必须能自证失败

不会失败的门禁等于没有门禁。「跑绿了」不是证据；每条门禁交付前必须做牙齿测试：

1. 破坏被守护项（删定义、改版本号、断链接、改写已发布段落……）。
2. 确认门禁报错，且输出**指名 `文件:行`**（或等价的精确位置）。
3. 回滚破坏。
4. 确认复绿。

同时每条门禁必须通过**空目录探针**：把扫描面指向空目录，必须非 0 退出并打印指名诊断。`scripts/gate_floor_probe.sh` 把全部 `scripts/check_*.py` 逐个空跑，任何一条 exit 0 即判 VACUOUS-PASS。

门禁脚本自身要有单元测试（`scripts/tests/`，unittest，纯标准库），断言的正是「能失败」这个性质。

### 缺件即失败（不静默降级）

发布与打包链路上，任何 `2>/dev/null || true`、`if-no-files-found: warn` 式的吞错都是缺陷：制品没产出，Release 照建，用户拿到不完整包且没有任何失败信号。

口径：制品收集段必须计数，缺任一预期包即 `exit 1` 并指名缺哪个；上传步骤用 `if-no-files-found: error`。checksum 生成失败必须让 job 失败。

### 新增门禁的四步注册

1. 脚本放 `scripts/check_*.py`（纯标准库、跨平台、`pathlib` 路径、不硬编码项目名——改名安全靠 `scripts/rename-project.*` 之后仍能用）。
2. 在 `cmake/Tests.cmake` 注册为 CTest：依赖构建产物的放 wizard markers 内（生成项目自动注释掉）；依赖 git 的同样放 markers 内（生成项目创建时不是 git 仓库），且**非 git 上下文必须报「无法取证」exit 1，不降级为跳过**（跳过 ≠ 通过）。
3. 补牙齿测试与空目录探针证据（进 `scripts/tests/test_gate_behaviors.py` 或交付记录）。
4. 在本节与 `docs/guides/testing.md` 登记该门禁守护什么、失败时怎么读输出。

### git 视角门禁需要完整历史

判定依赖 git 工作副本的门禁，其证据前提是**完整历史与 tag**。浅克隆（`git clone --depth 1`，GitHub Actions 的 checkout 步骤默认只取一个提交）会让「历史并集」退化成单个提交、「tag 列表」退化为空——此时「路径不存在」与「无 tag 段落」都不是发现，而是假结论：前者假红，后者假绿。

口径：这类门禁检测到 `git rev-parse --is-shallow-repository` 为真时报「无法取证」并 exit 1（不降级为跳过）；跑这些门禁的 CI job 必须 `fetch-depth: 0`。同一门禁在本地完整克隆与 CI 浅克隆下给出不同结论，本身就是缺陷，不是环境差异。

### 门禁的文本编码口径唯一

门禁的诊断输出与被读取的子进程文本必须显式按 UTF-8 处理，不得依赖宿主 locale 或控制台代码页：

- 打印诊断前把 `sys.stdout` / `sys.stderr` 钉到 UTF-8。Windows 控制台默认 GBK（中文机器）或 cp1252（runner）：中文诊断写 stdout 会抛 `UnicodeEncodeError` 把门禁变成崩溃，写 stderr 会退化成转义序列把判定变成噪声。向导 CLI 用 `SetConsoleOutputCP(CP_UTF8)` 解决同一个问题，脚本按同一口径对齐。
- 读取子进程输出时显式 `encoding="utf-8", errors="replace"`，不要用 `text=True` 让 subprocess 去猜 locale。`git show <rev>:<path>` 回传的是 blob 原始字节（UTF-8），按本地代码页解码会在读取线程里抛 `UnicodeDecodeError`，留下 `stdout=None` 让门禁崩在别的行上。

判定口径：同一份代码在本地 GBK 控制台与 CI cp1252 runner 上必须给出同样的结论和同样可读的输出；编码差异不得改变任何门禁的通过/失败。`scripts/tests/test_gate_behaviors.py` 用 `PYTHONIOENCODING=cp1252` 复现旧控制台来锁定这条性质。

### 覆盖率基线的唯一平台口径

覆盖率的阈值基线只能有唯一平台口径。同一份代码在不同编译器/平台/工具版本下的行覆盖数字并不可比；棘轮基线（`scripts/coverage_ratchet.py`，默认不接入 CI）记录的数字必须注明取数平台，且只允许该平台更新它。混入第二个平台的数字会让「只升不降」失去意义——这是教训记录，不配脚本。

## 仓库维护标准

当仓库预期接受外部或 AI 辅助的贡献时，模板应包含轻量的 PR 和 issue 模板。
GitHub Actions 依赖应由 Dependabot 或等效的书面更新流程覆盖。

## 文档标准

保持本地文档简短：

- `README.md` 提供概览、快速开始与完整文档索引（唯一事实源；docs/ 下
  不再设独立的索引文件）。
- `CONTRIBUTING.md` 说明工作流与 PR 预期。
- `SECURITY.md` 说明漏洞处理。
- `docs/guides/environment.md` 说明工具安装与环境配置。
- `docs/guides/cmake.md` 说明 CMake 工作流、preset、包消费与故障排除。
- `docs/guides/coding-style.md` 说明命名、头文件、格式与 API 文档约定。
- `docs/guides/testing.md` 说明测试分层与预期。
- `docs/guides/release.md` 说明发布步骤。
- `docs/guides/template.md` 说明重命名与裁剪。
- `docs/adr/README.md` 说明架构决策。
- `AGENTS.md` 是 AI 代理说明。

文档只维护一种语言：中文。上游派生项目如需要英文文档，请自行翻译，不要在
本仓库重新引入双语镜像。

## 默认避免

除非项目已经需要，否则不要过早引入：

- 插件注册表。
- 描述符清单。
- 多阶段命令分发系统。
- 全局服务定位器对象。
- 生成代码。
- 大型 vendored 依赖。

使用能保持所有权清晰的最小设计。
