#pragma once
#include <stdint.h>

namespace boot_options {
    enum class root_kind : uint8_t { none, disk, part };

    struct options {
        bool valid;
        root_kind root;
        char root_uuid[37];
        char rootfs[32];
        char rootmode[3];
        char error[80];
    };

    void init(uint64_t mb_phys_addr);
    const options &get();
}
