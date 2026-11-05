# Instruction ROM

A DS ROM that runs every instruction form on both CPUs, for checking the JIT's native code on the console itself and seeing
what each instruction really costs. `tests/jit` checks generated code in an emulator, one instruction or short block at a time;
this runs it for real, on the Wii U, with real memory.

## Running it on the Wii U

```
./build-wiiu-debug.sh romtest
tests/rom/build.sh <any DS ROM, for the logo>
```

Copy `noodsu-romtest.wuhb` and `tests/rom/build/noodsu-instructions.nds` to the SD card, start the first and choose the second
as the game. It runs the ROM three times, with the interpreter, then the JIT with fast CPU timing, then the JIT with normal
timing. That takes a few minutes, showing which case it's on, and ends by saying how many cases differ. A case that doesn't
finish in 60 seconds stops the test, and if the console itself crashes, the last line written shows about where.

Everything goes in `romtest.txt` in the app's folder, which `report.py` turns into the differences and a table of times:

```
tests/rom/report.py romtest.txt
```

## What it checks

Each case is one instruction or a few, run with 6 sets of register values and 4 sets of flags. All 16 registers, the CPSR, the
SPSR and any memory it can touch go into a hash, so two runs can be compared case by case.

- **ARM:** data processing with every kind of operand, all conditions, multiplies including the ARM9's halfword ones, `QADD` and
  friends, `CLZ`, every addressing mode of words, bytes, halfwords and `LDRD`/`STRD`, `LDRT`/`STRT`, `SWP`, `LDM`/`STM` with every
  addressing mode, base in the list, across a page end and with `S`, everything that writes the PC, `B`/`BL`/`BX`/`BLX`, `MRS`/`MSR`
  with mode changes, `MRC`/`MCR` and `SWI` through the BIOS.
- **THUMB:** every format, including high registers, the stack, `BLX` and `SWI`.
- **Memory:** main RAM and the TCMs, which the JIT reaches directly, and palette RAM on the ARM9, which it doesn't. The ARM7 uses main
  RAM and its own WRAM.
- **Running:** code changing, blocks entered partway, long runs, timer interrupts during ARM and THUMB loops, and waking from a halt.

Handlers aren't left out, so it also compares what stays with the interpreter in each run. Comparing against the interpreter finds where the
JIT differs from it, not where they're both wrong; running the ROM on a real DS finds those, see below.

## What it times

Each unit of `gen.py`'s timed list runs 8 times a loop for 4,000 loops, 5 times, keeping the shortest, with the screens blanked and the other CPU asleep. A
DS timer can't show this, since it counts emulated time and reads the same either way, so the romtest build adds registers
at `0x04FFF000` with the console's real clock. `report.py` takes off the time of an empty loop and lists nanoseconds per instruction,
worst first, so what the JIT makes slower than the interpreter is at the top. It also lists guest cycles from a DS timer per instruction, and how they differ.

## On a real DS

Copy `build/noodsu-instructions.nds` to a flashcart and start it. It needs no libraries, touch screen or save. The case running stays on screen, so a hang shows where it happened. It takes a few minutes, then more to save, skipping the jumps and PC loads with misaligned addresses, which the ARM manual leaves undefined, so a real CPU might not come back from them.

Results are compared with what's built in, which is NooDS U's interpreter on the Mac.

| Button | What it does |
|---|---|
| Up and Down | Move through what differs, or scroll cycles |
| Left and Right | The same, a page at a time |
| A | Details of the chosen case: the first set of values to differ, and the registers, status and memory it leaves on this DS |
| B | Back from the details |
| L or R | Switch between what differs, and the ARM9's and ARM7's cycles |

Cycles are per instruction in tenths, this DS's first, red if they differ from NooDS U's. It can't time real speed on a DS.

### Saved results

When the run ends, it writes what differs to `noodsu-results.bin` in the root of the flashcart's card, using libfat and the flashcart's DLDI driver like homebrew does. It reads the file back to check it, and the second line of the first page says what happened. libfat comes from devkitPro's `libfat-gba`, since it works with any DLDI driver.

Flashcarts usually patch their driver into homebrew when starting it, which the ROM's header is set up for. If one doesn't, put the driver in yourself after building, since building starts over; the driver is usually on the flashcart's card, like `__rpg/r4_sd.dldi`:

```
tests/rom/dldi.py tests/rom/build/noodsu-instructions.nds <driver.dldi>
```

With no driver at all, the ROM tries the save chip instead, in case the flashcart keeps that as a `.sav` file, trying each kind and reading back to see what works. An empty `.sav` next to the ROM only means the flashcart took it for a game.

To compare saved results word by word, get the same values from the Mac by running the ROM with the interpreter and `RomTest::detail` set to 2, then:

```
tests/rom/results.py noodsu-results.bin <that run's results> tests/rom/build/cases.h
```

It lists which registers differ most with their commonest values, which combinations of them go together, and every difference. The details come from running each case again, so a case that gives the same values then was affected by what ran before it.

### Multiply flags

A real ARM7's multiplies with flags change the carry, which the ARM manual only calls meaningless. So the ROM also records the flags from every kind of multiply, on both CPUs, for 1,600 pairs of operands and with the carry clear and set beforehand. These go at the end of the saved results:

```
tests/rom/multiply.py noodsu-results.bin [output.csv]
```

## Updating what's built in

Run the ROM on the Mac with the interpreter and `RomTest::detail` set, which adds the `S` lines, then:

```
tests/rom/expected.py romtest.txt tests/rom/build/cases.h tests/rom/build/expected.bin
tests/rom/build.sh <the same ROM> tests/rom/build/expected.bin
```

The tables have a fixed size, so this doesn't move the code. Anything else that moves it means doing this again, since some cases see code addresses.

## What it doesn't cover

User mode, since a case needs a way back. Undefined instructions. Registers with side effects, DMA and the 3D engine. The ARM7's slow memory path. Interrupts arriving at particular instructions, which depend on timing. Whether the guest cycles are right, beyond listing them. Halting MCRs run only as the wake test.
