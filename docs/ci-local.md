# Local CI image and architecture

`ci/Containerfile` builds the local validation image from the pinned Ubuntu 24.04
base. Its release-binary installer uses `dpkg --print-architecture` inside that
image and accepts only `amd64` and `arm64`. It does not default an unknown
architecture to amd64 or request emulation. The compiler and system libraries
come from the existing Ubuntu package policy; Python build tools use the root
`uv.lock`. These policies are distinct from the seven checksum-pinned binaries.

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
2. Run the controlled installer contracts with an existing Python interpreter:

   ```bash
   just ci-tools-test python3
   ```

   These tests execute the image's uv installation command and the installer
   against private OS/download boundaries. They cover both architecture mappings,
   real checksum rejection, failure status and cleanup. Positive download and
   extraction acknowledgments are fixtures; they do not prove Linux binary or
   image execution.
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
