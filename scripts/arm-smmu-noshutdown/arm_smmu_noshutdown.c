/*
 * arm-smmu-noshutdown — Linux kernel module that prepares the L4T
 * kexec path for a clean handoff into SLM-OS.
 *
 * Two things happen on insmod, and both are necessary for SLM-OS's
 * xHCI driver to reach `NO_OP round-trip OK` after the handoff:
 *
 *   1. Neutralise arm-smmu's platform_driver.shutdown so
 *      arm_smmu_device_shutdown() stops firing from
 *      device_shutdown() on kexec. This keeps the SMMU's clocks
 *      alive and leaves Linux's IOVA-bound context banks in place
 *      across the handoff.
 *
 *   2. Ask Linux's iommu subsystem to add an identity mapping
 *      (IOVA == PA) for SLM-OS's 2 MB NC region on the xusb
 *      stream's active iommu_domain, so SLM-OS can program DCBAAP
 *      / CRCR with raw physical addresses and have them translate
 *      straight through.
 *
 * See docs/jetson-usb-networking-plan.md §10.3 and §10.8 for the
 * full investigation, including the struct-field bug the previous
 * template had (NULLing the wrong field on device_driver) and the
 * four alternative approaches that were tested and ruled out before
 * converging on the identity-mapping design.
 *
 * Usage:
 *   insmod arm_smmu_noshutdown.ko
 *   # ... then kexec ...
 *
 * Licence: GPL v2 (kernel module requirement).
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/device.h>
#include <linux/iommu.h>
#include <linux/io.h>
#include <linux/moduleparam.h>
#include <linux/platform_device.h>
#include <linux/slab.h>
#include <linux/mm.h>
#include <linux/version.h>

#include "arm_smmu_noshutdown.h"

static bool iommu_mapping_added;
static bool slot3_devctx_alias_added;
static bool slot3_ep0_alias_added;
static u64 slot3_devctx_alias_iova;
static u64 slot3_ep0_alias_iova;
static size_t slot3_devctx_alias_size;
static size_t slot3_ep0_alias_size;
static char xhci_slot3_handoff_param[2048];

static int arm_smmu_noshutdown_get_xusb_domain(struct device **xusb_dev_out,
                                               struct iommu_domain **domain_out);
static void arm_smmu_noshutdown_unmap_slot3_aliases(struct iommu_domain *domain);
static int arm_smmu_noshutdown_install_slot3_aliases(u64 devctx_phys,
                                                     u64 ep0_deq_phys);
static int arm_smmu_noshutdown_snapshot_slot3_contexts(struct iommu_domain *domain,
                                                       u64 devctx_phys,
                                                       u32 slot_ctx_dw[4],
                                                       u32 ep_ctx_dw[SLMOS_XHCI_SLOT3_HANDOFF_CTX_DCIS]
                                                                    [SLMOS_XHCI_SLOT3_HANDOFF_EP_DWORDS]);
static int arm_smmu_noshutdown_format_slot3_payload(char *buf,
                                                    size_t buf_sz,
                                                    u64 dcbaap,
                                                    u64 devctx_phys,
                                                    u64 ep0_deq_phys,
                                                    u32 root_port,
                                                    const u32 slot_ctx_dw[4],
                                                    const u32 ep_ctx_dw[SLMOS_XHCI_SLOT3_HANDOFF_CTX_DCIS]
                                                                         [SLMOS_XHCI_SLOT3_HANDOFF_EP_DWORDS]);

static int arm_smmu_noshutdown_param_get_slot3_handoff(char *buffer,
                                                       const struct kernel_param *kp)
{
    return scnprintf(buffer, PAGE_SIZE, "%s", xhci_slot3_handoff_param);
}

static void arm_smmu_noshutdown_clear_slot3_handoff(void)
{
    void __iomem *handoff;

    handoff = ioremap_wc(SLMOS_XHCI_SLOT3_HANDOFF_PHYS,
                         sizeof(struct slmos_xhci_slot3_handoff));
    if (!handoff) {
        pr_warn("arm-smmu-noshutdown: failed to map slot3 handoff block at 0x%lx for clear\n",
                SLMOS_XHCI_SLOT3_HANDOFF_PHYS);
        return;
    }

    memset_io(handoff, 0, sizeof(struct slmos_xhci_slot3_handoff));
    wmb();
    iounmap(handoff);
}

static int arm_smmu_noshutdown_write_slot3_handoff(u64 dcbaap,
                                                   u64 devctx_phys,
                                                   u64 ep0_deq_phys,
                                                   u32 root_port,
                                                   const u32 slot_ctx_dw[4],
                                                   const u32 ep0_ctx_dw[8])
{
    struct slmos_xhci_slot3_handoff block = {
        .magic = SLMOS_XHCI_SLOT3_HANDOFF_MAGIC,
        .version = SLMOS_XHCI_SLOT3_HANDOFF_VER,
        .slot_id = 3,
        .root_port = root_port,
        .dcbaap = dcbaap,
        .devctx_phys = devctx_phys,
        .ep0_deq_phys = ep0_deq_phys,
    };
    struct slmos_xhci_slot3_handoff verify;
    void __iomem *handoff;

    memcpy(block.slot_ctx_dw, slot_ctx_dw, sizeof(block.slot_ctx_dw));
    memcpy(block.ep0_ctx_dw, ep0_ctx_dw, sizeof(block.ep0_ctx_dw));

    handoff = ioremap_wc(SLMOS_XHCI_SLOT3_HANDOFF_PHYS, sizeof(block));
    if (!handoff) {
        pr_warn("arm-smmu-noshutdown: failed to map slot3 handoff block at 0x%lx for write\n",
                SLMOS_XHCI_SLOT3_HANDOFF_PHYS);
        return -ENOMEM;
    }

    memcpy_toio(handoff, &block, sizeof(block));
    wmb();

    memset(&verify, 0, sizeof(verify));
    memcpy_fromio(&verify, handoff, sizeof(verify));
    iounmap(handoff);

    pr_info("arm-smmu-noshutdown: wrote xhci slot3 handoff dcbaap=0x%016llx devctx=0x%016llx ep0_deq=0x%016llx root=%u slot_dw3=0x%08x ep0_dw2=0x%08x ep0_dw3=0x%08x verify_slot_dw3=0x%08x verify_ep0_dw2=0x%08x verify_ep0_dw3=0x%08x\n",
            dcbaap, devctx_phys, ep0_deq_phys, root_port,
            block.slot_ctx_dw[3], block.ep0_ctx_dw[2], block.ep0_ctx_dw[3],
            verify.slot_ctx_dw[3], verify.ep0_ctx_dw[2], verify.ep0_ctx_dw[3]);
    return 0;
}

static int arm_smmu_noshutdown_snapshot_slot3_contexts(
    struct iommu_domain *domain,
    u64 devctx_phys,
    u32 slot_ctx_dw[4],
    u32 ep_ctx_dw[SLMOS_XHCI_SLOT3_HANDOFF_CTX_DCIS]
                 [SLMOS_XHCI_SLOT3_HANDOFF_EP_DWORDS])
{
    unsigned dci;
    phys_addr_t phys_page;
    void *map;
    u8 *base;

    if (!domain || !devctx_phys)
        return -EINVAL;

    phys_page = iommu_iova_to_phys(domain, devctx_phys & PAGE_MASK);
    if (!phys_page) {
        pr_warn("arm-smmu-noshutdown: iommu_iova_to_phys failed for slot3 devctx 0x%016llx\n",
                devctx_phys);
        return -ENXIO;
    }

    map = memremap(phys_page, PAGE_SIZE, MEMREMAP_WB);
    if (!map) {
        pr_warn("arm-smmu-noshutdown: memremap failed for slot3 devctx phys page 0x%llx\n",
                (u64)phys_page);
        return -ENOMEM;
    }

    base = (u8 *)map + (devctx_phys & ~PAGE_MASK);
    memcpy(slot_ctx_dw, base, sizeof(u32) * 4);
    for (dci = 0; dci < SLMOS_XHCI_SLOT3_HANDOFF_CTX_DCIS; dci++) {
        memcpy(ep_ctx_dw[dci],
               base + ((dci + 1) * 64),
               sizeof(u32) * SLMOS_XHCI_SLOT3_HANDOFF_EP_DWORDS);
    }

    memunmap(map);
    return 0;
}

static int arm_smmu_noshutdown_format_slot3_payload(
    char *buf, size_t buf_sz,
    u64 dcbaap, u64 devctx_phys, u64 ep0_deq_phys, u32 root_port,
    const u32 slot_ctx_dw[4],
    const u32 ep_ctx_dw[SLMOS_XHCI_SLOT3_HANDOFF_CTX_DCIS]
                 [SLMOS_XHCI_SLOT3_HANDOFF_EP_DWORDS])
{
    unsigned i;
    unsigned dci;
    size_t off = 0;

    if (!buf || !buf_sz)
        return -EINVAL;

    off += scnprintf(buf + off, buf_sz - off,
                     "0x%llx,0x%llx,0x%llx,%u",
                     dcbaap, devctx_phys, ep0_deq_phys, root_port);
    for (i = 0; i < 4 && off < buf_sz; i++)
        off += scnprintf(buf + off, buf_sz - off, ",0x%x", slot_ctx_dw[i]);
    for (dci = 0; dci < SLMOS_XHCI_SLOT3_HANDOFF_CTX_DCIS && off < buf_sz; dci++) {
        for (i = 0; i < SLMOS_XHCI_SLOT3_HANDOFF_EP_DWORDS && off < buf_sz; i++)
            off += scnprintf(buf + off, buf_sz - off, ",0x%x", ep_ctx_dw[dci][i]);
    }
    return (off < buf_sz) ? 0 : -ENOSPC;
}

static int arm_smmu_noshutdown_param_set_slot3_handoff(const char *val,
                                                       const struct kernel_param *kp)
{
    u64 values[16];
    u32 slot_ctx_dw[4];
    u32 ep0_ctx_dw[8];
    u32 snap_slot_ctx_dw[4] = {0};
    u32 snap_ep_ctx_dw[SLMOS_XHCI_SLOT3_HANDOFF_CTX_DCIS]
                      [SLMOS_XHCI_SLOT3_HANDOFF_EP_DWORDS] = {{0}};
    u64 dcbaap, devctx_phys, ep0_deq_phys;
    u32 root_port;
    char *dup, *p, *tok;
    struct device *xusb_dev = NULL;
    struct iommu_domain *domain = NULL;
    size_t i;
    size_t count = 0;
    int rc;

    if (sysfs_streq(val, "clear")) {
        struct device *xusb_dev;
        struct iommu_domain *domain;

        strscpy(xhci_slot3_handoff_param, "clear",
                sizeof(xhci_slot3_handoff_param));
        arm_smmu_noshutdown_clear_slot3_handoff();
        if (arm_smmu_noshutdown_get_xusb_domain(&xusb_dev, &domain) == 0) {
            arm_smmu_noshutdown_unmap_slot3_aliases(domain);
            put_device(xusb_dev);
        }
        pr_info("arm-smmu-noshutdown: cleared xhci slot3 handoff block\n");
        return 0;
    }

    dup = kstrdup(val, GFP_KERNEL);
    if (!dup)
        return -ENOMEM;

    p = dup;
    while ((tok = strsep(&p, ",")) != NULL) {
        if (*tok == '\0')
            continue;
        if (count >= ARRAY_SIZE(values)) {
            kfree(dup);
            return -EINVAL;
        }
        rc = kstrtoull(tok, 0, &values[count]);
        if (rc) {
            kfree(dup);
            return rc;
        }
        count++;
    }
    kfree(dup);

    if (count != 16)
        return -EINVAL;

    dcbaap = values[0];
    devctx_phys = values[1];
    ep0_deq_phys = values[2];
    root_port = (u32)values[3];
    for (i = 0; i < ARRAY_SIZE(slot_ctx_dw); i++)
        slot_ctx_dw[i] = (u32)values[4 + i];
    for (i = 0; i < ARRAY_SIZE(ep0_ctx_dw); i++)
        ep0_ctx_dw[i] = (u32)values[8 + i];

    rc = arm_smmu_noshutdown_install_slot3_aliases(devctx_phys, ep0_deq_phys);
    if (rc)
        return rc;

    rc = arm_smmu_noshutdown_get_xusb_domain(&xusb_dev, &domain);
    if (rc == 0) {
        rc = arm_smmu_noshutdown_snapshot_slot3_contexts(domain, devctx_phys,
                                                         snap_slot_ctx_dw,
                                                         snap_ep_ctx_dw);
        put_device(xusb_dev);
        xusb_dev = NULL;
    }
    if (rc == 0) {
        rc = arm_smmu_noshutdown_write_slot3_handoff(dcbaap, devctx_phys,
                                                     ep0_deq_phys, root_port,
                                                     snap_slot_ctx_dw,
                                                     snap_ep_ctx_dw[0]);
        if (rc)
            pr_warn("arm-smmu-noshutdown: raw slot3 handoff block write failed (%d); continuing with expanded payload only\n",
                    rc);
        rc = arm_smmu_noshutdown_format_slot3_payload(
            xhci_slot3_handoff_param, sizeof(xhci_slot3_handoff_param),
            dcbaap, devctx_phys, ep0_deq_phys, root_port,
            snap_slot_ctx_dw, snap_ep_ctx_dw);
        if (rc)
            return rc;
        pr_info("arm-smmu-noshutdown: slot3 handoff payload expanded to %u fields; ep3_dw2=0x%08x ep4_dw2=0x%08x ep5_dw2=0x%08x ep6_dw2=0x%08x ep7_dw2=0x%08x\n",
                SLMOS_XHCI_SLOT3_HANDOFF_PAYLOAD_U64S,
                snap_ep_ctx_dw[2][2], snap_ep_ctx_dw[3][2],
                snap_ep_ctx_dw[4][2], snap_ep_ctx_dw[5][2],
                snap_ep_ctx_dw[6][2]);
        return 0;
    }

    rc = arm_smmu_noshutdown_write_slot3_handoff(dcbaap, devctx_phys,
                                                 ep0_deq_phys, root_port,
                                                 slot_ctx_dw, ep0_ctx_dw);
    if (rc)
        return rc;

    strscpy(xhci_slot3_handoff_param, val, sizeof(xhci_slot3_handoff_param));
    return 0;
}

static const struct kernel_param_ops arm_smmu_noshutdown_slot3_handoff_ops = {
    .set = arm_smmu_noshutdown_param_set_slot3_handoff,
    .get = arm_smmu_noshutdown_param_get_slot3_handoff,
};

module_param_cb(xhci_slot3_handoff,
                &arm_smmu_noshutdown_slot3_handoff_ops,
                &xhci_slot3_handoff_param,
                0600);
MODULE_PARM_DESC(xhci_slot3_handoff,
                 "CSV dcbaap,devctx_phys,ep0_deq_phys,root_port for the retained xHCI slot-3 handoff block, or 'clear'");

static int arm_smmu_noshutdown_get_xusb_domain(struct device **xusb_dev_out,
                                               struct iommu_domain **domain_out)
{
    struct device *xusb_dev;
    struct iommu_domain *domain;

    xusb_dev = bus_find_device_by_name(&platform_bus_type, NULL,
                                       "3610000.usb");
    if (!xusb_dev) {
        pr_warn("arm-smmu-noshutdown: tegra-xusb 3610000.usb not found\n");
        return -ENODEV;
    }

    domain = iommu_get_domain_for_dev(xusb_dev);
    if (!domain) {
        pr_warn("arm-smmu-noshutdown: xusb has no attached iommu_domain\n");
        put_device(xusb_dev);
        return -ENODEV;
    }

    *xusb_dev_out = xusb_dev;
    *domain_out = domain;
    return 0;
}

static void arm_smmu_noshutdown_unmap_slot3_aliases(struct iommu_domain *domain)
{
    if (slot3_devctx_alias_added) {
        size_t unmapped;

        unmapped = iommu_unmap(domain, slot3_devctx_alias_iova,
                               slot3_devctx_alias_size);
        pr_info("arm-smmu-noshutdown: slot3 devctx alias unmap iova=0x%llx size=0x%zx unmapped=0x%zx\n",
                slot3_devctx_alias_iova, slot3_devctx_alias_size, unmapped);
        slot3_devctx_alias_added = false;
    }

    if (slot3_ep0_alias_added) {
        size_t unmapped;

        unmapped = iommu_unmap(domain, slot3_ep0_alias_iova,
                               slot3_ep0_alias_size);
        pr_info("arm-smmu-noshutdown: slot3 ep0 alias unmap iova=0x%llx size=0x%zx unmapped=0x%zx\n",
                slot3_ep0_alias_iova, slot3_ep0_alias_size, unmapped);
        slot3_ep0_alias_added = false;
    }
}

static int arm_smmu_noshutdown_map_slot3_alias(struct iommu_domain *domain,
                                               u64 iova,
                                               phys_addr_t phys,
                                               size_t size,
                                               const char *tag)
{
    int rc;
    size_t unmapped;

    unmapped = iommu_unmap(domain, iova, size);

    pr_info("arm-smmu-noshutdown: slot3 alias pre-unmap %s iova=0x%llx size=0x%zx unmapped=0x%zx\n",
            tag, iova, size, unmapped);

#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 0, 0)
    rc = iommu_map(domain, iova, phys, size, IOMMU_READ | IOMMU_WRITE,
                   GFP_KERNEL);
#else
    rc = iommu_map(domain, iova, phys, size, IOMMU_READ | IOMMU_WRITE);
#endif
    if (rc) {
        pr_warn("arm-smmu-noshutdown: slot3 alias map %s iova=0x%llx phys=0x%llx size=0x%zx failed: %d\n",
                tag, iova, (u64)phys, size, rc);
        return rc;
    }

    pr_info("arm-smmu-noshutdown: slot3 alias map %s iova=0x%llx -> phys=0x%llx size=0x%zx\n",
            tag, iova, (u64)phys, size);
    return 0;
}

static int arm_smmu_noshutdown_install_slot3_aliases(u64 devctx_phys,
                                                     u64 ep0_deq_phys)
{
    struct device *xusb_dev;
    struct iommu_domain *domain;
    u64 devctx_iova = devctx_phys & PAGE_MASK;
    u64 ep0_iova = ep0_deq_phys & PAGE_MASK;
    size_t ep0_size = PAGE_ALIGN((ep0_deq_phys & ~PAGE_MASK) +
                                 SLMOS_XHCI_SLOT3_EP0_RING_BYTES);
    int rc;

    rc = arm_smmu_noshutdown_get_xusb_domain(&xusb_dev, &domain);
    if (rc)
        return rc;

    arm_smmu_noshutdown_unmap_slot3_aliases(domain);

    rc = arm_smmu_noshutdown_map_slot3_alias(
        domain, devctx_iova, SLMOS_XHCI_SLOT3_DEVCXT_MIRROR_PHYS,
        PAGE_SIZE, "devctx");
    if (rc)
        goto out;
    slot3_devctx_alias_added = true;
    slot3_devctx_alias_iova = devctx_iova;
    slot3_devctx_alias_size = PAGE_SIZE;

    rc = arm_smmu_noshutdown_map_slot3_alias(
        domain, ep0_iova, SLMOS_XHCI_SLOT3_EP0_RING_PHYS,
        ep0_size, "ep0");
    if (rc)
        goto out;
    slot3_ep0_alias_added = true;
    slot3_ep0_alias_iova = ep0_iova;
    slot3_ep0_alias_size = ep0_size;

out:
    put_device(xusb_dev);
    return rc;
}

/*
 * Ask Linux's iommu subsystem to add an identity mapping for SLM-OS's
 * 2 MB NC region on the xusb stream's active domain. After this call,
 * HC DMAs the SLM-OS xHCI driver issues with IOVA == 0xBDE00000+
 * translate straight through to the same physical addresses — no
 * bypass, no context-bank replacement, no race with other live DMA
 * on this or any other SMMU instance.
 */
static int arm_smmu_noshutdown_map_slmos_region(void)
{
    struct device *xusb_dev;
    struct iommu_domain *domain;
    int rc;

    rc = arm_smmu_noshutdown_get_xusb_domain(&xusb_dev, &domain);
    if (rc)
        return rc;

    pr_info("arm-smmu-noshutdown: xusb iommu_domain type=%d "
            "(IOMMU_DOMAIN_DMA=%d, IDENTITY=%d, UNMANAGED=%d)\n",
            domain->type, IOMMU_DOMAIN_DMA, IOMMU_DOMAIN_IDENTITY,
            IOMMU_DOMAIN_UNMANAGED);

#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 0, 0)
    rc = iommu_map(domain, SLMOS_NC_BASE, SLMOS_NC_BASE, SLMOS_NC_SIZE,
                   IOMMU_READ | IOMMU_WRITE, GFP_KERNEL);
#else
    rc = iommu_map(domain, SLMOS_NC_BASE, SLMOS_NC_BASE, SLMOS_NC_SIZE,
                   IOMMU_READ | IOMMU_WRITE);
#endif
    if (rc) {
        pr_warn("arm-smmu-noshutdown: iommu_map(IOVA=0x%lx PA=0x%lx "
                "size=0x%lx) failed: %d\n",
                SLMOS_NC_BASE, SLMOS_NC_BASE, SLMOS_NC_SIZE, rc);
    } else {
        iommu_mapping_added = true;
        pr_info("arm-smmu-noshutdown: added identity IOMMU mapping "
                "IOVA 0x%lx..0x%lx (PA identical) on xusb domain "
                "(#266 Phase 3A Path 1 option 1)\n",
                SLMOS_NC_BASE, SLMOS_NC_BASE + SLMOS_NC_SIZE);
    }

    put_device(xusb_dev);
    return rc;
}

static int __init arm_smmu_noshutdown_init(void)
{
    struct device_driver *drv = driver_find("arm-smmu", &platform_bus_type);
    struct platform_driver *pdrv;

    if (!drv) {
        pr_warn("arm-smmu-noshutdown: arm-smmu driver not found — no-op\n");
        return 0;
    }

    pdrv = to_platform_driver(drv);

    if (pdrv->shutdown) {
        /*
         * Plain pointer store. Safe because platform_drv_shutdown()
         * only reads this field under device_shutdown() on the
         * reboot / kexec / poweroff path, which runs single-threaded
         * after all userspace is gone. At module-load time no kexec
         * is in flight, so no concurrent reader exists. By the time
         * one appears the new value is already in place.
         */
        pdrv->shutdown = NULL;
        pr_info("arm-smmu-noshutdown: NULLed arm-smmu "
                "platform_driver->shutdown (#266 Phase 3A) — "
                "arm_smmu_device_shutdown() will no longer fire at "
                "kexec\n");
    } else {
        pr_info("arm-smmu-noshutdown: platform_driver->shutdown "
                "already NULL — no-op\n");
    }

    /* Independent of the shutdown suppression above — if this fails
     * we still want the shutdown-suppression half of the fix to take
     * effect, and the failure mode is observable in dmesg. */
    (void)arm_smmu_noshutdown_map_slmos_region();
    arm_smmu_noshutdown_clear_slot3_handoff();

    return 0;
}

static void __exit arm_smmu_noshutdown_exit(void)
{
    /*
     * Reverse the IOMMU mapping on rmmod so a subsequent reload of
     * a rebuilt module doesn't see `iommu_map` fail with -EEXIST.
     * `platform_driver.shutdown` is deliberately NOT restored —
     * load-and-forget model; restoring the pointer would reopen the
     * exact failure we came here to prevent.
     */
    if (iommu_mapping_added) {
        struct device *xusb_dev =
            bus_find_device_by_name(&platform_bus_type, NULL,
                                     "3610000.usb");
        if (xusb_dev) {
            struct iommu_domain *domain =
                iommu_get_domain_for_dev(xusb_dev);
            if (domain) {
                size_t unmapped;

                arm_smmu_noshutdown_unmap_slot3_aliases(domain);
                unmapped = iommu_unmap(domain, SLMOS_NC_BASE,
                                       SLMOS_NC_SIZE);
                if (unmapped == SLMOS_NC_SIZE) {
                    pr_info("arm-smmu-noshutdown: iommu_unmap "
                            "returned %zu bytes (expected %lu)\n",
                            unmapped, SLMOS_NC_SIZE);
                } else {
                    /* Partial unmap leaks IOMMU page-table entries
                     * across a subsequent reload — flag loudly so a
                     * reboot can be considered before kexec. */
                    pr_warn("arm-smmu-noshutdown: iommu_unmap "
                            "returned %zu bytes (expected %lu) — "
                            "partial unmap, IOMMU state may be "
                            "inconsistent\n",
                            unmapped, SLMOS_NC_SIZE);
                }
            }
            put_device(xusb_dev);
        }
        iommu_mapping_added = false;
    }
}

module_init(arm_smmu_noshutdown_init);
module_exit(arm_smmu_noshutdown_exit);

MODULE_LICENSE("GPL v2");
MODULE_AUTHOR("SLM-OS (John Jezl)");
MODULE_DESCRIPTION("Suppress arm-smmu .shutdown and install identity "
                   "IOMMU mapping for the xusb stream across kexec "
                   "into SLM-OS. See #266, #285.");
