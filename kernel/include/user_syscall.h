/*
 * user_syscall.h - User-Mode Syscall Stubs for SLM-OS
 *
 * Inline assembly wrappers for making system calls from EL0.
 * User-mode components include this header instead of calling
 * kernel functions directly.
 *
 * These functions use SVC #0 with the syscall number in x8.
 */

#ifndef USER_SYSCALL_H
#define USER_SYSCALL_H

#include "syscall.h"

#if defined(__aarch64__)

/* Exit the current component */
static inline void sys_exit(int code)
{
    register uint64_t x0 __asm__("x0") = (uint64_t)(uint32_t)code;
    register uint64_t x8 __asm__("x8") = SYS_EXIT;
    __asm__ volatile("svc #0" :: "r"(x0), "r"(x8) : "memory");
    __builtin_unreachable();
}

/* Yield the CPU */
static inline void sys_yield(void)
{
    register uint64_t x8 __asm__("x8") = SYS_YIELD;
    __asm__ volatile("svc #0" :: "r"(x8) : "memory", "x0");
}

/* Sleep for the given number of milliseconds */
static inline void sys_sleep(uint32_t ms)
{
    register uint64_t x0 __asm__("x0") = (uint64_t)ms;
    register uint64_t x8 __asm__("x8") = SYS_SLEEP;
    __asm__ volatile("svc #0" :: "r"(x0), "r"(x8) : "memory");
}

/* Log a string to the kernel UART */
static inline void sys_log(const char *str, uint32_t len)
{
    register uint64_t x0 __asm__("x0") = (uint64_t)str;
    register uint64_t x1 __asm__("x1") = (uint64_t)len;
    register uint64_t x8 __asm__("x8") = SYS_LOG;
    __asm__ volatile("svc #0" :: "r"(x0), "r"(x1), "r"(x8) : "memory");
}

/* Send a message to a topic */
static inline int sys_send(const char *topic, const char *data, uint32_t len)
{
    register uint64_t x0 __asm__("x0") = (uint64_t)topic;
    register uint64_t x1 __asm__("x1") = (uint64_t)data;
    register uint64_t x2 __asm__("x2") = (uint64_t)len;
    register uint64_t x8 __asm__("x8") = SYS_SEND;
    __asm__ volatile("svc #0" : "+r"(x0) : "r"(x1), "r"(x2), "r"(x8) : "memory");
    return (int)(int64_t)x0;
}

/* Receive a message */
static inline int sys_recv(char *topic_out, char *buf, uint32_t len, uint32_t timeout_ms)
{
    register uint64_t x0 __asm__("x0") = (uint64_t)topic_out;
    register uint64_t x1 __asm__("x1") = (uint64_t)buf;
    register uint64_t x2 __asm__("x2") = (uint64_t)len;
    register uint64_t x3 __asm__("x3") = (uint64_t)timeout_ms;
    register uint64_t x8 __asm__("x8") = SYS_RECV;
    __asm__ volatile("svc #0" : "+r"(x0) : "r"(x1), "r"(x2), "r"(x3), "r"(x8) : "memory");
    return (int)(int64_t)x0;
}

/* Run inference on a loaded model */
static inline int sys_infer(uint32_t model_idx, const void *input, uint32_t in_len,
                            void *output, uint32_t out_len)
{
    register uint64_t x0 __asm__("x0") = (uint64_t)model_idx;
    register uint64_t x1 __asm__("x1") = (uint64_t)input;
    register uint64_t x2 __asm__("x2") = (uint64_t)in_len;
    register uint64_t x3 __asm__("x3") = (uint64_t)output;
    register uint64_t x4 __asm__("x4") = (uint64_t)out_len;
    register uint64_t x8 __asm__("x8") = SYS_INFER;
    __asm__ volatile("svc #0" : "+r"(x0) : "r"(x1), "r"(x2), "r"(x3), "r"(x4), "r"(x8) : "memory");
    return (int)(int64_t)x0;
}

#endif /* __aarch64__ */

#endif /* USER_SYSCALL_H */
