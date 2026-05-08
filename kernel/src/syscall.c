/*
 * syscall.c - System Call Dispatch for SLM-OS
 *
 * Handles system calls from EL0 (user-mode) components.
 * Each syscall handler receives the trap frame and can read arguments
 * from x0-x5 and write the return value to x0.
 */

#include "syscall.h"
#include "trap.h"
#include "task.h"
#include "sched.h"
#include "uart.h"
#include "timer.h"
#include "slm_ffi.h"
#include "vmm.h"
#include "pmm.h"
#include "string.h"
#include <stdint.h>
#include <stddef.h>

/*
 * Scan a validated user buffer for a NUL byte within [0, max_len).
 * The caller must have already validated the pointer is readable for
 * max_len bytes via validate_user_ptr(). Returns 1 if a NUL was found,
 * 0 otherwise.
 */
static int user_str_has_nul(const char *p, size_t max_len)
{
    for (size_t i = 0; i < max_len; i++) {
        if (p[i] == '\0') return 1;
    }
    return 0;
}

/* ============================================================================
 * Pointer Validation
 * ============================================================================ */

/*
 * Basic user pointer validation.
 *
 * For Phase 5 (shared address space, no per-component page tables),
 * this checks that the pointer is within the task's stack region.
 * Future: validate against per-component page table mappings.
 */
static int validate_user_ptr(const void *ptr, size_t len)
{
    if (!ptr || len == 0) return 0;  /* NULL or empty is "valid" (no access) */

    struct task *t = task_current();
    if (!t || !t->stack_base) return 0;

    uintptr_t start = (uintptr_t)ptr;
    uintptr_t end = start + len;

    /* Overflow check */
    if (end < start) return 0;

    /* For now, allow any kernel-mapped address (shared address space) */
    /* Future: restrict to component's own memory region */
    (void)t;
    return 1;
}

/* ============================================================================
 * Syscall Handlers
 * ============================================================================ */

/* SYS_EXIT: Terminate the calling component */
static int64_t sys_exit_handler(struct trap_frame *frame)
{
    int code = (int)frame->x0;
    (void)code;

    struct task *t = task_current();
    if (t) {
        uart_printf("[SYSCALL] Task '%s' exiting with code %d\r\n", t->name, code);
    }

    /* Mark task as terminated and schedule away */
    task_exit();

    /* Never reached */
    return 0;
}

/* SYS_YIELD: Voluntarily yield the CPU */
static int64_t sys_yield_handler(struct trap_frame *frame)
{
    (void)frame;
    yield();
    return 0;
}

/* SYS_SEND: Publish a message to a topic */
static int64_t sys_send_handler(struct trap_frame *frame)
{
    const char *topic = (const char *)frame->x0;
    const char *data = (const char *)frame->x1;
    uint32_t len = (uint32_t)frame->x2;

    /* Topic: must be a NUL-terminated C string within the router's
     * topic-name window. Validate the whole window is readable before
     * scanning, then require a terminator within it. */
    if (!validate_user_ptr(topic, MSG_ROUTER_TOPIC_LEN)) {
        return -1;
    }
    if (!user_str_has_nul(topic, MSG_ROUTER_TOPIC_LEN)) {
        return -1;
    }

    /* Data: caller-sized, but the Rust router treats it as a C string.
     * Require non-empty and explicit NUL in the final byte. */
    if (len == 0 || !validate_user_ptr(data, len) || data[len - 1] != '\0') {
        return -1;
    }

    /* Use the Rust message router (declared in slm_ffi.h) */
    return msg_router_publish((const uint8_t *)topic, (const uint8_t *)data);
}

/* SYS_RECV: Receive a message */
static int64_t sys_recv_handler(struct trap_frame *frame)
{
    char *topic_out = (char *)frame->x0;
    char *buf = (char *)frame->x1;
    uint32_t len = (uint32_t)frame->x2;
    uint32_t timeout_ms = (uint32_t)frame->x3;

    if (!validate_user_ptr(buf, len)) {
        return -1;
    }
    if (!validate_user_ptr(topic_out, MSG_ROUTER_TOPIC_LEN)) {
        return -1;
    }

    /* Use the Rust message router receive */
    extern const uint8_t *msg_router_receive(int32_t component_idx, uint8_t *topic_out);
    struct task *t = task_current();
    int comp_idx = t ? (int)t->id : -1;

    const uint8_t *data = msg_router_receive(comp_idx, (uint8_t *)topic_out);
    if (data) {
        /* Copy data to user buffer */
        for (uint32_t i = 0; i < len && data[i]; i++) {
            buf[i] = (char)data[i];
        }
        /* Acknowledge receipt */
        extern void msg_router_ack(int32_t component_idx);
        msg_router_ack(comp_idx);
        return 0;
    }

    (void)timeout_ms;  /* TODO: implement polling with timeout */
    return -1;  /* No message available */
}

/* SYS_INFER: Run inference on a loaded model */
static int64_t sys_infer_handler(struct trap_frame *frame)
{
    uint32_t model_idx = (uint32_t)frame->x0;
    const void *input = (const void *)frame->x1;
    uint32_t in_len = (uint32_t)frame->x2;
    void *output = (void *)frame->x3;
    uint32_t out_len = (uint32_t)frame->x4;

    /* Reject lengths that would overflow `len * sizeof(float)` before the
     * validate_user_ptr bounds check. */
    if (in_len > UINT32_MAX / 4 || out_len > UINT32_MAX / 4) {
        return -1;
    }
    if (!validate_user_ptr(input, in_len * 4) || !validate_user_ptr(output, out_len * 4)) {
        return -1;
    }

    return rust_infer(model_idx, (const float *)input, in_len,
                      (float *)output, out_len);
}

/* SYS_SLEEP: Sleep for N milliseconds */
static int64_t sys_sleep_handler(struct trap_frame *frame)
{
    uint32_t ms = (uint32_t)frame->x0;
    slm_sleep_ms(ms);
    return 0;
}

/* SYS_LOG: Write a string to the kernel UART */
static int64_t sys_log_handler(struct trap_frame *frame)
{
    const char *str = (const char *)frame->x0;
    uint32_t len = (uint32_t)frame->x1;

    if (!validate_user_ptr(str, len)) {
        return -1;
    }

    /* Print character by character (safe, bounded) */
    for (uint32_t i = 0; i < len && i < 256; i++) {
        uart_putc(str[i]);
    }
    return 0;
}

/* SYS_MMAP: Allocate and map zero-filled anonymous user pages.
 *
 *   x0 = hint (advisory; ignored today)
 *   x1 = len in bytes (rounded up to PAGE_SIZE)
 *   x2 = prot bits (PROT_READ | PROT_WRITE | PROT_EXEC)
 *   x3 = flags (reserved; today implicitly MAP_ANONYMOUS)
 *
 * Returns the user VA of the new mapping in x0, or -1 on failure
 * (overlong len, PMM exhausted, user-VA window exhausted, called
 * from a kernel-mode task). Pages are zero-filled before mapping.
 *
 * Allocation is via the per-task `user_va_next` bump cursor. Fresh
 * VA never aliases prior TLB entries from a prior munmap, so no
 * TLB invalidation is needed at install time.
 */
static int64_t sys_mmap_handler(struct trap_frame *frame)
{
    (void)frame->x0;   /* hint — ignored */
    uint64_t len    = frame->x1;
    uint32_t prot   = (uint32_t)frame->x2;
    uint32_t flags  = (uint32_t)frame->x3;
    (void)flags;       /* MAP_ANONYMOUS implicit */

    /* Reject zero / absurdly-large requests. 64 MB cap is well above
     * any realistic per-call mmap and below the user-VA window's
     * 256 GB ceiling — guards against integer-overflow shenanigans
     * in the page-count arithmetic below. */
    if (len == 0 || len > (64UL * 1024 * 1024)) {
        return -1;
    }

    struct task *t = task_current();
    if (!t || !t->is_user || !t->user_l1_pa) {
        return -1;
    }

    uint64_t pages = (len + PAGE_SIZE - 1) / PAGE_SIZE;
    uint64_t total = pages * PAGE_SIZE;

    /* user_va_next is monotonic; cursor exhaustion = -1. */
    if (t->user_va_next < USER_MMAP_VA_START ||
        t->user_va_next + total > USER_VA_LIMIT) {
        return -1;
    }
    uint64_t base_va = t->user_va_next;

    /* Build the per-page VMM flags from the user's prot bits. PMM_OWNED
     * tells vmm_destroy_user_l1 (and vmm_user_unmap_page) to reclaim
     * the leaf when the mapping is torn down. */
    uint32_t vmm_flags = VMM_FLAG_USER | VMM_FLAG_PMM_OWNED;
    if (prot & PROT_READ)  vmm_flags |= VMM_FLAG_READ;
    if (prot & PROT_WRITE) vmm_flags |= VMM_FLAG_WRITE;
    if (prot & PROT_EXEC)  vmm_flags |= VMM_FLAG_EXEC;

    /* Allocate + map each page. On any failure mid-loop, roll back
     * by unmapping the partial range — vmm_user_unmap_page sees the
     * PMM_OWNED bit and frees the leaf back to PMM. */
    for (uint64_t i = 0; i < pages; i++) {
        void *page = pmm_alloc_pages(1);
        if (!page) {
            for (uint64_t j = 0; j < i; j++) {
                vmm_user_unmap_page(t->user_l1_pa, base_va + j * PAGE_SIZE);
            }
            return -1;
        }
        memset(page, 0, PAGE_SIZE);

        if (vmm_user_map_page(t->user_l1_pa, base_va + i * PAGE_SIZE,
                              (uint64_t)(uintptr_t)page, vmm_flags) != 0) {
            pmm_free_pages(page, 1);
            for (uint64_t j = 0; j < i; j++) {
                vmm_user_unmap_page(t->user_l1_pa, base_va + j * PAGE_SIZE);
            }
            return -1;
        }
    }

    t->user_va_next = base_va + total;
    return (int64_t)base_va;
}

/* SYS_MUNMAP: Tear down pages previously returned by sys_mmap.
 *
 *   x0 = addr (must be page-aligned, in the mmap window)
 *   x1 = len in bytes (rounded up to PAGE_SIZE)
 *
 * Returns 0 on success, -1 on validation failure. Each page is
 * unmapped and its PMM-owned leaf freed via vmm_user_unmap_page,
 * then the TLB is invalidated for the range so a subsequent EL0
 * access faults instead of hitting a stale entry.
 */
static int64_t sys_munmap_handler(struct trap_frame *frame)
{
    uint64_t addr = frame->x0;
    uint64_t len  = frame->x1;

    if (len == 0 || len > (64UL * 1024 * 1024)) {
        return -1;
    }
    if ((addr & (PAGE_SIZE - 1)) != 0) {
        return -1;
    }

    struct task *t = task_current();
    if (!t || !t->is_user || !t->user_l1_pa) {
        return -1;
    }

    /* Only the mmap window is munmap-able; .text.user / stack are
     * managed by task_create_user / task_destroy. */
    if (addr < USER_MMAP_VA_START || addr >= USER_VA_LIMIT) {
        return -1;
    }
    uint64_t pages = (len + PAGE_SIZE - 1) / PAGE_SIZE;
    if (addr + pages * PAGE_SIZE > USER_VA_LIMIT) {
        return -1;
    }

    int rc = 0;
    for (uint64_t i = 0; i < pages; i++) {
        if (vmm_user_unmap_page(t->user_l1_pa, addr + i * PAGE_SIZE) != 0) {
            rc = -1;
        }
    }

    /* The task's L1 is the active TTBR0_EL1 right now (we got here
     * via SVC from EL0), so stale entries would shadow the unmap.
     * Flush the range. */
    vmm_invalidate_tlb_range(addr, addr + pages * PAGE_SIZE);
    return rc;
}

/* SYS_TOUCH_BLOCK: Touch a model memory block (update LRU timestamp).
 * x0 = block_index (uint16), x1 = pool_id (uint8), x2 = generation (uint8).
 * Packed into a ModelHandle and forwarded to rust_model_touch.
 * Returns 0 on success, -1 on invalid handle. #123. */
static int64_t sys_touch_block_handler(struct trap_frame *frame)
{
    /* Reconstruct ModelHandle from individual fields passed in
     * registers. User code cannot fabricate a valid handle without
     * the generation — stale or forged handles are caught by the
     * Rust side's generation check. */
    typedef struct { uint16_t block_index; uint8_t pool_id;
                     uint8_t generation; uint32_t _reserved; } Handle;
    Handle h;
    h.block_index = (uint16_t)frame->x0;
    h.pool_id     = (uint8_t)frame->x1;
    h.generation  = (uint8_t)frame->x2;
    h._reserved   = 0;

    extern int rust_model_touch(Handle handle);
    return rust_model_touch(h);
}

/* ============================================================================
 * Dispatch Table
 * ============================================================================ */

typedef int64_t (*syscall_handler_t)(struct trap_frame *frame);

static syscall_handler_t syscall_table[SYS_MAX] = {
    [SYS_EXIT]  = sys_exit_handler,
    [SYS_YIELD] = sys_yield_handler,
    [SYS_SEND]  = sys_send_handler,
    [SYS_RECV]  = sys_recv_handler,
    [SYS_INFER] = sys_infer_handler,
    [SYS_SLEEP] = sys_sleep_handler,
    [SYS_LOG]         = sys_log_handler,
    [SYS_TOUCH_BLOCK] = sys_touch_block_handler,
    [SYS_MMAP]   = sys_mmap_handler,
    [SYS_MUNMAP] = sys_munmap_handler,
};

void syscall_dispatch(struct trap_frame *frame)
{
    uint64_t sysno = frame->x8;

    if (sysno >= SYS_MAX) {
        uart_printf("[SYSCALL] Invalid syscall %lu from task '%s'\r\n",
                    (unsigned long)sysno,
                    task_current() ? task_current()->name : "?");
        frame->x0 = (uint64_t)(int64_t)-1;
        return;
    }

    syscall_handler_t handler = syscall_table[sysno];
    if (!handler) {
        frame->x0 = (uint64_t)(int64_t)-1;
        return;
    }

    int64_t result = handler(frame);
    frame->x0 = (uint64_t)result;
}
