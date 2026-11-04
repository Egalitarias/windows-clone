.section .text

.equ TCB_STACK_POINTER_OFFSET, 16
.equ TCB_INSTRUCTION_POINTER_OFFSET, 24
.equ TCB_REGISTER_STATE_OFFSET, 32

.equ REG_RAX_OFFSET, (0 * 8)
.equ REG_RBX_OFFSET, (1 * 8)
.equ REG_RCX_OFFSET, (2 * 8)
.equ REG_RDX_OFFSET, (3 * 8)
.equ REG_RSI_OFFSET, (4 * 8)
.equ REG_RDI_OFFSET, (5 * 8)
.equ REG_R8_OFFSET, (6 * 8)
.equ REG_R9_OFFSET, (7 * 8)
.equ REG_R10_OFFSET, (8 * 8)
.equ REG_R11_OFFSET, (9 * 8)
.equ REG_R12_OFFSET, (10 * 8)
.equ REG_R13_OFFSET, (11 * 8)
.equ REG_R14_OFFSET, (12 * 8)
.equ REG_R15_OFFSET, (13 * 8)
.equ REG_RSP_OFFSET, (14 * 8)
.equ REG_RBP_OFFSET, (15 * 8)
.equ REG_RIP_OFFSET, (16 * 8)

.globl context_switch
context_switch:
    test %rdi, %rdi
    jz .Lreturn_direct
    test %rsi, %rsi
    jz .Lreturn_direct

    // Save full GP register context for the current execution point.
    push %rax
    push %rbx
    push %rcx
    push %rdx
    push %rsi
    push %rdi
    push %rbp
    push %r8
    push %r9
    push %r10
    push %r11
    push %r12
    push %r13
    push %r14
    push %r15

    // Pull from/to pointers from the saved call-site register image.
    mov 72(%rsp), %r10
    mov 80(%rsp), %r11
    test %r10, %r10
    jz .Lrestore_and_return
    test %r11, %r11
    jz .Lrestore_and_return

    mov TCB_REGISTER_STATE_OFFSET(%r10), %r12
    mov TCB_REGISTER_STATE_OFFSET(%r11), %r13
    test %r12, %r12
    jz .Lrestore_and_return
    test %r13, %r13
    jz .Lrestore_and_return
    
    // Save destination TCB pointer (r13) in r14 for later CR3 loading.
    // r14 is preserved across the context switch sequence.
    mov %r13, %r14
    
    // Check that destination RSP and RIP are valid. Save validated values to r10/r11.
    // (The TCB pointers in r10/r11 are not needed after this point.)
    mov REG_RSP_OFFSET(%r13), %r10
    test %r10, %r10
    jz .Lrestore_and_return
    mov REG_RIP_OFFSET(%r13), %r11
    test %r11, %r11
    jz .Lrestore_and_return
    
    // TODO: Load CR3 if destination is a user-mode process (address_space != NULL).
    // This will be implemented when creating the address_space module.
    // For now, skip CR3 loading - kernel tasks will continue with current CR3.

    // Save current register image into from->register_state.
    mov 112(%rsp), %rax
    mov %rax, REG_RAX_OFFSET(%r12)
    mov 104(%rsp), %rax
    mov %rax, REG_RBX_OFFSET(%r12)
    mov 96(%rsp), %rax
    mov %rax, REG_RCX_OFFSET(%r12)
    mov 88(%rsp), %rax
    mov %rax, REG_RDX_OFFSET(%r12)
    mov 80(%rsp), %rax
    mov %rax, REG_RSI_OFFSET(%r12)
    mov 72(%rsp), %rax
    mov %rax, REG_RDI_OFFSET(%r12)
    mov 56(%rsp), %rax
    mov %rax, REG_R8_OFFSET(%r12)
    mov 48(%rsp), %rax
    mov %rax, REG_R9_OFFSET(%r12)
    mov 40(%rsp), %rax
    mov %rax, REG_R10_OFFSET(%r12)
    mov 32(%rsp), %rax
    mov %rax, REG_R11_OFFSET(%r12)
    mov 24(%rsp), %rax
    mov %rax, REG_R12_OFFSET(%r12)
    mov 16(%rsp), %rax
    mov %rax, REG_R13_OFFSET(%r12)
    mov 8(%rsp), %rax
    mov %rax, REG_R14_OFFSET(%r12)
    mov 0(%rsp), %rax
    mov %rax, REG_R15_OFFSET(%r12)
    // Resume with the stack pointer value expected after a normal return.
    lea 128(%rsp), %rax
    mov %rax, REG_RSP_OFFSET(%r12)
    mov 64(%rsp), %rax
    mov %rax, REG_RBP_OFFSET(%r12)
    mov 120(%rsp), %rax
    mov %rax, REG_RIP_OFFSET(%r12)

    mov REG_RSP_OFFSET(%r12), %rax
    mov %rax, TCB_STACK_POINTER_OFFSET(%r10)
    mov REG_RIP_OFFSET(%r12), %rax
    mov %rax, TCB_INSTRUCTION_POINTER_OFFSET(%r10)

    // Restore destination context from register_state (r13).
    // All general-purpose registers except R10/R11 need to be restored.
    // R10 and R11 hold the validated RSP and RIP that we saved earlier.
    mov REG_RAX_OFFSET(%r13), %rax
    mov REG_RBX_OFFSET(%r13), %rbx
    mov REG_RCX_OFFSET(%r13), %rcx
    mov REG_RDX_OFFSET(%r13), %rdx
    mov REG_RSI_OFFSET(%r13), %rsi
    mov REG_RDI_OFFSET(%r13), %rdi
    mov REG_R8_OFFSET(%r13), %r8
    mov REG_R9_OFFSET(%r13), %r9
    // Skip R10 and R11 - they hold validated RSP and RIP for the final jump.
    mov REG_R12_OFFSET(%r13), %r12
    mov REG_R14_OFFSET(%r13), %r14
    mov REG_R15_OFFSET(%r13), %r15
    mov REG_RBP_OFFSET(%r13), %rbp
    // Load R13 last - once we do this, r13 no longer points to register_state.
    mov REG_R13_OFFSET(%r13), %r13
    
    // Set RSP and jump to restored RIP, using the validated values in r10/r11.
    mov %r10, %rsp
    jmp *%r11

.Lrestore_and_return:
    pop %r15
    pop %r14
    pop %r13
    pop %r12
    pop %r11
    pop %r10
    pop %r9
    pop %r8
    pop %rbp
    pop %rdi
    pop %rsi
    pop %rdx
    pop %rcx
    pop %rbx
    pop %rax

.Lreturn_direct:
    ret
