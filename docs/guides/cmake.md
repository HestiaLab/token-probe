# CMake 指南

本指南介绍此模板中的常用 CMake 工作流。当你是 CMake 新手，或需要添加源文件、测试、安装规则或下游消费者时，可使用本指南。

## 前置要求

以下工具不由仓库提供，请在配置项目之前安装。

- CMake 3.21 或更高版本。
- C11 编译器。
- 以下受支持的编译器家族之一：
  - Windows 上的 MSVC。
  - Linux 上的 GCC 或 Clang。
  - macOS 上的 AppleClang 或 Clang。
- 默认 preset 使用 Ninja；或通过本地 preset 或命令行 generator 选择来使用 CMake 支持的其他 generator。

受支持的 Windows 开发环境（VS Code + MSYS2 UCRT64 + GCC）请参见 [environment.md](environment.md)。

项目通过 `c_std_11` 在目标级别要求 C11。请保持公共头文件在受支持的编译器家族之间可移植，并避免使用编译器扩展，除非它们被隔离在小型 CMake/编译器检查之后。

## 工具安装说明

典型的环境组合：

| 平台 | 编译器 | Generator |
| --- | --- | --- |
| Windows（MSVC）| 来自 Visual Studio Build Tools 或 Visual Studio 的 MSVC | Ninja |
| Windows（MSYS2）| 来自 MSYS2 UCRT64 的 Clang 或 GCC | Ninja |
| Linux | 来自系统包管理器的 GCC 或 Clang | Ninja |
| macOS | 来自 Xcode Command Line Tools 的 AppleClang 或 Clang | Ninja |

在 Windows 上，请在 Developer PowerShell、Developer Command Prompt 或初始化了 MSVC 的 VS Code CMake kit 中使用 Ninja preset。普通 shell 可能找得到 CMake，但在选择 Ninja 时仍然找不到 `cl.exe`。

VS Code 是可选的。如果使用它，请安装 CMake Tools 和 clangd。默认工作区设置使用 `ninja-debug` preset，`.clangd` 将 clangd 指向 `build/ninja-debug/compile_commands.json`。Microsoft C/C++ 与 CMake Tools 搭配时仍可作为本地后备方案，但不要为同一工作区启用多个 C/C++ 语言服务，除非你有意管理它们之间的重叠。

此模板提交了可移植的编辑器默认配置：

- `.vscode/extensions.json` 推荐使用 CMake Tools 和 clangd。
- `.vscode/settings.json` 让 VS Code 使用 `ninja-debug` CMake preset，并禁用 Microsoft C/C++ IntelliSense，使 clangd 成为生效的语言服务。
- `.vscode/c_cpp_properties.example.json` 提供可选的 Microsoft C/C++ 后备 include 路径，不硬编码编译器安装路径。
- `.vscode/tasks.json` 提供直接调用 CMake 和 CTest 的 configure、build、test 任务。
- `.clangd` 将 clangd 指向 Ninja 编译数据库，并提供后备项目 include 路径。

请保持这些文件通用。不要提交本地 Visual Studio、MSYS2、Homebrew 或系统 SDK 安装路径等绝对编译器路径。将这些路径放在 VS Code 用户设置、CMake kit、`CMakeUserPresets.json`、shell 环境配置或本地 toolchain 文件中。

## 心智模型

CMake 工作通常分为四个独立步骤：

1. Configure：读取 `CMakeLists.txt`、选择选项并生成构建文件。
2. Build：使用生成的构建文件编译源文件并链接目标。
3. Test：从已配置的构建树运行 CTest 测试。
4. Install：将公共制品复制到安装前缀，供下游使用。

这些步骤被有意地分开。如果 configure 失败，构建文件就不可靠；如果 build 失败，测试和安装就没有意义。

## 快速命令

对于新检出的代码，先运行 bootstrap。它会检查所需的命令行工具并执行 configure，以便 clangd 能读取编译数据库：

**Windows PowerShell**

```powershell
./scripts/bootstrap.ps1
```

**Linux/macOS**

```sh
./scripts/bootstrap.sh
```

日常开发使用本地 check 封装脚本：

**Windows PowerShell**

```powershell
./scripts/check.ps1
```

**Linux/macOS**

```sh
./scripts/check.sh
```

当你需要逐步执行时，仍可直接使用 CMake 命令：

```sh
cmake --preset ninja-debug
cmake --build --preset ninja-debug
ctest --preset ninja-debug --output-on-failure
```

## Presets

Preset 是存储在 `CMakePresets.json` 中的命名构建配置。

| Preset | 用途 |
| --- | --- |
| `ninja-debug` | 带调试符号和 `compile_commands.json` 的默认跨平台开发构建。 |
| `ninja-release` | 使用 Ninja 的优化 release 风格验证与安装冒烟测试。 |
| `ninja-shared` | 带 `BUILD_SHARED_LIBS=ON` 的 `ninja-debug` 构建。 |
| `ninja-asan` | 在受支持平台上带 AddressSanitizer 和 UBSan 的 Ninja debug 构建。 |
| `ninja-coverage` | 在受支持平台上带覆盖率标志的 Ninja debug 构建。 |
| `debug` | 带调试符号的快速本地开发构建。 |
| `release` | 用于 release 风格验证与安装冒烟测试的优化构建。 |
| `shared` | 带 `BUILD_SHARED_LIBS=ON` 的 `debug` 构建。 |
| `asan` | 在受支持平台上带 AddressSanitizer 和 UBSan 的 debug 构建。 |
| `coverage` | 在受支持平台上带覆盖率标志的 debug 构建。 |

configure、build 和 test preset 共用相同名称：

```sh
cmake --preset ninja-shared
cmake --build --preset ninja-shared
ctest --preset ninja-shared --output-on-failure
```

`ninja-*` preset 是编辑器与 CI 的默认路径。它们强制使用 Ninja generator，因此 CMake 会将 `compile_commands.json` 写入 preset 构建目录。请在使用这些默认配置的每个开发平台上保持安装 Ninja。

`debug`、`release`、`shared`、`asan` 和 `coverage` preset 仍是与 generator 无关的兼容性条目。当你需要使用 Visual Studio、Xcode、Make 或其他 generator 时，可以通过 `CMakeUserPresets.json` 文件、CMake kit 或显式的命令行 generator 选择来使用它们：

```sh
cmake --preset debug -G "<your generator>"
```

要创建本地 preset，请将 `CMakeUserPresets.example.json` 复制为 `CMakeUserPresets.json`，并根据你的机器修改 generator 名称。真正的 `CMakeUserPresets.json` 文件被 Git 忽略。对于 Visual Studio 工作流，添加一个继承自 `debug` 并设置你已安装 generator 的本地 preset，例如 `Visual Studio 17 2022` 或更新的本地版本。

Visual Studio 和 Xcode 构建仍然受支持，但它们是 multi-config generator。请使用 CMake Tools 作为 IDE IntelliSense 的配置提供者，或在语言服务器无法直接读取 generator 元数据时为生成头文件添加编辑器本地 include 路径。

所有项目 preset 都继承一个启用 `CMAKE_EXPORT_COMPILE_COMMANDS` 的基础 preset。支持编译数据库的 generator 会将 `compile_commands.json` 写入 preset 构建目录。

## 重要选项

以下选项可在 configure 阶段传入：

```sh
cmake --preset ninja-debug -DTOKENPROBE_BUILD_CLI=OFF
```

| 选项 | 含义 |
| --- | --- |
| `BUILD_SHARED_LIBS` | 在受支持时将库构建为共享库。 |
| `TOKENPROBE_BUILD_CLI` | 将 `src/main.c` 构建为 `tokenprobe` 命令行工具。 |
| `TOKENPROBE_HTTP_BACKEND` | HTTP 传输后端：`auto`（Windows 取 `winhttp`，其余取 `openssl`）、`winhttp`、`openssl` 或 `mock`（不编译平台传输，仅离线传输）。 |
| `TOKENPROBE_WARNINGS_AS_ERRORS` | 把编译警告当作错误。 |
| `TOKENPROBE_BUILD_TESTING` | 构建 CTest 测试目标。 |
| `TOKENPROBE_INSTALL` | 生成安装规则。 |
| `TOKENPROBE_ENABLE_ASAN` | 在受支持平台启用 AddressSanitizer。 |
| `TOKENPROBE_ENABLE_UBSAN` | 在受支持平台启用 UndefinedBehaviorSanitizer。 |
| `TOKENPROBE_ENABLE_COVERAGE` | 在受支持平台启用覆盖率标志。 |

对于顶层构建，CLI、测试和安装规则默认为开启。当本项目通过 `add_subdirectory()` 被包含时，它们默认为关闭。

`TOKENPROBE_HTTP_BACKEND=openssl` 需要 OpenSSL 1.1.1 及以上；该依赖只以 PRIVATE 方式链接，不会出现在已安装目标的 INTERFACE 链接库中，所以消费者不会继承它。

如需检查可用的缓存选项，可在 configure 之后运行 `cmake -LAH -S . -B build/ninja-debug`。

## 多配置 Generator

Visual Studio 和 Xcode 是 multi-config generator。它们可以从同一构建目录生成 Debug 和 Release 制品，因此某些工作流中的 build 和 test 命令需要显式指定配置：

```sh
cmake --build build/package-smoke --config Release
ctest --test-dir build/package-smoke -C Release --output-on-failure
```

Ninja 和 Make 等 single-config generator 则在 configure 阶段使用 `CMAKE_BUILD_TYPE`。

## 工具链策略

构建定义是语言模式与 include 目录的唯一事实来源：

- CMake 最低版本：3.21。
- C 语言标准：C11，以目标特性 `c_std_11` 表示。
- 受支持的编译器家族：MSVC、GCC、Clang 和 AppleClang。
- 默认 generator：通过 `ninja-*` preset 使用 Ninja。
- 受支持的兼容 generator：任何能与所选编译器配合的 CMake generator，包括通过 generator-neutral preset 或用户 preset 使用 Visual Studio、Xcode、Ninja 和 Make。

仓库声明了环境要求和推荐的编辑器扩展，但不会替用户安装编译器、SDK、CMake、Ninja、VS Code 扩展或 clangd。当项目需要更严格的环境搭建指导时，请将安装命令保留在贡献者文档或平台入门说明中。

不要在已提交的 preset 中放入特定于机器的编译器路径、SDK 路径或 Visual Studio 安装路径。请使用用户 preset、CMake kit、toolchain 文件或环境配置脚本来选择本地工具链。

请将生成头文件保留在构建树下，并通过目标 include 目录暴露它们。编辑器应从 CMake 元数据或本地工作区配置中获取这些路径；源文件不应通过相对文件系统路径包含生成头文件。

## 添加源文件

将实现文件放在 `src/tokenprobe/` 下，公共头文件放在 `include/tokenprobe/` 下。

1. 在 `src/tokenprobe/` 下创建 `.c` 文件。
2. 在 `include/tokenprobe/` 下创建或更新对应的公共头文件。
3. 将新的 `.c` 文件添加到 `cmake/Sources.cmake` 的 `TOKENPROBE_CORE_SOURCES` 中。
4. 在 `tests/` 下添加测试。
5. 运行 `./scripts/check.ps1` 或 `./scripts/check.sh`。

公共头文件应只包含其所需的内容，并在暴露 C API 时保持可供 C++ 调用方使用。

## 添加测试

使用 `cmake/Tests.cmake` 中的 `tokenprobe_add_test()`：

```cmake
tokenprobe_add_test(tokenprobe_feature_tests
    SOURCES
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_feature.c
    LIBS
        tokenprobe_core
)
```

然后运行：

```sh
cmake --preset ninja-debug
cmake --build --preset ninja-debug
ctest --preset ninja-debug --output-on-failure
```

测试应断言公共行为。请保持实现细节测试的稀缺性，并记录它们所保护的内部契约。

## 安装并通过 find_package 使用

配置并构建一个 release 包：

```sh
cmake --preset ninja-release
cmake --build --preset ninja-release
cmake --install build/ninja-release --config Release --prefix install
```

下游 CMake 项目随后即可消费它：

```cmake
find_package(TokenProbe CONFIG REQUIRED)
target_link_libraries(app PRIVATE TokenProbe::tokenprobe_core)
```

包冒烟测试项目验证这条路径：

**Linux/macOS**

```sh
cmake -G Ninja -S tests/package_smoke -B build/package-smoke -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH="$PWD/install"
cmake --build build/package-smoke --config Release
ctest --test-dir build/package-smoke -C Release --output-on-failure
```

**Windows PowerShell**

```powershell
cmake -G Ninja -S tests/package_smoke -B build/package-smoke -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH="$PWD/install"
cmake --build build/package-smoke --config Release
ctest --test-dir build/package-smoke -C Release --output-on-failure
```

对于共享库安装，请使用 `ninja-shared` preset 和单独的安装前缀：

**Linux/macOS**

```sh
cmake --preset ninja-shared
cmake --build --preset ninja-shared
cmake --install build/ninja-shared --config Debug --prefix install-shared
cmake -G Ninja -S tests/package_smoke -B build/package-smoke-shared -DCMAKE_BUILD_TYPE=Debug -DCMAKE_PREFIX_PATH="$PWD/install-shared"
cmake --build build/package-smoke-shared --config Debug
ctest --test-dir build/package-smoke-shared -C Debug --output-on-failure
```

**Windows PowerShell**

```powershell
cmake --preset ninja-shared
cmake --build --preset ninja-shared
cmake --install build/ninja-shared --config Debug --prefix install-shared
cmake -G Ninja -S tests/package_smoke -B build/package-smoke-shared -DCMAKE_BUILD_TYPE=Debug -DCMAKE_PREFIX_PATH="$PWD/install-shared"
cmake --build build/package-smoke-shared --config Debug
ctest --test-dir build/package-smoke-shared -C Debug --output-on-failure
```

在 Windows 上，共享库消费者需要将已安装的 DLL 放在测试可执行文件旁边或加入 `PATH`。当 CMake 通过已安装目标的元数据暴露运行时 DLL 时，冒烟项目会复制该 DLL。

## 作为子项目使用

项目也可以直接消费本仓库：

```cmake
add_subdirectory(path/to/c-project-standard)
target_link_libraries(app PRIVATE TokenProbe::tokenprobe_core)
```

子项目冒烟测试验证这种模式：

**Linux/macOS**

```sh
cmake -G Ninja -S tests/subproject_smoke -B build/subproject-smoke -DTOKENPROBE_SOURCE_DIR="$PWD"
cmake --build build/subproject-smoke --config Debug
ctest --test-dir build/subproject-smoke -C Debug --output-on-failure
```

**Windows PowerShell**

```powershell
cmake -G Ninja -S tests/subproject_smoke -B build/subproject-smoke -DTOKENPROBE_SOURCE_DIR="$PWD"
cmake --build build/subproject-smoke --config Debug
ctest --test-dir build/subproject-smoke -C Debug --output-on-failure
```

作为子项目被包含时，示例、测试和安装规则默认为关闭；下游项目如有需要可以显式开启它们。

## 生成的版本头文件

项目版本只声明一次：

```cmake
project(TokenProbe VERSION 0.2.0 LANGUAGES C)
```

CMake 根据该值生成 `tokenprobe/version.h`。源文件应使用生成的宏，而不是重复字面版本号。

## 本地质量检查

在克隆之后、安装工具之后或切换到新机器之后运行 bootstrap：

**Windows PowerShell**

```powershell
./scripts/bootstrap.ps1
```

**Linux/macOS**

```sh
./scripts/bootstrap.sh
```

check 脚本会执行 configure、build 和测试，并在工具可用时运行可选工具：

**Windows PowerShell**

```powershell
./scripts/check.ps1 -EnableTidy
```

**Linux/macOS**

```sh
./scripts/check.sh --enable-tidy
```

如果本地没有安装 `clang-format`、`clang-tidy` 或 `yamllint`，请将 CI 作为这些工具的权威检查。

## 故障排除

旧的 CMake 缓存指向其他路径：

- 症状：configure 提示了不同的源目录。
- 解决方法：删除受影响的构建目录（例如 `build/ninja-release`），并重新运行 `cmake --preset ninja-release`。

缺少 Ninja 或 C 编译器：

- 症状：configure 提示找不到 Ninja 构建程序，或未设置 `CMAKE_C_COMPILER`。
- 解决方法：运行 `./scripts/bootstrap.ps1` 或 `./scripts/bootstrap.sh` 以获得更清晰的诊断信息。在 Windows 上，配置 Ninja preset 之前请打开 Developer PowerShell、Developer Command Prompt，或在 VS Code 中选择 MSVC CMake kit。

VS Code 使用 Ninja 构建 `ALL_BUILD`：

- 症状：CMake Tools 运行 `cmake --build ... --target ALL_BUILD` 时，Ninja 报告 `unknown target 'ALL_BUILD'`。
- 解决方法：运行 `CMake: Set Build Target` 并选择 `all`。如果旧目标仍被缓存，请依次运行 `CMake: Reset CMake Tools Extension State` 和 `CMake: Delete Cache and Reconfigure`。

`find_package(TokenProbe)` 找不到包：

- 症状：CMake 找不到 `TokenProbeConfig.cmake`。
- 解决方法：先安装项目，并通过 `-DCMAKE_PREFIX_PATH=/path/to/install` 传入绝对安装前缀。

Windows 共享库测试找不到 DLL：

- 症状：已安装的共享库消费者能构建，但运行时失败。
- 解决方法：将已安装的 DLL 复制到可执行文件旁边，或将安装的 `bin` 目录添加到 `PATH`。

`ctest` 在使用 Visual Studio 时找不到测试可执行文件：

- 症状：CTest 在错误的配置目录中查找。
- 解决方法：传入与构建配置匹配的 `-C Debug` 或 `-C Release`。

缺少 `clang-format`、`clang-tidy` 或 `yamllint`：

- 症状：本地脚本跳过了某个工具，或 shell 找不到它。
- 解决方法：在本地安装该工具，或依赖工作流会安装所需工具链的 CI。
