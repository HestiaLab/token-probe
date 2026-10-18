# 发布

本项目用 CPack 在三个平台打包。模板原本为 Windows 单独提供 WiX/MSI，
这里已移除：TokenProbe 是"库 + 命令行工具"，ZIP 比安装器更合适。
如果将来确实需要签名的 MSI，再把它加回来。

## 打包

| 平台 | CPack 生成器 | 制品 |
| -------- | --------------------- | -------------------------- |
| Linux    | TGZ，可用时加 DEB/RPM | `.tar.gz`, `.deb`, `.rpm`   |
| macOS    | TGZ                   | `.tar.gz`                  |
| Windows  | ZIP                   | `.zip`                     |

本地生成：

```sh
cmake --preset ninja-release
cmake --build --preset ninja-release
cpack --config build/ninja-release/CPackConfig.cmake
```

DEB/RPM 只在 `dpkg` / `rpmbuild` 存在时启用；TGZ/ZIP 总是生成。

## 便携 zip（package.ps1）

要在 Windows 上快速得到一个完整安装树的 zip：

```powershell
cmake --preset ninja-release
cmake --build --preset ninja-release
./scripts/package.ps1
```

脚本把构建安装到暂存目录，对**安装后的** `tokenprobe --version` 做冒烟检查
（必须与构建产物报同一个版本），然后打包整个安装树并写出 `SHA256SUMS.txt`。
产物是 `dist/tokenprobe-win64-<version>.zip`。

## 源码包

```sh
cmake --preset ninja-debug
cpack --config build/ninja-debug/CPackSourceConfig.cmake
```

## 包内容

```text
bin/                     tokenprobe 命令行工具
include/tokenprobe/      公共头文件 + 生成的 version.h
lib/                     静态或动态库
lib/cmake/TokenProbe/    find_package 配置与导出目标
```

## 发布工作流

`.github/workflows/release.yml` 在 tag 上构建并上传制品。发布前确认：

1. `CMakeLists.txt` 的 `project(... VERSION ...)` 已更新。
2. `CHANGELOG.md` 最新段落与该版本一致（`tokenprobe_version_consistency`
   门禁会在 CTest 中强制这一点）。
3. 本地跑通 `./scripts/check.ps1` 或 `./scripts/check.sh`，
   再跑一次 `ninja-shared` 与包冒烟测试。
4. 打 tag，推送。

## 动态库注意事项

`BUILD_SHARED_LIBS=ON` 时，公共入口点通过 `tokenprobe/export.h` 的
`TOKENPROBE_API` 导出。改动导出宏或符号可见性后，必须跑
`ninja-shared` 预设和 `tests/package_smoke/` 的共享库冒烟测试。
