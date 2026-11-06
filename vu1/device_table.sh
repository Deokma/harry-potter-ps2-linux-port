#!/bin/sh
# Runs on the device, from scripts/device_run.sh: VU1 cost on the saved frames, one thread on one core.
#   sh device_table.sh [check]
# With check, also compares with the interpreter on the device's own floats (slow: minutes).
cd "$(dirname "$0")" || exit 1
tar xof vu1-frames.tar
chmod +x vu1play-aarch64
export VU1PLAY_CORE=${VU1_CORE:-2}
for d in slot2 i01; do
    echo "--- $d"
    ./vu1play-aarch64 "$d" --mode native --passes 12 | cut -c1-150
done
if [ "${1:-}" = check ]; then
    for d in slot2 i01; do
        echo "--- $d interpreter and check"
        ./vu1play-aarch64 "$d" --mode interp --passes 2 | cut -c1-150
        ./vu1play-aarch64 "$d" --mode check --passes 2 | grep '^check: [0-9]* of\|^call' | tail -3 | cut -c1-150
    done
fi
rm -rf slot2 i01
