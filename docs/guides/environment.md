# 环境配置

本项目在 Windows 10/11（64 位）上支持唯一一套开发环境：

> **VS Code + MSYS2 UCRT64（GCC、GDB、Ninja、clangd）+ 原生 CMake + Git**

所有构建工具统一由 MSYS2 UCRT64 提供，GCC、GDB、Ninja、clangd 只有一个来源。CMake 使用 Windows 原生版本，Git 使用 Git for Windows。

## 需求

| 工具 | 必需 | 用途 |
| --- | --- | --- |
| CMake 3.21+（原生 Windows） | 是 | 配置并生成构建文件 |
| GCC（MSYS2 UCRT64） | 是 | 编译 C11 源码 |
| Ninja | 是（默认预设） | `ninja-*` 预设的构建生成器 |
| GDB | 是（调试） | 断点、单步、查看变量 |
| clangd | 否 | 编辑器代码导航与诊断 |
| clang-format | 否 | 本地格式化检查 |
| clang-tidy | 否 | 可选的本地静态分析 |

## 安装步骤

按以下顺序安装。安装路径尽量不含空格和非 ASCII 字符。

### 1. Visual Studio Code

安装 [VS Code](https://code.visualstudio.com/)（64 位）。安装时勾选 **Add to PATH** 和 *Open with Code* 右键菜单项。

```powershell
code --version
```

### 2. Git

安装 [Git for Windows](https://git-scm.com/download/win)。在 *Adjusting your PATH environment* 一步选择 **Git from the command line and also from 3rd-party software**，这样 VS Code、CMake 和 MSYS2 shell 都能调用 `git`。

```powershell
git --version
```

### 3. MSYS2

安装 [MSYS2](https://www.msys2.org/) 到默认路径 `C:\msys64`。从开始菜单启动 **MSYS2 UCRT64** 并全量更新：

```sh
pacman -Syu
```

如果更新要求重启，就重新打开 UCRT64 shell，重复执行 `pacman -Syu` 直到没有待更新的软件。MSYS2 是滚动发行版：安装任何工具前必须先全量升级，不要用 `pacman -Sy` 单独拉一个包。

### 4. GCC、GDB、Ninja、clangd

在 UCRT64 shell 中安装正式工具链：

```sh
pacman -S --needed \
  mingw-w64-ucrt-x86_64-gcc \
  mingw-w64-ucrt-x86_64-gdb \
  mingw-w64-ucrt-x86_64-ninja \
  mingw-w64-ucrt-x86_64-clang-tools-extra
```

`clang-tools-extra` 提供 `clangd`、`clang-format`、`clang-tidy`（`/ucrt64/bin/clangd.exe`），无需单独下载 LLVM。

### 5. CMake（原生 Windows）

安装 **Windows 原生 CMake**（来自 [cmake.org](https://cmake.org/download/) 的 `.msi`），而不是 MSYS2 `/usr/bin` 下的 CMake。安装时选择 **Add CMake to the system PATH**。版本须为 3.21 或更新。

```powershell
cmake --version
```

### 6. Windows PATH

把 `C:\msys64\ucrt64\bin` 加入用户或系统 PATH。不要全局把 `C:\msys64\clang64\bin` 排在它前面；sanitizer 工具链不在支持范围内（见下）。

### 7. VS Code 扩展

安装以下扩展：

```text
ms-vscode.cmake-tools                  # CMake configure/build/debug
llvm-vs-code-extensions.vscode-clangd  # 补全、诊断、跳转到定义
ms-vscode.cpptools                     # GDB 调试适配器 (cppdbg)
```

## Sanitizer（ASan/UBSan）

Sanitizer **不在支持范围内**。UCRT64 的 GCC 发行版不带可链接的 `libasan`/`libubsan`，重装 UCRT64 GCC 也不会补齐。AddressSanitizer/UBSan 需要单独的 MSYS2 CLANG64 工具链（Clang + compiler-rt），因此 `ninja-asan` 预设仅靠 UCRT64 GCC 无法工作。请把 sanitizer 运行交给 Linux/macOS 上的 CI，或本地单独配置 CLANG64。

## 验证

在仓库根目录运行 bootstrap 检查：

```powershell
./scripts/bootstrap.ps1 -CheckOnly    # 仅检查工具
./scripts/bootstrap.ps1 -CheckEditor   # 额外检查 gdb
./scripts/bootstrap.ps1                # 工具 + 配置 ninja-debug
```

然后确认每个工具都解析到 UCRT64 安装，而不是机器上的重复安装：

```powershell
where gcc
where gdb
where ninja
where clangd
where cmake
where git
```

`gcc` 应解析到 `C:\msys64\ucrt64\bin\gcc.exe`，`gdb` 解析到 `C:\msys64\ucrt64\bin\gdb.exe`。如果 `where gcc` 先列出了另一个 MinGW，调整 PATH 顺序，让 `C:\msys64\ucrt64\bin` 排在前面。

## 下一步

- **构建和测试**：配置、构建、测试、预设和故障排除见 [cmake.md](cmake.md)。
- **测试预期**：测试分层和静态分析见 [testing.md](testing.md)。
- **贡献**：分支、提交和 PR 流程见 [../../CONTRIBUTING.md](../../CONTRIBUTING.md)。
- **项目标准**：完整的工程标准见 [../../C_PROJECT_STANDARD.md](../../C_PROJECT_STANDARD.md)。
