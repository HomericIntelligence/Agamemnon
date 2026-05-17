FROM mcr.microsoft.com/devcontainers/base:ubuntu-24.04

# Install development tools and dependencies
RUN apt-get update && apt-get install -y --no-install-recommends \
    curl \
    wget \
    git \
    build-essential \
    cmake \
    pkg-config \
    ca-certificates \
    gnupg \
    ssh-client \
    clang-tools-19 \
    && rm -rf /var/lib/apt/lists/*

# Install pixi
RUN curl -fsSL https://pixi.sh/install.sh | bash

# Install just
RUN curl --proto '=https' --tlsv1.2 -sSf https://just.systems/install.sh | bash -s -- --to /usr/local/bin

# Install conan
RUN curl -fsSL https://github.com/conan-io/conan/releases/download/2.6.0/conan-2.6.0-glibc2.29-x86_64.tgz \
    | tar -xz -C /usr/local/bin/

# Ensure pixi is in PATH for shell initialization
RUN echo 'export PATH="$HOME/.pixi/bin:$PATH"' >> /etc/bash.bashrc

WORKDIR /workspace
