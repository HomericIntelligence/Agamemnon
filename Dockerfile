# syntax=docker/dockerfile:1
FROM ubuntu:24.04@sha256:c4a8d5503dfb2a3eb8ab5f807da5bc69a85730fb49b5cfca2330194ebcc41c7b AS builder

RUN --mount=type=cache,target=/var/cache/apt,sharing=locked \
    --mount=type=cache,target=/var/lib/apt,sharing=locked \
    apt-get update && apt-get install -y --no-install-recommends \
    cmake \
    ninja-build \
    make \
    g++ \
    git \
    ca-certificates \
    libssl-dev \
    python3 \
    python3-pip \
    && rm -rf /var/lib/apt/lists/*

RUN --mount=type=cache,target=/root/.cache/pip \
    pip3 install --break-system-packages conan && conan profile detect

WORKDIR /src

# Copy Conan files first for dependency caching.
COPY conanfile.py ./
COPY conan/ conan/
RUN --mount=type=cache,target=/root/.conan2/p,sharing=locked \
    conan install . \
    --output-folder=build \
    --profile=conan/profiles/default \
    --build=missing

# Copy CMake configuration so FetchContent (nats.c) can be cached separately.
COPY CMakeLists.txt ./
COPY CMakePresets.json ./
COPY cmake/ cmake/

# Copy source tree.
COPY include/ include/
COPY src/ src/
COPY test/ test/

RUN cmake -B build -G Ninja \
    -DCMAKE_TOOLCHAIN_FILE=build/conan_toolchain.cmake \
    -DCMAKE_BUILD_TYPE=Release \
    -DProjectAgamemnon_BUILD_TESTING=OFF \
    -DProjectAgamemnon_ENABLE_CLANG_TIDY=OFF \
    -DProjectAgamemnon_ENABLE_CPPCHECK=OFF \
    -DProjectAgamemnon_WARNINGS_AS_ERRORS=OFF \
    && cmake --build build --target ProjectAgamemnon_server ProjectAgamemnon_healthcheck

# ── Runtime image ─────────────────────────────────────────────────────────────
FROM debian:12-slim@sha256:f9c6a2fd2ddbc23e336b6257a5245e31f996953ef06cd13a59fa0a1df2d5c252

RUN --mount=type=cache,target=/var/cache/apt,sharing=locked \
    --mount=type=cache,target=/var/lib/apt,sharing=locked \
    apt-get update && apt-get install -y --no-install-recommends \
    libssl3 \
    && rm -rf /var/lib/apt/lists/*

COPY --from=builder /src/build/ProjectAgamemnon_server /usr/local/bin/ProjectAgamemnon_server
COPY --from=builder /src/build/ProjectAgamemnon_healthcheck /usr/local/bin/ProjectAgamemnon_healthcheck

EXPOSE 8080

ENV NATS_URL=nats://localhost:4222
ENV PORT=8080
ENV SERVER_THREAD_COUNT=8
ENV SERVER_READ_TIMEOUT_SEC=10
ENV SERVER_WRITE_TIMEOUT_SEC=10
ENV SERVER_REQUEST_SIZE_LIMIT_MB=4
ENV NATS_STREAM_MAX_BYTES_MB=50
ENV NATS_STREAM_MAX_AGE_SEC=3600

HEALTHCHECK --interval=10s --timeout=3s --start-period=5s --retries=3 \
    CMD ["/usr/local/bin/ProjectAgamemnon_healthcheck"]

RUN useradd -r -s /usr/sbin/nologin agamemnon
USER agamemnon

CMD ["ProjectAgamemnon_server"]
