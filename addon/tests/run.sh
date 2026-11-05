#!/usr/bin/env bash
# Builds and runs the offline shot-engine tests (no game needed). Same zig / SWOFTY_SDK rules as build.sh.
set -euo pipefail
cd "$(dirname "$0")/.."
if ! command -v zig >/dev/null 2>&1; then
    export PATH="$LOCALAPPDATA/Microsoft/WinGet/Packages/zig.zig_Microsoft.Winget.Source_8wekyb3d8bbwe/zig-x86_64-windows-0.17.0:$PATH"
fi
SDK="${SWOFTY_SDK:-../../SwoftyLauncher-HotSwap/packages/sdk/include}"
export ZIG_GLOBAL_CACHE_DIR="$PWD/native/build/zig-cache" ZIG_LOCAL_CACHE_DIR="$PWD/native/build/zig-cache"
mkdir -p native/build
zig c++ -target x86_64-windows-gnu -std=c++23 -O2 -Wall -Wextra -I"$SDK" \
    -o native/build/seq_test.exe tests/seq_test.cpp
./native/build/seq_test.exe
