# syntax=docker/dockerfile:1

# uv provides the CMake / Ninja / Conan build toolchain as locked wheels
# (Odysseus ADR-018). Pinned by digest for reproducibility. This is a podman-safe
# named stage — the `COPY --from=uv` below lifts only the static `uv` binary.
FROM ghcr.io/astral-sh/uv:0.12.2@sha256:069a51314a7bb6031777a9273205fe1b0b19e914ef418207d1338b268df641dd AS uv

FROM ubuntu:26.04@sha256:678c6550cc43645e08669028bc177f50be4e7c5b8cca677067b1914d4afc7a03 AS builder

# The system compiler (g++) and OpenSSL dev headers come from apt; the
# CMake / Ninja / Conan toolchain comes from uv (see the `uv` stage above), so
# they are NOT apt-installed here.
RUN --mount=type=cache,target=/var/cache/apt,sharing=locked \
    --mount=type=cache,target=/var/lib/apt,sharing=locked \
    apt-get update && apt-get install -y --no-install-recommends \
    make \
    g++ \
    git \
    ca-certificates \
    libssl-dev \
    python3 \
    && rm -rf /var/lib/apt/lists/*

# Bring in the uv binary from the pinned named stage (it lives at the image
# root `/uv` in the distroless astral-sh/uv image).
COPY --from=uv /uv /usr/local/bin/uv

# Install the build toolchain as uv tools whose entry points land on
# /usr/local/bin, so the bare `conan` / `cmake` / `ninja` invocations below (and
# the `cmake` that conan shells out to when building gtest from source) all
# resolve. Then detect a default conan profile.
ENV UV_TOOL_BIN_DIR=/usr/local/bin
RUN --mount=type=cache,target=/root/.cache/uv \
    uv tool install cmake \
    && uv tool install ninja \
    && uv tool install conan \
    && conan profile detect

WORKDIR /src

# Copy Conan files first for dependency caching.
COPY conanfile.py ./
COPY conan/ conan/
# NOTE: do NOT put the conan package store (/root/.conan2) on a
# --mount=type=cache. That mount is ephemeral per build, but `conan install`
# also writes build/*.cmake (e.g. OpenSSL-Target-release.cmake) into the image
# layer with absolute package-lib paths baked in (/root/.conan2/p/<hash>/lib/
# libssl.a). When buildx cache-hits this layer on a fresh runner, the build/
# cmake files come back from the layer cache but the mounted package store is
# empty, so cmake configure aborts: "Library 'ssl' not found in package".
# Baking the resolved packages into the layer keeps them consistent with the
# generated cmake files. (conanfile.py is COPYed just above, so this layer is
# still cache-invalidated only when dependencies actually change.)
RUN conan install . \
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
    -DAgamemnon_BUILD_TESTING=OFF \
    -DAgamemnon_ENABLE_CLANG_TIDY=OFF \
    -DAgamemnon_ENABLE_CPPCHECK=OFF \
    -DAgamemnon_WARNINGS_AS_ERRORS=OFF \
    && cmake --build build --target Agamemnon_server Agamemnon_healthcheck

# ── Runtime image ─────────────────────────────────────────────────────────────
FROM debian:13-slim@sha256:3a39a0592364683e6bab97937b72cad5a8fa6dcbbee90edb3bb48c7f8e94f258

# NOTE (issue #279): NO libssl3 here. Conan links OpenSSL statically
# (openssl/3.6.2 with default shared=False -> libssl.a/libcrypto.a baked into
# Agamemnon_server), so neither the server nor the healthcheck binary has a
# runtime NEEDED entry for libssl3/libcrypto.so.3 — verified empirically:
#   ldd Agamemnon_server      | grep -E 'libssl|libcrypto'   # empty
#   ldd Agamemnon_healthcheck | grep -E 'libssl|libcrypto'   # empty
# Do NOT re-add libssl3 unless the Conan profile switches to shared OpenSSL.

COPY --from=builder /src/build/Agamemnon_server /usr/local/bin/Agamemnon_server
COPY --from=builder /src/build/Agamemnon_healthcheck /usr/local/bin/Agamemnon_healthcheck

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
    CMD ["/usr/local/bin/Agamemnon_healthcheck"]

RUN useradd -r -s /usr/sbin/nologin agamemnon
USER agamemnon

CMD ["Agamemnon_server"]
