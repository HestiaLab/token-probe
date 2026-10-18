#!/usr/bin/env python3
"""检查 include/ 中的每个公共函数声明在 src/ 中都有定义。

目的
----
防止“声明了却从未定义”的假 API 回归：公共头文件承诺了实现并未提供的函数。
向导会留下桩代码，后续修改可能删除定义却保留声明；本门禁用来发现这种偏移。

用法
----
    python3 scripts/check_decl_def.py <repo_root>

退出码：0 = 干净；1 = 至少有一个公共声明没有定义。

做法
----
只检查 ``include/**/*.h`` 中形如 ``name(...);`` 的声明。为了适应真实 C 头文件，
匹配前先预处理文本：

- 注释（块注释和行注释）以及字符串/字符字面量被替换为空白，
  因此注释内或 ``"Use tokenprobe_probe()"`` 字符串中的函数名不会被误判为声明；
- 全大写标识符被替换为空白。按本项目命名约定
  （见 ``docs/guides/coding-style.md``），函数和变量使用 snake_case，
  宏和常量使用 UPPER_SNAKE_CASE，因此 ``TOKENPROBE_API`` 这类全大写名称是宏，
  不是函数声明；
- C11 关键字（以及少量常见宏内建）直接跳过。

因此门禁不依赖项目前缀，``scripts/rename-project.*`` 重命名项目后仍然有效。

``typedef``、宏和 ``extern`` 变量声明不匹配 ``name(...)`` 形式，会被忽略。
头文件中的 ``static inline`` 函数以 ``{`` 结尾而不是 ``;``，不会被当作裸声明。
声明只要在 ``src/**/*.c`` 中有同名定义即视为匹配；签名是否等价由编译器负责，
不在本门禁范围内。
"""

import argparse
import re
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


# C11 关键字（以及常见宏内建），绝不能当作函数名，例如
# _Static_assert(sizeof(x) == 4, "...")，或实现中的 while (x) { ... } 等控制流语句。
_KEYWORDS = frozenset(
    {
        "auto", "break", "case", "char", "const", "continue", "default", "do",
        "double", "else", "enum", "extern", "float", "for", "goto", "if",
        "inline", "int", "long", "register", "restrict", "return", "short",
        "signed", "sizeof", "static", "struct", "switch", "typedef", "union",
        "unsigned", "void", "volatile", "while", "_Alignas", "_Alignof",
        "_Atomic", "_Bool", "_Complex", "_Generic", "_Imaginary", "_Noreturn",
        "_Static_assert", "_Thread_local", "offsetof",
    }
)

# 函数声明：`name(...)` 后跟 `;`（支持跨行）。
_DECL_RE = re.compile(r"\b([A-Za-z_][A-Za-z0-9_]*)\s*\([^;{}]*\)\s*;")
# 函数指针声明：`(*name)` 或 `(*name)[...]`。紧邻 `*` 的标识符是被声明的符号，
# 其前面的内容是返回类型。
_FUNC_PTR_DECL_RE = re.compile(r"\b([A-Za-z_][A-Za-z0-9_]*)\s*\(\s*\*\s*([A-Za-z_][A-Za-z0-9_]*)\s*\)")
# 函数定义：`name(...)` 后跟 `{`（支持跨行）。
_DEF_RE = re.compile(r"\b([A-Za-z_][A-Za-z0-9_]*)\s*\([^;{}]*\)\s*\{")
# 全大写标识符：按约定是宏或常量。
_MACRO_RE = re.compile(r"\b[A-Z][A-Z0-9_]*\b")

# 扫描面下限，分两档（命令行覆盖见 main()）。
#
# 绝对下限是默认值，适用于本模板以及由它生成的项目（生成项目的扫描面小得多，
# 向导只留下一个桩 src/）。它们让“解析到 0 项却打印 OK”不可能发生。
_ABS_MIN_DECLARATIONS = 1
_ABS_MIN_DEFINITIONS = 1
#
# 模板下限由 cmake/Tests.cmake 中向导标记内的 CTest 注册显式传入
# （生成项目不会继承）：2026-10-10 测得 include/**/*.h 中 4 个唯一公共声明，
# src/**/*.c 中 58 个唯一定义（含向导的静态函数），按基线 × 0.8 取整为 3 / 46。
# 用于发现模板本身的正则或布局漂移。


def _blank_span(chars: list[str], start: int, end: int) -> None:
    """把 ``chars[start:end]`` 替换为空格，保留换行。"""
    for i in range(start, end):
        if chars[i] != "\n":
            chars[i] = " "


def _strip_non_code(text: str) -> str:
    """把注释、字符串和字符字面量的内容替换为空白，保留换行。"""
    chars = list(text)
    i = 0
    n = len(text)
    while i < n:
        c = text[i]
        if c == "/" and i + 1 < n and text[i + 1] == "*":
            end = text.find("*/", i + 2)
            end = n if end < 0 else end + 2
            _blank_span(chars, i, end)
            i = end
        elif c == "/" and i + 1 < n and text[i + 1] == "/":
            end = text.find("\n", i + 2)
            end = n if end < 0 else end
            _blank_span(chars, i, end)
            i = end
        elif c == '"':
            j = i + 1
            while j < n and text[j] != '"':
                if text[j] == "\\":
                    j += 1
                j += 1
            j = min(j + 1, n)
            _blank_span(chars, i, j)
            i = j
        elif c == "'":
            j = i + 1
            while j < n and text[j] != "'":
                if text[j] == "\\":
                    j += 1
                j += 1
            j = min(j + 1, n)
            _blank_span(chars, i, j)
            i = j
        else:
            i += 1
    return "".join(chars)


def _code_text(path: Path) -> str:
    """返回把注释、字面量和宏替换为空白后的文件文本。"""
    text = path.read_text(encoding="utf-8", errors="replace")
    text = _strip_non_code(text)
    return _MACRO_RE.sub(lambda match: " " * len(match.group(0)), text)


def collect_declarations(root: Path) -> dict[str, list[str]]:
    """把每个函数声明名映射到声明它的头文件列表。"""
    declarations: dict[str, list[str]] = {}
    for header in (root / "include").rglob("*.h"):
        text = _code_text(header)
        rel = str(header.relative_to(root))

        # 形如
        #     tokenprobe_status_t (*perform)(void *impl, ...);
        # 的声明定义的是 `perform`，前面的 `tokenprobe_status_t` 是返回类型。
        # 若不遮蔽声明符，_DECL_RE 会把返回类型当成声明名，
        # 从而把每个回调成员都误报为未声明。
        masked = list(text)

        for match in _FUNC_PTR_DECL_RE.finditer(text):
            _blank_span(masked, match.start(1), match.end(1))
        text = "".join(masked)

        for match in _DECL_RE.finditer(text):
            name = match.group(1)
            if name in _KEYWORDS:
                continue
            declarations.setdefault(name, []).append(rel)
    return declarations


def collect_definitions(root: Path) -> set[str]:
    """返回 src/ 下已定义的函数名集合。"""
    definitions: set[str] = set()
    for source in (root / "src").rglob("*.c"):
        for match in _DEF_RE.finditer(_code_text(source)):
            name = match.group(1)
            if name not in _KEYWORDS:
                definitions.add(name)
    return definitions


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(
        description="Every public declaration in include/ must have a "
                    "definition in src/.")
    parser.add_argument("repo_root", type=Path, nargs="?", default=Path("."))
    parser.add_argument("--min-declarations", type=int,
                        default=_ABS_MIN_DECLARATIONS,
                        help="scan-surface floor for include/ declarations")
    parser.add_argument("--min-definitions", type=int,
                        default=_ABS_MIN_DEFINITIONS,
                        help="scan-surface floor for src/ definitions")
    args = parser.parse_args(argv[1:])

    root: Path = args.repo_root
    min_declarations: int = args.min_declarations
    min_definitions: int = args.min_definitions
    declarations = collect_declarations(root)
    definitions = collect_definitions(root)

    # 扫描面下限：解析结果远少于下限，说明扫描本身坏了
    # （路径错误、目录改名或正则失配），而不是项目合理缩小。
    # 结构性删除确实会降低计数时，必须在同一笔改动里下调对应下限并记录原因。
    if len(declarations) < min_declarations:
        print(f"FAIL: 解析失败：include/ 公共声明仅 {len(declarations)} 项"
              f"（下限 {min_declarations}）——检查被扫描路径或写法是否变化",
              file=sys.stderr)
        return 1
    if len(definitions) < min_definitions:
        print(f"FAIL: 解析失败：src/ 函数定义仅 {len(definitions)} 项"
              f"（下限 {min_definitions}）——检查被扫描路径或写法是否变化",
              file=sys.stderr)
        return 1

    missing = [(name, headers) for name, headers in declarations.items()
               if name not in definitions]
    if missing:
        for name, headers in sorted(missing, key=lambda item: item[0]):
            for header in headers:
                print(f"ERROR: {header}: public declaration without definition: {name}",
                      file=sys.stderr)
        return 1

    print(f"OK: {len(declarations)} public declarations, "
          f"{len(definitions)} definitions, no missing definitions")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
