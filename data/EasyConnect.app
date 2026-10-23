#!/bin/sh
cd /mnt/ext1 || exit 1
exec /mnt/ext1/system/share/easy-connect/bin/easy-connect.app \
    >> /mnt/ext1/system/share/easy-connect/ui.log 2>&1
