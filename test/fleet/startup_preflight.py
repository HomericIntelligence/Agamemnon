"""Verify missing API configuration fails before contacting an isolated sentinel."""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import select
import signal
import socket
import subprocess
import tempfile
import time
from pathlib import Path


def verify(binary: Path, root: Path) -> dict[str, object]:
    binary = binary.resolve(strict=True)
    if not root.is_dir() or root.is_symlink() or root.stat().st_mode & 0o077:
        raise ValueError("test root must be a private directory")
    with tempfile.TemporaryDirectory(prefix="startup-no-auth-", dir=root) as temporary:
        home = Path(temporary)
        with socket.socket() as sentinel, socket.socket() as reserved_http:
            sentinel.bind(("127.0.0.1", 0))
            sentinel.listen(8)
            reserved_http.bind(("127.0.0.1", 0))
            reserved_http.listen(1)
            endpoint = f"nats://127.0.0.1:{sentinel.getsockname()[1]}"
            environment = {
                "PATH": os.defpath,
                "HOME": str(home),
                "LANG": "C",
                "NATS_URL": endpoint,
                "GITHUB_RECONCILE_INTERVAL_SEC": "0",
                "AGAMEMNON_BIND_ADDRESS": "127.0.0.1",
                "PORT": str(reserved_http.getsockname()[1]),
            }
            started = time.monotonic()
            contacts = 0
            expired = False
            with tempfile.TemporaryFile() as stdout, tempfile.TemporaryFile() as stderr:
                process = subprocess.Popen(
                    [str(binary)],
                    cwd=home,
                    env=environment,
                    stdin=subprocess.DEVNULL,
                    stdout=stdout,
                    stderr=stderr,
                    start_new_session=True,
                )
                try:
                    while process.poll() is None:
                        remaining = started + 6 - time.monotonic()
                        if remaining <= 0:
                            expired = True
                            break
                        ready, _, _ = select.select(
                            [sentinel], [], [], min(remaining, 0.05)
                        )
                        if ready:
                            connection, _ = sentinel.accept()
                            connection.close()
                            contacts += 1
                finally:
                    if process.poll() is None:
                        os.killpg(process.pid, signal.SIGKILL)
                    process.wait(timeout=2)
                stdout.seek(0)
                stderr.seek(0)
                output = stdout.read(8192).decode("utf-8", "replace")
                errors = stderr.read(8192).decode("utf-8", "replace")
            expected_failure = "AGAMEMNON_API_KEY is not set" in errors
            passed = (
                contacts == 0
                and not expired
                and process.returncode == 1
                and expected_failure
            )
            return {
                "schema": "hi/fleet/startup-preflight-test/v1",
                "binary": str(binary),
                "binarySha256": hashlib.sha256(binary.read_bytes()).hexdigest(),
                "environmentKeys": sorted(environment),
                "brokerConnectionCount": contacts,
                "timedOut": expired,
                "exitCode": process.returncode,
                "missingApiKeyReported": expected_failure,
                "elapsedSeconds": time.monotonic() - started,
                "stdout": output,
                "stderr": errors,
                "passed": passed,
            }


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binary", required=True, type=Path)
    parser.add_argument("--root", required=True, type=Path)
    args = parser.parse_args()
    result = verify(args.binary, args.root)
    print(json.dumps(result, indent=2), flush=True)
    return 0 if result["passed"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
