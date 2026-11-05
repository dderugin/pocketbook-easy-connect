#!/usr/bin/env bash
set -euo pipefail
export LANG=C.UTF-8
cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.."
if [[ -n "${EC_SDK_ARCHIVE:-}" ]]; then
    python3 tools/sdk.py verify --archive "$EC_SDK_ARCHIVE"
else
    python3 tools/sdk.py fetch
fi
if [[ -z "${POCKETBOOK_SDK:-}" ]]; then
    if [[ ! -f work/sdk/.docker-extracted-6.10 ]]; then
        mkdir -p work/sdk
        bsdtar -xf "${EC_SDK_ARCHIVE:-work/sdk/SDK-B288-6.10.7z}" -C work/sdk
        touch work/sdk/.docker-extracted-6.10
    fi
    export POCKETBOOK_SDK="$PWD/work/sdk/SDK-B288-6.10"
fi
if [[ -n "${EC_DEPENDENCY_CACHE:-}" ]]; then
    mkdir -p work/downloads
    cp "$EC_DEPENDENCY_CACHE"/* work/downloads/
fi
python3 tools/dependencies.py fetch
export EC_HOST_BUILD="${EC_HOST_BUILD:-$PWD/work/build-host-$(uname -m)}"
cmake -S . -B "$EC_HOST_BUILD" -DCMAKE_BUILD_TYPE=Release
cmake --build "$EC_HOST_BUILD" --parallel "${JOBS:-4}"
ctest --test-dir "$EC_HOST_BUILD" --output-on-failure --output-junit ctest-results.xml
python3 tools/dependencies.py build
python3 tests/test_ssh.py "$EC_HOST_BUILD/easy-connect"
python3 tests/test_ftp.py "$EC_HOST_BUILD/easy-connect"
python3 tests/test_background.py "$EC_HOST_BUILD/easy-connect"
python3 tests/test_sessions.py "$EC_HOST_BUILD/ec-ui-test"
python3 - <<'REPORT'
from pathlib import Path
import json,sys,os
sys.path.insert(0,'tools')
from source_metadata import source_metadata
root=Path.cwd()
(Path(os.environ['EC_HOST_BUILD'])/'host-tests.json').write_text(json.dumps({'status':'passed',
    'source':source_metadata(root),'suites':['controller','core','tools','ui-layout','device-ui','shared-device','updater','ssh','ftp','background','ui-sessions'],
    'user':'non-root'},indent=2)+'\n')
REPORT
archives="${EC_FIRMWARE_ARCHIVES:-$PWD/work/downloads}"
python3 tools/firmware.py fetch --archives "$archives"
cache=()
if [[ -n "${EC_RUNTIME_CACHE:-}" ]]; then cache=(--runtime-cache "$EC_RUNTIME_CACHE"); fi
python3 tools/firmware.py prepare --archives "$archives" --output work/firmware "${cache[@]}"
bash tools/build-device.sh
