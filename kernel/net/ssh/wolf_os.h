/*
 * wolf_os.h - SLM-OS-side wolfSSL / wolfSSH glue.
 *
 * Public surface of `kernel/net/ssh/wolf_os.c`. Currently just the
 * WLOG callback; XMALLOC / XFREE / XREALLOC are exposed to wolfssl
 * via the `XMALLOC_USER` mechanism, not consumed by SLM-OS code
 * directly. The wolf_heap allocator backing those macros has its
 * own header (`kernel/net/ssh/wolf_heap.h`).
 *
 * Nothing outside `kernel/net/ssh/` should include this — the glue
 * is a wolfssl-side concern.
 */

#ifndef SLMOS_WOLF_OS_H
#define SLMOS_WOLF_OS_H

#include <wolfssh/log.h>

/* Routes wolfSSH WLOG output to uart_printf. Registered once via
 * `wolfSSH_SetLoggingCb(slm_wolfssh_log_cb)` from sshd_start; the
 * callback itself is a no-op unless wolfSSH was built with
 * DEBUG_WOLFSSH (off by default in user_settings.h). */
void slm_wolfssh_log_cb(enum wolfSSH_LogLevel level, const char *const msg);

#endif /* SLMOS_WOLF_OS_H */
