// What the shared WRAM test's CPUs share: a block of words in main RAM, laid out the same for C and the ARM7's assembly
// Each phase sets WRAMCNT, maybe writes MBK9's top byte, then flips WRAMCNT between 0 and 3 and looks at shared WRAM's
// eight 4KB blocks from both CPUs; a look reads each block, writes a marker and reads it back

#define W_MAGIC0 0 // "WRAM"
#define W_MAGIC1 1 // "TEST"
#define W_STATUS 2 // What happened, from the STATUS_ values
#define W_STEP 3 // The ARM9's last step, which is also on screen
#define W_DONE7 4 // The last command the ARM7 finished, with its count in the top bits
#define W_PARAM 5 // MBK9 value for COMMAND_MBK9
#define W_MBK9_BACK 6 // MBK9 read back after COMMAND_MBK9
#define W_OUT7 7 // Where COMMAND_LOOK puts its 16 words
#define W_MARK7 8 // COMMAND_LOOK's marker, with the block number in the low byte
#define W_BACKUP 9 // Where the ARM7 keeps shared WRAM during the test
#define W_INIT9 10 // WRAMCNT, SCFG_EXT9, MBK6, MBK7, MBK8 as the ARM9 found them
#define W_USED 15 // How many words of shared WRAM weren't 0 at the start, which may be something else's
#define W_INIT7 16 // MBK9, SCFG_EXT7, WRAMSTAT, MBK6, MBK7, MBK8 as the ARM7 found them
#define W_PHASES 24
#define PHASE_WORDS 72
#define P_LOCK 0 // MBK9's top byte for the phase, or -1 to leave MBK9 alone
#define P_CNT 1 // WRAMCNT when MBK9 is written; it then flips to the other of 0 and 3
#define P_MBK9 2 // MBK9 read back after writing it
#define P_LOOK9_BEFORE 8 // The ARM9's look before the flip: 8 words read, then 8 read back after writing markers
#define P_LOOK9_AFTER 24 // The ARM9's look after the flip
#define P_LOOK7 40 // The ARM7's look after the flip
#define P_LOOK9_LAST 56 // The ARM9's look after the ARM7's
#define PHASES 6
#define W_TOTAL (W_PHASES + PHASES * PHASE_WORDS)

#define WRAM_MAGIC0 0x4D415257 // WRAM
#define WRAM_MAGIC1 0x54534554 // TEST

#define STATUS_DONE 1
#define STATUS_NO_ARM7 2 // The ARM7 didn't answer
#define STATUS_NOT_ARM7S 3 // Shared WRAM wasn't all the ARM7's at the start, so it couldn't be kept safe
#define STATUS_NWRAM 4 // New WRAM was mapped over the addresses the test uses

// What the ARM9 asks of the ARM7, one message at a time, as in the instruction test
#define COMMAND_INIT 1 // Record MBK9, SCFG_EXT7, WRAMSTAT and MBK6-8
#define COMMAND_BACKUP 2 // Copy shared WRAM to W_BACKUP
#define COMMAND_MBK9 3 // Write W_PARAM to MBK9
#define COMMAND_LOOK 4 // Look at the blocks
#define COMMAND_RESTORE 5 // Copy shared WRAM back from W_BACKUP

// Both CPUs look where the DSi menu does, which no new WRAM covers in DSi mode; the ARM7's new WRAM covers where libnds
// says shared WRAM is
#define SHARED_ADDRESS 0x03000000
#define LOOK_ADDRESS (SHARED_ADDRESS + 0x10)
