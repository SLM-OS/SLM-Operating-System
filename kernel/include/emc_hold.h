/*
 * emc_hold.h - Keep the Jetson memory clock (EMC) at the rate SLM-OS asked
 * for, and find out what moves it (#1161).
 *
 * UEFI leaves EMC at 204 MHz. A BPMP request for 3199 MHz made right after
 * bpmp_init reads back 3199 MHz, yet the shell later reads 204 MHz. The
 * core below reads the rate back, counts a drop below the target, notes
 * where it was seen, and asks again. It is platform independent: the
 * BPMP calls come in through struct emc_hold_ops, so a test can drive it
 * with a fake.
 *
 * On Jetson, emc_hold_boot_begin() also installs a BPMP post-request hook
 * (bpmp_set_post_op_hook) so every BPMP request made during boot (GPU,
 * XUSB, fan) is followed by a readback. A drop is then attributed to the
 * exact request after which it was first seen.
 */
#ifndef EMC_HOLD_H
#define EMC_HOLD_H

#include <stdbool.h>
#include <stdint.h>

struct emc_hold_ops {
    /* Current rate in Hz. 0 on success. */
    int (*get_rate)(uint64_t *hz);
    /* Ask for hz again; *got is the rate BPMP reports. 0 on success. */
    int (*request)(uint64_t hz, uint64_t *got);
};

#define EMC_HOLD_WHERE_LEN 48

struct emc_hold_state {
    const struct emc_hold_ops *ops;
    uint64_t target_hz;      /* rate to hold; 0 = observe only */
    uint64_t last_hz;        /* rate seen at the previous check */
    uint32_t checks;         /* readbacks that succeeded */
    uint32_t read_errors;    /* readbacks that failed */
    uint32_t drops;          /* readbacks below target_hz */
    uint32_t reasserts;      /* re-requests after a drop that read back >= target */
    char first_drop[EMC_HOLD_WHERE_LEN];   /* where the first drop was seen */
    char last_drop[EMC_HOLD_WHERE_LEN];    /* where the latest drop was seen */
};

/* Result of one emc_hold_check. */
enum emc_hold_result {
    EMC_HOLD_OK = 0,          /* at or above target (or observe only) */
    EMC_HOLD_RESTORED,        /* was below target, re-request brought it back */
    EMC_HOLD_STILL_LOW,       /* was below target, re-request did not help */
    EMC_HOLD_READ_ERROR,      /* the readback itself failed */
};

void emc_hold_init(struct emc_hold_state *s, const struct emc_hold_ops *ops,
                   uint64_t target_hz);

/*
 * Read the rate. If it is below target_hz, count a drop, record `where`,
 * and request target_hz again. *before_hz / *after_hz (either may be NULL)
 * get the rate read and, after a re-request, the rate read back.
 */
enum emc_hold_result emc_hold_check(struct emc_hold_state *s, const char *where,
                                    uint64_t *before_hz, uint64_t *after_hz);

/* ---- Jetson boot and runtime binding (stubs elsewhere) ---- */

/*
 * After bpmp_init: log the rate UEFI left, request the target through
 * MRQ_CLK, read it back, and arm the per-BPMP-request watch. Boot steps
 * then call emc_hold_boot_step(); emc_hold_boot_end() disarms the watch
 * and prints a summary.
 */
void emc_hold_boot_begin(void);
void emc_hold_boot_step(const char *step);
void emc_hold_boot_end(void);

/* Periodic check from a task (runtime drop detection). */
void emc_hold_runtime_check(void);

/* Snapshot of the global state for the `clocks` shell command. */
void emc_hold_get_status(struct emc_hold_state *out);

#endif /* EMC_HOLD_H */
