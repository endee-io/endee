#!/usr/bin/env bash
# Build only the ndd-migrate-v0-to-v2 target via the project CMake. Configures
# the build directory if it doesn't exist yet. Override BUILD_DIR to use a
# different location.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
BUILD_DIR="${BUILD_DIR:-$REPO_ROOT/build-migrator}"

if [[ ! -f "$BUILD_DIR/CMakeCache.txt" ]]; then
    # The top-level configure gates x86 builds on a SIMD flag (server uses it
    # for distance kernels). The migrator doesn't, but the gate still fires -
    # pick a baseline if the caller didn't specify one.
    extra_args=()
    case "$(uname -m)" in
        x86_64|amd64)
            if [[ " $* " != *" -DUSE_AVX"* ]]; then
                extra_args+=(-DUSE_AVX2=ON)
            fi
            ;;
        aarch64|arm64)
            if [[ " $* " != *" -DUSE_NEON"* && " $* " != *" -DUSE_SVE2"* ]]; then
                extra_args+=(-DUSE_NEON=ON)
            fi
            ;;
    esac
    cmake -S "$REPO_ROOT" -B "$BUILD_DIR" "${extra_args[@]}" "$@"
fi

cmake --build "$BUILD_DIR" --target ndd-migrate-v0-to-v2 -j

echo "built: $BUILD_DIR/ndd-migrate-v0-to-v2"
