[bits 64]
global syscall_entry
extern syscall_handler

; size: 22 qwords (15 GP regs + 2 ints + 5 iret fields) = 22*8 = 176 bytes
%define REGS_SIZE 176

syscall_entry:
    ; swap to kernel GS base and switch to kernel stack:
    swapgs
    mov [gs:24], rsp        ; save user RSP to cpu_local.user_rsp (gs:24)
    mov rsp, [gs:16]        ; load kernel_stack from cpu_local.kernel_stack (gs:16)

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

    ; --- int_no / err_code (for syscalls we set them to zero) ---
    mov qword [rsp +15*8], 0    ; int_no
    mov qword [rsp +16*8], 0    ; err_code

    ; --- user iret frame: rip, cs, rflags, rsp, ss ---
    ; RCX = user RIP (CPU put it there on syscall), R11 = user RFLAGS
    mov qword [rsp +17*8], rcx          ; rip
    mov qword [rsp +18*8], 0x23         ; cs (user code selector)
    mov qword [rsp +19*8], r11          ; rflags
    mov rax, [gs:24]                     ; rax <- saved user RSP
    mov qword [rsp +20*8], rax          ; rsp (user)
    mov qword [rsp +21*8], 0x1B         ; ss (user data selector)

    ; pass pointer to struct regs (rsp points to the struct) in rdi (SysV)
    mov rdi, rsp
    call syscall_handler
    mov [rsp + 14*8], rax   ; store return value

    ; After the handler returns: reload GP registers from the struct area
    ; NOTE: we restore GP registers except we don't need to restore rcx/r11 for syscall/sysret,
    ; but restoring them (to user values) is harmless here when we then iretq.
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

    ; Prepare for iretq: set RSP to the saved user RSP and point stack to the iret frame
    ; Our iret frame fields start at offset 17*8 from the base (rsp)
    lea rdx, [rsp + 17*8]   ; rdx -> address of saved RIP (iret frame)
    ; free the regs area (optional -- not strictly necessary since we set rsp next)
    ; add rsp, REGS_SIZE   ; <-- don't do this because we need rdx as target
    ; switch RSP to the iret frame (so iretq pops RIP/CS/RFLAGS/RSP/SS)
    mov rsp, rdx

    swapgs                  ; restore user GS base
    iretq                   ; pop RIP, CS, RFLAGS, RSP, SS and return to user
; TODO: switch to sysretq