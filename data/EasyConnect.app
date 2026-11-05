#!/bin/sh
# Stable OTA bootstrap. Install this launcher once over USB with version 0.3+.
cd /mnt/ext1 || exit 1
share=/mnt/ext1/system/share/easy-connect
backup=/mnt/ext1/system/share/easy-connect-ota-old
# Recover power loss between the two directory renames, including rollback.
rollback=/mnt/ext1/system/share/easy-connect-ota-rollback
if [ ! -d "$share" ] && [ -f "$rollback/.ota-rollback-ready" ]; then
    mv "$rollback" "$share" || exit 1
    rm -f "$share/.ota-rollback-ready"
fi
if [ ! -d "$share" ] && [ -d "$backup" ]; then
    mv "$backup" "$share" || exit 1
fi
export GLIBC_TUNABLES="${GLIBC_TUNABLES}${GLIBC_TUNABLES:+:}glibc.rtld.execstack=2"
while :; do
    "$share/bin/easy-connect.app" >> "$share/ui.log" 2>&1
    status=$?
    if [ "$status" -eq 75 ]; then
        if ! "$share/bin/easy-connect" --root "$share" update-install >> "$share/ui.log" 2>&1; then
            # Let the interface explain/retry without an automatic restart loop.
            export EC_UPDATE_SKIP=1
        fi
        continue
    fi
    if [ -f "$share/.ota-first-run" ]; then
        if [ "$status" -ne 0 ]; then
            if "$backup/bin/easy-connect" --root "$share" update-rollback >> "$share/ui.log" 2>&1; then
                export EC_UPDATE_SKIP=1
                continue
            fi
        else
            rm -f "$share/.ota-first-run"
        fi
    fi
    exit "$status"
done
