#!/bin/bash
# Tests the JIT's generated PowerPC on a Mac, see README.md
# Usage: run.sh <small ROM> <folder with bios7.bin, bios9.bin, firmware.bin> [--mutate N] [test options]
set -e
here=$(cd "$(dirname "$0")"; pwd)
rom=$(cd "$(dirname "$1")"; pwd)/$(basename "$1"); cfg=$(cd "$2"; pwd); shift 2
mutation=0
if [ "$1" = --mutate ]; then mutation=$2; shift 2; fi
build=${JIT_TEST_BUILD:-${TMPDIR:-/tmp}/noodsu-jit-test}; [ "$mutation" = 0 ] || build=$build-m$mutation
mkdir -p "$build/obj"

# Get the Unicorn PowerPC emulator once
base=${JIT_TEST_BUILD:-${TMPDIR:-/tmp}/noodsu-jit-test}
if [ ! -d "$base/venv" ]; then python3 -m venv "$base/venv"; "$base/venv/bin/pip" install -q unicorn; fi
uni=$(ls -d "$base"/venv/lib/python*/site-packages/unicorn)

# Build from a changed copy of the core, only compiling what's different
rm -rf "$build/new"; mkdir -p "$build/new"; cp -R "$here/../../src/core/" "$build/new/"
python3 -I "$here/patch.py" "$build/new" "$mutation"
# A changed header can change what any file needs, so start over then
if rsync -rci --delete "$build/new/" "$build/src/" | grep -q '\.h$'; then rm -f "$build"/obj/*.o; fi
flags="-std=c++17 -O2 -w -I$here/shim -I$build/src -I$build/src/gpu -I$uni/include -DNO_FDOPEN"
for f in $(cd "$build/src"; find . -name '*.cpp'); do
    o="$build/obj/$(echo "${f#./}" | tr '/' '_').o"
    if [ ! -e "$o" ] || [ "$build/src/$f" -nt "$o" ]; then clang++ $flags -c "$build/src/$f" -o "$o" & fi
    while [ "$(jobs -r | wc -l)" -ge 12 ]; do sleep 0.05; done
done
clang++ $flags -c "$here/jit_test.cpp" -o "$build/obj/jit_test.o" &
wait
clang++ "$build"/obj/*.o -L"$uni/lib" -lunicorn -Wl,-rpath,"$uni/lib" -o "$build/jit_test"

# Run a shard per CPU core, in a folder for its logs
cd "$build"; ln -sfn "$cfg" cfg; cp -c "$rom" rom.nds 2>/dev/null || cp "$rom" rom.nds
shards=${JIT_TEST_SHARDS:-$(sysctl -n hw.ncpu)}; rm -f shard*.log; pids=()
for ((i = 0; i < shards; i++)); do ./jit_test cfg ./rom.nds $i $shards "$@" > shard$i.log 2>&1 & pids+=($!); done
status=0; for p in "${pids[@]}"; do wait "$p" || status=1; done
python3 -I "$here/report.py" shard*.log || status=1
exit $status
