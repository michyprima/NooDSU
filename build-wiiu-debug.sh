#!/bin/sh
# Build Wii U debug variants of NooDSU, each with its own build folder, app file and title, so they can sit next to
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
#   drcdebug     What's known about the GamePad's display on the TV, with combos to try getting it back
#   frametime    Heartbeat, plus where the emulator thread spends each frame, listing every slow frame, and samples
#                of where its core was during slow frames in trace.bin
#   framelog     The time of every emulator and UI frame in framelog.bin, cheap enough to measure dropped frames
#   romload      How long loading the ROM into RAM took, in romload.txt
set -e
cd "$(dirname "$0")"
export DEVKITPRO=${DEVKITPRO:-/opt/devkitpro} DEVKITPPC=${DEVKITPPC:-/opt/devkitpro/devkitPPC}
BASE='-g -Ofast -fno-allow-store-data-races -flto -ffunction-sections -DESPRESSO -mcpu=750 -meabi -mhard-float $(INCLUDE) -D__WIIU__ -D__WUT__ -DENDIAN_BIG -DNO_FDOPEN -DLOG_LEVEL=0'

[ $# -gt 0 ] || { sed -n '2,18p' "$0"; exit 1; }
for variant in "$@"; do
    case $variant in
        heartbeat) flags='-DHEARTBEAT'; name=noodsu-hb; title='NooDSU Heartbeat' ;;
        timeline) flags='-DHEARTBEAT -DGPU_TIMELINE'; name=noodsu-timeline; title='NooDSU GPU Timeline' ;;
        capture) flags='-DFRAME_CAPTURE'; name=noodsu-capture; title='NooDSU Capture' ;;
        statedebug) flags='-DSTATE_DEBUG -DHEARTBEAT'; name=noodsu-statedebug; title='NooDSU State Debug' ;;
        gpudump) flags='-DGPU_DUMP'; name=noodsu-dump; title='NooDSU Dump' ;;
        threadprobe) flags='-DTHREAD_PROBE'; name=noodsu-probe; title='NooDSU Probe' ;;
        profile) flags='-DPROFILE_EMU'; name=noodsu-profile; title='NooDSU Profile' ;;
        jittest) flags='-DJIT_SELFTEST'; name=noodsu-jittest; title='NooDSU JIT Test' ;;
        drcdebug) flags='-DDRC_DEBUG'; name=noodsu-drcdebug; title='NooDSU GamePad Debug' ;;
        frametime) flags='-DHEARTBEAT -DPROFILE_EMU'; name=noodsu-frametime; title='NooDSU Frame Times' ;;
        framelog) flags='-DFRAME_LOG'; name=noodsu-framelog; title='NooDSU Frame Log' ;;
        romload) flags='-DROM_LOAD_TEST'; name=noodsu-romload; title='NooDSU ROM Load' ;;
        *) echo "Unknown variant: $variant"; exit 1 ;;
    esac
    rm -rf "build-wiiu-$name"
    make -j8 BUILD="build-wiiu-$name" NAME="$name" APP_NAME="$title" \
        CXXFLAGS="$BASE $flags" CFLAGS="$BASE $flags"
    echo "Built $name.wuhb ($title)"
done
