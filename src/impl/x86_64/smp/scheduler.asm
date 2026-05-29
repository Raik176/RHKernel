[BITS 64]
section .text

; extern "C" void context_switch(regs* regs);
global context_switch
context_switch:
    mov rsp, rdi

    pop r15
    pop r14
    pop r13
    pop r12
    pop r11
    pop r10
    pop r9
    pop r8
    pop rbp
    pop rdi
    pop rsi
    pop rdx
    pop rcx
    pop rbx
    pop rax
    add rsp, 16
    test qword [rsp + 8], 3
    jz .iret
    swapgs
.iret:
    iretq 

; extern "C" void scheduler_yield_context();
extern scheduler_schedule_from_context
global scheduler_yield_context
scheduler_yield_context:
    sub rsp, 176

    mov [rsp + 0], r15
    mov [rsp + 8], r14
    mov [rsp + 16], r13
    mov [rsp + 24], r12
    mov [rsp + 32], r11
    mov [rsp + 40], r10
    mov [rsp + 48], r9
    mov [rsp + 56], r8
    mov [rsp + 64], rbp
    mov [rsp + 72], rdi
    mov [rsp + 80], rsi
    mov [rsp + 88], rdx
    mov [rsp + 96], rcx
    mov [rsp + 104], rbx
    mov [rsp + 112], rax

    xor eax, eax
    mov [rsp + 120], rax
    mov [rsp + 128], rax

    mov rax, [rsp + 176]
    mov [rsp + 136], rax

    xor eax, eax
    mov ax, cs
    mov [rsp + 144], rax

    pushfq
    pop rax
    mov [rsp + 152], rax

    lea rax, [rsp + 184]
    mov [rsp + 160], rax

    xor eax, eax
    mov ax, ss
    mov [rsp + 168], rax

    mov rdi, rsp
    sub rsp, 8
    call scheduler_schedule_from_context
    ud2
