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

#endif /* MACB_H */
