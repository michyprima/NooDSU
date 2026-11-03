#!/bin/sh
# Build Wii U debug variants of NooDS U, each with its own build folder, app file and title, so they can sit next to
# the regular build. The folder is cleared first, since make doesn't notice when only the flags change.
# Usage: ./build-wiiu-debug.sh <variant>...
#   heartbeat    Progress counters and timings in heartbeat.log, 4 times a second
#   timeline     Heartbeat, plus the GPU 3D renderer waiting for the GPU between parts of a frame to time each exactly
#   nocrackfill  Heartbeat without the GPU 3D renderer filling cracks between polygons, to compare frame times
#   capture      Hold L+R+Y (ZL+ZR+Y on the GamePad) to record 24 frames at each step of the pipeline to capture.bin
#   statedebug   Heartbeat, plus snapshots of what decides whether the CPUs run in statelog.txt around state loads
#   gpudump      Polygons, 3D layers and GPU renderer frames dumped every 60 frames, and frames drawn in software too
#   threadprobe  Every thread's state on request over UDP, answered from each core, for freezes
#   profile      Where the emulator thread spends its time, shown on screen
#   jittest      Runs the JIT against the interpreter before starting a game
#   drcdebug     What's known about the GamePad's display on the TV, with combos to try getting it back
#   frametime    Heartbeat, plus where the emulator thread spends each frame, listing every slow frame, and samples
#                of where its core was during slow frames in trace.bin
#   frameslots   frametime without the samples, which slow the emulator down, for truer times
#   framelog     The time of every emulator and UI frame in framelog.bin, cheap enough to measure dropped frames
#   audiocap     framelog, plus the audio the emulator made and the audio played in audio-produced.pcm and
#                audio-output.pcm; press ZL + ZR + Y to mark when you hear something
#   romload      How long loading the ROM into RAM took, in romload.txt
#   frameart     How long drawing the screen frames took, in frameart.txt
set -e
cd "$(dirname "$0")"
export DEVKITPRO=${DEVKITPRO:-/opt/devkitpro} DEVKITPPC=${DEVKITPPC:-/opt/devkitpro/devkitPPC}
BASE='-g -Ofast -fno-allow-store-data-races -flto -ffunction-sections -DESPRESSO -mcpu=750 -meabi -mhard-float $(INCLUDE) -D__WIIU__ -D__WUT__ -DENDIAN_BIG -DNO_FDOPEN -DLOG_LEVEL=0'

[ $# -gt 0 ] || { sed -n '2,22p' "$0"; exit 1; }
for variant in "$@"; do
    case $variant in
        heartbeat) flags='-DHEARTBEAT'; name=noodsu-hb; title='NooDS U Heartbeat' ;;
        timeline) flags='-DHEARTBEAT -DGPU_TIMELINE'; name=noodsu-timeline; title='NooDS U GPU Timeline' ;;
        nocrackfill) flags='-DHEARTBEAT -DGPU_NO_CRACK_FILL'; name=noodsu-nofill; title='NooDS U No Crack Fill' ;;
        capture) flags='-DFRAME_CAPTURE'; name=noodsu-capture; title='NooDS U Capture' ;;
        statedebug) flags='-DSTATE_DEBUG -DHEARTBEAT'; name=noodsu-statedebug; title='NooDS U State Debug' ;;
        gpudump) flags='-DGPU_DUMP'; name=noodsu-dump; title='NooDS U Dump' ;;
        threadprobe) flags='-DTHREAD_PROBE'; name=noodsu-probe; title='NooDS U Probe' ;;
        profile) flags='-DPROFILE_EMU'; name=noodsu-profile; title='NooDS U Profile' ;;
        jittest) flags='-DJIT_SELFTEST'; name=noodsu-jittest; title='NooDS U JIT Test' ;;
        drcdebug) flags='-DDRC_DEBUG'; name=noodsu-drcdebug; title='NooDS U GamePad Debug' ;;
        frametime) flags='-DHEARTBEAT -DPROFILE_EMU'; name=noodsu-frametime; title='NooDS U Frame Times' ;;
        frameslots) flags='-DHEARTBEAT -DPROFILE_EMU -DNO_FRAME_SAMPLES'; name=noodsu-frameslots; title='NooDS U Frame Slots' ;;
        framelog) flags='-DFRAME_LOG'; name=noodsu-framelog; title='NooDS U Frame Log' ;;
        audiocap) flags='-DFRAME_LOG -DAUDIO_CAPTURE'; name=noodsu-audiocap; title='NooDS U Audio Capture' ;;
        romload) flags='-DROM_LOAD_TEST'; name=noodsu-romload; title='NooDS U ROM Load' ;;
        frameart) flags='-DFRAME_ART_TEST'; name=noodsu-frameart; title='NooDS U Frame Art' ;;
        *) echo "Unknown variant: $variant"; exit 1 ;;
    esac
    rm -rf "build-wiiu-$name"
    make -j8 BUILD="build-wiiu-$name" NAME="$name" APP_NAME="$title" \
        CXXFLAGS="$BASE $flags" CFLAGS="$BASE $flags"
    echo "Built $name.wuhb ($title)"
done
