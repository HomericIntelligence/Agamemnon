"""Execute image uv preparation with actual uv and tiny local wheel fixtures.

Only the image's metadata COPY and uv sync instructions are evaluated. Package
resolution, installation, lock rejection and inventory checks use the real
pinned uv executable. This does not execute a container or a vulnerability scan.
"""

import hashlib
import json
import os
import shlex
import subprocess
import sys
import tempfile
import unittest
import zipfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]


class AuditPreparation(unittest.TestCase):
    def setUp(self):
        artifacts = os.environ.get("AGAMEMNON_AUDIT_TEST_ARTIFACT_DIR")
        if artifacts:
            Path(artifacts).mkdir(parents=True, exist_ok=True)
            self.root = Path(
                tempfile.mkdtemp(prefix="agam-ci-audit-", dir=artifacts)
            ).resolve()
        else:
            self.directory = tempfile.TemporaryDirectory(prefix="agam-ci-audit-")
            self.addCleanup(self.directory.cleanup)
            self.root = Path(self.directory.name).resolve()
        self.call_index = 0
        self.source = self.root / "source"
        self.image = self.root / "image"
        self.source.mkdir()
        self.image.mkdir()
        self.environment = self.root / "environment"
        self.uv = os.environ.get("UV_AUDIT_TEST_BINARY", "uv")
        self.env = {
            key: value
            for key, value in os.environ.items()
            if not key.startswith("UV_")
            and key not in ("VIRTUAL_ENV", "PYTHONPATH", "PYTHONHOME")
        }
        self.env.update(
            UV_CACHE_DIR=str(self.root / "cache"),
            UV_PROJECT_ENVIRONMENT=str(self.environment),
            UV_PYTHON=sys.executable,
            UV_PYTHON_DOWNLOADS="never",
            UV_NO_CONFIG="1",
            UV_OFFLINE="1",
            UV_CONCURRENT_INSTALLS="1",
            UV_CONCURRENT_BUILDS="1",
            UV_CONCURRENT_DOWNLOADS="1",
        )
        version = self.invoke(["--version"])
        self.assertEqual(version.returncode, 0, version.stderr)
        self.assertTrue(version.stdout.startswith("uv 0.12.1 "), version.stdout)
        root_wheel = self.wheel("root_sentinel")
        audit_wheel = self.wheel("audit_sentinel")
        self.write(
            "pyproject.toml",
            '[project]\nname="root-fixture"\nversion="0.1.0"\n'
            'requires-python=">=3.11"\n[dependency-groups]\n'
            'dev=["root-sentinel==1.0"]\n[tool.uv]\npackage=false\n'
            f'[tool.uv.sources]\nroot-sentinel={{path="{root_wheel}"}}\n',
        )
        self.write(
            "clients/python/pyproject.toml",
            '[project]\nname="client-fixture"\nversion="0.1.0"\n'
            'requires-python=">=3.11"\n[dependency-groups]\n'
            'dev=["orchestration-fixture"]\nlint=["audit-sentinel==1.0"]\n'
            "[tool.uv]\npackage=false\n[tool.uv.sources]\n"
            f'audit-sentinel={{path="{audit_wheel}"}}\n'
            'orchestration-fixture={path="../../agamemnon",editable=true}\n',
        )
        self.write(
            "agamemnon/pyproject.toml",
            '[project]\nname="orchestration-fixture"\nversion="0.1.0"\n'
            'requires-python=">=3.11"\n[tool.uv]\npackage=false\n',
        )
        for project in (self.source, self.source / "clients/python"):
            result = self.invoke(["lock", "--project", str(project)])
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def write(self, name, text):
        path = self.source / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(text)

    def wheel(self, name):
        directory = self.root / "wheels"
        directory.mkdir(exist_ok=True)
        path = directory / f"{name}-1.0-py3-none-any.whl"
        info = f"{name}-1.0.dist-info"
        with zipfile.ZipFile(path, "w") as archive:
            archive.writestr(
                info + "/METADATA",
                f"Metadata-Version: 2.1\nName: {name.replace('_', '-')}\nVersion: 1.0\n",
            )
            archive.writestr(
                info + "/WHEEL",
                "Wheel-Version: 1.0\nRoot-Is-Purelib: true\nTag: py3-none-any\n",
            )
            archive.writestr(info + "/RECORD", "")
        return path

    def invoke(self, arguments):
        argv = [self.uv, *arguments]
        result = subprocess.run(
            argv,
            cwd=self.image,
            env=self.env,
            text=True,
            capture_output=True,
            timeout=15,
            check=False,
        )
        prefix = self.root / f"call-{self.call_index:02d}"
        self.call_index += 1
        prefix.with_suffix(".stdout").write_text(result.stdout)
        prefix.with_suffix(".stderr").write_text(result.stderr)
        prefix.with_suffix(".json").write_text(
            json.dumps(
                {
                    "argv": argv,
                    "cwd": str(self.image),
                    "exitCode": result.returncode,
                    "stdoutSha256": hashlib.sha256(result.stdout.encode()).hexdigest(),
                    "stderrSha256": hashlib.sha256(result.stderr.encode()).hexdigest(),
                },
                indent=2,
            )
            + "\n"
        )
        return result

    def prepare_image(self):
        """Evaluate only metadata copies and actual uv sync commands in order."""
        logical = (ROOT / "ci/Containerfile").read_text().replace("\\\n", " ")
        results = []
        for line in logical.splitlines():
            tokens = shlex.split(line, comments=True)
            if not tokens:
                continue
            if tokens[0] == "COPY" and not tokens[1].startswith("--"):
                sources, destination = tokens[1:-1], tokens[-1]
                if not all(
                    Path(item).name in ("pyproject.toml", "uv.lock") for item in sources
                ):
                    continue
                target = self.image / destination
                self.assertTrue(target.resolve().is_relative_to(self.image))
                target.mkdir(parents=True, exist_ok=True)
                for relative in sources:
                    source = self.source / relative
                    self.assertTrue(source.resolve().is_relative_to(self.source))
                    (target / source.name).write_bytes(source.read_bytes())
            elif tokens[:3] == ["RUN", "uv", "sync"]:
                result = self.invoke(tokens[2:])
                results.append(result)
                if result.returncode:
                    return result, results
        self.assertTrue(results, "the image must execute its root uv preparation")
        return results[-1], results

    def test_image_prepares_audit_without_removing_root_packages(self):
        result, _ = self.prepare_image()
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        # The later container mounts the complete work checkout and selects its
        # client project while retaining the prepared image environment.
        ready = self.invoke(
            [
                "sync",
                "--check",
                "--locked",
                "--inexact",
                "--only-group",
                "lint",
                "--project",
                str(self.source / "clients/python"),
            ]
        )
        self.assertEqual(ready.returncode, 0, ready.stdout + ready.stderr)
        inventory = self.invoke(
            [
                "pip",
                "list",
                "--python",
                str(self.environment / "bin/python"),
                "--format",
                "json",
            ]
        )
        self.assertEqual(inventory.returncode, 0, inventory.stderr)
        self.assertEqual(
            {item["name"]: item["version"] for item in json.loads(inventory.stdout)},
            {"root-sentinel": "1.0", "audit-sentinel": "1.0"},
        )

    def test_image_rejects_changed_client_metadata_without_rewriting_lock(self):
        metadata = self.source / "clients/python/pyproject.toml"
        metadata.write_text(
            metadata.read_text().replace('version="0.1.0"', 'version="0.2.0"')
        )
        locked = (self.source / "clients/python/uv.lock").read_bytes()
        result, _ = self.prepare_image()
        self.assertNotEqual(
            result.returncode, 0, "stale client lock was not checked during preparation"
        )
        self.assertIn("lockfile", result.stderr.lower())
        self.assertEqual((self.image / "clients/python/uv.lock").read_bytes(), locked)


if __name__ == "__main__":
    unittest.main()
