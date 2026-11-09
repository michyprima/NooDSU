#!/bin/bash
# Checks the DSi's NDMA channels and everything that triggers them, on the Mac, see README.md
# Usage: ndma.sh <ROM of at least 1.5MB, for the DS cart trigger> <folder with the system files>
set -e
here=$(cd "$(dirname "$0")"; pwd)
rom=$(cd "$(dirname "$1")"; pwd)/$(basename "$1"); cfg=$(cd "$2"; pwd)
base=${JIT_TEST_BUILD:-${TMPDIR:-/tmp}/noodsu-jit-test}
build=${JIT_TEST_GAME_BUILD:-$base-game}

# game.sh builds the core and Unicorn; only its objects are used here, with this test in place of its main
"$here/game.sh" "$rom" "$cfg" 1 --modes interp dsi > /dev/null
rev=$(sed -n 's/^rev=//p' "$here/game.sh")
uni=${JIT_TEST_UNICORN:-$base-unicorn-${rev:0:7}}
clang++ -std=c++17 -O2 -w $JIT_TEST_CFLAGS -I"$here" -I"$here/shim" -I"$build/src" -I"$build/src/gpu" -I"$build/src/io" \
    -I"$uni/include" -DNO_FDOPEN -c "$here/ndma.cpp" -o "$build/ndma.o"
clang++ "$build/ndma.o" $(ls "$build"/obj/*.o | grep -v '/game\.o$') "$build/slirp.o" -L"$uni/build" -lunicorn \
    -Wl,-rpath,"$uni/build" -o "$build/ndma"

rm -rf "$build/run-ndma"; mkdir -p "$build/run-ndma"/{saves,bios,cheats,states}
cp -c "$cfg/nand.bin" "$build/run-ndma/nand.bin" 2>/dev/null || cp "$cfg/nand.bin" "$build/run-ndma/nand.bin"
cd "$build/run-ndma"
"$build/ndma" "$cfg" "$rom" | LC_ALL=C grep -a 'FAIL\|checks\|error'
exit "${PIPESTATUS[0]}"
