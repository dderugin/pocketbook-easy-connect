#!/usr/bin/env bash
set -euo pipefail
export LANG=C.UTF-8
cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.."
python3 tools/sdk.py fetch
python3 tools/dependencies.py fetch
if [[ ! -f work/sdk/.docker-extracted-6.10 ]]; then
    bsdtar -xf work/sdk/SDK-B288-6.10.7z -C work/sdk
    touch work/sdk/.docker-extracted-6.10
fi
bash tools/build-device.sh
