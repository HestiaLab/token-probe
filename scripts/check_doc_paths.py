#!/usr/bin/env python3
"""反引号路径可达性门禁（git 视图）。

文档中每个反引号包裹、形似路径的 token，都必须从 **git 视角**存在：
已跟踪（索引）∪ 已忽略 ∪ 历史并集。只在磁盘上存在、但不属于 git 的路径会被
明确报告——引用未提交文件的文档会让其他人无法使用。

token 选取规则（已文档化，未硬编码项目名）：

- token 含 ``/``，且匹配 ``[A-Za-z0-9._][A-Za-z0-9._/+-]*``
  （因此 ``--enable-tidy`` 这类选项、``include/<project>/x.h`` 这类尖括号写法
  以及含空格的普通文字都不算路径）。
- 锚点：先相对仓库根解析，再相对文档所在目录解析。
- 首段过滤：token 的第一段必须作为名称存在于某个锚点的磁盘或 git 视图中。
  这排除了安装树和构建树布局引用（如包内容表中的 ``bin/``、
  ``lib/cmake/<Project>/``、``share/<project>/``，以及 ``<ns>/version.h``
  包含路径），它们从不出现在仓库中。该过滤不会掩盖删除漂移：被删除的顶层目录
  仍在历史并集中，因此指向它的引用仍是候选，并通过 git 视图解析。

git I/O 全程使用字节和 ``-z``（NUL 分隔）：在 Windows 上文本模式会把 stdin 中的
``\\n`` 转成 ``\\r\\n``，导致 ``git check-ignore --stdin`` 匹配不到任何内容。

必须有 git 上下文：非 git 目录下门禁报告「无法取证」并以 1 退出，
绝不解级为静默跳过（跳过 ≠ 通过）。

扫描范围：git 会展示的 Markdown 文件（已跟踪 ∪ 未跟踪且未忽略，
排除 ``build/`` 和 ``.git/``）。本地被忽略的临时内容（``.hermes/``、``dist/``）
不能影响判定，否则同一提交在开发机和 CI 检出中会得到不同结果。

用法
----
    python3 scripts/check_doc_paths.py <repo_root>

退出码：0 = 干净；1 = 存在不可达/未跟踪引用、扫描面为空或缺少 git 证据。
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


_EXCLUDED_DIRS = {"build", ".git"}
_BACKTICK_RE = re.compile(r"`([^`\n]+)`")
_PATH_TOKEN_RE = re.compile(r"^[A-Za-z0-9._][A-Za-z0-9._/+-]*$")
# 扫描面下限：文档门禁在有 Markdown 文件的仓库里找不到路径 token，
# 说明分词器坏了，而不是文档干净。
_MIN_TOKENS = 10  # 2026-10-10 在本模板测得 48，留约 0.2 的余量


def _fail(message: str, problems: list[str]) -> None:
    problems.append(message)


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


def git_bytes(repo: Path, *args: str, stdin: bytes | None = None) -> tuple[int, bytes]:
    result = subprocess.run(
        ["git", *args], cwd=repo, input=stdin, capture_output=True, timeout=300)
    return result.returncode, result.stdout


def git_evidence_available(repo: Path) -> bool:
    code, out = git_bytes(repo, "rev-parse", "--is-inside-work-tree")
    return code == 0 and out.strip() == b"true"


def git_is_shallow(repo: Path) -> bool:
    code, out = git_bytes(repo, "rev-parse", "--is-shallow-repository")
    return code == 0 and out.strip() == b"true"


def git_view_paths(repo: Path) -> set[str]:
    """已跟踪（索引）∪ 历史的并集，表示为仓库相对 posix 字符串。"""
    view: set[str] = set()
    for args in (
        ("ls-files", "-z"),
        ("log", "--all", "--pretty=tformat:", "--name-only", "-z"),
    ):
        code, out = git_bytes(repo, *args)
        if code != 0:
            continue
        for raw in out.split(b"\0"):
            if raw:
                view.add(raw.decode("utf-8", "replace"))
    return view


def ignored_set(repo: Path, candidates: list[str]) -> set[str]:
    """批量执行 check-ignore，使用 NUL 分隔的字节 I/O（兼容 Windows）。"""
    if not candidates:
        return set()
    stdin = b"\0".join(c.encode("utf-8") for c in candidates) + b"\0"
    code, out = git_bytes(repo, "check-ignore", "--stdin", "-z", stdin=stdin)
    # 退出码 0 = 有路径被忽略；1 = 没有路径被忽略；>1 = 出错
    if code > 1:
        return set()
    return {raw.decode("utf-8", "replace") for raw in out.split(b"\0") if raw}


def strip_fenced(text: str) -> list[tuple[int, str]]:
    """返回不在围栏代码块内的 (行号, 行内容)。"""
    out = []
    in_fence = False
    for lineno, line in enumerate(text.splitlines(), start=1):
        if line.strip().startswith("```") or line.strip().startswith("~~~"):
            in_fence = not in_fence
            continue
        if not in_fence:
            out.append((lineno, line))
    return out


def iter_docs(root: Path):
    live = git_live_paths(root)
    for path in sorted(root.rglob("*.md")):
        rel = path.relative_to(root)
        if any(part in _EXCLUDED_DIRS for part in rel.parts):
            continue
        if live is not None and rel.as_posix() not in live:
            continue
        yield path


def collect_tokens(root: Path) -> list[tuple[str, str, int, str]]:
    """每个形似路径的反引号 token 的 (token, 文档相对路径, 行号, 原文)。"""
    found = []
    for path in iter_docs(root):
        rel = str(path.relative_to(root))
        text = path.read_text(encoding="utf-8", errors="replace")
        for lineno, line in strip_fenced(text):
            for match in _BACKTICK_RE.finditer(line):
                token = match.group(1).strip()
                if "/" not in token:
                    continue
                if token.startswith(("-", "/")):
                    continue
                if not _PATH_TOKEN_RE.match(token):
                    continue
                found.append((token.rstrip("/"), rel, lineno, token))
    return found


def anchor_candidates(root: Path, token: str, doc_rel: str):
    """依次产出每个锚点的 (锚点名, 仓库相对 posix 路径, 绝对路径)。"""
    doc_dir = (root / doc_rel).parent
    for name, base in (("repo-root", root), ("doc-dir", doc_dir)):
        abs_path = (base / token).resolve()
        try:
            rel = abs_path.relative_to(root.resolve())
        except ValueError:
            continue  # 通过 ../ 逃出了仓库，不属于仓库引用
        yield name, rel.as_posix(), abs_path


def first_segment_known(root: Path, token: str, doc_rel: str,
                        git_view: set[str]) -> bool:
    """token 的第一段是否存在于某个锚点（磁盘或 git 视图）？

    git 视图中的顶层名称易于直接推导；文档目录层的名称则同时在磁盘和
    该文档目录下的 git 视图中检查。
    """
    first = token.split("/", 1)[0]
    if (root / first).exists():
        return True
    if first in {p.split("/", 1)[0] for p in git_view}:
        return True
    doc_dir_rel = str((root / doc_rel).parent.relative_to(root))
    if doc_dir_rel != ".":
        if (root / doc_dir_rel / first).exists():
            return True
        prefix = doc_dir_rel + "/"
        if first in {p[len(prefix):].split("/", 1)[0]
                     for p in git_view if p.startswith(prefix)}:
            return True
    return False


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(
        description="Backtick path reachability gate (git view).")
    parser.add_argument("repo_root", type=Path)
    args = parser.parse_args(argv[1:])

    root: Path = args.repo_root.resolve()
    if not root.is_dir():
        print(f"FAIL: 目录不存在: {root}", file=sys.stderr)
        return 1
    if not git_evidence_available(root):
        print("FAIL: 无法取证——该目录不是 git 工作副本，"
              "反引号路径的 git 视角判定无法进行（不降级为跳过）", file=sys.stderr)
        return 1
    if git_is_shallow(root):
        print("FAIL: 无法取证——浅克隆的历史并集不完整，"
              "「git 视角下不存在」的判定不可靠；请用完整克隆"
              "（CI 需 fetch-depth: 0）（不降级为跳过）", file=sys.stderr)
        return 1

    tokens = collect_tokens(root)
    problems: list[str] = []
    if not tokens:
        _fail(f"{root}: 解析失败：活文档中没有任何反引号路径（扫描面为空）",
              problems)
    if 0 < len(tokens) < _MIN_TOKENS:
        _fail(f"{root}: 解析失败：反引号路径仅 {len(tokens)} 项"
              f"（下限 {_MIN_TOKENS}）——检查扫描范围或分词规则是否变化",
              problems)

    git_view = git_view_paths(root)
    if not git_view:
        _fail(f"{root}: 无法取证——git 视角为空（无 tracked 且无历史）", problems)

    # 在各锚点解析每个 token，按最有力的证据分类。
    unresolved_rel: set[str] = set()
    located: dict[tuple[str, str], tuple[str, str]] = {}
    skipped: list[str] = []
    for token, doc_rel, lineno, _raw in tokens:
        key = (token, doc_rel)
        if key in located:
            continue
        best = None
        for _anchor, rel, abs_path in anchor_candidates(root, token, doc_rel):
            in_view = rel in git_view or any(
                f.startswith(rel + "/") for f in git_view)
            on_disk = abs_path.exists()
            if in_view:
                best = ("git-view", rel)
                break
            if on_disk:
                best = ("untracked", rel)  # 继续寻找 git 视图中的锚点
                unresolved_rel.add(rel)
            elif best is None:
                best = ("nowhere", rel)
                unresolved_rel.add(rel)
        if best is None:
            continue  # 在所有锚点都通过 ../ 逃出了仓库
        if best[0] == "nowhere" and not first_segment_known(
                root, token, doc_rel, git_view):
            skipped.append(f"{doc_rel}:{lineno}: `{token}` 非仓库锚定路径"
                           "（首段在任何锚点的盘上/git 视角都不存在，"
                           "判为安装树/构建树布局引用）")
            continue
        located[key] = best

    ignored = ignored_set(root, sorted(unresolved_rel))

    reported: set[str] = set()
    for token, doc_rel, lineno, _raw in tokens:
        key = (token, doc_rel)
        if key not in located:
            continue
        status, rel = located[key]
        if status == "git-view" or rel in ignored:
            continue
        dedupe = f"{doc_rel}:{lineno}:{rel}"
        if dedupe in reported:
            continue
        reported.add(dedupe)
        if status == "untracked":
            _fail(f"{doc_rel}:{lineno}: 反引号路径盘上存在但未入库: `{token}`"
                  f"（{rel}）——git add 之后其他人才能看到该文件", problems)
        else:
            _fail(f"{doc_rel}:{lineno}: 反引号路径在 git 视角下不存在: `{token}`"
                  f"（已入库 ∪ 已忽略 ∪ 历史并集均无 {rel}）", problems)

    for skip in skipped:
        print(f"SKIP: {skip}")
    if problems:
        for problem in problems:
            print(f"ERROR: {problem}", file=sys.stderr)
        print(f"FAIL: 反引号路径门禁发现 {len(problems)} 处问题", file=sys.stderr)
        return 1
    print(f"OK: {len(tokens)} 个反引号路径 token 全部可达"
          f"（git 视角；{len(skipped)} 处判为非仓库锚定并跳过）")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
