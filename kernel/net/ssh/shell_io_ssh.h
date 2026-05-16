/*
 * shell_io_ssh.h - wolfSSH-backed shell_io constructor.
 *
 * Allocates a shell_io that drives I/O through a wolfSSH stream
 * (see kernel/net/ssh/shell_io_ssh.c). Caller-owned: destroy with
 * shell_io_ssh_destroy when the session ends.
 *
 * Note: the WOLFSSH pointer is borrowed — the caller is responsible
 * for keeping it alive for the lifetime of this shell_io and for
 * freeing it (`wolfSSH_free`) after `shell_io_ssh_destroy`.
 */

#ifndef SHELL_IO_SSH_H
#define SHELL_IO_SSH_H

#include "shell_io.h"

#include <wolfssh/ssh.h>

struct shell_io *shell_io_ssh_create(WOLFSSH *ssh);
void             shell_io_ssh_destroy(struct shell_io *io);

/* Mark the backend closed so the next read/write returns the
 * "session ended" signal. Used by sshd.c when the lwIP layer
 * reports peer FIN/RST. */
void             shell_io_ssh_mark_closed(struct shell_io *io);

#endif /* SHELL_IO_SSH_H */
