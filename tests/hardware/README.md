# Instruction tests for a real DS

A ROM that settles how the CPUs really behave where the interpreter, the ARM manual and Unicorn disagree, or where the
manual doesn't promise anything. It runs each test on the ARM9 and ARM7, and shows where the DS differs from NooDS U, whose
results are built in.

## Running it

Copy `build/noodsu-test.nds` to a flashcart and start it. It doesn't use any libraries, the touch screen or the save.

| Button | What it does |
|---|---|
| L or R | Switch between the ARM9's and ARM7's results |
| Up and Down | Scroll a line |
| Left and Right | Scroll a page |
| A | Switch between only the differences and all results |
| START | Run the next test that could hang or restart a DS, on both CPUs, one per press. Everything else is on screen first, so take photos before this |

The first line after the title gives how many tests match NooDS U and how many differ. In the differences, the line under a
test's name gives the result word that differs, then `R` for this DS and `E` for NooDS U. Words 0-5 are what the test
recorded, and 6-11 are the first six words of its data afterwards. `tests.S` says what each test does.

If it hangs, the third line says which ARM9 test was running. A stuck ARM7 is reported on screen after about 10 seconds. If the program starts again, the second line's BOOTS goes above 1 and the third line says which test it restarted in. A test that causes a CPU exception shows `DEADDEAD` in word 3, with the mode and return address in words 4 and 5.

## Building it

```
tests/hardware/build.sh <any DS ROM, for the logo> [results from NooDS U]
```

It needs devkitARM. The results are what the core leaves at `0x02300000` and `0x02310000` after running the ROM and pressing
START, as 2 x 110 x 12 little-endian words. Build once without them, run that in NooDS U to get them, and build again; the code
doesn't move, since they only fill in a table.

## What it doesn't test

Things that would need a way back from jumping somewhere unknown: `MUL`, `LDRH` or `LDRD` with the PC as the destination, `LDM`
with an empty list, and writeback to the PC. Also CP15, cycle counts, undefined encodings, and ARMv5 instructions on the ARM7.
