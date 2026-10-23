#!/usr/bin/env bash
set -euo pipefail
root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
if [[ "$(uname -s)/$(uname -m)" != Linux/x86_64 ]]; then
    echo "The pinned PocketBook SDK requires a Linux x86_64 build host" >&2
    exit 1
fi
export POCKETBOOK_SDK="${POCKETBOOK_SDK:-$root/work/sdk/SDK-B288-6.10}"
if [[ ! -x "$POCKETBOOK_SDK/usr/bin/arm-obreey-linux-gnueabi-gcc" ]]; then
    echo "SDK compiler missing. See README: Build for the reader." >&2
    exit 1
fi
python3 "$root/tools/dependencies.py" build --sdk "$POCKETBOOK_SDK"
cmake -S "$root" -B "$root/build-device" \
    -DCMAKE_TOOLCHAIN_FILE="$root/tools/toolchain-touch-hd3.cmake" \
    -DEASY_CONNECT_POCKETBOOK=ON -DBUILD_TESTING=OFF -DCMAKE_BUILD_TYPE=Release
cmake --build "$root/build-device" --parallel
# Record the actual target ABI and runtime requirements for firmware comparison.
readelf="$POCKETBOOK_SDK/usr/bin/arm-obreey-linux-gnueabi-readelf"
report="$root/build-device/arm-report.txt"
: > "$report"
for binary in "$root/build-device/easy-connect" "$root/build-device/easy-connect.app" \
              "$root/work/stage/bin/dropbear" "$root/work/stage/bin/dropbearkey" \
              "$root/work/stage/bin/vsftpd"; do
    printf '\nFile: %s\n' "$(basename "$binary")" >> "$report"
    "$readelf" --file-header --program-headers --dynamic --arch-specific \
        --version-info --wide "$binary" >> "$report"
    if "$readelf" --dynamic "$binary" | grep -Eq 'NEEDED.*lib(crypto|ssl)\.'; then
        echo "Unexpected shared OpenSSL dependency: $binary" >&2
        exit 1
    fi
done
python3 "$root/tools/package.py" --build "$root/build-device"
