#!/usr/bin/env python3
"""检查项目版本在四个事实来源之间是否一致。

比较来源：

1. ``CMakeLists.txt`` —— ``project(... VERSION x.y.z ...)`` 声明。
2. 生成的版本头文件 —— CMake ``configure_file`` 输出到构建树的
   ``<UPPER>_VERSION_STRING "x.y.z"``。
3. 构建出的 CLI —— ``<exe> --version`` 输出。
4. ``CHANGELOG.md`` —— 最新的 ``## x.y.z`` 段落标题。

有意不扫描 ``docs/`` 下的文档：指南中包含 ``project(... VERSION ...)``
示例片段，会造成误报。

用法
----
    python3 scripts/check_version_consistency.py <repo_root> \
        --build-dir <build_dir> --exe <path-to-cli-executable>

项目名从 ``CMakeLists.txt`` 解析，没有硬编码任何具体项目名或前缀，
因此 ``scripts/rename-project.*`` 之后门禁仍然有效。

退出码：0 = 四个来源一致；1 = 存在不一致或解析失败。
"""

import argparse
import re
import subprocess
import sys
from pathlib import Path


def _pin_utf8_console() -> None:
    """把 stdout/stderr 固定为 UTF-8，保证中文判定可读。

    旧式控制台代码页（Windows GBK/cp1252）会让 stdout 抛出
    UnicodeEncodeError，或让 stderr 退化为转义序列，使判定变成噪声。
    向导 CLI 也采用同样的做法（``SetConsoleOutputCP(CP_UTF8)``）。
    """
    for stream in (sys.stdout, sys.stderr):
        if hasattr(stream, "reconfigure"):
            stream.reconfigure(encoding="utf-8", errors="replace")


_pin_utf8_console()


_VERSION_RE = re.compile(r"\b(\d+)\.(\d+)\.(\d+)\b")


def _fail(message: str) -> None:
    print(f"FAIL: {message}", file=sys.stderr)


def read_cmake_version(cmakelists: Path) -> tuple[str, str] | None:
    """从 project() 命令返回 (project_name, version)，解析失败返回 None。"""
    text = cmakelists.read_text(encoding="utf-8", errors="replace")
    match = re.search(
        r"\bproject\s*\(\s*([A-Za-z_][A-Za-z0-9_]*)\s+VERSION\s+(\d+\.\d+\.\d+)",
        text,
        re.IGNORECASE,
    )
    if match:
        return match.group(1), match.group(2)
    # project(name ...) 不带内联 VERSION、另起一行写 VERSION 的情况很少，
    # 回退到宽松的两步解析。
    name_match = re.search(r"\bproject\s*\(\s*([A-Za-z_][A-Za-z0-9_]*)", text)
    ver_match = re.search(r"\bVERSION\s+(\d+\.\d+\.\d+)", text)
    if name_match and ver_match:
        return name_match.group(1), ver_match.group(1)
    return None


def find_generated_header(build_dir: Path, project_name: str) -> Path | None:
    """在 build_dir 下定位生成的版本头文件。

    宏前缀来自项目的小写命名空间（可能与 PascalCase 项目名不同），
    因此按形态识别头文件：定义了 ``<UPPER>_VERSION_STRING`` 的 ``version.h``。
    没有硬编码项目相关内容。
    """
    for candidate in sorted(build_dir.rglob("version.h")):
        text = candidate.read_text(encoding="utf-8", errors="replace")
        if re.search(r"\b[A-Z][A-Z0-9_]*_VERSION_STRING\s+\"", text):
            return candidate
    return None


def read_header_version(header: Path, project_name: str) -> str | None:
    text = header.read_text(encoding="utf-8", errors="replace")
    match = re.search(r"\b[A-Z][A-Z0-9_]*_VERSION_STRING\s+\"([^\"]+)\"", text)
    return match.group(1) if match else None


def read_cli_version(exe: Path) -> str | None:
    try:
        result = subprocess.run(
            [str(exe), "--version"],
            capture_output=True,
            encoding="utf-8",
            errors="replace",
            timeout=30,
        )
    except (OSError, subprocess.TimeoutExpired) as exc:
        _fail(f"无法运行 {exe} --version: {exc}")
        return None
    output = result.stdout + result.stderr
    match = _VERSION_RE.search(output)
    return match.group(0) if match else None


def read_changelog_version(changelog: Path) -> str | None:
    text = changelog.read_text(encoding="utf-8", errors="replace")
    match = re.search(r"^##\s+(\d+\.\d+\.\d+)", text, re.MULTILINE)
    return match.group(1) if match else None


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(
        description="Check that the project version agrees across its four sources of truth.")
    parser.add_argument("repo_root", type=Path)
    parser.add_argument("--build-dir", type=Path, required=True,
                        help="CMake build tree containing the generated header")
    parser.add_argument("--exe", type=Path, required=True,
                        help="built CLI executable to query with --version")
    args = parser.parse_args(argv[1:])

    root: Path = args.repo_root
    cmakelists = root / "CMakeLists.txt"
    changelog = root / "CHANGELOG.md"

    problems: list[str] = []

    if not cmakelists.is_file():
        _fail(f"缺少 {cmakelists}")
        return 1
    parsed = read_cmake_version(cmakelists)
    if parsed is None:
        _fail(f"{cmakelists}: 无法解析 project(... VERSION x.y.z)")
        return 1
    project_name, cmake_version = parsed

    if not args.build_dir.is_dir():
        _fail(f"构建目录不存在: {args.build_dir}")
        return 1
    header = find_generated_header(args.build_dir, project_name)
    header_version = None
    if header is None:
        problems.append(f"{args.build_dir}: 未找到生成的版本头文件（含 <UPPER>_VERSION_STRING 的 version.h）")
    else:
        header_version = read_header_version(header, project_name)
        if header_version is None:
            problems.append(f"{header}: 未找到 <UPPER>_VERSION_STRING")

    if not args.exe.is_file():
        problems.append(f"可执行文件不存在: {args.exe}")
        cli_version = None
    else:
        cli_version = read_cli_version(args.exe)
        if cli_version is None:
            problems.append(f"{args.exe} --version: 输出中没有 x.y.z 版本号")

    if not changelog.is_file():
        problems.append(f"缺少 {changelog}")
        changelog_version = None
    else:
        changelog_version = read_changelog_version(changelog)
        if changelog_version is None:
            problems.append(f"{changelog}: 未找到 '## x.y.z' 段落")

    sources = {
        "CMakeLists.txt project VERSION": cmake_version,
        f"generated header ({header.name if header else '?'}): "
        "<UPPER>_VERSION_STRING": header_version,
        f"{args.exe.name} --version": cli_version,
        "CHANGELOG.md newest section": changelog_version,
    }

    for problem in problems:
        _fail(problem)

    known = {label: value for label, value in sources.items() if value}
    distinct = sorted(set(known.values()))
    if len(distinct) != 1 or problems:
        print("版本不一致：", file=sys.stderr)
        for label, value in sources.items():
            print(f"  {label}: {value if value else '<缺失>'}", file=sys.stderr)
        return 1

    print(f"OK: version {distinct[0]} consistent across "
          f"CMakeLists.txt, generated header, CLI --version, and CHANGELOG.md")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
