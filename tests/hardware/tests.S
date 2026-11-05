@ Instruction tests that settle where the interpreter, the ARM manual and Unicorn disagree, assembled for both CPUs
@ Each test runs in System mode with interrupts off, with its results going to r11, and ends at test_done
@ Encodings the assembler would refuse are written as words, with what they are alongside

.arch armv5te
.syntax unified
.arm
.text

.macro RESULT index, reg
    str \reg, [r11, #(\index) * 4]
.endm

.macro FLAGS index
    mrs r10, cpsr
    str r10, [r11, #(\index) * 4]
.endm

@ Word-aligned code that runs differently in each state, setting r3 if it was reached in THUMB and going on in ARM
.macro LANDING
    .word 0xE1A0E003 @ ARM: MOV lr,r3; THUMB: B to the MOVS below
    b 9f
    .hword 0x0000
    .hword 0x2301 @ MOVS r3,#1
    .hword 0x4778 @ BX PC
    .hword 0x46C0 @ NOP
9:
.endm

@ Go to Supervisor mode with an SPSR in r1, without touching the flags
.macro TO_SVC
    msr cpsr_c, #0xD3
    msr spsr_fsxc, r1
.endm

.macro TEST name
    .global \name
    .type \name, %function
    .align 2
\name :
.endm

@ Tests that only make sense on the ARM9 are left empty on the ARM7, keeping the numbering the same on both
.macro ARM9_ONLY
#ifdef ARM7
    mvn r1, #0
    RESULT 0, r1
    b test_done
#endif
.endm

.global run_test
.type run_test, %function
.align 2
run_test: @ r0 = test, r1 = results
    push {r4-r11, lr}
    ldr r2, =saved_sp
    str sp, [r2]
    mov r11, r1
    ldr r2, =saved_out
    str r1, [r2]
    msr cpsr_cf, #0xDF
    mov r3, #0
    bx r0

.global test_done
test_done:
    msr cpsr_cxsf, #0xDF
    ldr r2, =saved_sp
    ldr sp, [r2]
    pop {r4-r11, lr}
    bx lr

.ltorg

.macro ARM7_ONLY @ These crash an ARM9
#ifdef ARM9
    mvn r1, #0
    RESULT 0, r1
    b test_done
#endif
.endm

@ ---- Flags from the two bugs already fixed, to confirm the fixes ----

TEST t_neg_min
    ldr r1, =0x80000000
    adr r2, 1f + 1
    bx r2
    .thumb
1:  negs r0, r1
    nop
    bx pc
    nop
    .arm
    RESULT 0, r0
    FLAGS 1
    b test_done

TEST t_neg_zero
    mov r1, #0
    adr r2, 1f + 1
    bx r2
    .thumb
1:  negs r0, r1
    nop
    bx pc
    nop
    .arm
    RESULT 0, r0
    FLAGS 1
    b test_done

TEST t_rscs_clear
    msr cpsr_f, #0x30000000 @ C and V set
    mov r1, #1
    mov r2, #2
    rscs r0, r1, r2 @ Result 1, no borrow and no overflow
    RESULT 0, r0
    FLAGS 1
    b test_done

TEST t_rscs_borrow
    msr cpsr_f, #0x30000000
    mov r1, #5
    mov r2, #2
    rscs r0, r1, r2 @ Borrows, which clears C
    RESULT 0, r0
    FLAGS 1
    b test_done

@ ---- LDM with S loading the PC: where the THUMB bit comes from ----

.macro LDM_RETURN name, spsr, offset, arm9only=0
TEST \name
    .if \arm9only
    ARM9_ONLY
    .endif
    ldr r1, =\spsr
    adr r0, 1f
    adr r2, 2f + \offset
    str r2, [r0]
    TO_SVC
    .word 0xE8D08000 @ LDMIA r0,{pc}^
    .align 2
1:  .word 0
    .align 2
2:  LANDING
    RESULT 0, r3
    FLAGS 1
    b test_done
.endm

LDM_RETURN t_ldm_arm_even, 0x600000DF, 0
LDM_RETURN t_ldm_arm_bit0, 0x600000DF, 1
@ A real ARM7 hangs on this one, seemingly fetching from the misaligned address, so it only runs on the ARM9
LDM_RETURN t_ldm_arm_bit1, 0x600000DF, 2, 1
LDM_RETURN t_ldm_thm_even, 0x600000FF, 0
LDM_RETURN t_ldm_thm_bit0, 0x600000FF, 1

.ltorg

@ ---- ALU instructions with S writing the PC: every one should restore the SPSR ----

.macro ALU_RETURN name, word, setup1, setup2
TEST \name
    adr r4, 1f
    \setup1
    \setup2
    ldr r1, =0x600000DF
    msr cpsr_f, #0x20000000 @ Carry set, for the ones that use it, and not what the SPSR has
    TO_SVC
    .word \word
    .align 2
1:  mov r0, r0
    RESULT 0, r3
    FLAGS 1
    b test_done
.endm

ALU_RETURN t_ret_ands, 0xE014F005, "mvn r5, #0",     "mov r0, r0" @ ANDS pc,r4,r5
ALU_RETURN t_ret_eors, 0xE034F005, "mov r5, #0",     "mov r0, r0" @ EORS pc,r4,r5
ALU_RETURN t_ret_subs, 0xE054F005, "mov r5, #0",     "mov r0, r0" @ SUBS pc,r4,r5
ALU_RETURN t_ret_rsbs, 0xE075F004, "mov r5, #0",     "mov r0, r0" @ RSBS pc,r5,r4
ALU_RETURN t_ret_adds, 0xE094F005, "mov r5, #0",     "mov r0, r0" @ ADDS pc,r4,r5
ALU_RETURN t_ret_adcs, 0xE0B4F005, "mvn r5, #0",     "mov r0, r0" @ ADCS pc,r4,r5, adding -1 and the carry
ALU_RETURN t_ret_sbcs, 0xE0D4F005, "mov r5, #0",     "mov r0, r0" @ SBCS pc,r4,r5
ALU_RETURN t_ret_rscs, 0xE0F5F004, "mov r5, #0",     "mov r0, r0" @ RSCS pc,r5,r4
ALU_RETURN t_ret_orrs, 0xE194F005, "mov r5, #0",     "mov r0, r0" @ ORRS pc,r4,r5
ALU_RETURN t_ret_movs, 0xE1B0F004, "mov r5, #0",     "mov r0, r0" @ MOVS pc,r4
ALU_RETURN t_ret_bics, 0xE1D4F005, "mov r5, #0",     "mov r0, r0" @ BICS pc,r4,r5
ALU_RETURN t_ret_mvns, 0xE1F0F005, "mvn r5, r4",     "mov r0, r0" @ MVNS pc,r5

.ltorg

@ ---- MSR: the THUMB bit, reserved bits, and User mode ----

TEST t_msr_reserved
    mrs r1, cpsr
    ldr r2, =0x0FFFFF00
    orr r1, r1, r2
    .word 0xE12FF001 @ MSR cpsr_fsxc,r1
    FLAGS 0
    b test_done

TEST t_spsr_reserved
    msr cpsr_c, #0xD3
    ldr r1, =0x0FFFFF00
    orr r1, r1, #0xDF
    msr spsr_fsxc, r1
    mrs r0, spsr
    RESULT 0, r0
    b test_done

@ Reached from the BIOS when a test causes an exception, recording that in words 3-5 and going on to the next test
.global exception_caught
.align 2
exception_caught:
    mrs r2, cpsr
    mov r3, lr
    msr cpsr_c, #0xDF
    ldr r1, =saved_out
    ldr r1, [r1]
    ldr r0, =0xDEADDEAD
    str r0, [r1, #12]
    str r2, [r1, #16]
    str r3, [r1, #20]
    b test_done

.ltorg

.global irq_escape
.align 2
irq_escape: @ Reached in IRQ mode from the BIOS, to get back from User mode
    mov r0, #0x04000000
    mov r1, #0
    str r1, [r0, #0x208]
    ldr r1, [r0, #0x214]
    str r1, [r0, #0x214]
    ldr sp, =IRQ_STACK
    msr cpsr_c, #0xDF
    ldr r0, =escape_to
    ldr pc, [r0]

.ltorg

TEST t_user_msr
    ldr r0, =escape_to
    adr r1, 2f
    str r1, [r0]

    @ Wait for the start of a frame, so V-blank is far away, then let it interrupt
    mov r0, #0x04000000
1:  ldrh r1, [r0, #6]
    cmp r1, #2
    bne 1b
    ldrh r1, [r0, #4]
    orr r1, r1, #8
    strh r1, [r0, #4]
    mov r1, #1
    str r1, [r0, #0x210]
    mvn r1, #0
    str r1, [r0, #0x214]
    mov r1, #1
    str r1, [r0, #0x208]

    msr cpsr_c, #0x10 @ User mode with interrupts on
    .word 0xE321F050 @ MSR cpsr_c,#0x50 (F)
    mrs r4, cpsr
    .word 0xE321F010 @ MSR cpsr_c,#0x10
    .word 0xE321F090 @ MSR cpsr_c,#0x90 (I)
    mrs r5, cpsr
    .word 0xE321F010
    .word 0xE321F01F @ MSR cpsr_c,#0x1F (trying for System mode)
    mrs r6, cpsr
    .word 0xE321F010
    msr cpsr_f, #0xF0000000
    mrs r7, cpsr
    msr cpsr_f, #0
    b . @ Until the interrupt

    .align 2
2:  RESULT 0, r4
    RESULT 1, r5
    RESULT 2, r6
    RESULT 3, r7
    b test_done

.ltorg

@ ---- Misaligned accesses ----

.macro LOAD_AT name, word, offset
TEST \name
    ldr r0, =test_data
    add r0, r0, #\offset
    mov r4, #0
    .word \word
    RESULT 0, r4
    RESULT 1, r0
    b test_done
.endm

LOAD_AT t_ldr_1,   0xE5904000, 1 @ LDR r4,[r0]
LOAD_AT t_ldr_2,   0xE5904000, 2
LOAD_AT t_ldr_3,   0xE5904000, 3
LOAD_AT t_ldrh_1,  0xE1D040B0, 1 @ LDRH r4,[r0]
LOAD_AT t_ldrsh_1, 0xE1D040F0, 1 @ LDRSH r4,[r0]
LOAD_AT t_ldrsb_1, 0xE1D040D0, 1 @ LDRSB r4,[r0]
LOAD_AT t_ldm_1,   0xE8900030, 1 @ LDMIA r0,{r4,r5}
LOAD_AT t_swp_1,   0xE1004095, 1 @ SWP r4,r5,[r0]
LOAD_AT t_swpb_1,  0xE1404095, 1 @ SWPB r4,r5,[r0]

.macro STORE_AT name, word, offset
TEST \name
    ldr r0, =test_data
    add r0, r0, #\offset
    ldr r4, =0xA1B2C3D4
    ldr r5, =0xE5F60718
    .word \word
    b test_done
.endm

STORE_AT t_str_1,  0xE5804000, 1 @ STR r4,[r0]
STORE_AT t_str_2,  0xE5804000, 2
STORE_AT t_strh_1, 0xE1C040B0, 1 @ STRH r4,[r0]
STORE_AT t_stm_1,  0xE8800030, 1 @ STMIA r0,{r4,r5}

TEST t_ldrd_2
    ARM9_ONLY
    ldr r0, =test_data
    add r0, r0, #2
    .word 0xE1C040D0 @ LDRD r4,[r0]
    RESULT 0, r4
    RESULT 1, r5
    b test_done

TEST t_strd_2
    ARM9_ONLY
    ldr r0, =test_data
    add r0, r0, #2
    ldr r4, =0xA1B2C3D4
    ldr r5, =0xE5F60718
    .word 0xE1C040F0 @ STRD r4,[r0]
    b test_done

TEST t_thumb_ldr_1
    ldr r0, =test_data
    add r0, r0, #1
    adr r2, 1f + 1
    bx r2
    .thumb
1:  ldr r4, [r0]
    nop
    bx pc
    nop
    .arm
    RESULT 0, r4
    b test_done

TEST t_thumb_ldrh_1
    ldr r0, =test_data
    add r0, r0, #1
    adr r2, 1f + 1
    bx r2
    .thumb
1:  ldrh r4, [r0]
    nop
    bx pc
    nop
    .arm
    RESULT 0, r4
    b test_done

.ltorg

@ ---- A base in its own register list ----

.macro LIST name, word
TEST \name
    ldr r0, =test_data
    ldr r1, =0x11111111
    ldr r2, =0x22222222
    ldr r3, =0x33333333
    mov r4, r0
    .word \word
    RESULT 0, r0
    RESULT 1, r1
    RESULT 2, r2
    RESULT 3, r3
    RESULT 4, r4
    b test_done
.endm

LIST t_ldm_base_only,  0xE8B00001 @ LDMIA r0!,{r0}
LIST t_ldm_base_first, 0xE8B00007 @ LDMIA r0!,{r0,r1,r2}
LIST t_stm_base_first, 0xE8A00007 @ STMIA r0!,{r0,r1,r2}

.ltorg

.macro LIST_R1 name, word
TEST \name
    ldr r1, =test_data
    ldr r0, =0x11111111
    ldr r2, =0x22222222
    ldr r3, =0x33333333
    mov r4, r1
    .word \word
    RESULT 0, r0
    RESULT 1, r1
    RESULT 2, r2
    RESULT 3, r3
    RESULT 4, r4
    b test_done
.endm

LIST_R1 t_ldm_r1_mid,   0xE8B1000F @ LDMIA r1!,{r0,r1,r2,r3}
LIST_R1 t_ldm_r1_last,  0xE8B10003 @ LDMIA r1!,{r0,r1}
LIST_R1 t_stm_r1_mid,   0xE8A10007 @ STMIA r1!,{r0,r1,r2}
LIST_R1 t_stm_r1_last,  0xE8A10003 @ STMIA r1!,{r0,r1}
LIST_R1 t_stm_r1_first, 0xE8A10006 @ STMIA r1!,{r1,r2}

TEST t_ldmda_r1_mid
    ldr r1, =test_data + 16
    ldr r0, =0x11111111
    ldr r2, =0x22222222
    ldr r3, =0x33333333
    .word 0xE8310007 @ LDMDA r1!,{r0,r1,r2}
    RESULT 0, r0
    RESULT 1, r1
    RESULT 2, r2
    RESULT 3, r3
    b test_done

TEST t_thumb_ldm_base
    ldr r0, =test_data
    mov r1, #0
    adr r2, 1f + 1
    bx r2
    .thumb
1:  .hword 0xC803 @ LDMIA r0!,{r0,r1}
    nop
    bx pc
    nop
    .arm
    RESULT 0, r0
    RESULT 1, r1
    b test_done

TEST t_thumb_stm_base
    ldr r1, =test_data
    mov r0, #0x55
    adr r2, 1f + 1
    bx r2
    .thumb
1:  .hword 0xC103 @ STMIA r1!,{r0,r1}
    nop
    bx pc
    nop
    .arm
    RESULT 0, r1
    b test_done

.ltorg

@ ---- Storing the PC ----

TEST t_str_pc
    ldr r0, =test_data
    .word 0xE580F000 @ STR pc,[r0]
    adr r1, .
    RESULT 0, r1
    b test_done

TEST t_strb_pc
    ldr r0, =test_data
    .word 0xE5C0F000 @ STRB pc,[r0]
    adr r1, .
    RESULT 0, r1
    b test_done

TEST t_strh_pc
    ldr r0, =test_data
    .word 0xE1C0F0B0 @ STRH pc,[r0]
    adr r1, .
    RESULT 0, r1
    b test_done

TEST t_stm_pc
    ldr r0, =test_data
    mov r1, #0x77
    .word 0xE8808002 @ STMIA r0,{r1,pc}
    adr r1, .
    RESULT 0, r1
    b test_done

.ltorg

@ ---- The PC with a register shift ----

TEST t_pc_rn_shift
    mov r1, #0
    mov r2, #0
    .word 0xE08F0211 @ ADD r0,pc,r1,LSL r2
    adr r1, .
    RESULT 0, r0
    RESULT 1, r1
    b test_done

TEST t_pc_rm_shift
    mov r1, #0
    mov r2, #0
    .word 0xE081021F @ ADD r0,r1,pc,LSL r2
    adr r1, .
    RESULT 0, r0
    RESULT 1, r1
    b test_done

TEST t_pc_rs_shift
    mov r1, #1
    .word 0xE1A00F11 @ MOV r0,r1,LSL pc
    adr r1, .
    RESULT 0, r0
    RESULT 1, r1
    b test_done

@ ---- Multiplies with the PC or their own destination as an operand ----

TEST t_mul_pc_rm
    mov r1, #1
    .word 0xE000019F @ MUL r0,pc,r1
    adr r1, .
    RESULT 0, r0
    RESULT 1, r1
    b test_done

TEST t_mul_rd_rm
    mov r0, #3
    mov r1, #5
    .word 0xE0000190 @ MUL r0,r0,r1
    RESULT 0, r0
    b test_done

@ ---- Writeback over loaded or stored data ----

.macro WB name, word
TEST \name
    ldr r0, =test_data
    mov r4, r0
    .word \word
    RESULT 0, r0
    RESULT 1, r4
    b test_done
.endm

WB t_ldr_post_same, 0xE4900004 @ LDR r0,[r0],#4
WB t_ldr_pre_same,  0xE5B00004 @ LDR r0,[r0,#4]!
WB t_ldrb_post_same, 0xE4D00004 @ LDRB r0,[r0],#4
WB t_str_post_same, 0xE4800004 @ STR r0,[r0],#4
WB t_str_pre_same,  0xE5A00004 @ STR r0,[r0,#4]!
WB t_ldrh_post_same, 0xE0D000B4 @ LDRH r0,[r0],#4
WB t_ldrh_pre_same, 0xE1F000B4 @ LDRH r0,[r0,#4]!
WB t_strh_post_same, 0xE0C000B4 @ STRH r0,[r0],#4

TEST t_ldrd_base_high
    ARM9_ONLY
    ldr r1, =test_data
    mov r4, r1
    .word 0xE0C100D8 @ LDRD r0,[r1],#8
    RESULT 0, r0
    RESULT 1, r1
    b test_done

TEST t_strd_base_high
    ARM9_ONLY
    ldr r1, =test_data
    mov r0, #0x99
    .word 0xE1E100F8 @ STRD r0,[r1,#8]!
    RESULT 0, r1
    b test_done

TEST t_ldrd_offset_low
    ARM9_ONLY
    ldr r1, =test_data
    mov r0, #8
    .word 0xE08100D0 @ LDRD r0,[r1],r0
    RESULT 0, r0
    RESULT 1, r1
    b test_done

.ltorg

@ ---- SWP with registers overlapping ----

TEST t_swp_rd_rn
    ldr r0, =test_data
    mov r1, #0x66
    .word 0xE1000091 @ SWP r0,r1,[r0]
    RESULT 0, r0
    b test_done

TEST t_swp_rm_rn
    ldr r0, =test_data
    mov r1, #0x66
    .word 0xE1001090 @ SWP r1,r0,[r0]
    RESULT 0, r1
    b test_done

.ltorg

@ ---- The PC as the base of a list ----

TEST t_ldm_pc_base
    mov r1, #0
    mov r2, #0
    .word 0xE89F0006 @ LDMIA pc,{r1,r2}
    b 1f
    .word 0xCAFE0001
    .word 0xCAFE0002
    .word 0xCAFE0003
    .word 0xCAFE0004
1:  RESULT 0, r1
    RESULT 1, r2
    b test_done

TEST t_stm_pc_base
    ldr r1, =0xBEEF0001
    ldr r2, =0xBEEF0002
    .word 0xE88F0006 @ STMIA pc,{r1,r2}
    b 1f
2:  .word 0
    .word 0
    .word 0
    .word 0
1:  adr r0, 2b
    ldr r1, [r0, #0]
    ldr r2, [r0, #4]
    ldr r3, [r0, #8]
    ldr r4, [r0, #12]
    mov r5, #0
    str r5, [r0, #0]
    str r5, [r0, #4]
    str r5, [r0, #8]
    str r5, [r0, #12]
    RESULT 0, r1
    RESULT 1, r2
    RESULT 2, r3
    RESULT 3, r4
    b test_done

.ltorg

@ ---- Things that need an SPSR, in a mode without one ----

TEST t_movs_no_spsr
    adr r4, 1f
    msr cpsr_f, #0x90000000
    .word 0xE1B0F004 @ MOVS pc,r4
    .align 2
1:  LANDING
    RESULT 0, r3
    FLAGS 1
    b test_done

TEST t_ands_no_spsr
    adr r4, 1f
    mvn r5, #0
    msr cpsr_f, #0x90000000
    .word 0xE014F005 @ ANDS pc,r4,r5
    .align 2
1:  LANDING
    RESULT 0, r3
    FLAGS 1
    b test_done

TEST t_ldm_s_no_spsr
    adr r2, 1f
    adr r0, 2f
    str r2, [r0]
    msr cpsr_f, #0x90000000
    .word 0xE8D08000 @ LDMIA r0,{pc}^
    .align 2
2:  .word 0
    .align 2
1:  LANDING
    RESULT 0, r3
    FLAGS 1
    b test_done

TEST t_mrs_no_spsr
    mov r0, #0
    mrs r0, spsr
    RESULT 0, r0
    b test_done

TEST t_msr_no_spsr
    mov r1, #0
    ldr r0, =0xF00000D3
    msr spsr_fsxc, r0
    mrs r1, spsr
    RESULT 0, r1
    FLAGS 1
    b test_done

@ ---- LDM and STM of User registers ----

TEST t_ldm_user_regs
    ldr r0, =test_data
    mov r1, sp
    msr cpsr_c, #0xD3
    mov r5, sp
    mov sp, #0x10
    .word 0xE8D02000 @ LDMIA r0,{sp}^ (the User one)
    mov r4, sp
    mov sp, r5
    msr cpsr_c, #0xDF
    mov r6, sp
    mov sp, r1
    RESULT 0, r4
    RESULT 1, r6
    b test_done

TEST t_stm_user_regs
    ldr r0, =test_data
    mov r1, sp
    ldr r2, =0x12340000
    mov sp, r2
    msr cpsr_c, #0xD3
    mov r5, sp
    mov sp, #0x10
    .word 0xE8C02000 @ STMIA r0,{sp}^ (the User one)
    mov sp, r5
    msr cpsr_c, #0xDF
    mov sp, r1
    b test_done

.ltorg

@ ---- ARM7 only: what ARMv5 changed, which the ARM9's interpreter behavior can't show ----

TEST t_ldr_pc_bit0
    @ An ARM7 stays in ARM on LDR PC; an ARM9 goes to THUMB
    adr r0, 1f
    adr r1, 2f + 1
    str r1, [r0]
    .word 0xE590F000 @ LDR pc,[r0]
    .align 2
1:  .word 0
    .align 2
2:  LANDING
    RESULT 0, r3
    FLAGS 1
    b test_done

TEST t_ldm_pc_bit0
    adr r0, 1f
    adr r1, 2f + 1
    str r1, [r0]
    .word 0xE8908000 @ LDMIA r0,{pc}
    .align 2
1:  .word 0
    .align 2
2:  LANDING
    RESULT 0, r3
    FLAGS 1
    b test_done

.ltorg

@ ---- Only after pressing START, since these could hang a DS ----

TEST t_msr_thumb
    ARM7_ONLY
    mrs r1, cpsr
    orr r1, r1, #0x20
    .word 0xE121F001 @ MSR cpsr_c,r1
    .align 2
    LANDING
    RESULT 0, r3
    FLAGS 1
    b test_done

TEST t_msr_imm_thumb
    ARM7_ONLY
    .word 0xE321F0FF @ MSR cpsr_c,#0xFF
    .align 2
    LANDING
    RESULT 0, r3
    FLAGS 1
    b test_done

.macro ARM_INTERRUPT_WAY_BACK
    ldr r0, =escape_to
    adr r1, 2f
    str r1, [r0]
    mov r0, #0x04000000
1:  ldrh r1, [r0, #6]
    cmp r1, #2
    bne 1b
    ldrh r1, [r0, #4]
    orr r1, r1, #8
    strh r1, [r0, #4]
    mov r1, #1
    str r1, [r0, #0x210]
    mvn r1, #0
    str r1, [r0, #0x214]
    mov r1, #1
    str r1, [r0, #0x208]
.endm

@ Each gets back with an interrupt, since a mode with no way out would otherwise affect the rest
.macro BAD_MODE name, mode
TEST \name
    ARM_INTERRUPT_WAY_BACK
    mov r4, #0
    .word 0xE321F000 | \mode @ MSR cpsr_c,#mode, with interrupts on
    mrs r4, cpsr
    b . @ Until the interrupt
    .align 2
2:  RESULT 0, r4
    b test_done
.endm

BAD_MODE t_bad_mode_00, 0x00
BAD_MODE t_bad_mode_01, 0x01
BAD_MODE t_bad_mode_02, 0x02
BAD_MODE t_bad_mode_03, 0x03
BAD_MODE t_bad_mode_07, 0x07
BAD_MODE t_bad_mode_0b, 0x0B
BAD_MODE t_bad_mode_0f, 0x0F
BAD_MODE t_bad_mode_14, 0x14
BAD_MODE t_bad_mode_18, 0x18
BAD_MODE t_bad_mode_1e, 0x1E

.ltorg

@ ---- What the SPSR is in modes without one, with the flags and interrupt bits showing it apart from the CPSR ----

TEST t_mrs_sys_flags
    mov r0, #0
    msr cpsr_f, #0x90000000
    mrs r0, spsr
    RESULT 0, r0
    FLAGS 1
    b test_done

TEST t_mrs_sys_irq_on
    mov r0, #0
    msr cpsr_c, #0x1F @ Interrupts are off in the IME
    msr cpsr_f, #0x60000000
    mrs r0, spsr
    RESULT 0, r0
    FLAGS 1
    b test_done

TEST t_movs_sys_irq_on
    adr r4, 1f
    msr cpsr_c, #0x1F
    msr cpsr_f, #0x60000000
    .word 0xE1B0F004 @ MOVS pc,r4
    .align 2
1:  LANDING
    RESULT 0, r3
    FLAGS 1
    b test_done

TEST t_ldm_no_spsr_bit0
    adr r2, 1f + 1
    adr r0, 2f
    str r2, [r0]
    .word 0xE8D08000 @ LDMIA r0,{pc}^
    .align 2
2:  .word 0
    .align 2
1:  LANDING
    RESULT 0, r3
    FLAGS 1
    b test_done

TEST t_user_spsr
    ARM_INTERRUPT_WAY_BACK
    adr r5, 3f
    mov r4, #0
    mov r7, #0
    msr cpsr_c, #0x10
    msr cpsr_f, #0x60000000
    mrs r4, spsr
    msr cpsr_f, #0x90000000
    .word 0xE1B0F005 @ MOVS pc,r5
    .align 2
3:  LANDING
    mrs r6, cpsr
    ldr r0, =0xA0000011
    msr spsr_fsxc, r0
    mrs r7, spsr
    b . @ Until the interrupt
    .align 2
2:  RESULT 0, r4
    RESULT 1, r6
    RESULT 2, r7
    RESULT 3, r3
    b test_done

.ltorg

TEST t_user_msr_t
    ARM_INTERRUPT_WAY_BACK
    mov r3, #0
    msr cpsr_c, #0x10
    .word 0xE321F030 @ MSR cpsr_c,#0x30 (T)
    .align 2
    LANDING
    mov r8, r3
    mrs r4, cpsr
    .word 0xE321F010
    b . @ Until the interrupt
    .align 2
2:  RESULT 0, r8
    RESULT 1, r4
    b test_done

.ltorg

.bss
.align 2
saved_sp: .space 4
saved_out: .space 4
.global escape_to
escape_to: .space 4
