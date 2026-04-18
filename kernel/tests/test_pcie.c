/*
 * test_pcie.c — Unit tests for the PCIe host-controller subsystem.
 *
 * These tests run on QEMU virt (ARM64) with the `qemu-gpex` backend.
 * The Makefile's `make test` adds `-device virtio-rng-pci,bus=pcie.0`
 * to the QEMU command line, so at least one endpoint is present
 * by the time pcie_init() scans bus 0.
 *
 * Coverage:
 *   - The backend registered and enumeration ran without fault.
 *   - Device count matches expectation (≥ 1 on QEMU_VIRT).
 *   - The virtio-rng endpoint (vendor 0x1AF4) is findable by both
 *     vendor-ID lookup and class code (non-standard: 0xFF).
 *   - BAR size-probe produced a non-zero, power-of-two region.
 *   - Capability-list walk finds the MSI-X capability (virtio PCI
 *     devices always expose one).
 *   - pcie_map_bar returns a non-NULL VA that's reachable.
 *
 * On non-QEMU platforms this suite runs but reports 0 assertions —
 * the backend is either not installed (Jetson stub) or hasn't been
 * exercised with a known test device (Pi 5 — real AI HAT+).
 */

#include "unity.h"
#include "../include/pcie.h"
#include "../include/uart.h"
#include "test_harness.h"
#include <stdbool.h>

/* PCI vendor for QEMU's virtio devices. */
#define VIRTIO_VENDOR_ID      0x1AF4

/* Capability IDs from the PCI spec. */
#define PCI_CAP_ID_MSI        0x05
#define PCI_CAP_ID_MSIX       0x11

#if defined(PLATFORM_QEMU_VIRT)

static const struct pcie_device *find_test_endpoint(void)
{
    uint32_t n = pcie_get_device_count();
    for (uint32_t i = 0; i < n; i++) {
        const struct pcie_device *d = pcie_get_device(i);
        if (d && d->vendor_id == VIRTIO_VENDOR_ID) {
            return d;
        }
    }
    return NULL;
}

/* Find the lowest-index BAR that is (a) present and (b) a memory
 * BAR, not an I/O BAR. Transitional virtio-rng-pci exposes BAR0 as
 * legacy I/O and BAR4 as the modern memory capability window — the
 * test scans until it hits the memory one. Returns -1 if none. */
static int find_mem_bar(const struct pcie_device *d)
{
    for (int b = 0; b < PCIE_NUM_BARS; b++) {
        if ((d->bar_flags[b] & PCIE_BAR_PRESENT)
         && !(d->bar_flags[b] & PCIE_BAR_IO)) {
            return b;
        }
    }
    return -1;
}

static void test_backend_registered(void)
{
    /* pcie_init ran at boot; device count is 0 only if the backend
     * failed or no endpoints were attached. With the Makefile's
     * -device virtio-rng-pci,bus=pcie.0, at least one must be here. */
    TEST_ASSERT_TRUE(pcie_get_device_count() >= 1);
}

static void test_find_device_by_vendor(void)
{
    const struct pcie_device *d = find_test_endpoint();
    TEST_ASSERT_NOT_NULL(d);
    TEST_ASSERT_EQUAL_HEX16(VIRTIO_VENDOR_ID, d->vendor_id);
}

static void test_memory_bar_present_and_sized(void)
{
    const struct pcie_device *d = find_test_endpoint();
    TEST_ASSERT_NOT_NULL(d);

    /* virtio-rng-pci's transitional mode puts legacy I/O in BAR0
     * and the modern config window in BAR4. The subsystem doesn't
     * care which BAR number — just that the size-probe succeeded
     * for some memory BAR. */
    int bar = find_mem_bar(d);
    TEST_ASSERT_TRUE(bar >= 0);
    TEST_ASSERT_TRUE(d->bar_size[bar] > 0);

    /* All PCIe memory BARs are power-of-two sized. */
    uint64_t size = d->bar_size[bar];
    TEST_ASSERT_EQUAL_UINT64(0, size & (size - 1));
}

static void test_capability_list_walk(void)
{
    const struct pcie_device *d = find_test_endpoint();
    TEST_ASSERT_NOT_NULL(d);

    /* Modern virtio PCI devices expose an MSI-X capability. A zero
     * return would mean the capability pointer was 0 or the walk
     * failed — either way, something wrong with the backend's
     * config-space access. */
    uint8_t msix_cap = pcie_find_capability(d, PCI_CAP_ID_MSIX);
    TEST_ASSERT_TRUE(msix_cap >= 0x40);
}

static void test_map_bar_returns_va(void)
{
    const struct pcie_device *d = find_test_endpoint();
    TEST_ASSERT_NOT_NULL(d);

    int bar = find_mem_bar(d);
    TEST_ASSERT_TRUE(bar >= 0);

    /*
     * Without UEFI firmware in the QEMU invocation, BAR addresses
     * are left at the hardware-reset value of 0 — QEMU does not
     * auto-assign. On real platforms (Pi 5 with VideoCore, x86 with
     * SeaBIOS) firmware programs BARs before the kernel boots.
     *
     * The point of this test is to exercise the backend's map_bar
     * path, not to perform BAR resource assignment. So if the BAR
     * address is 0, assert the backend correctly returns NULL for
     * that unmapped case; if the BAR is programmed, assert we get
     * a valid mapping back. Either branch proves the code path is
     * alive.
     */
    uint64_t size = 0;
    void *va = pcie_map_bar(d, (uint8_t)bar, &size);
    if (d->bar[bar] == 0) {
        /* QEMU-virt-sans-UEFI case: NULL is the right answer. */
        TEST_ASSERT_NULL(va);
    } else {
        TEST_ASSERT_NOT_NULL(va);
        TEST_ASSERT_TRUE(size >= d->bar_size[bar]);
        /* Probe the first dword and, if the BAR is larger than one
         * dword, the last aligned dword too. A single read at VA+0
         * proves the head of the mapping is reachable but wouldn't
         * catch a backend that truncates the mapping to less than
         * the BAR's advertised size. 0xFFFFFFFFu signals an
         * unmapped / unresponsive region. */
        volatile uint32_t probe_head = *(volatile uint32_t *)va;
        TEST_ASSERT_TRUE(probe_head != 0xFFFFFFFFu);
        if (size >= sizeof(uint32_t) * 2) {
            uintptr_t tail_off = (uintptr_t)(size - sizeof(uint32_t));
            volatile uint32_t probe_tail =
                *(volatile uint32_t *)((uint8_t *)va + tail_off);
            TEST_ASSERT_TRUE(probe_tail != 0xFFFFFFFFu);
        }
    }
}

static void test_config_read_consistency(void)
{
    const struct pcie_device *d = find_test_endpoint();
    TEST_ASSERT_NOT_NULL(d);

    /* The 32-bit read at offset 0 must equal vendor|device<<16. */
    uint32_t vid_did = pcie_config_read32(d, 0);
    TEST_ASSERT_EQUAL_HEX16(d->vendor_id, (uint16_t)(vid_did & 0xFFFFu));
    TEST_ASSERT_EQUAL_HEX16(d->device_id, (uint16_t)(vid_did >> 16));

    /* 16-bit read of the same fields must match the 32-bit halves. */
    TEST_ASSERT_EQUAL_HEX16(d->vendor_id, pcie_config_read16(d, 0x00));
    TEST_ASSERT_EQUAL_HEX16(d->device_id, pcie_config_read16(d, 0x02));

    /* 8-bit read of class code (byte offset 0x0B) matches cached. */
    TEST_ASSERT_EQUAL_HEX8(d->class_code, pcie_config_read8(d, 0x0B));
}

static void test_bus_master_toggle(void)
{
    const struct pcie_device *d = find_test_endpoint();
    TEST_ASSERT_NOT_NULL(d);

    int rc = pcie_enable_bus_master(d);
    TEST_ASSERT_EQUAL_INT(PCIE_OK, rc);

    /* Verify COMMAND register shows MEM + BUS_MASTER set. */
    uint16_t cmd = pcie_config_read16(d, 0x04);
    TEST_ASSERT_TRUE((cmd & (1u << 1)) != 0);  /* MEM SPACE */
    TEST_ASSERT_TRUE((cmd & (1u << 2)) != 0);  /* BUS MASTER */
}

/*
 * pcie_core_register_host validates the full vtable. Previously
 * only config_read32 and config_write32 were checked, so a backend
 * that forgot init() or map_bar() would crash on first use rather
 * than fail loudly at registration.
 *
 * These tests call pcie_core_register_host with deliberately
 * incomplete op tables and assert PCIE_ERR_INVAL. The already-
 * registered GPEX backend is not disturbed because the validation
 * fails before host_ops is replaced.
 *
 * pcie_core_register_host is declared in pcie.h under a
 * "backend-only" comment — device drivers never call it.
 */

static int      dummy_init(void) { return 0; }
static bool     dummy_link_up(void) { return true; }
static uint8_t  dummy_r8 (uint8_t b, uint8_t d, uint8_t f, uint16_t o)
    { (void)b; (void)d; (void)f; (void)o; return 0; }
static uint16_t dummy_r16(uint8_t b, uint8_t d, uint8_t f, uint16_t o)
    { (void)b; (void)d; (void)f; (void)o; return 0; }
static uint32_t dummy_r32(uint8_t b, uint8_t d, uint8_t f, uint16_t o)
    { (void)b; (void)d; (void)f; (void)o; return 0; }
static void     dummy_w32(uint8_t b, uint8_t d, uint8_t f, uint16_t o, uint32_t v)
    { (void)b; (void)d; (void)f; (void)o; (void)v; }
static void    *dummy_map(uint64_t a, uint64_t s)
    { (void)a; (void)s; return NULL; }

static void test_register_host_rejects_null_ops(void)
{
    TEST_ASSERT_EQUAL_INT(PCIE_ERR_INVAL, pcie_core_register_host(NULL));
}

static void test_register_host_rejects_missing_init(void)
{
    struct pcie_host_ops partial = {
        .name = "partial-no-init",
        .init = NULL,   /* MISSING */
        .link_up = dummy_link_up,
        .config_read8 = dummy_r8, .config_read16 = dummy_r16,
        .config_read32 = dummy_r32, .config_write32 = dummy_w32,
        .map_bar = dummy_map,
    };
    TEST_ASSERT_EQUAL_INT(PCIE_ERR_INVAL,
                          pcie_core_register_host(&partial));
}

static void test_register_host_rejects_missing_map_bar(void)
{
    struct pcie_host_ops partial = {
        .name = "partial-no-map-bar",
        .init = dummy_init, .link_up = dummy_link_up,
        .config_read8 = dummy_r8, .config_read16 = dummy_r16,
        .config_read32 = dummy_r32, .config_write32 = dummy_w32,
        .map_bar = NULL,   /* MISSING */
    };
    TEST_ASSERT_EQUAL_INT(PCIE_ERR_INVAL,
                          pcie_core_register_host(&partial));
}

#endif /* PLATFORM_QEMU_VIRT */

/* -------------------------------------------------------------------------- */
/* pcie_place_bar — bump-allocator math used by pcie_init's BAR-assignment   */
/* pass. These tests exercise the pure function directly; they're platform- */
/* agnostic and don't depend on a live backend, so they run everywhere.    */
/* -------------------------------------------------------------------------- */

static void test_place_bar_first_fit(void)
{
    /* Typical: window starts aligned, size divides evenly, no padding. */
    uint64_t next = 0x1b80000000ULL;
    uint64_t end  = next + 0x80000000ULL;
    uint64_t addr = 0;
    TEST_ASSERT_TRUE(pcie_place_bar(&next, end, 0x10000ULL, &addr));
    TEST_ASSERT_EQUAL_HEX64(0x1b80000000ULL, addr);
    TEST_ASSERT_EQUAL_HEX64(0x1b80010000ULL, next);
}

static void test_place_bar_aligns_up(void)
{
    /* next is misaligned for the requested BAR size — the helper must
     * round up. 64 KB BAR placed after a 4 KB bump needs to start
     * at 0x...10000, not 0x...01000. */
    uint64_t next = 0x1b80001000ULL;
    uint64_t end  = 0x1b80100000ULL;
    uint64_t addr = 0;
    TEST_ASSERT_TRUE(pcie_place_bar(&next, end, 0x10000ULL, &addr));
    TEST_ASSERT_EQUAL_HEX64(0x1b80010000ULL, addr);  /* aligned up */
    TEST_ASSERT_EQUAL_HEX64(0x1b80020000ULL, next);
}

static void test_place_bar_rejects_overflow(void)
{
    /* Window only has 0x4000 bytes left but we ask for 0x10000. */
    uint64_t next = 0x1bffffc000ULL;
    uint64_t end  = 0x1c00000000ULL;
    uint64_t addr = 0xdeadbeefULL;
    TEST_ASSERT_FALSE(pcie_place_bar(&next, end, 0x10000ULL, &addr));
    /* On failure, *next and *out_addr must not be clobbered with a
     * partially-committed state. Pin both. */
    TEST_ASSERT_EQUAL_HEX64(0x1bffffc000ULL, next);
    TEST_ASSERT_EQUAL_HEX64(0xdeadbeefULL,   addr);
}

static void test_place_bar_zero_size_rejected(void)
{
    uint64_t next = 0x1000ULL;
    uint64_t end  = 0x10000ULL;
    uint64_t addr = 0;
    TEST_ASSERT_FALSE(pcie_place_bar(&next, end, 0, &addr));
}

static void test_place_bar_rejects_high_address_wrap(void)
{
    /* Pathological: window ends at UINT64_MAX and requested BAR size
     * would overflow `aligned + size`. Must not silently succeed with
     * a wrapped address. */
    uint64_t next = 0xFFFFFFFFFFFF0000ULL;
    uint64_t end  = 0xFFFFFFFFFFFFFFFFULL;
    uint64_t addr = 0;
    TEST_ASSERT_FALSE(pcie_place_bar(&next, end, 0x100000ULL, &addr));
}

static void test_place_bar_sequential_packing(void)
{
    /* Three BARs of different sizes packed into a window. Each one
     * should end up naturally aligned to its own size. */
    uint64_t next = 0x1b80000000ULL;
    uint64_t end  = 0x1b80100000ULL;
    uint64_t a0 = 0, a1 = 0, a2 = 0;

    TEST_ASSERT_TRUE(pcie_place_bar(&next, end, 0x1000ULL,  &a0));
    TEST_ASSERT_TRUE(pcie_place_bar(&next, end, 0x10000ULL, &a1));
    TEST_ASSERT_TRUE(pcie_place_bar(&next, end, 0x4000ULL,  &a2));

    TEST_ASSERT_EQUAL_HEX64(0x1b80000000ULL, a0);
    TEST_ASSERT_EQUAL_HEX64(0x1b80010000ULL, a1);   /* 64 KB aligned */
    TEST_ASSERT_EQUAL_HEX64(0x1b80020000ULL, a2);   /* 16 KB aligned */
}

int test_suite_pcie(void)
{
    UnityBegin("PCIe host controller");

#if defined(PLATFORM_QEMU_VIRT)
    pcie_dump_devices();

    RUN_TEST(test_backend_registered);
    RUN_TEST(test_find_device_by_vendor);
    RUN_TEST(test_memory_bar_present_and_sized);
    RUN_TEST(test_capability_list_walk);
    RUN_TEST(test_map_bar_returns_va);
    RUN_TEST(test_config_read_consistency);
    RUN_TEST(test_bus_master_toggle);
    RUN_TEST(test_register_host_rejects_null_ops);
    RUN_TEST(test_register_host_rejects_missing_init);
    RUN_TEST(test_register_host_rejects_missing_map_bar);
#else
    /* Non-QEMU platforms: no known test endpoint is attached. The
     * backend is still exercised by pcie_init() at boot; dedicated
     * coverage on Pi 5 / Jetson lands with Phase 3 (Hailo driver). */
#endif

    /* pcie_place_bar is a pure function — runs on every platform. */
    RUN_TEST(test_place_bar_first_fit);
    RUN_TEST(test_place_bar_aligns_up);
    RUN_TEST(test_place_bar_rejects_overflow);
    RUN_TEST(test_place_bar_zero_size_rejected);
    RUN_TEST(test_place_bar_rejects_high_address_wrap);
    RUN_TEST(test_place_bar_sequential_packing);

    return UnityEnd();
}
