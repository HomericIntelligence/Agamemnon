FROM mcr.microsoft.com/devcontainers/base:ubuntu-24.04

# Install development tools and dependencies.
# The system compiler (build-essential) and the OpenSSL / libcurl dev headers
# come from apt; the CMake/Ninja/Conan/gcovr build toolchain comes from uv
# (ADR-018), so they are NOT installed here.
RUN apt-get update && apt-get install -y --no-install-recommends \
    curl \
    wget \
    git \
    build-essential \
    pkg-config \
    ca-certificates \
    gnupg \
    ssh-client \
    clang-tools-19 \
    clang-tidy \
    clang-format \
    libssl-dev \
    libcurl4-openssl-dev \
    && rm -rf /var/lib/apt/lists/*

# Install uv (manages the CMake/Ninja/Conan/gcovr build toolchain as locked wheels)
RUN curl -fsSL https://astral.sh/uv/install.sh | sh

# Install just
RUN curl --proto '=https' --tlsv1.2 -sSf https://just.systems/install.sh | bash -s -- --to /usr/local/bin

# Ensure uv is in PATH for shell initialization
RUN echo 'export PATH="$HOME/.local/bin:$PATH"' >> /etc/bash.bashrc

WORKDIR /workspace
