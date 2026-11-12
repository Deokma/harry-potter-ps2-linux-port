#!/bin/sh
# Gives a git worktree of this repo its own private build inputs, so several worktrees can build and run the PC runner at once.
# Usage (WSL, from the worktree's root):  sh scripts/worker_sandbox.sh /mnt/d/Projects/anbernic/PS2Recompiler
# Makes, under the worktree's git-ignored private/:
#   third_party/PS2Recomp      a full copy of the main checkout's (with its uncommitted changes), committed there as the tag
#                              sandbox-base: `git -C private/third_party/PS2Recomp diff --ignore-cr-at-eol sandbox-base` is this
#                              worktree's runtime change
#   third_party/build-m2/{gen,x86}   copies (objects keep their times, so only what changes is rebuilt)
#   third_party/trial/         a real directory of links to the main one's entries, without boot-*: this worktree's runs land here
#   everything else            links to the main checkout's private/ (read-only use: disc files, recordings, FFmpeg, vu1 frames)
# Tracked files identical to the main checkout's get its file times, or runner/build.sh would rebuild everything.
set -eu
main=$(cd "$1" && pwd)
here=$(pwd)
[ "$main" != "$here" ] || { echo "run from a worktree, not the main checkout" >&2; exit 2; }
[ -f "$here/runner/build.sh" ] || { echo "not a repo root: $here" >&2; exit 2; }
[ -d "$main/private/third_party/PS2Recomp" ] || { echo "no private tree in $main" >&2; exit 2; }
[ ! -e "$here/private" ] || { echo "private/ already exists here" >&2; exit 2; }
mkdir -p private/third_party/trial private/third_party/build-m2
for e in "$main"/private/*; do
    n=$(basename "$e")
    [ "$n" = third_party ] || ln -s "$e" "private/$n"
done
for e in "$main"/private/third_party/*; do
    n=$(basename "$e")
    case $n in
        PS2Recomp|build-m2|trial|build-fast|build-runtime|build-recomp*) ;;
        *) ln -s "$e" "private/third_party/$n" ;;
    esac
done
for e in "$main"/private/third_party/trial/*; do
    n=$(basename "$e")
    case $n in
        boot-*) ;;
        *) ln -s "$e" "private/third_party/trial/$n" ;;
    esac
done
cp -a "$main/private/third_party/PS2Recomp" private/third_party/PS2Recomp
cp -a "$main/private/third_party/build-m2/gen" "$main/private/third_party/build-m2/x86" private/third_party/build-m2/
# the batch lists name generated files by absolute path: point them at this worktree's copies, keeping the lists' times
for l in private/third_party/build-m2/x86/fun/list_*.cpp private/third_party/build-m2/x86/eg/list_*.cpp; do
    sed -i "s|$main/private/third_party/|$here/private/third_party/|" "$l"
    touch -r "$main/$l" "$l"
done
g="git -C private/third_party/PS2Recomp -c user.name=sandbox -c user.email=sandbox@localhost -c core.autocrlf=false -c core.safecrlf=false"
$g commit -q -a -m "sandbox baseline (the main checkout's uncommitted state)" --no-verify
$g tag -f sandbox-base >/dev/null
# (not `git ls-files`: WSL's git cannot read a worktree made by Windows git)
find eegen gsgles runner bench harness vu1 scripts tests -type f | while IFS= read -r f; do
    if [ -f "$main/$f" ] && cmp -s "$main/$f" "$f"; then touch -r "$main/$f" "$f"; fi
done
echo "sandbox ready: $(du -sh private/third_party/PS2Recomp private/third_party/build-m2 | tr '\n' ' ')"
