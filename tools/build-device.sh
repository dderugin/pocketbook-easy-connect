#!/usr/bin/env bash
set -euo pipefail
root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
: "${POCKETBOOK_SDK:?Set POCKETBOOK_SDK to SDK-B288-6.10}"
python3 "$root/tools/dependencies.py" build --sdk "$POCKETBOOK_SDK"
cmake -S "$root" -B "$root/build-device" \
    -DCMAKE_TOOLCHAIN_FILE="$root/tools/toolchain-touch-hd3.cmake" \
    -DEASY_CONNECT_POCKETBOOK=ON -DBUILD_TESTING=OFF -DCMAKE_BUILD_TYPE=Release
cmake --build "$root/build-device" --parallel
python3 "$root/tools/package.py" --build "$root/build-device"
