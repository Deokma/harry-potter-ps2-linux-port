#!/bin/sh
# Runs ON THE DEVICE as root, once per boot: lets the bench scripts log in
# with an SSH key on Knulli / Batocera.
#
# Why: root's home there is /userdata/system on an exFAT (fuseblk)
# partition, so it is world-writable and dropbear refuses
# ~/.ssh/authorized_keys. This build has no option to read keys from
# elsewhere, and Knulli has no `unshare`.
#
# What: mirror the root file system (rbind) into /run/ps2port-root, shadow
# etc/passwd there with a copy whose root home is /run/ps2port-home
# (tmpfs, mode 700, holding a copy of /userdata/system/.ssh/
# authorized_keys), and run a second dropbear on port 2222 chrooted into
# that mirror. Paths outside /run/ps2port-root are untouched; the normal
# SSH on port 22 keeps running; everything is gone after a reboot.
#
# From Windows PowerShell (you type only your device password):
#   cmd /c "ssh root@<ip> sh -s < scripts\device_ssh_setup.sh"
set -eu
PORT=${PORT:-2222}
H=/run/ps2port-home
R=/run/ps2port-root
KEYS=/userdata/system/.ssh/authorized_keys
PIDF=/run/ps2port-dropbear.pid

for tool in chroot mount sed tr; do
    command -v "$tool" >/dev/null || { echo "ERROR: no $tool on this firmware"; exit 1; }
done
[ -s "$KEYS" ] || { echo "ERROR: $KEYS is missing or empty"; exit 1; }
is_mounted() { grep -q " $1 " /proc/mounts; }

# restart cleanly if it is already running
if [ -f "$PIDF" ] && kill -0 "$(cat "$PIDF")" 2>/dev/null; then
    kill "$(cat "$PIDF")"
    sleep 1
fi

mkdir -p "$H/.ssh"
chmod 700 "$H" "$H/.ssh"
tr -d '\r' < "$KEYS" > "$H/.ssh/authorized_keys"
chmod 600 "$H/.ssh/authorized_keys"
# same root entry, home field (6th) replaced
sed "s#^\(root:[^:]*:[^:]*:[^:]*:[^:]*:\)[^:]*:#\1$H:#" /etc/passwd > /run/ps2port-passwd

mkdir -p "$R"
if ! is_mounted "$R"; then
    mount -o rbind / "$R" || { echo "ERROR: mount -o rbind / failed"; exit 1; }
fi
if ! is_mounted "$R/etc/passwd"; then
    mount -o bind /run/ps2port-passwd "$R/etc/passwd" \
        || { echo "ERROR: could not shadow etc/passwd in $R"; exit 1; }
fi
# /run inside the mirror is the same tmpfs, so the pid file and the home
# directory are visible at the same paths on both sides
chroot "$R" /usr/sbin/dropbear -p "$PORT" -W 10485760 -R -P "$PIDF"
sleep 1
if [ -f "$PIDF" ] && kill -0 "$(cat "$PIDF")" 2>/dev/null; then
    echo "OK: key-login SSH on port $PORT (pid $(cat "$PIDF")), home $H"
    grep '^root:' /etc/passwd | cut -d: -f6 | sed 's/^/system root home unchanged: /'
else
    echo "ERROR: dropbear on port $PORT did not stay up"
    exit 1
fi
