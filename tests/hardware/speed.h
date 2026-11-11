// What the speed test's CPUs share: a block of words in main RAM, laid out the same for C, assembly and speed.py
// Every measurement is a count of timer ticks, 33.513982MHz on both CPUs, taken REPEATS times with interrupts off

#define S_MAGIC0 0 // "SPED"
#define S_MAGIC1 1 // "TEST"
#define S_STATUS 2 // What happened, from the STATUS_ values
#define S_STEP 3 // The ARM9's last step, which is also on screen
#define S_DONE7 4 // The last command the ARM7 finished, with its count in the low bits
#define S_DSI 5 // 1 in DSi mode
#define S_SCFG9 6 // SCFG_CLK and SCFG_EXT as the ARM9 found them, 0 in DS mode or when they're locked
#define S_CP15 8 // The ARM9's control register, data and code cache regions, write buffer regions, then its 8 regions
#define S_ADDR9 20 // Where the ARM9's code lines and data lines are
#define S_SCFG7 22 // SCFG_CLK and SCFG_EXT as the ARM7 found them
#define S_ADDR7 24 // Where the ARM7's loops are, one per place
#define S_LINES 27 // LINES and DLINES, which were 128 and 64 in the first results saved, from before this was kept
#define S_RESULTS9 32 // M9_COUNT measurements of REPEATS words
#define S_RESULTS7 84 // M7_COUNT measurements of REPEATS words
#define S_TOTAL 136

#define SPED_MAGIC0 0x44455053 // SPED
#define SPED_MAGIC1 0x54534554 // TEST

#define STATUS_DONE 1
#define STATUS_NO_ARM7 2 // The ARM7 didn't answer

#define REPEATS 4
#define LOOPS 4096 // Turns of every loop, and the count the BIOS waits are given
#define LINES 48 // 32-byte lines of code the ARM9 runs through, fewer than its code cache's 64 sets, so none can push
// another out: the cache picks which of a set's four lines to replace by turns, whether or not the others are in use
#define DLINES 24 // 32-byte lines of data the ARM9 reads or writes a word of, fewer than its data cache's 32 sets
#define NOT_RUN 0xFFFFFFFF // A measurement that was left out, like new WRAM's outside DSi mode

// The ARM9's measurements. HOP runs two instructions in each of LINES lines of code and SEQ all eight; READ and WRITE
// touch one word in each of DLINES lines of data. HOT is with everything in the caches, COLD right after emptying the
// cache it uses, and OFF with the code cache turned off
#define M9_EMPTY 0 // A function that only returns, which is what every other measurement also includes
#define M9_LOOP 1 // LOOPS turns of subs and bne, 4 cycles each from the cache
#define M9_BIOS 2 // The BIOS's wait loop, with LOOPS as its count
#define M9_HOP_HOT 3
#define M9_HOP_COLD 4
#define M9_HOP_OFF 5
#define M9_SEQ_HOT 6
#define M9_SEQ_COLD 7
#define M9_SEQ_OFF 8
#define M9_READ_HOT 9
#define M9_READ_COLD 10
#define M9_WRITE_HOT 11
#define M9_WRITE_COLD 12
#define M9_COUNT 13

// The ARM7's measurements: four in each place its code can be, counted as place * 4 + kind, then the BIOS's wait loop.
// LOAD and STORE add a read or a write of a word in the same place to every turn of the loop
#define PLACE_WRAM 0 // Its own WRAM at 0x37F8000, where DS software has most of its code
#define PLACE_TWL 1 // New WRAM at 0x37C0000, where DSi software has most of its code; only in DSi mode
#define PLACE_MAIN 2 // Main RAM
#define PLACES 3
#define KIND_EMPTY 0
#define KIND_LOOP 1
#define KIND_LOAD 2
#define KIND_STORE 3
#define KINDS 4
#define M7_BIOS 12
#define M7_COUNT 13

// What the ARM9 asks of the ARM7, one message, as in the shared WRAM test
#define COMMAND_RUN 1 // Record SCFG and where the loops are, then take every measurement
