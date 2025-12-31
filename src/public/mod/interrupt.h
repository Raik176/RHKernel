#pragma once
#include <stdbool.h>
#include <stdint.h>

/**
 * @brief CPU mask for interrupt affinity.
 * bits 0-63 represent CPU IDs. 0xFFFFFFFFFFFFFFFF means all CPUs.
 */
typedef uint64_t cpu_affinity_t;

#define IRQ_AFFINITY_ALL ((cpu_affinity_t) - 1)

/**
 * @brief Interrupt handler return values.
 */
enum irq_return {
    IRQ_HANDLED,      // Handler dealt with the interrupt
    IRQ_NOT_HANDLED,  // Interrupt wasn't for this handler
    IRQ_CHAIN         // Continue to next handler in chain (default)
};

/**
 * @brief Signature for module interrupt handlers.
 */
typedef enum irq_return (*irq_handler_t)(void *priv);

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Register a handler for a specific IRQ.
 * * @param irq The hardware IRQ number (Legacy IRQ or GSI).
 * @param handler Function pointer to the handler.
 * @param affinity Bitmask of allowed CPUs.
 * @param priv Private data passed to the handler.
 * @return int 0 on success, negative on failure.
 */
int request_irq(uint8_t irq, irq_handler_t handler, cpu_affinity_t affinity, void *priv);

/**
 * @brief Unregister a handler.
 */
void free_irq(uint8_t irq, irq_handler_t handler);

#ifdef __cplusplus
}
#endif