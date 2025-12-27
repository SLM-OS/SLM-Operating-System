/**
 * VirtIO Network Device Driver for SLM-OS
 *
 * Implements the VirtIO-Net specification for QEMU's virtual network card.
 * Uses the MMIO transport with split virtqueue layout.
 *
 * Reference: VirtIO 1.1 Spec, Section 5.1 (Network Device)
 */

#ifndef VIRTIO_NET_H
#define VIRTIO_NET_H

#include <stdint.h>
#include <stdbool.h>
#include "virtio.h"

/* -------------------------------------------------------------------------- */
/* VirtIO-Net Device Slot and IRQ                                              */
/* -------------------------------------------------------------------------- */

/* Default slot for network device (QEMU typically uses first available) */
#ifndef VIRTIO_NET_SLOT
#define VIRTIO_NET_SLOT         0
#endif

#define VIRTIO_NET_BASE         VIRTIO_DEVICE_BASE(VIRTIO_NET_SLOT)
#define VIRTIO_NET_IRQ          VIRTIO_DEVICE_IRQ(VIRTIO_NET_SLOT)

/* -------------------------------------------------------------------------- */
/* VirtIO-Net Feature Bits                                                     */
/* -------------------------------------------------------------------------- */

#define VIRTIO_NET_F_CSUM               (1UL << 0)   /* Host can checksum */
#define VIRTIO_NET_F_GUEST_CSUM         (1UL << 1)   /* Guest can checksum */
#define VIRTIO_NET_F_CTRL_GUEST_OFFLOADS (1UL << 2)  /* Control channel */
#define VIRTIO_NET_F_MTU                (1UL << 3)   /* MTU in config */
#define VIRTIO_NET_F_MAC                (1UL << 5)   /* MAC in config */
#define VIRTIO_NET_F_GSO                (1UL << 6)   /* Generic segmentation */
#define VIRTIO_NET_F_GUEST_TSO4         (1UL << 7)   /* Guest can handle TSOv4 */
#define VIRTIO_NET_F_GUEST_TSO6         (1UL << 8)   /* Guest can handle TSOv6 */
#define VIRTIO_NET_F_GUEST_ECN          (1UL << 9)   /* Guest can handle ECN */
#define VIRTIO_NET_F_GUEST_UFO          (1UL << 10)  /* Guest can handle UFO */
#define VIRTIO_NET_F_HOST_TSO4          (1UL << 11)  /* Host can handle TSOv4 */
#define VIRTIO_NET_F_HOST_TSO6          (1UL << 12)  /* Host can handle TSOv6 */
#define VIRTIO_NET_F_HOST_ECN           (1UL << 13)  /* Host can handle ECN */
#define VIRTIO_NET_F_HOST_UFO           (1UL << 14)  /* Host can handle UFO */
#define VIRTIO_NET_F_MRG_RXBUF          (1UL << 15)  /* Merge receive bufs */
#define VIRTIO_NET_F_STATUS             (1UL << 16)  /* Status in config */
#define VIRTIO_NET_F_CTRL_VQ            (1UL << 17)  /* Control virtqueue */
#define VIRTIO_NET_F_CTRL_RX            (1UL << 18)  /* RX mode control */
#define VIRTIO_NET_F_CTRL_VLAN          (1UL << 19)  /* VLAN filtering */
#define VIRTIO_NET_F_CTRL_RX_EXTRA      (1UL << 20)  /* Extra RX mode */
#define VIRTIO_NET_F_GUEST_ANNOUNCE     (1UL << 21)  /* Gratuitous ARP */
#define VIRTIO_NET_F_MQ                 (1UL << 22)  /* Multiqueue */
#define VIRTIO_NET_F_CTRL_MAC_ADDR      (1UL << 23)  /* MAC control */
#define VIRTIO_NET_F_SPEED_DUPLEX       (1UL << 63)  /* Speed/duplex in config */

/* Features we want to use */
#define VIRTIO_NET_FEATURES_WANT ( \
    VIRTIO_NET_F_MAC           | \
    VIRTIO_NET_F_STATUS        | \
    VIRTIO_F_VERSION_1         \
)

/* -------------------------------------------------------------------------- */
/* VirtIO-Net Configuration Space                                              */
/* -------------------------------------------------------------------------- */

/* Configuration space structure (at MMIO offset 0x100) */
struct virtio_net_config {
    uint8_t  mac[6];        /* MAC address (if F_MAC) */
    uint16_t status;        /* Link status (if F_STATUS) */
    uint16_t max_virtq_pairs; /* Max multiqueue pairs (if F_MQ) */
    uint16_t mtu;           /* MTU (if F_MTU) */
    uint32_t speed;         /* Speed in Mbps (if F_SPEED_DUPLEX) */
    uint8_t  duplex;        /* Duplex mode (if F_SPEED_DUPLEX) */
} __attribute__((packed));

/* Status field bits */
#define VIRTIO_NET_S_LINK_UP    (1 << 0)    /* Link is up */
#define VIRTIO_NET_S_ANNOUNCE   (1 << 1)    /* Announcement needed */

/* -------------------------------------------------------------------------- */
/* VirtIO-Net Packet Header                                                    */
/* -------------------------------------------------------------------------- */

/*
 * Every packet sent/received has this header prepended.
 * When not using checksum offload or GSO, most fields are zero.
 */
struct virtio_net_hdr {
    uint8_t  flags;         /* VIRTIO_NET_HDR_F_* */
    uint8_t  gso_type;      /* GSO type (NONE if not using GSO) */
    uint16_t hdr_len;       /* Header length (ETH+IP+TCP/UDP) */
    uint16_t gso_size;      /* GSO segment size */
    uint16_t csum_start;    /* Checksum start offset */
    uint16_t csum_offset;   /* Checksum offset from csum_start */
    /* If VIRTIO_NET_F_MRG_RXBUF, followed by: uint16_t num_buffers; */
} __attribute__((packed));

/* Header flags */
#define VIRTIO_NET_HDR_F_NEEDS_CSUM     (1 << 0)    /* Checksum needed */
#define VIRTIO_NET_HDR_F_DATA_VALID     (1 << 1)    /* Csum is valid */
#define VIRTIO_NET_HDR_F_RSC_INFO       (1 << 2)    /* RSC info present */

/* GSO types */
#define VIRTIO_NET_HDR_GSO_NONE         0   /* No segmentation */
#define VIRTIO_NET_HDR_GSO_TCPV4        1   /* TCPv4 segmentation */
#define VIRTIO_NET_HDR_GSO_UDP          3   /* UDP fragmentation */
#define VIRTIO_NET_HDR_GSO_TCPV6        4   /* TCPv6 segmentation */
#define VIRTIO_NET_HDR_GSO_ECN          0x80 /* ECN flag */

/* -------------------------------------------------------------------------- */
/* VirtIO-Net Virtqueues                                                       */
/* -------------------------------------------------------------------------- */

#define VIRTIO_NET_RX_QUEUE     0   /* Receive queue index */
#define VIRTIO_NET_TX_QUEUE     1   /* Transmit queue index */
#define VIRTIO_NET_CTRL_QUEUE   2   /* Control queue (if F_CTRL_VQ) */

/* Queue sizes */
#define VIRTIO_NET_RX_QUEUE_SIZE    128
#define VIRTIO_NET_TX_QUEUE_SIZE    128

/* Maximum packet size (Ethernet frame + virtio header) */
#define VIRTIO_NET_MAX_PACKET   (1514 + sizeof(struct virtio_net_hdr))

/* -------------------------------------------------------------------------- */
/* VirtIO-Net Driver State                                                     */
/* -------------------------------------------------------------------------- */

struct virtio_net_device {
    /* Device base address */
    uintptr_t base;

    /* MAC address */
    uint8_t mac[6];

    /* Link status */
    bool link_up;

    /* Negotiated features */
    uint64_t features;

    /* Virtqueues */
    struct virtqueue rx_vq;
    struct virtqueue tx_vq;

    /* Receive buffers (pre-posted) */
    uint8_t *rx_buffers;
    uint16_t rx_buffer_count;

    /* Statistics */
    uint64_t rx_packets;
    uint64_t tx_packets;
    uint64_t rx_bytes;
    uint64_t tx_bytes;
    uint64_t rx_errors;
    uint64_t tx_errors;
};

/* -------------------------------------------------------------------------- */
/* VirtIO-Net Driver API                                                       */
/* -------------------------------------------------------------------------- */

/**
 * Initialize the VirtIO-Net driver
 *
 * Probes for a VirtIO network device, negotiates features, and sets up
 * the receive and transmit queues.
 *
 * @return  0 on success, negative error code on failure
 */
int virtio_net_init(void);

/**
 * Send a packet
 *
 * @param data  Packet data (Ethernet frame, without virtio header)
 * @param len   Packet length in bytes
 * @return  0 on success, negative error code on failure
 */
int virtio_net_send(const uint8_t *data, uint32_t len);

/**
 * Receive a packet (polling)
 *
 * @param buffer  Buffer to receive packet into
 * @param max_len Maximum buffer size
 * @return  Number of bytes received, 0 if no packet, negative on error
 */
int virtio_net_recv(uint8_t *buffer, uint32_t max_len);

/**
 * Handle VirtIO-Net interrupt
 *
 * Called from the GIC IRQ handler when VIRTIO_NET_IRQ fires.
 * Processes completed TX buffers and queues new RX buffers.
 */
void virtio_net_irq_handler(void);

/**
 * Get the MAC address
 *
 * @param mac  Buffer for 6-byte MAC address
 */
void virtio_net_get_mac(uint8_t mac[6]);

/**
 * Check if link is up
 *
 * @return  true if link is up
 */
bool virtio_net_link_up(void);

/**
 * Get driver statistics
 *
 * @param rx_pkts   Output: received packets
 * @param tx_pkts   Output: transmitted packets
 * @param rx_bytes  Output: received bytes
 * @param tx_bytes  Output: transmitted bytes
 */
void virtio_net_get_stats(uint64_t *rx_pkts, uint64_t *tx_pkts,
                          uint64_t *rx_bytes, uint64_t *tx_bytes);

#endif /* VIRTIO_NET_H */
