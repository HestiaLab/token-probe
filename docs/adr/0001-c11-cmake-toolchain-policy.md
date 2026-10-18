# 0001 C11 与 CMake 工具链策略

## 状态

已接受。

**日期**：2026-06-05
**最后修改**：2026-06-05

## 背景

本模板需要在 Windows、Linux 和 macOS 上都能工作，而不能把某一位开发者的
IDE、编译器安装路径或 generator 选择固化进共享的项目文件。编辑器和语言服务器
也需要一种可靠的方式，来发现与构建一致的 include 目录和语言模式。

## 决策

项目以 CMake 作为构建元数据的唯一事实源，以 C11 作为可移植的语言基线。库目标
通过 `target_compile_features(... c_std_11)` 声明这一要求。

受支持的编译器族为 MSVC、GCC、Clang 和 AppleClang。共享 preset 包含与
generator 无关的名称，因此开发者可以在各自平台上选择 Visual Studio、Xcode、
Ninja、Make 或其他 CMake 支持的 generator。

主编辑器与 CI 工作流使用显式的 `ninja-*` preset。这些 preset 在设计上依赖
generator，但仍是可移植的，因为它们不包含本地编译器路径、SDK 路径或 IDE kit
选择。它们为 clangd 和 clang-tidy 提供稳定的 `compile_commands.json` 位置。

机器特定的编译器路径、SDK 路径和 IDE kit 选择属于本地用户配置，不属于提交进
仓库的项目文件。

当可移植的编辑器模板文件不会绑定到某一位开发者的机器时，可以提交。推荐 VS Code
扩展、CMake preset 任务、通用 include 后备路径和 clangd 后备标志都是允许的。
绝对编译器路径和 SDK 安装路径属于本地配置。

## 后果

链接导出的 CMake 目标的消费者会继承 C11 要求。`ninja-*` preset 为语言服务器
提供 `compile_commands.json`。多配置 IDE generator 仍通过与 generator 无关的
preset 得到支持；当语言服务器无法直接消费 generator 元数据时，编辑器集成应使用
CMake 元数据或本地工作区配置。
