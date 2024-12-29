.global main

.section .data
my_double: .double 64.125

.section .text
.macro trap
    movq $62, %rax
    movq %r12, %rdi
    movq $5, %rsi
    syscall
.endm

main:
    push %rbp
    movq %rsp, %rbp

    # Get pid
    movq $39, %rax
    syscall
    mov %rax, %r12
    
    movq $0xcafecafe, %r13
    trap

    movb $42, %r13b
    trap

    movq $0xba5eba11, %r13
    movq %r13, %mm0
    trap

    movsd my_double(%rip), %xmm0
    trap

    emms
    fldl my_double(%rip)
    trap

    pop %rbp
    movq $0, %rax
    ret