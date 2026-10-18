#!/usr/bin/env python3
"""分层门禁：公共头与 provider 驱动的依赖边界。

规则：

- ``include/`` 下的公共头只能包含标准 C 头（白名单）和本项目的
  ``tokenprobe/`` 头，不能把第三方依赖带进公共 ABI。
- ``src/tokenprobe/tp_provider_*.c`` 不能包含操作系统或传输层头；
  provider 只能通过 ``tp_internal.h`` 使用传输层。

扫描面为空时失败，避免“解析到 0 项却打印 OK”的假绿。
"""

import argparse
import re
import sys
from pathlib import Path

_PUBLIC_ALLOWED = {
    "stddef.h",
    "stdint.h",
    "stdbool.h",
    "stdarg.h",
    "limits.h",
}

_PROVIDER_ALLOWED = {
    "stdlib.h",
    "string.h",
    "stdint.h",
    "stdarg.h",
    "stddef.h",
    "stdio.h",
    "assert.h",
    "limits.h",
}

_INCLUDE_RE = re.compile(r"^\s*#\s*include\s*[<\"]([^>\"]+)[>\"]")


def _pin_utf8_console() -> None:
    """把 stdout/stderr 固定为 UTF-8，保证中文判定可读。"""
    for stream in (sys.stdout, sys.stderr):
        if hasattr(stream, "reconfigure"):
            stream.reconfigure(encoding="utf-8", errors="replace")


_pin_utf8_console()


def _includes(text: str):
    for lineno, line in enumerate(text.splitlines(), start=1):
        match = _INCLUDE_RE.match(line)
        if match:
            yield lineno, match.group(1)


def check_public_headers(root: Path, min_headers: int) -> list[str]:
    problems = []
    headers = sorted((root / "include").rglob("*.h"))
    if len(headers) < min_headers:
        problems.append(f"{root}: 公共头扫描面为空或不足（{len(headers)} < {min_headers}）")
    for header in headers:
        rel = header.relative_to(root).as_posix()
        for lineno, include in _includes(header.read_text(encoding="utf-8", errors="replace")):
            if include.startswith("tokenprobe/"):
                continue
            if include in _PUBLIC_ALLOWED:
                continue
            problems.append(f"{rel}:{lineno}: 公共头文件不得包含 {include}")
    return problems


def check_providers(root: Path, min_providers: int) -> list[str]:
    problems = []
    providers = sorted((root / "src" / "tokenprobe").glob("tp_provider_*.c"))
    if len(providers) < min_providers:
        problems.append(f"{root}: provider 扫描面为空或不足（{len(providers)} < {min_providers}）")
    for provider in providers:
        rel = provider.relative_to(root).as_posix()
        for lineno, include in _includes(provider.read_text(encoding="utf-8", errors="replace")):
            if include.startswith("tokenprobe/") or include == "tp_internal.h":
                continue
            if include in _PROVIDER_ALLOWED:
                continue
            problems.append(f"{rel}:{lineno}: provider 不得包含 {include}")
    return problems


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(
        description="Layering gate: public headers and provider dependencies.")
    parser.add_argument("repo_root", type=Path)
    parser.add_argument("--min-public-headers", type=int, default=2)
    parser.add_argument("--min-providers", type=int, default=1)
    args = parser.parse_args(argv[1:])

    root: Path = args.repo_root.resolve()
    problems = check_public_headers(root, args.min_public_headers)
    problems += check_providers(root, args.min_providers)

    if problems:
        for problem in problems:
            print(f"FAIL: {problem}", file=sys.stderr)
        return 1
    print("OK: 分层检查通过")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
