#!/bin/bash
# Runs a game with the JIT's generated PowerPC in Unicorn and compares it with other ways of running it, see README.md
# Usage: game.sh <ROM, or - for none> <folder with the system files> <frames> [--mutate N] [--modes "interp native"] [game options]
set -e
here=$(cd "$(dirname "$0")"; pwd)
rom=-; [ "$1" = - ] || rom=$(cd "$(dirname "$1")"; pwd)/$(basename "$1"); cfg=$(cd "$2"; pwd); frames=$3; shift 3
mutation=0; modes="interp native"
while true; do
    if [ "$1" = --mutate ]; then mutation=$2; shift 2
    elif [ "$1" = --modes ]; then modes=$2; shift 2
    else break; fi
done
base=${JIT_TEST_BUILD:-${TMPDIR:-/tmp}/noodsu-jit-test}
build=$base-game; [ "$mutation" = 0 ] || build=$build-m$mutation
mkdir -p "$build/obj"

# Get the Unicorn PowerPC emulator once, shared with run.sh
if [ ! -d "$base/venv" ]; then python3 -m venv "$base/venv"; "$base/venv/bin/pip" install -q unicorn; fi
uni=$(ls -d "$base"/venv/lib/python*/site-packages/unicorn)

# Build from a changed copy of the core, only compiling what's different
rm -rf "$build/new"; mkdir -p "$build/new"; cp -R "$here/../../src/core/" "$build/new/"
python3 -I "$here/patch.py" "$build/new" "$mutation" game
if rsync -rci --delete "$build/new/" "$build/src/" | grep -q '\.h$'; then rm -f "$build"/obj/*.o; fi
flags="-std=c++17 -O2 -w $JIT_TEST_CFLAGS -I$here/shim -I$build/src -I$build/src/gpu -I$build/src/io -I$uni/include -DNO_FDOPEN"
for f in $(cd "$build/src"; find . -name '*.cpp'); do
    o="$build/obj/$(echo "${f#./}" | tr '/' '_').o"
    if [ ! -e "$o" ] || [ "$build/src/$f" -nt "$o" ]; then clang++ $flags -c "$build/src/$f" -o "$o" & fi
    while [ "$(jobs -r | wc -l)" -ge 12 ]; do sleep 0.05; done
done
if [ ! -e "$build/obj/game.o" ] || [ "$here/game.cpp" -nt "$build/obj/game.o" ] || [ "$build/src/core.h" -nt "$build/obj/game.o" ]; then
    clang++ $flags -c "$here/game.cpp" -o "$build/obj/game.o" &
fi
clang -O2 -c "$here/shim/slirp.c" -o "$build/obj/slirp.o" &
wait
clang++ $JIT_TEST_CFLAGS "$build"/obj/*.o -L"$uni/lib" -lunicorn -Wl,-rpath,"$uni/lib" -o "$build/game"

# Run each way in its own folder, since saves and the DSi NAND get written, then compare each with the first
pids=()
for m in $modes; do
    rm -rf "$build/run-$m"; mkdir -p "$build/run-$m"
    [ ! -e "$cfg/nand.bin" ] || cp -c "$cfg/nand.bin" "$build/run-$m/nand.bin" 2>/dev/null || cp "$cfg/nand.bin" "$build/run-$m/nand.bin"
    (cd "$build/run-$m"; exec "$build/game" "$cfg" "$rom" $m "$frames" "$@" > out.txt 2> err.txt) & pids+=($!)
done
status=0; for p in "${pids[@]}"; do wait "$p" || status=1; done
first=""
for m in $modes; do
    cat "$build/run-$m/err.txt"
    if [ -z "$first" ]; then first=$m; continue; fi
    if cmp -s "$build/run-$first/out.txt" "$build/run-$m/out.txt"; then
        echo "$m matches $first over $(grep -c '^frame' "$build/run-$m/out.txt") frames"
    else
        echo "$m differs from $first:"; diff "$build/run-$first/out.txt" "$build/run-$m/out.txt" | head -6; status=1
    fi
done
echo "Logs are in $build/run-*"
exit $status
