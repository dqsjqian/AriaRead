#!/bin/bash
# Build and distribute the same runtime directory: build/bin (or bin/<config>).
# --skip-cmake refreshes assets beside an already-built executable.
# --clean uses CMake clean-first; --config selects a multi-config build.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
BUILD_DIR="${ARIAREAD_BUILD_DIR:-$PROJECT_ROOT/build}"
CONFIG=Release
SKIP_CMAKE=false
CLEAN_BUILD=false
while [ "$#" -gt 0 ]; do
    case "$1" in
        --skip-cmake) SKIP_CMAKE=true ;;
        --clean) CLEAN_BUILD=true ;;
        --config)
            [ "$#" -ge 2 ] || { echo "--config requires a value" >&2; exit 1; }
            CONFIG="$2"; shift ;;
        *) echo "Unknown option: $1" >&2; exit 1 ;;
    esac
    shift
done
if [ "$SKIP_CMAKE" = true ] && [ "$CLEAN_BUILD" = true ]; then
    echo "--clean cannot be combined with --skip-cmake" >&2
    exit 1
fi

if [ "$SKIP_CMAKE" = false ]; then
    # 依赖前缀引导：CMake 配置强制要求钉定依赖前缀（manifest 校验）。
    # 缺失时自动执行依赖脚本——幂等可续跑，已有产物按存在性跳过，
    # 只有真正缺失的组件才会下载构建。显式设置 ARIAREAD_DEPS_PREFIX
    # 时跳过引导，尊重调用方指定的前缀。
    if [ -z "${ARIAREAD_DEPS_PREFIX:-}" ] \
            && [ ! -f "$PROJECT_ROOT/build/deps/prefix/share/ariaread-deps/manifest.json" ]; then
        echo "[deps] pinned dependency prefix not found; bootstrapping via tools/ci/build_ariaread_deps.py (idempotent)..." >&2
        python3 "$PROJECT_ROOT/tools/ci/build_ariaread_deps.py"
    fi
    if [ ! -f "$BUILD_DIR/CMakeCache.txt" ]; then
        cmake -S "$PROJECT_ROOT" -B "$BUILD_DIR" -DCMAKE_BUILD_TYPE="$CONFIG" \
            -DARIAREAD_ENFORCE_SELF_CONTAINED=ON -DARIAREAD_USE_SYSTEM_CURL=OFF
    fi
    BUILD_ARGS=(--build "$BUILD_DIR" --config "$CONFIG" --target ariaread_web_server)
    if [ "$CLEAN_BUILD" = true ]; then BUILD_ARGS+=(--clean-first); fi
    NPROC="${ARIAREAD_BUILD_JOBS:-$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 4)}"
    cmake "${BUILD_ARGS[@]}" --parallel "$NPROC"
fi

RUNTIME_DIR="$BUILD_DIR/bin"
if [ -f "$RUNTIME_DIR/$CONFIG/ariaread_web_server" ]; then
    RUNTIME_DIR="$RUNTIME_DIR/$CONFIG"
fi
BINARY="$RUNTIME_DIR/ariaread_web_server"
if [ ! -x "$BINARY" ]; then
    echo "Server executable is missing: $BINARY" >&2
    exit 1
fi
if [ "$SKIP_CMAKE" = true ]; then
    cmake "-DARIAREAD_SOURCE_DIR=$PROJECT_ROOT" "-DARIAREAD_OUTPUT_DIR=$RUNTIME_DIR" \
        -P "$PROJECT_ROOT/cmake/SyncWebRuntime.cmake"
fi

# CMake supplies relative runtime search paths. Do not copy or patch the binary.
"$BINARY" --help >/dev/null
printf '\nReady: %s\nRun: "%s"\nDistribute this directory with its libraries and web/ assets.\n' "$RUNTIME_DIR" "$BINARY"
