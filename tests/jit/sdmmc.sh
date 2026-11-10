#!/bin/bash
# Checks what the DSi's SD/MMC controller reports about its cards, on the Mac, see README.md
# Usage: sdmmc.sh <any DS ROM, not run> <folder with the system files>
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
    -I"$uni/include" -DNO_FDOPEN -c "$here/sdmmc.cpp" -o "$build/sdmmc.o"
clang++ "$build/sdmmc.o" $(ls "$build"/obj/*.o | grep -v '/game\.o$') "$build/slirp.o" -L"$uni/build" -lunicorn \
    -Wl,-rpath,"$uni/build" -o "$build/sdmmc"

rm -rf "$build/run-sdmmc"; mkdir -p "$build/run-sdmmc"/{saves,bios,cheats,states}
cp -c "$cfg/nand.bin" "$build/run-sdmmc/nand.bin" 2>/dev/null || cp "$cfg/nand.bin" "$build/run-sdmmc/nand.bin"
cd "$build/run-sdmmc"
"$build/sdmmc" "$cfg" "$rom" | LC_ALL=C grep -a 'FAIL\|checks\|error'
exit "${PIPESTATUS[0]}"
