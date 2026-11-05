#!/usr/bin/env bash
set -euo pipefail
root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
if [[ "$(uname -s)" != Linux ]]; then
    echo "The pinned PocketBook SDK requires Linux (x86_64 tools or configured x86 emulation)" >&2
    exit 1
fi
export POCKETBOOK_SDK="${POCKETBOOK_SDK:-$root/work/sdk/SDK-B288-6.10}"
if [[ ! -x "$POCKETBOOK_SDK/usr/bin/arm-obreey-linux-gnueabi-gcc" ]]; then
    echo "SDK compiler missing. See README: Build for the reader." >&2
    exit 1
fi
build="${EC_BUILD_DIR:-$root/work/build-device-shared}"
python3 "$root/tools/dependencies.py" build --sdk "$POCKETBOOK_SDK"
flags=$(python3 "$root/tools/pocketbook_platform.py" --cflags)
cmake -S "$root" -B "$build" -DCMAKE_C_FLAGS="$flags" \
    -DCMAKE_TOOLCHAIN_FILE="$root/tools/toolchain-pocketbook.cmake" \
    -DEASY_CONNECT_POCKETBOOK=ON -DEASY_CONNECT_RUNTIME_PROBES=ON \
    -DBUILD_TESTING=OFF -DCMAKE_BUILD_TYPE=Release
cmake --build "$build" --parallel "${JOBS:-4}"
readelf="$POCKETBOOK_SDK/usr/bin/arm-obreey-linux-gnueabi-readelf"
strip="$POCKETBOOK_SDK/usr/bin/arm-obreey-linux-gnueabi-strip"
report="$build/arm-report.txt"
: > "$report"
for binary in "$build/easy-connect" "$build/easy-connect.app" \
              "$root/work/stage/bin/dropbear" "$root/work/stage/bin/dropbearkey" \
              "$root/work/stage/bin/vsftpd"; do
    "$strip" "$binary"
    "$readelf" --file-header --program-headers --dynamic --arch-specific \
        --version-info --wide "$binary" >> "$report"
done
python3 "$root/tools/qualify-pocketbook.py" --build "$build" \
    --host-build "${EC_HOST_BUILD:-$root/work/build-host}" --firmware "${EC_FIRMWARE_DIR:-$root/work/firmware}"
python3 "$root/tools/package.py" --build "$build"
