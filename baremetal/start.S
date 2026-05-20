
    .section .init;
#include "riscv-tests/isa/rvtest_exit.h"

    .globl _start;
    .type _start,@function


_start:

  la gp, __global_pointer
  la sp, _sp
  la t0, mtvec_handler
  csrw mtvec, t0

  call main
  RVTEST_EXIT_FAIL

loop:
    j loop

    .section .text;
    .weak mtvec_handler;
    .type mtvec_handler,@function
mtvec_handler:
  RVTEST_EXIT_FAIL
1:
  j 1b
