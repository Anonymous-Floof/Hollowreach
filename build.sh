#!/bin/sh
# Hollowreach native build for Linux and macOS.
#
# Windows uses build.bat, which additionally has to locate the MSVC toolchain.
# Here cmake and a compiler are expected on PATH, as they normally are.
#
#   ./build.sh                 configure + build (RelWithDebInfo)
#   ./build.sh debug           configure + build Debug
#   ./build.sh release         configure + build Release
#   ./build.sh run             build then launch
#   ./build.sh clean           delete the build directory
#   ./build.sh package         build then produce the release zip in ../dist
#
# Anything after -- is forwarded to the game when running.

set -eu
cd "$(dirname "$0")"

CONFIG=RelWithDebInfo
ACTION=build
PASSTHROUGH=""

while [ $# -gt 0 ]; do
  case "$1" in
    debug) CONFIG=Debug ;;
    release) CONFIG=Release ;;
    reldeb) CONFIG=RelWithDebInfo ;;
    run) ACTION=run ;;
    clean) ACTION=clean ;;
    package) ACTION=package ;;
    --)
      shift
      PASSTHROUGH="$*"
      break
      ;;
    *)
      echo "Unknown argument: $1" >&2
      echo "Usage: ./build.sh [debug|release|reldeb] [run|clean|package] [-- game args]" >&2
      exit 1
      ;;
  esac
  shift
done

BUILD_DIR="build/$CONFIG"

if [ "$ACTION" = clean ]; then
  rm -rf build
  echo "Removed build directory."
  exit 0
fi

if ! command -v cmake >/dev/null 2>&1; then
  echo "ERROR: cmake is not on PATH." >&2
  echo "  Debian/Ubuntu: sudo apt install cmake ninja-build build-essential \\" >&2
  echo "                 libgl1-mesa-dev xorg-dev libasound2-dev" >&2
  echo "  Fedora:        sudo dnf install cmake ninja-build gcc-c++ \\" >&2
  echo "                 mesa-libGL-devel libX11-devel libXrandr-devel \\" >&2
  echo "                 libXinerama-devel libXcursor-devel libXi-devel alsa-lib-devel" >&2
  echo "  macOS:         brew install cmake ninja" >&2
  exit 1
fi

GENERATOR="Unix Makefiles"
if command -v ninja >/dev/null 2>&1; then
  GENERATOR=Ninja
fi

if [ ! -f "$BUILD_DIR/CMakeCache.txt" ]; then
  echo "=== Configuring $CONFIG ==="
  cmake -S . -B "$BUILD_DIR" -G "$GENERATOR" -DCMAKE_BUILD_TYPE="$CONFIG"
fi

echo "=== Building $CONFIG ==="
# Leave one core free so an interactive session stays responsive.
JOBS=$( (command -v nproc >/dev/null 2>&1 && nproc) || sysctl -n hw.ncpu 2>/dev/null || echo 4 )
JOBS=$(( JOBS > 1 ? JOBS - 1 : 1 ))
cmake --build "$BUILD_DIR" --config "$CONFIG" --parallel "$JOBS"

EXE="$BUILD_DIR/bin/Hollowreach"
if [ ! -x "$EXE" ]; then
  echo "ERROR: expected $EXE but it was not produced." >&2
  exit 1
fi
echo "=== Built $EXE ==="

case "$ACTION" in
  package)
    echo "=== Packaging ==="
    cmake --build "$BUILD_DIR" --config "$CONFIG" --target package
    ;;
  run)
    echo "=== Running ==="
    # Unquoted on purpose: PASSTHROUGH holds already-split arguments.
    # shellcheck disable=SC2086
    exec "$EXE" $PASSTHROUGH
    ;;
esac
