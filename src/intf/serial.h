/**
 * @file serial.h
 * @brief Kernel serial port interface
 *
 * Provides functions for initializing and writing to a serial port.
 */

#pragma once
#include <stdint.h>

namespace serial {

    /**
     * @brief Initialize the serial port
     *
     * Configures the UART for basic output. Should be called before using `putchar`.
     */
    void init();

    /**
     * @brief Write a single character to the serial port
     *
     * Sends the character `c` over the serial port.
     *
     * @param c Character to send
     */
    void putchar(char c);

}  // namespace serial