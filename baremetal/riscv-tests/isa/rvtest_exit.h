#ifndef _RVTEST_EXIT_H
#define _RVTEST_EXIT_H

/*
 * RISC-V gem5 pseudo instructions are encoded as:
 *   0x0000007b | (func << 25)
 * with M5OP_EXIT = 0x21 and M5OP_FAIL = 0x22 in gem5's m5ops.h.
 */
#define RVTEST_GEM5_M5_EXIT_INSN 0x4200007b
#define RVTEST_GEM5_M5_FAIL_INSN 0x4400007b

#if defined(RVTEST_EXIT_BACKEND_GEM5)

#define RVTEST_EXIT_PASS                                                 \
        li a0, 0;                                                       \
        .long RVTEST_GEM5_M5_EXIT_INSN;                                 \
1:      j 1b;

#define RVTEST_EXIT_FAIL                                                 \
        li a0, 0;                                                       \
        li a1, 1;                                                       \
        .long RVTEST_GEM5_M5_FAIL_INSN;                                 \
1:      j 1b;

#elif defined(RVTEST_EXIT_BACKEND_WALLY)

#define RVTEST_WALLY_TOHOST_SECTION                                      \
        .pushsection .tohost,"aw",@progbits;                            \
        .align 3;                                                       \
        .global tohost;                                                 \
tohost: .dword 0;                                                       \
        .global fromhost;                                               \
fromhost: .dword 0;                                                     \
        .popsection;

#define RVTEST_EXIT_PASS                                                 \
        RVTEST_WALLY_TOHOST_SECTION                                      \
        la t0, tohost;                                                  \
        li t1, 1;                                                       \
        sw t1, 0(t0);                                                   \
1:      j 1b;

#define RVTEST_EXIT_FAIL                                                 \
        li a0, 1;                                                       \
        ebreak;                                                         \
1:      j 1b;

#else

#define RVTEST_EXIT_PASS                                                 \
        li a0, 0;                                                       \
        ebreak;

#define RVTEST_EXIT_FAIL                                                 \
        li a0, 1;                                                       \
        ebreak;

#endif

#endif
