#pragma once
#include "file/vfs.h"
#include "smp/scheduler.h"
#include "smp/smp.h"

namespace fd_manager {
    int alloc_fd(scheduler::task *t = smp::get_cpu()->current_task);
    vfs::open_file *get_file(int fd, scheduler::task *t = smp::get_cpu()->current_task);
    bool expand_table(uint32_t needed_capacity, scheduler::task *t = smp::get_cpu()->current_task);
    bool reserve_fd(int fd, scheduler::task *t = smp::get_cpu()->current_task);
    void release_reserved_fd(int fd, scheduler::task *t = smp::get_cpu()->current_task);
    int close_fd(int fd, scheduler::task *t = smp::get_cpu()->current_task);
}  // namespace fd_manager