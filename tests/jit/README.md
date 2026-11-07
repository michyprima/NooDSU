# JIT tests

Checks the JIT's generated PowerPC on a Mac by running it in the [Unicorn](https://www.unicorn-engine.org/) emulator and
comparing the results with the interpreter. The real `compile()`, `generate()`, linking and retiring are used. C++ called
from generated code, like memory accesses and interpreter handlers, is caught by the test, which checks how it's called and
overwrites the registers C++ is allowed to change.

```
tests/jit/run.sh <small ROM> <folder with bios7.bin, bios9.bin, firmware.bin> [suites]
tests/jit/mutate.sh <small ROM> <same folder> [suites]
```

Any small DS ROM works, since the game itself isn't run. The first run makes a Python environment to get Unicorn and builds a
changed copy of `src/core` in `$TMPDIR/noodsu-jit-test`; set `JIT_TEST_BUILD` to use another folder, and `JIT_TEST_SHARDS` to
limit the processes. It exits with a failure if anything differs. `mutate.sh` plants the bugs listed in `patch.py` one at a
time and checks that each is noticed; add one there when adding a kind of test.

## Suites

| Suite | What it covers |
|---|---|
| `thumb` | All 65,536 THUMB encodings on both CPUs, in System, IRQ and FIQ modes, with memory mapped, unmapped and mixed |
| `arm` | Every position of the interpreter's ARM table with all 16 conditions and random registers and offsets, on both CPUs |
| `forms` | ARM instructions with exact encodings, which random bits rarely make: `BX`, `BLX`, `CLZ`, `MRS`, `SWP`, multiplies, and every transfer addressing mode |
| `flags` | Flag-setting ARM and THUMB instructions over every pair of edge values and starting carry |
| `conditions` | Every condition with every flag value, on each way through a condition |
| `lists` | `PUSH`, `POP`, `LDM` and `STM` with lists crossing into pages that take the slow path |
| `blocks` | Random ARM and THUMB blocks with batching on: entered at the start or the middle, ending at a page end, stopping at every check, partway, at a deadline from an event or the other CPU, or from an event a C++ call made due, and with code changing during a store. Every stop must show exactly the state after an instruction, flags included |
| `links` | Blocks jumping to each other directly and through the dispatcher, changing CPU mode, as they're compiled, dropped and compiled again |
| `reference` | The interpreter's handlers against Unicorn's ARM946, an independent model of the same CPU: all THUMB encodings and sampled ARM ones, with banked registers, SPSRs and memory compared. The ARM7 is only compared on what it has in common with the ARM946 |

## What this doesn't cover

- **Speed.** Unicorn has no timing, so only the console can show whether something is faster.
- **The real CPU.** A pass relies on Unicorn behaving like Espresso, which it's set up as a PowerPC 750 to approximate.
- **What the handlers do, in places.** The `reference` suite checks them against QEMU's model, which isn't a real DS. It leaves
  out CP15, SWI, undefined encodings, cycle counts and everything the report lists as skipped, because the architecture doesn't
  promise a result there or QEMU is known to differ: misaligned accesses, a base in its own register list, stored PCs, writeback
  over loaded data and invalid modes. ARMv5 additions and loads of the PC aren't compared on the ARM7 at all.
- **Real memory and I/O.** Slow memory accesses are stood in for by the test and only reach RAM, so registers with side effects,
  DMA and interrupts aren't involved. Real writes over compiled code, and what the ARM7 does during a stop, aren't either.
- **Long-running behavior.** These are single instructions and short blocks, not a game; `game.sh` below runs whole games.

The JIT self-test on the console (`./build-wiiu-debug.sh jittest`) covers most of this, but turns batching off, so playing games
on the console is still needed for that.

## Whole games

`game.sh` runs a game with the generated PowerPC in Unicorn and compares it with the interpreter, frame by frame. This covers
what the cases above leave out: real memory and I/O, the ARM7 and tasks running during stops, invalidation and linking over
millions of blocks, and how all of that adds up over time.

```
tests/jit/game.sh <ROM, or - for none> <folder with the system files> <frames> [--modes "interp native"] [--mutate N] [--live]
    [--window] [--input] [--wifi] [options]
```

The folder needs `bios9.bin`, `bios7.bin` and `firmware.bin`, plus `bios9i.bin`, `bios7i.bin`, `firmwarei.bin` and `nand.bin`
for DSi mode. Each way of running is given by `--modes`: `interp` without the JIT, `portable` with the JIT's portable executor,
and `native` with generated code. Each prints a line per frame with a hash of the screens, the cycle counts and both CPUs'
program counters, then hashes of RAM and WRAM, and every way is compared with the first. Options are:

- `fast` for fast timing, `dsi` for DSi mode, and `firmware` to start a ROM from the firmware's menu instead of directly
- `press=N`, `start=N` and `touch=N` to press A, press START or touch the middle of the bottom screen every N frames
- `keys=frame:key,...` to press keys at given frames, numbered like KEYINPUT's bits, then 10 and 11 for X and Y
- `taps=frame:x:y,...` to touch the bottom screen at given frames and positions, for 8 frames each
- `realtime` to run at most 60 frames a second, to watch a run in its window or give a network real time to answer
- `trace=N` to also print both CPUs' registers at the start of every scanline from frame N
- `ppm=file` to save the last frame, `ram=file` to save main RAM at the end, and `results=file` to save what
  `tests/hardware`'s ROM recorded, for its build

To watch a run, `--live` also prints each way's lines as they come, marked with its mode, and `--window` shows each way's
screens in a window of its own, at most 60 times a second. The window needs SDL2 (`brew install sdl2`), and is only built
with `--window`; closing it lets the run carry on without it, while quitting from the menu bar ends the run.

`--input` also takes input from the window, at 60 frames a second: clicking or dragging on the bottom screen touches it,
and the keyboard has the buttons: arrows for the D-pad, X for A, Z for B, S for X, A for Y, Q for L, W for R, Enter for
START and right Shift for SELECT. Input only counts while the window has focus, and closing the window ends the run; give
enough frames to play, since it also stops after them.

`--wifi` turns on the software access point and links the real libslirp (`brew install libslirp`) in place of the stubs, so
runs can go online, like System Settings' connection test. What comes back from the network differs from run to run, so
runs with it aren't expected to match each other.

The first run also builds Unicorn, which needs `cmake` and `ninja`: a development version, pinned in `game.sh`, since the
release checks every write to RAM for code being written over, which makes runs much slower. Set `JIT_TEST_UNICORN` to
another Unicorn source folder, built in its `build` folder, to use that instead.

It builds in `$TMPDIR/noodsu-jit-test-game`, and leaves each run's output in a `run-<mode>` folder there; set
`JIT_TEST_CFLAGS` to build with more flags, like `-fsanitize=address,undefined -g`.

Fast timing lets generated code count cycles in batches, so its runs differ from the interpreter's by design; it can still be
run alone to see that a game works. With exact timing, any difference is a bug.

The emulator works on the core's own state while generated code runs. The block cache and the memory maps are areas whose
reads go to the core, with the 64-bit pointers in them turned into the emulator's 32-bit addresses. The interpreter and the
scheduler's values are read and written far more often, so they're copied into the emulator in its byte order when generated
code is entered or a call from it returns, and copied back when it calls or returns, which is the only time C++ runs; a
write anywhere generated code shouldn't write stops the run. Main RAM and the core object are mapped into the emulator
directly, so memory the maps point to is shared; anywhere else gets a null pointer, which sends accesses through the slow
path. Memory generated code doesn't run from is mapped without execute permission, so Unicorn doesn't check writes to it
for code being written over. Calls from generated code
to C++, like interpreter handlers and the interleave helper, run for real. Each CPU has its own emulator, since ARM7 code
can run during a stop in ARM9 code.

Mario Kart DS with `press=40` reaches a race by about frame 1500. DSi mode with no ROM and `press=1250` is at the DSi menu
by frame 1300; the next press, at frame 2500, starts the DSi Browser, which runs until it finds no network. With
`keys=1500:0`, Left (key 5) every 40 frames from 1640 to 1880 and `1960:0`, it starts System Settings, up by frame 2900.
Pokemon Black 2 in DSi mode with `press=37` names the rival by frame 4400. With generated code in Unicorn, these run at 15 to 65 frames a second.
