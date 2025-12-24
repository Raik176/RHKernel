extern idt_handler

%macro ISR_ERROR 1
    global isr%1
isr%1:
    push qword %1
    jmp idt_common_stub
%endmacro

%macro ISR_NO_ERROR 1
    global isr%1
isr%1:
    push qword 0
    push qword %1
    jmp idt_common_stub
%endmacro

%macro IRQ_STUB 2
global irq%1
irq%1:
    push qword 0
    push qword %2
    jmp idt_common_stub
%endmacro

IRQ_STUB 0, 32

ISR_NO_ERROR 0
ISR_NO_ERROR 1
ISR_NO_ERROR 2
ISR_NO_ERROR 3
ISR_NO_ERROR 4
ISR_NO_ERROR 5
ISR_NO_ERROR 6
ISR_NO_ERROR 7
ISR_ERROR    8
ISR_NO_ERROR 9
ISR_ERROR   10
ISR_ERROR   11
ISR_ERROR   12
ISR_ERROR   13
ISR_ERROR   14
ISR_NO_ERROR 15
ISR_NO_ERROR 16
ISR_NO_ERROR 17
ISR_NO_ERROR 18
ISR_NO_ERROR 19
ISR_NO_ERROR 20
ISR_NO_ERROR 21
ISR_NO_ERROR 22
ISR_NO_ERROR 23
ISR_NO_ERROR 24
ISR_NO_ERROR 25
ISR_NO_ERROR 26
ISR_NO_ERROR 27
ISR_NO_ERROR 28
ISR_NO_ERROR 29
ISR_NO_ERROR 30
ISR_NO_ERROR 31

ISR_NO_ERROR 129
ISR_NO_ERROR 254

idt_common_stub:
    push rax
    push rbx
    push rcx
    push rdx
    push rsi
    push rdi
    push rbp
    push r8
    push r9
    push r10
    push r11
    push r12
    push r13
    push r14
    push r15

    mov rdi, rsp          ; Argument 1 for C++ (regs*)
    
    mov rbp, rsp          ; Save original stack pointer in callee-saved RBP
    and rsp, -16          ; Align stack to 16 bytes
    call idt_handler

    mov rsp, rax

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
    iretq