.section .text

.equ SCHEDULER_SWITCH_LAUNCH_FLAG, 1

.extern scheduler_launch_first_task
.extern scheduler_on_timer_tick
.extern scheduler_task_entry
.extern timer_tick

.globl timer_interrupt_stub
timer_interrupt_stub:
    cli
    // Save full GP register context because this can interrupt arbitrary code.
    push %r15
    push %r14
    push %r13
    push %r12
    push %r11
    push %r10
    push %r9
    push %r8
    push %rbp
    push %rdi
    push %rsi
    push %rdx
    push %rcx
    push %rbx
    push %rax

    // Keep the saved interrupt frame pointer in RBX and align before C calls.
    mov %rsp, %rbx
    and $~15, %rsp
    cld
    call timer_tick
    mov %rbx, %rdi
    call scheduler_on_timer_tick
    mov %rax, %r11

    // Send EOI to PIC master
    mov $0x20, %al
    mov $0x20, %dx
    outb %al, %dx

    // Switch to the scheduler-selected interrupt frame (may be current or next task).
    test $SCHEDULER_SWITCH_LAUNCH_FLAG, %r11
    jnz .Llaunch_selected_task

    // Resuming a previously-started task uses interrupt-return semantics.
    mov %r11, %rsp

    // Restore full GP register context
    pop %rax
    pop %rbx
    pop %rcx
    pop %rdx
    pop %rsi
    pop %rdi
    pop %rbp
    pop %r8
    pop %r9
    pop %r10
    pop %r11
    pop %r12
    pop %r13
    pop %r14
    pop %r15

    // Return from interrupt
    iretq

.Llaunch_selected_task:
    and $~SCHEDULER_SWITCH_LAUNCH_FLAG, %r11
    mov %r11, %rsp
    pop %rax
    pop %rbx
    pop %rcx
    pop %rdx
    pop %rsi
    pop %rdi
    pop %rbp
    pop %r8
    pop %r9
    pop %r10
    pop %r11
    pop %r12
    pop %r13
    pop %r14
    pop %r15
    sti
    jmp scheduler_task_entry_stub

.globl scheduler_launch_first_task
scheduler_launch_first_task:
    mov %rdi, %rsp
    pop %rax
    pop %rbx
    pop %rcx
    pop %rdx
    pop %rsi
    pop %rdi
    pop %rbp
    pop %r8
    pop %r9
    pop %r10
    pop %r11
    pop %r12
    pop %r13
    pop %r14
    pop %r15
    sti
    jmp scheduler_task_entry_stub

.globl scheduler_task_entry_stub
scheduler_task_entry_stub:
    and $~15, %rsp
    cld
    call scheduler_task_entry
1:
    hlt
    jmp 1b
