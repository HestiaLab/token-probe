#!/usr/bin/env python3
"""CHANGELOG 与 git tag 一致性门禁。

版本段落形如 ``## x.y.z``，后面可跟日期。检查项：

1. **段落格式** —— ``CHANGELOG.md`` 中每个 ``##`` 段落都必须是纯版本标题
   （``## x.y.z``，可带 `` — 日期`` 后缀）。标题不是版本即失败。
2. **每个 tag 都有段落** —— 每个 ``v*`` tag 都必须在 ``CHANGELOG.md`` 中有
   同版本段落。
3. **已发布段落冻结** —— 对有 tag 的版本，tag 处的段落文本必须与工作区中的
   段落文本逐字节一致。改写已发布段落即失败，并报告版本和 tag。

没有 tag 的版本（开发中/未发布）不冻结：跳过逐字节比较，并明确报告
**跳过 ≠ 通过**（跳过表示没有取证，不代表内容正确）。

必须有 git 上下文。若不是 git 工作区，或 git 无法提供 tag/段落证据，
门禁报告「无法取证」并以 1 退出——绝不降级为静默跳过，因为跳过却打印 OK
与真正通过无法区分。

用法
----
    python3 scripts/check_changelog_tag.py <repo_root>

退出码：0 = 全部检查通过（跳过项已报告，均不致命）；1 = 存在不一致，
或无法获得 git 证据。
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


_SECTION_RE = re.compile(r"^##\s+(?P<version>\d+\.\d+\.\d+)\s*(?:—|--|-)?\s*(?P<rest>.*)$")
_ANY_H2_RE = re.compile(r"^##\s+(?P<text>.*)$")
_TAG_RE = re.compile(r"^v(\d+\.\d+\.\d+)$")


def _fail(message: str, problems: list[str]) -> None:
    problems.append(message)


def run_git(repo: Path, *args: str) -> tuple[int, str, str]:
    """运行 git，并显式按 UTF-8 解码输出。

    ``git show <rev>:<path>`` 原样返回 blob（不像日志输出那样转码），
    本仓库又由 ``.gitattributes`` 保证 UTF-8。若让 subprocess 猜测本地编码
    （中文 Windows 为 GBK，runner 为 cp1252），读取线程会抛
    UnicodeDecodeError 并把 stdout 留成 ``None``，门禁会崩溃而不是给出判定。
    """
    result = subprocess.run(
        ["git", *args], cwd=repo, capture_output=True,
        encoding="utf-8", errors="replace", timeout=120)
    return result.returncode, result.stdout, result.stderr


def git_evidence_available(repo: Path) -> bool:
    code, out, _ = run_git(repo, "rev-parse", "--is-inside-work-tree")
    return code == 0 and out.strip() == "true"


def git_is_shallow(repo: Path) -> bool:
    code, out, _ = run_git(repo, "rev-parse", "--is-shallow-repository")
    return code == 0 and out.strip() == "true"


def list_version_tags(repo: Path) -> list[str]:
    """返回形如 x.y.z 的 v* tag 版本号，已排序。"""
    code, out, _ = run_git(repo, "tag", "-l", "v*")
    if code != 0:
        return []
    versions = []
    for line in out.splitlines():
        match = _TAG_RE.match(line.strip())
        if match:
            versions.append(match.group(1))
    return sorted(versions)


def parse_sections(changelog: Path) -> tuple[list[tuple[str, int, str]], list[str]]:
    """返回 (sections, shape_problems)。

    sections：由 (version, start_line, 含标题的段落文本) 组成的列表。
    段落从其 ``##`` 标题开始，到下一个 ``##`` 标题（不含）或文件末尾结束。
    """
    lines = changelog.read_text(encoding="utf-8", errors="replace").splitlines()
    sections: list[tuple[str, int, str]] = []
    problems: list[str] = []
    current: tuple[str, int, list[str]] | None = None

    for lineno, line in enumerate(lines, start=1):
        h2 = _ANY_H2_RE.match(line)
        if h2:
            if current is not None:
                sections.append((current[0], current[1], "\n".join(current[2])))
                current = None
            ver = _SECTION_RE.match(line)
            if ver is None:
                problems.append(
                    f"{changelog.name}:{lineno}: 段落标题不是版本号形态: "
                    f"'{h2.group('text').strip()}'（应为 '## x.y.z'，可带 ' — 日期' 后缀）")
                continue
            current = (ver.group("version"), lineno, [line])
        elif current is not None:
            current[2].append(line)

    if current is not None:
        sections.append((current[0], current[1], "\n".join(current[2])))
    return sections, problems


def section_at_tag(repo: Path, changelog_rel: str, tag: str) -> tuple[str, str] | None:
    """返回 `tag` 对应版本的 (段落文本, 原始文件文本)，不存在时返回 None。"""
    version = tag[1:] if tag.startswith("v") else tag
    code, out, _ = run_git(repo, "show", f"{tag}:{changelog_rel}")
    if code != 0:
        return None
    tag_path = repo / changelog_rel
    lines = out.splitlines()
    collected: list[str] = []
    in_section = False
    for line in lines:
        if _ANY_H2_RE.match(line):
            if in_section:
                break
            ver = _SECTION_RE.match(line)
            if ver and ver.group("version") == version:
                in_section = True
                collected.append(line)
                continue
        elif in_section:
            collected.append(line)
    if not in_section:
        return None
    del tag_path
    return "\n".join(collected), out


def normalize(text: str) -> str:
    """比较时忽略尾部空行和换行风格。"""
    return text.replace("\r\n", "\n").rstrip("\n")


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(
        description="CHANGELOG <-> git tag consistency gate.")
    parser.add_argument("repo_root", type=Path)
    args = parser.parse_args(argv[1:])

    root: Path = args.repo_root.resolve()
    if not root.is_dir():
        print(f"FAIL: 目录不存在: {root}", file=sys.stderr)
        return 1

    changelog = root / "CHANGELOG.md"
    if not changelog.is_file():
        print(f"FAIL: 缺少 {changelog}", file=sys.stderr)
        return 1

    if not git_evidence_available(root):
        print("FAIL: 无法取证——该目录不是 git 工作副本，"
              "CHANGELOG↔tag 一致性无法验证（不降级为跳过）", file=sys.stderr)
        return 1
    if git_is_shallow(root):
        # 浅克隆不会拉取 tag，因此这里得到“没有 tag”会对一个实际有发布版本的
        # 仓库造成空泛通过。
        print("FAIL: 无法取证——浅克隆不会取到 tag，"
              "「无 tag 段落」结论是假绿；请用完整克隆"
              "（CI 需 fetch-depth: 0）（不降级为跳过）", file=sys.stderr)
        return 1

    problems: list[str] = []
    skips: list[str] = []

    sections, shape_problems = parse_sections(changelog)
    for problem in shape_problems:
        _fail(problem, problems)

    if not sections:
        _fail(f"{changelog.name}: 没有任何 '## x.y.z' 版本段落（扫描面为空）",
              problems)

    section_by_version = {version: (lineno, text) for version, lineno, text in sections}

    tags = list_version_tags(root)
    for tag_version in tags:
        if tag_version not in section_by_version:
            _fail(f"{changelog.name}: tag v{tag_version} 没有对应的版本段落",
                  problems)
            continue
        lineno, worktree_text = section_by_version[tag_version]
        at_tag = section_at_tag(root, changelog.name, f"v{tag_version}")
        if at_tag is None:
            _fail(f"{changelog.name}:{lineno}: 无法取证——tag v{tag_version} 处"
                  f"读不到该版本段落（不降级为跳过）", problems)
            continue
        tag_text, _ = at_tag
        if normalize(tag_text) != normalize(worktree_text):
            _fail(f"{changelog.name}:{lineno}: 已发布段落被改写——v{tag_version} "
                  f"版本段落与 tag 处不逐字一致（已发布段落不可改写）", problems)

    for version, (lineno, _text) in sorted(section_by_version.items()):
        if version not in tags:
            skips.append(f"{changelog.name}:{lineno}: 版本 {version} 无对应 tag，"
                         "跳过冻结比对（跳过 ≠ 通过：未取证，不代表内容正确）")

    for skip in skips:
        print(f"SKIP: {skip}")
    if problems:
        for problem in problems:
            print(f"ERROR: {problem}", file=sys.stderr)
        print(f"FAIL: CHANGELOG↔tag 门禁发现 {len(problems)} 处问题", file=sys.stderr)
        return 1

    print(f"OK: {len(sections)} 个版本段落，{len(tags)} 个 v* tag，"
          f"冻结比对全部逐字一致（{len(skips)} 处跳过）")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
