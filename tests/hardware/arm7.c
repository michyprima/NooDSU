#include "list.h"
void runNormal(int cpu);
void runRisky(int cpu, int index);
void onTest(int cpu, int index) {}

void arm7Main(void) {
    while (1) {
        unsigned command = COMMAND_ARM7;
        if (command == 1) {
            COMMAND_ARM7 = 0;
            runNormal(1);
            DONE_ARM7 = 1;
        }
        else if (command >= 2) {
            COMMAND_ARM7 = 0;
            runRisky(1, command - 2);
            DONE_ARM7 = command;
        }
    }
}
