#include "serial.h"

namespace serial {

    static inline void outb(uint16_t port, uint8_t val) {
        asm volatile("outb %0, %1" : : "a"(val), "Nd"(port));
    }

    static inline uint8_t inb(uint16_t port) {
        uint8_t ret;
        asm volatile("inb %1, %0" : "=a"(ret) : "Nd"(port));
        return ret;
    }

    static constexpr uint16_t COM1 = 0x3F8;

    void init() {
        outb(COM1 + 1, 0x00);  // Disable interrupts
        outb(COM1 + 3, 0x80);  // Enable DLAB
        outb(COM1 + 0, 0x03);  // Baud divisor low (38400)
        outb(COM1 + 1, 0x00);  // Baud divisor high
        outb(COM1 + 3, 0x03);  // 8 bits, no parity, one stop
        outb(COM1 + 2, 0xC7);  // Enable FIFO
        outb(COM1 + 4, 0x0B);  // IRQs enabled, RTS/DSR
    }

    static inline int can_transmit() { return inb(COM1 + 5) & 0x20; }

    void putchar(char c) {
        while (!can_transmit()) {}
        outb(COM1, c);
    }

}  // namespace serial