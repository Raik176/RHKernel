[bits 64]
%include "src/assets/gdt_constants.inc"
global syscall_entry
extern syscall_handler

%define INT_SYSENTER 0x81

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

    cmp qword [rsp +18*8], UCODE64_SEL
    jne .iret_return
    cmp qword [rsp +21*8], UDATA64_SEL
    jne .iret_return

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

.iret_return:
    mov r15, [rsp + 0*8]
    mov r14, [rsp + 1*8]
    mov r13, [rsp + 2*8]
    mov r12, [rsp + 3*8]
    mov r11, [rsp + 4*8]
    mov r10, [rsp + 5*8]
    mov r9,  [rsp + 6*8]
    mov r8,  [rsp + 7*8]
    mov rbp, [rsp + 8*8]
    mov rdi, [rsp + 9*8]
    mov rsi, [rsp +10*8]
    mov rdx, [rsp +11*8]
    mov rcx, [rsp +12*8]
    mov rbx, [rsp +13*8]
    mov rax, [rsp +14*8]
    swapgs
    add rsp, 15*8
    add rsp, 16
    iretq

; 32-bit SYSENTER ABI:
; eax=syscall, ebx/esi/edi/ebp=args, edx=return eip, ecx=return esp.
global sysenter_entry
sysenter_entry:
    cld
    swapgs
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

    mov qword [rsp +15*8], INT_SYSENTER
    mov qword [rsp +16*8], 0
    mov qword [rsp +17*8], rdx
    mov qword [rsp +18*8], UCODE32_SEL
    pushfq
    pop rax
    or rax, 0x200
    and rax, ~0x400
    mov qword [rsp +19*8], rax
    mov qword [rsp +20*8], rcx
    mov qword [rsp +21*8], UDATA32_SEL

    mov rdi, rsp
    call syscall_handler
    mov [rsp +14*8], rax

    cmp qword [rsp +15*8], INT_SYSENTER
    jne .sysenter_iret_return
    cmp qword [rsp +18*8], UCODE32_SEL
    jne .sysenter_iret_return
    cmp qword [rsp +21*8], UDATA32_SEL
    jne .sysenter_iret_return
    mov r11, 0x00000000FFFFFFFF
    cmp qword [rsp +17*8], r11
    ja .sysenter_iret_return
    cmp qword [rsp +20*8], r11
    ja .sysenter_iret_return
    test qword [rsp +19*8], (1 << 8) | (1 << 14) | (1 << 16) | (1 << 17)
    jnz .sysenter_iret_return

    mov r15, [rsp + 0*8]
    mov r14, [rsp + 1*8]
    mov r13, [rsp + 2*8]
    mov r12, [rsp + 3*8]
    mov r11, [rsp + 4*8]
    mov r10, [rsp + 5*8]
    mov r9,  [rsp + 6*8]
    mov r8,  [rsp + 7*8]
    mov rbp, [rsp + 8*8]
    mov rdi, [rsp + 9*8]
    mov rsi, [rsp +10*8]
    mov rbx, [rsp +13*8]
    mov rax, [rsp +14*8]

    mov rdx, [rsp +17*8]
    mov rcx, [rsp +20*8]
    mov r11, [rsp +19*8]
    and r11, 0x00000000000008D5
    or  r11, 0x0000000000000002
    push r11
    popfq
    swapgs
    sti
    sysexit

.sysenter_iret_return:
    mov r15, [rsp + 0*8]
    mov r14, [rsp + 1*8]
    mov r13, [rsp + 2*8]
    mov r12, [rsp + 3*8]
    mov r11, [rsp + 4*8]
    mov r10, [rsp + 5*8]
    mov r9,  [rsp + 6*8]
    mov r8,  [rsp + 7*8]
    mov rbp, [rsp + 8*8]
    mov rdi, [rsp + 9*8]
    mov rsi, [rsp +10*8]
    mov rdx, [rsp +11*8]
    mov rcx, [rsp +12*8]
    mov rbx, [rsp +13*8]
    mov rax, [rsp +14*8]
    swapgs
    add rsp, 15*8
    add rsp, 16
    iretq
