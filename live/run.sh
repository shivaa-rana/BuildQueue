#!/bin/sh
set -eu

if [ "$#" -gt 1 ] || { [ "$#" -eq 1 ] && [ "$1" != "--build-only" ]; }; then
    printf 'Usage: %s [--build-only]\n' "$0" >&2
    exit 2
fi

cd "$(dirname "$0")"
if ! command -v cmake >/dev/null 2>&1 ||
   ! command -v c++ >/dev/null 2>&1 ||
   [ ! -x /usr/bin/cksum ] ||
   [ ! -x /usr/bin/prlimit ]; then
    printf 'Install the desktop build tools: sudo apt install build-essential cmake qt6-base-dev coreutils util-linux\n' >&2
    exit 1
fi

cmake -S . -B build-qt -DBUILDQUEUE_QT=ON
cmake --build build-qt --target buildqueue-qt --parallel 2

if [ "$#" -eq 0 ]; then
    exec ./build-qt/buildqueue-qt
fi
