#!/usr/bin/env bash
# Copy the same native emulator used by the Docker build, without running the
# binfmt installer or altering host registration. Intended for native Linux CI.
set -euo pipefail
root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
destination="${1:?Usage: fetch-qemu.sh DESTINATION}"
image=$(awk '/^FROM .* AS emulator$/ {print $3}' "$root/tools/Dockerfile")
[[ "$image" == tonistiigi/binfmt@sha256:* ]]
container=$(docker create "$image")
trap 'docker rm "$container" >/dev/null' EXIT
mkdir -p "$(dirname -- "$destination")"
docker cp "$container:/usr/bin/qemu-arm" "$destination"
chmod 755 "$destination"
