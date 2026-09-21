"""Launch only an isolated loopback broker, then run the compiled transport tests."""

import json
import os
import subprocess
import sys
import tempfile
import time
from pathlib import Path

with tempfile.TemporaryDirectory(prefix="agamemnon-jetstream-") as directory:
    root = Path(directory)
    with (root / "broker.log").open("wb") as output:
        broker = subprocess.Popen(
            [
                sys.argv[1],
                "-a",
                "127.0.0.1",
                "-p",
                "-1",
                "-js",
                "-sd",
                str(root / "storage"),
                "--ports_file_dir",
                directory,
            ],
            stdout=output,
            stderr=subprocess.STDOUT,
        )
        try:
            ports = root / f"nats-server_{broker.pid}.ports"
            for _ in range(100):
                if ports.exists():
                    break
                if broker.poll() is not None:
                    raise RuntimeError("private broker failed to start")
                time.sleep(0.05)
            data = json.loads(ports.read_text())
            urls = data["nats"]
            url = urls[0]
            if not url.startswith("nats://127.0.0.1:"):
                raise RuntimeError("broker did not bind loopback")
            env = {"PATH": os.defpath, "LANG": "C", "AGAMEMNON_TEST_NATS_URL": url}
            result = subprocess.run(sys.argv[2:], env=env, timeout=30, check=False)
            sys.exit(result.returncode)
        finally:
            broker.terminate()
            try:
                broker.wait(timeout=5)
            except subprocess.TimeoutExpired:
                broker.kill()
                broker.wait(timeout=5)
