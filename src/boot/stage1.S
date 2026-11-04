.text
.globl _stage1_entry

_stage1_entry:
    cli
1:
    hlt
    jmp 1b
