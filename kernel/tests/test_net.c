/*
 * test_net.c - Networking Tests for SLM-OS
 *
 * Tests networking utility functions and API behavior.
 * Note: Hardware-dependent tests (actual packet I/O) require VirtIO
 * and are tested interactively via shell commands.
 */

#include "unity.h"

#if defined(ENABLE_NETWORKING)
#include "../include/net.h"
#include "../include/tcp_shell_server.h"
#include "../include/tcp_telemetry_server.h"
#include "../include/shell_io_tcp.h"
#include <stdint.h>
#include <stdbool.h>
#include <string.h>

/*
 * Virtqueue tests use the MMIO driver's virtqueue_add_buf/get_buf
 * implementations which are only compiled for QEMU_VIRT. The x86-64
 * PCI driver has its own virtqueue code with the same ring format.
 */
#if defined(PLATFORM_QEMU_VIRT)
#include "../include/virtio.h"
int virtqueue_add_buf(struct virtqueue *vq, void *addr, uint32_t len, bool write);
int virtqueue_get_buf(struct virtqueue *vq, uint32_t *len);
#endif

/* ============================================================================
 * IP Address Utility Tests
 * ============================================================================ */

/*
 * Test: net_ip4_addr creates correct network byte order address
 */
static void test_net_ip4_addr_basic(void)
{
    /* 10.0.2.15 in network byte order (little endian host) */
    uint32_t addr = net_ip4_addr(10, 0, 2, 15);

    /* In network byte order on little-endian: 10 is LSB */
    uint8_t *bytes = (uint8_t *)&addr;
    TEST_ASSERT_EQUAL_UINT8(10, bytes[0]);
    TEST_ASSERT_EQUAL_UINT8(0, bytes[1]);
    TEST_ASSERT_EQUAL_UINT8(2, bytes[2]);
    TEST_ASSERT_EQUAL_UINT8(15, bytes[3]);
}

/*
 * Test: net_ip4_addr handles edge cases
 */
static void test_net_ip4_addr_edge_cases(void)
{
    /* 0.0.0.0 */
    uint32_t zero = net_ip4_addr(0, 0, 0, 0);
    TEST_ASSERT_EQUAL_HEX32(0, zero);

    /* 255.255.255.255 (broadcast) */
    uint32_t bcast = net_ip4_addr(255, 255, 255, 255);
    TEST_ASSERT_EQUAL_HEX32(0xFFFFFFFF, bcast);

    /* 127.0.0.1 (localhost) */
    uint32_t localhost = net_ip4_addr(127, 0, 0, 1);
    uint8_t *bytes = (uint8_t *)&localhost;
    TEST_ASSERT_EQUAL_UINT8(127, bytes[0]);
    TEST_ASSERT_EQUAL_UINT8(0, bytes[1]);
    TEST_ASSERT_EQUAL_UINT8(0, bytes[2]);
    TEST_ASSERT_EQUAL_UINT8(1, bytes[3]);
}

/*
 * Test: net_ip_to_str converts address to string correctly
 */
static void test_net_ip_to_str_basic(void)
{
    char buf[16];
    uint32_t addr = net_ip4_addr(192, 168, 1, 100);

    char *result = net_ip_to_str(addr, buf);

    TEST_ASSERT_EQUAL_PTR(buf, result);
    TEST_ASSERT_EQUAL_STRING("192.168.1.100", buf);
}

/*
 * Test: net_ip_to_str handles edge cases
 */
static void test_net_ip_to_str_edge_cases(void)
{
    char buf[16];

    /* 0.0.0.0 */
    net_ip_to_str(net_ip4_addr(0, 0, 0, 0), buf);
    TEST_ASSERT_EQUAL_STRING("0.0.0.0", buf);

    /* 255.255.255.255 */
    net_ip_to_str(net_ip4_addr(255, 255, 255, 255), buf);
    TEST_ASSERT_EQUAL_STRING("255.255.255.255", buf);

    /* Single digits */
    net_ip_to_str(net_ip4_addr(1, 2, 3, 4), buf);
    TEST_ASSERT_EQUAL_STRING("1.2.3.4", buf);

    /* Mixed digits */
    net_ip_to_str(net_ip4_addr(10, 0, 2, 15), buf);
    TEST_ASSERT_EQUAL_STRING("10.0.2.15", buf);
}

/*
 * Test: net_str_to_ip parses valid addresses
 */
static void test_net_str_to_ip_valid(void)
{
    uint32_t addr;
    int result;

    /* Standard address */
    result = net_str_to_ip("192.168.1.100", &addr);
    TEST_ASSERT_EQUAL_INT(0, result);
    TEST_ASSERT_EQUAL_HEX32(net_ip4_addr(192, 168, 1, 100), addr);

    /* QEMU gateway */
    result = net_str_to_ip("10.0.2.2", &addr);
    TEST_ASSERT_EQUAL_INT(0, result);
    TEST_ASSERT_EQUAL_HEX32(net_ip4_addr(10, 0, 2, 2), addr);

    /* Localhost */
    result = net_str_to_ip("127.0.0.1", &addr);
    TEST_ASSERT_EQUAL_INT(0, result);
    TEST_ASSERT_EQUAL_HEX32(net_ip4_addr(127, 0, 0, 1), addr);

    /* All zeros */
    result = net_str_to_ip("0.0.0.0", &addr);
    TEST_ASSERT_EQUAL_INT(0, result);
    TEST_ASSERT_EQUAL_HEX32(0, addr);

    /* Broadcast */
    result = net_str_to_ip("255.255.255.255", &addr);
    TEST_ASSERT_EQUAL_INT(0, result);
    TEST_ASSERT_EQUAL_HEX32(0xFFFFFFFF, addr);
}

/*
 * Test: net_str_to_ip rejects invalid addresses
 */
static void test_net_str_to_ip_invalid(void)
{
    uint32_t addr;

    /* NULL inputs */
    /* All parse failures return NET_E_INVAL (#213) */
    TEST_ASSERT_EQUAL_INT(NET_E_INVAL, net_str_to_ip(NULL, &addr));
    TEST_ASSERT_EQUAL_INT(NET_E_INVAL, net_str_to_ip("1.2.3.4", NULL));

    /* Too few octets */
    TEST_ASSERT_EQUAL_INT(NET_E_INVAL, net_str_to_ip("1.2.3", &addr));
    TEST_ASSERT_EQUAL_INT(NET_E_INVAL, net_str_to_ip("1.2", &addr));
    TEST_ASSERT_EQUAL_INT(NET_E_INVAL, net_str_to_ip("1", &addr));

    /* Empty string */
    TEST_ASSERT_EQUAL_INT(NET_E_INVAL, net_str_to_ip("", &addr));

    /* Invalid characters */
    TEST_ASSERT_EQUAL_INT(NET_E_INVAL, net_str_to_ip("1.2.3.a", &addr));
    TEST_ASSERT_EQUAL_INT(NET_E_INVAL, net_str_to_ip("abc.def.ghi.jkl", &addr));

    /* Value out of range (>255) */
    TEST_ASSERT_EQUAL_INT(NET_E_INVAL, net_str_to_ip("256.1.2.3", &addr));
    TEST_ASSERT_EQUAL_INT(NET_E_INVAL, net_str_to_ip("1.2.3.999", &addr));

    /* Too many digits in octet */
    TEST_ASSERT_EQUAL_INT(NET_E_INVAL, net_str_to_ip("1.2.3.1234", &addr));
}

/*
 * Test: IP address roundtrip (str -> addr -> str)
 */
static void test_net_ip_roundtrip(void)
{
    const char *test_addrs[] = {
        "10.0.2.15",
        "192.168.100.1",
        "172.16.0.254",
        "8.8.8.8",
        "0.0.0.0",
        "255.255.255.0",
    };

    for (size_t i = 0; i < sizeof(test_addrs) / sizeof(test_addrs[0]); i++) {
        uint32_t addr;
        char buf[16];

        /* Parse the string */
        int result = net_str_to_ip(test_addrs[i], &addr);
        TEST_ASSERT_EQUAL_INT(0, result);

        /* Convert back to string */
        net_ip_to_str(addr, buf);
        TEST_ASSERT_EQUAL_STRING(test_addrs[i], buf);
    }
}

/* ============================================================================
 * Error Code Tests (#213)
 * ============================================================================ */

/*
 * Test: net_strerror returns non-NULL descriptions for every enum value
 * and a catch-all for unknown codes.
 */
static void test_net_strerror_coverage(void)
{
    /* Every named enum value must have a string that isn't "unknown" */
    TEST_ASSERT_EQUAL_STRING("ok",                       net_strerror(NET_OK));
    TEST_ASSERT_EQUAL_STRING("unspecified error",        net_strerror(NET_E_GENERIC));
    TEST_ASSERT_EQUAL_STRING("network not initialized",  net_strerror(NET_E_NOT_INIT));
    TEST_ASSERT_EQUAL_STRING("no driver registered",     net_strerror(NET_E_NO_DRIVER));
    TEST_ASSERT_EQUAL_STRING("device not found",        net_strerror(NET_E_NO_DEVICE));
    TEST_ASSERT_EQUAL_STRING("out of memory",            net_strerror(NET_E_NO_MEM));
    TEST_ASSERT_EQUAL_STRING("busy",                     net_strerror(NET_E_BUSY));
    TEST_ASSERT_EQUAL_STRING("timeout",                  net_strerror(NET_E_TIMEOUT));
    TEST_ASSERT_EQUAL_STRING("invalid argument",         net_strerror(NET_E_INVAL));
    TEST_ASSERT_EQUAL_STRING("packet too large",         net_strerror(NET_E_TOO_LARGE));
    TEST_ASSERT_EQUAL_STRING("link down",                net_strerror(NET_E_LINK_DOWN));
    TEST_ASSERT_EQUAL_STRING("protocol error",           net_strerror(NET_E_PROTO));

    /* Unknown codes fall through to the catch-all */
    TEST_ASSERT_EQUAL_STRING("unknown", net_strerror(-999));
    TEST_ASSERT_EQUAL_STRING("unknown", net_strerror(42));
}

/*
 * Test: the existing error contract — every NET_E_* is negative, NET_OK
 * is zero — holds so that callers using `if (rc < 0)` still work.
 */
static void test_net_error_codes_are_negative(void)
{
    TEST_ASSERT_EQUAL_INT(0, NET_OK);
    TEST_ASSERT_TRUE(NET_E_GENERIC     < 0);
    TEST_ASSERT_TRUE(NET_E_NOT_INIT    < 0);
    TEST_ASSERT_TRUE(NET_E_NO_DRIVER   < 0);
    TEST_ASSERT_TRUE(NET_E_NO_DEVICE   < 0);
    TEST_ASSERT_TRUE(NET_E_NO_MEM      < 0);
    TEST_ASSERT_TRUE(NET_E_BUSY        < 0);
    TEST_ASSERT_TRUE(NET_E_TIMEOUT     < 0);
    TEST_ASSERT_TRUE(NET_E_INVAL       < 0);
    TEST_ASSERT_TRUE(NET_E_TOO_LARGE   < 0);
    TEST_ASSERT_TRUE(NET_E_LINK_DOWN   < 0);
    TEST_ASSERT_TRUE(NET_E_PROTO       < 0);
}

/* ============================================================================
 * Network State Tests
 * ============================================================================ */

/*
 * Test: net_is_up is callable without crash
 *
 * Smoke test: net_is_up() should return a valid boolean without faulting.
 * The actual value depends on whether a prior test already initialized
 * networking, so the return value is not asserted.
 */
static void test_net_is_up_before_init(void)
{
    bool is_up = net_is_up();
    (void)is_up;
    TEST_IGNORE_MESSAGE("smoke test: network state depends on test order");
}

/*
 * Test: net_get_info fails gracefully when network not initialized
 */
static void test_net_get_info_not_initialized(void)
{
    if (net_is_up()) {
        /* Network is already up, so get_info should work */
        struct net_info info;
        int result = net_get_info(&info);
        TEST_ASSERT_EQUAL_INT(NET_OK, result);
    } else {
        /* Network not initialized - should return NET_E_NOT_INIT (#213) */
        struct net_info info;
        int result = net_get_info(&info);
        TEST_ASSERT_EQUAL_INT(NET_E_NOT_INIT, result);
    }
}

/*
 * Test: net_get_info rejects NULL pointer
 */
static void test_net_get_info_null_pointer(void)
{
    /* NULL check runs before the init check (#213) */
    int result = net_get_info(NULL);
    TEST_ASSERT_EQUAL_INT(NET_E_INVAL, result);
}

/*
 * Test: Network commands fail gracefully when not initialized
 */
static void test_net_commands_without_init(void)
{
    if (!net_is_up()) {
        /* All three should return NET_E_NOT_INIT (#213) */
        int result = net_ping(net_ip4_addr(10, 0, 2, 2), 1, NULL, NULL);
        TEST_ASSERT_EQUAL_INT(NET_E_NOT_INIT, result);

        result = net_set_static_ip(
            net_ip4_addr(10, 0, 2, 15),
            net_ip4_addr(255, 255, 255, 0),
            net_ip4_addr(10, 0, 2, 2)
        );
        TEST_ASSERT_EQUAL_INT(NET_E_NOT_INIT, result);

        result = net_enable_dhcp();
        TEST_ASSERT_EQUAL_INT(NET_E_NOT_INIT, result);
    } else {
        /* Network is up - skip this test */
        TEST_PASS();
    }
}

/* ============================================================================
 * Statistics Tests
 * ============================================================================ */

/*
 * Test: net_get_stats returns sane values and handles NULL safely
 */
static void test_net_get_stats_safety(void)
{
    struct net_stats stats;

    /* Should not crash with valid pointer */
    net_get_stats(&stats);

    /* Sanity: error counts should never exceed packet counts */
    TEST_ASSERT_TRUE(stats.rx_errors <= stats.rx_packets);
    TEST_ASSERT_TRUE(stats.tx_errors <= stats.tx_packets);

    /* Sanity: dropped packets should never exceed received packets */
    TEST_ASSERT_TRUE(stats.rx_dropped <= stats.rx_packets);

    /* rx_no_buffers should be zero in steady state. Bounded sanity
     * check (non-negative is implicit in unsigned) + upper bound to
     * catch runaway counters from a buggy driver re-post path. */
    TEST_ASSERT_TRUE(stats.rx_no_buffers < 1000000);

    /* Should not crash with NULL (just doesn't write) */
    net_get_stats(NULL);
}

/*
 * Test: rx_no_buffers counter is zero under normal init + idle poll.
 *
 * Guards against a regression where the re-post path in recv()
 * starts silently leaking descriptors (which would eventually force
 * net_stats_rx_no_buffers_inc() to fire when the pool is drained).
 * Run this after the live DHCP + ping test_driver_tx so the RX path
 * has seen real traffic.
 */
static void test_net_rx_no_buffers_clean(void)
{
    if (!net_is_up()) {
        TEST_IGNORE_MESSAGE("network not initialized");
        return;
    }

    struct net_stats stats;
    net_get_stats(&stats);
    TEST_ASSERT_MESSAGE(stats.rx_no_buffers == 0,
        "rx_no_buffers should be 0 in steady state — driver leaked descriptors?");
}

/*
 * Test: RX-stall watchdog snapshot is NULL-safe.
 *
 * Mirrors test_net_get_stats_safety. A telemetry feed that wraps
 * net_watchdog_get must not crash on a stale / NULL output buffer.
 */
static void test_net_watchdog_get_null_safe(void)
{
    /* Must not crash. There's no return value to check. */
    net_watchdog_get(NULL);
}

/*
 * Test: RX-stall watchdog reports a sane initial state.
 *
 * Before any RX has happened, the watchdog is disarmed (armed=false,
 * alarmed=false), the threshold reads back as the configured value,
 * and pool/heap fields fall inside lwIP's static maxima.
 */
static void test_net_watchdog_initial_state(void)
{
    struct net_watchdog_snapshot snap;
    net_watchdog_get(&snap);

    TEST_ASSERT_MESSAGE(!snap.alarmed,
        "watchdog should not be alarmed at boot");

    TEST_ASSERT_TRUE(snap.stall_threshold_ms >= 100u);
    TEST_ASSERT_TRUE(snap.stall_threshold_ms <= 60u * 1000u);

    /* used <= avail in both cases. Pre-`net_init` lwIP hasn't run
     * memp_init yet so the pointers are NULL and the snapshot reads
     * 0/0; post-init avail mirrors PBUF_POOL_SIZE / MEMP_NUM_TCP_PCB.
     * Either way the invariant holds. */
    TEST_ASSERT_TRUE(snap.pbuf_pool_used <= snap.pbuf_pool_avail);
    TEST_ASSERT_TRUE(snap.tcp_pcb_used <= snap.tcp_pcb_avail);
}

/*
 * Test: net_watchdog_set_threshold_ms clamps and restores correctly.
 *
 * Pinning the runtime knob: 0 -> default, sub-100 ms -> floor, larger
 * values pass through. Restores the default before returning so it
 * doesn't perturb subsequent tests.
 */
static void test_net_watchdog_threshold_clamps(void)
{
    struct net_watchdog_snapshot snap;

    /* Capture default for restoration at end. */
    net_watchdog_get(&snap);
    uint32_t default_ms = snap.stall_threshold_ms;

    /* 0 -> restore default. */
    net_watchdog_set_threshold_ms(0);
    net_watchdog_get(&snap);
    TEST_ASSERT_EQUAL_UINT32(default_ms, snap.stall_threshold_ms);

    /* Small values are clamped to 100 ms minimum. */
    net_watchdog_set_threshold_ms(1);
    net_watchdog_get(&snap);
    TEST_ASSERT_EQUAL_UINT32(100u, snap.stall_threshold_ms);

    /* Large values pass through. */
    net_watchdog_set_threshold_ms(30000u);
    net_watchdog_get(&snap);
    TEST_ASSERT_EQUAL_UINT32(30000u, snap.stall_threshold_ms);

    /* Restore. */
    net_watchdog_set_threshold_ms(0);
}

/*
 * Test: tcp_shell_server_get_stats is NULL-safe.
 *
 * Mirrors test_net_watchdog_get_null_safe. A telemetry feed that
 * wraps the shell-tcp stats getter must not crash on a stale or
 * NULL output buffer.
 */
static void test_tcp_shell_server_get_stats_null_safe(void)
{
    /* Must not crash. There's no return value to check. */
    tcp_shell_server_get_stats(NULL);
}

/*
 * Test: shell-tcp stats report sane values without any sessions.
 *
 * Order-independent: this test runs in whatever sequence the suite
 * runner picks, and the static counters in tcp_shell_server.c
 * persist across tests. The invariants checked here hold for any
 * non-corrupted state — `closed <= opened`, `active == opened - closed`,
 * `peak_active >= active`, and the leak counters are monotonic.
 */
static void test_tcp_shell_server_stats_invariants(void)
{
    struct tcp_shell_server_stats s;
    tcp_shell_server_get_stats(&s);

    TEST_ASSERT_MESSAGE(s.sessions_closed <= s.sessions_opened,
        "closed must never exceed opened");
    TEST_ASSERT_MESSAGE(s.active == s.sessions_opened - s.sessions_closed,
        "active must equal opened - closed");
    TEST_ASSERT_MESSAGE(s.peak_active >= s.active,
        "peak_active must be >= current active");
    /* leak_warnings counts events; each event accumulates a positive
     * delta into total_suspicious_leak_bytes. So total >= warnings *
     * threshold (1024 in default builds). Sanity-check that total is
     * at least 1024 * warnings, allowing for the weakest-warning case. */
    TEST_ASSERT_MESSAGE(
        s.total_suspicious_leak_bytes >= s.leak_warnings * 1024u,
        "total_suspicious_leak_bytes must be >= warnings * threshold");
    /* max delta must be >= last delta when last is positive (max only
     * tracks positive deltas via the same > comparison). */
    if (s.last_session_heap_delta_bytes > 0) {
        TEST_ASSERT_MESSAGE(
            s.max_session_heap_delta_bytes >= s.last_session_heap_delta_bytes,
            "max delta must be >= last positive delta");
    }
}

/*
 * Test: shell_io_tcp_poll defers freeing the slot until the
 * post-close settle window has elapsed (#537).
 *
 * The previous behaviour measured the lwIP heap delta in the same
 * poll cycle as tcp_close, before unacked TCP_WRITE_FLAG_COPY pbufs
 * had a chance to be ACKed and freed — that produced multi-KB false-
 * positive "leak" warnings (#442 closure was incomplete). The fix
 * holds the slot in a "settling" state for TCP_SHELL_CLOSE_SETTLE_MS
 * before measuring + freeing.
 */
static void test_shell_io_tcp_close_settling(void)
{
    int rc = shell_io_tcp_test_run_close_settling();
    TEST_ASSERT_MESSAGE(rc != -1,
        "test_run_close_settling: pool slot allocation failed");
    TEST_ASSERT_MESSAGE(rc != -2,
        "test_run_close_settling: poll freed slot before settle window elapsed");
    TEST_ASSERT_MESSAGE(rc != -3,
        "test_run_close_settling: poll failed to free slot after settle window");
    TEST_ASSERT_EQUAL_INT(0, rc);
}

/*
 * Test: tcp_read_buf drains the rx ring in a single call (#597).
 *
 * Pre-fix, the line-edit loop in shell_read_command read one byte
 * per `read_char` call, costing one spin_lock_irqsave + IRQ-disable
 * cycle per byte. A 32 KB hex `xput chunk` line ate ~32K cycles
 * before any actual processing happened — the dominant cost in
 * #597's per-chunk breakdown. The new tcp_read_buf vtable method
 * drains everything in the ring in ONE lock cycle, so the line-
 * edit loop's per-char work happens against a local prefetch
 * buffer instead of the locked ring.
 *
 * The helper primes the ring with a 58-byte pattern and asserts
 * the read drains all 58 in a single tcp_read_buf call (vs. 58
 * tcp_read_char calls). A regression that re-introduces the per-
 * char overhead would either return 1 (per-char regression) or
 * fail content match (ring-walk bug).
 */
static void test_shell_io_tcp_read_buf_drains_ring(void)
{
    int rc = shell_io_tcp_test_run_read_buf_drains_ring();
    TEST_ASSERT_MESSAGE(rc != -1,
        "test_run_read_buf_drains_ring: pool slot allocation failed");
    TEST_ASSERT_MESSAGE(rc != -2,
        "test_run_read_buf_drains_ring: tcp_read_buf returned wrong "
        "byte count or content");
    TEST_ASSERT_EQUAL_INT(0, rc);
}

/*
 * Test: tcp_read_buf correctly handles a payload that spans the
 * ring-wrap boundary (#597 review follow-up).
 *
 * Pre-tcp_read_buf, multi-byte reads that crossed the wrap point
 * weren't possible (per-char reads always touched one slot at a
 * time). The new helper's two-step copy + the `(rx_tail + want) &
 * MASK` advance are both new code that this test exercises directly.
 * A bug in either copy length or the wrap-around tail update would
 * silently corrupt the assembled buffer or leave rx_tail at the
 * wrong position for the next read.
 */
static void test_shell_io_tcp_read_buf_handles_wrap(void)
{
    int rc = shell_io_tcp_test_run_read_buf_wrap();
    TEST_ASSERT_MESSAGE(rc != -1,
        "test_run_read_buf_wrap: pool slot allocation failed");
    TEST_ASSERT_MESSAGE(rc != -2,
        "test_run_read_buf_wrap: tcp_read_buf wrap path returned wrong "
        "byte count, content, or post-read rx_tail position");
    TEST_ASSERT_EQUAL_INT(0, rc);
}

/*
 * Test: 50-cycle close-path drive must not increment leak_warnings (#537).
 *
 * Field observation summary (2026-05-02 jetson-nano-2 traces): post the
 * deferred-measurement settle work, every clean session close in 16+
 * sequential disconnect cycles registered as a *negative* heap delta
 * (returned more memory than it took at open-time), and `leaks:
 * warnings=0 total=0 bytes` held throughout. The pre-fix late-April
 * state had ~2 of 13 sessions tripping +1300-2000 byte warnings.
 *
 * This test pins that "no false-positive leaks" property into CI by
 * driving `shell_io_tcp_test_run_clean_close_cycle` 50 times back-to-
 * back. Each cycle:
 *   - allocates a pool slot
 *   - snapshots the lwIP heap baseline + per-MEMP-pool baseline
 *   - rewinds the close-settle timer past the 1500 ms window
 *   - drives `shell_io_tcp_poll`, which measures the close-time delta
 *     and routes through `tcp_shell_server_note_session_close`
 *
 * If the close-path bookkeeping ever regresses to the pre-fix state
 * (false-positive leak warnings on clean closes), this test fires
 * immediately rather than surfacing in a 1 GB hardware upload trace.
 *
 * Coverage caveat: cycles never call `tcp_write` against a real pcb,
 * so this test cannot detect leaks in lwIP-side allocation paths
 * (those are caught only by the slm-put.py-driven hardware exerciser
 * in #537's plan). It catches the kernel-side accounting, which is
 * where the late-April leak lived.
 */
static void test_tcp_shell_no_false_leak_over_50_close_cycles(void)
{
    struct tcp_shell_server_stats before;
    tcp_shell_server_get_stats(&before);

    for (int i = 0; i < 50; i++) {
        int rc = shell_io_tcp_test_run_clean_close_cycle();
        TEST_ASSERT_MESSAGE(rc == 0,
            "clean-close cycle must succeed (-1=alloc fail, -2=slot held)");
    }

    struct tcp_shell_server_stats after;
    tcp_shell_server_get_stats(&after);

    /* Each cycle calls both `note_session_open` and (via poll)
     * `note_session_close`, so the open/close counters advance in
     * lock-step. Pairing matters because the `stats_invariants`
     * test asserts `closed <= opened`; an unmatched close would
     * underflow `active = opened - closed` on subsequent reads. */
    TEST_ASSERT_EQUAL_UINT(before.sessions_opened + 50,
                           after.sessions_opened);
    TEST_ASSERT_EQUAL_UINT(before.sessions_closed + 50,
                           after.sessions_closed);

    /* The load-bearing assertion: zero false-positive leak warnings.
     * Pre-deferred-measurement, each cycle would have tripped a
     * warning if `lwip_stats.mem.used` exceeded baseline + 1024 B at
     * measurement time. With the baseline-snapshot fix, delta should
     * round to zero (or slightly negative if other work freed memory
     * mid-test) and leak_warnings stays put. */
    TEST_ASSERT_MESSAGE(after.leak_warnings == before.leak_warnings,
        "50 clean closes must not trip any leak warnings — "
        "regression of the deferred-measurement fix");
}

/*
 * Test: tcp_write_buf bails within the configured wall-clock cap when
 * the drain is wedged (#536).
 *
 * Drives shell_io_tcp_test_run_write_timeout with a 100 ms override
 * so the test doesn't have to wait the production 2 s. The helper
 * allocates a pool slot, primes a tcp_shell_ctx with no pcb (no
 * net_pump drain will fire), pushes more bytes than the ring can
 * hold, and asserts that tcp_write_buf returned with the session
 * marked degraded + closed and elapsed time inside [override, +1 s].
 */
static void test_shell_io_tcp_write_buf_timeout(void)
{
    int rc = shell_io_tcp_test_run_write_timeout(100);
    TEST_ASSERT_MESSAGE(rc != -1,
        "test_run_write_timeout: pool slot allocation failed");
    TEST_ASSERT_MESSAGE(rc == 0,
        "test_run_write_timeout: tcp_write_buf did not respect the cap");
}

/*
 * Test: note_session_open + note_session_close move the counters as
 * advertised. Captures stats before, calls open + close with a known
 * delta, captures stats after, asserts deltas. Order-independent.
 *
 * Uses a delta below the leak threshold (256 < 1024) to avoid
 * tripping the WARN — we don't want test runs to spam serial.
 */
static void test_tcp_shell_server_note_session_pair(void)
{
    struct tcp_shell_server_stats before;
    tcp_shell_server_get_stats(&before);

    tcp_shell_server_note_session_open(0xDEADBEEFu);
    tcp_shell_server_note_session_close(0xDEADBEEFu, 256, NULL, NULL);

    struct tcp_shell_server_stats after;
    tcp_shell_server_get_stats(&after);

    TEST_ASSERT_MESSAGE(after.sessions_opened == before.sessions_opened + 1,
        "open hook must increment sessions_opened");
    TEST_ASSERT_MESSAGE(after.sessions_closed == before.sessions_closed + 1,
        "close hook must increment sessions_closed");
    /* active should be unchanged after the matched pair. */
    TEST_ASSERT_MESSAGE(after.active == before.active,
        "active should be unchanged after open+close pair");
    /* last_session_heap_delta should reflect our 256 input. */
    TEST_ASSERT_MESSAGE(after.last_session_heap_delta_bytes == 256,
        "last delta should be the value passed to note_session_close");
    /* No leak warning at delta=256 (below 1024 threshold). */
    TEST_ASSERT_MESSAGE(after.leak_warnings == before.leak_warnings,
        "leak_warnings must not trip at delta below threshold");
}

/*
 * Test: Statistics start at zero
 */
static void test_net_stats_initial_values(void)
{
    struct net_stats stats;
    net_get_stats(&stats);

    /* If network not used, all stats should be zero */
    /* We can't guarantee this if network was used, so just verify reasonable values */
    TEST_ASSERT_TRUE(stats.rx_packets <= 1000000);  /* Sanity check */
    TEST_ASSERT_TRUE(stats.tx_packets <= 1000000);
    TEST_ASSERT_TRUE(stats.rx_errors <= stats.rx_packets);
    TEST_ASSERT_TRUE(stats.tx_errors <= stats.tx_packets);
}

/* ============================================================================
 * Virtqueue Descriptor Ring Tests (DRV-H2)
 *
 * These exercise virtqueue_add_buf / virtqueue_get_buf against a synthetic
 * virtqueue built from static storage (no MMIO, no device). The goal is to
 * catch regressions in descriptor-ring bookkeeping, wrap-around handling,
 * and the cache-maintenance calls added for DRV-H2. On QEMU ARM64 the
 * cache helpers resolve to a dmb, so these tests also verify that the
 * barrier calls do not corrupt the ring state.
 *
 * These tests link against the MMIO driver's virtqueue functions, so they
 * are only compiled for PLATFORM_QEMU_VIRT.
 * ============================================================================ */

#if defined(PLATFORM_QEMU_VIRT)

#define TVQ_SIZE 16

static struct virtq_desc tvq_desc[TVQ_SIZE];

/*
 * avail/used storage is sized for TVQ_SIZE entries plus the flags/idx header.
 * The structs use flexible array members so we back them with a byte buffer.
 */
static uint8_t tvq_avail_storage[4 + TVQ_SIZE * sizeof(uint16_t) + 2];
static uint8_t tvq_used_storage[4 + TVQ_SIZE * sizeof(struct virtq_used_elem) + 2];

static uint8_t tvq_payload_a[64];
static uint8_t tvq_payload_b[64];

/* Build a synthetic virtqueue that looks like what virtqueue_init produces. */
static void tvq_reset(struct virtqueue *vq)
{
    memset(tvq_desc, 0, sizeof(tvq_desc));
    memset(tvq_avail_storage, 0, sizeof(tvq_avail_storage));
    memset(tvq_used_storage, 0, sizeof(tvq_used_storage));

    vq->index = 0;
    vq->size = TVQ_SIZE;
    vq->regs = NULL;          /* No kick in these tests. */
    vq->desc = tvq_desc;
    vq->avail = (struct virtq_avail *)tvq_avail_storage;
    vq->used = (struct virtq_used *)tvq_used_storage;

    vq->free_head = 0;
    vq->num_free = TVQ_SIZE;
    vq->last_used_idx = 0;
    for (uint16_t i = 0; i < TVQ_SIZE - 1; i++) {
        vq->desc[i].next = i + 1;
    }
}

/*
 * Test: add_buf populates the descriptor, inserts the index into the avail
 * ring, advances avail->idx, and returns the allocated descriptor index.
 */
static void test_virtqueue_add_buf_basic(void)
{
    struct virtqueue vq;
    tvq_reset(&vq);

    int idx = virtqueue_add_buf(&vq, tvq_payload_a, sizeof(tvq_payload_a),
                                true /* device writes */);

    TEST_ASSERT_EQUAL_INT(0, idx);
    TEST_ASSERT_EQUAL_UINT64((uintptr_t)tvq_payload_a, vq.desc[0].addr);
    TEST_ASSERT_EQUAL_UINT32(sizeof(tvq_payload_a), vq.desc[0].len);
    TEST_ASSERT_EQUAL_UINT16(VIRTQ_DESC_F_WRITE, vq.desc[0].flags);
    TEST_ASSERT_EQUAL_UINT16(0, vq.avail->ring[0]);
    TEST_ASSERT_EQUAL_UINT16(1, vq.avail->idx);
    TEST_ASSERT_EQUAL_UINT16(TVQ_SIZE - 1, vq.num_free);
}

/*
 * Test: add_buf with write=false sets no flags (driver-owned read buffer).
 */
static void test_virtqueue_add_buf_read_only(void)
{
    struct virtqueue vq;
    tvq_reset(&vq);

    int idx = virtqueue_add_buf(&vq, tvq_payload_a, 32, false);

    TEST_ASSERT_EQUAL_INT(0, idx);
    TEST_ASSERT_EQUAL_UINT16(0, vq.desc[0].flags);
}

/*
 * Test: add_buf fills the ring, then returns -1 when descriptors exhausted.
 */
static void test_virtqueue_add_buf_exhaustion(void)
{
    struct virtqueue vq;
    tvq_reset(&vq);

    for (int i = 0; i < TVQ_SIZE; i++) {
        int idx = virtqueue_add_buf(&vq, tvq_payload_a, 16, true);
        TEST_ASSERT_TRUE(idx >= 0);
    }

    /* Next add must fail — no free descriptors. */
    int idx = virtqueue_add_buf(&vq, tvq_payload_a, 16, true);
    TEST_ASSERT_EQUAL_INT(-1, idx);
    TEST_ASSERT_EQUAL_UINT16(0, vq.num_free);
    TEST_ASSERT_EQUAL_UINT16(TVQ_SIZE, vq.avail->idx);
}

/*
 * Test: avail->idx counts monotonically past vq.size without wrapping
 * (wrapping happens at the ring slot, not at avail->idx itself).
 */
static void test_virtqueue_avail_idx_beyond_size(void)
{
    struct virtqueue vq;
    tvq_reset(&vq);

    /* Add one, free one, repeat — avail->idx grows past TVQ_SIZE. */
    for (int i = 0; i < TVQ_SIZE + 5; i++) {
        int idx = virtqueue_add_buf(&vq, tvq_payload_a, 16, true);
        TEST_ASSERT_TRUE(idx >= 0);

        /* Simulate the device returning this descriptor via the used ring. */
        uint16_t used_pos = (uint16_t)(i % TVQ_SIZE);
        vq.used->ring[used_pos].id = (uint32_t)idx;
        vq.used->ring[used_pos].len = 16;
        vq.used->idx++;

        uint32_t rx_len = 0;
        int got = virtqueue_get_buf(&vq, &rx_len);
        TEST_ASSERT_EQUAL_INT(idx, got);
        TEST_ASSERT_EQUAL_UINT32(16, rx_len);
    }

    /* avail->idx should keep climbing; ring slot is the modular index. */
    TEST_ASSERT_EQUAL_UINT16(TVQ_SIZE + 5, vq.avail->idx);
    TEST_ASSERT_EQUAL_UINT16(TVQ_SIZE, vq.num_free);
}

/*
 * Test: get_buf returns -1 when used->idx has not advanced.
 */
static void test_virtqueue_get_buf_empty(void)
{
    struct virtqueue vq;
    tvq_reset(&vq);

    uint32_t len = 0xDEADBEEF;
    int ret = virtqueue_get_buf(&vq, &len);
    TEST_ASSERT_EQUAL_INT(-1, ret);
    TEST_ASSERT_EQUAL_UINT32(0xDEADBEEF, len);  /* Must not clobber */
}

/*
 * Test: get_buf reads the used-ring entry the device wrote and returns
 * its descriptor index and length.
 */
static void test_virtqueue_get_buf_returns_device_len(void)
{
    struct virtqueue vq;
    tvq_reset(&vq);

    int desc_idx = virtqueue_add_buf(&vq, tvq_payload_a, 64, true);
    TEST_ASSERT_TRUE(desc_idx >= 0);

    /* Device writes 42 bytes and publishes the used entry. */
    vq.used->ring[0].id = (uint32_t)desc_idx;
    vq.used->ring[0].len = 42;
    vq.used->idx = 1;

    uint32_t rx_len = 0;
    int got = virtqueue_get_buf(&vq, &rx_len);
    TEST_ASSERT_EQUAL_INT(desc_idx, got);
    TEST_ASSERT_EQUAL_UINT32(42, rx_len);
    TEST_ASSERT_EQUAL_UINT16(TVQ_SIZE, vq.num_free);  /* Descriptor freed. */
}

/*
 * Test: Two concurrent in-flight buffers — add_buf increments avail->idx
 * to 2, each has a unique descriptor slot.
 */
static void test_virtqueue_add_two_distinct_buffers(void)
{
    struct virtqueue vq;
    tvq_reset(&vq);

    int idx_a = virtqueue_add_buf(&vq, tvq_payload_a, 16, true);
    int idx_b = virtqueue_add_buf(&vq, tvq_payload_b, 32, true);

    TEST_ASSERT_TRUE(idx_a >= 0);
    TEST_ASSERT_TRUE(idx_b >= 0);
    TEST_ASSERT_NOT_EQUAL(idx_a, idx_b);
    TEST_ASSERT_EQUAL_UINT64((uintptr_t)tvq_payload_a, vq.desc[idx_a].addr);
    TEST_ASSERT_EQUAL_UINT64((uintptr_t)tvq_payload_b, vq.desc[idx_b].addr);
    TEST_ASSERT_EQUAL_UINT32(16, vq.desc[idx_a].len);
    TEST_ASSERT_EQUAL_UINT32(32, vq.desc[idx_b].len);
    TEST_ASSERT_EQUAL_UINT16(2, vq.avail->idx);
    TEST_ASSERT_EQUAL_UINT16(TVQ_SIZE - 2, vq.num_free);
}

#endif /* PLATFORM_QEMU_VIRT — virtqueue tests */

/* ============================================================================
 * Live Driver Integration Tests
 *
 * These tests exercise the full net_init() path against the live VirtIO-Net
 * device QEMU exposes. They are the no-hardware equivalent of running
 * `net init && ifconfig && ping 10.0.2.2` in the shell — they prove that
 * the registered driver, the lwIP netif adapter, and the configured QEMU
 * netdev all line up. Skipped automatically when no network device is
 * present (e.g. someone running the test kernel in QEMU with -nic none).
 * ============================================================================ */

#include "net_driver.h"
#include "arch/sys_arch.h"  /* sys_now() for DHCP timeout polling */
#include "lwip/stats.h"     /* MEMP_PBUF_POOL peak tracking for #581 regression test */
extern void net_test_force_boot_deferred_dhcp(void);
extern void net_test_force_dhcp_start_fail(void);
#if defined(PLATFORM_QEMU_VIRT)
#include "../include/virtio_net.h"  /* virtio_net_get_irq_count (ARM64 MMIO) */
#include "../include/virtio.h"      /* VIRTIO_DEVICE_IRQ */
#include "../include/gic.h"         /* gic_lookup_handler */
#endif

#if defined(PLATFORM_X86_64)
#include "../include/virtio_net_pci.h"  /* accessors + handler */
#endif

static const struct net_driver *link_test_base_driver;
static bool link_test_force_up;

static int link_test_driver_init(void)
{
    if (link_test_base_driver && link_test_base_driver->init)
        return link_test_base_driver->init();
    return NET_OK;
}

static int link_test_driver_send(const void *buf, size_t len)
{
    return link_test_base_driver->send(buf, len);
}

static int link_test_driver_recv(void *buf, size_t max_len)
{
    return link_test_base_driver->recv(buf, max_len);
}

static void link_test_driver_get_mac(uint8_t mac[6])
{
    link_test_base_driver->get_mac(mac);
}

static bool link_test_driver_link_status(void)
{
    return link_test_force_up;
}

static void link_test_driver_tx_reap(void)
{
    if (link_test_base_driver->tx_reap)
        link_test_base_driver->tx_reap();
}

static const struct net_driver link_test_driver = {
    .name = "test-link-wrapper",
    .init = link_test_driver_init,
    .send = link_test_driver_send,
    .recv = link_test_driver_recv,
    .get_mac = link_test_driver_get_mac,
    .link_status = link_test_driver_link_status,
    .tx_reap = link_test_driver_tx_reap,
};

static void link_test_install(bool link_up)
{
    link_test_base_driver = net_get_driver();
    TEST_ASSERT_NOT_NULL(link_test_base_driver);
    link_test_force_up = link_up;
    net_register_driver(&link_test_driver);
}

static void link_test_set(bool link_up)
{
    link_test_force_up = link_up;
}

static void link_test_restore(void)
{
    TEST_ASSERT_NOT_NULL(link_test_base_driver);
    net_register_driver(link_test_base_driver);
    link_test_base_driver = NULL;
}

/*
 * Test: a network driver was registered during platform init.
 *
 * Verifies the platform-init path in main.c calls
 * virtio_net_register() (QEMU_VIRT) or virtio_net_pci_register() (X86_64).
 */
static void test_net_driver_registered(void)
{
    const struct net_driver *drv = net_get_driver();
    TEST_ASSERT_NOT_NULL(drv);
    TEST_ASSERT_NOT_NULL(drv->name);
    TEST_ASSERT_NOT_NULL(drv->init);
    TEST_ASSERT_NOT_NULL(drv->send);
    TEST_ASSERT_NOT_NULL(drv->recv);
    TEST_ASSERT_NOT_NULL(drv->get_mac);
    TEST_ASSERT_NOT_NULL(drv->link_status);
}

/*
 * Test: net_init() brings the driver up and configures the netif.
 *
 * On QEMU with virtio-net attached, this should succeed: the driver
 * probes the device, negotiates features, sets up virtqueues, and the
 * lwIP netif comes up with the default 10.0.2.15 address.
 *
 * If the device is missing (no -netdev / -device on the QEMU command
 * line) the driver init returns -1 and the test skips rather than failing —
 * this lets the test kernel run in environments without networking.
 */
static void test_net_init_live(void)
{
    if (net_is_up()) {
        TEST_PASS();  /* Already initialized by a previous test run */
        return;
    }

    int ret = net_init();
    if (ret != 0) {
        TEST_IGNORE_MESSAGE("VirtIO-Net device not present — skipping live test");
        return;
    }

    TEST_ASSERT_TRUE(net_is_up());

    struct net_info info;
    TEST_ASSERT_EQUAL_INT(0, net_get_info(&info));

    /* MAC address must be non-zero (driver should have read it from
     * the device's config space, or fallen back to a locally-administered
     * address). */
    bool any_nonzero = false;
    for (int i = 0; i < 6; i++) {
        if (info.mac[i] != 0) { any_nonzero = true; break; }
    }
    TEST_ASSERT_MESSAGE(any_nonzero, "MAC address should be non-zero after init");

    /* Default IP should be QEMU's 10.0.2.15 (set by net_init before DHCP) */
    TEST_ASSERT_EQUAL_HEX32(net_ip4_addr(10, 0, 2, 15), info.ip_addr);

    /* Link should be up since QEMU emulates an always-connected link */
    TEST_ASSERT_TRUE(info.link_up);
}

/*
 * Test: net_poll() runs without crashing after init.
 *
 * Polls the registered driver's recv path and lwIP timers. With no
 * traffic on the wire there should be no packets, but the call must
 * not fault — this catches NULL-deref bugs in the receive loop, lwIP
 * timer callbacks, and the netif input chain.
 */
static void test_net_poll_after_init(void)
{
    if (!net_is_up()) {
        TEST_IGNORE_MESSAGE("network not initialized");
        return;
    }

    /* Poll a few times — exercises virtqueue empty path + lwIP timers */
    for (int i = 0; i < 16; i++) {
        net_poll();
    }
    TEST_PASS();
}

/*
 * Test: auto-DHCP starts during net_init when NET_DHCP_AT_BOOT is set
 * (issue #197).
 *
 * After net_init(), the dhcp_enabled flag should be true and
 * dhcp_status should be PENDING (not enough poll iterations yet for
 * a bind) or BOUND (if QEMU's SLIRP answered the DISCOVER immediately,
 * which it often does). If the flag is OFF at build time, status is
 * DISABLED and dhcp_enabled is false.
 */
static void test_net_auto_dhcp_at_boot(void)
{
    if (!net_is_up()) {
        TEST_IGNORE_MESSAGE("network not initialized");
        return;
    }

    struct net_info info;
    TEST_ASSERT_EQUAL_INT(0, net_get_info(&info));

#if defined(NET_DHCP_AT_BOOT)
    TEST_ASSERT_MESSAGE(info.dhcp_enabled,
        "NET_DHCP_AT_BOOT=ON: dhcp_enabled should be true after net_init");
    TEST_ASSERT_MESSAGE(
        info.dhcp_status == NET_DHCP_PENDING ||
        info.dhcp_status == NET_DHCP_BOUND,
        "NET_DHCP_AT_BOOT=ON: dhcp_status should be PENDING or BOUND");
#else
    TEST_ASSERT_MESSAGE(!info.dhcp_enabled,
        "NET_DHCP_AT_BOOT=OFF: dhcp_enabled should be false");
    TEST_ASSERT_EQUAL_INT(NET_DHCP_DISABLED, info.dhcp_status);
#endif
}

/*
 * Test: DHCP binds an address under QEMU SLIRP.
 *
 * QEMU user-mode networking includes a built-in DHCP server at
 * 10.0.2.2 that hands out 10.0.2.15 by default. After enough poll
 * iterations to complete the DISCOVER/OFFER/REQUEST/ACK handshake,
 * dhcp_status should transition from PENDING to BOUND. We poll for
 * up to 2 seconds (net_poll drives lwIP timers and the driver recv).
 *
 * Skipped if DHCP wasn't auto-started at boot.
 */
static void test_net_dhcp_binds(void)
{
    if (!net_is_up()) {
        TEST_IGNORE_MESSAGE("network not initialized");
        return;
    }

    struct net_info info;
    net_get_info(&info);
    if (!info.dhcp_enabled) {
        TEST_IGNORE_MESSAGE("DHCP not enabled (NET_DHCP_AT_BOOT=OFF)");
        return;
    }

    /* Poll for up to 2 seconds waiting for a bind. Using elapsed time
     * rather than absolute compare avoids uint32_t wrap issues. */
    uint32_t start = sys_now();
    while ((sys_now() - start) < 2000) {
        net_poll();
        net_get_info(&info);
        if (info.dhcp_status == NET_DHCP_BOUND)
            break;
    }

    if (info.dhcp_status != NET_DHCP_BOUND) {
        TEST_IGNORE_MESSAGE("DHCP did not bind within 2s "
                            "(QEMU SLIRP may not be active)");
        return;
    }

    /* Assert the lease is non-zero — the lease address is network-
     * specific (10.0.2.15 under QEMU SLIRP, a real lab-subnet lease
     * on Pi 5 / MACB), so a zero-value IP is the only platform-
     * independent failure signal we can check here. */
    TEST_ASSERT_MESSAGE(info.ip_addr != 0,
        "DHCP bound but IP address is 0 — lease never populated info.ip_addr");
}

/*
 * Test: DHCP bind status callback fires when an address is acquired
 * (issue #201).
 *
 * The netif status callback installed by slm_netif_init() is supposed
 * to log "DHCP bound: ..." on the false→true transition of
 * dhcp_supplied_address(). We can't capture log output from Unity, so
 * net_get_dhcp_bind_count() exposes a counter; this test asserts it
 * advanced during the live DHCP test. Runs *after* test_net_dhcp_binds
 * (which drives the netif to BOUND via QEMU SLIRP).
 *
 * Skipped if DHCP didn't actually bind (e.g. no SLIRP) — binding is
 * the precondition, and test_net_dhcp_binds already covers the
 * "did bind" assertion itself.
 */
static void test_net_dhcp_bind_notification(void)
{
    if (!net_is_up()) {
        TEST_IGNORE_MESSAGE("network not initialized");
        return;
    }

    struct net_info info;
    net_get_info(&info);
    if (info.dhcp_status != NET_DHCP_BOUND) {
        TEST_IGNORE_MESSAGE("DHCP not bound — precondition for notify test");
        return;
    }

    /* At least one BOUND transition must have been recorded. If zero,
     * the status callback didn't fire — regression in slm_netif_init's
     * netif_set_status_callback() registration. */
    uint32_t binds = net_get_dhcp_bind_count();
    TEST_ASSERT_MESSAGE(binds >= 1,
        "DHCP bind-count should be >=1 after BOUND (#201 status callback)");
}

/*
 * Test: DHCP fallback restores static IP when no server answers
 * (issue #197, auto-DHCP fallback path).
 *
 * Can't force SLIRP to not answer, so this exercises the fallback
 * logic with a different approach: call net_set_dhcp_timeout_ms(1)
 * to shrink the timeout below the polling cadence, then re-enable
 * DHCP. The first net_poll() after reaches the timeout check before
 * lwIP has a chance to bind, so dhcp_status transitions to
 * NET_DHCP_FAILED and the static 10.0.2.15 is restored.
 *
 * After the test, the timeout is restored to the default so
 * subsequent tests aren't affected.
 */
static void test_net_dhcp_fallback(void)
{
    if (!net_is_up()) {
        TEST_IGNORE_MESSAGE("network not initialized");
        return;
    }

    /* Save the current timeout to restore at end */
    uint32_t saved_timeout = net_get_dhcp_timeout_ms();

    /* Reset to static IP first — this stops any in-progress DHCP so
     * the re-enable below has fresh state. net_set_static_ip also
     * clears the netif's DHCP binding, so dhcp_supplied_address()
     * returns false on the next poll. */
    int ret = net_set_static_ip(net_ip4_addr(10, 0, 2, 15),
                                net_ip4_addr(255, 255, 255, 0),
                                net_ip4_addr(10, 0, 2, 2));
    TEST_ASSERT_EQUAL_INT(0, ret);

    /* Re-enable DHCP with a 0 ms timeout — the fallback check will
     * fire immediately on the next call, before SLIRP has any chance
     * to respond. Avoids the recv → OFFER → bind race that makes
     * millisecond timeouts non-deterministic in CI. */
    net_set_dhcp_timeout_ms(0);
    ret = net_enable_dhcp();
    TEST_ASSERT_EQUAL_INT(0, ret);

    /* Direct call — bypasses net_poll()'s recv path so SLIRP can't
     * bind DHCP before the timeout check runs. */
    int fired = net_dhcp_check_timeout();
    TEST_ASSERT_MESSAGE(fired == 1,
        "net_dhcp_check_timeout should fire fallback with 0 ms timeout");

    struct net_info info;
    TEST_ASSERT_EQUAL_INT(0, net_get_info(&info));
    TEST_ASSERT_MESSAGE(info.dhcp_status == NET_DHCP_FAILED,
        "fallback should transition dhcp_status to NET_DHCP_FAILED");
    TEST_ASSERT_MESSAGE(!info.dhcp_enabled,
        "dhcp_enabled should be cleared after fallback");
    TEST_ASSERT_MESSAGE(info.ip_addr == net_ip4_addr(10, 0, 2, 15),
        "fallback should restore the original static IP");

    /* Restore default timeout for subsequent tests */
    net_set_dhcp_timeout_ms(saved_timeout);
}

/*
 * Test: a manual DHCP request made while the link is down stays
 * pending until the client can actually start, rather than burning
 * its timeout budget before link-ready.
 */
static void test_net_dhcp_fallback_while_link_down(void)
{
    if (!net_is_up()) {
        TEST_IGNORE_MESSAGE("network not initialized");
        return;
    }

    uint32_t saved_timeout = net_get_dhcp_timeout_ms();

    TEST_ASSERT_EQUAL_INT(0, net_set_static_ip(net_ip4_addr(10, 0, 2, 15),
                                               net_ip4_addr(255, 255, 255, 0),
                                               net_ip4_addr(10, 0, 2, 2)));

    link_test_install(false);
    net_poll();

    net_set_dhcp_timeout_ms(20);
    TEST_ASSERT_EQUAL_INT(0, net_enable_dhcp());
    TEST_ASSERT_EQUAL_INT(0, net_dhcp_check_timeout());

    struct net_info info;
    TEST_ASSERT_EQUAL_INT(0, net_get_info(&info));
    TEST_ASSERT_EQUAL_INT(NET_DHCP_PENDING, info.dhcp_status);
    TEST_ASSERT_TRUE(info.dhcp_enabled);
    TEST_ASSERT_EQUAL_HEX32(net_ip4_addr(10, 0, 2, 15), info.ip_addr);

    extern void sleep_ms(uint32_t ms);
    sleep_ms(30);
    TEST_ASSERT_EQUAL_INT(0, net_dhcp_check_timeout());

    link_test_set(true);
    net_poll();
    TEST_ASSERT_EQUAL_INT(0, net_dhcp_check_timeout());

    TEST_ASSERT_EQUAL_INT(0, net_get_info(&info));
    TEST_ASSERT_NOT_EQUAL(NET_DHCP_FAILED, info.dhcp_status);
    TEST_ASSERT_TRUE(info.dhcp_enabled);

    TEST_ASSERT_EQUAL_INT(0, net_set_static_ip(net_ip4_addr(10, 0, 2, 15),
                                               net_ip4_addr(255, 255, 255, 0),
                                               net_ip4_addr(10, 0, 2, 2)));
    link_test_restore();
    net_poll();
    net_set_dhcp_timeout_ms(saved_timeout);
}

/*
 * Test: issuing a duplicate manual DHCP request while discovery is
 * already running must not disable the existing timeout budget.
 */
static void test_net_dhcp_duplicate_request_preserves_timeout(void)
{
    if (!net_is_up()) {
        TEST_IGNORE_MESSAGE("network not initialized");
        return;
    }

    uint32_t saved_timeout = net_get_dhcp_timeout_ms();

    TEST_ASSERT_EQUAL_INT(0, net_set_static_ip(net_ip4_addr(10, 0, 2, 15),
                                               net_ip4_addr(255, 255, 255, 0),
                                               net_ip4_addr(10, 0, 2, 2)));

    /* Immediate timeout so the direct fallback check stays
     * deterministic and avoids the live recv path. */
    net_set_dhcp_timeout_ms(0);
    TEST_ASSERT_EQUAL_INT(0, net_enable_dhcp());
    TEST_ASSERT_EQUAL_INT(0, net_enable_dhcp());

    TEST_ASSERT_EQUAL_INT(1, net_dhcp_check_timeout());

    struct net_info info;
    TEST_ASSERT_EQUAL_INT(0, net_get_info(&info));
    TEST_ASSERT_EQUAL_INT(NET_DHCP_FAILED, info.dhcp_status);
    TEST_ASSERT_FALSE(info.dhcp_enabled);
    TEST_ASSERT_EQUAL_HEX32(net_ip4_addr(10, 0, 2, 15), info.ip_addr);

    net_set_dhcp_timeout_ms(saved_timeout);
}

/*
 * Test: boot-time deferred DHCP does not consume its fallback budget
 * before the client actually starts.
 */
static void test_net_boot_deferred_dhcp_waits_for_real_start(void)
{
    if (!net_is_up()) {
        TEST_IGNORE_MESSAGE("network not initialized");
        return;
    }

    uint32_t saved_timeout = net_get_dhcp_timeout_ms();

    TEST_ASSERT_EQUAL_INT(0, net_set_static_ip(net_ip4_addr(10, 0, 2, 15),
                                               net_ip4_addr(255, 255, 255, 0),
                                               net_ip4_addr(10, 0, 2, 2)));

    link_test_install(false);
    net_poll();

    net_set_dhcp_timeout_ms(0);
    net_test_force_boot_deferred_dhcp();
    TEST_ASSERT_EQUAL_INT(0, net_dhcp_check_timeout());

    struct net_info info;
    TEST_ASSERT_EQUAL_INT(0, net_get_info(&info));
    TEST_ASSERT_EQUAL_INT(NET_DHCP_PENDING, info.dhcp_status);
    TEST_ASSERT_TRUE(info.dhcp_enabled);
    TEST_ASSERT_EQUAL_HEX32(net_ip4_addr(10, 0, 2, 15), info.ip_addr);

    TEST_ASSERT_EQUAL_INT(0, net_set_static_ip(net_ip4_addr(10, 0, 2, 15),
                                               net_ip4_addr(255, 255, 255, 0),
                                               net_ip4_addr(10, 0, 2, 2)));
    link_test_restore();
    net_poll();
    net_set_dhcp_timeout_ms(saved_timeout);
}

/*
 * Test: if dhcp_start() fails, networking does not remain stuck in a
 * fake DHCP(pending) state.
 */
static void test_net_dhcp_start_failure_clears_pending_state(void)
{
    if (!net_is_up()) {
        TEST_IGNORE_MESSAGE("network not initialized");
        return;
    }

    TEST_ASSERT_EQUAL_INT(0, net_set_static_ip(net_ip4_addr(10, 0, 2, 15),
                                               net_ip4_addr(255, 255, 255, 0),
                                               net_ip4_addr(10, 0, 2, 2)));

    net_test_force_dhcp_start_fail();
    TEST_ASSERT_EQUAL_INT(NET_E_NO_MEM, net_enable_dhcp());

    struct net_info info;
    TEST_ASSERT_EQUAL_INT(0, net_get_info(&info));
    TEST_ASSERT_EQUAL_INT(NET_DHCP_DISABLED, info.dhcp_status);
    TEST_ASSERT_FALSE(info.dhcp_enabled);
    TEST_ASSERT_EQUAL_HEX32(net_ip4_addr(10, 0, 2, 15), info.ip_addr);
}

/*
 * Test: a transient link drop during DHCP discovery pauses the timeout
 * while carrier is down, then restarts with a fresh budget on
 * reconnect.
 */
static void test_net_dhcp_link_drop_restarts_timeout(void)
{
    if (!net_is_up()) {
        TEST_IGNORE_MESSAGE("network not initialized");
        return;
    }

    extern void sleep_ms(uint32_t ms);
    uint32_t saved_timeout = net_get_dhcp_timeout_ms();

    TEST_ASSERT_EQUAL_INT(0, net_set_static_ip(net_ip4_addr(10, 0, 2, 15),
                                               net_ip4_addr(255, 255, 255, 0),
                                               net_ip4_addr(10, 0, 2, 2)));

    link_test_install(true);
    net_poll();

    net_set_dhcp_timeout_ms(20);
    TEST_ASSERT_EQUAL_INT(0, net_enable_dhcp());

    link_test_set(false);
    net_poll();
    sleep_ms(30);
    TEST_ASSERT_EQUAL_INT(0, net_dhcp_check_timeout());

    link_test_set(true);
    net_poll();

    TEST_ASSERT_EQUAL_INT(0, net_dhcp_check_timeout());

    struct net_info info;
    TEST_ASSERT_EQUAL_INT(0, net_get_info(&info));
    TEST_ASSERT_TRUE(info.dhcp_enabled);
    TEST_ASSERT_NOT_EQUAL(NET_DHCP_FAILED, info.dhcp_status);

    link_test_restore();
    net_poll();
    net_set_dhcp_timeout_ms(saved_timeout);
}

/*
 * Test: a raw frame transmits through the registered driver's send path.
 *
 * Bypasses lwIP and ARP entirely — pushes a single Ethernet broadcast
 * frame directly into the driver's send() ops. The ARM64 MMIO driver
 * and x86-64 PCI driver both implement send() synchronously: push the
 * descriptor onto the TX virtqueue, kick, wait for used-ring completion.
 * A return value of 0 proves the full TX path works: net_driver.send →
 * virtqueue add_buf → device kick → used-ring completion.
 *
 * Uses a minimum-size (64-byte) Ethernet frame with broadcast dest,
 * the driver's MAC as source, EtherType 0x9000 (Loopback test,
 * RFC1042 §19) for the
 * payload — chosen because it doesn't depend on IP/ARP setup. The
 * actual byte content doesn't matter to the device; the test only verifies
 * that the descriptor cycle completes.
 */
/*
 * Build a minimum-size broadcast Ethernet frame in `frame` using the
 * registered driver's MAC as the source. Shared between the raw-TX
 * tests below (send path exercises that don't go through lwIP).
 */
static void test_net_build_loopback_frame(uint8_t frame[64])
{
    const struct net_driver *drv = net_get_driver();
    memset(frame, 0, 64);
    for (int i = 0; i < 6; i++) frame[i] = 0xFF;  /* broadcast dest */
    drv->get_mac(&frame[6]);                       /* source MAC */
    frame[12] = 0x90;                              /* EtherType 0x9000 */
    frame[13] = 0x00;
}

static void test_net_driver_tx(void)
{
    if (!net_is_up()) {
        TEST_IGNORE_MESSAGE("network not initialized");
        return;
    }

    const struct net_driver *drv = net_get_driver();
    TEST_ASSERT_NOT_NULL(drv);

    uint8_t frame[64];
    test_net_build_loopback_frame(frame);

    int ret = drv->send(frame, sizeof(frame));
    TEST_ASSERT_MESSAGE(ret == 0, "driver send() should succeed");
}

/*
 * Test: send() now exposes a tx_reap op (#204).
 *
 * Both VirtIO drivers complete TX asynchronously: send() submits and
 * returns immediately, completion arrives later when net_poll() runs
 * the driver's tx_reap. Verify the op is actually wired up.
 */
static void test_net_driver_has_tx_reap(void)
{
    if (!net_is_up()) {
        TEST_IGNORE_MESSAGE("network not initialized");
        return;
    }

    const struct net_driver *drv = net_get_driver();
    TEST_ASSERT_NOT_NULL(drv);

    /* tx_reap is OPTIONAL per net_driver.h — "drivers that complete
     * TX synchronously inside send() may leave it NULL". Only VirtIO
     * is required to expose it (#204 was specifically about
     * decoupling VirtIO TX ack from send-path spinning). Pi 5's
     * MACB driver and CDC-ECM USB net driver are synchronous and
     * deliberately leave tx_reap NULL. */
    if (drv->name && strcmp(drv->name, "virtio-net-mmio") == 0) {
        TEST_ASSERT_MESSAGE(drv->tx_reap != NULL,
            "VirtIO drivers must expose tx_reap for async completion (#204)");
    } else {
        /* Nothing to assert — tx_reap is optional for this driver. */
        TEST_PASS();
    }
}

/*
 * Test: send() returns promptly without spinning for completion (#204).
 *
 * The pre-#204 send() spun up to VIRTIO_NET_TX_TIMEOUT_MS (100 ms)
 * waiting for the device to ack. The async send returns as soon as
 * the descriptor is queued, which on QEMU is microseconds. Use
 * sys_now() to bound a single send: 50 ms catches the 100 ms
 * spin-wait regression while tolerating scheduler jitter under
 * systemd-run CPU quota (vCPU stalls of several ms are plausible
 * when 4 vCPUs compete for 2 host cores).
 */
static void test_net_send_returns_quickly(void)
{
    if (!net_is_up()) {
        TEST_IGNORE_MESSAGE("network not initialized");
        return;
    }

    const struct net_driver *drv = net_get_driver();
    uint8_t frame[64];
    test_net_build_loopback_frame(frame);

    uint32_t start = sys_now();
    int ret = drv->send(frame, sizeof(frame));
    uint32_t elapsed = sys_now() - start;

    TEST_ASSERT_MESSAGE(ret == 0, "driver send should accept the frame");
    TEST_ASSERT_MESSAGE(elapsed < 50,
        "driver send should return promptly (async, not spin-wait) — #204");
}

/*
 * Test: send() rejects packets larger than MTU with NET_E_TOO_LARGE (#204).
 *
 * Drivers differ on their exact cap:
 *   virtio-net-mmio: 1514 B (strict Ethernet MTU)
 *   cdns-macb (Pi 5): 2048 B (TX buffer size)
 * so the test uses 8192 B — comfortably above every driver's
 * accept limit. Content irrelevant; the size check happens before
 * any pool logic runs.
 */
static void test_net_send_oversized_rejected(void)
{
    if (!net_is_up()) {
        TEST_IGNORE_MESSAGE("network not initialized");
        return;
    }

    const struct net_driver *drv = net_get_driver();

    static uint8_t oversized[8192];
    int ret = drv->send(oversized, sizeof(oversized));
    TEST_ASSERT_MESSAGE(ret == NET_E_TOO_LARGE,
        "oversized send must return NET_E_TOO_LARGE (#204)");
}

/*
 * Test: pool exhaustion returns NET_E_BUSY without blocking (#204).
 *
 * The TX buffer pool is sized at 16 slots in both drivers. To
 * exhaust it without hitting the opportunistic reap in send(), we
 * need to submit more than 16 frames before any can complete. The
 * existing 8-burst test above shows 8 submits all succeed; this
 * test pushes past the pool limit and confirms the overflow path
 * returns NET_E_BUSY instead of spinning or deadlocking.
 *
 * On QEMU the device completes TX so fast that the opportunistic
 * reap inside send() keeps reclaiming slots even in a tight loop —
 * we may never actually see NET_E_BUSY. The test is written to
 * pass in either case:
 *   - if the device keeps up: all 32 submits return 0 (observed
 *     behaviour on QEMU with SLIRP)
 *   - if the pool fills: at least one returns NET_E_BUSY and none
 *     return other error codes
 * In both cases the test succeeds. The point is to prove the
 * NET_E_BUSY return is the only overflow outcome — no spin, no
 * crash, no NET_E_GENERIC.
 */
static void test_net_send_pool_exhaustion(void)
{
    if (!net_is_up()) {
        TEST_IGNORE_MESSAGE("network not initialized");
        return;
    }

    const struct net_driver *drv = net_get_driver();
    uint8_t frame[64];
    test_net_build_loopback_frame(frame);

    /* Push past pool size (16) without any intervening net_poll */
    int ok = 0, busy = 0, other = 0;
    uint32_t start = sys_now();
    for (int i = 0; i < 32; i++) {
        int ret = drv->send(frame, sizeof(frame));
        if (ret == 0)               ok++;
        else if (ret == NET_E_BUSY) busy++;
        else                        other++;
    }
    uint32_t elapsed = sys_now() - start;

    TEST_ASSERT_MESSAGE(other == 0,
        "pool overflow must only produce NET_E_BUSY, no other error codes");
    TEST_ASSERT_MESSAGE(ok + busy == 32,
        "every send must return either 0 or NET_E_BUSY (#204)");
    /* 500ms ceiling matches the 50ms single-send bound × 32 × margin;
     * under systemd-run CPUQuota=200% with 4 vCPUs on 2 host cores,
     * pooled scheduler jitter can accumulate. The point is catching
     * a reintroduced spin (100 ms × 32 = 3.2 s), not tight timing. */
    TEST_ASSERT_MESSAGE(elapsed < 500,
        "32 async submits must not spin — total < 500 ms");

    /* Drain completions so subsequent tests have a clean pool */
    for (int i = 0; i < 64; i++) {
        net_poll();
    }
}

/*
 * Test: multiple back-to-back sends fit in the TX buffer pool without
 * blocking, completion drains via net_poll() (#204).
 *
 * Submits 8 frames in rapid succession (TX_BUFFER_COUNT == 16, so
 * 8 fits with margin). With the old synchronous TX each send would
 * spin for completion; with async, all 8 submit immediately and the
 * pool absorbs them. After a few net_poll() cycles, tx_reap drains
 * the used ring and frees the slots. Asserts:
 *   - all 8 sends return 0 (no NET_E_BUSY despite back-to-back submit)
 *   - elapsed wall time well under what 8× synchronous waits would
 *     have taken (8 × 100 ms = 800 ms; we expect < 100 ms)
 *
 * Note: the test calls drv->send() directly rather than going
 * through lwIP, so net_statistics.tx_packets (maintained by
 * slm_netif_output) doesn't advance here. The pool behavior is what
 * we're validating, not lwIP accounting.
 */
static void test_net_burst_8_sends_async(void)
{
    if (!net_is_up()) {
        TEST_IGNORE_MESSAGE("network not initialized");
        return;
    }

    const struct net_driver *drv = net_get_driver();
    uint8_t frame[64];
    test_net_build_loopback_frame(frame);

    uint32_t start = sys_now();
    int ok = 0;
    for (int i = 0; i < 8; i++) {
        if (drv->send(frame, sizeof(frame)) == 0)
            ok++;
    }
    uint32_t submit_elapsed = sys_now() - start;

    TEST_ASSERT_MESSAGE(ok == 8,
        "all 8 back-to-back sends should fit in the TX pool");
    TEST_ASSERT_MESSAGE(submit_elapsed < 100,
        "8 async submits should complete in <100ms (was 8×100ms sync)");

    /* Drive completion: net_poll calls tx_reap, freeing pool slots */
    for (int i = 0; i < 32; i++) {
        net_poll();
    }
}

#if defined(PLATFORM_QEMU_VIRT)
/*
 * Test: VirtIO-Net handler is registered in the GIC dispatch table (#204).
 *
 * Proves virtio_net_init wired gic_register_handler successfully.  The
 * dispatch path in kernel/arch/arm64/exceptions.c uses gic_lookup_handler
 * to route unknown SPIs to driver-provided handlers — if registration
 * silently fails (table full, or init skips it), TX completions stay
 * polled-only on hardware, defeating the point of the #204 follow-up.
 * A fast, deterministic check that doesn't depend on actual IRQ delivery
 * (which on QEMU only happens when the idle task runs daifclr+wfi).
 */
static void test_net_irq_handler_registered(void)
{
    if (!net_is_up()) {
        TEST_IGNORE_MESSAGE("network not initialized");
        return;
    }

    /* Use the runtime IRQ number — the device slot is probed at init,
     * so the compile-time VIRTIO_NET_IRQ (which assumes slot 0) does
     * not match when QEMU places the device at a different slot. */
    uint32_t irq = virtio_net_get_irq();
    TEST_ASSERT_MESSAGE(irq != 0,
        "virtio_net_get_irq() returned 0 — init skipped handler registration");

    gic_handler_fn h = gic_lookup_handler(irq);
    TEST_ASSERT_NOT_NULL_MESSAGE(h,
        "gic_lookup_handler returned NULL for the registered IRQ");
    TEST_ASSERT_MESSAGE(h == virtio_net_irq_handler,
        "registered handler does not match virtio_net_irq_handler");
}

/*
 * Test: invoking virtio_net_irq_handler drains any pending TX completions
 * and bumps the IRQ-count observability counter (#204).
 *
 * Submits a few frames to push descriptors through the TX virtqueue,
 * then calls the handler directly (not via GIC — see comment above).
 * The handler reads the ISR, acks it, and drains the used ring on
 * USED_BUFFER. irq_count incrementing confirms the handler itself is
 * reachable from the dispatch path and does not early-exit on a freshly
 * initialized driver. With end-to-end GIC → CPU delivery blocked in
 * task context on QEMU (tasks run DAIF.I=1), this is the closest we get
 * to exercising the production path from userland tests.
 */
static void test_net_irq_handler_drains_tx(void)
{
    if (!net_is_up()) {
        TEST_IGNORE_MESSAGE("network not initialized");
        return;
    }

    const struct net_driver *drv = net_get_driver();
    uint8_t frame[64];
    test_net_build_loopback_frame(frame);

    uint32_t before = virtio_net_get_irq_count();

    /* Submit a few frames so the device has something to complete. */
    for (int i = 0; i < 4; i++) {
        (void)drv->send(frame, sizeof(frame));
    }

    /* Invoke the handler directly. No GIC unmasking — this tests the
     * handler body, not the dispatch/delivery plumbing. */
    virtio_net_irq_handler();

    uint32_t after = virtio_net_get_irq_count();
    TEST_ASSERT_MESSAGE(after == before + 1,
        "virtio_net_irq_handler did not increment irq_count — "
        "handler early-exited or counter wiring broken");
}
#endif /* PLATFORM_QEMU_VIRT */

#if defined(PLATFORM_X86_64)
/*
 * Test: MSI-X was enabled during virtio_net_pci_init (#204 item 3).
 *
 * QEMU's virtio-net-pci exposes the MSI-X capability by default.
 * If this fails, either the capability walk missed it, the programming
 * write sequence was rejected, or someone disabled MSI-X with
 * `-device virtio-net-pci,msix=off` — the last of which would make
 * TX completion fall back to polling, defeating the #204 point.
 */
static void test_net_msix_enabled(void)
{
    if (!net_is_up()) {
        TEST_IGNORE_MESSAGE("network not initialized");
        return;
    }
    TEST_ASSERT_MESSAGE(virtio_net_pci_msix_enabled(),
        "MSI-X not enabled — TX completion silently fell back to polling");
}

/*
 * Test: the allocated MSI-X vector is in the x86-64 free-vector range
 * (50-63). Below 50 collides with timer/RESCHED/PIC; above 63 collides
 * with nothing yet but isn't in our convention. A value of 0 means
 * msix_enabled() returned false, which the prior test already
 * catches — assert a plausible positive number too to guard against
 * a silent drift of VIRTIO_NET_MSIX_VECTOR.
 */
static void test_net_msix_vector_is_in_range(void)
{
    if (!net_is_up()) {
        TEST_IGNORE_MESSAGE("network not initialized");
        return;
    }
    uint32_t v = virtio_net_pci_get_msix_vector();
    TEST_ASSERT_MESSAGE(v >= 50 && v <= 63,
        "MSI-X vector outside the 50-63 range reserved for virtio drivers");
}

/*
 * Test: direct invocation of virtio_net_pci_irq_handler drains the
 * TX used ring and bumps the IRQ count (#204 item 3).
 *
 * Parallel of the ARM64 test_net_irq_handler_drains_tx — exercises
 * the handler body without depending on end-to-end MSI-X delivery
 * (which on QEMU in task context works but adds scheduling
 * nondeterminism to the assertion). Submits a few frames, calls
 * the handler directly, verifies the counter advanced exactly once.
 */
static void test_net_msix_handler_drains_tx(void)
{
    if (!net_is_up()) {
        TEST_IGNORE_MESSAGE("network not initialized");
        return;
    }
    if (!virtio_net_pci_msix_enabled()) {
        TEST_IGNORE_MESSAGE("MSI-X not enabled — handler path not active");
        return;
    }

    const struct net_driver *drv = net_get_driver();
    uint8_t frame[64];
    test_net_build_loopback_frame(frame);

    uint32_t before = virtio_net_pci_get_irq_count();
    for (int i = 0; i < 4; i++) {
        (void)drv->send(frame, sizeof(frame));
    }
    virtio_net_pci_irq_handler(0);

    /* >= rather than == because x86-64 runs tasks with IF=1, so a
     * real MSI-X delivery can slip in between the sends and the
     * direct handler call and bump the counter ahead of ours. The
     * ARM64 equivalent uses exact-match because DAIF.I=1 blocks
     * in-task IRQ delivery. Either way the handler was reached
     * at least once, which is what this test verifies. */
    uint32_t after = virtio_net_pci_get_irq_count();
    TEST_ASSERT_MESSAGE(after >= before + 1,
        "virtio_net_pci_irq_handler did not bump irq_count — handler "
        "early-exited or counter wiring broken");
}

/*
 * Test: stuck-descriptor watchdog stays silent on normal traffic
 * (#204 item 4).
 *
 * Run a standard burst-and-drain cycle through the PCI driver.
 * Every reap either finds completions (progressed=true, watchdog
 * resets) or runs on an empty pool (tx_has_inflight=false, watchdog
 * path skipped). Either way the stall count must not advance. If
 * this fails, the watchdog is firing spuriously on healthy traffic
 * and will flood the log in production.
 */
static void test_net_pci_watchdog_quiet(void)
{
    if (!net_is_up()) {
        TEST_IGNORE_MESSAGE("network not initialized");
        return;
    }

    const struct net_driver *drv = net_get_driver();
    uint8_t frame[64];
    test_net_build_loopback_frame(frame);

    uint32_t before = virtio_net_pci_get_tx_stall_count();
    for (int i = 0; i < 8; i++) {
        (void)drv->send(frame, sizeof(frame));
    }
    for (int i = 0; i < 32; i++) {
        net_poll();
    }
    uint32_t after = virtio_net_pci_get_tx_stall_count();

    TEST_ASSERT_MESSAGE(after == before,
        "TX stuck-descriptor watchdog fired on healthy traffic");
}

/*
 * Test: stuck-descriptor watchdog fires when a real stall is
 * simulated (#204 item 4).
 *
 * Can't easily stall QEMU's TX completion, and tight-loop sending
 * gets opportunistically reaped inside send() anyway. Instead the
 * driver exposes a test-only trigger that drives the watchdog's
 * inner branch with synthetic inputs: no-progress + in-flight=true
 * + elapsed > threshold. Same code path a real stall would take;
 * the counter must advance by exactly 1.
 */
static void test_net_pci_watchdog_fires_on_stall(void)
{
    if (!net_is_up()) {
        TEST_IGNORE_MESSAGE("network not initialized");
        return;
    }

    uint32_t before = virtio_net_pci_get_tx_stall_count();
    virtio_net_pci_test_trigger_watchdog();
    uint32_t after = virtio_net_pci_get_tx_stall_count();

    TEST_ASSERT_MESSAGE(after == before + 1,
        "Watchdog trigger hook did not advance tx_stall_warn_count by 1");
}
#endif /* PLATFORM_X86_64 */

/*
 * Cross-platform watchdog quiet-path test. On ARM64 this uses the
 * MMIO driver's accessor; PLATFORM_X86_64 version lives above.
 * Both drivers must keep the watchdog silent on normal traffic.
 */
#if defined(PLATFORM_QEMU_VIRT)
static void test_net_mmio_watchdog_quiet(void)
{
    if (!net_is_up()) {
        TEST_IGNORE_MESSAGE("network not initialized");
        return;
    }

    const struct net_driver *drv = net_get_driver();
    uint8_t frame[64];
    test_net_build_loopback_frame(frame);

    uint32_t before = virtio_net_get_tx_stall_count();
    for (int i = 0; i < 8; i++) {
        (void)drv->send(frame, sizeof(frame));
    }
    for (int i = 0; i < 32; i++) {
        net_poll();
    }
    uint32_t after = virtio_net_get_tx_stall_count();

    TEST_ASSERT_MESSAGE(after == before,
        "TX stuck-descriptor watchdog fired on healthy traffic (mmio)");
}

static void test_net_mmio_watchdog_fires_on_stall(void)
{
    if (!net_is_up()) {
        TEST_IGNORE_MESSAGE("network not initialized");
        return;
    }

    uint32_t before = virtio_net_get_tx_stall_count();
    virtio_net_test_trigger_watchdog();
    uint32_t after = virtio_net_get_tx_stall_count();

    TEST_ASSERT_MESSAGE(after == before + 1,
        "Watchdog trigger hook did not advance tx_stall_warn_count by 1 (mmio)");
}
#endif /* PLATFORM_QEMU_VIRT */

/* ============================================================================
 * lwIP RNG (kernel/net/sys_arch.c)
 *
 * lwip_rand_seed mixes DTB-supplied /chosen/{rng-seed,kaslr-seed}
 * entropy into the LCG state at boot. These tests verify the seed is
 * observable in subsequent output. The underlying RNG is
 * non-cryptographic; we only verify state-mutation, not statistical
 * quality.
 * ============================================================================ */

/* lwip_rand_slm + lwip_rand_seed prototypes come from <net.h>. */

static void test_lwip_rand_seed_changes_output(void)
{
    uint32_t baseline_a = lwip_rand_slm();
    uint32_t baseline_b = lwip_rand_slm();

    static const uint8_t entropy[32] = {
        0xa1,0xb2,0xc3,0xd4,0xe5,0xf6,0x07,0x18,
        0x29,0x3a,0x4b,0x5c,0x6d,0x7e,0x8f,0x90,
        0x11,0x22,0x33,0x44,0x55,0x66,0x77,0x88,
        0x99,0xaa,0xbb,0xcc,0xdd,0xee,0xff,0x00,
    };
    lwip_rand_seed(entropy, sizeof(entropy));

    uint32_t after_a = lwip_rand_slm();
    uint32_t after_b = lwip_rand_slm();

    bool any_diff = (after_a != baseline_a) || (after_b != baseline_b);
    TEST_ASSERT_TRUE(any_diff);
}

static void test_lwip_rand_seed_null_or_zero_len_does_not_crash(void)
{
    /* Smoke test: NULL bytes and zero length must be safe inputs.
     *
     * Note: a stricter "is observably a no-op on rand_state" assertion
     * would need a state-readback hook in sys_arch.c, since
     * `lwip_rand_slm` advances state and folds in a fresh timer count
     * on every call — making "did the seed call write state?" hard to
     * observe from outside. The function's body has an early
     * `if (!bytes || len == 0) return;` guard; this test verifies the
     * guard is reachable and downstream RNG usage still works. */
    lwip_rand_seed(NULL, 32);
    lwip_rand_seed("data", 0);
    (void)lwip_rand_slm();
    TEST_ASSERT_TRUE(true);
}

static void test_lwip_rand_seed_different_inputs_diverge(void)
{
    static const uint8_t seed_a[16] = {1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1};
    static const uint8_t seed_b[16] = {2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2};

    /* Pre-roll to align timer mix between the two paths. */
    (void)lwip_rand_slm();
    (void)lwip_rand_slm();

    lwip_rand_seed(seed_a, sizeof(seed_a));
    uint32_t r1 = lwip_rand_slm();
    lwip_rand_seed(seed_b, sizeof(seed_b));
    uint32_t r2 = lwip_rand_slm();

    TEST_ASSERT_TRUE(r1 != r2);
}

/*
 * #581 regression: RX frames must come from PBUF_POOL, not the lwip
 * heap. Production fix lives in `kernel/net/lwip_slm.c::net_poll`,
 * where the per-frame `pbuf_alloc` switched from `PBUF_RAM` to
 * `PBUF_POOL`. Without that change, sustained RX exhausts the heap
 * (which is shared with TCP send/receive buffers, retransmit
 * segments, ARP queues, etc.) and large transfers stall under the
 * RX-stall watchdog.
 *
 * The test installs a wrapper driver whose `recv()` returns a
 * canned 64-byte ethertype-0x9000 frame N times. lwip routes each
 * frame to the netif input, finds no matching upper protocol, and
 * frees the pbuf. The structural assertion is "the pbuf pool was
 * exercised" — `lwip_stats.memp[MEMP_PBUF_POOL]->max` ticks above
 * its pre-burst value once at least one frame is processed. With a
 * regression to PBUF_RAM, RX would never touch the pool and `max`
 * would stay flat.
 */
/* Burst size deliberately well below PBUF_POOL_SIZE (64 in lwipopts.h)
 * so the test doesn't depend on lwip processing fully synchronously
 * against the live virtio-net RX traffic the net_pump task is also
 * driving. The structural claim (PBUF_POOL is used at all) only
 * needs the pool peak to advance — exact frame counts aren't the
 * load-bearing assertion. */
#define RX_BURST_TEST_FRAMES        32
#define RX_BURST_TEST_POLL_ITERS    128  /* generous; net_poll consumes 1/iter */

static const struct net_driver *rx_burst_test_base;
static int  rx_burst_test_remaining;
static uint8_t rx_burst_test_frame[64];

static int rx_burst_test_init(void)
{
    if (rx_burst_test_base && rx_burst_test_base->init)
        return rx_burst_test_base->init();
    return NET_OK;
}

static int rx_burst_test_send(const void *buf, size_t len)
{
    return rx_burst_test_base->send(buf, len);
}

static int rx_burst_test_recv(void *buf, size_t max_len)
{
    if (rx_burst_test_remaining <= 0)
        return rx_burst_test_base->recv(buf, max_len);

    size_t n = sizeof(rx_burst_test_frame);
    if (n > max_len)
        n = max_len;
    memcpy(buf, rx_burst_test_frame, n);
    rx_burst_test_remaining--;
    return (int)n;
}

static void rx_burst_test_get_mac(uint8_t mac[6])
{
    rx_burst_test_base->get_mac(mac);
}

static bool rx_burst_test_link_status(void)
{
    return rx_burst_test_base->link_status();
}

static void rx_burst_test_tx_reap(void)
{
    if (rx_burst_test_base->tx_reap)
        rx_burst_test_base->tx_reap();
}

static const struct net_driver rx_burst_test_driver = {
    .name        = "rx-burst-test",
    .init        = rx_burst_test_init,
    .send        = rx_burst_test_send,
    .recv        = rx_burst_test_recv,
    .get_mac     = rx_burst_test_get_mac,
    .link_status = rx_burst_test_link_status,
    .tx_reap     = rx_burst_test_tx_reap,
};

static void test_net_rx_burst_uses_pbuf_pool_not_heap(void)
{
    if (!net_is_up()) {
        TEST_IGNORE_MESSAGE("network not initialized");
        return;
    }

    /* Capture pre-burst state. The pbuf pool peak is what
     * differentiates PBUF_POOL (touched at least once) from PBUF_RAM
     * (pool never touched). */
    struct net_watchdog_snapshot before;
    net_watchdog_get(&before);

    /* Build a 64-byte broadcast-dest ethertype-0x9000 frame. lwip
     * accepts the netif input but finds no matching upper protocol
     * and frees the pbuf — no TCP/UDP state retained. */
    test_net_build_loopback_frame(rx_burst_test_frame);

    /* Install the wrapper driver; subsequent net_poll() calls pull
     * RX_BURST_TEST_FRAMES copies of the canned frame. */
    rx_burst_test_base = net_get_driver();
    TEST_ASSERT_NOT_NULL(rx_burst_test_base);
    rx_burst_test_remaining = RX_BURST_TEST_FRAMES;
    net_register_driver(&rx_burst_test_driver);

    for (int i = 0; i < RX_BURST_TEST_POLL_ITERS &&
                    rx_burst_test_remaining > 0; i++) {
        net_poll();
    }

    /* Restore the original driver before asserting so a failure
     * doesn't leave the test wrapper installed for downstream tests.
     *
     * Order matters: clear `rx_burst_test_remaining` first so any
     * in-flight wrapper-recv call from net_pump_task running on a
     * different CPU falls into the delegate-to-base path; then swap
     * `active_driver` back to the real driver. We deliberately do
     * NOT null `rx_burst_test_base` afterwards — leaving it pointing
     * at the (still-valid) real driver keeps a late wrapper call
     * safe to dereference. The pump task may sit on the wrapper for
     * one more poll iteration after the swap; that's harmless. */
    rx_burst_test_remaining = 0;
    net_register_driver(rx_burst_test_base);

    struct net_watchdog_snapshot after;
    net_watchdog_get(&after);

    /* All N frames must have been delivered to the netif input
     * without dropping. Note `rx_packets` is monotonic and may
     * include packets the live driver received during the test
     * window — assert >= delta, not equality. */
    TEST_ASSERT_TRUE(after.rx_packets >= before.rx_packets +
                     RX_BURST_TEST_FRAMES);
    TEST_ASSERT_EQUAL_UINT64(before.rx_dropped, after.rx_dropped);

    /* Load-bearing claim: PBUF_POOL was actually used during the
     * burst. With the production fix in place, every RX frame allocs
     * a pool slot, lwip processes it (ethertype 0x9000 has no upper-
     * layer handler so it's freed at the eth-input default case),
     * and the pbuf returns to the pool. Pool peak rises to at least
     * 1 (transient during processing). Without the fix (PBUF_RAM
     * regression), the pool is never touched and peak stays at 0.
     *
     * Earlier revisions of this test asserted `peak_after >
     * peak_before` (strict delta). That worked accidentally because
     * pre-fix unknown ethertypes leaked pbufs in the pool — the peak
     * grew monotonically with each frame. With the leak fixed (#581),
     * pool churns through alloc-free pairs so the peak settles at 1
     * for the whole burst. The correct invariant is "peak >= 1 at
     * any point during the burst", which the post-burst max captures
     * since lwip_stats.memp[].max is monotonic. */
#if MEMP_STATS
    uint32_t pbuf_pool_max_after =
        (uint32_t)lwip_stats.memp[MEMP_PBUF_POOL]->max;
    TEST_ASSERT_TRUE(pbuf_pool_max_after >= 1);
#endif

    /* Sanity: heap usage stayed bounded. Even with PBUF_RAM the
     * heap settles back to baseline after the burst (lwip drops
     * unrouted ethertype-0x9000 frames promptly), so this is a
     * coarser check — the pool-peak assertion above is the load-
     * bearing one. */
#if MEM_STATS
    TEST_ASSERT_TRUE(after.heap_used_peak < (MEM_SIZE - 1024u));
#endif
}

/*
 * #581 throughput follow-up: net_poll must drain ALL frames the
 * driver has buffered, not just one. Pre-fix net_poll() called
 * active_driver->recv() exactly once and bailed; with TCP fragmenting
 * an 8 KB framed `xput chunk` command into ~6 segments and net_pump
 * running at ~100 Hz, that single-frame-per-tick cadence put a 60 ms
 * floor on per-chunk turnaround and capped 1 GB upload throughput at
 * ~2 MB/min. Post-fix, net_poll loops until recv returns 0.
 *
 * This test asserts the loop semantics directly: queue MULTIPLE
 * frames in the wrapper driver, call net_poll() exactly ONCE, and
 * verify rx_packets advanced by the queued count. Pre-fix this
 * delta would be 1; post-fix it equals the queued count.
 */
#define RX_DRAIN_TEST_FRAMES   4

static void test_net_poll_drains_all_buffered_frames(void)
{
    if (!net_is_up()) {
        TEST_IGNORE_MESSAGE("network not initialized");
        return;
    }

    test_net_build_loopback_frame(rx_burst_test_frame);

    rx_burst_test_base = net_get_driver();
    TEST_ASSERT_NOT_NULL(rx_burst_test_base);

    struct net_watchdog_snapshot before;
    net_watchdog_get(&before);

    /* Queue N frames in the wrapper. Each `recv()` returns one and
     * decrements the counter; when zero, the wrapper falls through
     * to the live driver. After this assignment, rx_burst_test_recv
     * returns the canned frame N times. */
    rx_burst_test_remaining = RX_DRAIN_TEST_FRAMES;
    net_register_driver(&rx_burst_test_driver);

    /* The load-bearing call: ONE net_poll. Pre-fix, this drains
     * exactly 1 frame (recv called once); post-fix, it loops until
     * recv returns 0 and drains all N. */
    net_poll();

    /* Restore the real driver. Order matters per the burst test:
     * clear the counter first so any in-flight wrapper-recv from a
     * concurrent net_pump tick falls into the delegate path, then
     * swap active_driver. */
    rx_burst_test_remaining = 0;
    net_register_driver(rx_burst_test_base);

    struct net_watchdog_snapshot after;
    net_watchdog_get(&after);

    /* Strict delta: rx_packets MUST have advanced by exactly
     * RX_DRAIN_TEST_FRAMES from this single net_poll call.
     * `>= +N` rather than `== +N` only because net_pump is also
     * polling concurrently and may have processed real traffic
     * (DHCP renewals, ARP) during this test window. The
     * load-bearing claim is "more than 1 frame drained per tick";
     * the +N lower bound covers that without flaking on background
     * traffic. */
    TEST_ASSERT_TRUE(after.rx_packets >= before.rx_packets +
                     RX_DRAIN_TEST_FRAMES);

    /* Counter must hit zero — proves recv was called RX_DRAIN_TEST_FRAMES
     * times within the single net_poll. If only 1 frame was drained,
     * remaining would be (N-1). */
    TEST_ASSERT_EQUAL_INT(0, rx_burst_test_remaining);

    /* No drops — pool/heap budgets are unchanged from the test_net_rx_burst
     * test which already exercises the same path with 32 frames. */
    TEST_ASSERT_EQUAL_UINT64(before.rx_dropped, after.rx_dropped);
}

/*
 * #585: regression test for the chained-pbuf RX assembly path.
 *
 * The #581 fix replaced a contiguous `memcpy(p->payload, src, len)`
 * in lwip_slm.c's RX loop with `pbuf_take(p, src, len)`. The new
 * call walks the pbuf chain so frames longer than PBUF_POOL_BUFSIZE
 * (1536 B) are populated correctly across multiple pool slots; the
 * old memcpy would silently truncate them to the first slot's
 * worth.
 *
 * The existing `test_net_rx_burst_uses_pbuf_pool_not_heap` only
 * covers 64-byte frames, so the chained-pbuf code path is not
 * exercised by it. And the production driver buffer
 * (`rx_packet_buf` in lwip_slm.c) is sized 1518 B, below the pool
 * slot size — meaning a wrapper-driver test that returns >1518 B
 * gets clamped to 1518 by `recv()` and never reaches the chain
 * threshold. To exercise the chain path this test calls
 * `net_test_inject_rx_frame`, which bypasses the recv buffer cap
 * but routes through the same `net_input_frame_into_stack` helper
 * the production loop uses — so a regression of pbuf_take to
 * memcpy fails BOTH the production path AND this test.
 *
 * Frame size: 3000 B (2× PBUF_POOL_BUFSIZE = 3072), guaranteed to
 * span at least two pool slots. Ethertype 0x9000 has no upper-layer
 * handler, so lwIP frees the pbuf at the eth-input default case
 * after parsing the header — the chain assembly is what matters,
 * not whether anyone consumed the payload.
 *
 * Pre-fix regression mode: bytes past offset 1536 are junk
 * (whatever was in the second pool slot before alloc). lwIP's
 * ethernet_input only inspects the L2 header so the corrupted
 * tail wouldn't surface as a parse error — the bug would silently
 * affect any future caller that consumes bigger frames (jumbo
 * support, tunneled protocols, etc.). This test pins the assembly
 * itself: rx_packets advances by 1, rx_dropped doesn't.
 */
#define CHAINED_PBUF_TEST_FRAME_LEN  3000

static uint8_t chained_pbuf_test_frame[CHAINED_PBUF_TEST_FRAME_LEN];

extern int net_test_inject_rx_frame(const uint8_t *frame, size_t len);
extern int net_test_alloc_take_readback(const uint8_t *frame, size_t len,
                                        uint8_t *dest, size_t dest_len);

static uint8_t chained_pbuf_readback[CHAINED_PBUF_TEST_FRAME_LEN];

static void test_net_rx_chained_pbuf_assembly(void)
{
    if (!net_is_up()) {
        TEST_IGNORE_MESSAGE("network not initialized");
        return;
    }

    /* Build the frame: standard broadcast Ethernet header followed
     * by a deterministic byte pattern. The byte-pattern (vs all-
     * zeros) is the load-bearing fixture for the readback assertion
     * below — a regression of pbuf_take to a contiguous memcpy
     * would leave bytes past offset 1536 as whatever was in the
     * second pool slot before alloc, which is almost never our
     * deterministic pattern. */
    static const uint8_t bcast_mac[6] = {0xff, 0xff, 0xff, 0xff, 0xff, 0xff};
    memcpy(chained_pbuf_test_frame + 0, bcast_mac, 6);     /* dst */
    memcpy(chained_pbuf_test_frame + 6, bcast_mac, 6);     /* src */
    chained_pbuf_test_frame[12] = 0x90;                    /* ethertype hi */
    chained_pbuf_test_frame[13] = 0x00;                    /* ethertype lo */
    for (size_t i = 14; i < CHAINED_PBUF_TEST_FRAME_LEN; i++) {
        chained_pbuf_test_frame[i] = (uint8_t)(i & 0xFF);
    }

    /* Phase 1: production code-path smoke check.
     *
     * Drive `net_input_frame_into_stack` end-to-end via the inject
     * hook: alloc + take + slm_netif.input. Asserts the chain alloc
     * doesn't fail and the frame doesn't get dropped at any step in
     * the production helper. Doesn't byte-compare — see phase 2 for
     * that. */
    struct net_watchdog_snapshot before;
    net_watchdog_get(&before);

    int inject_rc = net_test_inject_rx_frame(chained_pbuf_test_frame,
                                             CHAINED_PBUF_TEST_FRAME_LEN);
    TEST_ASSERT_EQUAL_INT(CHAINED_PBUF_TEST_FRAME_LEN, inject_rc);

    struct net_watchdog_snapshot after;
    net_watchdog_get(&after);

    TEST_ASSERT_TRUE(after.rx_packets >= before.rx_packets + 1);
    TEST_ASSERT_EQUAL_UINT64(before.rx_dropped, after.rx_dropped);

    /* Phase 2: chain-assembly correctness.
     *
     * Drive the same alloc + pbuf_take pair, then read the assembled
     * chain back into `chained_pbuf_readback` via pbuf_copy_partial
     * (which walks the chain). Byte-compare against the source. A
     * regression of pbuf_take to `memcpy(p->payload, src, len)`
     * would only populate the first pool slot's worth (1536 B), and
     * pbuf_copy_partial would copy the still-uninitialized contents
     * of the second slot for offsets [1536, 3000) — the byte-
     * compare fails immediately at offset 1536.
     *
     * Done as a separate test hook (not via inject above) because
     * inject hands the pbuf to slm_netif.input, which frees it
     * before we can inspect — and lwIP's ethernet_input only reads
     * the L2 header so it can't see chain corruption past offset
     * 14. The duplication of alloc + take in this hook is documented
     * in the helper's docblock. */
    memset(chained_pbuf_readback, 0xAA, sizeof(chained_pbuf_readback));
    int rb_rc = net_test_alloc_take_readback(
        chained_pbuf_test_frame,
        CHAINED_PBUF_TEST_FRAME_LEN,
        chained_pbuf_readback,
        sizeof(chained_pbuf_readback));
    TEST_ASSERT_EQUAL_INT(CHAINED_PBUF_TEST_FRAME_LEN, rb_rc);
    TEST_ASSERT_EQUAL_MEMORY(chained_pbuf_test_frame,
                             chained_pbuf_readback,
                             CHAINED_PBUF_TEST_FRAME_LEN);
}

/*
 * #609: net_shutdown / net_init cycle leaves the stack in a usable
 * state.
 *
 * The shutdown path tears down the netif, aborts every TCP PCB,
 * releases the DHCP lease, and resets the RX-stall watchdog. The
 * subsequent net_init must succeed (lwip_init is one-shot but the
 * netif setup is re-runnable) and net_is_up() must report true.
 *
 * Pre-fix, net_init had only the early-return guard `if
 * (net_initialized) return 0;` — there was no inverse to drop the
 * netif and reset state. A wedge required a full kexec.
 *
 * Test does the cycle once to catch the most likely regression
 * (e.g., an attempt to call lwip_init twice would corrupt the
 * memp pool free lists and any subsequent allocation would fail
 * or assert; netif_remove without resetting net_initialized would
 * leave the state inconsistent).
 */
static void test_net_shutdown_then_init_succeeds(void)
{
    if (!net_is_up()) {
        TEST_IGNORE_MESSAGE("network not initialized");
        return;
    }

    /* Snapshot the watchdog state to verify it was reset. */
    struct net_watchdog_snapshot before_shutdown;
    net_watchdog_get(&before_shutdown);

    int rc = net_shutdown();
    TEST_ASSERT_EQUAL_INT(0, rc);
    TEST_ASSERT_FALSE(net_is_up());

    /* Watchdog state must be cleared post-shutdown. */
    struct net_watchdog_snapshot after_shutdown;
    net_watchdog_get(&after_shutdown);
    TEST_ASSERT_FALSE(after_shutdown.armed);
    TEST_ASSERT_FALSE(after_shutdown.alarmed);
    TEST_ASSERT_EQUAL_UINT(0, after_shutdown.stall_events);
    TEST_ASSERT_EQUAL_UINT(0, after_shutdown.recovery_events);

    /* Idempotent: a second shutdown is a no-op. */
    rc = net_shutdown();
    TEST_ASSERT_EQUAL_INT(0, rc);

    /* Re-init must succeed. lwip_init() is gated behind a separate
     * `lwip_subsystem_initialized` flag so a second call here
     * does NOT re-run it (which would corrupt pool free lists);
     * only the netif setup runs. */
    rc = net_init();
    TEST_ASSERT_EQUAL_INT(0, rc);
    TEST_ASSERT_TRUE(net_is_up());
}

/*
 * PR #630: net_shutdown must clear in-tree listeners' static
 * `listen_pcb` pointers before lwIP frees the underlying memory.
 *
 * Pre-fix, net_shutdown walked tcp_listen_pcbs and tcp_close()'d
 * each pcb directly. lwIP freed the memp slot, but tcp_shell_server
 * and tcp_telemetry_server each kept their cached static pointers
 * dangling. The first start after net cycling hit
 * `if (listen_pcb) return early`, the next stop tcp_close()'d
 * freed memory and corrupted the MEMP_TCP_PCB_LISTEN free list,
 * and the following start tripped `tcp_free: LISTEN` deep in lwIP.
 *
 * The fix invokes tcp_shell_server_stop() / tcp_telemetry_server_stop()
 * before the listen-pcb walk so each owner nullifies its own pointer.
 * This test verifies that contract: bring up the listeners, run
 * net_shutdown, observe both `_running()` queries return false. A
 * regression that re-introduces the dangling pointer would leave
 * `_running()` returning true (because the static is non-NULL),
 * which is the exact precondition that turned into a UAF.
 */
static void test_net_shutdown_clears_in_tree_listeners(void)
{
    if (!net_is_up()) {
        TEST_IGNORE_MESSAGE("network not initialized");
        return;
    }

    /* Bring telnetd up. Pick port 2324 to avoid colliding with any
     * external state on the default 2323. */
    int rc = tcp_shell_server_start(2324);
    TEST_ASSERT_EQUAL_INT(0, rc);
    TEST_ASSERT_MESSAGE(tcp_shell_server_running(),
        "tcp_shell_server should be running after start");

    /* Bring telemetry up too — same UAF risk applied to it pre-fix. */
    int trc = tcp_telemetry_server_start(0);  /* 0 = default port */
    TEST_ASSERT_EQUAL_INT(0, trc);
    TEST_ASSERT_MESSAGE(tcp_telemetry_server_running(),
        "tcp_telemetry_server should be running after start");

    /* Tear the network down. The pre-fix bug was that this left
     * each listener's static pointer dangling. */
    int sd = net_shutdown();
    TEST_ASSERT_EQUAL_INT(0, sd);

    /* The fix's load-bearing assertion: net_shutdown must invoke
     * each listener's _stop() so the static pointer is cleared.
     * If either of these fails, the next telnetd/telemetry start
     * would observe a non-NULL stale pointer and short-circuit
     * (PR #630 root cause). */
    TEST_ASSERT_MESSAGE(!tcp_shell_server_running(),
        "tcp_shell_server's listen_pcb must be cleared by net_shutdown — "
        "a non-NULL value here is the dangling pointer that triggers "
        "`tcp_free: LISTEN` on the next start/stop/start cycle (PR #630)");
    TEST_ASSERT_MESSAGE(!tcp_telemetry_server_running(),
        "tcp_telemetry_server's listen_pcb must be cleared by "
        "net_shutdown for the same reason (PR #630)");

    /* Re-init the network so subsequent tests have a clean state.
     * Mirrors test_net_shutdown_then_init_succeeds's tail. */
    rc = net_init();
    TEST_ASSERT_EQUAL_INT(0, rc);
    TEST_ASSERT_TRUE(net_is_up());
}

#endif /* ENABLE_NETWORKING */

/* ============================================================================
 * Test Suite Entry Point
 * ============================================================================ */

int test_suite_net(void)
{
#if defined(ENABLE_NETWORKING)
    UNITY_BEGIN();

    /* IP address utility tests */
    RUN_TEST(test_net_ip4_addr_basic);
    RUN_TEST(test_net_ip4_addr_edge_cases);
    RUN_TEST(test_net_ip_to_str_basic);
    RUN_TEST(test_net_ip_to_str_edge_cases);
    RUN_TEST(test_net_str_to_ip_valid);
    RUN_TEST(test_net_str_to_ip_invalid);
    RUN_TEST(test_net_ip_roundtrip);

    /* Error code tests (#213) */
    RUN_TEST(test_net_strerror_coverage);
    RUN_TEST(test_net_error_codes_are_negative);

    /* Network state tests */
    RUN_TEST(test_net_is_up_before_init);
    RUN_TEST(test_net_get_info_not_initialized);
    RUN_TEST(test_net_get_info_null_pointer);
    RUN_TEST(test_net_commands_without_init);

    /* Statistics tests */
    RUN_TEST(test_net_get_stats_safety);
    RUN_TEST(test_net_watchdog_get_null_safe);
    RUN_TEST(test_net_watchdog_initial_state);
    RUN_TEST(test_net_watchdog_threshold_clamps);
    RUN_TEST(test_tcp_shell_server_get_stats_null_safe);
    RUN_TEST(test_tcp_shell_server_stats_invariants);
    RUN_TEST(test_tcp_shell_server_note_session_pair);
    RUN_TEST(test_shell_io_tcp_write_buf_timeout);
    RUN_TEST(test_shell_io_tcp_close_settling);
    RUN_TEST(test_shell_io_tcp_read_buf_drains_ring);
    RUN_TEST(test_shell_io_tcp_read_buf_handles_wrap);
    RUN_TEST(test_tcp_shell_no_false_leak_over_50_close_cycles);
    RUN_TEST(test_net_stats_initial_values);

    /* lwIP RNG / lwip_rand_seed (DTB-driven entropy seeding) */
    RUN_TEST(test_lwip_rand_seed_changes_output);
    RUN_TEST(test_lwip_rand_seed_null_or_zero_len_does_not_crash);
    RUN_TEST(test_lwip_rand_seed_different_inputs_diverge);

    /* Virtqueue descriptor ring tests (MMIO driver, QEMU_VIRT only) */
#if defined(PLATFORM_QEMU_VIRT)
    RUN_TEST(test_virtqueue_add_buf_basic);
    RUN_TEST(test_virtqueue_add_buf_read_only);
    RUN_TEST(test_virtqueue_add_buf_exhaustion);
    RUN_TEST(test_virtqueue_avail_idx_beyond_size);
    RUN_TEST(test_virtqueue_get_buf_empty);
    RUN_TEST(test_virtqueue_get_buf_returns_device_len);
    RUN_TEST(test_virtqueue_add_two_distinct_buffers);
#endif

    /* Live integration tests against QEMU's virtio-net device.
     * Order: driver_registered → init_live → poll → DHCP → driver_tx. */
    RUN_TEST(test_net_driver_registered);
    RUN_TEST(test_net_init_live);
    RUN_TEST(test_net_poll_after_init);
    RUN_TEST(test_net_auto_dhcp_at_boot);
    RUN_TEST(test_net_dhcp_binds);
    RUN_TEST(test_net_dhcp_bind_notification);
    RUN_TEST(test_net_dhcp_fallback);
    RUN_TEST(test_net_dhcp_fallback_while_link_down);
    RUN_TEST(test_net_dhcp_duplicate_request_preserves_timeout);
    RUN_TEST(test_net_boot_deferred_dhcp_waits_for_real_start);
    RUN_TEST(test_net_dhcp_start_failure_clears_pending_state);
    RUN_TEST(test_net_dhcp_link_drop_restarts_timeout);
    RUN_TEST(test_net_driver_tx);
    RUN_TEST(test_net_driver_has_tx_reap);
    /* #581 regression — runs while the live driver is up so net_poll
     * actually invokes the wrapper recv. Must be BEFORE the
     * not-initialized teardown block at the end of the suite. */
    RUN_TEST(test_net_rx_burst_uses_pbuf_pool_not_heap);
    /* #581 throughput follow-up: drain-loop semantics. Must run
     * while the live driver is up so net_poll's single invocation
     * actually reaches the wrapper recv. */
    RUN_TEST(test_net_poll_drains_all_buffered_frames);
    /* #585 follow-up: chained-pbuf assembly when frame > pool slot
     * size. Goes through net_test_inject_rx_frame to bypass the
     * 1518 B rx_packet_buf cap. */
    RUN_TEST(test_net_rx_chained_pbuf_assembly);
    RUN_TEST(test_net_send_returns_quickly);
    RUN_TEST(test_net_send_oversized_rejected);
    RUN_TEST(test_net_send_pool_exhaustion);
    RUN_TEST(test_net_burst_8_sends_async);
#if defined(PLATFORM_QEMU_VIRT)
    RUN_TEST(test_net_irq_handler_registered);
    RUN_TEST(test_net_irq_handler_drains_tx);
    RUN_TEST(test_net_mmio_watchdog_quiet);
    RUN_TEST(test_net_mmio_watchdog_fires_on_stall);
#endif
#if defined(PLATFORM_X86_64)
    RUN_TEST(test_net_msix_enabled);
    RUN_TEST(test_net_msix_vector_is_in_range);
    RUN_TEST(test_net_msix_handler_drains_tx);
    RUN_TEST(test_net_pci_watchdog_quiet);
    RUN_TEST(test_net_pci_watchdog_fires_on_stall);
#endif
    RUN_TEST(test_net_rx_no_buffers_clean);
    /* #609: net_shutdown / net_init cycle. Destructive — tears down
     * the netif and re-initializes it. Must run LAST among the
     * "live driver" tests so any subtle state-after-restart
     * differences don't leak into other tests' expectations. */
    RUN_TEST(test_net_shutdown_then_init_succeeds);
    /* PR #630: net_shutdown must clear in-tree listeners' static
     * `listen_pcb` pointers. Same destructive shape as the test
     * above — runs after it for the same reason. */
    RUN_TEST(test_net_shutdown_clears_in_tree_listeners);

    return UNITY_END();
#else
    /* Networking not available on this platform */
    return 0;
#endif
}
