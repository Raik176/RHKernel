#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/event.h>
#include <sys/result.h>
#include <unistd.h>

static int open_event_path(const char *path) { return open(path, O_RDWR); }

static int pid_event_path(char *buf, size_t size, pid_t pid, const char *leaf) {
    if (!buf || !leaf || pid < 0) return LIBC_RESULT_INVAL;
    int n = snprintf(buf, size, "/task/%ld/events/%s", (long)pid, leaf);
    if (n < 0) return LIBC_RESULT_IO;
    return (size_t)n < size ? LIBC_RESULT_OK : LIBC_RESULT_NAMETOOLONG;
}

int task_event_open_self_inbox(void) { return open_event_path("/task/self/events/inbox"); }
int task_event_open_self_poll(void) { return open_event_path("/task/self/events/poll"); }
int task_event_open_self_fault(void) { return open_event_path("/task/self/events/fault"); }
int task_event_open_self_faultctl(void) { return open_event_path("/task/self/events/faultctl"); }

int task_event_open_pid_inbox(pid_t pid) {
    char path[64];
    int r = pid_event_path(path, sizeof(path), pid, "inbox");
    return r < 0 ? r : open_event_path(path);
}

int task_event_recv(int fd, struct task_event *event) {
    if (!event) return LIBC_RESULT_INVAL;
    ssize_t n = read(fd, event, sizeof(*event));
    if (n < 0) return (int)n;
    return n == (ssize_t)sizeof(*event) ? LIBC_RESULT_OK : LIBC_RESULT_IO;
}

int task_event_send(int fd, const struct task_event *event) {
    if (!event) return LIBC_RESULT_INVAL;
    if (event->data_len > TASK_EVENT_DATA_SIZE) return LIBC_RESULT_INVAL;
    ssize_t n = write(fd, event, sizeof(*event));
    if (n < 0) return (int)n;
    return n == (ssize_t)sizeof(*event) ? LIBC_RESULT_OK : LIBC_RESULT_IO;
}

int task_event_send_pid(pid_t pid, const struct task_event *event) {
    int fd = task_event_open_pid_inbox(pid);
    if (fd < 0) return fd;
    int r = task_event_send(fd, event);
    int c = close(fd);
    return r < 0 ? r : c;
}

int task_event_send_user(pid_t pid, int64_t code, const void *data, size_t size) {
    if (size > TASK_EVENT_DATA_SIZE) return LIBC_RESULT_INVAL;
    if (size && !data) return LIBC_RESULT_INVAL;

    struct task_event event;
    memset(&event, 0, sizeof(event));
    event.type = TASK_EVENT_USER;
    event.code = code;
    event.data_len = (uint32_t)size;
    if (size) memcpy(event.data, data, size);
    return task_event_send_pid(pid, &event);
}

int task_event_wait(struct task_event *event) {
    int fd = task_event_open_self_inbox();
    if (fd < 0) return fd;
    int r = task_event_recv(fd, event);
    int c = close(fd);
    return r < 0 ? r : c;
}

int task_fault_read(struct task_fault_frame *frame) {
    if (!frame) return LIBC_RESULT_INVAL;
    int fd = task_event_open_self_fault();
    if (fd < 0) return fd;
    ssize_t n = read(fd, frame, sizeof(*frame));
    int c = close(fd);
    if (n < 0) return (int)n;
    if (n != (ssize_t)sizeof(*frame)) return LIBC_RESULT_IO;
    return c;
}

int task_fault_reply(const struct task_fault_return *result) {
    if (!result) return LIBC_RESULT_INVAL;
    int fd = task_event_open_self_fault();
    if (fd < 0) return fd;
    ssize_t n = write(fd, result, sizeof(*result));
    int c = close(fd);
    if (n < 0) return (int)n;
    if (n != (ssize_t)sizeof(*result)) return LIBC_RESULT_IO;
    return c;
}

int task_fault_configure(const struct task_faultctl *ctl) {
    if (!ctl) return LIBC_RESULT_INVAL;
    int fd = task_event_open_self_faultctl();
    if (fd < 0) return fd;
    ssize_t n = write(fd, ctl, sizeof(*ctl));
    int c = close(fd);
    if (n < 0) return (int)n;
    if (n != (ssize_t)sizeof(*ctl)) return LIBC_RESULT_IO;
    return c;
}
