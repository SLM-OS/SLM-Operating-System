/**
 * Cadence MACB/GEM Ethernet driver (Pi 5) — public interface.
 *
 * Symmetric with kernel/include/virtio_net.h and virtio_net_pci.h for
 * the other platforms. Gated on PLATFORM_RASPI5 at the .c side.
 *
 * Linux device tree compat: "raspberrypi,rp1-gem", "cdns,macb".
 */

#ifndef MACB_H
#define MACB_H

#include <stdint.h>
#include <stdbool.h>

/**
 * Register the Cadence MACB/GEM driver with the net_driver abstraction.
 * Called from kernel/src/main.c platform init (RASPI5 branch) before
 * net_init() runs.
 */
void macb_register(void);

/**
 * Driver init entry point — same shape as the other net_driver inits.
 * Returns 0 on success, -1 if hardware probe fails. net_init() treats
 * any non-zero return as "no network driver available" and the shell
 * comes up without networking.
 */
int macb_init(void);

/**
 * Count of MACB IRQs that have actually been delivered via the GIC.
 * Bumped every time macb_irq_handler runs. Stays 0 when IRQ delivery
 * fails (e.g. RP1 MSIX_CFG engine not firing on peripheral assertion —
 * a known issue on Pi 5 per kernel/drivers/uart_rp1.c). The driver
 * falls back to polled completion regardless; this counter lets
 * diagnostics + tests distinguish "IRQs live" from "IRQs silent".
 */
uint32_t macb_get_irq_count(void);

/**
 * Contents of MACB_ISR last time the handler ran. Useful for looking
 * at which interrupt lines the MAC actually asserts when something
 * does fire.
 */
uint32_t macb_get_last_isr(void);

/**
 * Whether the GIC handler table registration succeeded. Does NOT
 * guarantee IRQs are flowing — use macb_get_irq_count() for that.
 */
bool macb_irq_is_registered(void);

#endif /* MACB_H */
