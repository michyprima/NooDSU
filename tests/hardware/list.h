// The tests in the order they run, which both CPUs share, with short names for the screen
#define NORMAL_TESTS \
    T(t_neg_min, "NEG 80000000") T(t_neg_zero, "NEG 0") T(t_rscs_clear, "RSCS CLEARS CV") T(t_rscs_borrow, "RSCS BORROW") \
    T(t_ldm_arm_even, "LDM^ ARM PC+0") T(t_ldm_arm_bit0, "LDM^ ARM PC+1") T(t_ldm_arm_bit1, "LDM^ ARM PC+2") \
    T(t_ldm_thm_even, "LDM^ THM PC+0") T(t_ldm_thm_bit0, "LDM^ THM PC+1") \
    T(t_ret_ands, "ANDS PC RET") T(t_ret_eors, "EORS PC RET") T(t_ret_subs, "SUBS PC RET") T(t_ret_rsbs, "RSBS PC RET") \
    T(t_ret_adds, "ADDS PC RET") T(t_ret_adcs, "ADCS PC RET") T(t_ret_sbcs, "SBCS PC RET") T(t_ret_rscs, "RSCS PC RET") \
    T(t_ret_orrs, "ORRS PC RET") T(t_ret_movs, "MOVS PC RET") T(t_ret_bics, "BICS PC RET") T(t_ret_mvns, "MVNS PC RET") \
    T(t_msr_reserved, "MSR RESERVED") T(t_spsr_reserved, "SPSR RESERVED") \
    T(t_user_msr, "USER MSR") \
    T(t_ldr_1, "LDR +1") T(t_ldr_2, "LDR +2") T(t_ldr_3, "LDR +3") T(t_ldrh_1, "LDRH +1") T(t_ldrsh_1, "LDRSH +1") T(t_ldrsb_1, "LDRSB +1") \
    T(t_ldm_1, "LDM +1") T(t_swp_1, "SWP +1") T(t_swpb_1, "SWPB +1") T(t_str_1, "STR +1") T(t_str_2, "STR +2") T(t_strh_1, "STRH +1") \
    T(t_stm_1, "STM +1") T(t_ldrd_2, "LDRD +2") T(t_strd_2, "STRD +2") T(t_thumb_ldr_1, "T LDR +1") T(t_thumb_ldrh_1, "T LDRH +1") \
    T(t_ldm_base_only, "LDM! BASE ONLY") T(t_ldm_base_first, "LDM! BASE 1ST") T(t_ldm_r1_mid, "LDM! BASE MID") T(t_ldm_r1_last, "LDM! BASE LAST") \
    T(t_ldmda_r1_mid, "LDMDA! BASE MID") T(t_stm_base_first, "STM! BASE 1ST") T(t_stm_r1_first, "STM! BASE 1ST B") T(t_stm_r1_mid, "STM! BASE MID") \
    T(t_stm_r1_last, "STM! BASE LAST") T(t_thumb_ldm_base, "T LDM! BASE") T(t_thumb_stm_base, "T STM! BASE") \
    T(t_str_pc, "STR PC") T(t_strb_pc, "STRB PC") T(t_strh_pc, "STRH PC") T(t_stm_pc, "STM PC") \
    T(t_pc_rn_shift, "PC RN REG SHIFT") T(t_pc_rm_shift, "PC RM REG SHIFT") T(t_pc_rs_shift, "PC RS SHIFT") \
    T(t_mul_pc_rm, "MUL PC") T(t_mul_rd_rm, "MUL RD=RM") \
    T(t_ldr_post_same, "LDR RD=RN POST") T(t_ldr_pre_same, "LDR RD=RN PRE") T(t_ldrb_post_same, "LDRB RD=RN") T(t_str_post_same, "STR RD=RN POST") \
    T(t_str_pre_same, "STR RD=RN PRE") T(t_ldrh_post_same, "LDRH RD=RN POST") T(t_ldrh_pre_same, "LDRH RD=RN PRE") T(t_strh_post_same, "STRH RD=RN") \
    T(t_ldrd_base_high, "LDRD RN=RD+1") T(t_strd_base_high, "STRD RN=RD+1") T(t_ldrd_offset_low, "LDRD RM=RD") \
    T(t_swp_rd_rn, "SWP RD=RN") T(t_swp_rm_rn, "SWP RM=RN") T(t_ldm_pc_base, "LDM PC BASE") T(t_stm_pc_base, "STM PC BASE") \
    T(t_movs_no_spsr, "MOVS NO SPSR") T(t_ands_no_spsr, "ANDS NO SPSR") T(t_ldm_s_no_spsr, "LDM^ NO SPSR") T(t_mrs_no_spsr, "MRS NO SPSR") \
    T(t_msr_no_spsr, "MSR NO SPSR") T(t_ldm_user_regs, "LDM^ USER REG") T(t_stm_user_regs, "STM^ USER REG") \
    T(t_ldr_pc_bit0, "LDR PC+1") T(t_ldm_pc_bit0, "LDM PC+1") T(t_mrs_sys_flags, "MRS SYS FLAGS") T(t_mrs_sys_irq_on, "MRS SYS IRQ ON") \
    T(t_movs_sys_irq_on, "MOVS SYS IRQ ON") T(t_ldm_no_spsr_bit0, "LDM^ SYS PC+1") T(t_user_spsr, "USER SPSR") T(t_user_msr_t, "USER MSR T") \
    T(t_mrc_id, "MRC P15 ID") T(t_mrc_p0, "MRC P0") T(t_mcr_p0, "MCR P0")

#define RISKY_TESTS \
    T(t_msr_thumb, "MSR T REG A7") T(t_msr_imm_thumb, "MSR T IMM A7") T(t_bad_mode_00, "MODE 00") T(t_bad_mode_01, "MODE 01") T(t_bad_mode_02, "MODE 02") \
    T(t_bad_mode_03, "MODE 03") T(t_bad_mode_07, "MODE 07") T(t_bad_mode_0b, "MODE 0B") T(t_bad_mode_0f, "MODE 0F") T(t_bad_mode_14, "MODE 14") T(t_bad_mode_18, "MODE 18") T(t_bad_mode_1e, "MODE 1E") \
    T(t_mrc_pc_low, "MRC PC 0") T(t_mrc_pc_high, "MRC PC 5")

#define MAX_TESTS 110
#define WORDS 12 // Six from the test, then the first six words of its data

// Shared between the CPUs, in main RAM with caches off
#define RESULTS(cpu) ((volatile unsigned*)(0x02300000 + (cpu) * 0x10000))
#define COMMAND_ARM7 (*(volatile unsigned*)0x02380000) // 1 runs the normal tests, 2 onward one risky test each
#define DONE_ARM7 (*(volatile unsigned*)0x02380004)
#define BOOT_MAGIC (*(volatile unsigned*)0x02380020) // These survive the program restarting, to show that it did
#define BOOT_COUNT (*(volatile unsigned*)0x02380024)
#define BOOT_LAST_RISKY (*(volatile unsigned*)0x02380028) // 1 + the risky test the ARM9 last started
#define BOOT_LAST_CPU (*(volatile unsigned*)0x0238002C)
#define PROGRESS(cpu) (*(volatile unsigned*)(0x02380010 + (cpu) * 4)) // The test running, to see where a hang happened
