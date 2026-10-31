#!/bin/sh
# Build Wii U debug variants of NooDS, each with its own build folder, app file and title, so they can sit next to
# the regular build. The folder is cleared first, since make doesn't notice when only the flags change.
# Usage: ./build-wiiu-debug.sh <variant>...
#   heartbeat    Progress counters and timings in heartbeat.log, 4 times a second
#   timeline     Heartbeat, plus the GPU 3D renderer waiting for the GPU between parts of a frame to time each exactly
#   capture      Hold L+R+Y (ZL+ZR+Y on the GamePad) to record 24 frames at each step of the pipeline to capture.bin
#   statedebug   Heartbeat, plus snapshots of what decides whether the CPUs run in statelog.txt around state loads
#   gpudump      Polygons, 3D layers and GPU renderer frames dumped every 60 frames, and frames drawn in software too
#   threadprobe  Every thread's state on request over UDP, answered from each core, for freezes
#   profile      Where the emulator thread spends its time, shown on screen
#   jittest      Runs the JIT against the interpreter before starting a game
set -e
cd "$(dirname "$0")"
export DEVKITPRO=${DEVKITPRO:-/opt/devkitpro} DEVKITPPC=${DEVKITPPC:-/opt/devkitpro/devkitPPC}
BASE='-g -Ofast -fno-allow-store-data-races -flto -ffunction-sections -DESPRESSO -mcpu=750 -meabi -mhard-float $(INCLUDE) -D__WIIU__ -D__WUT__ -DENDIAN_BIG -DNO_FDOPEN -DLOG_LEVEL=0'

[ $# -gt 0 ] || { sed -n '2,13p' "$0"; exit 1; }
for variant in "$@"; do
    case $variant in
        heartbeat) flags='-DHEARTBEAT'; name=noods-hb; title='NooDS Heartbeat' ;;
        timeline) flags='-DHEARTBEAT -DGPU_TIMELINE'; name=noods-timeline; title='NooDS GPU Timeline' ;;
        capture) flags='-DFRAME_CAPTURE'; name=noods-capture; title='NooDS Capture' ;;
        statedebug) flags='-DSTATE_DEBUG -DHEARTBEAT'; name=noods-statedebug; title='NooDS State Debug' ;;
        gpudump) flags='-DGPU_DUMP'; name=noods-dump; title='NooDS Dump' ;;
        threadprobe) flags='-DTHREAD_PROBE'; name=noods-probe; title='NooDS Probe' ;;
        profile) flags='-DPROFILE_EMU'; name=noods-profile; title='NooDS Profile' ;;
        jittest) flags='-DJIT_SELFTEST'; name=noods-jittest; title='NooDS JIT Test' ;;
        *) echo "Unknown variant: $variant"; exit 1 ;;
    esac
    rm -rf "build-wiiu-$name"
    make -f Makefile.wiiu -j8 BUILD="build-wiiu-$name" NAME="$name" APP_NAME="$title" \
        CXXFLAGS="$BASE $flags" CFLAGS="$BASE $flags"
    echo "Built $name.wuhb ($title)"
done
