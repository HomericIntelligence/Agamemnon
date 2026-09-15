"""Run the real image installer with private OS/download boundary fixtures."""

import json
import os
import shlex
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
# Official release asset identities. Positive download/digest/extract results are
# fixtures; the rejection tests hash the actual controlled bytes independently.
ASSETS = {
    "amd64": {
        "uv": (
            "uv-x86_64-unknown-linux-gnu.tar.gz",
            "90b2f223fb69d19db49e117da601f64978593417988530aa733d456141b4bcbb",
        ),
        "nats-server": (
            "nats-server-v2.10.24-linux-amd64.tar.gz",
            "ee6500f364e3a741b496ae0296c04f2a9d53bbaabac457104ac74596b4a59d85",
        ),
        "actionlint": (
            "actionlint_1.7.7_linux_amd64.tar.gz",
            "023070a287cd8cccd71515fedc843f1985bf96c436b7effaecce67290e7e0757",
        ),
        "gitleaks": (
            "gitleaks_8.30.1_linux_x64.tar.gz",
            "551f6fc83ea457d62a0d98237cbad105af8d557003051f41f3e7ca7b3f2470eb",
        ),
        "trivy": (
            "trivy_0.69.3_Linux-64bit.tar.gz",
            "1816b632dfe529869c740c0913e36bd1629cb7688bd5634f4a858c1d57c88b75",
        ),
        "syft": (
            "syft_1.4.1_linux_amd64.tar.gz",
            "5e4c6a0d1ca28d25e060a29c7cca0aedc50d951bfb270b45bc9a71e86ac6fbe2",
        ),
        "grype": (
            "grype_0.87.0_linux_amd64.tar.gz",
            "be710d15f5477e5c77ce03d14e480263415d7ab135e04b8483663f688823087d",
        ),
    },
    "arm64": {
        "uv": (
            "uv-aarch64-unknown-linux-gnu.tar.gz",
            "769d373e146692c639b5fbaae33b331c297a32e03d30448772051902df52bbf4",
        ),
        "nats-server": (
            "nats-server-v2.10.24-linux-arm64.tar.gz",
            "a4ae6c46ef545a13a3214bc35696b2806e05b60742f7ed5b2082d3c2f5af854f",
        ),
        "actionlint": (
            "actionlint_1.7.7_linux_arm64.tar.gz",
            "401942f9c24ed71e4fe71b76c7d638f66d8633575c4016efd2977ce7c28317d0",
        ),
        "gitleaks": (
            "gitleaks_8.30.1_linux_arm64.tar.gz",
            "e4a487ee7ccd7d3a7f7ec08657610aa3606637dab924210b3aee62570fb4b080",
        ),
        "trivy": (
            "trivy_0.69.3_Linux-ARM64.tar.gz",
            "7e3924a974e912e57b4a99f65ece7931f8079584dae12eb7845024f97087bdfd",
        ),
        "syft": (
            "syft_1.4.1_linux_arm64.tar.gz",
            "a28d63bb2bca96092a1a42cd5afdd0787633ae05998935a5e6e2aac8f2e2ec44",
        ),
        "grype": (
            "grype_0.87.0_linux_arm64.tar.gz",
            "3c64dc19d0dab8a1ab30860c9f5167383088d009528054b3854c56aac3574948",
        ),
    },
}


class NativeTools(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory(prefix="agam-ci-tools-")
        self.addCleanup(self.directory.cleanup)
        self.root = Path(self.directory.name)
        self.bin = self.root / "bin"
        self.bin.mkdir()
        self.tmp = self.root / "tmp"
        self.tmp.mkdir()
        self.calls = self.root / "calls.jsonl"
        fixture = self.bin / "fixture"
        fixture.write_text(
            f"#!{sys.executable}\n"
            + """import hashlib, json, os, pathlib, sys
name = pathlib.Path(sys.argv[0]).name
args = sys.argv[1:]
with open(os.environ["TOOL_CALLS"], "a") as output:
    output.write(json.dumps([name, *args]) + "\\n")
if name == "dpkg":
    assert args == ["--print-architecture"]
    print(os.environ["TOOL_ARCH"])
elif name == "curl":
    if os.environ.get("DOWNLOAD_FAILURE") == "1":
        sys.exit(47)
    pathlib.Path(args[args.index("-o") + 1]).write_bytes(b"controlled bytes\\n")
elif name == "sha256sum":
    digest, archive = sys.stdin.read().split()
    with open(os.environ["TOOL_CALLS"], "a") as output:
        output.write(json.dumps(["checksum", digest, archive]) + "\\n")
    if os.environ.get("REAL_CHECKSUM") == "1":
        actual = hashlib.sha256(pathlib.Path(archive).read_bytes()).hexdigest()
        sys.exit(0 if actual == digest else 1)
elif name == "tar" and os.environ.get("EXTRACTION_FAILURE") == "1":
    sys.exit(48)
elif name == "rm" and os.environ.get("IMAGE_STAGE") != "1":
    os.execv("/bin/rm", ["/bin/rm", *args])
"""
        )
        fixture.chmod(0o755)
        for name in ("dpkg", "curl", "sha256sum", "tar", "apt-get", "rm"):
            (self.bin / name).symlink_to(fixture)
        self.env = {
            "PATH": f"{self.bin}:{os.defpath}",
            "LANG": "C",
            "TMPDIR": str(self.tmp),
            "TOOL_CALLS": str(self.calls),
            "TOOL_ARCH": "arm64",
        }

    def recorded(self):
        return [json.loads(line) for line in self.calls.read_text().splitlines()]

    def invoke(self, tool, **extra):
        self.calls.write_text("")
        return subprocess.run(
            [
                "/bin/bash",
                str(ROOT / "ci/install-tool.sh"),
                tool,
                str(self.root / "destination with spaces"),
            ],
            env={**self.env, **extra},
            text=True,
            capture_output=True,
            timeout=10,
            check=False,
        )

    def check_assets(self, architecture):
        for tool, (asset, digest) in ASSETS[architecture].items():
            with self.subTest(tool=tool, architecture=architecture):
                result = self.invoke(tool, TOOL_ARCH=architecture)
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                calls = self.recorded()
                download = next(c for c in calls if c[0] == "curl")
                self.assertTrue(any(arg.endswith("/" + asset) for arg in download))
                checksum = next(c for c in calls if c[0] == "checksum")
                self.assertEqual(checksum[1], digest)
                extract = next(c for c in calls if c[0] == "tar")
                self.assertLess(calls.index(checksum), calls.index(extract))
                if tool in ("uv", "nats-server"):
                    self.assertIn("--strip-components=1", extract)
                    self.assertIn(asset.removesuffix(".tar.gz") + "/" + tool, extract)
                    if tool == "uv":
                        self.assertIn(asset.removesuffix(".tar.gz") + "/uvx", extract)
                else:
                    self.assertEqual(extract[-1], tool)
                self.assertEqual(list(self.tmp.iterdir()), [])

    def test_native_amd64_preserves_existing_versions_and_checksums(self):
        self.check_assets("amd64")

    def test_native_arm64_uses_official_matching_releases(self):
        self.check_assets("arm64")

    def test_unknown_architecture_fails_before_download_or_destination(self):
        result = self.invoke("uv", TOOL_ARCH="riscv64")
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("Unsupported CI tool architecture: riscv64", result.stderr)
        self.assertEqual([c[0] for c in self.recorded()], ["dpkg"])
        self.assertFalse((self.root / "destination with spaces").exists())

    def test_unknown_tool_fails_before_download_or_destination(self):
        result = self.invoke("unknown")
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("Unsupported CI tool: unknown", result.stderr)
        self.assertEqual([c[0] for c in self.recorded()], ["dpkg"])
        self.assertFalse((self.root / "destination with spaces").exists())

    def test_download_failure_preserves_status_and_cleans_temporary_files(self):
        result = self.invoke("uv", DOWNLOAD_FAILURE="1")
        self.assertEqual(result.returncode, 47)
        self.assertFalse(any(c[0] in ("checksum", "tar") for c in self.recorded()))
        self.assertFalse((self.root / "destination with spaces").exists())
        self.assertEqual(list(self.tmp.iterdir()), [])

    def test_actual_wrong_sha256_prevents_every_tool_destination_write(self):
        for tool in ASSETS["arm64"]:
            with self.subTest(tool=tool):
                result = self.invoke(tool, REAL_CHECKSUM="1")
                self.assertNotEqual(result.returncode, 0)
                self.assertTrue(any(c[0] == "checksum" for c in self.recorded()))
                self.assertFalse(any(c[0] == "tar" for c in self.recorded()))
                self.assertFalse((self.root / "destination with spaces").exists())
                self.assertEqual(list(self.tmp.iterdir()), [])

    def test_extraction_failure_preserves_status_and_cleans_temporary_files(self):
        result = self.invoke("nats-server", EXTRACTION_FAILURE="1")
        self.assertEqual(result.returncode, 48)
        self.assertTrue(any(c[0] == "checksum" for c in self.recorded()))
        self.assertEqual(list(self.tmp.iterdir()), [])

    def test_image_uv_install_selects_native_arm64(self):
        # Execute the actual RUN, with only package/download/filesystem boundaries
        # controlled. No engine, network call, or host system path is writable.
        text = (ROOT / "ci/Containerfile").read_text().replace("\\\n", " ")
        steps = [line[4:] for line in text.splitlines() if line.startswith("RUN ")]
        selected = [
            step
            for step in steps
            if "uv/releases/download/" in step or "install-tool.sh uv " in step
        ]
        self.assertEqual(len(selected), 1)
        step = selected[0].replace(
            "/opt/ci/install-tool.sh", shlex.quote(str(ROOT / "ci/install-tool.sh"))
        )
        step = step.replace("/usr/local/bin", shlex.quote(str(self.root / "image-bin")))
        step = step.replace("/tmp/uv.tar.gz", shlex.quote(str(self.tmp / "uv.tar.gz")))
        result = subprocess.run(
            ["/bin/bash", "-c", step],
            env={**self.env, "IMAGE_STAGE": "1"},
            text=True,
            capture_output=True,
            timeout=10,
            check=False,
        )
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        downloads = [
            arg for call in self.recorded() if call[0] == "curl" for arg in call
        ]
        self.assertIn(
            "https://github.com/astral-sh/uv/releases/download/0.12.1/"
            "uv-aarch64-unknown-linux-gnu.tar.gz",
            downloads,
        )
        self.assertFalse(any("uv-x86_64" in arg for arg in downloads))


if __name__ == "__main__":
    unittest.main()
