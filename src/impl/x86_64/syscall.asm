[bits 64]
%include "src/assets/gdt_constants.inc"
global syscall_entry
extern syscall_handler

%define REGS_SIZE 176

syscall_entry:
    swapgs
    mov [gs:24], rsp

    mov rsp, [gs:16]

    sub rsp, REGS_SIZE

    mov [rsp + 0*8],  r15
    mov [rsp + 1*8],  r14
    mov [rsp + 2*8],  r13
    mov [rsp + 3*8],  r12
    mov [rsp + 4*8],  r11
    mov [rsp + 5*8],  r10
    mov [rsp + 6*8],  r9
    mov [rsp + 7*8],  r8
    mov [rsp + 8*8],  rbp
    mov [rsp + 9*8],  rdi
    mov [rsp +10*8],  rsi
    mov [rsp +11*8],  rdx
    mov [rsp +12*8],  rcx
    mov [rsp +13*8],  rbx
    mov [rsp +14*8],  rax

    mov qword [rsp +15*8], 0    ; int_no
    mov qword [rsp +16*8], 0    ; err_code

    ; user return frame: rip, cs, rflags, rsp, ss
    mov qword [rsp +17*8], rcx          ; rip
    mov qword [rsp +18*8], UCODE64_SEL ; cs
    mov qword [rsp +19*8], r11          ; rflags
    mov rax, [gs:24]
    mov qword [rsp +20*8], rax          ; rsp (user)
    mov qword [rsp +21*8], UDATA64_SEL ; ss

    mov rdi, rsp
    call syscall_handler
    mov [rsp +14*8], rax

    mov r15, [rsp + 0*8]
    mov r14, [rsp + 1*8]
    mov r13, [rsp + 2*8]
    mov r12, [rsp + 3*8]
    mov r10, [rsp + 5*8]
    mov r9,  [rsp + 6*8]
    mov r8,  [rsp + 7*8]
    mov rbp, [rsp + 8*8]
    mov rdi, [rsp + 9*8]
    mov rsi, [rsp +10*8]
    mov rdx, [rsp +11*8]
    mov rbx, [rsp +13*8]
    mov rax, [rsp +14*8]

    mov rcx, [rsp +17*8]   ; SYSRET target RIP
    mov r11, [rsp +19*8]   ; SYSRET target RFLAGS
    mov rsp, [rsp +20*8]   ; SYSRET target RSP
    swapgs
    o64 sysret
