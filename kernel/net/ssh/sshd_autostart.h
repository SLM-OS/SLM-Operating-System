/*
 * sshd_autostart.h - Boot-time entry for the SSH daemon.
 *
 * Modeled on `kernel/include/telnetd_autostart.h`. Called once from
 * shell_init at boot; either:
 *
 *   1. /mnt/files/etc/sshd.conf says enable=1  →  start sshd on the
 *      configured port.
 *   2. NET_SSHD_AUTOSTART compile define is set  →  start on the
 *      compile-default port.
 *   3. Otherwise, silent no-op.
 *
 * Default for #199c / #891 is OFF — the bootstrap gate that makes a
 * default-on flip safe lives in #199d / #896, and the flip itself
 * happens in #199e / #895. This module is the wiring; the policy
 * sits in CMakeLists.txt's NET_SSHD_AUTOSTART option.
 */

#ifndef SSHD_AUTOSTART_H
#define SSHD_AUTOSTART_H

void sshd_autostart(void);

#endif /* SSHD_AUTOSTART_H */
