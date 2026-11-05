#!/bin/sh
cd /mnt/ext1 || exit 1
# Match the shared reader startup workaround for executable-stack firmware libs.
export GLIBC_TUNABLES="${GLIBC_TUNABLES}${GLIBC_TUNABLES:+:}glibc.rtld.execstack=2"
exec /mnt/ext1/system/share/easy-connect/bin/easy-connect.app \
    >> /mnt/ext1/system/share/easy-connect/ui.log 2>&1
