# 编码规范

本指南描述本模板使用的 C 编码约定。它刻意保持简短：编译器标志与 `.clang-format`/`.clang-tidy` 文件是可强制执行的基线，本文档记录工具无法检查的选型背后的意图。

## 语言

- C11，以 CMake 目标特性 `c_std_11` 表达。
- 保持公共代码在 MSVC、GCC、Clang 和 AppleClang 之间可移植。将编译器特定构造隔离在 `include/<project>/compiler.h` 的宏之后，而不是到处散布 `#ifdef` 块。

## 命名

| 对象 | 约定 | 示例 |
| --- | --- | --- |
| 文件与目录 | snake_case | `example.c`、`compiler.h` |
| 函数与变量 | snake_case，前缀为项目命名空间 | `tokenprobe_add_checked` |
| 类型 | snake_case + `_t` 后缀 | `tokenprobe_version_t` |
| 宏与常量 | UPPER_SNAKE_CASE | `TOKENPROBE_API`、`TOKENPROBE_VERSION_MAJOR` |
| Include guards | `<PROJECT>_<FILE>_H` | `TOKENPROBE_EXAMPLE_H` |

函数式宏使用 UPPER_SNAKE_CASE 书写，函数使用 snake_case。这一区别十分关键：`scripts/check_decl_def.py` 依靠它来区分宏（如 `TOKENPROBE_DEPRECATED(...)`）与已声明的公共函数。

## 头文件组织

- 公共的跨模块接口位于 `include/<project>/` 下。
- 为每个公共头文件提供名为 `<PROJECT>_<FILE>_H` 的 include guard。
- 用 `extern "C"` guard 保持公共头文件可供 C++ 调用方使用。
- 公共头文件只包含它需要的内容。
- 实现私有头文件位于 `src/` 下与其源文件相邻的位置，绝不泄漏到 `include/`。

## 格式与静态分析

- `clang-format`（LLVM 基础风格、4 空格缩进、100 列）是格式化的权威。检查脚本运行 `clang-format --dry-run --Werror`。
- `clang-tidy` 在 CI 中以警告即错误运行。本地运行用 `./scripts/check.ps1 -EnableTidy`（Windows）或 `./scripts/check.sh --enable-tidy`（Linux/macOS）。
- `tests/vendor/` 下的第三方 vendored 代码不参与 `clang-format`、`clang-tidy` 和 `cppcheck` 检查。不要手工格式化或手工修复 vendored 代码。

## 注释与 API 文档

- 注释与 API 文档统一使用中文，便于国内团队维护；标识符、Doxygen 标签、命令和代码片段保持原样。
- 注释只写代码无法直接表达的内容，例如原因、约束、边界条件和失败后果，避免逐行复述代码。
- 公共头文件用 Doxygen `/** ... */` 注释记录模块、类型与函数契约。
- 每个公共函数都有一行 `@brief`。非 void 函数有 `@return`，描述每个返回值。`@param` 条目记录 NULL 约束、有效范围、借用指针与拥有指针，以及输出参数的失败状态。
- 结构体与枚举字段仅在含义、单位、范围或所有权从名称看不明显时，才加行尾 `/**< ... */` 注释。
- 实现文件不重复公共头文件的契约注释。局部注释解释为什么，而不是做了什么。

## 所有权与错误处理

- 明确所有权：负责分配的函数负责释放，除非所有权被转移并在头文件中记录。
- 优先返回带输出参数的错误/状态码来传递结果，如 `tokenprobe_add_checked` 所示，而不是使用隐藏的全局状态。

## 文件编码

源文件为 UTF-8 编码，使用 LF 行尾（由 `.gitattributes` 强制执行）。
