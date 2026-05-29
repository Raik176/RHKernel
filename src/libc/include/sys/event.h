#ifndef _SYS_EVENT_H
#define _SYS_EVENT_H

#include <stdint.h>
#include <stddef.h>
#include <sys/types.h>

#ifndef RH_TASK_EVENT_ABI_H
#define RH_TASK_EVENT_ABI_H

#define TASK_EVENT_DATA_SIZE 32U
#define TASK_EVENT_QUEUE_SIZE 64U

#define TASK_EVENT_NONE 0U
#define TASK_EVENT_QUIT 1U
#define TASK_EVENT_FORCE_KILL 2U
#define TASK_EVENT_EXIT 3U
#define TASK_EVENT_KILLED 4U
#define TASK_EVENT_FAULTED 5U
#define TASK_EVENT_PAGE_FAULT 6U
#define TASK_EVENT_GENERAL_FAULT 7U
#define TASK_EVENT_DIVIDE_BY_ZERO 8U
#define TASK_EVENT_INVALID_OPCODE 9U
#define TASK_EVENT_BREAKPOINT 10U
#define TASK_EVENT_OVERFLOW 11U
#define TASK_EVENT_BOUNDS 12U
#define TASK_EVENT_STACK_FAULT 13U
#define TASK_EVENT_FPU_FAULT 14U
#define TASK_EVENT_ALIGNMENT_FAULT 15U
#define TASK_EVENT_USER 16U
#define TASK_EVENT_USER_FAULT 17U

#define TASK_EVENT_F_DROPPED 0x00000001U
#define TASK_EVENT_F_TRUNCATED 0x00000002U
#define TASK_EVENT_F_SYNCHRONOUS 0x00000004U
#define TASK_EVENT_F_RECOVERABLE 0x00000008U

#define TASK_FAULTCTL_F_ENABLED 0x00000001U
#define TASK_FAULTCTL_F_ONESHOT 0x00000002U

#define TASK_FAULT_RETURN_UNHANDLED 0U
#define TASK_FAULT_RETURN_RESUME 1U
#define TASK_FAULT_RETURN_RESUME_AT 2U
#define TASK_FAULT_RETURN_EXIT 3U

#define TASK_EVENT_MASK(type) (1ULL << (type))

struct task_event {
    uint32_t type;
    uint32_t flags;
    uint64_t source;
    uint64_t target;
    int64_t code;
    uint64_t detail;
    uint32_t data_len;
    uint8_t data[TASK_EVENT_DATA_SIZE];
} __attribute__((packed));

struct task_faultctl {
    uint32_t version;
    uint32_t flags;
    uint64_t event_mask;
    uint64_t handler_ip;
    uint64_t handler_sp;
    uint32_t max_depth;
    uint32_t reserved;
} __attribute__((packed));

struct task_fault_regs {
    uint64_t r15;
    uint64_t r14;
    uint64_t r13;
    uint64_t r12;
    uint64_t r11;
    uint64_t r10;
    uint64_t r9;
    uint64_t r8;
    uint64_t rbp;
    uint64_t rdi;
    uint64_t rsi;
    uint64_t rdx;
    uint64_t rcx;
    uint64_t rbx;
    uint64_t rax;
    uint64_t rip;
    uint64_t rflags;
    uint64_t rsp;
} __attribute__((packed));

struct task_fault_frame {
    struct task_event event;
    struct task_fault_regs regs;
    uint64_t fault_address;
    uint64_t cpu_error;
    uint64_t arch_vector;
    uint64_t depth;
} __attribute__((packed));

struct task_fault_return {
    uint32_t action;
    uint32_t reserved;
    int64_t code;
    uint64_t resume_ip;
    uint64_t resume_sp;
    struct task_fault_regs regs;
} __attribute__((packed));

#ifdef __cplusplus
static_assert(sizeof(task_event) == 76, "task_event ABI changed");
static_assert(sizeof(task_faultctl) == 40, "task_faultctl ABI changed");
static_assert(sizeof(task_fault_regs) == 144, "task_fault_regs ABI changed");
#endif

#endif

#ifdef __cplusplus
extern "C" {
#endif

int task_event_open_self_inbox(void);
int task_event_open_self_poll(void);
int task_event_open_self_fault(void);
int task_event_open_self_faultctl(void);
int task_event_open_pid_inbox(pid_t pid);
int task_event_recv(int fd, struct task_event *event);
int task_event_send(int fd, const struct task_event *event);
int task_event_send_pid(pid_t pid, const struct task_event *event);
int task_event_send_user(pid_t pid, int64_t code, const void *data, size_t size);
int task_event_wait(struct task_event *event);
int task_fault_read(struct task_fault_frame *frame);
int task_fault_reply(const struct task_fault_return *result);
int task_fault_configure(const struct task_faultctl *ctl);

#ifdef __cplusplus
}
#endif

#endif
