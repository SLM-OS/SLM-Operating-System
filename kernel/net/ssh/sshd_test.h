/*
 * sshd_test.h - Test-only hooks into sshd.c internals.
 *
 * Consumed only by kernel/tests/test_sshd.c. Implementations live at
 * the bottom of kernel/net/ssh/sshd.c (always compiled when NET_SSHD
 * is on; dead-stripped from production kernels via --gc-sections).
 *
 * The hooks take and return `void *` rather than `struct sshd_conn *`
 * because that struct is deliberately opaque outside sshd.c.
 */

#ifndef SSHD_TEST_H
#define SSHD_TEST_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Rent a slot from the per-listener connection pool. Returns NULL if
 * SSHD_MAX_SESSIONS slots are already in use. */
void    *sshd_test_take_slot(void);
void     sshd_test_release_slot(void *handle);

/* Drop the `in_use` flag on every slot. Defensive reset called at the
 * start of each test that takes slots; tolerates earlier tests that
 * longjmp'd out of an assertion without releasing what they held. */
void     sshd_test_release_all_slots(void);

/* Direct access to the per-conn RX ring. */
size_t   sshd_test_ring_push(void *handle, const void *src, size_t n);
size_t   sshd_test_ring_pop (void *handle, void *dst,       size_t n);
uint16_t sshd_test_ring_free(void *handle);

/* Usable capacity (bytes). One less than the underlying buffer size
 * because the ring reserves a byte for full-vs-empty disambiguation. */
size_t   sshd_test_ring_capacity(void);

/* Pre-auth deadline predicate used by the session task (#1067). */
bool     sshd_test_preauth_expired(uint64_t start, uint64_t now,
                                   uint64_t freq, uint32_t limit_s);
uint32_t sshd_test_preauth_timeout_s(void);

#endif /* SSHD_TEST_H */
