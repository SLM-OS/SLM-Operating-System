/*
 * ipc.c - Inter-Process Communication for SLM-OS
 *
 * Implements message queues (ring buffer) and shared buffers.
 */

#include "ipc.h"
#include "pmm.h"
#include "vmm.h"
#include "sched.h"
#include "uart.h"
#include "debug.h"
#include "../gpu/gpu.h"
#include <stddef.h>
#include <stdint.h>

/* Hard cap on per-priority capacity for msg_queue_create. The total slot
 * count is `capacity * MSG_PRIO_COUNT`; 65536 keeps the upper bound well
 * below any plausible working-set size while preventing DoS via huge
 * capacities. */
#define MSG_QUEUE_MAX_CAPACITY 65536

/*
 * ==========================================================================
 * Local utilities (freestanding - no libc)
 * ==========================================================================
 */

static inline void *ipc_memcpy(void *dest, const void *src, size_t n)
{
    uint8_t *d = dest;
    const uint8_t *s = src;
    while (n--) {
        *d++ = *s++;
    }
    return dest;
}

static inline void ipc_memset(void *dest, int c, size_t n)
{
    uint8_t *d = dest;
    while (n--) {
        *d++ = (uint8_t)c;
    }
}

/*
 * ==========================================================================
 * Global IPC State
 * ==========================================================================
 */

static struct {
    struct msg_queue   *queues[MSG_QUEUE_MAX];
    struct shared_buffer *buffers[SHM_BUFFER_MAX];
    spinlock_t          lock;           /* Protects arrays */
    uint32_t            next_queue_id;
    uint32_t            next_buffer_id;
    int                 initialized;
} ipc_state;

/*
 * ==========================================================================
 * Timer helpers for timeout support
 * ==========================================================================
 */
#include "timer.h"

/*
 * Convert milliseconds to timer ticks.
 */
static inline uint64_t ms_to_ticks(uint32_t ms)
{
    uint64_t freq = timer_get_frequency();
    return (uint64_t)ms * freq / 1000;
}

/*
 * Check if timeout has elapsed.
 * Returns 1 if expired, 0 otherwise.
 */
static inline int timeout_expired(uint64_t start_time, uint64_t timeout_ticks)
{
    uint64_t now = timer_get_count();
    return (now - start_time) >= timeout_ticks;
}

/*
 * ==========================================================================
 * Blocking helpers
 * ==========================================================================
 *
 * Blocking is implemented via sleep/wake with optional timeout support.
 *
 * For infinite waits, tasks are put in BLOCKED state and removed from
 * the run queue until explicitly woken.
 *
 * For timed waits, we use polling with yield() to periodically check
 * if the condition is met or timeout has expired.
 */

/*
 * Block the current task on a wait queue.
 * Caller must hold `lock` with IRQs saved into `flags`. Returns the
 * new IRQ flags after re-acquiring the lock so the caller can restore
 * symmetrically without leaving a lock-held-IRQs-enabled window
 * between the previous `irq_restore` and a fresh `irq_save`. A timer
 * ISR firing in that window and contending the same `lock` would
 * deadlock the CPU.
 */
static irq_flags_t block_on_queue(struct task **wait_queue,
                                  spinlock_t *lock,
                                  irq_flags_t flags)
{
    struct task *current = task_current();

    /* Add to wait queue (simple linked list) */
    current->next = *wait_queue;
    *wait_queue = current;

    /* Mark as blocked and remove from scheduler */
    current->state = TASK_BLOCKED;

    /* Release lock + restore IRQs as one operation so we never sit
     * lock-held with IRQs enabled. */
    spin_unlock_irqrestore(lock, flags);

    /* Yield - scheduler will skip this task until woken */
    yield();

    /* Re-acquire lock and capture fresh IRQ flags for the caller. */
    return spin_lock_irqsave(lock);
}

/*
 * Wake one task from a wait queue.
 * Caller must hold queue->lock.
 * Returns the woken task, or NULL if queue was empty.
 */
static struct task *wake_one(struct task **wait_queue)
{
    struct task *task = *wait_queue;

    if (task) {
        *wait_queue = task->next;
        task->next = NULL;
        task->state = TASK_READY;
        scheduler_add_task(task);
    }

    return task;
}

/*
 * ==========================================================================
 * Message Queue Implementation
 * ==========================================================================
 */

struct msg_queue *msg_queue_create(size_t capacity, size_t msg_size)
{
    if (capacity == 0 || capacity > MSG_QUEUE_MAX_CAPACITY) {
        return NULL;
    }

    if (msg_size == 0) {
        msg_size = MSG_SIZE_DEFAULT;
    }

    /*
     * Reject inputs that would overflow the buffer-size math below.
     * capacity is already bounded by MSG_QUEUE_MAX_CAPACITY, so
     * `capacity * MSG_PRIO_COUNT` cannot overflow; the remaining risk is
     * `total_capacity * msg_size`.
     *
     * Pin MSG_PRIO_COUNT > 0 at compile time so the divide below stays
     * safe even if the header constant is ever lowered.
     */
    _Static_assert(MSG_PRIO_COUNT > 0, "MSG_PRIO_COUNT must be > 0");
    size_t total_capacity = capacity * MSG_PRIO_COUNT;
    if (msg_size > SIZE_MAX / total_capacity) {
        return NULL;
    }
    size_t buffer_size = total_capacity * msg_size;
    size_t pages_needed = (buffer_size + PAGE_SIZE - 1) / PAGE_SIZE;

    /* Allocate queue structure (1 page is more than enough) */
    struct msg_queue *queue = pmm_alloc_page();
    if (!queue) {
        WARN("msg_queue_create: failed to allocate queue structure");
        return NULL;
    }
    ipc_memset(queue, 0, PAGE_SIZE);

    queue->buffer = pmm_alloc_pages(pages_needed);
    if (!queue->buffer) {
        WARN("msg_queue_create: failed to allocate buffer (%zu bytes)",
             buffer_size);
        pmm_free_page(queue);
        return NULL;
    }
    ipc_memset(queue->buffer, 0, pages_needed * PAGE_SIZE);

    /* Initialize queue */
    spin_init(&queue->lock);
    queue->msg_size = msg_size;
    queue->capacity = capacity;
    queue->total_capacity = total_capacity;
    queue->total_count = 0;
    queue->send_waiters = NULL;
    queue->recv_waiters = NULL;

    /* Initialize per-priority state */
    for (int p = 0; p < MSG_PRIO_COUNT; p++) {
        queue->prio[p].head = 0;
        queue->prio[p].tail = 0;
        queue->prio[p].count = 0;
    }

    /* Initialize starvation prevention */
    queue->high_prio_recv_count = 0;

    /* Initialize statistics */
    queue->msgs_sent = 0;
    queue->msgs_recv = 0;
    queue->high_water = 0;
    for (int p = 0; p < MSG_PRIO_COUNT; p++) {
        queue->prio_msgs_sent[p] = 0;
    }

    /* Register in global table */
    irq_flags_t flags = spin_lock_irqsave(&ipc_state.lock);

    uint32_t id = ipc_state.next_queue_id++;
    queue->id = id;

    /* Find free slot. If the table is full, surface the failure to the
     * caller as NULL — silently dropping the queue would leak the
     * pmm_alloc_page above and the buffer pages, with no diagnostic. */
    bool registered = false;
    for (size_t i = 0; i < MSG_QUEUE_MAX; i++) {
        if (!ipc_state.queues[i]) {
            ipc_state.queues[i] = queue;
            registered = true;
            break;
        }
    }

    spin_unlock_irqrestore(&ipc_state.lock, flags);

    if (!registered) {
        WARN("msg_queue_create: queue table full (MSG_QUEUE_MAX=%d)", MSG_QUEUE_MAX);
        pmm_free_pages(queue->buffer, pages_needed);
        pmm_free_page(queue);
        return NULL;
    }

    DEBUG_PRINT("Created message queue %u (capacity=%zu per-prio, msg_size=%zu)",
                queue->id, capacity, msg_size);

    return queue;
}

int msg_queue_destroy(struct msg_queue *queue)
{
    if (!queue) {
        return IPC_ERR_INVALID;
    }

    irq_flags_t flags = spin_lock_irqsave(&queue->lock);

    /* Check for waiting tasks */
    if (queue->send_waiters || queue->recv_waiters) {
        spin_unlock_irqrestore(&queue->lock, flags);
        WARN("msg_queue_destroy: queue %u has waiting tasks", queue->id);
        return IPC_ERR_BUSY;
    }

    spin_unlock_irqrestore(&queue->lock, flags);

    /* Remove from global table */
    flags = spin_lock_irqsave(&ipc_state.lock);
    for (size_t i = 0; i < MSG_QUEUE_MAX; i++) {
        if (ipc_state.queues[i] == queue) {
            ipc_state.queues[i] = NULL;
            break;
        }
    }
    spin_unlock_irqrestore(&ipc_state.lock, flags);

    /* Free buffer and queue structure */
    size_t buffer_size = queue->total_capacity * queue->msg_size;
    size_t pages = (buffer_size + PAGE_SIZE - 1) / PAGE_SIZE;
    pmm_free_pages(queue->buffer, pages);
    pmm_free_page(queue);

    DEBUG_PRINT("Destroyed message queue %u", queue->id);

    return IPC_OK;
}

/*
 * Calculate buffer offset for a priority level's slot.
 * Each priority level has its own section of the buffer.
 */
static inline uint8_t *prio_slot(struct msg_queue *queue, int prio, size_t idx)
{
    /* Buffer layout: [prio0 slots][prio1 slots][prio2 slots][prio3 slots] */
    size_t base_offset = (size_t)prio * queue->capacity * queue->msg_size;
    size_t slot_offset = idx * queue->msg_size;
    return queue->buffer + base_offset + slot_offset;
}

int msg_send_priority(struct msg_queue *queue, const void *msg,
                      int priority, int timeout_ms)
{
    if (!queue || !msg) {
        return IPC_ERR_INVALID;
    }

    /* Clamp priority to valid range */
    if (priority < MSG_PRIO_LOW) priority = MSG_PRIO_LOW;
    if (priority > MSG_PRIO_URGENT) priority = MSG_PRIO_URGENT;

    /* Record start time for timeout tracking */
    uint64_t start_time = 0;
    uint64_t timeout_ticks = 0;
    if (timeout_ms > 0) {
        start_time = timer_get_count();
        timeout_ticks = ms_to_ticks((uint32_t)timeout_ms);
    }

    irq_flags_t flags = spin_lock_irqsave(&queue->lock);

    /* Wait for space if this priority level is full */
    while (queue->prio[priority].count >= queue->capacity) {
        if (timeout_ms == MSG_NO_WAIT) {
            spin_unlock_irqrestore(&queue->lock, flags);
            return IPC_ERR_FULL;
        }

        /* Check timeout for timed waits */
        if (timeout_ms > 0 && timeout_expired(start_time, timeout_ticks)) {
            spin_unlock_irqrestore(&queue->lock, flags);
            return IPC_ERR_TIMEOUT;
        }

        if (timeout_ms < 0) {
            /* Infinite wait: block until space available */
            flags = block_on_queue(&queue->send_waiters, &queue->lock, flags);
        } else {
            /* Timed wait: yield and retry */
            spin_unlock_irqrestore(&queue->lock, flags);
            yield();
            flags = spin_lock_irqsave(&queue->lock);
        }
    }

    /* Copy message to this priority's buffer section */
    struct prio_buffer *pb = &queue->prio[priority];
    uint8_t *slot = prio_slot(queue, priority, pb->head);
    ipc_memcpy(slot, msg, queue->msg_size);

    /* Advance head for this priority level */
    pb->head = (pb->head + 1) % queue->capacity;
    pb->count++;
    queue->total_count++;

    /* Update statistics */
    queue->msgs_sent++;
    queue->prio_msgs_sent[priority]++;
    if (queue->total_count > queue->high_water) {
        queue->high_water = queue->total_count;
    }

    /* Wake a waiting receiver */
    wake_one(&queue->recv_waiters);

    spin_unlock_irqrestore(&queue->lock, flags);

    return IPC_OK;
}

int msg_send(struct msg_queue *queue, const void *msg, int timeout_ms)
{
    /* Default to normal priority for backward compatibility */
    return msg_send_priority(queue, msg, MSG_PRIO_NORMAL, timeout_ms);
}

/*
 * Find the priority level to receive from.
 * Normally receives from highest priority with messages.
 * Starvation prevention: after N consecutive high-priority receives,
 * check lower priorities to give them a chance.
 *
 * Caller must hold queue->lock.
 * Returns priority level (0-3) or -1 if all empty.
 */
static int select_recv_priority(struct msg_queue *queue)
{
    /*
     * Starvation prevention: after MSG_STARVATION_THRESHOLD consecutive
     * receives from higher priorities, try to serve lower priorities once.
     */
    if (queue->high_prio_recv_count >= MSG_STARVATION_THRESHOLD) {
        /* Check all priorities from lowest to highest for starvation relief */
        for (int p = MSG_PRIO_LOW; p <= MSG_PRIO_URGENT; p++) {
            if (queue->prio[p].count > 0) {
                /* Reset counter when serving lower priority */
                queue->high_prio_recv_count = 0;
                return p;
            }
        }
    }

    /* Normal case: return highest priority with messages */
    for (int p = MSG_PRIO_URGENT; p >= MSG_PRIO_LOW; p--) {
        if (queue->prio[p].count > 0) {
            /*
             * Track consecutive high-priority receives.
             * Only count if we're receiving above MSG_PRIO_LOW.
             */
            if (p > MSG_PRIO_LOW) {
                queue->high_prio_recv_count++;
            } else {
                queue->high_prio_recv_count = 0;
            }
            return p;
        }
    }

    return -1;  /* All priorities empty */
}

int msg_recv(struct msg_queue *queue, void *msg, int timeout_ms)
{
    if (!queue || !msg) {
        return IPC_ERR_INVALID;
    }

    /* Record start time for timeout tracking */
    uint64_t start_time = 0;
    uint64_t timeout_ticks = 0;
    if (timeout_ms > 0) {
        start_time = timer_get_count();
        timeout_ticks = ms_to_ticks((uint32_t)timeout_ms);
    }

    irq_flags_t flags = spin_lock_irqsave(&queue->lock);

    /* Wait for message if queue is empty */
    while (queue->total_count == 0) {
        if (timeout_ms == MSG_NO_WAIT) {
            spin_unlock_irqrestore(&queue->lock, flags);
            return IPC_ERR_EMPTY;
        }

        /* Check timeout for timed waits */
        if (timeout_ms > 0 && timeout_expired(start_time, timeout_ticks)) {
            spin_unlock_irqrestore(&queue->lock, flags);
            return IPC_ERR_TIMEOUT;
        }

        if (timeout_ms < 0) {
            /* Infinite wait: block until message available */
            flags = block_on_queue(&queue->recv_waiters, &queue->lock, flags);
        } else {
            /* Timed wait: yield and retry */
            spin_unlock_irqrestore(&queue->lock, flags);
            yield();
            flags = spin_lock_irqsave(&queue->lock);
        }
    }

    /* Select which priority level to receive from */
    int prio = select_recv_priority(queue);
    if (prio < 0) {
        /* Should not happen since total_count > 0 */
        spin_unlock_irqrestore(&queue->lock, flags);
        return IPC_ERR_EMPTY;
    }

    /* Copy message from the selected priority's buffer section */
    struct prio_buffer *pb = &queue->prio[prio];
    uint8_t *slot = prio_slot(queue, prio, pb->tail);
    ipc_memcpy(msg, slot, queue->msg_size);

    /* Advance tail for this priority level */
    pb->tail = (pb->tail + 1) % queue->capacity;
    pb->count--;
    queue->total_count--;

    /* Update statistics */
    queue->msgs_recv++;

    /* Wake a waiting sender (any priority - they'll check their level) */
    wake_one(&queue->send_waiters);

    spin_unlock_irqrestore(&queue->lock, flags);

    return IPC_OK;
}

size_t msg_queue_count(struct msg_queue *queue)
{
    if (!queue) {
        return 0;
    }

    irq_flags_t flags = spin_lock_irqsave(&queue->lock);
    size_t count = queue->total_count;
    spin_unlock_irqrestore(&queue->lock, flags);

    return count;
}

struct msg_queue *msg_queue_lookup(uint32_t id)
{
    irq_flags_t flags = spin_lock_irqsave(&ipc_state.lock);

    struct msg_queue *queue = NULL;
    for (size_t i = 0; i < MSG_QUEUE_MAX; i++) {
        if (ipc_state.queues[i] && ipc_state.queues[i]->id == id) {
            queue = ipc_state.queues[i];
            break;
        }
    }

    spin_unlock_irqrestore(&ipc_state.lock, flags);
    return queue;
}

void msg_queue_stats(struct msg_queue *queue, uint64_t *msgs_sent,
                     uint64_t *msgs_recv, size_t *high_water)
{
    if (!queue) {
        return;
    }

    irq_flags_t flags = spin_lock_irqsave(&queue->lock);

    if (msgs_sent) {
        *msgs_sent = queue->msgs_sent;
    }
    if (msgs_recv) {
        *msgs_recv = queue->msgs_recv;
    }
    if (high_water) {
        *high_water = queue->high_water;
    }

    spin_unlock_irqrestore(&queue->lock, flags);
}

void ipc_get_stats(struct ipc_stats *stats)
{
    if (!stats) {
        return;
    }

    ipc_memset(stats, 0, sizeof(*stats));

    irq_flags_t flags = spin_lock_irqsave(&ipc_state.lock);

    /* Count queues and aggregate statistics */
    for (size_t i = 0; i < MSG_QUEUE_MAX; i++) {
        if (ipc_state.queues[i]) {
            stats->queue_count++;
            stats->total_msgs_sent += ipc_state.queues[i]->msgs_sent;
            stats->total_msgs_recv += ipc_state.queues[i]->msgs_recv;
        }
    }

    /* Count shared buffers */
    for (size_t i = 0; i < SHM_BUFFER_MAX; i++) {
        if (ipc_state.buffers[i]) {
            stats->buffer_count++;
        }
    }

    spin_unlock_irqrestore(&ipc_state.lock, flags);
}

/*
 * ==========================================================================
 * Shared Buffer Implementation
 * ==========================================================================
 */

/*
 * Allocate a shm_mapping structure.
 * Uses part of a physical page (simple bump allocator per buffer).
 */
static struct shm_mapping *alloc_mapping(void)
{
    /* For simplicity, allocate a full page per mapping.
     * A proper implementation would use a slab allocator. */
    struct shm_mapping *m = pmm_alloc_page();
    if (m) {
        ipc_memset(m, 0, sizeof(*m));
    }
    return m;
}

static void free_mapping(struct shm_mapping *m)
{
    if (m) {
        pmm_free_page(m);
    }
}

struct shared_buffer *shared_buffer_create(size_t size, uint32_t flags)
{
    if (size == 0) {
        return NULL;
    }

    /* Round up to 2MB block size */
    size = (size + BLOCK_SIZE - 1) & ~(BLOCK_SIZE - 1);
    size_t pages = size / PAGE_SIZE;

    /* Allocate buffer structure */
    struct shared_buffer *buf = pmm_alloc_page();
    if (!buf) {
        WARN("shared_buffer_create: failed to allocate descriptor");
        return NULL;
    }
    ipc_memset(buf, 0, PAGE_SIZE);

    /* Allocate backing physical memory.
     * If GPU_ACCESSIBLE is requested and GPU is available, use the GPU
     * allocator which ensures proper cache coherency attributes. */
    void *phys = NULL;
    bool gpu_backed = false;

    if ((flags & SHM_GPU_ACCESSIBLE) && gpu_available()) {
        gpu_buffer_t gpu_buf;
        uint32_t gpu_flags = GPU_MEM_READWRITE;
        if (size >= 2 * 1024 * 1024) {
            gpu_flags |= GPU_MEM_ALIGN_2MB;
        }
        int ret = gpu_alloc(size, gpu_flags, &gpu_buf);
        if (ret == GPU_OK) {
            phys = gpu_buf.cpu_addr;
            gpu_backed = true;
        }
        /* Fall through to PMM if GPU alloc fails */
    }

    if (!phys) {
        phys = pmm_alloc_pages(pages);
    }

    if (!phys) {
        WARN("shared_buffer_create: failed to allocate %zu pages", pages);
        pmm_free_page(buf);
        return NULL;
    }
    ipc_memset(phys, 0, size);

    /* Initialize buffer */
    spin_init(&buf->lock);
    buf->phys_base = phys;
    buf->size = size;
    buf->flags = flags;
    buf->refcount = 1;  /* Owner's implicit reference */
    buf->owner = task_current();
    buf->mappings = NULL;
    buf->gpu_backed = gpu_backed;

    /* Register in global table. Same fail-loud rule as msg_queue_create:
     * if the table is full, free the buffer pages and the buf page so a
     * full table doesn't silently leak system memory on every retry. */
    irq_flags_t irqflags = spin_lock_irqsave(&ipc_state.lock);

    buf->id = ipc_state.next_buffer_id++;

    bool registered = false;
    for (size_t i = 0; i < SHM_BUFFER_MAX; i++) {
        if (!ipc_state.buffers[i]) {
            ipc_state.buffers[i] = buf;
            registered = true;
            break;
        }
    }

    spin_unlock_irqrestore(&ipc_state.lock, irqflags);

    if (!registered) {
        WARN("shared_buffer_create: buffer table full (SHM_BUFFER_MAX=%d)", SHM_BUFFER_MAX);
        pmm_free_pages(phys, pages);
        pmm_free_page(buf);
        return NULL;
    }

    DEBUG_PRINT("Created shared buffer %u (size=%zu, phys=%p, flags=0x%x)",
                buf->id, size, phys, flags);

    return buf;
}

void *shared_buffer_map(struct shared_buffer *buffer, struct task *task,
                        uint32_t perms)
{
    if (!buffer) {
        return NULL;
    }

    if (!task) {
        task = task_current();
    }

    /* Limit permissions to what buffer allows */
    if ((buffer->flags & SHM_READ) == 0) {
        perms &= ~SHM_READ;
    }
    if ((buffer->flags & SHM_WRITE) == 0) {
        perms &= ~SHM_WRITE;
    }

    irq_flags_t flags = spin_lock_irqsave(&buffer->lock);

    /* Check if already mapped for this task */
    struct shm_mapping *m = buffer->mappings;
    while (m) {
        if (m->task == task) {
            spin_unlock_irqrestore(&buffer->lock, flags);
            WARN("shared_buffer_map: buffer %u already mapped for task %u",
                 buffer->id, task->id);
            return NULL;
        }
        m = m->next;
    }

    /* Create mapping structure */
    m = alloc_mapping();
    if (!m) {
        spin_unlock_irqrestore(&buffer->lock, flags);
        WARN("shared_buffer_map: failed to allocate mapping");
        return NULL;
    }

    /*
     * For Phase 2, all tasks share the kernel address space, so we
     * can use the physical address directly via identity mapping or
     * create a new high VA mapping.
     *
     * For simplicity, we return the kernel virtual address of the
     * physical memory (via identity map).
     *
     * TODO: In Phase 3 with user space, each task would get its own
     * virtual mapping in its address space.
     */
    void *virt_addr = buffer->phys_base;  /* Identity mapped in kernel */

    /* If GPU_ACCESSIBLE, clean caches so GPU sees the latest data */
    if ((perms & SHM_GPU_ACCESSIBLE) && buffer->gpu_backed) {
        gpu_buffer_t gpu_buf = {
            .cpu_addr = buffer->phys_base,
            .gpu_addr = (uint64_t)(uintptr_t)buffer->phys_base,
            .size = buffer->size,
            .flags = 0,
        };
        gpu_sync_for_gpu(&gpu_buf);
    }

    m->task = task;
    m->virt_addr = virt_addr;
    m->perms = perms;

    /* Add to mappings list */
    m->next = buffer->mappings;
    buffer->mappings = m;
    buffer->refcount++;

    spin_unlock_irqrestore(&buffer->lock, flags);

    DEBUG_PRINT("Mapped buffer %u to task %u at %p (perms=0x%x)",
                buffer->id, task->id, virt_addr, perms);

    return virt_addr;
}

int shared_buffer_unmap(struct shared_buffer *buffer, struct task *task)
{
    if (!buffer) {
        return IPC_ERR_INVALID;
    }

    if (!task) {
        task = task_current();
    }

    irq_flags_t flags = spin_lock_irqsave(&buffer->lock);

    /* Find and remove mapping */
    struct shm_mapping *prev = NULL;
    struct shm_mapping *m = buffer->mappings;

    while (m) {
        if (m->task == task) {
            /* Found it */
            if (prev) {
                prev->next = m->next;
            } else {
                buffer->mappings = m->next;
            }

            buffer->refcount--;

            spin_unlock_irqrestore(&buffer->lock, flags);

            DEBUG_PRINT("Unmapped buffer %u from task %u", buffer->id, task->id);

            free_mapping(m);
            return IPC_OK;
        }
        prev = m;
        m = m->next;
    }

    spin_unlock_irqrestore(&buffer->lock, flags);

    return IPC_ERR_INVALID;  /* Not mapped for this task */
}

int shared_buffer_destroy(struct shared_buffer *buffer)
{
    if (!buffer) {
        return IPC_ERR_INVALID;
    }

    /* Only owner can destroy */
    if (buffer->owner != task_current()) {
        WARN("shared_buffer_destroy: task %u is not owner of buffer %u",
             task_current()->id, buffer->id);
        return IPC_ERR_INVALID;
    }

    /*
     * Lock order: ipc_state.lock first, then buffer->lock; both are
     * held until we know whether destroy proceeds.
     *
     * The original code checked refcount under buffer->lock then
     * acquired ipc_state.lock separately, which let a concurrent
     * shared_buffer_lookup → shared_buffer_map find the buffer between
     * those two regions and race with our pmm_free_page below.
     *
     * Holding both locks across the refcount check makes the decision
     * atomic with respect to lookup/unmap. Lock-ordering rules across
     * the rest of the file:
     *   - shared_buffer_create takes only ipc_state.lock — OK.
     *   - shared_buffer_lookup takes only ipc_state.lock — OK.
     *   - shared_buffer_map / unmap take only buffer->lock — OK.
     * Nothing else takes buffer->lock then ipc_state.lock, so the
     * ipc_state.lock → buffer->lock order here cannot deadlock.
     *
     * Earlier review iteration cleared the slot first and tried to
     * "restore on BUSY"; that introduced a race against
     * shared_buffer_create reusing the freed slot. Holding both locks
     * avoids that entirely.
     */
    irq_flags_t s_flags = spin_lock_irqsave(&ipc_state.lock);
    int slot = -1;
    for (size_t i = 0; i < SHM_BUFFER_MAX; i++) {
        if (ipc_state.buffers[i] == buffer) {
            slot = (int)i;
            break;
        }
    }
    if (slot < 0) {
        spin_unlock_irqrestore(&ipc_state.lock, s_flags);
        return IPC_ERR_INVALID;  /* Already destroyed or never registered. */
    }

    irq_flags_t flags = spin_lock_irqsave(&buffer->lock);

    /* Cannot destroy if other tasks have it mapped. */
    if (buffer->refcount > 1) {
        spin_unlock_irqrestore(&buffer->lock, flags);
        spin_unlock_irqrestore(&ipc_state.lock, s_flags);
        WARN("shared_buffer_destroy: buffer %u has %u references",
             buffer->id, buffer->refcount);
        return IPC_ERR_BUSY;
    }

    /* Refcount OK — clear the slot under both locks so no new
     * shared_buffer_lookup can find us, then drop both. */
    ipc_state.buffers[slot] = NULL;
    spin_unlock_irqrestore(&buffer->lock, flags);
    spin_unlock_irqrestore(&ipc_state.lock, s_flags);

    /* Free physical memory */
    if (buffer->gpu_backed) {
        gpu_buffer_t gpu_buf = {
            .cpu_addr = buffer->phys_base,
            .gpu_addr = (uint64_t)(uintptr_t)buffer->phys_base,
            .size = buffer->size,
            .flags = 0,
        };
        gpu_free(&gpu_buf);
    } else {
        size_t pages = buffer->size / PAGE_SIZE;
        pmm_free_pages(buffer->phys_base, pages);
    }

    /* Free descriptor */
    pmm_free_page(buffer);

    DEBUG_PRINT("Destroyed shared buffer %u", buffer->id);

    return IPC_OK;
}

struct shared_buffer *shared_buffer_lookup(uint32_t id)
{
    irq_flags_t flags = spin_lock_irqsave(&ipc_state.lock);

    struct shared_buffer *buffer = NULL;
    for (size_t i = 0; i < SHM_BUFFER_MAX; i++) {
        if (ipc_state.buffers[i] && ipc_state.buffers[i]->id == id) {
            buffer = ipc_state.buffers[i];
            break;
        }
    }

    spin_unlock_irqrestore(&ipc_state.lock, flags);
    return buffer;
}

void *shared_buffer_phys_addr(struct shared_buffer *buffer)
{
    if (!buffer) {
        return NULL;
    }
    return buffer->phys_base;
}

/*
 * ==========================================================================
 * IPC Initialization
 * ==========================================================================
 */

void ipc_init(void)
{
    spin_init(&ipc_state.lock);
    ipc_state.next_queue_id = 1;    /* Start IDs at 1 (0 = invalid) */
    ipc_state.next_buffer_id = 1;

    for (size_t i = 0; i < MSG_QUEUE_MAX; i++) {
        ipc_state.queues[i] = NULL;
    }
    for (size_t i = 0; i < SHM_BUFFER_MAX; i++) {
        ipc_state.buffers[i] = NULL;
    }

    ipc_state.initialized = 1;

    INFO("IPC subsystem initialized");
}
