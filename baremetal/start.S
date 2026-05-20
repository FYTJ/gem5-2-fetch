
    .section .init;
    .globl _start;
    .type _start,@function


_start:

  la gp, __global_pointer
  la sp, _sp
  la t0, mtvec_handler
  csrw mtvec, t0

  call main
  ebreak

loop:
    j loop

    .section .text;
    .weak mtvec_handler;
    .type mtvec_handler,@function
mtvec_handler:
  li a0, 1
  ebreak
1:
  j 1b
