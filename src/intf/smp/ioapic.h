#pragma once

namespace ioapic {
    void init();
    void set_redirection(uint32_t gsi, uint8_t vector, uint32_t lapic_id, bool mask);
    uint32_t resolve_gsi(uint8_t irq);
}  // namespace ioapic