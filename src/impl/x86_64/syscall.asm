[bits 64]
extern syscall_handler
global syscall_entry

syscall_entry:
    ; Swap GS to get access to cpu_local (per-cpu storage)
    swapgs
    
    ; Save user stack pointer and load kernel stack pointer
    mov [gs:24], rsp             ; gs:16 is smp::cpu_local->user_rsp
    mov rsp, [gs:16]              ; gs:8 is smp::cpu_local->kernel_stack

    ; Construct the 'regs' struct on the stack
    ; Note: SS, RSP, RFLAGS, CS, RIP are handled differently in syscall vs interrupts
    push 0x2B                    ; SS (User Data)
    push qword [gs:24]           ; RSP (User RSP)
    push r11                     ; RFLAGS
    push 0x23                    ; CS (User Code)
    push rcx                     ; RIP (Return address)
    
    push 0                       ; error code
    push 0                       ; int_no
    
    push rax                     ; Save rest of the registers to match 'struct regs'
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

    ; The first argument to syscall_handler is the regs* (RDI)
    mov rdi, rsp
    
    ; Call the C++ handler
    call syscall_handler

    ; Return value from C++ is in RAX, but we need to restore RAX from stack
    ; If your handler modifies RAX in the struct, move it to the actual RAX now
    mov [rsp + 112], rax         ; Offset of RAX in our pushed stack

    ; Restore registers
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

    ; Clean up error code and int_no
    add rsp, 16

    ; Prepare for sysretq
    pop rcx                      ; Restore RIP into RCX for sysret
    add rsp, 8                   ; Skip CS
    pop r11                      ; Restore RFLAGS into R11 for sysret
    pop qword [gs:24]            ; Store User RSP temporarily
    add rsp, 8                   ; Skip SS

    mov rsp, [gs:24]             ; Switch back to User stack
    swapgs                       ; Restore GS to user mode
    sysretq