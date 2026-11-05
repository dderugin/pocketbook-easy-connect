#!/usr/bin/env bash
set -euo pipefail
uid="${EC_HOST_UID:-1000}"
gid="${EC_HOST_GID:-1000}"
if [[ "$uid" == 0 ]]; then
    echo 'Use a non-root build account so rootless daemon tests actually run' >&2
    exit 1
fi
if ! getent group "$gid" >/dev/null; then groupadd --gid "$gid" ec-build; fi
if ! getent passwd "$uid" >/dev/null; then
    useradd --uid "$uid" --gid "$gid" --create-home --shell /bin/bash ec-build
fi
chown -R "$uid:$gid" /project/work /project/dist
exec runuser --user "$(getent passwd "$uid" | cut -d: -f1)" -- bash /project/tools/build-docker.sh
