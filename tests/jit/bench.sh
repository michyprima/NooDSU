#!/bin/bash
# Runs a game with this tree's core and another commit's side by side in game.sh, and compares the work done, see README.md
# Usage: bench.sh <commit or tag> <ROM, or - for none> <folder with the system files> <frames> [game options]
set -e
here=$(cd "$(dirname "$0")"; pwd)
ref=$1; shift
base=${JIT_TEST_BUILD:-${TMPDIR:-/tmp}/noodsu-jit-test}
old=$base-bench-tree

# Check out the other commit with this tree's harness, which also runs older cores
repo=$(git -C "$here" rev-parse --show-toplevel)
git -C "$repo" worktree remove --force "$old" 2> /dev/null || rm -rf "$old"
git -C "$repo" worktree add -q --detach "$old" "$ref"
trap 'git -C "$repo" worktree remove --force "$old"' EXIT
rm -rf "$old/tests/jit"; mkdir -p "$old/tests"; cp -R "$here" "$old/tests/jit"

# Run both at once, since some games depend on when they start
JIT_TEST_GAME_BUILD=$base-bench-old "$old/tests/jit/game.sh" "$@" > "$base-bench-old.log" 2>&1 & a=$!
JIT_TEST_GAME_BUILD=$base-bench-new "$here/game.sh" "$@" > "$base-bench-new.log" 2>&1 & b=$!
status=0; wait $a || status=1; wait $b || status=1
for side in old new; do
    echo "$side ($([ $side = old ] && echo "$ref" || echo "this tree")):"
    grep -E '^(interp|native|portable): [0-9]+ frames|differs' "$base-bench-$side.log" | sed 's/^/  /'
done
[ "$status" = 0 ] || { echo "a run failed, see $base-bench-old.log and $base-bench-new.log"; exit 1; }

# The instruction counts are what to compare; a change to what the game does shows in its frames too
for m in $(ls -d "$base-bench-new"/run-* | sed 's/.*run-//'); do
    LC_ALL=C awk -v m=$m '$1 == m":" { for (i = 1; i <= NF; i++) if ($i == "PowerPC") { a9[FILENAME] = $(i - 3); a7[FILENAME] = $(i - 1) } }
        END { o = ARGV[1]; n = ARGV[2]; if (a9[o] + a7[o] == 0) exit
            printf "%s: PowerPC instructions %+.3f%% for the ARM9, %+.3f%% for the ARM7\n", m,
                (a9[n] - a9[o]) * 100 / a9[o], a7[o] ? (a7[n] - a7[o]) * 100 / a7[o] : 0 }' \
        "$base-bench-old.log" "$base-bench-new.log"
    if cmp -s "$base-bench-old/run-$m/out.txt" "$base-bench-new/run-$m/out.txt"; then
        echo "$m: frames and memory match"
    else
        echo "$m: frames or memory differ, first at:"
        diff "$base-bench-old/run-$m/out.txt" "$base-bench-new/run-$m/out.txt" | head -4
    fi
done
