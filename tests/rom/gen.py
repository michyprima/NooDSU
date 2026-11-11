#!/usr/bin/env python3
# Generates the assembly for every instruction form the ROM checks and times
# Usage: gen.py <cases.S> <cases.h>
import sys

cases = [] # (name, cpu, thumb, mode, region, lines); cpu is 9, 7 or None for both
times = [] # (name, cpu, thumb, setup, unit, between)
SHIFTS = ['LSL #0', 'LSL #3', 'LSL #31', 'LSR #1', 'LSR #32', 'ASR #4', 'ASR #32', 'ROR #5', 'RRX']
CONDS = 'eq ne cs cc mi pl vs vc hi ls ge lt gt le'.split()
MAIN, SLOW, DTCM, ITCM, WRAM = 'DATA_MAIN', 'DATA_SLOW', 'DATA_DTCM', 'DATA_ITCM', 'DATA_WRAM'
IRQ, FIQ = 0x12, 0x11

def case(name, *lines, cpu=None, thumb=False, mode=0x1F, region=None):
    cases.append((name, cpu, thumb, mode, region, lines))
def asm(text, **options):
    case(text, text, **options)
def time(name, setup, unit, between='', cpu=None, thumb=False):
    times.append((name, cpu, thumb, setup, unit, between))

# ARM data processing, with every kind of second operand
ALU3 = 'and eor sub rsb add adc sbc rsc orr bic'.split()
for s in ('', 's'):
    for op in ALU3:
        asm(f'{op}{s} r0, r1, #0x7F')
        asm(f'{op}{s} r0, r1, #0xFF000000')
        for shift in SHIFTS:
            asm(f'{op}{s} r0, r1, r2, {shift}')
        for shift in ('lsl', 'lsr', 'asr', 'ror'):
            asm(f'{op}{s} r0, r1, r2, {shift} r3')
        asm(f'{op}{s} r1, r1, r1')
        asm(f'{op}{s} r0, pc, r1')
        asm(f'{op}{s} r8, r9, r10', mode=FIQ)
    for op in ('mov', 'mvn'):
        asm(f'{op}{s} r0, #0x7F')
        asm(f'{op}{s} r0, #0xFF000000')
        for shift in SHIFTS:
            asm(f'{op}{s} r0, r1, {shift}')
        for shift in ('lsl', 'lsr', 'asr', 'ror'):
            asm(f'{op}{s} r0, r1, {shift} r3')
        asm(f'{op}{s} r0, pc')
        asm(f'{op}{s} r8, r9', mode=FIQ)
for op in 'tst teq cmp cmn'.split():
    asm(f'{op} r1, #0x7F')
    asm(f'{op} r1, #0xFF000000')
    for shift in SHIFTS:
        asm(f'{op} r1, r2, {shift}')
    for shift in ('lsl', 'lsr', 'asr', 'ror'):
        asm(f'{op} r1, r2, {shift} r3')
    asm(f'{op} pc, r1')
for cond in CONDS:
    asm(f'mov{cond} r0, #1')
    asm(f'adds{cond} r0, r1, r2')
    asm(f'mul{cond} r0, r1, r2')
    asm(f'clz{cond} r0, r1', cpu=9)
    case(f'b{cond} taken or not', f'b{cond} 9f', 'mov r0, #0x55', '9:')
    case(f'bl{cond} taken or not', f'bl{cond} 9f', 'mov r0, #0x55', '9:')
    case(f'bx{cond} ARM', 'ldr r1, =9f', f'bx{cond} r1', 'mov r0, #0x55', '9:')
    case(f'ldr{cond} word', f'ldr r1, ={MAIN}', f'ldr{cond} r0, [r1, #4]', region=MAIN)
    case(f'str{cond} word', f'ldr r1, ={MAIN}', f'str{cond} r0, [r1, #4]', region=MAIN)
    case(f'ldm{cond} list', f'ldr r1, ={MAIN}', f'ldm{cond} r1!, {{r2, r3}}', region=MAIN)
    case(f'pop{cond} into pc', f'ldr r1, ={MAIN}', 'ldr r2, =9f', 'str r2, [r1]', f'ldr{cond} pc, [r1]', 'mov r0, #0x55', '9:', region=MAIN)
    case(f'add{cond} pc', f'add{cond} pc, pc, #0', 'mov r0, #0x55', 'mov r1, #0x66')
    case(f'msr{cond} flags', f'msr{cond} cpsr_f, r1')
    case(f'swp{cond}', f'ldr r1, ={MAIN}', f'swp{cond} r0, r2, [r1]', region=MAIN)

# ALU writes to the PC
for text in ('add pc, pc, #0', 'mov pc, pc', 'sub pc, pc, #4', 'add pc, pc, r9, LSL #2', 'bic pc, pc, #1',
    'orr pc, pc, #0', 'rsb pc, r9, pc', 'adc pc, pc, #0', 'add pc, pc, r9, LSR r9'):
    case(text, 'mov r9, #0', text, 'mov r0, #0x55', 'mov r1, #0x66', 'mov r2, #0x77')
case('misaligned mov pc', 'ldr r1, =9f + 2', 'mov pc, r1', 'mov r0, #0x55', '9:')
case('jump table', 'mov r9, #1', 'cmp r9, #2', 'addls pc, pc, r9, LSL #2', 'b 8f', 'b 7f', 'b 6f', 'b 5f', '8:', 'mov r0, #1', 'b 9f', '7:', 'mov r0, #2', 'b 9f',
    '6:', 'mov r0, #3', 'b 9f', '5:', 'mov r0, #4', '9:')
for text in ('subs pc, r14, #0', 'movs pc, r14', 'adds pc, r14, #0'):
    for mode, name in ((IRQ, 'IRQ'), (FIQ, 'FIQ')):
        case(f'{text} {name}', 'mrs r1, cpsr', 'msr spsr_fsxc, r1', 'ldr r14, =9f', text, 'mov r0, #0x55', '9:', 'mrs r2, spsr', mode=mode)
case('subs pc into System', 'mrs r5, cpsr', 'ldr r14, =9f', 'mov r1, #0x9F', 'msr spsr_fsxc, r1', 'subs pc, r14, #0', '9:', 'mov r2, r14', 'mrs r3, cpsr', 'msr cpsr_c, r5', mode=IRQ)

# Multiplies
for text in ('mul r0, r1, r2', 'mla r0, r1, r2, r3', 'muls r0, r1, r2', 'mlas r0, r1, r2, r3', 'umull r0, r1, r2, r3', 'umlal r0, r1, r2, r3',
    'smull r0, r1, r2, r3', 'smlal r0, r1, r2, r3', 'umulls r0, r1, r2, r3', 'umlals r0, r1, r2, r3', 'smulls r0, r1, r2, r3', 'smlals r0, r1, r2, r3'):
    asm(text)
for a in 'bt':
    for b in 'bt':
        asm(f'smul{a}{b} r0, r1, r2', cpu=9)
        asm(f'smla{a}{b} r0, r1, r2, r3', cpu=9)
        asm(f'smlal{a}{b} r0, r1, r2, r3', cpu=9)
    asm(f'smulw{a} r0, r1, r2', cpu=9)
    asm(f'smlaw{a} r0, r1, r2, r3', cpu=9)
for op in 'qadd qsub qdadd qdsub'.split():
    asm(f'{op} r0, r1, r2', cpu=9)
    asm(f'{op} r1, r1, r1', cpu=9)
asm('clz r0, r1', cpu=9)
asm('clz r8, r9', cpu=9, mode=FIQ)

# Words and bytes, in memory the JIT reaches directly and memory it doesn't
def transfers(region, cpu):
    for op in ('ldr', 'str', 'ldrb', 'strb'):
        forms = [f'{op} r0, [r1]', f'{op} r0, [r1, #8]', f'{op} r0, [r1, #-8]', f'{op} r0, [r1, #8]!', f'{op} r0, [r1, #-8]!', f'{op} r0, [r1], #8', f'{op} r0, [r1], #-8',
            f'{op} r0, [r1, r2]', f'{op} r0, [r1, -r2]', f'{op} r0, [r1, r2]!', f'{op} r0, [r1, -r2]!', f'{op} r0, [r1], r2', f'{op} r0, [r1], -r2']
        for shift in ('LSL #2', 'LSR #1', 'ASR #1', 'ROR #1'):
            for sign in ('', '-'):
                forms += [f'{op} r0, [r1, {sign}r3, {shift}]', f'{op} r0, [r1, {sign}r3, {shift}]!', f'{op} r0, [r1], {sign}r3, {shift}']
        for text in forms:
            for extra in (0, 1, 2) if op in ('ldr', 'str') and text.endswith('[r1]') else (0,):
                case(f'{text} {region} +{extra}', f'ldr r1, ={region} + 0x40 + {extra}', 'mov r2, #16', 'mov r3, #16', text, region=region, cpu=cpu)
        case(f'{op} r1, [r1, #4]! {region}', f'ldr r1, ={region} + 0x40', f'{op} r1, [r1, #4]!', region=region, cpu=cpu)
        case(f'{op} r1, [r1], #4 {region}', f'ldr r1, ={region} + 0x40', f'{op} r1, [r1], #4', region=region, cpu=cpu)
        case(f'{op} r8, [r9, #4]! {region} FIQ', f'ldr r9, ={region} + 0x40', f'{op} r8, [r9, #4]!', region=region, cpu=cpu, mode=FIQ)
    for op in ('ldrh', 'strh', 'ldrsb', 'ldrsh'):
        forms = [f'{op} r0, [r1]', f'{op} r0, [r1, #8]', f'{op} r0, [r1, #-8]', f'{op} r0, [r1, #8]!', f'{op} r0, [r1, #-8]!', f'{op} r0, [r1], #8', f'{op} r0, [r1], #-8',
            f'{op} r0, [r1, r2]', f'{op} r0, [r1, -r2]', f'{op} r0, [r1, r2]!', f'{op} r0, [r1, -r2]!', f'{op} r0, [r1], r2', f'{op} r0, [r1], -r2']
        for text in forms:
            for extra in (0, 1):
                case(f'{text} {region} +{extra}', f'ldr r1, ={region} + 0x40 + {extra}', 'mov r2, #16', text, region=region, cpu=cpu)
        case(f'{op} r1, [r1, #4]! {region}', f'ldr r1, ={region} + 0x40', f'{op} r1, [r1, #4]!', region=region, cpu=cpu)
        case(f'{op} r0, [r1], #4 {region}', f'ldr r1, ={region} + 0x40', f'{op} r0, [r1], #4', region=region, cpu=cpu)
    for text in ('ldrd r2, [r1, #8]', 'ldrd r2, [r1, #8]!', 'ldrd r2, [r1], #8', 'strd r2, [r1, #8]', 'strd r2, [r1, #8]!', 'strd r2, [r1], #8', 'ldrd r6, [r1, r4]', 'strd r6, [r1, r4]'):
        if region != WRAM: case(f'{text} {region}', f'ldr r1, ={region} + 0x40', 'mov r4, #8', text, region=region, cpu=9)
    for text in ('ldrt r0, [r1], #4', 'strt r0, [r1], #4', 'ldrbt r0, [r1], #4', 'strbt r0, [r1], #4', 'ldrt r0, [r1], #-4', 'strt r0, [r1], #-4',
        'ldrt r0, [r1], r2', 'strt r0, [r1], r2', 'ldrbt r0, [r1], r2', 'strbt r0, [r1], r2', 'ldrt r0, [r1], -r2', 'ldrt r0, [r1], r2, LSL #2', 'strt r0, [r1], r2, LSR #1',
        'ldrt r1, [r1], #4', 'strt r1, [r1], #4'):
        case(f'{text} {region}', f'ldr r1, ={region} + 0x40', 'mov r2, #16', text, region=region, cpu=cpu)
    for op in ('ldr', 'str', 'ldrb', 'strb'):
        case(f'{op} from the PC', f'{op} r0, [pc, #8]' if op[0] == 'l' else 'mov r0, r0', 'b 9f', 'nop', '.word 0x12345678', '9:', cpu=cpu)
    case(f'swp {region}', f'ldr r1, ={region} + 0x40', 'swp r0, r2, [r1]', region=region, cpu=cpu)
    case(f'swpb {region}', f'ldr r1, ={region} + 0x41', 'swpb r0, r2, [r1]', region=region, cpu=cpu)
    case(f'swp misaligned {region}', f'ldr r1, ={region} + 0x41', 'swp r0, r2, [r1]', region=region, cpu=cpu)
    case(f'swp r0, r0 {region}', f'ldr r1, ={region} + 0x40', 'swp r0, r0, [r1]', region=region, cpu=cpu)
    # Lists: every addressing mode, across a page end, and with the base in the list
    for op in ('ldm', 'stm'):
        for mode in ('ia', 'ib', 'da', 'db'):
            for wb in ('', '!'):
                for regs in ('r2', 'r2, r3', 'r0, r2-r9', 'r0, r1, r2', 'r2, r3, r14'):
                    if wb and 'r1' in regs and 'r1,' in regs: continue
                    case(f'{op}{mode} r1{wb}, {{{regs}}} {region}', f'ldr r1, ={region} + 0x40', f'{op}{mode} r1{wb}, {{{regs}}}', region=region, cpu=cpu)
            case(f'{op}{mode} r1, {{r0, r1, r2}}! {region}', f'ldr r1, ={region} + 0x40', f'.word 0xE{"8" if mode in ("ia", "da") else "9"}{"A" if op == "stm" and mode in ("ia", "ib") else "2" if op == "stm" else "B" if mode in ("ia", "ib") else "3"}10007',
                region=region, cpu=cpu)
    case(f'ldm across a page {region}', f'ldr r1, ={region} + 0xFF8', 'ldmia r1!, {r2, r3, r4, r5}', region=region, cpu=cpu)
    case(f'stm across a page {region}', f'ldr r1, ={region} + 0xFF8', 'stmia r1!, {r2, r3, r4, r5}', region=region, cpu=cpu)
    case(f'ldm misaligned {region}', f'ldr r1, ={region} + 0x42', 'ldmia r1!, {r2, r3}', region=region, cpu=cpu)
    case(f'stm misaligned {region}', f'ldr r1, ={region} + 0x42', 'stmia r1!, {r2, r3}', region=region, cpu=cpu)
    case(f'stm with the PC {region}', f'ldr r1, ={region} + 0x40', 'stmia r1!, {r2, pc}', region=region, cpu=cpu)
    case(f'ldm with the PC {region}', f'ldr r1, ={region} + 0x40', 'ldr r2, =9f', 'str r2, [r1, #4]', 'ldmia r1!, {r3, pc}', 'mov r0, #0x55', '9:', region=region, cpu=cpu)
    case(f'ldm into THUMB {region}', f'ldr r1, ={region} + 0x40', 'ldr r2, =8f + 1', 'str r2, [r1, #4]', 'ldmia r1!, {r3, pc}', 'mov r0, #0x55', 'b 9f', '.thumb', '8:', 'movs r0, #0x33', '.p2align 2', 'bx pc', 'nop', '.arm', '9:', region=region, cpu=9)
    for text in ('ldr pc, [r1]', 'ldr pc, [r1, #4]', 'ldr pc, [r1, #4]!', 'ldr pc, [r1], #4', 'ldr pc, [r1, r3]', 'ldr pc, [r1, r3]!', 'ldr pc, [r1], r3'):
        case(f'{text} {region}', f'ldr r1, ={region} + 0x40', 'mov r3, #4', 'ldr r2, =9f', f'str r2, [r1, #{4 if "[r1," in text else 0}]', text, 'mov r0, #0x55', '9:', region=region, cpu=cpu)
    case(f'ldr pc misaligned {region}', f'ldr r1, ={region} + 0x41', 'ldr r2, =9f', 'bic r3, r1, #3', 'mov r4, r2, ROR #24', 'str r4, [r3]', 'ldr pc, [r1]', 'mov r0, #0x55', '9:', 'b 8f', '8:', region=region, cpu=cpu)
    case(f'ldr pc into THUMB {region}', f'ldr r1, ={region} + 0x40', 'ldr r2, =8f + 1', 'str r2, [r1]', 'ldr pc, [r1]', 'mov r0, #0x55', 'b 9f', '.thumb', '8:', 'movs r0, #0x33', '.p2align 2', 'bx pc', 'nop', '.arm', '9:', region=region, cpu=9)

for region in (MAIN, SLOW, DTCM, ITCM):
    transfers(region, None if region == MAIN else 9)
transfers(WRAM, 7)

# Lists with a mode's own registers, which only exist outside User and System
for mode, name in ((IRQ, 'IRQ'), (FIQ, 'FIQ')):
    for text in ('ldmia r1, {r8, r9, r13, r14}^', 'stmia r1, {r8, r9, r13, r14}^'):
        case(f'{text} {name}', f'ldr r1, ={MAIN} + 0x40', 'mrs r2, cpsr', 'msr spsr_fsxc, r2', text, 'nop', region=MAIN, mode=mode)
    case(f'ldm with S into the PC {name}', f'ldr r1, ={MAIN} + 0x40', 'mrs r2, cpsr', 'msr spsr_fsxc, r2', 'ldr r3, =9f', 'str r3, [r1]', 'ldmia r1, {pc}^', 'mov r0, #0x55', '9:', region=MAIN, mode=mode)

# Branches
case('b', 'b 9f', 'mov r0, #0x55', '9:')
case('b back', 'b 8f', '7:', 'b 9f', '8:', 'b 7b', '9:')
case('bl', 'bl 9f', 'mov r0, #0x55', '9:', 'mov r1, r14')
case('bx ARM', 'ldr r1, =9f', 'bx r1', 'mov r0, #0x55', '9:')
case('bx misaligned ARM', 'ldr r1, =9f + 2', 'bx r1', 'mov r0, #0x55', '9:')
case('bx pc', 'bx pc', 'mov r0, #0x55', 'mov r1, #0x66')
case('blx reg ARM', 'ldr r1, =9f', 'blx r1', 'mov r0, #0x55', '9:', 'mov r2, r14', cpu=9)
case('bx into THUMB', 'ldr r1, =8f + 1', 'bx r1', 'mov r0, #0x55', 'b 9f', '.thumb', '8:', 'movs r0, #0x33', '.p2align 2', 'bx pc', 'nop', '.arm', '9:')
case('blx reg into THUMB', 'ldr r1, =8f + 1', 'blx r1', 'mov r0, #0x55', 'b 9f', '.thumb', '8:', 'movs r0, #0x33', '.p2align 2', 'bx pc', 'nop', '.arm', '9:', 'mov r2, r14', cpu=9)
case('blx label', 'blx 8f', 'mov r0, #0x55', 'b 9f', '.thumb', '.p2align 2', '8:', 'movs r0, #0x33', '.p2align 2', 'bx pc', 'nop', '.arm', '9:', 'mov r2, r14', cpu=9)
case('blx label +2', 'blx 8f', 'mov r0, #0x55', 'b 9f', '.thumb', '.p2align 2', 'nop', '8:', 'movs r0, #0x44', '.p2align 2', 'bx pc', 'nop', '.arm', '9:', 'mov r2, r14', cpu=9)

# Status registers
for text in ('mrs r0, cpsr', 'msr cpsr_f, r1', 'msr cpsr_f, #0xF0000000', 'msr cpsr_f, #0x60000000', 'msr cpsr_fs, r1', 'msr cpsr_fx, r1'):
    asm(text)
asm('mrs r0, spsr', mode=IRQ)
asm('mrs r8, spsr', mode=FIQ)
for text in ('msr spsr_c, r1', 'msr spsr_f, r1', 'msr spsr_fc, r1', 'msr spsr_fsxc, r1', 'msr spsr_s, r1', 'msr spsr_x, r1', 'msr spsr_c, #0x1F', 'msr spsr_f, #0xF0000000'):
    for mode in (IRQ, FIQ):
        case(f'{text} mode {mode:X}', 'mov r0, #0', 'msr spsr_fsxc, r0', text, mode=mode)
    case(f'{text} System', text)
# Control byte: nothing changes, interrupt bits change, and the mode changes and comes back, with banked registers shown
case('msr cpsr_c same', 'mrs r1, cpsr', 'msr cpsr_c, r1', 'mrs r2, cpsr')
case('msr cpsr_fc same', 'mrs r1, cpsr', 'msr cpsr_fc, r1', 'mrs r2, cpsr')
for bits in (0xDF, 0x9F, 0x5F):
    case(f'msr cpsr_c #{bits:X}', f'msr cpsr_c, #{bits:#x}', 'mrs r1, cpsr', 'msr cpsr_c, #0xDF')
    case(f'msr cpsr_fc reg {bits:X}', f'ldr r2, ={0xF0000000 | bits:#x}', 'msr cpsr_fc, r2', 'mrs r1, cpsr', 'msr cpsr_c, #0xDF', 'mov r2, #0', 'msr cpsr_f, r2')
for bits, name in ((0xD2, 'IRQ'), (0xD1, 'FIQ'), (0xD3, 'Supervisor'), (0xD7, 'Abort'), (0xDB, 'Undefined')):
    case(f'mode change to {name}', 'mov r8, #1', 'mov r13, r13', 'mov r14, #2', f'msr cpsr_c, #{bits:#x}', 'mov r8, #3', 'mov r3, r13', 'mov r4, r14', 'mov r14, #5', 'mov r5, r8', 'msr cpsr_c, #0xDF', 'mov r6, r8', 'mov r7, r14')
case('invalid mode bit 4', 'msr cpsr_c, #0xCF', 'mrs r1, cpsr', 'msr cpsr_c, #0xDF')
case('msr flags then cond', 'msr cpsr_f, #0xF0000000', 'moveq r0, #1', 'movne r0, #2', 'msr cpsr_f, #0', 'moveq r1, #3', 'movne r1, #4')

# Coprocessor, where the cache operations do nothing and registers read back
for text in ('c7, c5, 0', 'c7, c6, 0', 'c7, c10, 4', 'c7, c14, 1', 'c7, c5, 1', 'c7, c10, 1'):
    asm(f'mcr p15, 0, r0, {text}', cpu=9)
for text in ('c0, c0, 0', 'c0, c0, 1', 'c1, c0, 0', 'c9, c1, 0', 'c9, c1, 1'):
    asm(f'mrc p15, 0, r0, {text}', cpu=9)
asm('mrc p15, 0, r8, c9, c1, 0', cpu=9, mode=FIQ)
case('mcr c9 same DTCM', 'mrc p15, 0, r1, c9, c1, 0', 'mcr p15, 0, r1, c9, c1, 0', 'mrc p15, 0, r0, c9, c1, 0', cpu=9)
case('mcr c7 from THUMB neighbours', 'mcr p15, 0, r0, c7, c5, 0', 'add r0, r1, r2', cpu=9)

# BIOS calls, through real exception entry and return
# The BIOS pushes to the System stack, which cases otherwise point at their results
case('swi Div', 'mov r11, sp', f'ldr sp, ={MAIN} + 0x800', 'mov r0, #100', 'mov r1, #7', 'swi #0x090000', 'mov sp, r11')
case('swi Sqrt', 'mov r11, sp', f'ldr sp, ={MAIN} + 0x800', 'mov r0, #100', 'swi #0x0D0000', 'mov sp, r11')

# THUMB
def thumb(text, *setup, **options):
    case('T ' + text, *setup, text, thumb=True, **options)
for op in ('lsls', 'lsrs', 'asrs'):
    for amount in (0, 1, 31) if op == 'lsls' else (1, 31, 32):
        thumb(f'{op} r0, r1, #{amount}')
for text in ('adds r0, r1, r2', 'subs r0, r1, r2', 'adds r0, r1, #3', 'subs r0, r1, #3', 'movs r0, #0x7F', 'cmp r1, #0x7F', 'adds r0, #0x7F', 'subs r0, #0x7F'):
    thumb(text)
for op in 'ands eors lsls lsrs asrs adcs sbcs rors tst negs cmp cmn orrs muls bics mvns'.split():
    thumb(f'{op} r0, r1')
    thumb(f'{op} r1, r1')
for text in ('add r0, r9', 'add r9, r0', 'add r8, r9', 'cmp r0, r9', 'cmp r9, r0', 'cmp r8, r9', 'mov r0, r9', 'mov r9, r0', 'mov r8, r9', 'add r0, pc', 'mov r0, pc', 'mov r0, r13', 'mov r0, r14'):
    thumb(text)
case('T cmp r0, pc', '.hword 0x4578', thumb=True)
thumb('add r8, r9', mode=FIQ)
thumb('mov r8, r9', mode=FIQ)
for offset in (0, 2):
    case(f'T add pc, r0 +{offset}', 'movs r0, #0', '.p2align 2', 'add pc, r0', 'movs r1, #0x11', 'movs r2, #0x22', 'movs r3, #0x33', thumb=True)
    case(f'T mov pc, r0 +{offset}', 'adr r0, 9f', '.p2align 2', 'mov pc, r0', 'movs r1, #0x11', '.p2align 2', '9:', 'movs r2, #0x22', thumb=True)
case('T bx into ARM', 'adr r0, 8f', 'bx r0', 'movs r1, #0x11', '.p2align 2', '8:', '.arm', 'mov r1, #0x22', 'b case_exit', '.thumb', thumb=True)
case('T bx thumb', 'adr r0, 9f', 'adds r0, #1', 'bx r0', 'movs r1, #0x11', '.p2align 2', '9:', 'movs r2, #0x22', thumb=True)
case('T blx reg', 'adr r0, 9f', 'adds r0, #1', 'blx r0', 'movs r1, #0x11', '.p2align 2', '9:', 'mov r3, r14', thumb=True, cpu=9)
case('T blx reg into ARM', 'adr r0, 8f', 'blx r0', 'movs r1, #0x11', '.p2align 2', '8:', '.arm', 'mov r3, r14', 'b case_exit', '.thumb', thumb=True, cpu=9)
case('T bx pc', '.p2align 2', 'bx pc', 'nop', '.arm', 'mov r1, #0x22', 'b case_exit', '.thumb', thumb=True)
case('T ldr literal', 'ldr r0, 9f', 'b 8f', '.p2align 2', '9:', '.word 0x12345678', '8:', thumb=True)
case('T add r0, pc imm', 'add r0, pc, #16', thumb=True)
case('T add r0, sp imm', 'add r0, sp, #16', thumb=True)
for region in (MAIN, SLOW, DTCM, ITCM, WRAM):
    cpu = None if region == MAIN else 7 if region == WRAM else 9
    for op in ('str', 'strh', 'strb', 'ldrsb', 'ldr', 'ldrh', 'ldrb', 'ldrsh'):
        for extra in (0, 1, 2):
            case(f'T {op} r0, [r1, r2] {region} +{extra}', f'ldr r1, ={region} + 0x40 + {extra}', 'movs r2, #4', f'{op} r0, [r1, r2]', thumb=True, region=region, cpu=cpu)
    for text in ('str r0, [r1, #4]', 'ldr r0, [r1, #4]', 'strb r0, [r1, #4]', 'ldrb r0, [r1, #4]', 'strh r0, [r1, #4]', 'ldrh r0, [r1, #4]'):
        for extra in (0, 1):
            case(f'T {text} {region} +{extra}', f'ldr r1, ={region} + 0x40 + {extra}', text, thumb=True, region=region, cpu=cpu)
    for text in ('stmia r1!, {r2, r3}', 'ldmia r1!, {r2, r3}', 'ldmia r1!, {r1, r2}', 'ldmia r1, {r1, r2}', 'stmia r1!, {r1, r2}', 'stmia r1!, {r0, r1}', 'stmia r1!, {r2}', 'ldmia r1!, {r0, r2, r3, r4, r5, r6, r7}'):
        case(f'T {text} {region}', f'ldr r1, ={region} + 0x40', f'.hword {0xC000 if text[0] == "s" else 0xC800} | (1 << 8) | ' + str(sum(1 << int(x[1:]) for x in text[text.index('{') + 1:text.index('}')].split(', '))) if 'r1, {' in text or '{r1' in text or 'r0, r1' in text else text, thumb=True, region=region, cpu=cpu)
    # The stack, which is any memory: the test keeps the real SP in r11 meanwhile
    for text in ('push {r0, r1}', 'push {r0, r14}', 'pop {r0, r1}', 'push {r0, r1, r2, r3, r4, r5, r6, r7, r14}', 'str r0, [sp, #8]', 'ldr r0, [sp, #8]', 'add sp, #16', 'sub sp, #16', 'add r0, sp, #8'):
        case(f'T {text} {region}', 'mov r11, sp', f'ldr r1, ={region} + 0x40', 'mov sp, r1', text, 'mov r10, sp', 'mov sp, r11', thumb=True, region=region, cpu=cpu)
    case(f'T pop into pc {region}', 'mov r11, sp', f'ldr r1, ={region} + 0x40', 'mov sp, r1', 'adr r2, 9f', 'adds r2, #1', 'str r2, [r1, #4]', 'pop {r3, pc}', 'movs r0, #0x55', '.p2align 2', '9:', 'mov r10, sp', 'mov sp, r11', thumb=True, region=region, cpu=cpu)
    if region != WRAM: case(f'T pop into ARM {region}', 'mov r11, sp', f'ldr r1, ={region} + 0x40', 'mov sp, r1', 'adr r2, 8f', 'str r2, [r1, #4]', 'pop {r3, pc}', 'movs r0, #0x55', '.p2align 2', '8:', '.arm', 'mov r10, sp', 'mov sp, r11', 'b case_exit', '.thumb', thumb=True, region=region, cpu=9)
for cond in CONDS:
    case(f'T b{cond}', f'b{cond} 9f', 'movs r0, #0x55', '9:', thumb=True)
case('T b', 'b 9f', 'movs r0, #0x55', '9:', thumb=True)
case('T bl', 'bl 9f', 'movs r0, #0x55', '9:', 'mov r1, r14', thumb=True)
case('T blx label', 'blx 8f', 'movs r0, #0x55', '.p2align 2', '8:', '.arm', 'mov r1, r14', 'b case_exit', '.thumb', thumb=True, cpu=9)
case('T blx label +2', 'nop', 'blx 8f', 'movs r0, #0x55', '.p2align 2', '8:', '.arm', 'mov r1, r14', 'b case_exit', '.thumb', thumb=True, cpu=9)
case('T swi Div', 'mov r11, sp', f'ldr r1, ={MAIN} + 0x800', 'mov sp, r1', 'movs r0, #100', 'movs r1, #7', 'swi #9', 'mov sp, r11', thumb=True)

# Code the emulator has to notice changing, and blocks entered partway
# The nops are for what a real CPU has already fetched
nops = ['nop'] * 4
case('code changes ahead in the block', 'ldr r1, =8f', 'ldr r2, =0xE3A00001', 'str r2, [r1]', *nops, '8:', 'mov r0, #2', 'mov r3, r0', 'ldr r2, =0xE3A00002', 'str r2, [r1]')
case('code changes in another block', 'ldr r1, =8f', 'ldr r2, =0xE3A00005', 'str r2, [r1]', 'bl 8f', 'mov r3, r0', 'ldr r2, =0xE3A00006', 'str r2, [r1]', 'bl 8f', 'mov r4, r0', 'b 9f', '8:', 'mov r0, #7', 'bx lr', '9:', 'ldr r2, =0xE3A00007', 'str r2, [r1]')
case('entered partway', 'bl 8f', 'mov r4, r0', 'mov r1, #1', 'b 7f', '8:', 'mov r0, #1', '7:', 'add r0, r0, #1', 'add r0, r0, #1', 'cmp r1, #1', 'bxne lr')
case('long run', *[f'add r{i % 8}, r{(i + 1) % 8}, r{(i + 2) % 8}' for i in range(40)])
case('long run with flags', *[f'adds r{i % 8}, r{(i + 1) % 8}, r{(i + 2) % 8}' for i in range(40)])
case('run of conditions', 'cmp r1, r2', *[f'{CONDS[i % 14]} r0, r0, #1'.replace(CONDS[i % 14], 'add' + CONDS[i % 14]) for i in range(28)])
case('T long run', *[f'adds r{i % 7}, r{(i + 1) % 7}, r{(i + 2) % 7}' for i in range(40)], thumb=True)

# Runs where the JIT, with fast timing, leaves out flags that nothing reads before they're set again, or only keeps the carry
case('flags set twice', 'adds r0, r1, r2', 'subs r3, r1, r2', 'ands r4, r1, r2', 'cmp r2, r1', 'movs r5, r1, LSR #3', 'adds r6, r1, #1')
case('carry through three words', 'adds r0, r1, r2', 'adcs r3, r4, r5', 'adcs r6, r7, r1', 'adc r0, r2, r3')
case('borrow through three words', 'subs r0, r1, r2', 'sbcs r3, r4, r5', 'rscs r6, r7, r1', 'sbc r0, r2, r3')
case('carry then a compare', 'cmp r1, r2', 'adc r0, r1, r2', 'cmn r2, r3', 'sbc r4, r1, r2', 'cmp r3, r1')
case('carry kept by logic', 'adds r0, r1, r2', 'ands r3, r1, r2', 'orrs r4, r1, r2', 'adc r5, r1, r2', 'adds r6, r1, r1')
case('carry through shifts', 'adds r0, r1, r2', 'movs r3, r1, LSR #1', 'adc r4, r1, r2', 'movs r5, r1, RRX', 'movs r6, r2, LSL r3', 'adc r7, r1, r2', 'cmp r1, r2')
case('overflow kept by logic', 'adds r0, r1, r2', 'movs r3, r1, LSL #1', 'tst r1, r2', 'addvs r4, r4, #1', 'adds r5, r1, r2', 'movs r6, r1, LSL #1')
case('flags read by conditions', 'adds r0, r1, r2', 'moveq r3, #1', 'subs r4, r1, r2', 'addcs r5, r5, #1', 'cmp r1, r2')
case('multiply flags set twice', 'muls r0, r1, r2', 'umulls r3, r4, r1, r2', 'adc r5, r1, r2', 'muls r6, r1, r2', 'cmp r1, r2')
case('flags before a store', 'adds r0, r1, r2', f'ldr r3, ={MAIN} + 0x80', 'str r0, [r3]', 'subs r4, r1, r2', 'ldr r5, [r3]', 'cmp r5, r1')
case('division step', 'adds r0, r0, r0', 'adcs r1, r1, r1', 'adcs r7, r7, r7', 'subs r0, r0, r2', 'sbcs r1, r1, r3', 'sbcs r7, r7, #0',
    'adds r4, r4, r4', 'adc r5, r5, r5', 'cmp r7, #0')
case('T flags set twice', 'adds r0, r1, r2', 'lsls r3, r1, #3', 'subs r4, r1, r2', 'movs r5, #7', 'eors r5, r1', 'cmp r2, r1', 'negs r6, r1',
    'adds r0, #1', thumb=True)
case('T carry through words', 'adds r0, r1, r2', 'adcs r3, r4', 'sbcs r5, r6', 'adcs r0, r1', thumb=True)
case('T carry kept by logic', 'adds r0, r1, r2', 'ands r3, r1', 'muls r4, r1', 'movs r5, #1', 'adcs r6, r1', 'lsrs r0, r1, #1', 'adcs r2, r0',
    'lsls r3, r4', 'sbcs r5, r1', thumb=True)
case('T flags before a store', 'adds r0, r1, r2', f'ldr r3, ={MAIN} + 0x80', 'str r0, [r3]', 'subs r4, r1, r2', 'ldr r5, [r3]', 'cmp r5, r1', thumb=True)

# What's timed: a unit repeated 8 times in a loop of a fixed count, with setup before and anything between repeats after
D = f'ldr r1, ={MAIN} + 0x80'
for text in ('mov r0, r1', 'mov r0, #1', 'add r0, r1, #1', 'add r0, r1, r2', 'add r0, r1, r2, LSL #3', 'add r0, r1, r2, LSL r3', 'eor r0, r1, r2, ROR #5',
    'adds r0, r1, r2', 'subs r0, r1, r2', 'ands r0, r1, r2', 'orrs r0, r1, r2', 'movs r0, r1, LSR #3', 'adds r0, r1, r2, LSL r3', 'cmp r1, r2', 'tst r1, r2', 'adc r0, r1, r2', 'adcs r0, r1, r2', 'rscs r0, r1, r2'):
    time(text, 'mov r1, #5\nmov r2, #3\nmov r3, #2', text)
for text in ('moveq r0, #1', 'movne r0, #1', 'addlt r0, r1, r2', 'addhi r0, r1, r2', 'addeq r0, r1, r2'):
    time(text + ' (taken or not)', 'mov r1, #5\nmov r2, #3\ncmp r1, r1', text)
for text in ('mul r0, r1, r2', 'mla r0, r1, r2, r3', 'muls r0, r1, r2', 'umull r0, r3, r1, r2', 'smlal r0, r3, r1, r2'):
    time(text, 'mov r1, #5\nmov r2, #3\nmov r3, #2', text)
for text in ('smulbb r0, r1, r2', 'smlabb r0, r1, r2, r3', 'smulwb r0, r1, r2', 'smlawb r0, r1, r2, r3', 'smlalbb r0, r3, r1, r2', 'qadd r0, r1, r2', 'qsub r0, r1, r2', 'qdadd r0, r1, r2', 'clz r0, r1'):
    time(text, 'mov r1, #5\nmov r2, #3\nmov r3, #2', text, cpu=9)
for text in ('ldr r0, [r1]', 'ldr r0, [r1, #4]', 'ldr r0, [r1, r2]', 'ldrb r0, [r1, #4]', 'ldrh r0, [r1, #4]', 'ldrsh r0, [r1, r2]', 'ldrsb r0, [r1, r2]', 'str r0, [r1, #4]', 'strb r0, [r1, #4]', 'strh r0, [r1, #4]', 'strh r0, [r1, r2]',
    'swp r0, r2, [r1]', 'ldmia r1, {r2, r3}', 'ldmia r1, {r2-r9}', 'stmia r1, {r2, r3}', 'stmia r1, {r2-r9}'):
    time(text, D + '\nmov r2, #4', text)
for text in ('ldr r0, [r1, #4]!', 'ldrh r0, [r1, #2]!', 'strh r0, [r1, #2]!', 'ldr r0, [r1], #4', 'str r0, [r1], #4', 'ldrh r0, [r1], #2', 'strh r0, [r1], #2', 'ldrsh r0, [r1], #2', 'ldrsb r0, [r1], #1', 'ldmia r1!, {r2, r3}', 'stmia r1!, {r2, r3}'):
    time(text, 'ldr r4, =' + MAIN + ' + 0x80\nmov r1, r4', text, 'mov r1, r4')
for text, cpu in (('ldrd r2, [r1, #8]', 9), ('strd r2, [r1, #8]', 9), ('ldrt r0, [r1], #0', None)):
    time(text, D, text, cpu=cpu)
time('ldr r0, [r1] slow memory', f'ldr r1, ={SLOW}', 'ldr r0, [r1]', cpu=9)
time('str r0, [r1] slow memory', f'ldr r1, ={SLOW}', 'str r0, [r1]', cpu=9)
time('ldr r0, [r1] DTCM', f'ldr r1, ={DTCM}', 'ldr r0, [r1]', cpu=9)
time('str r0, [r1] DTCM', f'ldr r1, ={DTCM}', 'str r0, [r1]', cpu=9)
time('b', '', 'b 1f\n1:')
time('bne taken', 'mvn r0, #0\nmovs r0, r0', 'bne 1f\n1:')
time('bl', '', 'bl 1f\n1:')
time('bx', '', 'adr r1, 1f\nbx r1\n1:')
time('blx reg', '', 'adr r1, 1f\nblx r1\n1:', cpu=9)
time('blx label and back', '', 'blx 2f\nb 1f\n.thumb\n.p2align 2\n2:\nbx pc\nnop\n.arm\nb 1f\n1:', cpu=9)
time('bx into THUMB and back', '', 'adr r1, 2f + 1\nbx r1\nb 1f\n.thumb\n.p2align 2\n2:\nbx pc\nnop\n.arm\n1:')
time('mov pc, r1', '', 'adr r1, 1f\nmov pc, r1\n1:')
time('add pc, pc, #0', '', 'add pc, pc, #0\nnop\nnop')
time('jump table', 'mov r2, #1', 'add pc, pc, r2, LSL #2\nnop\nb 1f\nb 1f\n1:')
time('ldr pc, [r1]', D, 'adr r2, 1f\nstr r2, [r1]\nldr pc, [r1]\n1:')
time('ldm into pc', D, 'adr r2, 1f\nstr r2, [r1]\nldmia r1, {pc}\n1:')
time('msr cpsr_f', 'mov r1, #0', 'msr cpsr_f, r1')
time('msr cpsr_c same', 'mrs r1, cpsr', 'msr cpsr_c, r1')
time('msr cpsr_c I toggles', 'mrs r1, cpsr\neor r2, r1, #0x40', 'msr cpsr_c, r2\nmsr cpsr_c, r1')
time('msr cpsr_c mode change', 'mov r0, #0', 'msr cpsr_c, #0xD2\nmsr cpsr_c, #0xDF')
time('msr spsr_f', 'msr cpsr_c, #0xD2\nmov r1, #0', 'msr spsr_f, r1', cpu=None)
time('mrs cpsr', '', 'mrs r0, cpsr')
time('mrs spsr', 'msr cpsr_c, #0xD2', 'mrs r0, spsr', cpu=None)
time('subs pc, lr', 'msr cpsr_c, #0xD2\nmrs r1, cpsr\nmsr spsr_fsxc, r1', 'adr lr, 1f\nsubs pc, lr, #0\n1:', cpu=None)
time('ldm with S', 'msr cpsr_c, #0xD2\n' + D, 'ldmia r1, {r8, r9}^\nnop')
time('stm with S', 'msr cpsr_c, #0xD2\n' + D, 'stmia r1, {r8, r9}^\nnop')
time('mcr c7', 'mov r0, #0', 'mcr p15, 0, r0, c7, c10, 4', cpu=9)
time('mrc c9', '', 'mrc p15, 0, r0, c9, c1, 0', cpu=9)
time('swi Div', '', 'mov r0, #100\nmov r1, #7\nswi #0x090000')
for text in ('movs r0, #1', 'adds r0, r1, r2', 'adds r0, r1, #1', 'subs r0, #1', 'cmp r1, #1', 'ands r0, r1', 'eors r0, r1', 'lsls r0, r1, #2', 'lsls r0, r3', 'muls r0, r1', 'mov r0, r9', 'add r0, r9', 'cmp r0, r9'):
    time('T ' + text, 'movs r1, #5\nmovs r2, #3\nmovs r3, #2', text, thumb=True)
for text in ('ldr r0, [r1, #4]', 'ldrb r0, [r1, #4]', 'ldrh r0, [r1, #4]', 'ldr r0, [r1, r2]', 'ldrsh r0, [r1, r2]', 'str r0, [r1, #4]', 'strb r0, [r1, #4]', 'strh r0, [r1, #4]', 'str r0, [sp, #4]', 'ldr r0, [sp, #4]'):
    time('T ' + text, D + '\nmovs r2, #4\nsub sp, #16', text, thumb=True)
for text in ('push {r0, r1}\npop {r0, r1}', 'push {r0, r1, r2, r3, r4}\npop {r0, r1, r2, r3, r4}'):
    time('T ' + text.replace('\n', ' '), '', text, thumb=True)
for text in ('stmia r1!, {r2, r3}', 'ldmia r1!, {r2, r3}'):
    time('T ' + text, 'ldr r4, =' + MAIN + ' + 0x80\nadds r1, r4, #0', text, 'adds r1, r4, #0', thumb=True)
time('T b', '', 'b 1f\n1:', thumb=True)
time('T bne taken', 'cmp r0, r0', 'bne 1f\nbeq 1f\n1:', thumb=True)
time('T beq not taken', 'movs r0, #1', 'beq 1f\n1:', thumb=True)
time('T bl', '', 'bl 1f\n1:', thumb=True)
time('T bx', '', 'adr r1, 1f\nadds r1, #1\nbx r1\n.p2align 2\n1:', thumb=True)
time('T blx label and back', '', 'blx 2f\nb 1f\n.p2align 2\n.arm\n2:\nadr r1, 3f + 1\nbx r1\n.thumb\n3:\n1:', thumb=True, cpu=9)
time('T mov pc, r1', '', 'adr r1, 1f\nadds r1, #1\nmov pc, r1\n.p2align 2\n1:', thumb=True)
time('T pop into pc', '', 'adr r1, 1f\nadds r1, #1\npush {r1}\npop {pc}\n.p2align 2\n1:', thumb=True)
time('T swi Div', '', 'movs r0, #100\nmovs r1, #7\nswi #9', thumb=True)
time('empty loop', '', '')
time('T empty loop', '', '', thumb=True)

def emit(out, lines):
    for line in lines:
        out.write('    ' + line + '\n')

with open(sys.argv[1], 'w') as out:
    out.write('@ Generated by gen.py\n.syntax unified\n.text\n')
    for i, (name, cpu, thumb, mode, region, lines) in enumerate(cases):
        guard = {9: 'ARM9', 7: 'ARM7'}.get(cpu)
        if guard: out.write(f'#ifdef {guard}\n')
        out.write(('.thumb\n.p2align 2\n.thumb_func\n' if thumb else '.arm\n.p2align 2\n') + f'.global case_{i}\ncase_{i}:\n')
        emit(out, lines)
        if thumb: emit(out, ('.p2align 2', 'bx pc', 'nop', '.arm'))
        emit(out, ('b case_exit', '.ltorg'))
        if guard: out.write('#endif\n')
    for i, (name, cpu, thumb, setup, unit, between) in enumerate(times):
        guard = {9: 'ARM9', 7: 'ARM7'}.get(cpu)
        if guard: out.write(f'#ifdef {guard}\n')
        out.write(f'.arm\n.p2align 2\n.global time_{i}\ntime_{i}:\n    push {{r4-r11, lr}}\n    mov r11, sp\n    mov r12, r0\n')
        if thumb: emit(out, ('adr r1, 90f + 1', 'bx r1', '.thumb', '.p2align 1', '90:', 'mov r7, r12'))
        emit(out, setup.split('\n') if setup else ())
        out.write('91:\n')
        for _ in range(8):
            emit(out, unit.split('\n') if unit else ())
        emit(out, between.split('\n') if between else ())
        if thumb:
            emit(out, ('subs r7, #1', 'beq 92f', 'b 91b', '92:', '.p2align 2', 'bx pc', 'nop', '.arm'))
        else:
            emit(out, ('subs r12, r12, #1', 'bne 91b'))
        emit(out, ('msr cpsr_c, #0xDF', 'mov sp, r11', 'pop {r4-r11, lr}', 'bx lr', '.ltorg'))
        if guard: out.write('#endif\n')

def guarded(items, prefix, row):
    text = ''
    for i, item in enumerate(items):
        cpu = item[1]
        guard = {9: 'ARM9', 7: 'ARM7'}.get(cpu)
        if guard: text += f'#ifdef {guard}\n'
        text += f'{row(i, item)}\n'
        if guard: text += f'#else\n{{0}},\n#endif\n'
    return text

with open(sys.argv[2], 'w') as out:
    out.write('// Generated by gen.py\nextern char ' + ', '.join(f'case_{i}[]' for i, c in enumerate(cases) if c[1] is None) + ';\n')
    for i, c in enumerate(cases):
        if c[1]:
            out.write(f'#ifdef ARM{c[1]}\nextern char case_{i}[];\n#endif\n')
    for i, t in enumerate(times):
        guard = {9: 'ARM9', 7: 'ARM7'}.get(t[1])
        if guard: out.write(f'#ifdef {guard}\n')
        out.write(f'extern void time_{i}(int);\n')
        if guard: out.write('#endif\n')
    regions = {None: 0, MAIN: 1, SLOW: 2, DTCM: 3, ITCM: 4, WRAM: 5}
    quote = lambda s: '"' + s.replace('\\', '\\\\').replace('"', '\\"') + '"'
    out.write('static const struct Case cases[] = {\n')
    # What a real CPU might not come back from, which only runs in emulators
    risky = ('misaligned mov pc', 'bx misaligned ARM', 'ldr pc misaligned')
    out.write(guarded(cases, 'case', lambda i, c: f'{{ case_{i} + {1 if c[2] else 0}, {c[3]}, {regions[c[4]]}, {int(c[0].startswith(risky))} }},'))
    out.write('};\nstatic const struct Timed timed[] = {\n')
    out.write(guarded(times, 'time', lambda i, t: f'{{ time_{i} }},'))
    out.write('};\n')

    # Names aren't guarded, so each CPU's build can name the other's
    out.write('static const char *const caseNames[] = {\n' + ''.join(quote(c[0]) + ',\n' for c in cases) + '};\n')
    out.write('static const char *const timedNames[] = {\n' + ''.join(quote(t[0]) + ',\n' for t in times) + '};\n')
assert len(cases) < 4000 and len(times) < 160, 'MAX_CASES or MAX_TIMED in rom.h is too small'
print(f'{len(cases)} cases, {len(times)} timed')
