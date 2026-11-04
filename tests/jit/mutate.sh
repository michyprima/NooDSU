#!/bin/bash
# Plants each of the bugs listed in patch.py and checks that the tests notice, to show the tests can fail
# Usage: mutate.sh <small ROM> <folder with bios7.bin, bios9.bin, firmware.bin> [test options]
here=$(cd "$(dirname "$0")"; pwd)
rom=$1; cfg=$2; shift 2
count=$(grep -c "^    (.*# [0-9]*:" "$here/patch.py")
only=${JIT_TEST_ONLY:-}
base=${JIT_TEST_BUILD:-${TMPDIR:-/tmp}/noodsu-jit-test}
missed=0
for ((m = 1; m <= count; m++)); do
    if [ -n "$only" ] && ! echo " $only " | grep -q " $m "; then continue; fi
    # Start from the normal build, so only what the bug changes gets compiled
    if [ -d "$base/obj" ] && [ ! -d "$base-m$m" ]; then mkdir -p "$base-m$m"; cp -Rp "$base/obj" "$base/src" "$base-m$m/"; fi
    output=$("$here/run.sh" "$rom" "$cfg" --mutate "$m" "$@" 2>&1); status=$?
    label=$(grep "# $m:" "$here/patch.py" | sed 's/.*# [0-9]*: //')
    found=$(echo "$output" | grep '^Mismatches:' | cut -d' ' -f2)
    if [ "$status" = 0 ]; then echo "MISSED $m: $label"; missed=$((missed + 1)); else echo "caught $m ($found mismatches): $label"; fi
done
echo "$missed of $count bugs were missed"
exit $((missed > 0))
