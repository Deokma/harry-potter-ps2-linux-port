#!/bin/sh
# Runs ON THE DEVICE as root, once per boot: lets the bench scripts log in
# with an SSH key on Knulli / Batocera.
#
# Why: root's home there is /userdata/system on an exFAT (fuseblk)
# partition, so it is world-writable and dropbear refuses
# ~/.ssh/authorized_keys. This build has no option to read keys from
# elsewhere.
#
# What: start a second dropbear on port 2222 in its own mount namespace,
# where /etc/passwd gives root a home on tmpfs (/run/ps2port-home, mode
# 700) holding a copy of /userdata/system/.ssh/authorized_keys. Outside
# that namespace nothing changes; the normal SSH on port 22 keeps
# running, and everything here is gone after a reboot.
#
# From Windows PowerShell (you type only your device password):
#   cmd /c "ssh root@<ip> sh -s < scripts\device_ssh_setup.sh"
set -eu
PORT=${PORT:-2222}
H=/run/ps2port-home
KEYS=/userdata/system/.ssh/authorized_keys
PIDF=/run/ps2port-dropbear.pid

command -v unshare >/dev/null || { echo "ERROR: no unshare on this firmware"; exit 1; }
[ -s "$KEYS" ] || { echo "ERROR: $KEYS is missing or empty"; exit 1; }

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

unshare -m sh -c "mount --bind /run/ps2port-passwd /etc/passwd && exec /usr/sbin/dropbear -p $PORT -W 10485760 -R -P $PIDF"
sleep 1
if [ -f "$PIDF" ] && kill -0 "$(cat "$PIDF")" 2>/dev/null; then
    echo "OK: key-login SSH on port $PORT (pid $(cat "$PIDF")), home $H"
else
    echo "ERROR: dropbear on port $PORT did not stay up"
    exit 1
fi
