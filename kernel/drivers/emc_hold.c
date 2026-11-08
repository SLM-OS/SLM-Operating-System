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
#include "timer.h"
#include "uart.h"

/* Investigation build (#1161): log every BPMP request made during boot
 * with the EMC rate read after it, and poll the rate for a while right
 * after the first request to separate a time-based reset from one a
 * request causes. */
#ifndef EMC_HOLD_BISECT
#define EMC_HOLD_BISECT 1
#endif
#define EMC_HOLD_DELAY_CHECK_MS   3000u
#define EMC_HOLD_DELAY_STEP_MS    250u

static int bpmp_emc_get(uint64_t *hz)
{
    return bpmp_clk_get_rate(TEGRA234_CLK_EMC, hz);
}

static int bpmp_emc_request(uint64_t hz, uint64_t *got)
{
    return bpmp_clk_set_rate(TEGRA234_CLK_EMC, hz, got);
}

static const struct emc_hold_ops g_bpmp_ops = {
    .get_rate = bpmp_emc_get,
    .request = bpmp_emc_request,
};

static struct emc_hold_state g_hold;
static bool g_hold_ready;
static bool g_boot_done;
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

static void log_check(const char *where, uint32_t id, int rc, bool always)
{
    uint64_t before = 0, after = 0;
    uint64_t prev = g_hold.last_hz;
    enum emc_hold_result r = emc_hold_check(&g_hold, where, &before, &after);
    switch (r) {
    case EMC_HOLD_OK:
        if (always || before != prev) {
            uart_printf("[emc-hold] +%llu ms %s(%u) rc=%d: EMC %llu MHz%s\n",
                        ms_since_begin(), where, (unsigned)id, rc, mhz(before),
                        before != prev ? " (changed)" : "");
        }
        break;
    case EMC_HOLD_RESTORED:
    case EMC_HOLD_STILL_LOW:
        uart_printf("[emc-hold] +%llu ms %s(%u) rc=%d: EMC DROP %llu -> %llu MHz,"
                    " re-request -> %llu MHz (drop #%u)\n",
                    ms_since_begin(), where, (unsigned)id, rc, mhz(prev),
                    mhz(before), mhz(after), (unsigned)g_hold.drops);
        break;
    case EMC_HOLD_READ_ERROR:
        uart_printf("[emc-hold] +%llu ms %s(%u) rc=%d: EMC readback failed\n",
                    ms_since_begin(), where, (unsigned)id, rc);
        break;
    }
}

static void post_op_hook(const char *op, uint32_t id, int rc)
{
    /* After boot only changes are logged (the fan task's thermal reads
     * make this a check every 2 s). */
    log_check(op, id, rc, EMC_HOLD_BISECT != 0 && !g_boot_done);
}

void emc_hold_boot_begin(void)
{
    g_t0 = timer_get_count();
    emc_hold_init(&g_hold, &g_bpmp_ops, TEGRA234_EMC_MAX_HZ);
    uint64_t left = 0, reply = 0, readback = 0;
    int grc = bpmp_emc_get(&left);
    int src = bpmp_emc_request(TEGRA234_EMC_MAX_HZ, &reply);
    int rrc = bpmp_emc_get(&readback);
    uart_printf("[emc-hold] UEFI left EMC at %llu MHz (rc=%d); MRQ_CLK set_rate(%llu MHz)"
                " rc=%d reply %llu MHz; readback %llu MHz (rc=%d)\n",
                mhz(left), grc, mhz(TEGRA234_EMC_MAX_HZ), src, mhz(reply),
                mhz(readback), rrc);
    g_hold.last_hz = readback;
    g_hold_ready = true;
#if EMC_HOLD_BISECT
    /* Delay-only check: no other BPMP request in this window. */
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
    log_check(step, 0, 0, true);
}

void emc_hold_boot_end(void)
{
    if (!g_hold_ready) return;
#if !EMC_HOLD_BISECT
    bpmp_set_post_op_hook(NULL);
#endif
    g_boot_done = true;
    log_check("boot_end", 0, 0, true);
    uart_printf("[emc-hold] boot: %u checks, %u drops (first after %s, last after %s),"
                " %u restored, %u read errors\n",
                (unsigned)g_hold.checks, (unsigned)g_hold.drops,
                g_hold.first_drop[0] ? g_hold.first_drop : "-",
                g_hold.last_drop[0] ? g_hold.last_drop : "-",
                (unsigned)g_hold.reasserts, (unsigned)g_hold.read_errors);
}

void emc_hold_runtime_check(void)
{
    if (!g_hold_ready) return;
    uint64_t before = 0, after = 0;
    enum emc_hold_result r = emc_hold_check(&g_hold, "runtime", &before, &after);
    if (r == EMC_HOLD_RESTORED || r == EMC_HOLD_STILL_LOW) {
        WARN("EMC dropped to %llu MHz at runtime; re-requested, now %llu MHz (drop #%u)",
             mhz(before), mhz(after), (unsigned)g_hold.drops);
    }
}

void emc_hold_get_status(struct emc_hold_state *out)
{
    *out = g_hold;
}

#else /* !PLATFORM_JETSON_ORIN_NANO */

void emc_hold_boot_begin(void) {}
void emc_hold_boot_step(const char *step) { (void)step; }
void emc_hold_boot_end(void) {}
void emc_hold_runtime_check(void) {}
void emc_hold_get_status(struct emc_hold_state *out)
{
    emc_hold_init(out, NULL, 0);
}

#endif /* PLATFORM_JETSON_ORIN_NANO */
