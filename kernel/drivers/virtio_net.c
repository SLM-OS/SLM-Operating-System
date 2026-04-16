/**
 * VirtIO Network Device Driver for SLM-OS
 *
 * Implements the VirtIO-Net specification using MMIO transport.
 * Provides send/receive functionality for the lwIP TCP/IP stack.
 */

#include "virtio_net.h"
#include "virtio.h"
#include "net_driver.h"
#include "pmm.h"
#include "debug.h"
#include "gic.h"
#include "spinlock.h"
#include "cache.h"
#include <string.h>

/* -------------------------------------------------------------------------- */
/* Driver State                                                                */
/* -------------------------------------------------------------------------- */

static struct virtio_net_device netdev;
static spinlock_t net_lock = SPINLOCK_INIT;
static bool initialized = false;

/* Receive buffer pool */
#define RX_BUFFER_COUNT     16
#define RX_BUFFER_SIZE      VIRTIO_NET_MAX_PACKET

static uint8_t rx_buffer_pool[RX_BUFFER_COUNT][RX_BUFFER_SIZE]
    __attribute__((aligned(4096)));

/* Transmit buffer (single, synchronous TX for simplicity) */
static uint8_t tx_buffer[VIRTIO_NET_MAX_PACKET] __attribute__((aligned(16)));

/* -------------------------------------------------------------------------- */
/* VirtIO Common Functions                                                     */
/* -------------------------------------------------------------------------- */

uintptr_t virtio_probe(unsigned int slot, uint32_t expected_type) {
    uintptr_t base = VIRTIO_DEVICE_BASE(slot);

    uint32_t magic = virtio_read32(base, VIRTIO_MMIO_MAGIC);
    if (magic != VIRTIO_MMIO_MAGIC_VALUE) {
        return 0;  /* Not a VirtIO device */
    }

    uint32_t version = virtio_read32(base, VIRTIO_MMIO_VERSION);
    if (version != 1 && version != 2) {
        INFO("VirtIO slot %u: unsupported version %u", slot, version);
        return 0;
    }

    uint32_t device_id = virtio_read32(base, VIRTIO_MMIO_DEVICE_ID);
    if (device_id == 0) {
        return 0;  /* No device in this slot */
    }

    if (device_id != expected_type) {
        INFO("VirtIO slot %u: device type %u (expected %u)",
             slot, device_id, expected_type);
        return 0;
    }

    return base;
}

/* Calculate memory needed for virtqueue structures */
static size_t virtqueue_size(uint16_t qsize) {
    /* Descriptor table: 16 bytes per entry */
    size_t desc_size = qsize * sizeof(struct virtq_desc);

    /* Available ring: 2 bytes header + 2 bytes per entry + 2 bytes event */
    size_t avail_size = 4 + qsize * 2 + 2;

    /* Used ring needs 4-byte alignment */
    size_t used_offset = (desc_size + avail_size + 3) & ~3;

    /* Used ring: 4 bytes header + 8 bytes per entry + 2 bytes event */
    size_t used_size = 4 + qsize * 8 + 2;

    /* Total size rounded to page */
    return (used_offset + used_size + 4095) & ~4095;
}

int virtqueue_init(struct virtqueue *vq, uintptr_t base, uint16_t index) {
    /* Select the queue */
    virtio_write32(base, VIRTIO_MMIO_QUEUE_SEL, index);
    virtio_mb();

    /* Get max queue size */
    uint32_t max_size = virtio_read32(base, VIRTIO_MMIO_QUEUE_NUM_MAX);
    if (max_size == 0) {
        ERROR("VirtIO queue %u not available", index);
        return -1;
    }

    /* Use smaller of max size or our default */
    uint16_t qsize = (max_size < VIRTQ_MAX_SIZE) ? max_size : VIRTQ_MAX_SIZE;

    /* Allocate aligned memory for queue structures */
    size_t mem_size = virtqueue_size(qsize);
    size_t pages = (mem_size + 4095) / 4096;
    void *mem = pmm_alloc_pages(pages);
    if (!mem) {
        ERROR("Failed to allocate virtqueue memory");
        return -1;
    }
    memset(mem, 0, mem_size);

    /* Set up pointers */
    vq->index = index;
    vq->size = qsize;
    vq->regs = (volatile uint32_t *)base;
    vq->desc = (struct virtq_desc *)mem;

    /* Available ring follows descriptors */
    size_t desc_size = qsize * sizeof(struct virtq_desc);
    vq->avail = (struct virtq_avail *)((uint8_t *)mem + desc_size);

    /* Used ring is 4-byte aligned after available ring */
    size_t avail_size = 4 + qsize * 2 + 2;
    size_t used_offset = (desc_size + avail_size + 3) & ~3;
    vq->used = (struct virtq_used *)((uint8_t *)mem + used_offset);

    /* Initialize free list: chain all descriptors */
    vq->free_head = 0;
    vq->num_free = qsize;
    for (uint16_t i = 0; i < qsize - 1; i++) {
        vq->desc[i].next = i + 1;
    }
    vq->last_used_idx = 0;

    /* Tell device about queue size */
    virtio_write32(base, VIRTIO_MMIO_QUEUE_NUM, qsize);

    /* Set queue addresses (VirtIO 1.0 / modern) */
    uintptr_t desc_addr = (uintptr_t)vq->desc;
    uintptr_t avail_addr = (uintptr_t)vq->avail;
    uintptr_t used_addr = (uintptr_t)vq->used;

    virtio_write32(base, VIRTIO_MMIO_QUEUE_DESC_LOW, (uint32_t)desc_addr);
    virtio_write32(base, VIRTIO_MMIO_QUEUE_DESC_HIGH, (uint32_t)(desc_addr >> 32));
    virtio_write32(base, VIRTIO_MMIO_QUEUE_AVAIL_LOW, (uint32_t)avail_addr);
    virtio_write32(base, VIRTIO_MMIO_QUEUE_AVAIL_HIGH, (uint32_t)(avail_addr >> 32));
    virtio_write32(base, VIRTIO_MMIO_QUEUE_USED_LOW, (uint32_t)used_addr);
    virtio_write32(base, VIRTIO_MMIO_QUEUE_USED_HIGH, (uint32_t)(used_addr >> 32));

    /* Enable the queue */
    virtio_write32(base, VIRTIO_MMIO_QUEUE_READY, 1);
    virtio_mb();

    INFO("VirtIO queue %u initialized: size=%u", index, qsize);
    return 0;
}

/* Allocate a descriptor from the free list */
static int alloc_desc(struct virtqueue *vq) {
    if (vq->num_free == 0) {
        return -1;
    }

    int idx = vq->free_head;
    vq->free_head = vq->desc[idx].next;
    vq->num_free--;
    return idx;
}

/* Free a descriptor back to the free list */
static void free_desc(struct virtqueue *vq, int idx) {
    vq->desc[idx].next = vq->free_head;
    vq->free_head = idx;
    vq->num_free++;
}

int virtqueue_add_buf(struct virtqueue *vq, void *addr, uint32_t len, bool write) {
    int desc_idx = alloc_desc(vq);
    if (desc_idx < 0) {
        return -1;
    }

    /* Set up descriptor */
    vq->desc[desc_idx].addr = (uintptr_t)addr;
    vq->desc[desc_idx].len = len;
    vq->desc[desc_idx].flags = write ? VIRTQ_DESC_F_WRITE : 0;
    vq->desc[desc_idx].next = 0;

    /* Add to available ring */
    uint16_t avail_idx = vq->avail->idx % vq->size;
    vq->avail->ring[avail_idx] = desc_idx;
    virtio_mb();
    vq->avail->idx++;
    virtio_mb();

    /*
     * Cache maintenance for platforms without device coherency.
     *
     * virtio_mb() (dsb sy) orders stores between CPUs but does not push
     * dirty cachelines out to PoC on platforms where SMPEN is disabled
     * (Pi 5, Jetson). VirtIO devices do DMA reads through PoC, so they
     * see stale data if the descriptor / avail ring updates are still
     * sitting in a dirty L1/L2 cacheline. Clean the three ranges we
     * just wrote: the descriptor entry, the avail ring slot, and the
     * avail index. On coherent platforms (QEMU, x86-64) these resolve
     * to no-ops or plain barriers.
     */
    cache_clean_range(&vq->desc[desc_idx], sizeof(vq->desc[desc_idx]));
    cache_clean_range(&vq->avail->ring[avail_idx], sizeof(vq->avail->ring[avail_idx]));
    cache_clean_range(&vq->avail->idx, sizeof(vq->avail->idx));

    return desc_idx;
}

int virtqueue_get_buf(struct virtqueue *vq, uint32_t *len) {
    /*
     * Invalidate the used ring header so we re-read from PoC instead of
     * a possibly-stale cacheline left by a prior poll. On coherent
     * platforms this is a no-op / barrier.
     */
    cache_invalidate_range(&vq->used->idx, sizeof(vq->used->idx));
    virtio_mb();

    if (vq->last_used_idx == vq->used->idx) {
        return -1;  /* No buffers ready */
    }

    uint16_t used_idx = vq->last_used_idx % vq->size;

    /* Invalidate the specific used ring entry we're about to read. */
    cache_invalidate_range(&vq->used->ring[used_idx], sizeof(vq->used->ring[used_idx]));

    uint32_t desc_idx = vq->used->ring[used_idx].id;

    if (len) {
        *len = vq->used->ring[used_idx].len;
    }

    /* Free the descriptor */
    free_desc(vq, desc_idx);

    vq->last_used_idx++;
    return desc_idx;
}

void virtqueue_kick(struct virtqueue *vq) {
    virtio_mb();
    virtio_write32((uintptr_t)vq->regs, VIRTIO_MMIO_QUEUE_NOTIFY, vq->index);
}

/* -------------------------------------------------------------------------- */
/* VirtIO-Net Driver Implementation                                            */
/* -------------------------------------------------------------------------- */

/* Read MAC address from config space */
static void read_mac_address(uintptr_t base, uint8_t mac[6]) {
    for (int i = 0; i < 6; i++) {
        mac[i] = *(volatile uint8_t *)(base + VIRTIO_MMIO_CONFIG + i);
    }
}

/* Post receive buffers to RX queue */
static void post_rx_buffers(void) {
    for (int i = 0; i < RX_BUFFER_COUNT; i++) {
        int ret = virtqueue_add_buf(&netdev.rx_vq, rx_buffer_pool[i],
                                    RX_BUFFER_SIZE, true /* device writes */);
        if (ret < 0) {
            WARN("Failed to post RX buffer %d", i);
            break;
        }
    }
    virtqueue_kick(&netdev.rx_vq);
}

int virtio_net_init(void) {
    if (initialized) {
        return 0;  /* Already initialized */
    }

    INFO("Initializing VirtIO-Net driver...");

    /* Probe for network device */
    uintptr_t base = virtio_probe(VIRTIO_NET_SLOT, VIRTIO_DEVICE_NET);
    if (base == 0) {
        ERROR("VirtIO-Net device not found");
        return -1;
    }

    netdev.base = base;
    INFO("VirtIO-Net found at 0x%lx", (unsigned long)base);

    /* Reset device */
    virtio_write32(base, VIRTIO_MMIO_STATUS, 0);
    virtio_mb();

    /* Set ACKNOWLEDGE status bit */
    virtio_write32(base, VIRTIO_MMIO_STATUS, VIRTIO_STATUS_ACKNOWLEDGE);
    virtio_mb();

    /* Set DRIVER status bit */
    uint32_t status = virtio_read32(base, VIRTIO_MMIO_STATUS);
    virtio_write32(base, VIRTIO_MMIO_STATUS, status | VIRTIO_STATUS_DRIVER);
    virtio_mb();

    /* Read device features */
    virtio_write32(base, VIRTIO_MMIO_DEVICE_FEATURES_SEL, 0);
    uint32_t features_lo = virtio_read32(base, VIRTIO_MMIO_DEVICE_FEATURES);
    virtio_write32(base, VIRTIO_MMIO_DEVICE_FEATURES_SEL, 1);
    uint32_t features_hi = virtio_read32(base, VIRTIO_MMIO_DEVICE_FEATURES);

    uint64_t device_features = ((uint64_t)features_hi << 32) | features_lo;
    INFO("Device features: 0x%llx", (unsigned long long)device_features);

    /* Negotiate features (accept what we want and device supports) */
    uint64_t driver_features = device_features & VIRTIO_NET_FEATURES_WANT;
    netdev.features = driver_features;

    virtio_write32(base, VIRTIO_MMIO_DRIVER_FEATURES_SEL, 0);
    virtio_write32(base, VIRTIO_MMIO_DRIVER_FEATURES, (uint32_t)driver_features);
    virtio_write32(base, VIRTIO_MMIO_DRIVER_FEATURES_SEL, 1);
    virtio_write32(base, VIRTIO_MMIO_DRIVER_FEATURES, (uint32_t)(driver_features >> 32));
    virtio_mb();

    /* Set FEATURES_OK */
    status = virtio_read32(base, VIRTIO_MMIO_STATUS);
    virtio_write32(base, VIRTIO_MMIO_STATUS, status | VIRTIO_STATUS_FEATURES_OK);
    virtio_mb();

    /* Verify FEATURES_OK was accepted */
    status = virtio_read32(base, VIRTIO_MMIO_STATUS);
    if (!(status & VIRTIO_STATUS_FEATURES_OK)) {
        ERROR("Device did not accept features");
        virtio_write32(base, VIRTIO_MMIO_STATUS, VIRTIO_STATUS_FAILED);
        return -1;
    }

    /* Initialize virtqueues */
    if (virtqueue_init(&netdev.rx_vq, base, VIRTIO_NET_RX_QUEUE) < 0) {
        ERROR("Failed to init RX queue");
        virtio_write32(base, VIRTIO_MMIO_STATUS, VIRTIO_STATUS_FAILED);
        return -1;
    }

    if (virtqueue_init(&netdev.tx_vq, base, VIRTIO_NET_TX_QUEUE) < 0) {
        ERROR("Failed to init TX queue");
        virtio_write32(base, VIRTIO_MMIO_STATUS, VIRTIO_STATUS_FAILED);
        return -1;
    }

    /* Read MAC address from config space */
    if (netdev.features & VIRTIO_NET_F_MAC) {
        read_mac_address(base, netdev.mac);
        INFO("MAC address: %02x:%02x:%02x:%02x:%02x:%02x",
             netdev.mac[0], netdev.mac[1], netdev.mac[2],
             netdev.mac[3], netdev.mac[4], netdev.mac[5]);
    } else {
        /* Generate a random-ish MAC (locally administered) */
        netdev.mac[0] = 0x52;
        netdev.mac[1] = 0x54;
        netdev.mac[2] = 0x00;
        netdev.mac[3] = 0x12;
        netdev.mac[4] = 0x34;
        netdev.mac[5] = 0x56;
        INFO("Using default MAC: %02x:%02x:%02x:%02x:%02x:%02x",
             netdev.mac[0], netdev.mac[1], netdev.mac[2],
             netdev.mac[3], netdev.mac[4], netdev.mac[5]);
    }

    /* Set DRIVER_OK to complete initialization */
    status = virtio_read32(base, VIRTIO_MMIO_STATUS);
    virtio_write32(base, VIRTIO_MMIO_STATUS, status | VIRTIO_STATUS_DRIVER_OK);
    virtio_mb();

    /* Check link status */
    if (netdev.features & VIRTIO_NET_F_STATUS) {
        struct virtio_net_config *cfg =
            (struct virtio_net_config *)(base + VIRTIO_MMIO_CONFIG);
        netdev.link_up = (cfg->status & VIRTIO_NET_S_LINK_UP) != 0;
    } else {
        netdev.link_up = true;  /* Assume link is up */
    }
    INFO("Link status: %s", netdev.link_up ? "UP" : "DOWN");

    /* Post receive buffers */
    post_rx_buffers();

    /* Register IRQ handler */
    gic_set_priority(VIRTIO_NET_IRQ, 0x80);
    gic_enable_irq(VIRTIO_NET_IRQ);

    initialized = true;
    INFO("VirtIO-Net driver initialized");
    return 0;
}

int virtio_net_send(const uint8_t *data, uint32_t len) {
    if (!initialized) {
        return -1;
    }

    if (len > 1514) {  /* Max Ethernet frame size */
        ERROR("Packet too large: %u bytes", len);
        return -1;
    }

    spin_lock(&net_lock);

    /* Prepare virtio-net header (all zeros for simple case) */
    struct virtio_net_hdr *hdr = (struct virtio_net_hdr *)tx_buffer;
    memset(hdr, 0, sizeof(*hdr));

    /* Copy packet data after header */
    memcpy(tx_buffer + sizeof(*hdr), data, len);

    /* Add to TX queue */
    uint32_t total_len = sizeof(*hdr) + len;
    int desc_idx = virtqueue_add_buf(&netdev.tx_vq, tx_buffer, total_len,
                                     false /* device reads */);
    if (desc_idx < 0) {
        spin_unlock(&net_lock);
        ERROR("TX queue full");
        netdev.tx_errors++;
        return -1;
    }

    /* Notify device */
    virtqueue_kick(&netdev.tx_vq);

    /* Wait for completion (synchronous TX) */
    int timeout = 100000;
    while (timeout > 0) {
        uint32_t used_len;
        if (virtqueue_get_buf(&netdev.tx_vq, &used_len) >= 0) {
            break;
        }
        timeout--;
    }

    if (timeout == 0) {
        WARN("TX timeout");
        netdev.tx_errors++;
        spin_unlock(&net_lock);
        return -1;
    }

    netdev.tx_packets++;
    netdev.tx_bytes += len;

    spin_unlock(&net_lock);
    return 0;
}

int virtio_net_recv(uint8_t *buffer, uint32_t max_len) {
    if (!initialized) {
        return -1;
    }

    spin_lock(&net_lock);

    uint32_t used_len;
    int desc_idx = virtqueue_get_buf(&netdev.rx_vq, &used_len);
    if (desc_idx < 0) {
        spin_unlock(&net_lock);
        return 0;  /* No packet available */
    }

    /* Get the buffer address from the descriptor's stored addr.
     * Cannot use desc_idx as buffer pool index — after the first round
     * of receives, descriptors are reused from a free list and indices
     * no longer correspond to the original buffer pool slots. */
    uint8_t *rx_buf = (uint8_t *)(uintptr_t)netdev.rx_vq.desc[desc_idx].addr;

    /* Skip virtio-net header */
    struct virtio_net_hdr *hdr = (struct virtio_net_hdr *)rx_buf;
    uint8_t *packet = rx_buf + sizeof(*hdr);
    uint32_t packet_len = used_len - sizeof(*hdr);

    if (packet_len > max_len) {
        WARN("RX packet too large: %u > %u", packet_len, max_len);
        netdev.rx_errors++;
        packet_len = max_len;
    }

    memcpy(buffer, packet, packet_len);

    /* Re-post the buffer */
    virtqueue_add_buf(&netdev.rx_vq, rx_buf, RX_BUFFER_SIZE, true);
    virtqueue_kick(&netdev.rx_vq);

    netdev.rx_packets++;
    netdev.rx_bytes += packet_len;

    spin_unlock(&net_lock);
    return packet_len;
}

void virtio_net_irq_handler(void) {
    if (!initialized) {
        return;
    }

    /* Read and acknowledge interrupt */
    uint32_t isr = virtio_read32(netdev.base, VIRTIO_MMIO_INTERRUPT_STATUS);
    virtio_write32(netdev.base, VIRTIO_MMIO_INTERRUPT_ACK, isr);

    if (isr & VIRTIO_IRQ_CONFIG_CHANGE) {
        /* Link status may have changed */
        if (netdev.features & VIRTIO_NET_F_STATUS) {
            struct virtio_net_config *cfg =
                (struct virtio_net_config *)(netdev.base + VIRTIO_MMIO_CONFIG);
            bool old_link = netdev.link_up;
            netdev.link_up = (cfg->status & VIRTIO_NET_S_LINK_UP) != 0;
            if (old_link != netdev.link_up) {
                INFO("Link status changed: %s", netdev.link_up ? "UP" : "DOWN");
            }
        }
    }

    /* Used buffer notifications are handled in recv/send functions */
}

void virtio_net_get_mac(uint8_t mac[6]) {
    if (initialized) {
        memcpy(mac, netdev.mac, 6);
    } else {
        memset(mac, 0, 6);
    }
}

bool virtio_net_link_up(void) {
    return initialized && netdev.link_up;
}

void virtio_net_get_stats(uint64_t *rx_pkts, uint64_t *tx_pkts,
                          uint64_t *rx_bytes, uint64_t *tx_bytes) {
    if (rx_pkts) *rx_pkts = netdev.rx_packets;
    if (tx_pkts) *tx_pkts = netdev.tx_packets;
    if (rx_bytes) *rx_bytes = netdev.rx_bytes;
    if (tx_bytes) *tx_bytes = netdev.tx_bytes;
}

/* -------------------------------------------------------------------------- */
/* net_driver Interface                                                        */
/* -------------------------------------------------------------------------- */

static int virtio_net_drv_send(const void *buf, size_t len) {
    return virtio_net_send((const uint8_t *)buf, (uint32_t)len);
}

static int virtio_net_drv_recv(void *buf, size_t max_len) {
    return virtio_net_recv((uint8_t *)buf, (uint32_t)max_len);
}

static const struct net_driver virtio_net_mmio_driver = {
    .name        = "virtio-net-mmio",
    .init        = virtio_net_init,
    .send        = virtio_net_drv_send,
    .recv        = virtio_net_drv_recv,
    .get_mac     = virtio_net_get_mac,
    .link_status = virtio_net_link_up,
};

void virtio_net_register(void) {
    net_register_driver(&virtio_net_mmio_driver);
}
