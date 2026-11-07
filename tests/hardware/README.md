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

## Building it

```
tests/hardware/build.sh [results from NooDS U]
```

It needs devkitPro's `nds-dev`. The results are NooDS U's for both CPUs, as 2 x 110 x 12 little-endian words, which go in a
table of the same size either way, so the code doesn't move. Build once without them, run that in NooDS U to get them, and
build again. On the Mac, `tests/jit/game.sh` gets them:

```
tests/jit/game.sh tests/hardware/build/noodsu-test.nds <system files folder> 600 --modes interp start=30 results=results.bin
```

## What it doesn't test

Things that would need a way back from jumping somewhere unknown: `MUL`, `LDRH` or `LDRD` with the PC as the destination, `LDM`
with an empty list, and writeback to the PC. Also most of CP15, cycle counts, undefined encodings, and ARMv5 instructions on the
ARM7.
