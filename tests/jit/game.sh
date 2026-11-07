#!/bin/bash
# Runs a game with the JIT's generated PowerPC in Unicorn and compares it with other ways of running it, see README.md
# Usage: game.sh <ROM, or - for none> <folder with the system files> <frames> [--mutate N] [--modes "interp native"] [--live]
# [--window] [--input] [--wifi] [game options]
set -e
here=$(cd "$(dirname "$0")"; pwd)
rom=-; [ "$1" = - ] || rom=$(cd "$(dirname "$1")"; pwd)/$(basename "$1"); cfg=$(cd "$2"; pwd); frames=$3; shift 3
mutation=0; modes="interp native"; live=0; window=0; input=0; wifi=0
while true; do
    if [ "$1" = --mutate ]; then mutation=$2; shift 2
    elif [ "$1" = --modes ]; then modes=$2; shift 2
    elif [ "$1" = --live ]; then live=1; shift
    elif [ "$1" = --window ]; then window=1; shift
    elif [ "$1" = --input ]; then window=1; input=1; shift
    elif [ "$1" = --wifi ]; then wifi=1; shift
    else break; fi
done
base=${JIT_TEST_BUILD:-${TMPDIR:-/tmp}/noodsu-jit-test}
build=$base-game; [ "$mutation" = 0 ] || build=$build-m$mutation
mkdir -p "$build/obj"

# Build the Unicorn PowerPC emulator once, from a development version: the release checks every write to RAM for code
# being written over, which makes runs much slower. JIT_TEST_UNICORN can give another Unicorn source folder, built in build/
rev=2a05c41831c678c22b2c6de471aae15b1cfc9ae3
uni=${JIT_TEST_UNICORN:-$base-unicorn-${rev:0:7}}
if ! ls "$uni"/build/libunicorn.* > /dev/null 2>&1; then
    [ -z "$JIT_TEST_UNICORN" ] || { echo "no Unicorn build in $uni/build"; exit 1; }
    echo "Building Unicorn, once"
    rm -rf "$uni.new"; mkdir -p "$uni.new"
    curl -sfL "https://github.com/unicorn-engine/unicorn/archive/$rev.tar.gz" | tar -xz -C "$uni.new" --strip-components 1
    cmake -S "$uni.new" -B "$uni.new/build" -G Ninja -DCMAKE_BUILD_TYPE=Release -DUNICORN_ARCH=ppc -DUNICORN_BUILD_TESTS=OFF \
        -DBUILD_SHARED_LIBS=ON > "$uni.new/build.log" && ninja -C "$uni.new/build" >> "$uni.new/build.log" ||
        { echo "couldn't build Unicorn, see $uni.new/build.log"; exit 1; }
    rm -rf "$uni"; mv "$uni.new" "$uni"
fi

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
clang -O2 -c "$here/shim/slirp.c" -o "$build/slirp.o" &

# The window needs SDL2, so it's only built and linked in when asked for, outside the other objects
extra=()
if [ "$window" = 1 ]; then
    sdl=$(pkg-config --cflags sdl2) || { echo "--window needs SDL2 (brew install sdl2)"; exit 1; }
    clang++ -std=c++17 -O2 -w $sdl -c "$here/window.cpp" -o "$build/window.o" &
    extra=("$build/window.o" $(pkg-config --libs-only-L sdl2) -lSDL2)
fi

# Wi-Fi needs the real libslirp (brew install libslirp) in place of the stubs, for the access point's network
if [ "$wifi" = 1 ]; then
    slirp=$(pkg-config --libs slirp) || { echo "--wifi needs libslirp (brew install libslirp)"; exit 1; }
    extra+=($slirp)
else
    extra+=("$build/slirp.o")
fi
wait
clang++ $JIT_TEST_CFLAGS "$build"/obj/*.o "${extra[@]}" -L"$uni/build" -lunicorn -Wl,-rpath,"$uni/build" -o "$build/game"

# Run each way in its own folder, since saves and the DSi NAND get written, then compare each with the first
pids=()
for m in $modes; do
    rm -rf "$build/run-$m"; mkdir -p "$build/run-$m"
    [ ! -e "$cfg/nand.bin" ] || cp -c "$cfg/nand.bin" "$build/run-$m/nand.bin" 2>/dev/null || cp "$cfg/nand.bin" "$build/run-$m/nand.bin"
    opts=(); [ "$live" = 0 ] || opts+=(live); [ "$window" = 0 ] || opts+=(window); [ "$input" = 0 ] || opts+=(input); [ "$wifi" = 0 ] || opts+=(wifi)
    if [ "$live" = 1 ]; then
        # Show each line as it's written too, marked with the mode
        (set -o pipefail; cd "$build/run-$m"; "$build/game" "$cfg" "$rom" $m "$frames" "$@" "${opts[@]}" 2> err.txt | tee out.txt | sed -l "s/^/$m: /") &
    else
        (cd "$build/run-$m"; exec "$build/game" "$cfg" "$rom" $m "$frames" "$@" "${opts[@]}" > out.txt 2> err.txt) &
    fi
    pids+=($!)
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
