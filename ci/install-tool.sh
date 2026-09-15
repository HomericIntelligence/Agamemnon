#!/bin/bash
# Install a pinned CI binary for the Debian image's native architecture.
# Official release provenance and the remaining CI gates: docs/ci-local.md.
set -euo pipefail

tool="${1:?tool is required}"
destination="${2:?destination is required}"
architecture="$(dpkg --print-architecture)"
case "$architecture" in
    amd64|arm64) ;;
    *) echo "Unsupported CI tool architecture: $architecture" >&2; exit 1 ;;
esac

case "$tool:$architecture" in
    uv:amd64)
        repository=astral-sh/uv; version=0.12.1
        asset=uv-x86_64-unknown-linux-gnu.tar.gz
        checksum=90b2f223fb69d19db49e117da601f64978593417988530aa733d456141b4bcbb ;;
    uv:arm64)
        repository=astral-sh/uv; version=0.12.1
        asset=uv-aarch64-unknown-linux-gnu.tar.gz
        checksum=769d373e146692c639b5fbaae33b331c297a32e03d30448772051902df52bbf4 ;;
    nats-server:amd64)
        repository=nats-io/nats-server; version=v2.10.24
        asset=nats-server-v2.10.24-linux-amd64.tar.gz
        checksum=ee6500f364e3a741b496ae0296c04f2a9d53bbaabac457104ac74596b4a59d85 ;;
    nats-server:arm64)
        repository=nats-io/nats-server; version=v2.10.24
        asset=nats-server-v2.10.24-linux-arm64.tar.gz
        checksum=a4ae6c46ef545a13a3214bc35696b2806e05b60742f7ed5b2082d3c2f5af854f ;;
    actionlint:amd64)
        repository=rhysd/actionlint; version=v1.7.7
        asset=actionlint_1.7.7_linux_amd64.tar.gz
        checksum=023070a287cd8cccd71515fedc843f1985bf96c436b7effaecce67290e7e0757 ;;
    actionlint:arm64)
        repository=rhysd/actionlint; version=v1.7.7
        asset=actionlint_1.7.7_linux_arm64.tar.gz
        checksum=401942f9c24ed71e4fe71b76c7d638f66d8633575c4016efd2977ce7c28317d0 ;;
    gitleaks:amd64)
        repository=gitleaks/gitleaks; version=v8.30.1
        asset=gitleaks_8.30.1_linux_x64.tar.gz
        checksum=551f6fc83ea457d62a0d98237cbad105af8d557003051f41f3e7ca7b3f2470eb ;;
    gitleaks:arm64)
        repository=gitleaks/gitleaks; version=v8.30.1
        asset=gitleaks_8.30.1_linux_arm64.tar.gz
        checksum=e4a487ee7ccd7d3a7f7ec08657610aa3606637dab924210b3aee62570fb4b080 ;;
    trivy:amd64)
        repository=aquasecurity/trivy; version=v0.69.3
        asset=trivy_0.69.3_Linux-64bit.tar.gz
        checksum=1816b632dfe529869c740c0913e36bd1629cb7688bd5634f4a858c1d57c88b75 ;;
    trivy:arm64)
        repository=aquasecurity/trivy; version=v0.69.3
        asset=trivy_0.69.3_Linux-ARM64.tar.gz
        checksum=7e3924a974e912e57b4a99f65ece7931f8079584dae12eb7845024f97087bdfd ;;
    syft:amd64)
        repository=anchore/syft; version=v1.4.1
        asset=syft_1.4.1_linux_amd64.tar.gz
        checksum=5e4c6a0d1ca28d25e060a29c7cca0aedc50d951bfb270b45bc9a71e86ac6fbe2 ;;
    syft:arm64)
        repository=anchore/syft; version=v1.4.1
        asset=syft_1.4.1_linux_arm64.tar.gz
        checksum=a28d63bb2bca96092a1a42cd5afdd0787633ae05998935a5e6e2aac8f2e2ec44 ;;
    grype:amd64)
        repository=anchore/grype; version=v0.87.0
        asset=grype_0.87.0_linux_amd64.tar.gz
        checksum=be710d15f5477e5c77ce03d14e480263415d7ab135e04b8483663f688823087d ;;
    grype:arm64)
        repository=anchore/grype; version=v0.87.0
        asset=grype_0.87.0_linux_arm64.tar.gz
        checksum=3c64dc19d0dab8a1ab30860c9f5167383088d009528054b3854c56aac3574948 ;;
    *) echo "Unsupported CI tool: $tool" >&2; exit 1 ;;
esac

archive_directory="$(mktemp -d)"
trap 'rm -rf -- "$archive_directory"' EXIT
archive="$archive_directory/$asset"
curl --fail --silent --show-error --location --retry 5 --retry-all-errors \
    --connect-timeout 15 --max-time 180 \
    "https://github.com/$repository/releases/download/$version/$asset" -o "$archive"
printf '%s  %s\n' "$checksum" "$archive" | sha256sum --check
mkdir -p "$destination"
case "$tool" in
    uv)
        member_root="${asset%.tar.gz}"
        tar xzf "$archive" -C "$destination" --strip-components=1 \
            "$member_root/uv" "$member_root/uvx"
        ;;
    nats-server)
        tar xzf "$archive" -C "$destination" --strip-components=1 \
            "${asset%.tar.gz}/nats-server"
        ;;
    *) tar xzf "$archive" -C "$destination" "$tool" ;;
esac
