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
| `returns` | ALU instructions with flags that write the program counter, returning to every kind of saved mode on both CPUs, which of them generated code leaves to the interpreter, and code after a return using the new mode's banked registers, with the JIT's real table of them |
| `math` | Every access to the math registers at `0x4000280` to `0x40002BF` against the real ones, for what generated code reaches directly and what it leaves to C++, then random runs of accesses against the same run done only in C++, and the same for `IME`, `IE` and `IF` |
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
- `audio` to mix the sound as well, which is left out otherwise for speed, and add a running hash of it to every frame's line, so comparing runs shows the first frame it differs on
- `wav=file` to also save the sound as a WAV file, at 32768 Hz in 16-bit stereo, which turns on `audio`
- `realtime` to run at most 60 frames a second, to watch a run in its window or give a network real time to answer
- `threads` to run with the 2D threads drawing behind the emulator, as fast 2D does on the console; their timing varies, so
  runs with it aren't expected to match each other
- `vramstress` to run only a test of VRAM reads from another thread during VRAM bank changes, which ignores the frame count
  and exits as soon as it's done, or crashes if a read can follow a mapping that's only partly there. Planted bug 56 removes
  what stops that, so `--mutate 56` with this option should crash; `mutate.sh` doesn't run it, since it runs the cases above. Planted bug 57 breaks ADPCM sound for both the interpreter and generated code, so check it by comparing an `audio` run against one without it
- `trace=N` to also print both CPUs' registers at the start of every scanline from frame N
- `ppm=file` to save the last frame, `ram=file` to save main RAM at the end, and `results=file` to save what
  `tests/hardware`'s ROM recorded, for its build
- `headphones=N` to plug headphones into the DSi at frame N, as the GamePad's jack would

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
`JIT_TEST_GAME_BUILD` to use another folder, and `JIT_TEST_CFLAGS` to build with more flags, like
`-fsanitize=address,undefined -g`.

### Comparing with a release

Each run also counts the PowerPC instructions generated code runs for each CPU. Unicorn has no timing, but with the same
frames, more instructions in generated code means more work for the console, so `bench.sh` compares them with an older
commit's, like the last release before making a new one:

```
tests/jit/bench.sh <commit or tag> <ROM, or - for none> <folder with the system files> <frames> [game.sh options]
```

It checks the commit out in `$TMPDIR/noodsu-jit-test-bench-tree` with this tree's harness, runs both cores at once in
`game.sh`, then prints each run's counts, the change in instructions, and whether the frames and memory still match. Older
cores only work as far back as `patch.py` still applies to them; v0.5 does. The counts leave out the C++ that generated
code calls, which runs natively on the Mac, so a change there only shows in the `interp` time, and only roughly. At 17fa864,
Mario Kart DS in a race (`press=40`, 2500 frames) and Pokemon Black (`press=37`, 6000) ran within 3 instructions of v0.5
in billions, with the same frames; Pokemon's main RAM differed, probably from the new cartridge chip ID.

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

## NDMA

`ndma.sh <ROM> <system files folder>` checks the DSi's NDMA channels and everything that triggers them, on the Mac with the
interpreter, and prints how many checks passed. The ROM is only read from, by the DS cart trigger's checks, so it needs to be at
least 1.5MB; any game will do. It runs in DSi mode, so the folder needs a NAND. See `tests/hardware` for the ROM that showed how a
real DSi behaves.

## GBA slot

`slot2.sh <DS ROM> <system files folder>` checks the GBA slot in DS mode, on the Mac with the interpreter, and prints how many
checks passed: what an empty slot reads at each ROM access time, `EXMEMCNT` giving the slot to one CPU, each accessory (Rumble
Pak, Memory Expansion Pak, Guitar Grip, Easy Piano, paddle, solar sensor), and the GPIO pins of Boktai and WarioWare: Twisted
carts in DS and GBA mode, with small GBA ROMs it makes. The DS ROM only gives the solar sensor's header its logo, and GBA mode
needs `gba_bios.bin` in the folder. The expected values come from GBATEK, DeSmuME and melonDS, not a real DS.

## Display FIFO

`fifo.sh <DS ROM> <system files folder>` checks the main memory display FIFO, on the Mac with the interpreter, and prints how
many checks passed: what writes of each size to `DISP_MMEM_FIFO` do, engine A showing it in display mode 3 with master
brightness, display capture taking it as source B alone and blended, DMA timing 4 filling it in different ways and being
stopped at V-blank, the ring repeating when nothing fills it, and DSi NDMA mode 9. The display checks run in DS mode, DS mode
with the 2D threads and fast 2D, and DSi mode, so the folder needs a NAND. Any small ROM will do, since it isn't run. The
expected values come from GBATEK and melonDS, not a real DS; `tests/hardware`'s FIFO test is for that.

## DSi microphone

`mic.sh <DS ROM> <system files folder>` checks the DSi's microphone FIFO (`MIC_CNT`, `MIC_DATA`) and `SNDEXCNT`, on the Mac
with the interpreter, and prints how many checks passed: register bits, nothing arriving unless both enable bits are set,
every rate and data format through NDMA mode 0xC, the order of samples in each word, the status bits as the FIFO fills to
an overrun, clearing it, the interrupt modes, NDMA starting late and stopping, and save states. Any small ROM will do, since
it isn't run. It runs in DSi mode, so the folder needs a NAND. The expected values come from GBATEK, not a real DSi.

## DSi SD card and NAND

`sdmmc.sh <DS ROM> <system files folder>` checks what the DSi's SD/MMC controller reports about its cards, on the Mac with
the interpreter, and prints how many checks passed: the NAND's CSD (a real DSi eMMC's), the CSD of standard- and
high-capacity SD images of several sizes and the capacity a driver works out from it, the OCR's capacity bit, the SCR and SD
status read through the FIFO, the bus width from ACMD6, the card detect after a reset, and an empty SD slot, whose commands time out while the NAND and Wi-Fi
chip still answer. It makes sparse images in its run folder and removes them. Any small ROM will do, since it isn't run. It
runs in DSi mode, so the folder needs a NAND. The SD card values come from the SD specification, not a real DSi.

## DSi headphones

`gpio.sh <DS ROM> <system files folder>` checks the DSi's GPIO pins and the touchscreen controller's headset detection, on the
Mac with the interpreter, and prints how many checks passed: the pins' register bits and idle levels, outputs ANDed with the
lines, the headphone pin following the GamePad's jack with its interrupt on the chosen edge, the codec's headset detection,
sticky change flag and GPI2 pin, save states, and nothing in DS mode. Any small ROM will do, since it isn't run. It runs in DSi
mode, so the folder needs a NAND. The expected values come from GBATEK, not a real DSi.
