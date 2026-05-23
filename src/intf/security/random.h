#pragma once
#include <stdint.h>

namespace random {
    void init();
    void add_entropy(const void *data, uint64_t len);
    void fill(void *buffer, uint64_t len);
    uint64_t next_u64();
}
