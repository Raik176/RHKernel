#pragma once

namespace devfs {
    /**
     * @brief Bootstraps the /dev directory in the VFS.
     */
    void init();
}  // namespace devfs

namespace procfs {
    /**
     * @brief Bootstraps the /proc directory in the VFS.
     */
    void init();
}  // namespace procfs

void init_virt_fs();