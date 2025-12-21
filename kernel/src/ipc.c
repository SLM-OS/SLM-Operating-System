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
#include <stddef.h>

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
 * Blocking helpers
 * ==========================================================================
 *
 * For Phase 2, we implement simple blocking by removing the task from
 * the run queue (BLOCKED state) and re-adding when the condition is met.
 *
 * TODO: Implement timeout support (requires timer integration).
 * For now, timeout > 0 behaves like MSG_WAIT_FOREVER.
 */

/*
 * Block the current task on a wait queue.
 * Caller must hold queue->lock.
 */
static void block_on_queue(struct task **wait_queue, spinlock_t *lock)
{
    struct task *current = task_current();

    /* Add to wait queue (simple linked list) */
    current->next = *wait_queue;
    *wait_queue = current;

    /* Mark as blocked and remove from scheduler */
    current->state = TASK_BLOCKED;

    /* Release lock before yielding (will be re-acquired after wake) */
    spin_unlock(lock);

    /* Yield - scheduler will skip this task until woken */
    yield();

    /* Re-acquire lock after waking */
    spin_lock(lock);
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
    if (capacity == 0) {
        return NULL;
    }

    if (msg_size == 0) {
        msg_size = MSG_SIZE_DEFAULT;
    }

    /* Allocate queue structure (1 page is more than enough) */
    struct msg_queue *queue = pmm_alloc_page();
    if (!queue) {
        WARN("msg_queue_create: failed to allocate queue structure");
        return NULL;
    }
    ipc_memset(queue, 0, PAGE_SIZE);

    /* Calculate buffer size and allocate */
    size_t buffer_size = capacity * msg_size;
    size_t pages_needed = (buffer_size + PAGE_SIZE - 1) / PAGE_SIZE;

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
    queue->head = 0;
    queue->tail = 0;
    queue->count = 0;
    queue->send_waiters = NULL;
    queue->recv_waiters = NULL;

    /* Register in global table */
    irq_flags_t flags = spin_lock_irqsave(&ipc_state.lock);

    uint32_t id = ipc_state.next_queue_id++;
    queue->id = id;

    /* Find free slot */
    for (size_t i = 0; i < MSG_QUEUE_MAX; i++) {
        if (!ipc_state.queues[i]) {
            ipc_state.queues[i] = queue;
            break;
        }
    }

    spin_unlock_irqrestore(&ipc_state.lock, flags);

    DEBUG_PRINT("Created message queue %u (capacity=%zu, msg_size=%zu)",
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
    size_t buffer_size = queue->capacity * queue->msg_size;
    size_t pages = (buffer_size + PAGE_SIZE - 1) / PAGE_SIZE;
    pmm_free_pages(queue->buffer, pages);
    pmm_free_page(queue);

    DEBUG_PRINT("Destroyed message queue %u", queue->id);

    return IPC_OK;
}

int msg_send(struct msg_queue *queue, const void *msg, int timeout_ms)
{
    if (!queue || !msg) {
        return IPC_ERR_INVALID;
    }

    irq_flags_t flags = spin_lock_irqsave(&queue->lock);

    /* Wait for space if queue is full */
    while (queue->count >= queue->capacity) {
        if (timeout_ms == MSG_NO_WAIT) {
            spin_unlock_irqrestore(&queue->lock, flags);
            return IPC_ERR_FULL;
        }

        /* Block until space available */
        /* Note: block_on_queue releases and re-acquires lock */
        irq_restore(flags);  /* Re-enable IRQs for blocking */
        block_on_queue(&queue->send_waiters, &queue->lock);
        flags = irq_save();  /* Disable again for consistency */
    }

    /* Copy message to buffer */
    uint8_t *slot = queue->buffer + (queue->head * queue->msg_size);
    ipc_memcpy(slot, msg, queue->msg_size);

    /* Advance head */
    queue->head = (queue->head + 1) % queue->capacity;
    queue->count++;

    /* Wake a waiting receiver */
    wake_one(&queue->recv_waiters);

    spin_unlock_irqrestore(&queue->lock, flags);

    return IPC_OK;
}

int msg_recv(struct msg_queue *queue, void *msg, int timeout_ms)
{
    if (!queue || !msg) {
        return IPC_ERR_INVALID;
    }

    irq_flags_t flags = spin_lock_irqsave(&queue->lock);

    /* Wait for message if queue is empty */
    while (queue->count == 0) {
        if (timeout_ms == MSG_NO_WAIT) {
            spin_unlock_irqrestore(&queue->lock, flags);
            return IPC_ERR_EMPTY;
        }

        /* Block until message available */
        irq_restore(flags);
        block_on_queue(&queue->recv_waiters, &queue->lock);
        flags = irq_save();
    }

    /* Copy message from buffer */
    uint8_t *slot = queue->buffer + (queue->tail * queue->msg_size);
    ipc_memcpy(msg, slot, queue->msg_size);

    /* Advance tail */
    queue->tail = (queue->tail + 1) % queue->capacity;
    queue->count--;

    /* Wake a waiting sender */
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
    size_t count = queue->count;
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

    /* Allocate backing physical memory */
    void *phys = pmm_alloc_pages(pages);
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

    /* Register in global table */
    irq_flags_t irqflags = spin_lock_irqsave(&ipc_state.lock);

    buf->id = ipc_state.next_buffer_id++;

    for (size_t i = 0; i < SHM_BUFFER_MAX; i++) {
        if (!ipc_state.buffers[i]) {
            ipc_state.buffers[i] = buf;
            break;
        }
    }

    spin_unlock_irqrestore(&ipc_state.lock, irqflags);

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

    /* If GPU_ACCESSIBLE is requested, set the appropriate page flags */
    if (perms & SHM_GPU_ACCESSIBLE) {
        /* Page table flags already set by vmm if needed */
        /* For now, physical memory is accessible to GPU via identity map */
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

    irq_flags_t flags = spin_lock_irqsave(&buffer->lock);

    /* Cannot destroy if other tasks have it mapped */
    if (buffer->refcount > 1) {
        spin_unlock_irqrestore(&buffer->lock, flags);
        WARN("shared_buffer_destroy: buffer %u has %u references",
             buffer->id, buffer->refcount);
        return IPC_ERR_BUSY;
    }

    spin_unlock_irqrestore(&buffer->lock, flags);

    /* Remove from global table */
    flags = spin_lock_irqsave(&ipc_state.lock);
    for (size_t i = 0; i < SHM_BUFFER_MAX; i++) {
        if (ipc_state.buffers[i] == buffer) {
            ipc_state.buffers[i] = NULL;
            break;
        }
    }
    spin_unlock_irqrestore(&ipc_state.lock, flags);

    /* Free physical memory and descriptor */
    size_t pages = buffer->size / PAGE_SIZE;
    pmm_free_pages(buffer->phys_base, pages);
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

/*
 * ==========================================================================
 * IPC Tests
 * ==========================================================================
 */

int ipc_run_tests(void)
{
    int errors = 0;

    uart_puts("\n[IPC] Running IPC tests...\n");

    /*
     * Test 1: Message queue create/destroy
     */
    {
        struct msg_queue *q = msg_queue_create(8, 0);  /* 8 slots, default size */
        if (q && q->capacity == 8 && q->msg_size == MSG_SIZE_DEFAULT) {
            uart_puts("  [PASS] Message queue create (8 slots, 64 bytes)\n");
        } else {
            uart_puts("  [FAIL] Message queue create\n");
            errors++;
        }

        if (q) {
            int ret = msg_queue_destroy(q);
            if (ret == IPC_OK) {
                uart_puts("  [PASS] Message queue destroy\n");
            } else {
                uart_puts("  [FAIL] Message queue destroy\n");
                errors++;
            }
        }
    }

    /*
     * Test 2: Non-blocking send/recv
     */
    {
        struct msg_queue *q = msg_queue_create(4, sizeof(uint32_t));
        if (!q) {
            uart_puts("  [FAIL] Queue create for send/recv test\n");
            errors++;
        } else {
            uint32_t send_val = 0x12345678;
            uint32_t recv_val = 0;

            /* Send one message */
            int ret = msg_send(q, &send_val, MSG_NO_WAIT);
            if (ret == IPC_OK && msg_queue_count(q) == 1) {
                uart_puts("  [PASS] Non-blocking send\n");
            } else {
                uart_puts("  [FAIL] Non-blocking send\n");
                errors++;
            }

            /* Receive it back */
            ret = msg_recv(q, &recv_val, MSG_NO_WAIT);
            if (ret == IPC_OK && recv_val == send_val && msg_queue_count(q) == 0) {
                uart_puts("  [PASS] Non-blocking recv (value matches)\n");
            } else {
                uart_printf("  [FAIL] Non-blocking recv (got 0x%x, expected 0x%x)\n",
                           recv_val, send_val);
                errors++;
            }

            /* Recv on empty queue should fail */
            ret = msg_recv(q, &recv_val, MSG_NO_WAIT);
            if (ret == IPC_ERR_EMPTY) {
                uart_puts("  [PASS] Recv on empty returns IPC_ERR_EMPTY\n");
            } else {
                uart_printf("  [FAIL] Recv on empty returned %d\n", ret);
                errors++;
            }

            /* Fill queue and test full condition */
            for (int i = 0; i < 4; i++) {
                send_val = i;
                msg_send(q, &send_val, MSG_NO_WAIT);
            }
            send_val = 99;
            ret = msg_send(q, &send_val, MSG_NO_WAIT);
            if (ret == IPC_ERR_FULL) {
                uart_puts("  [PASS] Send on full returns IPC_ERR_FULL\n");
            } else {
                uart_printf("  [FAIL] Send on full returned %d\n", ret);
                errors++;
            }

            msg_queue_destroy(q);
        }
    }

    /*
     * Test 3: Queue lookup by ID
     */
    {
        struct msg_queue *q = msg_queue_create(4, 32);
        if (!q) {
            uart_puts("  [FAIL] Queue create for lookup test\n");
            errors++;
        } else {
            uint32_t id = q->id;
            struct msg_queue *found = msg_queue_lookup(id);
            if (found == q) {
                uart_puts("  [PASS] Queue lookup by ID\n");
            } else {
                uart_puts("  [FAIL] Queue lookup by ID\n");
                errors++;
            }

            msg_queue_destroy(q);

            /* Lookup after destroy should fail */
            found = msg_queue_lookup(id);
            if (found == NULL) {
                uart_puts("  [PASS] Queue lookup after destroy returns NULL\n");
            } else {
                uart_puts("  [FAIL] Queue lookup after destroy returned non-NULL\n");
                errors++;
            }
        }
    }

    /*
     * Test 4: Shared buffer create/destroy
     */
    {
        struct shared_buffer *buf = shared_buffer_create(4096, SHM_RDWR);
        if (buf && buf->size == BLOCK_SIZE && buf->refcount == 1) {
            uart_printf("  [PASS] Shared buffer create (size rounded to %u)\n",
                       (unsigned)buf->size);
        } else {
            uart_puts("  [FAIL] Shared buffer create\n");
            errors++;
        }

        if (buf) {
            int ret = shared_buffer_destroy(buf);
            if (ret == IPC_OK) {
                uart_puts("  [PASS] Shared buffer destroy\n");
            } else {
                uart_puts("  [FAIL] Shared buffer destroy\n");
                errors++;
            }
        }
    }

    /*
     * Test 5: Shared buffer map/unmap
     */
    {
        struct shared_buffer *buf = shared_buffer_create(BLOCK_SIZE, SHM_RDWR);
        if (!buf) {
            uart_puts("  [FAIL] Buffer create for map test\n");
            errors++;
        } else {
            void *addr = shared_buffer_map(buf, NULL, SHM_RDWR);
            if (addr && buf->refcount == 2) {
                uart_puts("  [PASS] Shared buffer map\n");

                /* Write and read back */
                volatile uint32_t *p = (volatile uint32_t *)addr;
                *p = 0xDEADBEEF;
                if (*p == 0xDEADBEEF) {
                    uart_puts("  [PASS] Shared buffer read/write\n");
                } else {
                    uart_puts("  [FAIL] Shared buffer read/write\n");
                    errors++;
                }

                int ret = shared_buffer_unmap(buf, NULL);
                if (ret == IPC_OK && buf->refcount == 1) {
                    uart_puts("  [PASS] Shared buffer unmap\n");
                } else {
                    uart_puts("  [FAIL] Shared buffer unmap\n");
                    errors++;
                }
            } else {
                uart_puts("  [FAIL] Shared buffer map\n");
                errors++;
            }

            shared_buffer_destroy(buf);
        }
    }

    /*
     * Test 6: Buffer lookup by ID
     */
    {
        struct shared_buffer *buf = shared_buffer_create(BLOCK_SIZE, SHM_READ);
        if (!buf) {
            uart_puts("  [FAIL] Buffer create for lookup test\n");
            errors++;
        } else {
            uint32_t id = buf->id;
            struct shared_buffer *found = shared_buffer_lookup(id);
            if (found == buf) {
                uart_puts("  [PASS] Buffer lookup by ID\n");
            } else {
                uart_puts("  [FAIL] Buffer lookup by ID\n");
                errors++;
            }

            shared_buffer_destroy(buf);

            found = shared_buffer_lookup(id);
            if (found == NULL) {
                uart_puts("  [PASS] Buffer lookup after destroy returns NULL\n");
            } else {
                uart_puts("  [FAIL] Buffer lookup after destroy returned non-NULL\n");
                errors++;
            }
        }
    }

    /* Summary */
    if (errors == 0) {
        INFO("IPC tests passed");
    } else {
        uart_printf("  [FAIL] IPC tests: %d errors\n", errors);
    }

    return errors;
}
