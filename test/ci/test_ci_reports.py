"""Keep generated scanner reports separate from real source changes in Git."""

import os
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
REPORTS = ("gitleaks.sarif", "conan-sbom.cdx.json")


class ScannerReportStatus(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory(prefix="agam-ci-reports-")
        self.addCleanup(self.directory.cleanup)
        private = Path(self.directory.name)
        self.root = private / "repository"
        self.root.mkdir()
        home = private / "home"
        home.mkdir()
        template = private / "empty-template"
        template.mkdir()
        self.git_program = shutil.which("git")
        self.assertIsNotNone(self.git_program)
        self.env = {
            "PATH": os.defpath,
            "HOME": str(home),
            "XDG_CONFIG_HOME": str(home),
            "GIT_CONFIG_GLOBAL": os.devnull,
            "GIT_CONFIG_NOSYSTEM": "1",
            "GIT_TERMINAL_PROMPT": "0",
            "GIT_OPTIONAL_LOCKS": "0",
            "LANG": "C",
        }
        self.git("init", "--quiet", f"--template={template}")
        (self.root / ".gitignore").write_bytes((ROOT / ".gitignore").read_bytes())
        (self.root / "source.txt").write_text("original source\n")
        self.git("add", ".gitignore", "source.txt")

    def git(self, *arguments):
        result = subprocess.run(
            [self.git_program, *arguments],
            cwd=self.root,
            env=self.env,
            capture_output=True,
            text=True,
            timeout=5,
            check=False,
        )
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        return result.stdout

    def test_root_scanner_reports_do_not_change_git_status(self):
        before = self.git("status", "--porcelain=v1", "--untracked-files=all")
        for report in REPORTS:
            with self.subTest(report=report):
                path = self.root / report
                path.write_text('{"controlledReport":true}\n')
                try:
                    self.assertEqual(
                        self.git("status", "--porcelain=v1", "--untracked-files=all"),
                        before,
                    )
                    self.assertTrue(path.is_file())
                finally:
                    path.unlink()

    def test_nested_reports_and_unrelated_files_remain_visible(self):
        paths = [*(f"src/{report}" for report in REPORTS), "other.sarif", "source.json"]
        for name in paths:
            path = self.root / name
            path.parent.mkdir(exist_ok=True)
            path.write_text("controlled source\n")
        actual = self.git("ls-files", "--others", "--exclude-standard", "-z")
        self.assertEqual(set(actual.rstrip("\0").split("\0")), set(paths))

    def test_tracked_source_changes_remain_visible_with_reports_present(self):
        for report in REPORTS:
            (self.root / report).write_text('{"controlledReport":true}\n')
        (self.root / "source.txt").write_text("changed source\n")
        self.assertEqual(self.git("diff", "--name-only", "--"), "source.txt\n")


if __name__ == "__main__":
    unittest.main()
