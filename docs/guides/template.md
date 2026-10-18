# 本项目的来源与裁剪

本仓库由 [C_PROJECT_STANDARD.md](../../C_PROJECT_STANDARD.md) 定义的
C 工程标准生成，随后按 TokenProbe 的实际形态裁剪。本页记录两件事：
生成时做了什么，以及裁剪掉了什么、为什么。

## 生成

用标准仓库自带的向导生成骨架：

```sh
cproject new TokenProbe --output <父目录> --yes
```

向导完成三件事：复制模板、把 `cproject` / `CPROJECT` / `CProjectStandard`
等标识符替换为 `tokenprobe` / `TOKENPROBE` / `TokenProbe`、重命名
`include/` 与 `src/` 下的命名空间目录与 CMake 配置文件。

版本元数据只有一个事实源：`CMakeLists.txt` 的 `project(... VERSION ...)`。
它由 `cmake/tokenprobe_version.h.in` 生成 `tokenprobe/version.h`，
运行时代码只读这个生成头文件，不在源文件里复制版本字面量。
`tokenprobe_version_consistency` 门禁在 CTest 中强制这一点。

## 裁剪清单

标准允许按其"裁剪可选内容"一节删除与本项目无关的部分。本项目删除了：

| 删除项 | 原因 |
| --- | --- |
| WiX 安装器目录与 PackagingMSI.cmake | TokenProbe 是"库 + 命令行工具"，分发包用 ZIP/TGZ 更合适。见 [release.md](release.md)。 |
| 向导资源（图标、`run-wizard.bat`、模板源码安装规则） | 本项目不是项目生成器，不安装自己的源码树。 |
| 标准附带的 scalar.h 别名头文件及其测试 | 标准提供的示例类型别名，本项目公共 API 直接用 `<stdint.h>`，保留会变成死代码。 |
| check-cpack-config.py 门禁及其自测 | 该校验针对 NSIS 安装器的向导快捷方式，安装器已移除，校验对象不存在。 |
| Windows 版 `AGENTS.md` | 标准中该项为可选项，生成时未启用。 |

保留但改名的：`TOKENPROBE_BUILD_EXAMPLE` 改为 `TOKENPROBE_BUILD_CLI`，
因为该目标构建的是真实的命令行工具，不是示例程序。

## 保留的部分

以下部分原样保留，改动它们前先确认必要性：

- `cmake/CompilerOptions.cmake` 的警告与 sanitizer 开关，以及
  `cmake/Tests.cmake` 的 Unity 注册辅助函数。
- `.clang-format`、`.clang-tidy`、`.editorconfig`、`.gitattributes`。
- `docs/guides/` 下的其余指南与 `docs/adr/`。
- `tests/package_smoke/` 与 `tests/subproject_smoke/` 两条下游消费路径。
  对"可被别的项目集成"这一需求来说，它们是必须保留的验证。
- `scripts/check_*.py` 门禁与其自测。门禁脚本的价值在于"能失败"，
  不要为了让它们变绿而削弱断言。

## 重命名

如果需要把本项目改名，`scripts/rename-project.ps1` 与
`scripts/rename-project.sh` 仍然可用；它们会替换 `TokenProbe` /
`tokenprobe` / `TOKENPROBE` 三种写法并相应重命名目录与 CMake 配置文件。
改名后必须跑一遍 `scripts/bootstrap.ps1` 或 `scripts/bootstrap.sh`
以及 `scripts/check.ps1` 或 `scripts/check.sh`。
