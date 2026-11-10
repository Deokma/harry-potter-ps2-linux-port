#!/bin/sh
# Puts the port on the device as a Knulli "Ports" entry (runner/knulli/HarryPotterPS2.sh). Run from Git Bash in the repo root,
# with nothing running on the device:   sh scripts/install_port.sh
# Copies private/third_party/build-m2/aarch64/runner-aarch64 and the launcher; the game's own files must already be on the
# device (GAME_DIR in the launcher, default /userdata/ps2bench/game). Then refresh the game list on the device (Start ->
# Game settings -> Update gamelists) and start "HarryPotterPS2" under Ports.
set -eu
here=$(cd "$(dirname "$0")" && pwd)
repo=$(cd "$here/.." && pwd)
. "$here/env.sh"
host=${BENCH_HOST:?set BENCH_HOST in .env}
ssh_opts="${BENCH_SSH_OPTS:-} -o ConnectTimeout=10"
scp_opts=$(echo "${BENCH_SSH_OPTS:-}" | sed 's/-p /-P /')
bin="$repo/private/third_party/build-m2/aarch64/runner-aarch64"
[ -f "$bin" ] || { echo "no $bin: build it first (sh runner/build.sh aarch64, then strip)"; exit 1; }
# shellcheck disable=SC2086
if ssh $ssh_opts "$host" "pidof runner-aarch64 >/dev/null"; then
    echo "BUSY: the game is running on the device; quit it first"
    exit 75
fi
# shellcheck disable=SC2086
ssh $ssh_opts "$host" "mkdir -p /userdata/roms/ports/HarryPotterPS2 && test -f /userdata/ps2bench/game/SLUS_208.26 || echo 'note: no SLUS_208.26 in /userdata/ps2bench/game: set GAME_DIR in the launcher'"
# shellcheck disable=SC2086
scp -q $scp_opts "$bin" "$host:/userdata/roms/ports/HarryPotterPS2/runner-aarch64"
# shellcheck disable=SC2086
scp -q $scp_opts "$repo/runner/knulli/HarryPotterPS2.sh" "$host:/userdata/roms/ports/HarryPotterPS2.sh"
# shellcheck disable=SC2086
ssh $ssh_opts "$host" "chmod +x /userdata/roms/ports/HarryPotterPS2.sh /userdata/roms/ports/HarryPotterPS2/runner-aarch64 && md5sum /userdata/roms/ports/HarryPotterPS2/runner-aarch64 | cut -c1-8"
echo "installed: refresh the game list on the device, then Ports -> HarryPotterPS2"
