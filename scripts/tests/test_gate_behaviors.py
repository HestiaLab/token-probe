#!/usr/bin/env python3
"""质量门禁脚本的单元测试（仅用标准库）。

这些测试验证门禁确实会失败——在错误输入上无法返回非零退出码的门禁形同虚设。
每个测试都在临时目录下用一次性夹具，以子进程（或导入）方式驱动真实脚本，
不会修改仓库本身。

直接运行：
    python3 scripts/tests/test_gate_behaviors.py
或通过 unittest 发现机制：
    python3 -m unittest discover -s scripts/tests
"""

import importlib.util
import os
import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

# scripts/tests/ -> scripts/ -> 仓库根目录
_REPO_ROOT = Path(__file__).resolve().parents[2]
_SCRIPTS_DIR = _REPO_ROOT / "scripts"
_GIT = shutil.which("git")


def _run_script(script: Path, *args: str,
                env: "dict[str, str] | None" = None) -> subprocess.CompletedProcess:
    """以子进程运行门禁脚本并捕获判定结果。

    脚本已把输出流固定为 UTF-8，因此这里也按 UTF-8 解码：父进程的本地编码
    不可靠（在控制台代码页不是 UTF-8 的主机上会把中文判定变成乱码）。
    ``env`` 在当前环境之上追加覆盖项。
    """
    child_env = None if env is None else {**os.environ, **env}
    return subprocess.run(
        [sys.executable, str(script), *args],
        capture_output=True,
        encoding="utf-8",
        errors="replace",
        env=child_env,
        timeout=120,
    )


def _load_module(script: Path, module_name: str):
    spec = importlib.util.spec_from_file_location(module_name, script)
    assert spec is not None and spec.loader is not None
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def _repo_is_full_git_clone() -> bool:
    """当 _REPO_ROOT 是拥有完整历史的 git 工作区时返回 True。

    只有此时依赖 git 的门禁才有足够证据给出结论，“必须通过”的断言才有意义。
    向导生成的项目在创建时不是 git 仓库，也会运行本测试文件，
    因此那里的正向测试必须跳过。
    """
    if not _GIT:
        return False
    inside = subprocess.run(
        [_GIT, "rev-parse", "--is-inside-work-tree"],
        cwd=_REPO_ROOT, capture_output=True, text=True)
    if inside.returncode != 0 or inside.stdout.strip() != "true":
        return False
    shallow = subprocess.run(
        [_GIT, "rev-parse", "--is-shallow-repository"],
        cwd=_REPO_ROOT, capture_output=True, text=True)
    return not (shallow.returncode == 0 and shallow.stdout.strip() == "true")


class TestDeclDefGate(unittest.TestCase):
    """scripts/check_decl_def.py 必须让干净的树通过，让损坏的树失败。"""

    def setUp(self) -> None:
        self.script = _SCRIPTS_DIR / "check_decl_def.py"
        self.assertTrue(self.script.is_file(), f"missing gate script: {self.script}")
        self.tmp = Path(tempfile.mkdtemp(prefix="gate-test-decldef-"))
        self.addCleanup(shutil.rmtree, self.tmp, ignore_errors=True)

    def test_clean_repo_passes(self) -> None:
        result = _run_script(self.script, str(_REPO_ROOT))
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("OK:", result.stdout)

    def test_empty_scan_surface_fails(self) -> None:
        """扫描不到任何内容的门禁不得报告 OK（扫描面下限）。"""
        empty = self.tmp / "empty"
        empty.mkdir()
        result = _run_script(self.script, str(empty))
        self.assertEqual(result.returncode, 1)
        self.assertIn("解析失败", result.stderr)

    def test_explicit_floor_is_enforced(self) -> None:
        """显式下限高于真实扫描面时必须失败。

        模板严格注册（位于向导标记内）依赖这一行为；
        它对任何树都必须成立，而不只是本仓库。
        """
        result = _run_script(self.script, str(_REPO_ROOT),
                             "--min-declarations", "100000")
        self.assertEqual(result.returncode, 1)
        self.assertIn("下限 100000", result.stderr)

    def test_floor_at_or_below_surface_passes(self) -> None:
        result = _run_script(self.script, str(_REPO_ROOT),
                             "--min-declarations", "1",
                             "--min-definitions", "1")
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_missing_definition_fails_and_names_location(self) -> None:
        """从树的副本中删除一个定义必须失败，并指出头文件和函数名。"""
        work = self.tmp / "repo"
        shutil.copytree(_REPO_ROOT / "include", work / "include")
        shutil.copytree(_REPO_ROOT / "src", work / "src")

        # 删除一个确实在 include/ 中声明的函数定义。
        # 门禁只检查公共声明，删除静态辅助函数仍会合法通过。
        import re
        target = work / "src" / "tokenprobe" / "tp_decimal.c"
        self.assertTrue(target.is_file(), f"fixture expects {target}")
        text = target.read_text(encoding="utf-8")
        match = re.search(
            r"\btokenprobe_decimal_is_zero\s*\([^;{}]*\)\s*\{", text)
        assert match is not None, "fixture expects tokenprobe_decimal_is_zero"
        start = text.rfind("\n", 0, match.start()) + 1
        depth = 0
        end = start
        for i in range(match.end() - 1, len(text)):
            if text[i] == "{":
                depth += 1
            elif text[i] == "}":
                depth -= 1
                if depth == 0:
                    end = i + 1
                    break
        patched = text[:start] + text[end:]
        target.write_text(patched, encoding="utf-8")

        result = _run_script(self.script, str(work))
        self.assertEqual(result.returncode, 1)
        self.assertIn("public declaration without definition", result.stderr)


class TestVersionConsistencyGate(unittest.TestCase):
    """scripts/check_version_consistency.py 的解析辅助函数必须精确。"""

    def setUp(self) -> None:
        script = _SCRIPTS_DIR / "check_version_consistency.py"
        self.assertTrue(script.is_file(), f"missing gate script: {script}")
        self.module = _load_module(script, "check_version_consistency")
        self.tmp = Path(tempfile.mkdtemp(prefix="gate-test-version-"))
        self.addCleanup(shutil.rmtree, self.tmp, ignore_errors=True)

    def test_parses_project_version(self) -> None:
        cmakelists = self.tmp / "CMakeLists.txt"
        cmakelists.write_text(
            "cmake_minimum_required(VERSION 3.21)\n"
            "project(SomeName\n    VERSION 1.2.3\n    LANGUAGES C\n)\n",
            encoding="utf-8",
        )
        parsed = self.module.read_cmake_version(cmakelists)
        self.assertEqual(parsed, ("SomeName", "1.2.3"))

    def test_rejects_missing_project_version(self) -> None:
        cmakelists = self.tmp / "CMakeLists.txt"
        cmakelists.write_text("cmake_minimum_required(VERSION 3.21)\n",
                              encoding="utf-8")
        self.assertIsNone(self.module.read_cmake_version(cmakelists))

    def test_parses_newest_changelog_section(self) -> None:
        changelog = self.tmp / "CHANGELOG.md"
        changelog.write_text(
            "# 更新日志\n\n## 0.3.0 — 2026-01-01\n\n- x\n\n## 0.2.0\n\n- y\n",
            encoding="utf-8",
        )
        self.assertEqual(self.module.read_changelog_version(changelog), "0.3.0")

    def test_reads_generated_header(self) -> None:
        header = self.tmp / "version.h"
        header.write_text(
            '#define SOMEPROJ_VERSION_STRING "4.5.6"\n', encoding="utf-8")
        self.assertEqual(self.module.read_header_version(header, "SomeProj"),
                         "4.5.6")

    def test_missing_inputs_exit_nonzero(self) -> None:
        """把门禁指向空树必须失败，不能空泛地通过。"""
        empty = self.tmp / "empty"
        empty.mkdir()
        result = _run_script(
            _SCRIPTS_DIR / "check_version_consistency.py",
            str(empty),
            "--build-dir", str(empty),
            "--exe", str(empty / "no-such-exe"),
        )
        self.assertEqual(result.returncode, 1)


class TestGitEvidenceGates(unittest.TestCase):
    """依赖 git 的门禁在没有证据时必须拒绝给出结论。

    CI 中观察到两种失败模式，必须保持修复：
    - 非 git 目录（完全没有证据）；
    - 浅克隆（历史/tag 被截断，此时“路径不存在”或“没有 tag”的判定是错误结果，
      而不是真实发现）。
    """

    def setUp(self) -> None:
        self.doc_paths = _SCRIPTS_DIR / "check_doc_paths.py"
        self.changelog_tag = _SCRIPTS_DIR / "check_changelog_tag.py"
        for script in (self.doc_paths, self.changelog_tag):
            self.assertTrue(script.is_file(), f"missing gate script: {script}")
        self.tmp = Path(tempfile.mkdtemp(prefix="gate-test-gitevidence-"))
        self.addCleanup(shutil.rmtree, self.tmp, ignore_errors=True)

    def test_non_git_directory_reports_cannot_verify(self) -> None:
        plain = self.tmp / "plain"
        plain.mkdir()
        (plain / "CHANGELOG.md").write_text(
            "# 更新日志\n\n## 1.0.0 — 2026-01-01\n\n- x\n", encoding="utf-8")
        for script in (self.doc_paths, self.changelog_tag):
            result = _run_script(script, str(plain))
            self.assertEqual(result.returncode, 1,
                             f"{script.name}: {result.stdout}{result.stderr}")
            self.assertIn("无法取证", result.stderr)

    @unittest.skipUnless(_GIT, "git not available")
    def test_shallow_clone_reports_cannot_verify(self) -> None:
        git_exec = _GIT
        assert git_exec is not None
        origin = self.tmp / "origin"
        origin.mkdir()
        (origin / "CHANGELOG.md").write_text(
            "# 更新日志\n\n## 1.0.0 — 2026-01-01\n\n- x\n", encoding="utf-8")
        (origin / "README.md").write_text("# doc\n", encoding="utf-8")

        def git(*args: str, cwd: Path) -> None:
            subprocess.run([git_exec, *args], cwd=cwd, check=True,
                           capture_output=True)

        git("init", "-q", ".", cwd=origin)
        git("config", "user.email", "probe@local", cwd=origin)
        git("config", "user.name", "probe", cwd=origin)
        git("-c", "commit.gpgsign=false", "add", "-A", cwd=origin)
        git("-c", "commit.gpgsign=false", "commit", "-q", "-m", "init",
            cwd=origin)
        git("-c", "tag.gpgsign=false", "tag", "v1.0.0", cwd=origin)

        shallow = self.tmp / "shallow"
        subprocess.run(
            [git_exec, "clone", "-q", "--depth", "1",
             origin.as_uri(), str(shallow)],
            check=True, capture_output=True)

        # 健全性检查：克隆确实是浅的，并且确实丢失了 tag。
        probe = subprocess.run(
            [git_exec, "rev-parse", "--is-shallow-repository"],
            cwd=shallow, capture_output=True, text=True, check=True)
        self.assertEqual(probe.stdout.strip(), "true")

        for script in (self.doc_paths, self.changelog_tag):
            result = _run_script(script, str(shallow))
            self.assertEqual(result.returncode, 1,
                             f"{script.name}: {result.stdout}{result.stderr}")
            self.assertIn("无法取证", result.stderr)

    @unittest.skipUnless(_repo_is_full_git_clone(),
                         "repository is not a full-history git clone")
    def test_clean_repo_verdicts_pass(self) -> None:
        """在本仓库（完整历史）中，两个门禁都应正常给出结论。"""
        for script in (self.doc_paths, self.changelog_tag):
            result = _run_script(script, str(_REPO_ROOT))
            self.assertEqual(result.returncode, 0,
                             f"{script.name}: {result.stdout}{result.stderr}")


class TestLegacyConsoleEncoding(unittest.TestCase):
    """门禁判定在非 UTF-8 控制台上也必须可读。

    Windows runner 默认 cp1252（中文 Windows 桌面为 GBK）。在脚本把输出流固定为
    UTF-8 之前，写入 stdout 的中文判定会抛 UnicodeEncodeError（门禁崩溃），
    写入 stderr 的同一判定会退化为转义序列（输出不可读）。
    PYTHONIOENCODING 在这里复现该控制台，使回归问题在任何平台都能被发现。
    """

    _LEGACY_CONSOLE = {"PYTHONIOENCODING": "cp1252"}

    def setUp(self) -> None:
        self.tmp = Path(tempfile.mkdtemp(prefix="gate-test-console-"))
        self.addCleanup(shutil.rmtree, self.tmp, ignore_errors=True)
        self.empty = self.tmp / "empty"
        self.empty.mkdir()

    def test_stdout_verdicts_survive_legacy_console(self) -> None:
        """通过的门禁会把中文判定打印到 stdout。"""
        tree = self.tmp / "docs-tree"
        tree.mkdir()
        (tree / "README.md").write_text("# doc\n", encoding="utf-8")

        result = _run_script(_SCRIPTS_DIR / "check_docs.py", str(tree),
                             "--mode", "links", env=self._LEGACY_CONSOLE)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("门禁通过", result.stdout)

    def test_stderr_verdicts_survive_legacy_console(self) -> None:
        """每个门禁都应把中文失败判定打印到 stderr。"""
        # 非 git 树：两个依赖 git 证据的门禁必须拒绝给出结论。
        non_git = self.tmp / "non-git"
        non_git.mkdir()
        (non_git / "README.md").write_text("# doc\n", encoding="utf-8")
        (non_git / "CHANGELOG.md").write_text(
            "# 更新日志\n\n## 1.0.0 — 2026-01-01\n\n- x\n", encoding="utf-8")

        version_tree = self.tmp / "version-tree"
        version_tree.mkdir()
        (version_tree / "CMakeLists.txt").write_text(
            "cmake_minimum_required(VERSION 3.21)\n"
            "project(Probe\n    VERSION 1.2.3\n    LANGUAGES C\n)\n",
            encoding="utf-8")

        cases = (
            ("check_decl_def.py", (str(self.empty),), "解析失败"),
            ("check_layers.py", (str(self.empty),), "扫描面为空"),
            ("check_doc_paths.py", (str(non_git),), "无法取证"),
            ("check_changelog_tag.py", (str(non_git),), "无法取证"),
            ("check_version_consistency.py",
             (str(version_tree),
              "--build-dir", str(self.tmp / "missing-build"),
              "--exe", str(self.tmp / "missing-exe")), "构建目录不存在"),
            ("coverage_ratchet.py",
             (str(self.tmp / "missing-build"), "--repo-root", str(self.empty)),
             "构建目录不存在"),
        )
        for name, args, expected in cases:
            with self.subTest(script=name):
                result = _run_script(_SCRIPTS_DIR / name, *args,
                                     env=self._LEGACY_CONSOLE)
                self.assertEqual(result.returncode, 1,
                                 result.stdout + result.stderr)
                self.assertIn(expected, result.stderr, result.stderr)


class TestDocGatesScanSurface(unittest.TestCase):
    """文档门禁检查的是仓库，而不是工作目录。

    本地被忽略的临时目录（``.hermes/``、``dist/``）过去也会像普通目录一样被扫描：
    一个引用了仓库中并不存在路径的计划文件，会让两个文档门禁在开发机上变红，
    而 CI（没有该目录）保持绿色。现在扫描范围是 git 的实时集合
    （已跟踪 ∪ 未跟踪且未忽略）。这些测试从两个方向锁定行为：
    被忽略文件不得影响判定，实时文件（已跟踪*或*未跟踪）仍必须被检查。
    """

    _MIN_TOKENS = 10  # check_doc_paths.py 中的扫描面下限

    def setUp(self) -> None:
        self.script_docs = _SCRIPTS_DIR / "check_docs.py"
        self.script_paths = _SCRIPTS_DIR / "check_doc_paths.py"
        for script in (self.script_docs, self.script_paths):
            self.assertTrue(script.is_file(), f"missing gate script: {script}")
        self.tmp = Path(tempfile.mkdtemp(prefix="gate-test-surface-"))
        self.addCleanup(shutil.rmtree, self.tmp, ignore_errors=True)

        self.repo = self.tmp / "repo"
        (self.repo / "docs").mkdir(parents=True)
        (self.repo / ".gitignore").write_text(".scratch/\n", encoding="utf-8")
        (self.repo / "README.md").write_text("# repo\n", encoding="utf-8")
        (self.repo / "docs" / "a.md").write_text("# a\n", encoding="utf-8")

        # 提供足够多且真实存在的反引号 token，
        # 使排除被忽略的干扰内容后仍能通过扫描面下限。
        token_lines = ["`.scratch/junk.md` is scratch"]
        for index in range(self._MIN_TOKENS + 2):
            name = f"docs/f{index:02d}.md"
            (self.repo / name).write_text("# filler\n", encoding="utf-8")
            token_lines.append(f"see `{name}`")
        self.live_doc_text = "\n".join(token_lines) + "\n"
        (self.repo / "docs" / "a.md").write_text(self.live_doc_text,
                                                 encoding="utf-8")

        # 被忽略的临时内容：损坏的相对链接 + 无法解析的 token。
        # 两者都不得影响门禁判定。
        scratch = self.repo / ".scratch"
        scratch.mkdir()
        (scratch / "junk.md").write_text(
            "# junk\n\n[dead](no-such-target.md)\n\nsee `no/such/path`\n",
            encoding="utf-8")

        self.git = _GIT
        if self.git is None:
            self.skipTest("git not available")
        for args in (("init", "-q", "."),
                     ("config", "user.email", "probe@local"),
                     ("config", "user.name", "probe")):
            subprocess.run([self.git, *args], cwd=self.repo, check=True,
                           capture_output=True)
        subprocess.run(
            [self.git, "-c", "commit.gpgsign=false", "add", "-A"],
            cwd=self.repo, check=True, capture_output=True)
        subprocess.run(
            [self.git, "-c", "commit.gpgsign=false", "commit", "-q", "-m",
             "init"],
            cwd=self.repo, check=True, capture_output=True)

    def _scratch_ignored(self) -> None:
        result = subprocess.run(
            [self.git, "check-ignore", "-q", ".scratch/junk.md"], cwd=self.repo,
            capture_output=True, text=True)
        self.assertEqual(result.returncode, 0, "fixture: .scratch/ must be ignored")

    def test_ignored_docs_do_not_flip_verdicts(self) -> None:
        self._scratch_ignored()
        for script, args in ((self.script_docs, ("--mode", "links")),
                             (self.script_paths, ())):
            with self.subTest(script=script.name):
                result = _run_script(script, str(self.repo), *args)
                self.assertEqual(result.returncode, 0,
                                 result.stdout + result.stderr)

    def test_live_tracked_doc_is_still_judged(self) -> None:
        self._scratch_ignored()
        # ``docs/`` 存在，因此该 token 会被判定
        # （不会作为安装树/构建树布局引用被跳过）。
        broken = self.live_doc_text + "\nsee `docs/gone.md`\n"
        (self.repo / "docs" / "a.md").write_text(broken, encoding="utf-8")

        result = _run_script(self.script_paths, str(self.repo))
        self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
        # 门禁使用平台路径分隔符报告仓库相对路径。
        reported = result.stderr.replace("\\", "/")
        self.assertIn("docs/a.md", reported)
        self.assertIn("docs/gone.md", reported)

    def test_live_untracked_doc_is_still_judged(self) -> None:
        """未跟踪但未忽略的文件也属于扫描范围。"""
        self._scratch_ignored()
        (self.repo / "docs" / "untracked.md").write_text(
            "# untracked\n\n[dead](no-such-target.md)\n", encoding="utf-8")

        result = _run_script(self.script_docs, str(self.repo), "--mode", "links")
        self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
        self.assertIn("untracked.md", result.stderr)
        self.assertIn("no-such-target.md", result.stderr)


class TestLayeringGate(unittest.TestCase):
    """scripts/check_layers.py 必须拒绝越过分层的依赖。"""

    def setUp(self) -> None:
        self.script = _SCRIPTS_DIR / "check_layers.py"
        self.assertTrue(self.script.is_file(), f"missing gate script: {self.script}")
        self.tmp = Path(tempfile.mkdtemp(prefix="gate-test-layers-"))
        self.addCleanup(shutil.rmtree, self.tmp, ignore_errors=True)

    def _tree(self, public_include: str = "#include <stddef.h>\n",
              provider_include: str = '#include "tp_internal.h"\n') -> Path:
        root = self.tmp / "repo"
        (root / "include" / "tokenprobe").mkdir(parents=True)
        (root / "src" / "tokenprobe").mkdir(parents=True)
        (root / "include" / "tokenprobe" / "a.h").write_text(
            public_include, encoding="utf-8")
        (root / "include" / "tokenprobe" / "b.h").write_text(
            "#include <tokenprobe/a.h>\n", encoding="utf-8")
        (root / "src" / "tokenprobe" / "tp_provider_demo.c").write_text(
            provider_include, encoding="utf-8")
        return root

    def test_clean_tree_passes(self) -> None:
        result = _run_script(self.script, str(self._tree()))
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("分层检查通过", result.stdout)

    def test_public_header_with_third_party_fails(self) -> None:
        root = self._tree(public_include="#include <openssl/ssl.h>\n")
        result = _run_script(self.script, str(root))
        self.assertEqual(result.returncode, 1)
        self.assertIn("公共头文件不得包含", result.stderr)

    def test_provider_with_os_header_fails(self) -> None:
        root = self._tree(provider_include="#include <windows.h>\n")
        result = _run_script(self.script, str(root))
        self.assertEqual(result.returncode, 1)
        self.assertIn("provider 不得包含", result.stderr)

    def test_empty_tree_fails(self) -> None:
        empty = self.tmp / "empty"
        empty.mkdir()
        result = _run_script(self.script, str(empty))
        self.assertEqual(result.returncode, 1)
        self.assertIn("扫描面为空或不足", result.stderr)


if __name__ == "__main__":
    unittest.main(verbosity=2)
