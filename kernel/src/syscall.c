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
    [SYS_LOG]   = sys_log_handler,
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
