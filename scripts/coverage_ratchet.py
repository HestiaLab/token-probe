#!/usr/bin/env python3
"""覆盖率棘轮：行覆盖率只能升，不能降。

读取项目覆盖率预设构建（默认 ``ninja-coverage``：带覆盖率标志配置、构建并运行
CTest）产生的 gcov 数据，计算项目自身源码（``src/``）的行覆盖率，
并与已保存的基线比较。

基线策略：基线只向上调整。``--update`` 仅在本次测量值大于或等于已存基线时
写入新基线；低于基线的测量会使检查失败。

本脚本是可选入口：有意不接入 CI 或 ``scripts/check.sh``
（覆盖率数据对工具链和平台敏感；参见 ``C_PROJECT_STANDARD.md`` 中的经验：
覆盖率阈值基线只能有唯一记录平台）。

用法
----
    # 完整流程（推荐）：
    cmake --preset ninja-coverage
    cmake --build --preset ninja-coverage
    python3 scripts/coverage_ratchet.py --reset-gcda <build-dir>   # 清除旧计数器
    ctest --preset ninja-coverage --output-on-failure             # 重新生成计数器
    python3 scripts/coverage_ratchet.py <build-dir>               # 与基线比较
    python3 scripts/coverage_ratchet.py <build-dir> --update      # 上调基线

要求：GCC/Clang 覆盖率构建（``--coverage``），且 PATH 中有 ``gcov``。
没有硬编码项目相关内容：源文件从 gcov 数据中发现，并过滤到仓库的 ``src/`` 树。

退出码：0 = 达到或高于基线（或已用 --update 写入基线）；
1 = 低于基线或测量失败。
"""

import argparse
import re
import shutil
import subprocess
import sys
import tempfile
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


_BASELINE_FILENAME = "coverage_baseline.txt"
_GCOV_LINE_RE = re.compile(r"^\s*(#####|-|\d+[*!]?):")


def _fail(message: str) -> None:
    print(f"FAIL: {message}", file=sys.stderr)


def find_gcda_files(build_dir: Path) -> list[Path]:
    return sorted(build_dir.rglob("*.gcda"))


def reset_gcda(build_dir: Path) -> int:
    """删除旧的 .gcda 计数器，使下一次测试运行只统计自身。

    经验：不清除 .gcda 时，少跑测试仍会显示旧的（可能是完整的）覆盖率，
    因为计数器会跨运行累积。
    """
    removed = 0
    for gcda in find_gcda_files(build_dir):
        gcda.unlink()
        removed += 1
    print(f"removed {removed} stale .gcda file(s) under {build_dir}")
    return 0


def measure_coverage(build_dir: Path, repo_root: Path) -> tuple[int, int]:
    """对构建树运行 gcov，统计 src/ 文件已覆盖/总行数。"""
    gcov = shutil.which("gcov")
    if gcov is None:
        raise RuntimeError("gcov not found on PATH")

    src_root = (repo_root / "src").resolve()
    covered = total = 0
    with tempfile.TemporaryDirectory(prefix="coverage-ratchet-") as tmp:
        for gcda in find_gcda_files(build_dir):
            result = subprocess.run(
                [gcov, "--preserve-paths", "--object-directory",
                 str(gcda.parent), str(gcda)],
                capture_output=True, encoding="utf-8", errors="replace",
                timeout=300, cwd=tmp,
            )
            if result.returncode != 0:
                continue
            for gcov_file in Path(tmp).rglob("*.gcov"):
                source = _gcov_source_path(gcov_file)
                if source is None:
                    continue
                try:
                    resolved = Path(source).resolve()
                except OSError:
                    continue
                if not _is_within(resolved, src_root):
                    continue
                c, t = _count_gcov_lines(gcov_file)
                covered += c
                total += t
                gcov_file.unlink()
    return covered, total


def _gcov_source_path(gcov_file: Path) -> str | None:
    """从 .gcov 头部行提取源文件路径。

    gcov 输出形如 ``        -:    0:Source:/path/to/file.c``
    （三个冒号分隔的前缀字段，然后是记录内容）。
    """
    try:
        with gcov_file.open(encoding="utf-8", errors="replace") as handle:
            for line in handle:
                if ":Source:" in line:
                    return line.split(":Source:", 1)[1].strip()
                if ":Graph:" in line:
                    break  # 已过头部但未出现 Source 记录
    except OSError:
        return None
    return None


def _is_within(path: Path, root: Path) -> bool:
    try:
        path.relative_to(root)
        return True
    except ValueError:
        return False


def _count_gcov_lines(gcov_file: Path) -> tuple[int, int]:
    covered = total = 0
    with gcov_file.open(encoding="utf-8", errors="replace") as handle:
        for line in handle:
            match = _GCOV_LINE_RE.match(line)
            if not match:
                continue
            count = match.group(1)
            if count == "-":
                continue  # 不可执行行
            total += 1
            if count != "#####":
                covered += 1
    return covered, total


def read_baseline(baseline_path: Path) -> float | None:
    if not baseline_path.is_file():
        return None
    text = baseline_path.read_text(encoding="utf-8").strip()
    match = re.search(r"(\d+(?:\.\d+)?)", text)
    return float(match.group(1)) if match else None


def write_baseline(baseline_path: Path, percent: float) -> None:
    # 保存足够精度，避免同一棵树重新测量时因显示舍入被判为“低于基线”。
    baseline_path.write_text(f"{percent:.6f}\n", encoding="utf-8")


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(
        description="Coverage ratchet: line coverage may only go up.")
    parser.add_argument("build_dir", type=Path,
                        help="coverage-instrumented build tree (e.g. "
                             "build/ninja-coverage)")
    parser.add_argument("--repo-root", type=Path,
                        default=Path(__file__).resolve().parents[1])
    parser.add_argument("--baseline-file", type=Path, default=None,
                        help=f"default: scripts/{_BASELINE_FILENAME}")
    parser.add_argument("--update", action="store_true",
                        help="write the measured value as the new baseline "
                             "(only upward moves are allowed)")
    parser.add_argument("--reset-gcda", action="store_true",
                        help="delete stale .gcda counters and exit (run this "
                             "before re-running the tests)")
    args = parser.parse_args(argv[1:])

    repo_root: Path = args.repo_root.resolve()
    build_dir: Path = args.build_dir
    if not build_dir.is_dir():
        _fail(f"构建目录不存在: {build_dir}")
        return 1

    if args.reset_gcda:
        return reset_gcda(build_dir)

    baseline_path = args.baseline_file or (
        Path(__file__).resolve().parent / _BASELINE_FILENAME)

    try:
        covered, total = measure_coverage(build_dir, repo_root)
    except RuntimeError as exc:
        _fail(str(exc))
        return 1

    if total == 0:
        _fail(f"覆盖率测量为 0 行可执行代码——{build_dir} 可能不是覆盖率构建，"
              "或测试尚未运行（先 --reset-gcda 再跑 ctest）")
        return 1

    percent = 100.0 * covered / total
    print(f"measured: {covered}/{total} lines = {percent:.2f}% (src/ only)")

    baseline = read_baseline(baseline_path)
    if baseline is None:
        if args.update:
            write_baseline(baseline_path, percent)
            print(f"baseline written: {percent:.2f}% -> {baseline_path}")
            return 0
        _fail(f"没有基线文件 {baseline_path}；先用 --update 写入首个基线")
        return 1

    print(f"baseline: {baseline:.2f}% ({baseline_path})")
    if percent + 1e-6 < baseline:
        _fail(f"覆盖率下降：{percent:.2f}% < 基线 {baseline:.2f}%")
        return 1

    if args.update and percent > baseline:
        write_baseline(baseline_path, percent)
        print(f"baseline ratcheted up: {baseline:.2f}% -> {percent:.2f}%")
    elif percent >= baseline:
        print(f"OK: coverage {percent:.2f}% >= baseline {baseline:.2f}%")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
