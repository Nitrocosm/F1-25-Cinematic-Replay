#!/usr/bin/env bash
# Builds native/cinecam.dll with zig (0.17) against the Swofty SDK headers.
#   SWOFTY_SDK  the SDK's include folder (default: ../../SwoftyLauncher-HotSwap/packages/sdk/include)
#   zig         taken from PATH, else from its WinGet folder
set -euo pipefail
cd "$(dirname "$0")"
if ! command -v zig >/dev/null 2>&1; then
    export PATH="$LOCALAPPDATA/Microsoft/WinGet/Packages/zig.zig_Microsoft.Winget.Source_8wekyb3d8bbwe/zig-x86_64-windows-0.17.0:$PATH"
fi
SDK="${SWOFTY_SDK:-../../SwoftyLauncher-HotSwap/packages/sdk/include}"
if [ ! -f "$SDK/swofty/mixin.hpp" ]; then
    echo "Swofty SDK headers not found at $SDK: set SWOFTY_SDK to the SDK's include folder" >&2
    exit 1
fi
export ZIG_GLOBAL_CACHE_DIR="$PWD/native/build/zig-cache" ZIG_LOCAL_CACHE_DIR="$PWD/native/build/zig-cache"
mkdir -p native/build
zig c++ -target x86_64-windows-gnu -std=c++23 -O2 -Wall -Wextra -I"$SDK" -shared -s \
    -o native/cinecam.dll native/src/cinecam.cpp -lole32 -ld3d11 -ldxgi -lshell32 -luuid
rm -f native/cinecam.lib native/cinecam.pdb
echo "built native/cinecam.dll"
