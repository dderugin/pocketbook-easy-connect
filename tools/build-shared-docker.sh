#!/usr/bin/env bash
set -euo pipefail
root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
image="${EC_DOCKER_IMAGE:-easy-connect-shared-builder:local}"
volume="${EC_DOCKER_VOLUME:-easy-connect-pocketbook-armv7-softfp-work}"
architecture=$(docker version --format '{{.Server.Arch}}')
case "$architecture" in amd64|arm64) ;; *) echo "Unsupported Docker engine architecture: $architecture" >&2; exit 1 ;; esac
platform="linux/$architecture"
builder=(docker buildx)
if [[ -n "${EC_BUILDX:-}" ]]; then builder=("$EC_BUILDX"); fi
"${builder[@]}" version >/dev/null || { echo "Docker Buildx is required for the shared SDK image" >&2; exit 1; }
"${builder[@]}" build --load --platform "$platform" --build-arg "EC_BUILD_PLATFORM=$platform" -t "$image" -f "$root/tools/Dockerfile" "$root"
docker volume create "$volume" >/dev/null
mkdir -p "$root/dist"
mounts=(--mount "type=bind,source=$root,target=/project,readonly"
        --mount "type=volume,source=$volume,target=/project/work"
        --mount "type=bind,source=$root/dist,target=/project/dist")
environment=()
if [[ -d "$root/work/downloads" ]]; then
    mounts+=(--mount "type=bind,source=$root/work/downloads,target=/daemon-sources,readonly")
    environment+=(-e EC_DEPENDENCY_CACHE=/daemon-sources)
fi
environment+=(-e "EC_HOST_UID=$(id -u)" -e "EC_HOST_GID=$(id -g)")
if [[ -n "${EC_SDK_VOLUME:-}" ]]; then
    mounts+=(--mount "type=volume,source=$EC_SDK_VOLUME,target=/shared-sdk,readonly")
    environment+=(-e POCKETBOOK_SDK=/shared-sdk/sdk/SDK-B288-6.10)
fi
if [[ -n "${EC_ARCHIVE_CACHE:-}" ]]; then
    mounts+=(--mount "type=bind,source=$EC_ARCHIVE_CACHE,target=/firmware-archives,readonly")
    environment+=(-e EC_FIRMWARE_ARCHIVES=/firmware-archives -e EC_SDK_ARCHIVE=/firmware-archives/SDK-B288-6.10.7z)
fi
if [[ -n "${EC_RUNTIME_CACHE:-}" ]]; then
    mounts+=(--mount "type=bind,source=$EC_RUNTIME_CACHE,target=/firmware-cache,readonly")
    environment+=(-e EC_RUNTIME_CACHE=/firmware-cache)
fi
docker run --rm --init --platform "$platform" "${mounts[@]}" "${environment[@]}" \
    "$image" bash /project/tools/docker-entrypoint.sh
