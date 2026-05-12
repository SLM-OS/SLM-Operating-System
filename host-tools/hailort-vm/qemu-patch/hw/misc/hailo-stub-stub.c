/*
 * Hailo stub-stub QEMU PCI device.
 *
 * Placeholder for Task 0.3 of the Hailo BAR4 RE plan (issue #795). Exposes
 * the Hailo-8 PCI IDs (0x1e60:0x2864) and three BARs (0/2/4) so the upstream
 * `hailo_pci` kernel driver can probe the device. All reads return 0; all
 * writes are discarded. The point is solely to let HailoRT inside the VM
 * complete device-open and begin issuing MMIO, which QEMU's
 * memory_region_ops_{read,write} trace events capture.
 *
 * NOT a faithful Hailo-8 emulation. Throwaway — Task 0.2 (`hailo-re/0.2-qemu-stub`)
 * replaces this with a real-protocol stub backed by the corpus file.
 *
 * BAR layout chosen to match the Linux driver's expectations
 * (see ~/slmos-ref/hailo/v4.23.0/common/pcie_common.h):
 *   BAR0 — config window           (4 KB)
 *   BAR2 — VDMA registers          (16 KB)
 *   BAR4 — firmware-access window  (1 MB)
 *
 * BAR sizes are conservative lower bounds — enough that pci_resource_len()
 * returns non-zero. Real Hailo-8 BARs are larger; the placeholder returns
 * zero outside any meaningful range anyway.
 */

#include "qemu/osdep.h"
#include "qapi/error.h"
#include "hw/pci/pci_device.h"
#include "hw/pci/msi.h"
#include "qemu/module.h"
#include "qom/object.h"

#define TYPE_HAILO_STUB_STUB "hailo-stub-stub"
typedef struct HailoStubStubState HailoStubStubState;
DECLARE_INSTANCE_CHECKER(HailoStubStubState, HAILO_STUB_STUB, TYPE_HAILO_STUB_STUB)

#define HAILO_VENDOR_ID  0x1e60
#define HAILO_DEVICE_ID  0x2864
#define HAILO_BAR0_SIZE  (4   * 1024)
#define HAILO_BAR2_SIZE  (16  * 1024)
#define HAILO_BAR4_SIZE  (1   * 1024 * 1024)

struct HailoStubStubState {
    PCIDevice parent_obj;
    MemoryRegion bar0;
    MemoryRegion bar2;
    MemoryRegion bar4;
};

/*
 * Probe-time magic register whitelist on BAR0. Two reads must succeed for
 * `hailo_pci`'s probe to reach the `device_create_with_groups` call that
 * creates /dev/hailo0:
 *
 * 1. hailo_pcie_is_device_connected() reads BAR0 + 0x0098 (16-bit)
 *    and expects PCI_VENDOR_ID_HAILO (0x1e60).
 *    (~/slmos-ref/hailo/v4.23.0/common/pcie_common.c:51, 884)
 *
 * 2. hailo_pcie_is_firmware_loaded(), for the Hailo-8 board path, reads
 *    BAR0 + ATR_PCIE_BRIDGE_OFFSET(0) + offsetof(atr_trsl_addr_1) (32-bit)
 *    = 0x700 + 8 = 0x708, and expects PCIE_CONTROL_SECTION_ADDRESS_H8
 *    (0x60000000) so it can short-circuit firmware loading.
 *    (~/slmos-ref/hailo/v4.23.0/common/pcie_common.c:817-821)
 *
 *    The short-circuit only triggers when the module is loaded with
 *    `support_soft_reset=0`. 10-bind-stub.sh sets that.
 *
 * Without these, probe bails with "Failed reading device BARs, device may
 * be disconnected" (1) or hangs on a 5-second firmware-load timeout (2).
 * The corpus produced past these reads is HailoRT's actual MMIO sequence
 * once it opens /dev/hailo0 — the DoD's "first BAR read" target.
 *
 * Resist extending this whitelist beyond what probe demands. Anything more
 * is Task 0.2's surface.
 */
#define PCIE_CONFIG_VENDOR_OFFSET           0x0098
#define PCI_VENDOR_ID_HAILO                 0x1e60
#define HAILO_BAR0_FW_LOADED_OFFSET         0x0708
#define PCIE_CONTROL_SECTION_ADDRESS_H8     0x60000000

static uint64_t hailo_stub_bar0_read(void *opaque, hwaddr addr, unsigned size)
{
    /*
     * The Hailo driver's `hailo_resource_read16` macro expands to a 32-bit
     * MMIO read on x86 (the kernel masks the high 16 bits). So observed
     * `size` here is 4 for the "_read16" case. Match both 2 and 4 for the
     * vendor magic; the high 16 bits of a 32-bit response are always 0.
     */
    if (addr == PCIE_CONFIG_VENDOR_OFFSET && (size == 2 || size == 4)) {
        return PCI_VENDOR_ID_HAILO;
    }
    if (addr == HAILO_BAR0_FW_LOADED_OFFSET && size == 4) {
        return PCIE_CONTROL_SECTION_ADDRESS_H8;
    }
    return 0;
}

static uint64_t hailo_stub_read(void *opaque, hwaddr addr, unsigned size)
{
    return 0;
}

static void hailo_stub_write(void *opaque, hwaddr addr, uint64_t val,
                             unsigned size)
{
    /* Writes are discarded. The interesting work happens in QEMU's
     * memory_region_ops_write trace event, which fires before this callback. */
}

static const MemoryRegionOps hailo_stub_bar0_ops = {
    .read       = hailo_stub_bar0_read,
    .write      = hailo_stub_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid      = { .min_access_size = 1, .max_access_size = 8 },
    .impl       = { .min_access_size = 1, .max_access_size = 8 },
};

static const MemoryRegionOps hailo_stub_ops = {
    .read       = hailo_stub_read,
    .write      = hailo_stub_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid      = { .min_access_size = 1, .max_access_size = 8 },
    .impl       = { .min_access_size = 1, .max_access_size = 8 },
};

static void hailo_stub_realize(PCIDevice *pdev, Error **errp)
{
    HailoStubStubState *s = HAILO_STUB_STUB(pdev);
    uint8_t *pci_conf = pdev->config;
    Error *local_err = NULL;

    /* Advertise INTA so the kernel's PCI subsystem won't reject the device
     * even when it falls back from MSI; the stub never asserts the line. */
    pci_conf[PCI_INTERRUPT_PIN] = 1;

    memory_region_init_io(&s->bar0, OBJECT(s), &hailo_stub_bar0_ops, s,
                          "hailo-stub-bar0", HAILO_BAR0_SIZE);
    memory_region_init_io(&s->bar2, OBJECT(s), &hailo_stub_ops, s,
                          "hailo-stub-bar2", HAILO_BAR2_SIZE);
    memory_region_init_io(&s->bar4, OBJECT(s), &hailo_stub_ops, s,
                          "hailo-stub-bar4", HAILO_BAR4_SIZE);

    pci_register_bar(pdev, 0,
                     PCI_BASE_ADDRESS_SPACE_MEMORY |
                     PCI_BASE_ADDRESS_MEM_TYPE_64,
                     &s->bar0);
    pci_register_bar(pdev, 2,
                     PCI_BASE_ADDRESS_SPACE_MEMORY |
                     PCI_BASE_ADDRESS_MEM_TYPE_64,
                     &s->bar2);
    pci_register_bar(pdev, 4,
                     PCI_BASE_ADDRESS_SPACE_MEMORY |
                     PCI_BASE_ADDRESS_MEM_TYPE_64,
                     &s->bar4);

    /* Single MSI vector so the guest driver can register an IRQ handler.
     * The stub never raises MSIs; Task 0.2 will. msi_init failure is
     * non-fatal (the kernel driver tolerates absence of MSI), so discard
     * the Error via local_err rather than leaking it through *errp. */
    if (msi_init(pdev, 0, 1, true, false, &local_err) < 0) {
        error_free(local_err);
    }
}

static void hailo_stub_class_init(ObjectClass *class, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(class);
    PCIDeviceClass *k = PCI_DEVICE_CLASS(class);

    k->realize     = hailo_stub_realize;
    k->vendor_id   = HAILO_VENDOR_ID;
    k->device_id   = HAILO_DEVICE_ID;
    k->revision    = 0;
    k->class_id    = PCI_CLASS_PROCESSOR_CO;

    set_bit(DEVICE_CATEGORY_MISC, dc->categories);
    dc->desc = "Hailo-8 stub-stub for RE capture";
}

static const TypeInfo hailo_stub_info = {
    .name          = TYPE_HAILO_STUB_STUB,
    .parent        = TYPE_PCI_DEVICE,
    .instance_size = sizeof(HailoStubStubState),
    .class_init    = hailo_stub_class_init,
    .interfaces    = (InterfaceInfo[]) {
        { INTERFACE_CONVENTIONAL_PCI_DEVICE },
        { },
    },
};

static void hailo_stub_register_types(void)
{
    type_register_static(&hailo_stub_info);
}

type_init(hailo_stub_register_types)
