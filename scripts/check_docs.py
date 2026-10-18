#!/usr/bin/env python3
"""文档门禁：链接/锚点可达性、围栏配对、索引覆盖。

子检查（``--mode``）：

``links``（B1）
    - ``*.md`` 中每个相对 Markdown 链接目标都必须存在于磁盘
      （先做 URL 解码；``%20`` 是合法写法）。排除 ``build/`` 和 ``.git/``。
      外部 URL（http/https/mailto）不做抓取，也不检查。
    - 每个页内锚点（``#foo``）和跨文件锚点（``file.md#foo``）都必须匹配
      目标文件中的标题；锚点由真实标题文本按统一 slug 规则生成
      （GitHub 风格：小写、去掉标点但保留连字符/下划线/中文、空格转连字符）。
      手写锚点匹配不到任何标题即失败。
    - 每个文件中的代码围栏（``` 和 ~~~）必须成对。

扫描范围
--------
只检查 git 会展示的文件（已跟踪 ∪ 未跟踪且未忽略），排除 ``build/`` 和 ``.git/``。
本地被忽略的临时目录不能影响判定，否则同一提交在开发机和 CI 检出中会得到
不同结果。不在 git 工作区内时使用普通磁盘遍历，因为此时没有忽略语义可遵循。

``index``（B3'）
    - 双向比较根 ``README.md`` 中的文档索引（其 docs/ 链接）与
      ``docs/**/*.md`` 文件集合：不能有孤立文档（docs/ 下每个文件都应在
      README 索引中链接），也不能有死索引项（索引中每个 docs 相对链接都能解析）。

有意不检查：CHANGELOG 表格连续性（模板文档不是受版本控制的文档）。

用法
----
    python3 scripts/check_docs.py <repo_root> [--mode links|index|all]

退出码：0 = 干净；1 = 至少有一处违规，每条以 ``文件:行号: 消息`` 输出。
"""

import argparse
import re
import subprocess
import sys
import urllib.parse
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


_EXCLUDED_DIRS = {"build", ".git"}
_LINK_RE = re.compile(r"(?<!!)\[[^\]]*\]\(([^)\s]+)\)")
_FENCE_RE = re.compile(r"^\s*(```|~~~)")
_HEADING_RE = re.compile(r"^(#{1,6})\s+(.*)$")


def _fail(location: str, message: str, problems: list[str]) -> None:
    problems.append(f"{location}: {message}")


def git_live_paths(root: Path) -> set[str] | None:
    """git 会展示的仓库相对路径：已跟踪 ∪ 未跟踪且未忽略。

    门禁检查的是仓库，而不是工作目录里恰好存在的文件。本地被忽略的临时内容
    （``.hermes/``、``dist/``）不能影响判定，否则同一提交在开发机和 CI 检出中
    会得到不同结果。不在 git 工作区内时返回 ``None``：此时没有忽略语义可遵循，
    磁盘遍历是唯一可用的定义。
    """
    try:
        inside = subprocess.run(
            ["git", "rev-parse", "--is-inside-work-tree"], cwd=root,
            capture_output=True, encoding="utf-8", errors="replace", timeout=60)
        if inside.returncode != 0 or inside.stdout.strip() != "true":
            return None
        listing = subprocess.run(
            ["git", "ls-files", "-z", "--cached", "--others",
             "--exclude-standard"], cwd=root,
            capture_output=True, encoding="utf-8", errors="replace", timeout=300)
    except (OSError, subprocess.TimeoutExpired):
        return None
    if listing.returncode != 0:
        return None
    return {entry for entry in listing.stdout.split("\0") if entry}


def iter_markdown_files(root: Path):
    live = git_live_paths(root)
    for path in sorted(root.rglob("*.md")):
        rel = path.relative_to(root)
        if any(part in _EXCLUDED_DIRS for part in rel.parts):
            continue
        if live is not None and rel.as_posix() not in live:
            continue
        yield path


def split_code_spans(text_lines: list[str]) -> list[tuple[int, str, bool]]:
    """返回文件中每行的 (行号, 行内容, 是否在围栏内)。"""
    result = []
    in_fence = False
    fence_char = ""
    for lineno, line in enumerate(text_lines, start=1):
        match = _FENCE_RE.match(line)
        if match:
            marker = match.group(1)
            if not in_fence:
                in_fence = True
                fence_char = marker
                result.append((lineno, line, True))
                continue
            if marker == fence_char:
                in_fence = False
                fence_char = ""
        result.append((lineno, line, in_fence))
    return result


def github_slug(heading_text: str) -> str:
    """GitHub 风格标题 slug：转小写，去掉标点（保留连字符、下划线和
    非 ASCII 单词字符），空格转连字符。"""
    text = heading_text.strip()
    text = re.sub(r"[`*_]", "", text)  # 行内标记字符
    text = text.lower()
    out = []
    for ch in text:
        if ch.isalnum() or ch in "-_" or ord(ch) > 127:
            out.append(ch)
        elif ch.isspace():
            out.append("-")
        # 其他字符（标点）一律丢弃
    return "".join(out)


def collect_anchors(lines: list[str]) -> set[str]:
    """全部真实标题的锚点（代码围栏内的标题不计入）。"""
    anchors: set[str] = set()
    for _lineno, line, in_fence in split_code_spans(lines):
        if in_fence:
            continue
        match = _HEADING_RE.match(line)
        if match:
            slug = github_slug(match.group(2))
            if slug:
                anchors.add(slug)
    return anchors


def check_fences(root: Path, path: Path, lines: list[str],
                 problems: list[str]) -> None:
    annotated = split_code_spans(lines)
    # 文件围栏平衡的充要条件：最后一行不在围栏内。
    if annotated and annotated[-1][2]:
        _fail(str(path.relative_to(root)), "代码围栏未配对（文件结尾仍在围栏内）",
              problems)


def check_links(root: Path, all_files: list[Path], problems: list[str]) -> None:
    anchor_cache: dict[Path, set[str]] = {}
    for path in all_files:
        lines = path.read_text(encoding="utf-8",
                               errors="replace").splitlines()
        check_fences(root, path, lines, problems)
        for lineno, line, in_fence in split_code_spans(lines):
            if in_fence:
                continue
            for match in _LINK_RE.finditer(line):
                target = match.group(1).strip()
                if target.startswith(("http://", "https://", "mailto:")):
                    continue
                location = f"{path.relative_to(root)}:{lineno}"
                decoded = urllib.parse.unquote(target)
                if decoded.startswith("#"):
                    if path not in anchor_cache:
                        anchor_cache[path] = collect_anchors(lines)
                    anchor = decoded[1:]
                    if anchor and anchor not in anchor_cache[path]:
                        _fail(location,
                              f"页内锚点不可达: #{anchor}（没有匹配的标题）",
                              problems)
                    continue
                file_part, _, frag = decoded.partition("#")
                if not file_part:
                    continue
                resolved = (path.parent / file_part).resolve()
                if not resolved.exists():
                    _fail(location, f"相对链接目标不存在: {file_part}", problems)
                    continue
                if frag:
                    if resolved.is_file() and resolved.suffix == ".md":
                        if resolved not in anchor_cache:
                            anchor_cache[resolved] = collect_anchors(
                                resolved.read_text(encoding="utf-8",
                                                   errors="replace")
                                .splitlines())
                        if frag not in anchor_cache[resolved]:
                            _fail(location,
                                  f"跨文件锚点不可达: {file_part}#{frag}",
                                  problems)


def check_index(root: Path, problems: list[str]) -> None:
    docs_root = root / "docs"
    index_path = root / "README.md"
    if not docs_root.is_dir():
        _fail(str(root), "docs/ 目录不存在", problems)
        return
    if not index_path.is_file():
        _fail(str(root), "缺少文档索引 README.md", problems)
        return

    actual = {path.resolve() for path in iter_markdown_files(docs_root)}
    if not actual:
        _fail(str(docs_root), "docs/ 下没有任何文档（扫描面为空）", problems)
        return

    linked: set[Path] = set()
    index_lines = index_path.read_text(encoding="utf-8",
                                       errors="replace").splitlines()
    for lineno, line, in_fence in split_code_spans(index_lines):
        if in_fence:
            continue
        for match in _LINK_RE.finditer(line):
            target = urllib.parse.unquote(match.group(1).strip())
            if target.startswith(("http://", "https://", "mailto:")):
                continue
            file_part = target.partition("#")[0]
            if not file_part:
                continue
            resolved = (index_path.parent / file_part).resolve()
            location = f"README.md:{lineno}"
            # 只有指向 docs/ 的链接属于索引契约：
            # docs/ 死链是索引缺陷，其他相对链接已由 links 模式覆盖。
            if docs_root.resolve() not in resolved.parents:
                continue
            if not resolved.exists():
                _fail(location, f"索引死链: {file_part}", problems)
                continue
            linked.add(resolved)

    orphans = sorted(actual - linked)
    for orphan in orphans:
        _fail(str(orphan.relative_to(root)),
              "孤儿文档: README.md 文档索引未引用该文件", problems)


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(
        description="Documentation gate: links, anchors, fences, index.")
    parser.add_argument("repo_root", type=Path)
    parser.add_argument("--mode", choices=("links", "index", "all"),
                        default="all")
    args = parser.parse_args(argv[1:])

    root: Path = args.repo_root.resolve()
    if not root.is_dir():
        print(f"FAIL: 目录不存在: {root}", file=sys.stderr)
        return 1

    all_files = list(iter_markdown_files(root))
    problems: list[str] = []

    if args.mode in ("links", "all"):
        if not all_files:
            _fail(str(root), "解析失败：扫描面内没有任何 .md 文件", problems)
        check_links(root, all_files, problems)
    if args.mode in ("index", "all"):
        check_index(root, problems)

    if problems:
        for problem in problems:
            print(f"ERROR: {problem}", file=sys.stderr)
        print(f"FAIL: docs 门禁发现 {len(problems)} 处问题", file=sys.stderr)
        return 1
    print(f"OK: docs 门禁通过（mode={args.mode}，扫描 {len(all_files)} 个 .md 文件）")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
