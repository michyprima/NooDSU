#!/usr/bin/env python3
# Changes a copy of the core so it builds on a Mac, with generated code going to the test instead of being run
# Usage: patch.py <copy of src/core> [mutation number] [game], where a mutation plants a bug the tests should catch
# With game, generated code runs whole games through game.cpp instead of being checked a case at a time
import os, re, sys

root = sys.argv[1]
mutation = int(sys.argv[2]) if len(sys.argv) > 2 else 0
game = len(sys.argv) > 3 and sys.argv[3] == 'game'

def edit(path, *changes):
    path = root + '/' + path
    text = open(path).read()
    for old, new in changes:
        assert old in text, old
        text = text.replace(old, new)
    open(path, 'w').write(text)

edit('arm/interpreter.h', ('private:', 'public:'))
edit('arm/jit.cpp', ('#include <cstdlib>', '#include <algorithm>\n#include <vector>\n#include <cstdlib>'))
edit('defines.h', ('__asm__ volatile("dcbz 0, %0" :: "r"(data) : "memory");', ''))
edit('gpu/gpu.cpp',
    ('malloc_usable_size', 'malloc_size'), ('__os_snprintf', 'snprintf'),
    ('(uint32_t)drawer->getFramebuffer()', '(uint32_t)(uintptr_t)drawer->getFramebuffer()'),
    ('(uint32_t)fastTargets[engine]', '(uint32_t)(uintptr_t)fastTargets[engine]'), ('(uint32_t)drawer,', '(uint32_t)(uintptr_t)drawer,'))
text = open(root + '/gpu/gpu.cpp').read()
open(root + '/gpu/gpu.cpp', 'w').write('#include <malloc/malloc.h>\n' + text)
edit('gpu/gpu_3d_renderer.cpp', ('asm volatile("dcbz 0, %0" :: "r"(block) : "memory");', ';'))

# Clang assumes the shift by 32 in this loop never happens, so it would give different timing to GCC's build
edit('arm/interpreter_alu.cpp', ('(op2 < (-1 << (m << 3)) || op2 >= (1 << (m << 3))) && m < 4',
    'm < 4 && (op2 < (-1 << (m << 3)) || op2 >= (1 << (m << 3)))'), ('(op3 < (-1 << (m << 3)) || op3 >= (1 << (m << 3))) && m < 4',
    'm < 4 && (op3 < (-1 << (m << 3)) || op3 >= (1 << (m << 3)))'))

# C++ functions called from generated code become fixed addresses that the test watches
path = root + '/arm/jit_ppc.cpp'
text = open(path).read()
text = re.sub(r'static_assert\(sizeof\(JitCacheEntry\) == 16[^\n]*\n', '', text)
text = text.replace('e.call((void*)&Jit::interleaveCall);', 'e.call((void*)(uintptr_t)0x3F2000);')
text = text.replace('    emitStubs(e);\n    codeStart =', '    rigOverride(*this);\n    emitStubs(e);\n    codeStart =')
text = text.replace('#include "../core.h"', '#define private public\n#include "../core.h"\n#undef private\nvoid rigOverride(Jit &jit);', 1)
if game:
    # Each handler gets its own address so the game run knows which to call, and code goes to the emulator when it's
    # flushed and runs there
    text = text.replace('return (parts.ptr & 1 || parts.adj) ? nullptr : (void*)parts.ptr;',
        'return (void*)(uintptr_t)rigHandler(&func, sizeof(func), std::is_same<T, int (Interpreter::*)(uint16_t)>::value);')
    text = text.replace('    JS_START(flushStart);\n    if (start) DCFlushRange', '    rigCodeWritten(*this, start, size, patched);\n'
        '    JS_START(flushStart);\n    if (start) DCFlushRange')
    text = text.replace('#include <cstring>', '#include <cstring>\n#include <type_traits>', 1)
    text = text.replace('void rigOverride(Jit &jit);', 'void rigOverride(Jit &jit);\nuint32_t rigHandler(const void *func, size_t size, bool thumb);\n'
        'void rigCodeWritten(Jit &jit, uint8_t *start, size_t size, const std::vector<uint32_t*> &patched);', 1)
    edit('arm/jit.cpp', ('        ((void(*)(Interpreter*, EventQueue*, const uint32_t*, void*, uint32_t*, JitCacheEntry*))\n'
        '            codeEnter)(&arm, &core->events, limit, block->code, &core->globalCycles, cache);',
        '        rigEnter(*this, limit, block->code);'),
        ('#include <algorithm>\n', '#include <algorithm>\nclass Jit;\nvoid rigEnter(Jit &jit, const uint32_t *limit, void *code);\n'))
else:
    text = text.replace('return (parts.ptr & 1 || parts.adj) ? nullptr : (void*)parts.ptr;', 'return (void*)(uintptr_t)0x3F1000;')

# Bugs that should be caught, by which suite they're aimed at
MUTATIONS = [
    ('e.addi(8, 8, thumbCode ? 2 : 4);', 'e.addi(8, 8, 4);'), # 1: POP PC on the ARM7
    ('wordPc ? (pc & ~0x3) : pc', 'pc'), # 2: LDR from the PC isn't aligned
    ('if (thumbCode) e.rlwinm(6, 6, 0, 27, 25);\n', ''), # 3: BX doesn't go back to ARM
    ('(!thumbCode && !(op & BIT(25))', '(!(op & BIT(25))'), # 4: THUMB register shifts take 2 cycles
    ('case 0xE: return 0xE1D00000', 'case 0xE: return 0xE1900000'), # 5: BIC is ORR
    ('case 0x9: return 0xE2700000 | (rs << 16) | (rd << 12);', 'case 0x9: return 0xE2600000 | (rs << 16) | (rd << 12);'), # 6: NEG sets no flags
    ('        e.call(codeRefresh);\n        e.lwz(0, 20, 1);\n        e.mtlr(0);\n        e.addi(1, 1, 16);\n        e.blr();\n    }\n}', '        e.lwz(0, 20, 1);\n        e.mtlr(0);\n        e.addi(1, 1, 16);\n        e.blr();\n    }\n}'), # 7: memory slow path doesn't refresh
    ('        if (load) e.mr(8, 3);\n', ''), # 8: memory slow path loses a loaded value
    ('e.loadConst(R_PC, host->address + (index + 2) * (host->thumb ? 2 : 4) - host->entriesBehind[index]);', 'e.loadConst(R_PC, host->address + (index + 2) * (host->thumb ? 2 : 4));'), # 9: mid-block entry has the wrong PC
    ('block->links.push_back({ e.here(), target | thumbCond, nullptr });', 'block->links.push_back({ e.here(), target, nullptr });'), # 10: THUMB branch links to ARM
    ('    e.stw(3, offCycles, R_ARM);\n    e.stw(R_CYCLES, 0, R_GLOBAL);\n', '    e.stw(3, offCycles, R_ARM);\n'), # 11: a stop doesn't save the time
    ('    stepPc(e);\n    e.call(codePost[!isPure(raw', '    e.call(codePost[!isPure(raw'), # 12: the PC is stale at a check
    ('e.add(3, 3, R_BATCH);\n        e.li(R_BATCH, 0);', 'e.li(R_BATCH, 0);'), # 13: a run's cycles are lost
    ('    e.lwz(R_CYCLES, 0, R_GLOBAL);\n    e.lwz(4, 0, R_LIMIT);', '    e.lwz(4, 0, R_LIMIT);'), # 14: refresh doesn't reload the time
    ('e.rlwinm(R_TMP2, R_TMP1, 0, 0, 29);', 'e.mr(R_TMP2, R_TMP1);'), # 15: LDM/STM bases aren't aligned in the slow path
    ('case 0x7: // RSC\n            if (arith) e.subfe(5, 3, 4);', 'case 0x7: // RSC\n            if (arith) e.subfe(5, 4, 3);'), # 16: RSCS has its operands swapped
    ('e.call(codeFlags[(opc == 0x4 || opc == 0x5 || opc == 0xB) ? 2 : (opc == 0x3 || opc == 0x7) ? 4 : 3]);', 'e.call(codeFlags[(opc == 0x4 || opc == 0xB) ? 2 : (opc == 0x3 || opc == 0x7) ? 4 : 3]);'), # 17: ADCS has SBCS flags
    ('if (i + 1 == block->instrs.size()) e.stw(R_PC, offPc, R_ARM);\n        return;', 'return;'), # 18: a block ending in a conditional branch loses the PC
    ('block->links.push_back({ e.here(), (pc + ((int32_t)(raw << 21) >> 20)) | 1, nullptr });', 'block->links.push_back({ e.here(), (pc + ((int32_t)(raw << 21) >> 20)), nullptr });'), # 19: THUMB B links to ARM
    ('uint32_t *last = e.here() - 1;\n        if ((*last & 0xFFFF0000) == 0x38600000 && (*last & 0xFFFF) > 0) {\n            *last = (14 << 26) | (R_BATCH << 21) | (R_BATCH << 16) | ((*last & 0xFFFF) - 1);', 'uint32_t *last = e.here() - 1;\n        if ((*last & 0xFFFF0000) == 0x38600000 && (*last & 0xFFFF) > 0) {\n            *last = (14 << 26) | (R_BATCH << 21) | (R_BATCH << 16) | (*last & 0xFFFF);'), # 20: folded conditions count a cycle too many
    ('e.call((!block->thumb && cond == 0xF) ? reserved : handler);\n        e.call(codeRefresh);\n', 'e.call((!block->thumb && cond == 0xF) ? reserved : handler);\n'), # 21: handlers don't refresh
    ('        e.stw(R_CYCLES, 0, R_GLOBAL);\n        e.stw(R_CYCLES, offCycles, R_ARM);\n        e.mr(3, R_ARM);\n', '        e.mr(3, R_ARM);\n'), # 22: handlers don't see the time
    ('    if (block->thumb && (raw & 0xF800) == 0xE000) {\n        // Unconditional branches jump straight to their target like ARM ones\n        block->links.push_back({ e.here(), (pc + ((int32_t)(raw << 21) >> 20)) | 1, nullptr });\n        e.b(codeDispatch);\n    }\n    else if', '    if'), # 23: THUMB B doesn't link
    ('            e.rlwinm(5, 5, 0, 24, 31);\n            e.patchBc(store2);', '            e.patchBc(store2);'), # 24: MRS of a missing SPSR keeps the flags on the ARM9
    (' && reg != 0x070004 && reg != 0x070802;', ';'), # 25: MCR that halts has no effect
    ('e.rlwinm(6, 6, 0, 8, 31);\n        e.rlwimi(6, 4, 0, 0, arm7 ? 3 : 4);\n    }\n    e.stw(6, 0, 5);', 'e.rlwimi(6, 4, 0, 0, arm7 ? 3 : 4);\n    }\n    e.stw(6, 0, 5);'), # 26: MSR SPSR keeps bits that don't exist
    ('e.andi_(8, 8, 0x3F);', 'e.andi_(8, 8, 0x1F);'), # 27: MSR setting THUMB stays out of the interpreter
    ('e.and_(9, 9, 10);\n        e.or_(8, 8, 9);', 'e.and_(9, 9, 10);'), # 28: MSR doesn't see all interrupts
    ('e.cmpwi(7, 0x10);\n        uint32_t *user', 'e.cmpwi(7, 0x11);\n        uint32_t *user'), # 29: User mode MSR changes control bits
    ('    if (load) {\n        if (writeback) writeReg(e, 9, rn);\n', '    if (load) {\n'), # 30: Halfword loads lose writeback
    ('blxTarget(op, pc) | 1, nullptr', 'blxTarget(op, pc), nullptr'), # 31: BLX links to ARM
    ('e.li(3, 5);\n        return;', 'e.li(3, 3);\n        return;'), # 32: LDR into the PC is too quick
    ('+ (jump ? 2 : 0)', ''), # 33: ALU writes to the PC are too quick
    ('e.rlwinm(8, 8, 28, 4, 4);', 'e.li(8, 0);'), # 34: SMLA doesn't set Q
    ('e.addc(5, 5, 7);\n        e.adde(6, 6, 8);\n        writeReg(e, 5, rn);', 'e.add(5, 5, 7);\n        e.adde(6, 6, 8);\n        writeReg(e, 5, rn);'), # 35: SMLAL loses the carry
    ('e.srawi(5, 3, 31);\n        e.loadConst(6, 0x7FFFFFFF);', 'e.srawi(5, 4, 31);\n        e.loadConst(6, 0x7FFFFFFF);'), # 36: QSUB saturates the wrong way
]
# Bugs in the interpreter, which only the reference suite can see if the JIT has no native code for them
INTERPRETER_MUTATIONS = [
    ('arm/interpreter_alu.cpp', 'cpsr = (cpsr & ~0xF0000000) | (*op0 & BIT(31)) | ((*op0 == 0) << 30) | ((op2 >= *op0 &&', 'cpsr = (cpsr & ~0xC0000000) | (*op0 & BIT(31)) | ((*op0 == 0) << 30) | ((op2 >= *op0 &&'), # 37: RSCS can't clear C or V
    ('arm/interpreter_alu.cpp', ' | ((*op0 <= 0) << 29) |\n        ((op2 & *op0 & BIT(31)) >> 3);', ' | ((*op0 <= 0) << 29);'), # 38: NEG always clears V
    ('arm/interpreter_transfer.cpp', 'int Interpreter::ldmiaU(uint32_t opcode) { // LDMIA Rn, <Rlist>^\n    // User block load, post-increment without writeback; normal registers if branching\n    uint8_t m = bitCount[opcode & 0xFF] + bitCount[(opcode >> 8) & 0xFF];\n    uint32_t op0 = *registers[(opcode >> 16) & 0xF];\n    uint32_t **regs = &registers[(~opcode & BIT(15)) >> 11];', 'int Interpreter::ldmiaU(uint32_t opcode) { // LDMIA Rn, <Rlist>^\n    // User block load, post-increment without writeback; normal registers if branching\n    uint8_t m = bitCount[opcode & 0xFF] + bitCount[(opcode >> 8) & 0xFF];\n    uint32_t op0 = *registers[(opcode >> 16) & 0xF];\n    uint32_t **regs = &registers[0];'), # 39: LDM with S doesn't use user registers
    ('arm/interpreter_transfer.cpp', 'if ((opcode & BIT(16)) && (cpsr & 0x1F) != 0x10)\n        setCpsr', 'if (opcode & BIT(16))\n        setCpsr'), # 40: User mode MSR changes control bits
    ('arm/interpreter_transfer.cpp', '    if (spsr) setCpsr(*spsr);\n    flushPipeline();', '    if (spsr) setCpsr(*spsr);\n    cpsr |= (*registers[15] & !arm7) << 5;\n    flushPipeline();'), # 41: LDM with S takes THUMB from the PC
]
if mutation > len(MUTATIONS):
    file, old, new = INTERPRETER_MUTATIONS[mutation - len(MUTATIONS) - 1]
    edit(file, (old, new))
elif mutation:
    old, new = MUTATIONS[mutation - 1]
    assert old in text, old
    text = text.replace(old, new)
text += open(os.path.dirname(os.path.abspath(__file__)) + '/rig.inc').read()
open(path, 'w').write(text)
