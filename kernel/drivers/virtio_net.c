/**
 * VirtIO Network Device Driver for SLM-OS
 *
 * Implements the VirtIO-Net specification using MMIO transport.
 * Provides send/receive functionality for the lwIP TCP/IP stack.
 */

#include "virtio_net.h"
#include "virtio.h"
#include "net.h"            /* net_stats_rx_no_buffers_inc */
#include "net_driver.h"
#include "pmm.h"
#include "debug.h"
#include "gic.h"            /* gic_register_handler / gic_enable_irq */
#include "spinlock.h"
#include "cache.h"
#include "arch/sys_arch.h"  /* sys_now() for wall-clock TX timeout */
#include <string.h>

/* -------------------------------------------------------------------------- */
/* Driver State                                                                */
/* -------------------------------------------------------------------------- */

static struct virtio_net_device netdev;

/*
 * Diagnostic counter — incremented by virtio_net_irq_handler on every
 * invocation. Exposed via virtio_net_get_irq_count() so tests can
 * verify the IRQ path is actually firing (and didn't fall through to
 * the polled net_poll fallback). Not part of net_stats because it's a
 * driver-internal observable, not a netif metric.
 */
static volatile uint32_t irq_count;

/* Runtime IRQ number, captured in virtio_net_init after slot probing.
 * Exposed via virtio_net_get_irq() so tests + diag don't have to
 * re-derive the SPI from the slot probe. Zero until init completes. */
static uint32_t net_irq_number;

/* Stuck-descriptor watchdog (#204 item 4). If tx_reap finds nothing
 * to reap while tx_inflight[] has entries, and it has been at least
 * TX_STALL_THRESHOLD_MS since we last successfully reaped a slot,
 * log a single warning. The warn-once latch (tx_stall_warned) resets
 * the moment a slot is freed, so a genuinely stuck link logs once
 * but transient congestion (which resolves on the next reap) stays
 * quiet. Not fatal: polling-only fallback still works if the IRQ
 * path misses a completion, and lwIP will surface transport errors
 * above the driver. Spurious tx_reap calls — invoked on an empty
 * pool — do not trigger the watchdog because tx_has_inflight() also
 * returns false. */
#define TX_STALL_THRESHOLD_MS 5000
static uint32_t tx_last_progress_ms;
static bool     tx_stall_warned;
/* Bumped every time the watchdog's WARN fires. Exposed via
 * virtio_net_get_tx_stall_count() so tests can verify the watchdog
 * stays quiet on healthy traffic and fires exactly when a stall is
 * simulated. */
static volatile uint32_t tx_stall_warn_count;

/*
 * Separate TX and RX locks. Both are IRQ-safe (accessed via
 * spin_lock_irqsave) because virtio_net_irq_handler acquires tx_lock
 * to drain TX completions — a plain spin_lock held by task context
 * would deadlock with a hard IRQ that tried to re-acquire it on the
 * same CPU.
 */
static spinlock_t tx_lock = SPINLOCK_INIT;
static spinlock_t rx_lock = SPINLOCK_INIT;
/* Volatile because the IRQ handler reads this to decide whether to
 * do any work. If the SPI ever retargets to a CPU other than the one
 * running init, the release-store at the end of virtio_net_init must
 * be observable there without a separate barrier. */
static volatile bool initialized = false;

/* Receive buffer pool */
#define RX_BUFFER_COUNT     16
#define RX_BUFFER_SIZE      VIRTIO_NET_MAX_PACKET

static uint8_t rx_buffer_pool[RX_BUFFER_COUNT][RX_BUFFER_SIZE]
    __attribute__((aligned(4096)));

/* Transmit buffer pool (#204).
 *
 * Replaces the single static tx_buffer that backed the old synchronous
 * TX. send() now allocates one slot from this pool, submits the
 * descriptor, and returns immediately — completion happens later when
 * tx_reap() drains the TX used ring (called from net_poll()). The pool
 * lets multiple TX requests be in flight at once, bounded by the slot
 * count.
 *
 * Slot ↔ buffer mapping is implicit: a buffer's address is stored in
 * its descriptor's `addr` field. On reap, we look at the completed
 * descriptor's addr and pointer-arithmetic our way back to the slot
 * index. tx_inflight tracks which slots are currently submitted so
 * send() can find a free one in O(N) — N is small (16). */
#define TX_BUFFER_COUNT     16
#define TX_BUFFER_SIZE      VIRTIO_NET_MAX_PACKET

static uint8_t tx_buffer_pool[TX_BUFFER_COUNT][TX_BUFFER_SIZE]
    __attribute__((aligned(16)));
static bool    tx_inflight[TX_BUFFER_COUNT];

/* Invariant that lets virtio_net_tx_reap_locked skip a redundant
 * bounds check — if the descriptor's addr is within the pool, then
 * (addr - pool_base) / TX_BUFFER_SIZE is guaranteed < TX_BUFFER_COUNT. */
static_assert(sizeof(tx_buffer_pool) == TX_BUFFER_COUNT * TX_BUFFER_SIZE,
              "tx_buffer_pool layout assumption broken");

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
        /* Mismatch is normal when scanning slots — caller decides
         * whether absence is an error. */
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
            net_stats_rx_no_buffers_inc();
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

    /* Probe for network device.
     *
     * QEMU's virt machine assigns virtio-mmio devices to slots in
     * declaration order, but the slot the network device lands in
     * depends on what other -device flags were passed. Scan all 32
     * slots for the first one whose device_id matches VIRTIO_DEVICE_NET
     * rather than hard-coding slot 0. */
    uintptr_t base = 0;
    unsigned net_slot = 0;
    for (unsigned slot = 0; slot < 32; slot++) {
        base = virtio_probe(slot, VIRTIO_DEVICE_NET);
        if (base != 0) {
            net_slot = slot;
            break;
        }
    }
    if (base == 0) {
        ERROR("VirtIO-Net device not found");
        return -1;
    }

    netdev.base = base;
    uint32_t version = virtio_read32(base, VIRTIO_MMIO_VERSION);
    INFO("VirtIO-Net found at 0x%lx (mmio version=%u)",
         (unsigned long)base, version);
    if (version < 2) {
        /* Modern (v2+) MMIO uses QUEUE_DESC/AVAIL/USED + QUEUE_READY.
         * Legacy (v1) uses QUEUE_PFN. Our driver only implements modern;
         * fail loudly so the user sees "force-legacy=false missing"
         * rather than a silent TX timeout later. See Makefile:QEMU_NET. */
        ERROR("VirtIO-Net legacy (v1) MMIO not supported. "
              "QEMU needs '-global virtio-mmio.force-legacy=false'");
        return -1;
    }

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

    /* Seed the TX watchdog so the first reap doesn't immediately
     * log "stuck for N ms" based on the initial zero value. */
    tx_last_progress_ms = sys_now();

    /* Register IRQ handler. The slot-derived IRQ number is looked up
     * by the EL1 IRQ dispatch in exceptions.c (via
     * gic_lookup_handler) and routed to virtio_net_irq_handler when
     * the device fires. Primary duty: drain TX completions so pool
     * slots free promptly without waiting for net_poll(). RX is
     * still polled (moving it to IRQ would require pbuf_alloc and
     * lwIP input from IRQ context — bigger scaffolding than
     * warranted today). */
    uint32_t irq = VIRTIO_DEVICE_IRQ(net_slot);
    if (gic_register_handler(irq, virtio_net_irq_handler) < 0) {
        WARN("gic_register_handler full — TX completion stays polled-only");
    } else {
        gic_set_priority(irq, 0x80);
        /* Populate observables BEFORE enabling the IRQ at the GIC.
         * A stale pending IRQ can fire the moment gic_enable_irq
         * lands; the handler's early-exit on !initialized is the
         * safety net, but any observer (diagnostic accessor,
         * future self-check) should see the runtime IRQ number
         * the instant deliveries are possible. */
        net_irq_number = irq;
        /* Publish initialized before enabling delivery. The
         * __atomic_store_n with release semantics pairs with the
         * acquire-load the handler would do if this ever retargets
         * to another CPU. Today the SPI targets CPU 0 only, so this
         * is defensive — but the cost is one barrier at init. */
        __atomic_store_n(&initialized, true, __ATOMIC_RELEASE);
        gic_enable_irq(irq);
        INFO("VirtIO-Net IRQ %u registered", irq);
        INFO("VirtIO-Net driver initialized");
        return 0;
    }

    /* Registration failed: device is still usable in polled-only mode. */
    __atomic_store_n(&initialized, true, __ATOMIC_RELEASE);
    INFO("VirtIO-Net driver initialized (polled-only)");
    return 0;
}

/*
 * Drain the TX used ring and free any pool slots whose descriptors
 * the device has finished with. Caller must hold tx_lock.
 *
 * The pool slot for each completed descriptor is recovered by
 * inspecting the descriptor's `addr` field (set by virtqueue_add_buf
 * to the buffer pointer at submit time) and pointer-arithmeticking
 * back to the slot index. Linear in TX_BUFFER_COUNT but bounded —
 * the loop also stops when the used ring is empty.
 */
/* Linear scan of tx_inflight[] — cheap (16 entries) and callers
 * already hold tx_lock, so no additional synchronization needed. */
static bool tx_has_inflight(void) {
    for (unsigned i = 0; i < TX_BUFFER_COUNT; i++) {
        if (tx_inflight[i]) return true;
    }
    return false;
}

/* Watchdog check extracted so the regression suite can exercise it
 * with synthetic inputs via virtio_net_test_trigger_watchdog().
 * Callers must hold tx_lock (same invariant as tx_reap_locked). */
static void tx_watchdog_warn_if_stuck(bool any_inflight) {
    if (tx_stall_warned || !any_inflight)
        return;
    uint32_t elapsed = sys_now() - tx_last_progress_ms;
    if (elapsed >= TX_STALL_THRESHOLD_MS) {
        WARN("TX descriptors stuck: no completion for %u ms (virtio-mmio)",
             elapsed);
        tx_stall_warned = true;
        tx_stall_warn_count++;
    }
}

static void virtio_net_tx_reap_locked(void) {
    bool progressed = false;
    while (true) {
        uint32_t used_len;
        int desc_idx = virtqueue_get_buf(&netdev.tx_vq, &used_len);
        if (desc_idx < 0)
            break;

        uintptr_t addr = (uintptr_t)netdev.tx_vq.desc[desc_idx].addr;
        uintptr_t pool_base = (uintptr_t)&tx_buffer_pool[0][0];
        if (addr < pool_base ||
            addr >= pool_base + sizeof(tx_buffer_pool)) {
            WARN("TX reap: descriptor addr 0x%lx outside pool",
                 (unsigned long)addr);
            continue;
        }
        /* slot < TX_BUFFER_COUNT by the static_assert above. */
        unsigned slot = (unsigned)((addr - pool_base) / TX_BUFFER_SIZE);
        tx_inflight[slot] = false;
        progressed = true;
    }

    /* On any successful reap, reset the watchdog latch. Otherwise
     * (a no-op reap) fall through to the stall check — which is a
     * no-op unless an in-flight pool has been idle past the
     * threshold. */
    if (progressed) {
        tx_last_progress_ms = sys_now();
        tx_stall_warned = false;
    } else {
        tx_watchdog_warn_if_stuck(tx_has_inflight());
    }
}

/* Entry point called from net_poll() via the net_driver op — wraps
 * the locked helper with tx_lock acquisition. Must be IRQ-safe
 * because tx_lock is also acquired from virtio_net_irq_handler. */
static void virtio_net_tx_reap(void) {
    if (!initialized)
        return;
    irq_flags_t flags = spin_lock_irqsave(&tx_lock);
    virtio_net_tx_reap_locked();
    spin_unlock_irqrestore(&tx_lock, flags);
}

int virtio_net_send(const uint8_t *data, uint32_t len) {
    if (!initialized)
        return -1;

    if (len > 1514) {  /* Max Ethernet frame size */
        ERROR("Packet too large: %u bytes", len);
        return NET_E_TOO_LARGE;
    }

    irq_flags_t flags = spin_lock_irqsave(&tx_lock);

    /* Reap completions opportunistically — frees pool slots so the
     * search below has a fresh view. Without this, two back-to-back
     * sends after a quiet period would see the pool full from the
     * prior round even though the device has long since finished. */
    virtio_net_tx_reap_locked();

    /* Find a free pool slot */
    int slot = -1;
    for (unsigned i = 0; i < TX_BUFFER_COUNT; i++) {
        if (!tx_inflight[i]) {
            slot = (int)i;
            break;
        }
    }
    if (slot < 0) {
        spin_unlock_irqrestore(&tx_lock, flags);
        return NET_E_BUSY;  /* All slots in flight; caller retries via net_poll */
    }

    /* Claim the slot BEFORE the device can see the descriptor. A
     * hard IRQ firing between virtqueue_add_buf and this assignment
     * would reap the completion, see tx_inflight[slot] still false,
     * and silently bail. Claiming first means the reap path always
     * observes a consistent "claimed" state. The IRQ is masked while
     * we hold tx_lock (_irqsave), so no such reentry can happen —
     * the pre-claim is belt-and-braces in case the lock flavor is
     * ever downgraded. */
    tx_inflight[slot] = true;

    /* Build packet in the chosen slot */
    uint8_t *buf = tx_buffer_pool[slot];
    struct virtio_net_hdr *hdr = (struct virtio_net_hdr *)buf;
    memset(hdr, 0, sizeof(*hdr));
    memcpy(buf + sizeof(*hdr), data, len);

    /* Submit and kick — device DMA-reads from the slot, then writes
     * back to the used ring at its own pace. send() returns
     * immediately; tx_reap() drains completions later. */
    uint32_t total_len = sizeof(*hdr) + len;
    int desc_idx = virtqueue_add_buf(&netdev.tx_vq, buf, total_len,
                                     false /* device reads */);
    if (desc_idx < 0) {
        tx_inflight[slot] = false;  /* release claim on submit failure */
        spin_unlock_irqrestore(&tx_lock, flags);
        return NET_E_BUSY;  /* TX virtqueue descriptor pool exhausted */
    }

    virtqueue_kick(&netdev.tx_vq);
    spin_unlock_irqrestore(&tx_lock, flags);
    return 0;
}

int virtio_net_recv(uint8_t *buffer, uint32_t max_len) {
    if (!initialized) {
        return -1;
    }

    /* rx_lock is IRQ-safe for symmetry with tx_lock, in case a
     * future IRQ handler gains RX-drain responsibilities. Not
     * strictly required today (the handler only touches TX). */
    irq_flags_t flags = spin_lock_irqsave(&rx_lock);

    uint32_t used_len;
    int desc_idx = virtqueue_get_buf(&netdev.rx_vq, &used_len);
    if (desc_idx < 0) {
        spin_unlock_irqrestore(&rx_lock, flags);
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
        packet_len = max_len;
    }

    memcpy(buffer, packet, packet_len);

    /* Re-post the buffer */
    if (virtqueue_add_buf(&netdev.rx_vq, rx_buf, RX_BUFFER_SIZE, true) < 0) {
        /* Descriptor pool exhausted — the buffer we just received is
         * about to be orphaned because the device has no free slot
         * to write future packets into. Makes the drop visible in
         * netstat. */
        net_stats_rx_no_buffers_inc();
    }
    virtqueue_kick(&netdev.rx_vq);

    spin_unlock_irqrestore(&rx_lock, flags);
    return packet_len;
}

void virtio_net_irq_handler(void) {
    if (!initialized) {
        return;
    }

    irq_count++;

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

    if (isr & VIRTIO_IRQ_USED_BUFFER) {
        /* Drain TX completions immediately so pool slots free up
         * without waiting for the next net_poll(). RX is still
         * drained from net_poll() — moving RX to IRQ context would
         * need pbuf_alloc + lwIP input from IRQ, which is a much
         * bigger scaffolding change.
         *
         * Already in IRQ context (EL1 IRQ vector entered with IRQ
         * masked), so spin_lock_irqsave's irq_save is a no-op but
         * still the correct primitive to pair with task-context
         * acquirers. */
        irq_flags_t flags = spin_lock_irqsave(&tx_lock);
        virtio_net_tx_reap_locked();
        spin_unlock_irqrestore(&tx_lock, flags);
    }
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

uint32_t virtio_net_get_irq_count(void) {
    return irq_count;
}

uint32_t virtio_net_get_irq(void) {
    return net_irq_number;
}

uint32_t virtio_net_get_tx_stall_count(void) {
    return tx_stall_warn_count;
}

/*
 * Test hook: run the watchdog check as if we'd just observed
 * no-progress + in-flight=true, with the last-progress timestamp
 * rewound past the threshold. Exercises exactly the code path
 * tx_reap_locked takes when a real stall happens, without
 * requiring 5 real seconds or a way to freeze QEMU's TX completion.
 *
 * After this runs, tx_stall_warn_count advances by 1 and the
 * warn-once latch is set (matching production behaviour). A
 * subsequent real reap that finds completions will reset the
 * latch naturally.
 */
void virtio_net_test_trigger_watchdog(void) {
    tx_last_progress_ms = sys_now() - (TX_STALL_THRESHOLD_MS + 100);
    tx_stall_warned = false;
    tx_watchdog_warn_if_stuck(true);  /* synthetic: pretend pool is busy */
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
    .tx_reap     = virtio_net_tx_reap,
};

void virtio_net_register(void) {
    net_register_driver(&virtio_net_mmio_driver);
}
