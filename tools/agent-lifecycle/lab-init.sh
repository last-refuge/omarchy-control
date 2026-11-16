#!/bin/sh
set -eu

# binfmt registrations and several sysctls affect the Docker VM, even when PID
# and mount namespaces are private. Require an explicitly read-only source and
# cover this container's /proc/sys before starting any systemd services.
test -d /host-proc-sys/fs
mount --bind /host-proc-sys /proc/sys
mount -o remount,bind,ro /proc/sys
exec /sbin/init
