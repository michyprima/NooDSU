# Instruction tests for a real DS

A ROM that settles how the CPUs really behave where the interpreter, the ARM manual and Unicorn disagree, or where the
manual doesn't promise anything. It runs each test on the ARM9 and ARM7, and shows where the DS differs from NooDS U, whose
results are built in.

## Running it

Copy `build/noodsu-test.nds` to the SD card or a flashcart and start it like any homebrew, such as from TWiLight Menu++ or
nds-bootstrap. On a DSi, run it in DSi mode, since that's what reaches the SD card. It doesn't use the touch screen.

It's built with libnds, whose ARM7 side gives the ARM9 the SD card or the flashcart's. The tests take over interrupts and
exceptions from libnds while they run, through the BIOS like a bare DS program, and give them back afterwards.

After the first results are on screen, and again after each test START runs, it writes all the results so far to
`noodsu-hwtest.bin` in the folder it started from, or else the root of the SD card or flashcart. The end of the second line
says how that went: `SAVED`, `SAVED SD:` or `SAVED FAT:` for where, `NOT SAVED` or `SAVE FAILED`. To list the differences:

```
tests/hardware/results.py noodsu-hwtest.bin [all]
```

The file has NooDS U's results too, so that's all it needs; `all` lists every result, not only the differences.

| Button | What it does |
|---|---|
| L or R | Switch between the ARM9's and ARM7's results |
| Up and Down | Scroll a line |
| Left and Right | Scroll a page |
| A | Switch between only the differences and all results |
| START | Run the next test that could hang a DS, on both CPUs, one per press. Everything else is saved first |

The first line after the title gives how many tests match NooDS U and how many differ. In the differences, the line under a
test's name gives the result word that differs, then `R` for this DS and `E` for NooDS U. Words 0-5 are what the test
recorded, and 6-11 are the first six words of its data afterwards. `tests.S` says what each test does.

If it hangs, the third line says which ARM9 test was running. A stuck ARM7 is reported on screen after about 10 seconds. A
test that causes a CPU exception shows `DEADDEAD` in word 3, with the mode and return address in words 4 and 5.

## The shared WRAM test

`build/noodsu-wram.nds` is a second ROM, for a DSi in DSi mode. It settles what the top byte of MBK9 does, which nothing
documents: cart headers set it with WRAMCNT, and the DSi menu sets it to 0xFC. On a DSi it reads back as 0 and does
nothing, and every block follows WRAMCNT as on a DS, which is what NooDS U does.

It runs by itself and saves `noodsu-wramtest.bin` like the other ROM. To see what it found, and how that compares with
NooDS U:

```
tests/hardware/wram.py noodsu-wramtest.bin [NooDS U's]
```

NooDS U's comes from a RAM dump, `tests/jit/game.sh tests/hardware/build/noodsu-wram.nds <system files folder> 300 --modes
interp dsi ram=wram.bin`, which `wram.py` reads too.

Each of its six phases sets WRAMCNT, maybe writes MBK9, then switches WRAMCNT between 0 and 3. Both CPUs then read each
4KB block at 0x3000000, write a marker and read it back. On screen and from `wram.py`, a digit means a block still holds
what the ARM9 wrote to that block before the switch, `W` that it could be written but held nothing from before, and `-`
that it couldn't be written.

It asks the ARM7 for one short thing at a time, the way the instruction test does, and the ARM9 reaches shared WRAM by
DMA, since libnds gives the ARM9's protection unit no region there. The ARM7 looks with interrupts off and from main RAM,
since with WRAMCNT at 0 it sees its own WRAM there, and puts back each word it writes. It keeps a copy of shared WRAM in
main RAM and puts it back at the end, with MBK9 and WRAMCNT as they were. It doesn't start if shared WRAM isn't all the
ARM7's, or if new WRAM is mapped over 0x3000000, and says how many words of shared WRAM weren't 0 before it started.

If it hangs, the third line gives the ARM9's step and the last command the ARM7 finished. If the ARM7 stops answering for
2 seconds, it says so and gives shared WRAM back to the ARM7.

## The NDMA test

`build/noodsu-ndma.nds` is a third ROM, for a DSi in DSi mode. It settles what GBATEK leaves open about the DSi's NDMA channels,
and what nothing else here tests: what each register keeps, what the address steps do including the reserved one, totals that
aren't a whole number of blocks, what's left in the registers afterwards, which scanlines V-blank, H-blank and display sync
transfers happen on, and timers and GXFIFO as triggers.

Each test runs on the ARM9 and then the ARM7, with interrupts off, on channel 2. Timers libnds already has running are skipped
and shown as `busy`. The DS cart trigger isn't tested, since that needs a real card in the slot.

It runs by itself and saves `noodsu-ndmatest.bin` like the other ROMs. To see what it found, and how that compares with NooDS U:

```
tests/hardware/ndma.py noodsu-ndmatest.bin [NooDS U's]
```

NooDS U's comes from a RAM dump, `tests/jit/game.sh tests/hardware/build/noodsu-ndma.nds <system files folder> 900 --modes interp
dsi ram=ndma.bin`, which `ndma.py` reads too. If it hangs, the third line says which test on which CPU. What each result word
means is in the comments in `ndma_tests.c`.

## The 3D timing test

`build/noodsu-rdlines.nds` is a fourth ROM, for a DS or a DSi in either mode. It settles how long the 3D engine takes to
render, through the two things it reports: RDLINES_COUNT, the fewest lines its 48-line buffer held during a frame, and
DISP3DCNT bit 12, set when the buffer ran dry. Nothing else documents the costs; melonDS's unmerged model guesses them.

Each scene in `rdlines.h` draws a number of flat quads of a given width and height. It's swapped in after an empty scene and
watched line by line for 6 frames, recording every change of RDLINES_COUNT and every time bit 12 is set, which is then
acknowledged. Four overloaded scenes are captured with display capture too, to show what the DS draws when it runs behind.
The 3D is on the top screen and the results on the bottom one, as the steady RDLINES_COUNT with `F` if bit 12 was set.

It saves `noodsu-rdlinestest.bin` like the other ROMs; in DS mode on a DSi, that may only work through a flashcart. To see
what it found, and how that compares with NooDS U:

```
tests/hardware/rdlines.py noodsu-rdlinestest.bin [NooDS U's] [--lines] [--images folder]
```

`rdlines-realds.bin` is what a real DS saved; a DSi gave the same in both modes. A model of the render timing that matched
all of it was added in 44edc7a and taken out again in a revert, since no game tested read RDLINES_COUNT or relied on bit 12,
and it cost time on nearly every frame; the revert can be reverted to bring it back.

`--lines` says, for each run of lines in a capture, which quad shows and how far right it reaches, and `--images` writes the
captures as PPM files. NooDS U's comes from a RAM dump, `tests/jit/game.sh tests/hardware/build/noodsu-rdlines.nds <system
files folder> 1200 --modes interp ram=rdlines.bin`, adding `dsi` for DSi mode.

## The display FIFO test

`build/noodsu-fifo.nds` is a fifth ROM, for a DS or a DSi in either mode. It settles what GBATEK leaves open about the main
memory display FIFO: what the display and display capture take from it when DMA fills it 4, 8 or 2 words at a time, in
halfwords, all at once, or not at all, what CPU writes of each size do, whether DMA timing 4 and NDMA mode 9 fire when nothing
uses the FIFO or when started mid-frame, which lines they fire on and how often, and whether V-blank stops them.

Each scene in `fifo.h` starts in a V-blank and runs for 2 frames with interrupts off. Most capture the FIFO with display
capture's source B in the first frame, which shows exactly what it gave each pixel; the rest record VCOUNT with a second DMA
on the same trigger. The top screen shows the FIFO in display mode 3, and the bottom one lists the scenes as they finish. The
NDMA scenes only run in DSi mode.

It saves `noodsu-fifotest.bin` like the other ROMs; in DS mode on a DSi, that may only work through a flashcart. To see what
it found, and how that compares with NooDS U:

```
tests/hardware/fifo.py noodsu-fifotest.bin [NooDS U's] [--images folder]
```

NooDS U's comes from a RAM dump, `tests/jit/game.sh tests/hardware/build/noodsu-fifo.nds <system files folder> 400 --modes
interp ram=fifo.bin`, adding `dsi` for DSi mode. `--images` writes each capture as a PPM file.

A real DSi in DSi mode settled it, and NooDS U now matches 15 of its 16 scenes:

- DMA timing 4 is requested every 8 pixels, 32 times a line, and only while the FIFO is shown or captured. Only channels
  already started when the frame starts answer, and V-blank stops them. NDMA mode 9 is the same, except that V-blank doesn't
  stop it.
- A repeating DMA sends at most 16 bytes a request, carrying on with the rest of its count at the next, so 8 words at a time
  still works. Each half-word from DMA is a pixel, though half-word writes from the CPU only move on with the top half.
- Its writes land between the display reading pixels 8n + 2 and 8n + 3, and the display shows whatever each of the ring's 16
  places last held, so 2 words a request shows a mix of new and older pixels.
- Capture blending rounds to the nearest, and a source B pixel with bit 15 clear blends as black.
- `BIG` is the one that differs: a DMA without repeat sends its whole count at the first request, as fast as the bus allows
  while the display reads, which NooDS U does all at once. Two pixels of `COUNT2` also came out 16 pixels older on the DSi,
  which looks like bus timing.

## The DSi microphone test

`build/noodsu-mic.nds` is a sixth ROM, for a DSi in DSi mode. It settles what GBATEK leaves open about the DSi's microphone
FIFO (`MIC_CNT`, `MIC_DATA`) and `SNDEXCNT`: which register bits stick, what the four data formats do, when the first sample
arrives and whether the sample clock runs freely, when each status bit and the interrupt come, what reads of each size take
from the FIFO and what an empty FIFO reads, what turning it off and on or clearing it keeps, how NDMA mode 0xC fetches blocks
of 1 to 16 words, and what the samples look like with the microphone muted and unmuted in the touchscreen controller.

Only the ARM7 has the microphone, so the ARM9 asks it for one test at a time, which runs with interrupts off and timers 0 and
1 counting cycles. Most tests are timelines: the time of every change of the FIFO's empty, half, full and overrun bits and of
IF2 bit 14, while nothing reads the FIFO, then what reading it gives. SNDEXCNT and the touchscreen controller are put back as
they were afterwards. Hold the DSi still and quiet, or talk, either works; the samples are shown but not compared.

It saves `noodsu-mictest.bin` like the other ROMs. To see what it found, and how that compares with NooDS U:

```
tests/hardware/mic.py noodsu-mictest.bin [NooDS U's]
```

NooDS U's comes from a RAM dump, `tests/jit/game.sh tests/hardware/build/noodsu-mic.nds <system files folder> 600 --modes interp
dsi ram=mic.bin`, which `mic.py` reads too. Timeline events are `cycles(samples of 1024 cycles):status`, with `E`, `H`, `F`, `O`
for empty, half full, full and overrun, and `I` for IF2 bit 14. If it hangs, the third line says which test. What each result
word means is in the comments in `mic_tests.c`.

A real DSi settled most of it, and NooDS U now does the same:

- The sample clock runs freely, so the first sample comes 0 to 1 frames after turning the microphone on, plus however long
  the format takes to fill a word. Each frame has a left slot and a right slot half a 32.73kHz or 47.61kHz frame later, even
  when the rate is divided. Format 0 takes both, starting from a left slot, format 1 only the right, 2 only the left, and 3
  none, as calico's names for the bits say. In mono a word needs 2 samples before the FIFO stops reading as empty.
- IRQ modes 1 and 3 fire when the FIFO becomes half full, and again only after it drops below that; mode 2 fires when it
  overruns, not when it becomes full.
- NDMA is asked for a block when the FIFO becomes half full, and not again until the FIFO has been empty, so blocks of 8 or
  16 words carry on and smaller ones stop after the first. A channel turned on after the FIFO is already half full never starts.
- Reads of any size, at either half of `MIC_DATA`, take a whole word. Turning the microphone off keeps the FIFO and an
  overrun, and clearing only works if it was off before the write.

- In stereo the right slot repeats the left slot's sample, so both halves of each word are the same; in mono the older
  sample is in the low half.
- An empty FIFO reads the last word read, again and again, even after it's cleared.
- Muting the microphone in the touchscreen controller doesn't zero the samples; they drift slowly, like an input settling.
  The first run read FFFF for every sample, since calico's touchscreen controller calls do nothing without the SPI lock.

## Building it

```
tests/hardware/build.sh [results from NooDS U]
```

It builds all six ROMs, and needs devkitPro's `nds-dev`. The results are NooDS U's for the instruction test's CPUs, as 2 x
110 x 12 little-endian words, which go in a table of the same size either way, so the code doesn't move. Build once without
them, run that in NooDS U to get them, and build again. On the Mac, `tests/jit/game.sh` gets them:

```
tests/jit/game.sh tests/hardware/build/noodsu-test.nds <system files folder> 600 --modes interp start=30 results=results.bin
```

Get them in the mode the ROM will run in on the console, adding `dsi` to that for a DSi in DSi mode. Tests that cause an
exception record a return address in the BIOS's handler, which differs between the DS and DSi BIOS.

## What it doesn't test

Things that would need a way back from jumping somewhere unknown: `MUL`, `LDRH` or `LDRD` with the PC as the destination, `LDM`
with an empty list, and writeback to the PC. Also most of CP15, cycle counts, undefined encodings, and ARMv5 instructions on the
ARM7.
