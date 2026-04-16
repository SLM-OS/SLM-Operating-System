/**
 * VirtIO Network Device Driver — PCI Transport (x86-64)
 *
 * Implements VirtIO-Net using PCI transport for x86-64 QEMU.
 * The virtqueue ring format (descriptor table, available ring, used ring)
 * is identical to the MMIO driver; the transport layer (device discovery,
 * configuration, notification) differs.
 *
 * VirtIO PCI uses capability structures in PCI config space to locate
 * device registers at BAR offsets. Each capability (vendor-specific,
 * cap_vndr=0x09) has a cfg_type field identifying what it provides:
 *   1 = Common configuration
 *   2 = Notifications
 *   3 = ISR status
 *   4 = Device-specific configuration
 *   5 = PCI configuration access
 *
 * Reference: VirtIO 1.1 Spec, Section 4.1 (PCI Transport)
 */

#include "platform.h"

#if defined(PLATFORM_X86_64) && defined(ENABLE_NETWORKING)

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "pci.h"
#include "net_driver.h"
#include "pmm.h"
#include "spinlock.h"
#include "debug.h"
#include <string.h>

/* -------------------------------------------------------------------------- */
/* VirtIO PCI Constants                                                        */
/* -------------------------------------------------------------------------- */

/* VirtIO vendor/device IDs */
#define VIRTIO_PCI_VENDOR       0x1AF4
#define VIRTIO_PCI_NET_LEGACY   0x1000  /* Transitional device */
#define VIRTIO_PCI_NET_MODERN   0x1041  /* Modern virtio-net */

/* VirtIO PCI capability types (cfg_type field) */
#define VIRTIO_PCI_CAP_COMMON   1
#define VIRTIO_PCI_CAP_NOTIFY   2
#define VIRTIO_PCI_CAP_ISR      3
#define VIRTIO_PCI_CAP_DEVICE   4
#define VIRTIO_PCI_CAP_PCI_CFG  5

/* Device status bits (same as MMIO) */
#define VIRTIO_STATUS_ACKNOWLEDGE   (1 << 0)
#define VIRTIO_STATUS_DRIVER        (1 << 1)
#define VIRTIO_STATUS_DRIVER_OK     (1 << 2)
#define VIRTIO_STATUS_FEATURES_OK   (1 << 3)
#define VIRTIO_STATUS_FAILED        (1 << 7)

/* VirtIO-Net feature bits */
#define VIRTIO_NET_F_MAC            (1UL << 5)
#define VIRTIO_NET_F_STATUS         (1UL << 16)
#define VIRTIO_F_VERSION_1          (1UL << 32)

#define VIRTIO_NET_FEATURES_WANT (VIRTIO_NET_F_MAC | VIRTIO_NET_F_STATUS | VIRTIO_F_VERSION_1)

/* Virtqueue descriptor flags */
#define VIRTQ_DESC_F_WRITE      (1 << 1)

/* Link status */
#define VIRTIO_NET_S_LINK_UP    (1 << 0)

/* -------------------------------------------------------------------------- */
/* VirtIO PCI Common Configuration Structure (at BAR + offset)                 */
/* -------------------------------------------------------------------------- */

struct virtio_pci_common_cfg {
    /* About the whole device */
    uint32_t device_feature_select;     /* 0x00: RW */
    uint32_t device_feature;            /* 0x04: RO */
    uint32_t driver_feature_select;     /* 0x08: RW */
    uint32_t driver_feature;            /* 0x0C: RW */
    uint16_t msix_config;               /* 0x10: RW */
    uint16_t num_queues;                /* 0x12: RO */
    uint8_t  device_status;             /* 0x14: RW */
    uint8_t  config_generation;         /* 0x15: RO */
    /* About a specific virtqueue */
    uint16_t queue_select;              /* 0x16: RW */
    uint16_t queue_size;                /* 0x18: RW */
    uint16_t queue_msix_vector;         /* 0x1A: RW */
    uint16_t queue_enable;              /* 0x1C: RW */
    uint16_t queue_notify_off;          /* 0x1E: RO */
    uint64_t queue_desc;                /* 0x20: RW */
    uint64_t queue_avail;               /* 0x28: RW (driver) */
    uint64_t queue_used;                /* 0x30: RW (device) */
} __attribute__((packed));

/* -------------------------------------------------------------------------- */
/* VirtIO-Net Device Configuration                                             */
/* -------------------------------------------------------------------------- */

struct virtio_net_config_pci {
    uint8_t  mac[6];
    uint16_t status;
    uint16_t max_virtq_pairs;
    uint16_t mtu;
} __attribute__((packed));

/* -------------------------------------------------------------------------- */
/* Virtqueue Ring Structures                                                   */
/* -------------------------------------------------------------------------- */

struct virtq_desc_pci {
    uint64_t addr;
    uint32_t len;
    uint16_t flags;
    uint16_t next;
} __attribute__((packed));

struct virtq_avail_pci {
    uint16_t flags;
    uint16_t idx;
    uint16_t ring[];
} __attribute__((packed));

struct virtq_used_elem_pci {
    uint32_t id;
    uint32_t len;
} __attribute__((packed));

struct virtq_used_pci {
    uint16_t flags;
    uint16_t idx;
    struct virtq_used_elem_pci ring[];
} __attribute__((packed));

/* VirtIO-Net header prepended to every packet */
struct virtio_net_hdr_pci {
    uint8_t  flags;
    uint8_t  gso_type;
    uint16_t hdr_len;
    uint16_t gso_size;
    uint16_t csum_start;
    uint16_t csum_offset;
} __attribute__((packed));

/* -------------------------------------------------------------------------- */
/* Driver State                                                                */
/* -------------------------------------------------------------------------- */

#define VIRTQ_SIZE          128
#define RX_BUFFER_COUNT     16
#define MAX_PACKET_SIZE     (1514 + sizeof(struct virtio_net_hdr_pci))

struct pci_virtqueue {
    uint16_t size;
    uint16_t free_head;
    uint16_t num_free;
    uint16_t last_used_idx;
    uint16_t notify_off;

    struct virtq_desc_pci  *desc;
    struct virtq_avail_pci *avail;
    struct virtq_used_pci  *used;
};

static struct {
    bool initialized;
    uint8_t bus, dev, func;

    /* Mapped BAR regions */
    volatile struct virtio_pci_common_cfg *common;
    volatile uint16_t *notify_base;
    uint32_t notify_off_multiplier;
    volatile uint32_t *isr;
    volatile struct virtio_net_config_pci *dev_cfg;

    /* Virtqueues */
    struct pci_virtqueue rx_vq;
    struct pci_virtqueue tx_vq;

    /* MAC address */
    uint8_t mac[6];
    bool link_up;
    uint64_t features;

    /* Statistics */
    uint64_t rx_packets, tx_packets;
    uint64_t rx_bytes, tx_bytes;
    uint64_t rx_errors, tx_errors;

    /* Buffers */
    spinlock_t lock;
} pci_net;

static uint8_t rx_buffers[RX_BUFFER_COUNT][MAX_PACKET_SIZE]
    __attribute__((aligned(4096)));
static uint8_t tx_buffer[MAX_PACKET_SIZE] __attribute__((aligned(16)));

/* -------------------------------------------------------------------------- */
/* Memory barrier                                                              */
/* -------------------------------------------------------------------------- */

static inline void vio_mb(void) {
    __asm__ volatile("mfence" ::: "memory");
}

/* -------------------------------------------------------------------------- */
/* PCI Capability Parsing                                                      */
/* -------------------------------------------------------------------------- */

struct virtio_pci_cap_info {
    uint8_t bar;
    uint32_t offset;
    uint32_t length;
    /* For notify cap only */
    uint32_t notify_off_multiplier;
};

static bool find_virtio_cap(uint8_t bus, uint8_t dev, uint8_t func,
                            uint8_t cfg_type, struct virtio_pci_cap_info *out)
{
    /* Read PCI status register to check capabilities list */
    uint16_t status = pci_config_read16(bus, dev, func, 0x06);
    if (!(status & (1 << 4)))  /* Capabilities List bit */
        return false;

    /* Read capabilities pointer */
    uint8_t cap_ptr = pci_config_read8(bus, dev, func, 0x34) & 0xFC;

    while (cap_ptr != 0) {
        uint8_t cap_id = pci_config_read8(bus, dev, func, cap_ptr);
        uint8_t cap_next = pci_config_read8(bus, dev, func, cap_ptr + 1);

        /* VirtIO vendor-specific capability (cap_vndr = 0x09) */
        if (cap_id == 0x09) {
            uint8_t this_type = pci_config_read8(bus, dev, func, cap_ptr + 3);
            if (this_type == cfg_type) {
                out->bar = pci_config_read8(bus, dev, func, cap_ptr + 4);
                out->offset = pci_config_read32(bus, dev, func, cap_ptr + 8);
                out->length = pci_config_read32(bus, dev, func, cap_ptr + 12);
                if (cfg_type == VIRTIO_PCI_CAP_NOTIFY) {
                    out->notify_off_multiplier =
                        pci_config_read32(bus, dev, func, cap_ptr + 16);
                }
                return true;
            }
        }

        cap_ptr = cap_next & 0xFC;
    }

    return false;
}

/* -------------------------------------------------------------------------- */
/* Virtqueue Management                                                        */
/* -------------------------------------------------------------------------- */

static size_t vq_ring_size(uint16_t qsize) {
    size_t desc_size = qsize * sizeof(struct virtq_desc_pci);
    size_t avail_size = 4 + qsize * 2 + 2;
    size_t used_offset = (desc_size + avail_size + 3) & ~3u;
    size_t used_size = 4 + qsize * sizeof(struct virtq_used_elem_pci) + 2;
    return (used_offset + used_size + 4095) & ~4095u;
}

static int vq_init(struct pci_virtqueue *vq, uint16_t index) {
    volatile struct virtio_pci_common_cfg *common = pci_net.common;

    /* Select queue */
    common->queue_select = index;
    vio_mb();

    uint16_t max_size = common->queue_size;
    if (max_size == 0) {
        ERROR("VirtIO PCI queue %u not available", index);
        return -1;
    }

    uint16_t qsize = (max_size < VIRTQ_SIZE) ? max_size : VIRTQ_SIZE;
    common->queue_size = qsize;

    /* Allocate ring memory */
    size_t mem_size = vq_ring_size(qsize);
    size_t pages = (mem_size + 4095) / 4096;
    void *mem = pmm_alloc_pages(pages);
    if (!mem) {
        ERROR("Failed to allocate virtqueue memory");
        return -1;
    }
    memset(mem, 0, mem_size);

    vq->size = qsize;
    vq->desc = (struct virtq_desc_pci *)mem;

    size_t desc_size = qsize * sizeof(struct virtq_desc_pci);
    vq->avail = (struct virtq_avail_pci *)((uint8_t *)mem + desc_size);

    size_t avail_size = 4 + qsize * 2 + 2;
    size_t used_offset = (desc_size + avail_size + 3) & ~3u;
    vq->used = (struct virtq_used_pci *)((uint8_t *)mem + used_offset);

    /* Initialize free list */
    vq->free_head = 0;
    vq->num_free = qsize;
    vq->last_used_idx = 0;
    for (uint16_t i = 0; i < qsize - 1; i++)
        vq->desc[i].next = i + 1;

    /* Tell device about queue addresses */
    common->queue_desc  = (uint64_t)(uintptr_t)vq->desc;
    common->queue_avail = (uint64_t)(uintptr_t)vq->avail;
    common->queue_used  = (uint64_t)(uintptr_t)vq->used;

    /* Read notify offset for this queue */
    vq->notify_off = common->queue_notify_off;

    /* Disable MSI-X for this queue (use polling) */
    common->queue_msix_vector = 0xFFFF;

    /* Enable the queue */
    common->queue_enable = 1;
    vio_mb();

    INFO("VirtIO PCI queue %u: size=%u notify_off=%u", index, qsize, vq->notify_off);
    return 0;
}

static int vq_alloc_desc(struct pci_virtqueue *vq) {
    if (vq->num_free == 0)
        return -1;
    int idx = vq->free_head;
    vq->free_head = vq->desc[idx].next;
    vq->num_free--;
    return idx;
}

static void vq_free_desc(struct pci_virtqueue *vq, int idx) {
    vq->desc[idx].next = vq->free_head;
    vq->free_head = idx;
    vq->num_free++;
}

static int vq_add_buf(struct pci_virtqueue *vq, void *addr, uint32_t len,
                      bool write) {
    int idx = vq_alloc_desc(vq);
    if (idx < 0)
        return -1;

    vq->desc[idx].addr = (uint64_t)(uintptr_t)addr;
    vq->desc[idx].len = len;
    vq->desc[idx].flags = write ? VIRTQ_DESC_F_WRITE : 0;
    vq->desc[idx].next = 0;

    uint16_t avail_idx = vq->avail->idx % vq->size;
    vq->avail->ring[avail_idx] = idx;
    vio_mb();
    vq->avail->idx++;
    vio_mb();

    return idx;
}

static int vq_get_buf(struct pci_virtqueue *vq, uint32_t *len) {
    vio_mb();
    if (vq->last_used_idx == vq->used->idx)
        return -1;

    uint16_t used_idx = vq->last_used_idx % vq->size;
    uint32_t desc_idx = vq->used->ring[used_idx].id;
    if (len)
        *len = vq->used->ring[used_idx].len;

    vq_free_desc(vq, desc_idx);
    vq->last_used_idx++;
    return desc_idx;
}

static void vq_kick(struct pci_virtqueue *vq) {
    vio_mb();
    /* Write to the notify register for this queue */
    volatile uint16_t *notify_addr = (volatile uint16_t *)(
        (uint8_t *)pci_net.notify_base +
        vq->notify_off * pci_net.notify_off_multiplier);
    /* For the notification, we write the queue index (0 for RX, 1 for TX).
     * But the spec says we write the virtqueue index, which can be derived
     * from the queue. For simplicity, use avail->idx. Actually, for legacy
     * and modern VirtIO, we just need to write to the notify address. The
     * value written is the queue index. */
    *notify_addr = vq == &pci_net.rx_vq ? 0 : 1;
}

/* -------------------------------------------------------------------------- */
/* RX Buffer Management                                                        */
/* -------------------------------------------------------------------------- */

static void post_rx_buffers(void) {
    for (int i = 0; i < RX_BUFFER_COUNT; i++) {
        int ret = vq_add_buf(&pci_net.rx_vq, rx_buffers[i],
                             MAX_PACKET_SIZE, true);
        if (ret < 0) {
            WARN("Failed to post RX buffer %d", i);
            break;
        }
    }
    vq_kick(&pci_net.rx_vq);
}

/* -------------------------------------------------------------------------- */
/* Driver API                                                                  */
/* -------------------------------------------------------------------------- */

static int virtio_net_pci_init(void) {
    if (pci_net.initialized)
        return 0;

    INFO("Initializing VirtIO-Net PCI driver...");

    /* Find VirtIO network device */
    const struct pci_device *pdev = pci_find_device(VIRTIO_PCI_VENDOR,
                                                     VIRTIO_PCI_NET_MODERN);
    if (!pdev)
        pdev = pci_find_device(VIRTIO_PCI_VENDOR, VIRTIO_PCI_NET_LEGACY);
    if (!pdev) {
        /* Also try class-based search: Ethernet controller (02:00) with VirtIO vendor */
        for (uint32_t i = 0; i < pci_get_device_count(); i++) {
            const struct pci_device *d = pci_get_device(i);
            if (d && d->vendor_id == VIRTIO_PCI_VENDOR &&
                d->class_code == 0x02 && d->subclass == 0x00) {
                pdev = d;
                break;
            }
        }
    }
    if (!pdev) {
        ERROR("VirtIO-Net PCI device not found");
        return -1;
    }

    pci_net.bus = pdev->bus;
    pci_net.dev = pdev->dev;
    pci_net.func = pdev->func;
    INFO("VirtIO-Net PCI at %02x:%02x.%x (device 0x%04x)",
         pdev->bus, pdev->dev, pdev->func, pdev->device_id);

    /* Enable bus mastering and memory space */
    uint16_t cmd = pci_config_read16(pdev->bus, pdev->dev, pdev->func, 0x04);
    if (!(cmd & 0x06)) {
        cmd |= 0x06;
        pci_config_write32(pdev->bus, pdev->dev, pdev->func, 0x04, cmd);
    }

    /* Find VirtIO capability structures */
    struct virtio_pci_cap_info cap_common, cap_notify, cap_isr, cap_device;

    if (!find_virtio_cap(pdev->bus, pdev->dev, pdev->func,
                         VIRTIO_PCI_CAP_COMMON, &cap_common)) {
        ERROR("Missing VirtIO common config capability");
        return -1;
    }
    if (!find_virtio_cap(pdev->bus, pdev->dev, pdev->func,
                         VIRTIO_PCI_CAP_NOTIFY, &cap_notify)) {
        ERROR("Missing VirtIO notify capability");
        return -1;
    }
    if (!find_virtio_cap(pdev->bus, pdev->dev, pdev->func,
                         VIRTIO_PCI_CAP_ISR, &cap_isr)) {
        ERROR("Missing VirtIO ISR capability");
        return -1;
    }
    bool has_dev_cfg = find_virtio_cap(pdev->bus, pdev->dev, pdev->func,
                                        VIRTIO_PCI_CAP_DEVICE, &cap_device);

    /* Map BAR regions — on x86-64 with identity mapping, BAR addr = virtual addr */
    uint64_t bar_addr = pdev->bar[cap_common.bar] & ~0xFUL;
    if ((pdev->bar[cap_common.bar] & 0x6) == 0x4 && cap_common.bar < 5)
        bar_addr |= (uint64_t)pdev->bar[cap_common.bar + 1] << 32;

    pci_net.common = (volatile struct virtio_pci_common_cfg *)
        (uintptr_t)(bar_addr + cap_common.offset);

    uint64_t notify_bar = pdev->bar[cap_notify.bar] & ~0xFUL;
    if ((pdev->bar[cap_notify.bar] & 0x6) == 0x4 && cap_notify.bar < 5)
        notify_bar |= (uint64_t)pdev->bar[cap_notify.bar + 1] << 32;
    pci_net.notify_base = (volatile uint16_t *)
        (uintptr_t)(notify_bar + cap_notify.offset);
    pci_net.notify_off_multiplier = cap_notify.notify_off_multiplier;

    uint64_t isr_bar = pdev->bar[cap_isr.bar] & ~0xFUL;
    if ((pdev->bar[cap_isr.bar] & 0x6) == 0x4 && cap_isr.bar < 5)
        isr_bar |= (uint64_t)pdev->bar[cap_isr.bar + 1] << 32;
    pci_net.isr = (volatile uint32_t *)
        (uintptr_t)(isr_bar + cap_isr.offset);

    if (has_dev_cfg) {
        uint64_t dev_bar = pdev->bar[cap_device.bar] & ~0xFUL;
        if ((pdev->bar[cap_device.bar] & 0x6) == 0x4 && cap_device.bar < 5)
            dev_bar |= (uint64_t)pdev->bar[cap_device.bar + 1] << 32;
        pci_net.dev_cfg = (volatile struct virtio_net_config_pci *)
            (uintptr_t)(dev_bar + cap_device.offset);
    }

    INFO("Common cfg BAR%u+0x%x, Notify BAR%u+0x%x (mult=%u)",
         cap_common.bar, cap_common.offset,
         cap_notify.bar, cap_notify.offset,
         pci_net.notify_off_multiplier);

    /* Reset device */
    pci_net.common->device_status = 0;
    vio_mb();

    /* Acknowledge */
    pci_net.common->device_status = VIRTIO_STATUS_ACKNOWLEDGE;
    vio_mb();

    /* Driver */
    pci_net.common->device_status |= VIRTIO_STATUS_DRIVER;
    vio_mb();

    /* Feature negotiation */
    pci_net.common->device_feature_select = 0;
    vio_mb();
    uint32_t feat_lo = pci_net.common->device_feature;
    pci_net.common->device_feature_select = 1;
    vio_mb();
    uint32_t feat_hi = pci_net.common->device_feature;

    uint64_t device_features = ((uint64_t)feat_hi << 32) | feat_lo;
    uint64_t driver_features = device_features & VIRTIO_NET_FEATURES_WANT;
    pci_net.features = driver_features;

    INFO("Device features: 0x%llx, negotiated: 0x%llx",
         (unsigned long long)device_features,
         (unsigned long long)driver_features);

    pci_net.common->driver_feature_select = 0;
    vio_mb();
    pci_net.common->driver_feature = (uint32_t)driver_features;
    pci_net.common->driver_feature_select = 1;
    vio_mb();
    pci_net.common->driver_feature = (uint32_t)(driver_features >> 32);
    vio_mb();

    /* Features OK */
    pci_net.common->device_status |= VIRTIO_STATUS_FEATURES_OK;
    vio_mb();

    if (!(pci_net.common->device_status & VIRTIO_STATUS_FEATURES_OK)) {
        ERROR("Device did not accept features");
        pci_net.common->device_status = VIRTIO_STATUS_FAILED;
        return -1;
    }

    /* Disable MSI-X for config changes (use polling) */
    pci_net.common->msix_config = 0xFFFF;

    /* Initialize virtqueues (0=RX, 1=TX) */
    if (vq_init(&pci_net.rx_vq, 0) < 0) {
        ERROR("Failed to init RX queue");
        pci_net.common->device_status = VIRTIO_STATUS_FAILED;
        return -1;
    }
    if (vq_init(&pci_net.tx_vq, 1) < 0) {
        ERROR("Failed to init TX queue");
        pci_net.common->device_status = VIRTIO_STATUS_FAILED;
        return -1;
    }

    /* Read MAC address */
    if ((pci_net.features & VIRTIO_NET_F_MAC) && pci_net.dev_cfg) {
        for (int i = 0; i < 6; i++)
            pci_net.mac[i] = pci_net.dev_cfg->mac[i];
    } else {
        pci_net.mac[0] = 0x52; pci_net.mac[1] = 0x54;
        pci_net.mac[2] = 0x00; pci_net.mac[3] = 0x12;
        pci_net.mac[4] = 0x34; pci_net.mac[5] = 0x56;
    }
    INFO("MAC: %02x:%02x:%02x:%02x:%02x:%02x",
         pci_net.mac[0], pci_net.mac[1], pci_net.mac[2],
         pci_net.mac[3], pci_net.mac[4], pci_net.mac[5]);

    /* Driver OK */
    pci_net.common->device_status |= VIRTIO_STATUS_DRIVER_OK;
    vio_mb();

    /* Check link status */
    if ((pci_net.features & VIRTIO_NET_F_STATUS) && pci_net.dev_cfg) {
        pci_net.link_up = (pci_net.dev_cfg->status & VIRTIO_NET_S_LINK_UP) != 0;
    } else {
        pci_net.link_up = true;
    }
    INFO("Link status: %s", pci_net.link_up ? "UP" : "DOWN");

    /* Post RX buffers */
    post_rx_buffers();

    spin_init(&pci_net.lock);
    pci_net.initialized = true;
    INFO("VirtIO-Net PCI driver initialized");
    return 0;
}

static int virtio_net_pci_send(const void *data, size_t len) {
    if (!pci_net.initialized)
        return -1;
    if (len > 1514)
        return -1;

    irq_flags_t flags = spin_lock_irqsave(&pci_net.lock);

    /* Prepend virtio-net header */
    struct virtio_net_hdr_pci *hdr = (struct virtio_net_hdr_pci *)tx_buffer;
    memset(hdr, 0, sizeof(*hdr));
    memcpy(tx_buffer + sizeof(*hdr), data, len);

    uint32_t total = sizeof(*hdr) + (uint32_t)len;
    int desc_idx = vq_add_buf(&pci_net.tx_vq, tx_buffer, total, false);
    if (desc_idx < 0) {
        pci_net.tx_errors++;
        spin_unlock_irqrestore(&pci_net.lock, flags);
        return -1;
    }

    vq_kick(&pci_net.tx_vq);

    /* Synchronous TX: wait for completion */
    int timeout = 100000;
    while (timeout > 0) {
        uint32_t used_len;
        if (vq_get_buf(&pci_net.tx_vq, &used_len) >= 0)
            break;
        timeout--;
    }

    if (timeout == 0) {
        pci_net.tx_errors++;
        spin_unlock_irqrestore(&pci_net.lock, flags);
        return -1;
    }

    pci_net.tx_packets++;
    pci_net.tx_bytes += len;
    spin_unlock_irqrestore(&pci_net.lock, flags);
    return 0;
}

static int virtio_net_pci_recv(void *buffer, size_t max_len) {
    if (!pci_net.initialized)
        return -1;

    irq_flags_t flags = spin_lock_irqsave(&pci_net.lock);

    uint32_t used_len;
    int desc_idx = vq_get_buf(&pci_net.rx_vq, &used_len);
    if (desc_idx < 0) {
        spin_unlock_irqrestore(&pci_net.lock, flags);
        return 0;
    }

    uint8_t *rx_buf = (uint8_t *)(uintptr_t)pci_net.rx_vq.desc[desc_idx].addr;
    uint32_t pkt_len = used_len - sizeof(struct virtio_net_hdr_pci);
    uint8_t *pkt_data = rx_buf + sizeof(struct virtio_net_hdr_pci);

    if (pkt_len > (uint32_t)max_len) {
        pci_net.rx_errors++;
        pkt_len = (uint32_t)max_len;
    }

    memcpy(buffer, pkt_data, pkt_len);

    /* Repost buffer */
    vq_add_buf(&pci_net.rx_vq, rx_buf, MAX_PACKET_SIZE, true);
    vq_kick(&pci_net.rx_vq);

    pci_net.rx_packets++;
    pci_net.rx_bytes += pkt_len;
    spin_unlock_irqrestore(&pci_net.lock, flags);
    return pkt_len;
}

static void virtio_net_pci_get_mac(uint8_t mac[6]) {
    if (pci_net.initialized)
        memcpy(mac, pci_net.mac, 6);
    else
        memset(mac, 0, 6);
}

static bool virtio_net_pci_link_status(void) {
    return pci_net.initialized && pci_net.link_up;
}

/* -------------------------------------------------------------------------- */
/* net_driver Registration                                                     */
/* -------------------------------------------------------------------------- */

static const struct net_driver virtio_net_pci_driver = {
    .name        = "virtio-net-pci",
    .init        = virtio_net_pci_init,
    .send        = virtio_net_pci_send,
    .recv        = virtio_net_pci_recv,
    .get_mac     = virtio_net_pci_get_mac,
    .link_status = virtio_net_pci_link_status,
};

void virtio_net_pci_register(void) {
    net_register_driver(&virtio_net_pci_driver);
}

#endif /* PLATFORM_X86_64 && ENABLE_NETWORKING */
