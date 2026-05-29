#pragma once
#include <stdint.h>

#include "mod/workqueue.h"

namespace workqueue {
    struct stats {
        uint64_t queued;
        uint64_t completed;
        uint64_t failed;
        uint64_t rejected;
        uint64_t wakeups;
        uint64_t waits;
        uint64_t max_depth;
        uint64_t depth;
        uint64_t worker_pid;
        bool initialized;
        bool started;
    };

    void init();
    bool start();
    int queue(kernel_work *work);
    void snapshot(stats *out);
    uint64_t format_status(char *buf, uint64_t cap);
}
