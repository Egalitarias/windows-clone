.text
.extern exception_dispatch

.macro EXC_NOERR name, vector
.globl \name
\name:
    mov $\vector, %rdi
    xor %rsi, %rsi
    mov %rsp, %rdx
    and $~15, %rsp
    call exception_dispatch
1:
    pause
    jmp 1b
.endm

.macro EXC_ERR name, vector
.globl \name
\name:
    mov $\vector, %rdi
    mov (%rsp), %rsi
    mov %rsp, %rdx
    and $~15, %rsp
    call exception_dispatch
1:
    pause
    jmp 1b
.endm

EXC_NOERR exception_stub_0, 0
EXC_NOERR exception_stub_1, 1
EXC_NOERR exception_stub_2, 2
EXC_NOERR exception_stub_3, 3
EXC_NOERR exception_stub_4, 4
EXC_NOERR exception_stub_5, 5
EXC_NOERR exception_stub_6, 6
EXC_NOERR exception_stub_7, 7
EXC_ERR   exception_stub_8, 8
EXC_NOERR exception_stub_9, 9
EXC_ERR   exception_stub_10, 10
EXC_ERR   exception_stub_11, 11
EXC_ERR   exception_stub_12, 12
EXC_ERR   exception_stub_13, 13
EXC_ERR   exception_stub_14, 14
EXC_NOERR exception_stub_15, 15
EXC_NOERR exception_stub_16, 16
EXC_ERR   exception_stub_17, 17
EXC_NOERR exception_stub_18, 18
EXC_NOERR exception_stub_19, 19
EXC_NOERR exception_stub_20, 20
EXC_NOERR exception_stub_21, 21
EXC_NOERR exception_stub_22, 22
EXC_NOERR exception_stub_23, 23
EXC_NOERR exception_stub_24, 24
EXC_NOERR exception_stub_25, 25
EXC_NOERR exception_stub_26, 26
EXC_NOERR exception_stub_27, 27
EXC_NOERR exception_stub_28, 28
EXC_NOERR exception_stub_29, 29
EXC_ERR   exception_stub_30, 30
EXC_NOERR exception_stub_31, 31

.data
.globl exception_stub_table
exception_stub_table:
    .quad exception_stub_0
    .quad exception_stub_1
    .quad exception_stub_2
    .quad exception_stub_3
    .quad exception_stub_4
    .quad exception_stub_5
    .quad exception_stub_6
    .quad exception_stub_7
    .quad exception_stub_8
    .quad exception_stub_9
    .quad exception_stub_10
    .quad exception_stub_11
    .quad exception_stub_12
    .quad exception_stub_13
    .quad exception_stub_14
    .quad exception_stub_15
    .quad exception_stub_16
    .quad exception_stub_17
    .quad exception_stub_18
    .quad exception_stub_19
    .quad exception_stub_20
    .quad exception_stub_21
    .quad exception_stub_22
    .quad exception_stub_23
    .quad exception_stub_24
    .quad exception_stub_25
    .quad exception_stub_26
    .quad exception_stub_27
    .quad exception_stub_28
    .quad exception_stub_29
    .quad exception_stub_30
    .quad exception_stub_31
