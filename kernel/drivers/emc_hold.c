/*
 * emc_hold.c - Hold the Jetson memory clock at the requested rate (#1161).
 * See kernel/include/emc_hold.h.
 */
#include "emc_hold.h"

#include <stddef.h>

static void copy_where(char *dst, const char *src)
{
    size_t i = 0;
    if (src != NULL) {
        for (; i + 1 < EMC_HOLD_WHERE_LEN && src[i] != '\0'; i++) {
            dst[i] = src[i];
        }
    }
    dst[i] = '\0';
}

void emc_hold_init(struct emc_hold_state *s, const struct emc_hold_ops *ops,
                   uint64_t target_hz)
{
    s->ops = ops;
    s->target_hz = target_hz;
    s->last_hz = 0;
    s->checks = 0;
    s->read_errors = 0;
    s->drops = 0;
    s->reasserts = 0;
    s->first_drop[0] = '\0';
    s->last_drop[0] = '\0';
}

enum emc_hold_result emc_hold_check(struct emc_hold_state *s, const char *where,
                                    uint64_t *before_hz, uint64_t *after_hz)
{
    uint64_t hz = 0;
    if (s->ops == NULL || s->ops->get_rate == NULL || s->ops->get_rate(&hz) != 0) {
        s->read_errors++;
        return EMC_HOLD_READ_ERROR;
    }
    s->checks++;
    s->last_hz = hz;
    if (before_hz != NULL) *before_hz = hz;
    if (after_hz != NULL) *after_hz = hz;
    if (s->target_hz == 0 || hz >= s->target_hz) {
        return EMC_HOLD_OK;
    }

    s->drops++;
    if (s->first_drop[0] == '\0') {
        copy_where(s->first_drop, where);
    }
    copy_where(s->last_drop, where);

    uint64_t got = 0;
    if (s->ops->request == NULL || s->ops->request(s->target_hz, &got) != 0) {
        return EMC_HOLD_STILL_LOW;
    }
    /* Trust a readback over the request's reply: the reply is the rate
     * BPMP chose, not necessarily the one that stays. */
    uint64_t now = 0;
    if (s->ops->get_rate(&now) != 0) {
        now = got;
    }
    s->last_hz = now;
    if (after_hz != NULL) *after_hz = now;
    if (now < s->target_hz) {
        return EMC_HOLD_STILL_LOW;
    }
    s->reasserts++;
    return EMC_HOLD_RESTORED;
}

#if defined(PLATFORM_JETSON_ORIN_NANO)

#include "bpmp.h"
#include "debug.h"
#include "platform.h"
#include "spinlock.h"
#include "timer.h"
#include "uart.h"

/* Investigation logging (#1161), off by default. With it on, every BPMP
 * request made during boot is logged with the EMC rate read after it,
 * and the rate is polled for 3 s right after the first request, with no
 * other request in between, to tell a reset inside BPMP from one a
 * request causes. Set EMC_HOLD_BISECT to 1 here to turn it on. */
#ifndef EMC_HOLD_BISECT
#define EMC_HOLD_BISECT 0
#endif
#define EMC_HOLD_DELAY_CHECK_MS   3000u
#define EMC_HOLD_DELAY_STEP_MS    250u

static int bpmp_emc_get(uint64_t *hz)
{
    return bpmp_clk_get_rate(TEGRA234_CLK_EMC, hz);
}

/* The memory clock belongs to BPMP's bandwidth manager. Every
 * MRQ_BWMGR_INT request, and some event inside BPMP within about 250 ms
 * of the first request after a UEFI-direct boot, recomputes it from the
 * clients' floors and bandwidths, which discards a rate set with
 * MRQ_CLK (#1161, measured on jetson-nano-2). A floor is part of that
 * computation, so ask for one on behalf of CPU cluster 0, as Linux's
 * interconnect driver does for its clients (tegra234_mc_icc_set). */
static int bpmp_emc_request(uint64_t hz, uint64_t *got)
{
    return bpmp_emc_floor_khz(BPMP_BWMGR_CLIENT_CPU_CLUSTER0,
                              (uint32_t)(hz / 1000u), got);
}

static const struct emc_hold_ops g_bpmp_ops = {
    .get_rate = bpmp_emc_get,
    .request = bpmp_emc_request,
};

/* g_hold is shared by boot (CPU 0 alone), the fan task's runtime check
 * and the `clocks` shell command; g_hold_lock serialises them. BPMP
 * requests are made under it (the MRQ lock nests inside). g_hold_busy
 * stops the boot-time BPMP hook from re-checking the requests a check
 * makes itself. */
static spinlock_t g_hold_lock = SPINLOCK_INIT;
static struct emc_hold_state g_hold;
static bool g_hold_ready;
static bool g_hold_busy;
static uint64_t g_t0;

static unsigned long long ms_since_begin(void)
{
    uint64_t f = timer_get_frequency();
    if (f == 0) return 0;
    return (unsigned long long)((timer_get_count() - g_t0) * 1000u / f);
}

static unsigned long long mhz(uint64_t hz)
{
    return (unsigned long long)(hz / 1000000u);
}

static enum emc_hold_result locked_check(const char *where, uint64_t *prev,
                                         uint64_t *before, uint64_t *after,
                                         uint32_t *drops)
{
    irq_flags_t f = spin_lock_irqsave(&g_hold_lock);
    g_hold_busy = true;
    *prev = g_hold.last_hz;
    enum emc_hold_result r = emc_hold_check(&g_hold, where, before, after);
    *drops = g_hold.drops;
    g_hold_busy = false;
    spin_unlock_irqrestore(&g_hold_lock, f);
    return r;
}

/* Check, then log outside the lock: drops always, everything else only
 * when `always` is set. */
static void log_check(const char *where, uint32_t id, int rc, bool always)
{
    uint64_t prev = 0, before = 0, after = 0;
    uint32_t drops = 0;
    enum emc_hold_result r = locked_check(where, &prev, &before, &after, &drops);
    switch (r) {
    case EMC_HOLD_OK:
        if (always) {
            uart_printf("[emc-hold] +%llu ms %s(%u) rc=%d: EMC %llu MHz%s\n",
                        ms_since_begin(), where, (unsigned)id, rc, mhz(before),
                        before != prev ? " (changed)" : "");
        }
        break;
    case EMC_HOLD_RESTORED:
    case EMC_HOLD_STILL_LOW:
        WARN("EMC dropped %llu -> %llu MHz after %s(%u) at +%llu ms; requested"
             " again, now %llu MHz (drop #%u)", mhz(prev), mhz(before), where,
             (unsigned)id, ms_since_begin(), mhz(after), (unsigned)drops);
        break;
    case EMC_HOLD_READ_ERROR:
        if (always) {
            uart_printf("[emc-hold] +%llu ms %s(%u) rc=%d: EMC readback failed\n",
                        ms_since_begin(), where, (unsigned)id, rc);
        }
        break;
    }
}

static void post_op_hook(const char *op, uint32_t id, int rc)
{
    if (g_hold_busy) return;
    log_check(op, id, rc, EMC_HOLD_BISECT != 0);
}

void emc_hold_boot_begin(void)
{
    g_t0 = timer_get_count();
    uint64_t left = 0, reply = 0, readback = 0;
    irq_flags_t f = spin_lock_irqsave(&g_hold_lock);
    emc_hold_init(&g_hold, &g_bpmp_ops, TEGRA234_EMC_MAX_HZ);
    int grc = bpmp_emc_get(&left);
    int src = bpmp_emc_request(TEGRA234_EMC_MAX_HZ, &reply);
    int rrc = bpmp_emc_get(&readback);
    g_hold.last_hz = readback;
    g_hold_ready = true;
    spin_unlock_irqrestore(&g_hold_lock, f);
    if (src != 0) {
        WARN("EMC: bandwidth-manager floor of %llu MHz refused (rc=%d), at %llu MHz",
             mhz(TEGRA234_EMC_MAX_HZ), src, mhz(readback));
    } else {
        INFO("EMC: %llu MHz (rc=%d) -> floor %llu MHz, BPMP chose %llu, reads %llu MHz (rc=%d)",
             mhz(left), grc, mhz(TEGRA234_EMC_MAX_HZ), mhz(reply), mhz(readback), rrc);
    }
#if EMC_HOLD_BISECT
    for (uint32_t t = EMC_HOLD_DELAY_STEP_MS; t <= EMC_HOLD_DELAY_CHECK_MS;
         t += EMC_HOLD_DELAY_STEP_MS) {
        timer_busy_wait_us((uint64_t)EMC_HOLD_DELAY_STEP_MS * 1000u);
        log_check("delay", t, 0, t == EMC_HOLD_DELAY_CHECK_MS);
    }
#endif
    bpmp_set_post_op_hook(post_op_hook);
}

void emc_hold_boot_step(const char *step)
{
    if (!g_hold_ready) return;
    log_check(step, 0, 0, EMC_HOLD_BISECT != 0);
}

void emc_hold_boot_end(void)
{
    if (!g_hold_ready) return;
    /* After boot the fan task checks every 2 s (emc_hold_runtime_check);
     * the hook is boot-only because it assumes a single CPU. */
    bpmp_set_post_op_hook(NULL);
    log_check("boot_end", 0, 0, EMC_HOLD_BISECT != 0);
    struct emc_hold_state s;
    emc_hold_get_status(&s);
    INFO("EMC: %llu MHz at the end of boot (%u checks, %u drops%s%s)",
         mhz(s.last_hz), (unsigned)s.checks, (unsigned)s.drops,
         s.drops ? ", first after " : "", s.drops ? s.first_drop : "");
}

void emc_hold_runtime_check(void)
{
    if (!g_hold_ready) return;
    log_check("runtime", 0, 0, false);
}

int emc_hold_set_target(uint64_t hz, uint64_t *got)
{
    uint64_t reply = 0;
    irq_flags_t f = spin_lock_irqsave(&g_hold_lock);
    if (!g_hold_ready) {
        emc_hold_init(&g_hold, &g_bpmp_ops, hz);
        g_hold_ready = true;
    }
    g_hold.target_hz = hz;
    int rc = bpmp_emc_request(hz, &reply);
    spin_unlock_irqrestore(&g_hold_lock, f);
    if (got != NULL) *got = reply;
    return rc;
}

void emc_hold_get_status(struct emc_hold_state *out)
{
    irq_flags_t f = spin_lock_irqsave(&g_hold_lock);
    *out = g_hold;
    spin_unlock_irqrestore(&g_hold_lock, f);
}

#else /* !PLATFORM_JETSON_ORIN_NANO */

void emc_hold_boot_begin(void) {}
void emc_hold_boot_step(const char *step) { (void)step; }
void emc_hold_boot_end(void) {}
void emc_hold_runtime_check(void) {}
int emc_hold_set_target(uint64_t hz, uint64_t *got)
{
    (void)hz;
    if (got != NULL) *got = 0;
    return -1;
}
void emc_hold_get_status(struct emc_hold_state *out)
{
    emc_hold_init(out, NULL, 0);
}

#endif /* PLATFORM_JETSON_ORIN_NANO */
