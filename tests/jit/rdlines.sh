#!/bin/bash
# Checks the 3D engine's render timing against a real DS, on the Mac, see README.md
# Usage: rdlines.sh <folder with the system files>
set -eo pipefail
here=$(cd "$(dirname "$0")"; pwd); cfg=$(cd "$1"; pwd)
hw=$here/../hardware
base=${JIT_TEST_BUILD:-${TMPDIR:-/tmp}/noodsu-jit-test}
build=${JIT_TEST_GAME_BUILD:-$base-game}
[ -e "$hw/build/noodsu-rdlines.nds" ] || "$hw/build.sh" > /dev/null

# Run the 3D timing test ROM and compare what it saved in RAM with what a real DS saved
"$here/game.sh" "$hw/build/noodsu-rdlines.nds" "$cfg" 1200 --modes interp ram=rdlines.bin > /dev/null
python3 -I "$hw/rdlines.py" "$hw/rdlines-realds.bin" "$build/run-interp/rdlines.bin" | grep -v '^capture'
