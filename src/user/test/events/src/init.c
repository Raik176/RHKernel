#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <sys/event.h>






static void wr(int fd, const char *s) { write(fd, s, strlen(s)); }


static int read_event(struct task_event *ev) {
    return task_event_wait(ev);
}

static int send_self_user_event(void) {
    const char data[4] = {'t', 'e', 's', 't'};
    if (task_event_send_user(getpid(), 44, data, sizeof(data)) != 0) return -1;

    struct task_event got;
    if (read_event(&got) != 0) return -1;
    return got.type == TASK_EVENT_USER && got.code == 44 && got.data_len == 4 &&
           got.data[0] == 't' && got.data[1] == 'e' && got.data[2] == 's' && got.data[3] == 't'
               ? 0
               : -1;
}

static uint8_t fault_stack[4096] __attribute__((aligned(16)));
static volatile int fault_recovered;

static uintptr_t c_handler_stack(void *base, size_t size) {
    return (((uintptr_t)base + size) & ~(uintptr_t)15) - 8;
}

__attribute__((noinline, noreturn, no_stack_protector, optimize("O0"))) static void fault_handler(void) {
    struct task_fault_frame frame;
    if (task_fault_read(&frame) != 0) _exit(81);
    if (frame.event.type != TASK_EVENT_BREAKPOINT) _exit(82);

    struct task_fault_return ret;
    for (size_t i = 0; i < sizeof(ret); i++) ((uint8_t *)&ret)[i] = 0;
    ret.action = TASK_FAULT_RETURN_RESUME;
    if (task_fault_reply(&ret) != 0) _exit(83);
    _exit(84);
}

static int recover_breakpoint(void) {
    struct task_faultctl ctl;
    for (size_t i = 0; i < sizeof(ctl); i++) ((uint8_t *)&ctl)[i] = 0;
    ctl.version = 1;
    ctl.flags = TASK_FAULTCTL_F_ENABLED;
    ctl.event_mask = TASK_EVENT_MASK(TASK_EVENT_BREAKPOINT);
    ctl.handler_ip = (uintptr_t)fault_handler;
    ctl.handler_sp = c_handler_stack(fault_stack, sizeof(fault_stack));
    ctl.max_depth = 1;
    if (task_fault_configure(&ctl) != 0) return -1;

    asm volatile("int3" ::: "memory");
    fault_recovered = 1;
    return fault_recovered == 1 ? 0 : -1;
}

static int child_exit_event(void) {
    pid_t pid = fork();
    if (pid == 0) _exit(7);
    if (pid < 0) return -1;

    struct task_event ev;
    if (read_event(&ev) != 0) return -1;
    if (ev.type != TASK_EVENT_EXIT || ev.source != (uint64_t)pid || ev.code != 7) return -1;

    int status = -1;
    if (wait(&status) != pid || status != 7) return -1;
    return 0;
}

static int child_fault_event(void) {
    pid_t pid = fork();
    if (pid == 0) {
        *(volatile uint64_t *)0 = 1;
        _exit(99);
    }
    if (pid < 0) return -1;

    struct task_event ev;
    if (read_event(&ev) != 0) return -1;
    if (ev.type != TASK_EVENT_FAULTED || ev.source != (uint64_t)pid || ev.detail != 0) return -1;

    int status = 0;
    if (wait(&status) != pid) return -1;
    return status != 99 ? 0 : -1;
}

int main(void) {
    if (send_self_user_event() != 0) {
        wr(STDERR_FILENO, "events: self event failed\n");
        return 1;
    }
    if (recover_breakpoint() != 0) {
        wr(STDERR_FILENO, "events: breakpoint recovery failed\n");
        return 1;
    }
    if (child_exit_event() != 0) {
        wr(STDERR_FILENO, "events: child exit event failed\n");
        return 1;
    }
    if (child_fault_event() != 0) {
        wr(STDERR_FILENO, "events: child fault event failed\n");
        return 1;
    }
    wr(STDOUT_FILENO, "events: ok\n");
    return 0;
}
