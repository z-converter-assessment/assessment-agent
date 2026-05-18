#!/bin/sh
# build-linux.sh — release-grade Linux build inside manylinux2014 container.
#
# Host requirement: **Docker only**. No yum/apt install on the host. The
# container is self-contained: build-prep.sh installs perl/cmake inside,
# then vendor-fetch + vendor-build + USE_VENDORED=1 release builds everything,
# then chown's the outputs (vendor/, dist/) back to the invoking user.
#
# Usage (from repo root):
#   ./scripts/build-linux.sh
#
# Output: dist/assessment-agent-linux-x86_64 + dist/SHA256SUMS
#         (verify passed automatically by `make release`)
#
# NOTE on Apple Silicon: this image is linux/amd64. ARM hosts run it under
# Rosetta/QEMU emulation — works but slow (10x), and the binary is amd64
# emulated. Prefer running on a native amd64 build host (CI / EC2 / VM)
# for release artifacts that go to production.

set -eu

cd "$(dirname "$0")/.."

if ! command -v docker >/dev/null 2>&1; then
	echo "[build-linux] docker not found. Install Docker Desktop / Docker Engine first." >&2
	exit 1
fi
if ! docker info >/dev/null 2>&1; then
	echo "[build-linux] docker daemon not running. Start it and retry." >&2
	exit 1
fi

UID_HOST=$(id -u)
GID_HOST=$(id -g)

docker run --rm \
    --platform linux/amd64 \
    -v "$(pwd)":/src -w /src \
    quay.io/pypa/manylinux2014_x86_64 \
    bash -lc "
        set -e
        bash scripts/build-prep.sh
        make vendor-fetch
        make vendor-build
        make USE_VENDORED=1 release
        chown -R ${UID_HOST}:${GID_HOST} vendor dist 2>/dev/null || true
    "

echo
echo "[build-linux] OK — dist artifacts:"
ls -la dist/
