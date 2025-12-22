#pragma once

namespace scheduler {
    struct Task {
        uint64_t id;
        uintptr_t stack_pointer;
        uintptr_t cr3;
    
        uint64_t vruntime;
        uint32_t weight;
    
    Task* next;               // For a simple list, but eventually move to RBTree
};
}