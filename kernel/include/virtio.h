/**
 * VirtIO Common Definitions for SLM-OS
 *
 * This file defines the VirtIO MMIO transport interface as specified in
 * the VirtIO 1.1 specification. These definitions are shared by all VirtIO
 * device drivers (network, block, etc.).
 *
 * Reference: https://docs.oasis-open.org/virtio/virtio/v1.1/virtio-v1.1.pdf
 */

#ifndef VIRTIO_H
#define VIRTIO_H

#include <stdint.h>
#include <stdbool.h>
#include "platform.h"

/* -------------------------------------------------------------------------- */
/* VirtIO MMIO Transport (Legacy and Modern)                                   */
/* -------------------------------------------------------------------------- */

/*
 * QEMU virt machine VirtIO MMIO layout:
 * - Base address: 0x0A000000
 * - Each device: 0x200 bytes
 * - Device N at: 0x0A000000 + N * 0x200
 * - IRQ for device N: SPI (16 + N) = 32 + 16 + N = 48 + N
 */
#ifndef VIRTIO_MMIO_BASE
#define VIRTIO_MMIO_BASE        0x0A000000UL
#endif

#ifndef VIRTIO_MMIO_SIZE
#define VIRTIO_MMIO_SIZE        0x200UL
#endif

/* Calculate device base address from slot number */
#define VIRTIO_DEVICE_BASE(slot) (VIRTIO_MMIO_BASE + (slot) * VIRTIO_MMIO_SIZE)

/* Calculate IRQ number from slot number */
#define VIRTIO_DEVICE_IRQ(slot)  (48 + (slot))  /* SPI 16 + slot */

/* -------------------------------------------------------------------------- */
/* VirtIO MMIO Registers (offset from device base)                             */
/* -------------------------------------------------------------------------- */

/* Magic value for VirtIO MMIO */
#define VIRTIO_MMIO_MAGIC_VALUE     0x74726976  /* "virt" in little-endian */

/* Register offsets (VirtIO 1.0+ / MMIO) */
#define VIRTIO_MMIO_MAGIC           0x000   /* R: Magic value "virt" */
#define VIRTIO_MMIO_VERSION         0x004   /* R: Device version (1 or 2) */
#define VIRTIO_MMIO_DEVICE_ID       0x008   /* R: Device type ID */
#define VIRTIO_MMIO_VENDOR_ID       0x00C   /* R: Vendor ID */

#define VIRTIO_MMIO_DEVICE_FEATURES 0x010   /* R: Device feature bits (select) */
#define VIRTIO_MMIO_DEVICE_FEATURES_SEL 0x014 /* W: Select device feature word */
#define VIRTIO_MMIO_DRIVER_FEATURES 0x020   /* W: Driver feature bits (accept) */
#define VIRTIO_MMIO_DRIVER_FEATURES_SEL 0x024 /* W: Select driver feature word */

#define VIRTIO_MMIO_QUEUE_SEL       0x030   /* W: Select virtqueue index */
#define VIRTIO_MMIO_QUEUE_NUM_MAX   0x034   /* R: Max queue size for selected */
#define VIRTIO_MMIO_QUEUE_NUM       0x038   /* W: Queue size for selected */
#define VIRTIO_MMIO_QUEUE_READY     0x044   /* RW: Queue ready (modern) */
#define VIRTIO_MMIO_QUEUE_NOTIFY    0x050   /* W: Queue notification */

#define VIRTIO_MMIO_INTERRUPT_STATUS 0x060  /* R: Interrupt status */
#define VIRTIO_MMIO_INTERRUPT_ACK   0x064   /* W: Acknowledge interrupt */

#define VIRTIO_MMIO_STATUS          0x070   /* RW: Device status */

/* VirtIO 1.0+ queue descriptor addresses (64-bit, split into low/high) */
#define VIRTIO_MMIO_QUEUE_DESC_LOW  0x080   /* W: Descriptor table addr [31:0] */
#define VIRTIO_MMIO_QUEUE_DESC_HIGH 0x084   /* W: Descriptor table addr [63:32] */
#define VIRTIO_MMIO_QUEUE_AVAIL_LOW 0x090   /* W: Available ring addr [31:0] */
#define VIRTIO_MMIO_QUEUE_AVAIL_HIGH 0x094  /* W: Available ring addr [63:32] */
#define VIRTIO_MMIO_QUEUE_USED_LOW  0x0A0   /* W: Used ring addr [31:0] */
#define VIRTIO_MMIO_QUEUE_USED_HIGH 0x0A4   /* W: Used ring addr [63:32] */

/* Legacy (version 1) register offsets - still supported */
#define VIRTIO_MMIO_GUEST_PAGE_SIZE 0x028   /* W: Guest page size (legacy) */
#define VIRTIO_MMIO_QUEUE_PFN       0x040   /* RW: Queue PFN (legacy) */

/* Device-specific config space starts at offset 0x100 */
#define VIRTIO_MMIO_CONFIG          0x100

/* -------------------------------------------------------------------------- */
/* Device Status Bits                                                          */
/* -------------------------------------------------------------------------- */

#define VIRTIO_STATUS_ACKNOWLEDGE   (1 << 0)  /* Driver noticed device */
#define VIRTIO_STATUS_DRIVER        (1 << 1)  /* Driver knows how to drive */
#define VIRTIO_STATUS_DRIVER_OK     (1 << 2)  /* Driver setup complete */
#define VIRTIO_STATUS_FEATURES_OK   (1 << 3)  /* Feature negotiation done */
#define VIRTIO_STATUS_DEVICE_NEEDS_RESET (1 << 6) /* Device error, needs reset */
#define VIRTIO_STATUS_FAILED        (1 << 7)  /* Driver gave up */

/* -------------------------------------------------------------------------- */
/* Device Type IDs                                                             */
/* -------------------------------------------------------------------------- */

#define VIRTIO_DEVICE_NET           1   /* Network device */
#define VIRTIO_DEVICE_BLOCK         2   /* Block device */
#define VIRTIO_DEVICE_CONSOLE       3   /* Console */
#define VIRTIO_DEVICE_ENTROPY       4   /* Entropy source */
#define VIRTIO_DEVICE_BALLOON       5   /* Memory balloon */
#define VIRTIO_DEVICE_IOMEM         6   /* ioMemory */
#define VIRTIO_DEVICE_RPMSG         7   /* rpmsg */
#define VIRTIO_DEVICE_SCSI          8   /* SCSI host */
#define VIRTIO_DEVICE_9P            9   /* 9P transport */
#define VIRTIO_DEVICE_GPU           16  /* GPU device */
#define VIRTIO_DEVICE_INPUT         18  /* Input device */
#define VIRTIO_DEVICE_SOCKET        19  /* Socket device */

/* -------------------------------------------------------------------------- */
/* Interrupt Status Bits                                                       */
/* -------------------------------------------------------------------------- */

#define VIRTIO_IRQ_USED_BUFFER      (1 << 0)  /* Used buffer notification */
#define VIRTIO_IRQ_CONFIG_CHANGE    (1 << 1)  /* Config space changed */

/* -------------------------------------------------------------------------- */
/* Virtqueue Descriptor Structure                                              */
/* -------------------------------------------------------------------------- */

/* Descriptor flags */
#define VIRTQ_DESC_F_NEXT           (1 << 0)  /* More descriptors follow */
#define VIRTQ_DESC_F_WRITE          (1 << 1)  /* Device writes (vs reads) */
#define VIRTQ_DESC_F_INDIRECT       (1 << 2)  /* Contains list of descs */

/* Single descriptor in the descriptor table */
struct virtq_desc {
    uint64_t addr;      /* Physical address of buffer */
    uint32_t len;       /* Length in bytes */
    uint16_t flags;     /* VIRTQ_DESC_F_* flags */
    uint16_t next;      /* Index of next descriptor if NEXT set */
} __attribute__((packed));

/* Available ring - driver writes here to offer buffers to device */
struct virtq_avail {
    uint16_t flags;     /* VIRTQ_AVAIL_F_* */
    uint16_t idx;       /* Next ring entry to use */
    uint16_t ring[];    /* Descriptor indices (variable length) */
    /* Followed by: uint16_t used_event; if VIRTIO_F_EVENT_IDX */
} __attribute__((packed));

#define VIRTQ_AVAIL_F_NO_INTERRUPT  (1 << 0)  /* Don't interrupt */

/* Used ring element - returned by device */
struct virtq_used_elem {
    uint32_t id;        /* Descriptor index (start of chain) */
    uint32_t len;       /* Total bytes written by device */
} __attribute__((packed));

/* Used ring - device writes here to return buffers */
struct virtq_used {
    uint16_t flags;     /* VIRTQ_USED_F_* */
    uint16_t idx;       /* Next ring entry to use */
    struct virtq_used_elem ring[];  /* Elements (variable length) */
    /* Followed by: uint16_t avail_event; if VIRTIO_F_EVENT_IDX */
} __attribute__((packed));

#define VIRTQ_USED_F_NO_NOTIFY      (1 << 0)  /* Don't notify */

/* -------------------------------------------------------------------------- */
/* Virtqueue Management Structure                                              */
/* -------------------------------------------------------------------------- */

/* Maximum queue size (common default) */
#define VIRTQ_MAX_SIZE              256

/* Complete virtqueue state */
struct virtqueue {
    /* Queue index (0, 1, etc. for this device) */
    uint16_t index;

    /* Queue size (power of 2, <= QUEUE_NUM_MAX) */
    uint16_t size;

    /* Next descriptor index to allocate */
    uint16_t free_head;

    /* Number of free descriptors */
    uint16_t num_free;

    /* Last seen used->idx */
    uint16_t last_used_idx;

    /* Device base address for MMIO */
    volatile uint32_t *regs;

    /* Descriptor table */
    struct virtq_desc *desc;

    /* Available ring */
    struct virtq_avail *avail;

    /* Used ring */
    struct virtq_used *used;
};

/* -------------------------------------------------------------------------- */
/* VirtIO Register Access Helpers                                              */
/* -------------------------------------------------------------------------- */

/* Read a 32-bit MMIO register */
static inline uint32_t virtio_read32(uintptr_t base, uint32_t offset) {
    return *(volatile uint32_t *)(base + offset);
}

/* Write a 32-bit MMIO register */
static inline void virtio_write32(uintptr_t base, uint32_t offset, uint32_t value) {
    *(volatile uint32_t *)(base + offset) = value;
}

/* Memory barrier for MMIO ordering */
static inline void virtio_mb(void) {
#if defined(PLATFORM_X86_64)
    __asm__ volatile("mfence" ::: "memory");
#else
    __asm__ volatile("dsb sy" ::: "memory");
#endif
}

/* -------------------------------------------------------------------------- */
/* VirtIO Common Feature Bits                                                  */
/* -------------------------------------------------------------------------- */

/* Feature bits common to all devices */
#define VIRTIO_F_RING_INDIRECT_DESC (1UL << 28)  /* Indirect descriptors */
#define VIRTIO_F_RING_EVENT_IDX     (1UL << 29)  /* Used/avail events */
#define VIRTIO_F_VERSION_1          (1UL << 32)  /* VirtIO 1.0+ */
#define VIRTIO_F_ACCESS_PLATFORM    (1UL << 33)  /* Platform-specific */
#define VIRTIO_F_RING_PACKED        (1UL << 34)  /* Packed ring layout */
#define VIRTIO_F_IN_ORDER           (1UL << 35)  /* In-order completion */
#define VIRTIO_F_ORDER_PLATFORM     (1UL << 36)  /* Platform ordering */
#define VIRTIO_F_SR_IOV             (1UL << 37)  /* SR-IOV VF */
#define VIRTIO_F_NOTIFICATION_DATA  (1UL << 38)  /* Extra notify data */

/* -------------------------------------------------------------------------- */
/* VirtIO Common Functions (Prototypes)                                        */
/* -------------------------------------------------------------------------- */

/**
 * Probe for a VirtIO device at the given slot
 *
 * @param slot  Device slot number (0-31)
 * @param expected_type  Expected device type ID (VIRTIO_DEVICE_*)
 * @return  Device base address if found and matches type, 0 otherwise
 */
uintptr_t virtio_probe(unsigned int slot, uint32_t expected_type);

/**
 * Initialize a virtqueue for the given device
 *
 * @param vq    Virtqueue structure to initialize
 * @param base  Device MMIO base address
 * @param index Queue index (0, 1, etc.)
 * @return  0 on success, negative on error
 */
int virtqueue_init(struct virtqueue *vq, uintptr_t base, uint16_t index);

/**
 * Add a buffer to the virtqueue and notify the device
 *
 * @param vq    Virtqueue
 * @param addr  Physical address of buffer
 * @param len   Buffer length
 * @param write True if device should write to buffer
 * @return  Descriptor index on success, negative on error
 */
int virtqueue_add_buf(struct virtqueue *vq, void *addr, uint32_t len, bool write);

/**
 * Get the next completed buffer from the used ring
 *
 * @param vq    Virtqueue
 * @param len   Output: bytes written by device
 * @return  Descriptor index on success, negative if no buffer available
 */
int virtqueue_get_buf(struct virtqueue *vq, uint32_t *len);

/**
 * Notify device that buffers are available
 *
 * @param vq    Virtqueue
 */
void virtqueue_kick(struct virtqueue *vq);

#endif /* VIRTIO_H */
