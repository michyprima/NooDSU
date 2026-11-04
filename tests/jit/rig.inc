
// Added to jit_ppc.cpp by patch.py, for the test to see what's static there
void (*rigSetup)(Jit &jit) = nullptr;
void rigOverride(Jit &jit) { if (rigSetup) rigSetup(jit); }
uint32_t rigArm(uint32_t op) { return thumbToArm(op); }
int rigKind(uint32_t op, bool thumb, bool arm7) {
    if (thumb && thumbBranch(op, arm7)) return 100;
    return nativeKind(thumb ? thumbToArm(op) : op, arm7);
}
bool rigMayJump(uint32_t op, bool thumb) { return mayJump(op, thumb); }
bool rigKindJumps(int kind) { return nativeJumps(kind); }
