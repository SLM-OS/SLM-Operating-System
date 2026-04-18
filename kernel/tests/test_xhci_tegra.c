/*
 * test_xhci_tegra.c — Tegra234 IFR bringup unit tests (#266 Phase 3A.2)
 *
 * The wrapper MMIO accessors in xhci.c (fpci_r32/w32, bar2_r32/w32,
 * bar2_csb_r32/w32, tegra_xusb_config, etc.) are Jetson-only — they
 * can't be unit-tested in QEMU because they poke real hardware.
 *
 * What CAN be unit-tested is the pure-logic code underneath:
 *
 *   * CSB paging math — `xusb_csb_page_select` and
 *     `xusb_csb_page_offset` decompose a 32-bit CSB address into a
 *     23-bit page number and 9-bit page offset. These are inline
 *     static helpers in xhci_tegra.h, platform-neutral, exercised
 *     for every CSB access on Tegra234.
 *
 *   * Register-offset constants — Linux-reference values must not
 *     drift. Catching drift at unit-test time means a fresh
 *     investigator following §10 of jetson-usb-networking-plan.md
 *     doesn't waste a hardware iteration on a typo.
 *
 * The Linux reference line-numbers quoted throughout point at
 * `docs/reference/linux-xhci-tegra.c`. Keep the citations in sync
 * with any update to that file.
 */

#include "unity.h"
#include "test_harness.h"
#include "../drivers/usb/xhci/xhci_tegra.h"

#include <stdint.h>

/* -------------------------------------------------------------------------- */
/* CSB paging math                                                             */
/* -------------------------------------------------------------------------- */

static void test_csb_page_math_zero(void)
{
    /* CSB offset 0 — page = 0, offset = 0. Trivial but worth pinning
     * because many ports confuse "shift by 9" with "divide by 9" or
     * similar. */
    TEST_ASSERT_EQUAL_UINT32(0, xusb_csb_page_select(0x0));
    TEST_ASSERT_EQUAL_UINT32(0, xusb_csb_page_offset(0x0));
}

static void test_csb_page_math_falc_cpuctl(void)
{
    /* XUSB_FALC_CPUCTL = 0x100 — first page, offset 0x100 within
     * the page. Page-select must be 0. */
    TEST_ASSERT_EQUAL_UINT32(0, xusb_csb_page_select(XUSB_FALC_CPUCTL));
    TEST_ASSERT_EQUAL_UINT32(0x100, xusb_csb_page_offset(XUSB_FALC_CPUCTL));
}

static void test_csb_page_math_mp_apmap(void)
{
    /* XUSB_CSB_MP_APMAP = 0x10181c — page 0x80c, offset 0x01c.
     * This is the register that proved CSB paging control works
     * in task 3A.2.3 (the readback matched 0x80c). */
    TEST_ASSERT_EQUAL_UINT32(0x80c, xusb_csb_page_select(XUSB_CSB_MP_APMAP));
    TEST_ASSERT_EQUAL_UINT32(0x01c, xusb_csb_page_offset(XUSB_CSB_MP_APMAP));
}

static void test_csb_page_math_mp_iload_base_hi(void)
{
    /* XUSB_CSB_MP_ILOAD_BASE_HI = 0x101a08 — page 0x80d, offset
     * 0x008. Tests a different page from APMAP to make sure the
     * page arithmetic isn't accidentally fixed to one value. */
    TEST_ASSERT_EQUAL_UINT32(0x80d, xusb_csb_page_select(XUSB_CSB_MP_ILOAD_BASE_HI));
    TEST_ASSERT_EQUAL_UINT32(0x008, xusb_csb_page_offset(XUSB_CSB_MP_ILOAD_BASE_HI));
}

static void test_csb_page_math_aru_scratch0(void)
{
    /* XUSB_CSB_ARU_SCRATCH0 = 0x100100 — page 0x800, offset 0x100.
     * The interesting corner: page 0x800 and offset 0x100 together
     * don't alias to any other (page, offset) pair. */
    TEST_ASSERT_EQUAL_UINT32(0x800, xusb_csb_page_select(XUSB_CSB_ARU_SCRATCH0));
    TEST_ASSERT_EQUAL_UINT32(0x100, xusb_csb_page_offset(XUSB_CSB_ARU_SCRATCH0));
}

static void test_csb_page_math_roundtrip_boundaries(void)
{
    /*
     * Round-trip property: reconstructing the full address from
     * (page << 9) | offset must recover the original. Walk the
     * interesting boundary values:
     *
     *   0x1ff — last offset in page 0. Page=0, offset=0x1ff.
     *   0x200 — first offset in page 1. Page=1, offset=0x0.
     *   0xffffffff — full 32-bit all-ones. Page=0x7fffff (mask),
     *                offset=0x1ff (mask). Used to verify the mask
     *                constants are right.
     */
    uint32_t test_vals[] = {0x1ff, 0x200, 0x400, 0x100000, 0x101a04, 0x10181c, 0xffffffff};
    for (unsigned i = 0; i < sizeof(test_vals) / sizeof(test_vals[0]); i++) {
        uint32_t addr = test_vals[i];
        uint32_t page = xusb_csb_page_select(addr);
        uint32_t ofs  = xusb_csb_page_offset(addr);
        uint32_t rebuilt = (page << XUSB_CSB_PAGE_SELECT_SHIFT) | ofs;
        TEST_ASSERT_EQUAL_UINT32(addr, rebuilt);
    }
}

static void test_csb_page_math_masks_are_sane(void)
{
    /* CSB_PAGE_SELECT_SHIFT = 9 (offset is bits [8:0]).
     * CSB_PAGE_SELECT_MASK covers 23 bits — bits [31:9] of the full
     * address fit into 23 bits because the full CSB space is 32
     * bits and we're carving off 9 for the offset.
     *
     * CSB_PAGE_OFFSET_MASK covers 9 bits — bits [8:0].
     *
     * If these masks drift, CSB reads silently go to the wrong
     * register with no hardware fault. Unit-test them directly. */
    TEST_ASSERT_EQUAL_UINT32(9, XUSB_CSB_PAGE_SELECT_SHIFT);
    TEST_ASSERT_EQUAL_UINT32(0x7fffff, XUSB_CSB_PAGE_SELECT_MASK);
    TEST_ASSERT_EQUAL_UINT32(0x1ff, XUSB_CSB_PAGE_OFFSET_MASK);
}

/* -------------------------------------------------------------------------- */
/* FPCI + BAR2 register offset pinning                                         */
/* -------------------------------------------------------------------------- */

static void test_fpci_cfg_offsets_match_linux(void)
{
    /*
     * FPCI config-register offsets quoted from linux-xhci-tegra.c:
     * CFG_1 is at 0x004 (PCI command register — bus-master + I/O +
     * mem enables); CFG_4 is at 0x010 (BAR0); CFG_7 is at 0x01c
     * (BAR2, Tegra234-only). The CSB-paging control register lives
     * at 0x41c in the FPCI window (XUSB_CFG_ARU_C11_CSBRANGE), and
     * the CSB data window base is 0x800.
     *
     * These are copied verbatim from the Linux source. A typo that
     * swaps CFG_4 and CFG_7, for instance, would cause wrapper
     * programming to point the Falcon at the wrong aperture — and
     * since Linux often leaves the state workable, the bug could
     * silently mask itself until a future kexec variant exposed it.
     */
    TEST_ASSERT_EQUAL_UINT32(0x004, XUSB_CFG_1);
    TEST_ASSERT_EQUAL_UINT32(0x010, XUSB_CFG_4);
    TEST_ASSERT_EQUAL_UINT32(0x01c, XUSB_CFG_7);
    TEST_ASSERT_EQUAL_UINT32(0x41c, XUSB_CFG_ARU_C11_CSBRANGE);
    TEST_ASSERT_EQUAL_UINT32(0x800, XUSB_CFG_CSB_BASE_ADDR);
}

static void test_fpci_cfg1_bus_master_bit(void)
{
    /*
     * The three enables in XUSB_CFG_1 are tied to specific bit
     * positions — CFG_1 is a PCI-like command register.
     * First-hardware observation this session recorded
     * CFG_1 = 0x00b00007, which decomposes as:
     *   bit 0 = IO_SPACE_EN
     *   bit 1 = MEM_SPACE_EN
     *   bit 2 = BUS_MASTER_EN
     * (high byte 0x0b is a PCI status-register shadow, irrelevant
     * here).
     */
    TEST_ASSERT_EQUAL_UINT32(1u << 0, XUSB_IO_SPACE_EN);
    TEST_ASSERT_EQUAL_UINT32(1u << 1, XUSB_MEM_SPACE_EN);
    TEST_ASSERT_EQUAL_UINT32(1u << 2, XUSB_BUS_MASTER_EN);
}

static void test_fpci_bar_address_masks(void)
{
    /*
     * BAR0 is bits [31:15] (17 bits) of CFG_4 — 32 KB alignment.
     * BAR2 is bits [31:16] (16 bits) of CFG_7 — 64 KB alignment.
     * The BAR0 decoder on Tegra234 hardwires bit 16, so writing
     * TEGRA_XHCI_HCD_BASE (0x03610000) is silently rejected; Linux
     * uses TEGRA_XHCI_FPCI_BASE (0x03600000). See §9.2 of the plan.
     */
    TEST_ASSERT_EQUAL_UINT32(15, XUSB_BASE_ADDR_SHIFT);
    TEST_ASSERT_EQUAL_UINT32(0x1ffff, XUSB_BASE_ADDR_MASK);
    TEST_ASSERT_EQUAL_UINT32(16, XUSB_BASE2_ADDR_SHIFT);
    TEST_ASSERT_EQUAL_UINT32(0xffff, XUSB_BASE2_ADDR_MASK);
}

static void test_bar2_offsets_match_linux(void)
{
    /*
     * BAR2 wrapper registers, per linux-xhci-tegra.c:82-94. The
     * mailbox and IFR scratch registers are the ones most likely
     * to get typo'd since they're dense in one 0x20-byte cluster.
     *
     * CAUTION: XUSB_BAR2_ARU_FW_SCRATCH (0x1000) is the register
     * that triggered TF-A RAS on write during task 3A.2.5. If this
     * constant is ever accidentally pointed at a different offset,
     * this test does not catch the RAS hazard — it only catches
     * drift from the Linux reference.
     */
    TEST_ASSERT_EQUAL_UINT32(0x004, XUSB_BAR2_ARU_MBOX_CMD);
    TEST_ASSERT_EQUAL_UINT32(0x008, XUSB_BAR2_ARU_MBOX_DATA_IN);
    TEST_ASSERT_EQUAL_UINT32(0x00c, XUSB_BAR2_ARU_MBOX_DATA_OUT);
    TEST_ASSERT_EQUAL_UINT32(0x010, XUSB_BAR2_ARU_MBOX_OWNER);
    TEST_ASSERT_EQUAL_UINT32(0x01c, XUSB_BAR2_ARU_SMI_ARU_FW_SCRATCH_DATA0);
    TEST_ASSERT_EQUAL_UINT32(0x09c, XUSB_BAR2_ARU_C11_CSBRANGE);
    TEST_ASSERT_EQUAL_UINT32(0x1000, XUSB_BAR2_ARU_FW_SCRATCH);
    TEST_ASSERT_EQUAL_UINT32(0x2000, XUSB_BAR2_CSB_BASE_ADDR);
}

static void test_falcon_csb_offsets_match_linux(void)
{
    /*
     * Falcon CSB register offsets, per linux-xhci-tegra.c:118-151.
     * These go through the CSB paging window so they're never
     * written directly — but the offsets still matter because
     * xusb_csb_page_select/offset derive from them.
     */
    TEST_ASSERT_EQUAL_UINT32(0x100, XUSB_FALC_CPUCTL);
    TEST_ASSERT_EQUAL_UINT32(0x104, XUSB_FALC_BOOTVEC);
    TEST_ASSERT_EQUAL_UINT32(0x10c, XUSB_FALC_DMACTL);
    TEST_ASSERT_EQUAL_UINT32(0x100100, XUSB_CSB_ARU_SCRATCH0);
    TEST_ASSERT_EQUAL_UINT32(0x101a04, XUSB_CSB_MP_ILOAD_BASE_LO);
    TEST_ASSERT_EQUAL_UINT32(0x101a08, XUSB_CSB_MP_ILOAD_BASE_HI);
    TEST_ASSERT_EQUAL_UINT32(0x10181c, XUSB_CSB_MP_APMAP);
}

static void test_falcon_cpuctl_bits(void)
{
    /*
     * FALC_CPUCTL bit positions per xHCI Falcon ISA: STARTCPU
     * (bit 1) is the software-asserted "begin execution" bit;
     * STATE_HALTED (bit 4) means the Falcon hit a HALT instruction;
     * STATE_STOPPED (bit 5) means the Falcon is in the STOPPED
     * state. Pinned because any future experiment that writes
     * CPUCTL via CSB (e.g. if SMMU work in §10 doesn't pan out
     * and Falcon re-kick becomes interesting) must not land on a
     * reserved bit.
     */
    TEST_ASSERT_EQUAL_UINT32(1u << 1, XUSB_FALC_CPUCTL_STARTCPU);
    TEST_ASSERT_EQUAL_UINT32(1u << 4, XUSB_FALC_CPUCTL_STATE_HALTED);
    TEST_ASSERT_EQUAL_UINT32(1u << 5, XUSB_FALC_CPUCTL_STATE_STOPPED);
}

static void test_fw_header_created_time_offset(void)
{
    /*
     * XUSB_FW_HDR_FWIMG_CREATED_TIME_OFF — byte offset of
     * fwimg_created_time within Linux's tegra_xusb_fw_header struct.
     * Verified by counting fields against the struct definition
     * at linux-xhci-tegra.c:160-192. If the struct ever changes
     * upstream and this offset drifts, the IFR-mailbox probe in
     * §9.2 / task 3A.2.5 would read the wrong header field — so
     * we pin the current value even though the mailbox path itself
     * is currently unsafe to call.
     */
    TEST_ASSERT_EQUAL_UINT32(44, XUSB_FW_HDR_FWIMG_CREATED_TIME_OFF);
}

static void test_fw_ioctl_shift(void)
{
    /* IOCTL-type field sits in bits [31:24] of the FW_SCRATCH
     * command word. Shift must be 24; command encoding must match
     * Linux's FW_IOCTL_CFGTBL_READ = 17. */
    TEST_ASSERT_EQUAL_UINT32(24, XUSB_FW_IOCTL_TYPE_SHIFT);
    TEST_ASSERT_EQUAL_UINT32(17, XUSB_FW_IOCTL_CFGTBL_READ);
}

/* -------------------------------------------------------------------------- */
/* Suite entry                                                                 */
/* -------------------------------------------------------------------------- */

int test_suite_xhci_tegra(void);

int test_suite_xhci_tegra(void)
{
    UnityBegin("test_xhci_tegra.c");
    RUN_TEST(test_csb_page_math_zero);
    RUN_TEST(test_csb_page_math_falc_cpuctl);
    RUN_TEST(test_csb_page_math_mp_apmap);
    RUN_TEST(test_csb_page_math_mp_iload_base_hi);
    RUN_TEST(test_csb_page_math_aru_scratch0);
    RUN_TEST(test_csb_page_math_roundtrip_boundaries);
    RUN_TEST(test_csb_page_math_masks_are_sane);
    RUN_TEST(test_fpci_cfg_offsets_match_linux);
    RUN_TEST(test_fpci_cfg1_bus_master_bit);
    RUN_TEST(test_fpci_bar_address_masks);
    RUN_TEST(test_bar2_offsets_match_linux);
    RUN_TEST(test_falcon_csb_offsets_match_linux);
    RUN_TEST(test_falcon_cpuctl_bits);
    RUN_TEST(test_fw_header_created_time_offset);
    RUN_TEST(test_fw_ioctl_shift);
    return UnityEnd();
}
