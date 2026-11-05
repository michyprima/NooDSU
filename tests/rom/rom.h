// What the ROM's C code shares between the CPUs
#pragma once
#define REG32(a) (*(volatile unsigned*)(a))
#define REG16(a) (*(volatile unsigned short*)(a))

// The romtest build of NooDS U answers these; on anything else they read as something other than the magic number
#define HOST_MAGIC REG32(0x04FFF000)
#define HOST_MICROSECONDS REG32(0x04FFF004)
#define HOST_TEXT REG32(0x04FFF008)
#define HOST_LENGTH REG32(0x04FFF00C)
#define HOST_PROGRESS REG32(0x04FFF010)
#define HOST_DONE REG32(0x04FFF014)
#define HOST_DETAIL REG32(0x04FFF018)
#define MAGIC 0x54534F48

#define IME REG32(0x04000208)
#define IE REG32(0x04000210)
#define IF REG32(0x04000214)
#define IPCSYNC REG16(0x04000180)
#define BIT16 (1 << 16)
#define TM0CNT_L REG16(0x04000100)
#define TM0CNT_H REG16(0x04000102)
#define TM1CNT_L REG16(0x04000104)
#define TM1CNT_H REG16(0x04000106)

#define MAX_CASES 4000
#define MAX_TIMED 160
#define SETS 24 // Sets of register values and flags each case runs with
#define DETAIL_WORDS 32 // The registers and status after a case, then some of its memory

// Shared between the CPUs, in main RAM
#define COMMAND REG32(0x02380000)
#define FINISHED REG32(0x02380004)
#define PROGRESS7 REG32(0x02380008) // Which of the ARM7's cases is running, plus 1
#define DETAIL_CASE REG32(0x0238000C)
#define DETAIL_SET REG32(0x02380010)
#define HASHES7 ((volatile unsigned*)0x02390000)
#define SETS7 ((volatile unsigned short*)0x02394000)
#define CYCLES7 ((volatile unsigned*)0x023D0000)
#define DETAIL7 ((volatile unsigned*)0x023D1000)
#define SURVEY7 ((volatile unsigned char*)0x023D2000)

// Flags after every kind of multiply, for each of these operands, with the carry clear and then set
#define SURVEY_OPS 7
#define SURVEY_GRID 24
#define SURVEY_RANDOM 1024
#define SURVEY_PAIRS (SURVEY_GRID * SURVEY_GRID + SURVEY_RANDOM)
#define SURVEY_BYTES (SURVEY_OPS * SURVEY_PAIRS * 2)

// Where each CPU's results go
extern volatile unsigned *hashOut, *cyclesOut;
extern volatile unsigned short *setsOut;
extern volatile unsigned char *surveyOut;

void runAll(void);
void onProgress(int cpu, int index);
int caseCount(void);
int timedCount(void);
int caseRisky(int index);
int hostHere(void);
const char *caseName(int index);
const char *timedName(int index);
void caseDetail(int index, int set, volatile unsigned *out);
int saveStart(const char **name);
int saveCapacity(void);
int saveEnd(void);
void saveWord(unsigned value);
