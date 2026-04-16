/**
 * Network Driver Abstraction for SLM-OS
 *
 * Lightweight driver ops struct that decouples the lwIP netif adapter
 * (lwip_slm.c) from any specific hardware driver. Each platform
 * registers its driver via net_register_driver() before net_init().
 */

#ifndef NET_DRIVER_H
#define NET_DRIVER_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

struct net_driver {
    const char *name;
    int  (*init)(void);
    /*
     * send() submits a packet for transmission and returns
     * immediately. Return values:
     *   0           — submitted; the buffer has been copied and the
     *                 caller may free it. Completion happens
     *                 asynchronously via tx_reap().
     *   NET_E_BUSY  — TX pool exhausted; caller should retry after
     *                 net_poll() drains some completions.
     *   <0          — other error per net_error.
     */
    int  (*send)(const void *buf, size_t len);
    int  (*recv)(void *buf, size_t max_len);
    void (*get_mac)(uint8_t mac[6]);
    bool (*link_status)(void);
    /*
     * Drain completed TX descriptors and free their pool buffers.
     * Called from net_poll() once per poll. Optional — drivers that
     * complete TX synchronously inside send() may leave it NULL.
     */
    void (*tx_reap)(void);
};

/**
 * Register a network driver.
 *
 * Must be called before net_init(). Only one driver can be active.
 *
 * @param drv  Pointer to driver ops (must remain valid for kernel lifetime)
 */
void net_register_driver(const struct net_driver *drv);

/**
 * Get the currently registered network driver.
 *
 * @return  Pointer to active driver, or NULL if none registered
 */
const struct net_driver *net_get_driver(void);

#endif /* NET_DRIVER_H */
