# Local CI image and architecture

`ci/Containerfile` builds the local validation image from the pinned Ubuntu 24.04
base. Its release-binary installer uses `dpkg --print-architecture` inside that
image and accepts only `amd64` and `arm64`. It does not default an unknown
architecture to amd64 or request emulation. The compiler and system libraries
come from the existing Ubuntu package policy; Python build tools use the root
`uv.lock`. These policies are distinct from the seven checksum-pinned binaries.

The image also prepares the Python client's locked `lint` dependency group in
the existing `/opt/agamemnon-venv`. Its inexact synchronization retains the root
toolchain packages so the unchanged security command uses the prepared shared
environment. Image inputs include `clients/python/pyproject.toml`, its `uv.lock`,
and `agamemnon/pyproject.toml`, whose relative editable source is recorded in the
client lock. Changes to any of these inputs require a new image qualification.
The separate `uv tool` installation of pip-audit does not provision this group.

Each local CI step runs in a disposable container. Installed packages and UV
cache changes from an earlier step are not shared with later steps. Preparing
the audit group in the image avoids repeating that acquisition in the security
step; it does not remove the scanners' need to access advisory services. Audit
findings and acquisition or service failures remain fatal.

## Release inputs

The native installer retains these versions and their existing amd64 checksums.
Both architectures have explicit SHA-256 values in `ci/install-tool.sh`.

| Tool | Version | Official release provenance |
| --- | --- | --- |
| uv | 0.12.1 | [Release asset digests](https://github.com/astral-sh/uv/releases/tag/0.12.1) |
| NATS Server | 2.10.24 | [SHA256SUMS](https://github.com/nats-io/nats-server/releases/download/v2.10.24/SHA256SUMS) |
| actionlint | 1.7.7 | [Checksums](https://github.com/rhysd/actionlint/releases/download/v1.7.7/actionlint_1.7.7_checksums.txt) |
| Gitleaks | 8.30.1 | [Release asset digests](https://github.com/gitleaks/gitleaks/releases/tag/v8.30.1) |
| Trivy | 0.69.3 | [Release asset digests](https://github.com/aquasecurity/trivy/releases/tag/v0.69.3) |
| Syft | 1.4.1 | [Checksums](https://github.com/anchore/syft/releases/download/v1.4.1/syft_1.4.1_checksums.txt) |
| Grype | 0.87.0 | [Checksums](https://github.com/anchore/grype/releases/download/v0.87.0/grype_0.87.0_checksums.txt) |

The installer verifies each downloaded archive before creating the destination
or extracting only the requested executable members. Unsupported inputs and
download, checksum, or extraction failures stop installation. Temporary download
directories are removed when the installer exits. NATS Server is used only by
the existing CTest launcher, which creates a loopback broker with private storage.

## Run validation

1. Use a clean, dedicated checkout with self-contained Git metadata on the
   intended Linux architecture. The local runner mounts this checkout and writes
   build and scanner outputs there. A worktree whose `.git` file points outside
   the mount does not provide the history that the secrets scan needs.
   The generated `gitleaks.sarif` and `conan-sbom.cdx.json` reports have ignore
   rules that apply only at the checkout root. Retain their bytes with the CI
   evidence; their presence is not a tracked source change.
2. Run the controlled CI contracts with an existing Python interpreter:

   ```bash
   just ci-tools-test python3
   just ci-reports-test python3
   just ci-audit-test python3 /path/to/uv-0.12.1
   ```

   These tests execute the image's uv installation command and the installer
   against private OS/download boundaries. They cover both architecture mappings,
   real checksum rejection, failure status and cleanup. Positive download and
   extraction acknowledgments are fixtures; they do not prove Linux binary or
   image execution. The report tests use private Git repositories to check that
   root reports leave source status unchanged while nested files and tracked
   source changes remain visible. They do not run scanners.
   The audit test executes the image's metadata-copy and uv-sync instructions
   with actual uv 0.12.1 and tiny private wheel fixtures, with network access
   disabled. It checks audit readiness, retention of a root package, and stale
   client-lock rejection. This verifies resolver behavior rather than the real
   Linux dependency inventory or a vulnerability scan. Set
   `AGAMEMNON_AUDIT_TEST_ARTIFACT_DIR` to retain its fixture files and every uv
   invocation/output for review.
3. Build the actual image with the existing Podman recipe:

   ```bash
   just ci-build
   ```

4. Run the canonical local container suite:

   ```bash
   just ci-check
   ```

   This runs lint (including actionlint), lock checks, build, unit tests,
   integration tests, security, secrets, workflow schema, version consistency and
   release dry-run checks. Required durable transport tests launch their own
   broker; missing or incompatible `nats-server` must fail. Scanner severities
   and failure policies remain unchanged by the architecture correction.

The current recipes do not set CPU/memory limits or forward host build-job
limits into containers. On a shared development host, the operator must use the
approved bounded engine wrapper; setting a host environment variable alone is
not proof of a container limit. Retain the exact wrapper, source, image identity,
commands and raw results with the validation receipt.

`just ci` is a CMake-only path, and `just fleet-test` is a focused native target.
Neither replaces `just ci-check`. The existing local suite does not include every
separate hosted job: packaging, clean-install checks, Markdown validation and
complete Python suites need their own applicable receipts. A successful local
image build or controlled installer test does not establish full local or hosted
CI success, Fleet admission, provider authentication, or cluster acceptance.
