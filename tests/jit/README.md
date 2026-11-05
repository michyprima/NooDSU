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
- **Long-running behavior.** These are single instructions and short blocks, not a game.

The JIT self-test on the console (`./build-wiiu-debug.sh jittest`) covers most of this, but turns batching off, so playing games
on the console is still needed for that.
