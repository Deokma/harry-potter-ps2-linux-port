#!/bin/sh
# Finds where eegen's code makes a boot go wrong, in WSL from the repo root. Each try boots the x86 runner with eegen's functions entered
# through the table only for addresses from LOW up to a limit. Functions it translated still call each other directly, so the address found
# is one that reaches the bug, not necessarily the bug.
#   sh runner/bisect.sh LOW HIGH SECONDS PAD PASS_REGEX
# PASS_REGEX is matched against the last [game] line. Writes progress to private/third_party/build-m2/bisect.log.
set -u
low=$(($1))
high=$(($2))
secs=$3
pad=$4
pass=$5
log=private/third_party/build-m2/bisect.log
: > "$log"
try() {
    PS2PORT_EG_FROM=$(printf '%x' "$low") PS2PORT_EG_TO=$(printf '%x' "$1") sh runner/try.sh "$secs" "$pad" PS2PORT_EG_FROM=$(printf '%x' "$low") PS2PORT_EG_TO=$(printf '%x' "$1") > /tmp/bisect-try.txt 2>&1
    line=$(grep -a 'scene=' /tmp/bisect-try.txt | tail -1 | cut -c1-60)
    if echo "$line" | grep -Eq "$pass"; then r=pass; else r=fail; fi
    echo "$(printf '%x' "$1") $r $line" >> "$log"
    [ "$r" = pass ]
}
# invariant: low..lo passes, low..hi fails
lo=$low
hi=$high
while [ $((hi - lo)) -gt 4 ]; do
    mid=$(((lo + hi) / 2))
    if try "$mid"; then lo=$mid; else hi=$mid; fi
done
echo "first failure with the limit at $(printf '%x' "$hi"), passes at $(printf '%x' "$lo")" >> "$log"
