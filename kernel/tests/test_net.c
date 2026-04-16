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

    /* QEMU SLIRP default lease is 10.0.2.15 */
    TEST_ASSERT_EQUAL_HEX32(net_ip4_addr(10, 0, 2, 15), info.ip_addr);
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
static void test_net_driver_tx(void)
{
    if (!net_is_up()) {
        TEST_IGNORE_MESSAGE("network not initialized");
        return;
    }

    const struct net_driver *drv = net_get_driver();
    TEST_ASSERT_NOT_NULL(drv);

    uint8_t frame[64] = {0};
    /* Destination MAC: broadcast */
    for (int i = 0; i < 6; i++) frame[i] = 0xFF;
    /* Source MAC: the driver's */
    drv->get_mac(&frame[6]);
    /* EtherType: 0x9000 (Loopback) — recognized but unused by SLIRP */
    frame[12] = 0x90;
    frame[13] = 0x00;
    /* Remaining 50 bytes are zero payload */

    int ret = drv->send(frame, sizeof(frame));
    TEST_ASSERT_MESSAGE(ret == 0, "driver send() should succeed");
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
    RUN_TEST(test_net_stats_initial_values);

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
    RUN_TEST(test_net_driver_tx);
    RUN_TEST(test_net_rx_no_buffers_clean);

    return UNITY_END();
#else
    /* Networking not available on this platform */
    return 0;
#endif
}
