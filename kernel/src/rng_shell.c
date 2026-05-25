/*
 * rng_shell.c - `rng` shell command.
 *
 * Subcommands:
 *
 *   rng              -> status (alias of `rng status`)
 *   rng status       -> source, TRNG state, counters
 *   rng stats        -> raw counters from rng_get_stats
 *   rng selftest     -> 4 KB pull + chi-squared sanity check
 *   rng read [N]     -> hex-dump N (default 32, max 1024) crypto bytes
 *
 * Hex output is for diagnostics — see also `#199e` (#895) which
 * runs the full NIST SP800-90B battery against the same source.
 */

#include "rng.h"
#include "shell.h"
#include "shell_internal.h"
#include "string.h"
#include "uart.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define RNG_SHELL_MAX_READ   1024u
#define RNG_SHELL_DFLT_READ  32u

static const char *bool_str(bool b) { return b ? "yes" : "no"; }

static void print_status(void)
{
    enum rng_source s = rng_active_source();
    struct rng_stats st;
    rng_get_stats(&st);

    uart_printf("rng source: %s (trng=%s)\r\n",
                rng_source_name(s), bool_str(rng_has_trng()));
    uart_printf("  bytes served: %llu\r\n",
                (unsigned long long)st.bytes_served);
    if (rng_has_trng()) {
        uart_printf("  hw calls:     %llu\r\n",
                    (unsigned long long)st.hw_calls);
        uart_printf("  hw failures:  %llu (of those, %llu fell back to jitter)\r\n",
                    (unsigned long long)st.hw_failures,
                    (unsigned long long)st.hw_degrade_events);
    }
    uart_printf("  jitter reseeds: %llu\r\n",
                (unsigned long long)st.jitter_reseeds);
}

static void print_stats(void)
{
    struct rng_stats st;
    rng_get_stats(&st);
    uart_printf("bytes_served=%llu hw_calls=%llu hw_failures=%llu "
                "hw_degrade_events=%llu jitter_reseeds=%llu\r\n",
                (unsigned long long)st.bytes_served,
                (unsigned long long)st.hw_calls,
                (unsigned long long)st.hw_failures,
                (unsigned long long)st.hw_degrade_events,
                (unsigned long long)st.jitter_reseeds);
}

static int do_selftest(void)
{
    char reason[80];
    reason[0] = '\0';

    int rc = rng_selftest(reason, sizeof(reason));
    if (rc == 0) {
        uart_printf("rng selftest: PASS (source=%s)\r\n",
                    rng_source_name(rng_active_source()));
        return 0;
    }
    uart_printf("rng selftest: FAIL (rc=%d source=%s reason=%s)\r\n",
                rc, rng_source_name(rng_active_source()), reason);
    return rc;
}

static int parse_decimal(const char *s, uint32_t *out)
{
    if (s == NULL || *s == '\0') return -1;
    uint32_t v = 0;
    while (*s != '\0') {
        if (*s < '0' || *s > '9') return -1;
        uint32_t d = (uint32_t)(*s - '0');
        if (v > (UINT32_MAX - d) / 10u) return -1;
        v = v * 10u + d;
        s++;
    }
    *out = v;
    return 0;
}

static void hex_dump(const uint8_t *buf, size_t len)
{
    static const char hex[] = "0123456789abcdef";
    for (size_t i = 0; i < len; i++) {
        char two[3];
        two[0] = hex[(buf[i] >> 4) & 0xFu];
        two[1] = hex[buf[i] & 0xFu];
        two[2] = '\0';
        uart_printf("%s", two);
        if ((i + 1u) % 32u == 0u) {
            uart_printf("\r\n");
        }
    }
    if ((len % 32u) != 0u) uart_printf("\r\n");
}

static int do_read(int argc, char **argv)
{
    uint32_t n = RNG_SHELL_DFLT_READ;
    if (argc >= 3) {
        if (parse_decimal(argv[2], &n) != 0 || n == 0u || n > RNG_SHELL_MAX_READ) {
            uart_printf("usage: rng read [N]  (1..%u; default %u)\r\n",
                        (unsigned)RNG_SHELL_MAX_READ, (unsigned)RNG_SHELL_DFLT_READ);
            return -1;
        }
    }
    /* Stack-allocated up to RNG_SHELL_MAX_READ — 1 KB on a 256 KB
     * task stack is comfortable. */
    uint8_t buf[RNG_SHELL_MAX_READ];
    if (rng_get_bytes(buf, n) != 0) {
        uart_printf("rng read: rng_get_bytes failed\r\n");
        return -1;
    }
    hex_dump(buf, n);
    /* Wipe so a transient `top` snapshot of this shell's stack doesn't
     * leak the bytes. secure_zero defeats dead-store elimination —
     * the plain `for ... buf[i] = 0u` that lived here previously was
     * removed at -O2 because `buf` is dead after this function returns. */
    secure_zero(buf, n);
    return 0;
}

static void print_usage(void)
{
    uart_printf("usage: rng [status|stats|selftest|read [N]]\r\n");
}

int cmd_rng(int argc, char **argv)
{
    if (argc < 2) {
        print_status();
        return 0;
    }

    const char *sub = argv[1];
    if (strcmp(sub, "status") == 0) {
        print_status();
        return 0;
    }
    if (strcmp(sub, "stats") == 0) {
        print_stats();
        return 0;
    }
    if (strcmp(sub, "selftest") == 0) {
        return do_selftest();
    }
    if (strcmp(sub, "read") == 0) {
        return do_read(argc, argv);
    }

    print_usage();
    return -1;
}
