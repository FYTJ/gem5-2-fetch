#ifndef _ENV_PHYSICAL_SINGLE_CORE_H
#define _ENV_PHYSICAL_SINGLE_CORE_H

#define TESTNUM gp

#define RVTEST_RV32U
#define RVTEST_RV32S
#define RVTEST_RV32M
#define RVTEST_RV64U
#define RVTEST_RV64S
#define RVTEST_RV64M

#define DRAM_BASE 0x80000000
#define RISCV_PGSHIFT 12
#define RISCV_PGSIZE (1 << RISCV_PGSHIFT)

#define PRV_U 0
#define PRV_S 1
#define PRV_M 3

#define MSTATUS_MIE  0x00000008
#define MSTATUS_SPIE 0x00000020
#define MSTATUS_SPP  0x00000100
#define MSTATUS_MPP  0x00001800
#define MSTATUS_FS   0x00006000
#define MSTATUS_MPRV 0x00020000
#define MSTATUS_SUM  0x00040000
#define MSTATUS_MXR  0x00080000
#define MSTATUS_TVM  0x00100000
#define MSTATUS_TSR  0x00400000

#define SSTATUS_SIE  0x00000002
#define SSTATUS_SPIE 0x00000020
#define SSTATUS_SPP  0x00000100
#define SSTATUS_SUM  0x00040000
#define SSTATUS_MXR  0x00080000
#define SSTATUS_UXL  0x0000000300000000

#define MIP_SSIP 0x2
#define SIP_SSIP MIP_SSIP

#define MCONTROL_M       (1 << 6)
#define MCONTROL_EXECUTE (1 << 2)
#define MCONTROL_STORE   (1 << 1)
#define MCONTROL_LOAD    (1 << 0)

#define SATP_MODE_OFF 0
#define SATP_MODE_SV39 8
#if __riscv_xlen == 64
#define SATP_MODE SATP_MODE_SV39
#else
#define SATP_MODE SATP_MODE_OFF
#endif

#define PMP_R     0x01
#define PMP_W     0x02
#define PMP_X     0x04
#define PMP_A     0x18
#define PMP_L     0x80
#define PMP_TOR   0x08
#define PMP_NA4   0x10
#define PMP_NAPOT 0x18

#define PTE_V         0x001
#define PTE_R         0x002
#define PTE_W         0x004
#define PTE_X         0x008
#define PTE_U         0x010
#define PTE_G         0x020
#define PTE_A         0x040
#define PTE_D         0x080
#define PTE_PPN_SHIFT 10

#define CAUSE_MISALIGNED_FETCH     0x0
#define CAUSE_FETCH_ACCESS         0x1
#define CAUSE_ILLEGAL_INSTRUCTION  0x2
#define CAUSE_BREAKPOINT           0x3
#define CAUSE_MISALIGNED_LOAD      0x4
#define CAUSE_LOAD_ACCESS          0x5
#define CAUSE_MISALIGNED_STORE     0x6
#define CAUSE_STORE_ACCESS         0x7
#define CAUSE_USER_ECALL           0x8
#define CAUSE_SUPERVISOR_ECALL     0x9
#define CAUSE_MACHINE_ECALL        0xb
#define CAUSE_FETCH_PAGE_FAULT     0xc
#define CAUSE_LOAD_PAGE_FAULT      0xd
#define CAUSE_STORE_PAGE_FAULT     0xf

#define RVTEST_CODE_BEGIN                                               \
  .section .text;                                                       \
  .globl main;                                                          \
main:

#define RVTEST_CODE_END

#define RVTEST_PASS                                                     \
        li a0, 0;                                                       \
        ebreak;

#define RVTEST_FAIL                                                     \
        li a0, 1;                                                       \
        ebreak;

#define TEST_PASSFAIL                                                   \
        j pass;                                                         \
fail:                                                                   \
        RVTEST_FAIL;                                                    \
pass:                                                                   \
        RVTEST_PASS;

#define RVTEST_DATA_BEGIN .section .data; .align 4;
#define RVTEST_DATA_END

#endif
