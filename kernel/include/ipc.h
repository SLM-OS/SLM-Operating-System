/*
 * ipc.h - Inter-Process Communication for SLM-OS
 *
 * Provides two-tier IPC:
 *   - Message queues: Control plane for small messages (64 bytes default)
 *   - Shared buffers: Data plane for zero-copy large transfers (tensors, etc.)
 *
 * Design rationale:
 *   - Ring buffer for messages: cache-friendly, no per-message allocation
 *   - Sleep/wake blocking: appropriate for ms-scale waits, saves CPU
 *   - Configurable message size: per-queue flexibility for future needs
 *   - Reference-counted shared buffers: safe multi-task access
 */

#ifndef IPC_H
#define IPC_H

#include <stdint.h>
#include <stddef.h>
#include "spinlock.h"
#include "task.h"
#include "config.h"

/*
 * ============================================================================
 * IPC Error Codes
 * ============================================================================
 */

#define IPC_OK              0       /* Success */
#define IPC_ERR_NOMEM      -1       /* Out of memory */
#define IPC_ERR_FULL       -2       /* Queue is full (non-blocking send) */
#define IPC_ERR_EMPTY      -3       /* Queue is empty (non-blocking recv) */
#define IPC_ERR_TIMEOUT    -4       /* Operation timed out */
#define IPC_ERR_INVALID    -5       /* Invalid parameter */
#define IPC_ERR_BUSY       -6       /* Resource busy (e.g., buffer has mappings) */

/*
 * ============================================================================
 * Message Queue
 * ============================================================================
 *
 * Fixed-size ring buffer for small messages. Each queue has a configurable
 * message size (default 64 bytes) set at creation time.
 *
 * Blocking semantics:
 *   - timeout > 0:  Block up to timeout_ms milliseconds
 *   - timeout == 0: Non-blocking, return immediately if would block
 *   - timeout < 0:  Block indefinitely
 */

/* Timeout values */
#define MSG_NO_WAIT         0       /* Non-blocking */
#define MSG_WAIT_FOREVER   -1       /* Block indefinitely */

/*
 * Default message structure (64 bytes).
 *
 * This is the recommended format for SLM-OS messages, but queues can
 * be created with any message size >= 4 bytes.
 */
struct slm_message {
    uint32_t type;              /*  4 bytes: Message type identifier */
    uint32_t flags;             /*  4 bytes: Message-specific flags */
    uint32_t sender_id;         /*  4 bytes: Sending task ID */
    uint32_t reserved;          /*  4 bytes: Reserved for future use */
    union {                     /* 48 bytes: Payload */
        uint8_t raw[48];        /* Raw byte access */
        struct {
            uint32_t buffer_handle; /* Shared buffer ID */
            uint32_t offset;        /* Offset within buffer */
            uint32_t length;        /* Data length */
            uint8_t  extra[36];     /* Additional metadata */
        } buffer_ref;           /* For shared buffer references */
        struct {
            int32_t  status;        /* Status/error code */
            uint32_t value;         /* Integer value */
            uint8_t  data[40];      /* Additional data */
        } response;             /* For response messages */
    } payload;
};

/*
 * Message queue structure.
 *
 * The ring buffer uses head/tail indices with wrap-around.
 * Buffer layout: capacity slots of msg_size bytes each.
 */
struct msg_queue {
    uint32_t        id;             /* Unique queue identifier */
    size_t          msg_size;       /* Size of each message slot (bytes) */
    size_t          capacity;       /* Number of message slots */
    size_t          head;           /* Next write position (producer) */
    size_t          tail;           /* Next read position (consumer) */
    size_t          count;          /* Messages currently in queue */
    uint8_t        *buffer;         /* Ring buffer: capacity * msg_size bytes */
    spinlock_t      lock;           /* Protects queue state */
    struct task    *send_waiters;   /* Tasks blocked on send (queue full) */
    struct task    *recv_waiters;   /* Tasks blocked on recv (queue empty) */

    /* Statistics */
    uint64_t        msgs_sent;      /* Total messages successfully sent */
    uint64_t        msgs_recv;      /* Total messages successfully received */
    size_t          high_water;     /* Peak queue depth reached */
};

/*
 * IPC statistics structure.
 */
struct ipc_stats {
    uint32_t        queue_count;    /* Number of active message queues */
    uint32_t        buffer_count;   /* Number of active shared buffers */
    uint64_t        total_msgs_sent;/* Total messages sent across all queues */
    uint64_t        total_msgs_recv;/* Total messages received across all queues */
};

/*
 * Create a new message queue.
 *
 * @capacity:  Number of message slots (must be > 0)
 * @msg_size:  Size of each message in bytes (0 = use MSG_SIZE_DEFAULT)
 *
 * Returns: Pointer to new queue, or NULL on failure.
 *
 * The queue ID can be shared with other tasks to allow them to
 * look up and use the queue.
 */
struct msg_queue *msg_queue_create(size_t capacity, size_t msg_size);

/*
 * Destroy a message queue.
 *
 * @queue: Queue to destroy
 *
 * Returns: IPC_OK on success, IPC_ERR_BUSY if tasks are waiting.
 *
 * Any messages still in the queue are discarded.
 */
int msg_queue_destroy(struct msg_queue *queue);

/*
 * Send a message to a queue.
 *
 * @queue:      Target queue
 * @msg:        Message data (must be queue->msg_size bytes)
 * @timeout_ms: Timeout in milliseconds (MSG_NO_WAIT, MSG_WAIT_FOREVER, or >0)
 *
 * Returns: IPC_OK on success, IPC_ERR_FULL (non-blocking), IPC_ERR_TIMEOUT.
 *
 * The message is copied into the queue buffer.
 */
int msg_send(struct msg_queue *queue, const void *msg, int timeout_ms);

/*
 * Receive a message from a queue.
 *
 * @queue:      Source queue
 * @msg:        Buffer to receive message (must be queue->msg_size bytes)
 * @timeout_ms: Timeout in milliseconds (MSG_NO_WAIT, MSG_WAIT_FOREVER, or >0)
 *
 * Returns: IPC_OK on success, IPC_ERR_EMPTY (non-blocking), IPC_ERR_TIMEOUT.
 *
 * The message is copied from the queue buffer to the provided buffer.
 */
int msg_recv(struct msg_queue *queue, void *msg, int timeout_ms);

/*
 * Get the number of messages currently in the queue.
 *
 * @queue: Queue to query
 *
 * Returns: Number of pending messages.
 */
size_t msg_queue_count(struct msg_queue *queue);

/*
 * Look up a queue by ID.
 *
 * @id: Queue identifier
 *
 * Returns: Pointer to queue, or NULL if not found.
 */
struct msg_queue *msg_queue_lookup(uint32_t id);

/*
 * Get message queue statistics.
 *
 * @queue: Queue to query
 * @msgs_sent: Output - total messages sent (or NULL to skip)
 * @msgs_recv: Output - total messages received (or NULL to skip)
 * @high_water: Output - peak queue depth (or NULL to skip)
 */
void msg_queue_stats(struct msg_queue *queue, uint64_t *msgs_sent,
                     uint64_t *msgs_recv, size_t *high_water);

/*
 * Get global IPC statistics.
 *
 * @stats: Pointer to stats structure to fill.
 */
void ipc_get_stats(struct ipc_stats *stats);

/*
 * ============================================================================
 * Shared Buffers
 * ============================================================================
 *
 * Zero-copy shared memory regions for large data transfers.
 * Used alongside message queues: send a buffer handle in a message,
 * receiver maps the buffer to access data directly.
 *
 * Memory is allocated in 2MB blocks (matching VMM block size).
 */

/* Shared buffer flags */
#define SHM_READ            (1 << 0)    /* Buffer is readable */
#define SHM_WRITE           (1 << 1)    /* Buffer is writable */
#define SHM_RDWR            (SHM_READ | SHM_WRITE)
#define SHM_GPU_ACCESSIBLE  (1 << 2)    /* Map with GPU-visible attributes */
#define SHM_MODEL_PAGE      (1 << 3)    /* Mark as model weight storage */
#define SHM_INFERENCE_HOT   (1 << 4)    /* Mark as hot inference data */

/*
 * Per-task mapping of a shared buffer.
 */
struct shm_mapping {
    struct task         *task;          /* Task this mapping belongs to */
    void                *virt_addr;     /* Virtual address in task's space */
    uint32_t             perms;         /* SHM_READ | SHM_WRITE */
    struct shm_mapping  *next;          /* Next mapping in list */
};

/*
 * Shared buffer descriptor.
 */
struct shared_buffer {
    uint32_t            id;             /* Unique buffer identifier */
    void               *phys_base;      /* Physical address of backing memory */
    size_t              size;           /* Size in bytes (2MB aligned) */
    uint32_t            flags;          /* SHM_* creation flags */
    uint32_t            refcount;       /* Number of active mappings + 1 (owner) */
    spinlock_t          lock;           /* Protects refcount and mappings */
    struct task        *owner;          /* Task that created the buffer */
    struct shm_mapping *mappings;       /* List of per-task mappings */
};

/*
 * Create a shared buffer.
 *
 * @size:  Requested size in bytes (rounded up to 2MB)
 * @flags: SHM_* flags (SHM_READ, SHM_WRITE, SHM_GPU_ACCESSIBLE, etc.)
 *
 * Returns: Pointer to new buffer, or NULL on failure.
 *
 * The buffer starts with refcount = 1 (owner's implicit reference).
 * The owner can write to the buffer before sharing with other tasks.
 */
struct shared_buffer *shared_buffer_create(size_t size, uint32_t flags);

/*
 * Map a shared buffer into a task's address space.
 *
 * @buffer: Buffer to map
 * @task:   Task to map into (NULL = current task)
 * @perms:  Access permissions (SHM_READ, SHM_WRITE, or SHM_RDWR)
 *
 * Returns: Virtual address of mapping, or NULL on failure.
 *
 * Permissions are limited by the buffer's creation flags.
 * Increments the buffer's refcount.
 */
void *shared_buffer_map(struct shared_buffer *buffer, struct task *task,
                        uint32_t perms);

/*
 * Unmap a shared buffer from a task's address space.
 *
 * @buffer: Buffer to unmap
 * @task:   Task to unmap from (NULL = current task)
 *
 * Returns: IPC_OK on success, IPC_ERR_INVALID if not mapped.
 *
 * Decrements the buffer's refcount.
 */
int shared_buffer_unmap(struct shared_buffer *buffer, struct task *task);

/*
 * Destroy a shared buffer.
 *
 * @buffer: Buffer to destroy
 *
 * Returns: IPC_OK on success, IPC_ERR_BUSY if other tasks have mappings.
 *
 * Only the owner can destroy a buffer, and only when refcount == 1
 * (no other tasks have it mapped).
 */
int shared_buffer_destroy(struct shared_buffer *buffer);

/*
 * Look up a shared buffer by ID.
 *
 * @id: Buffer identifier
 *
 * Returns: Pointer to buffer, or NULL if not found.
 *
 * Used by receiver to access a buffer after getting its handle in a message.
 */
struct shared_buffer *shared_buffer_lookup(uint32_t id);

/*
 * Get the physical address of a shared buffer.
 *
 * @buffer: Buffer to query
 *
 * Returns: Physical address of the buffer's backing memory.
 *
 * Useful for GPU operations that need physical addresses.
 */
void *shared_buffer_phys_addr(struct shared_buffer *buffer);

/*
 * ============================================================================
 * IPC Initialization
 * ============================================================================
 */

/*
 * Initialize the IPC subsystem.
 *
 * Must be called before creating any queues or buffers.
 */
void ipc_init(void);

#endif /* IPC_H */
