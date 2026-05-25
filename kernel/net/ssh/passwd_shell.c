/*
 * passwd_shell.c - `adduser` / `passwd` / `deluser` / `whoami`.
 *
 * Minimal interactive surface for #199d (#896). For this iteration
 * the password is passed on the command line:
 *
 *   adduser <name> <password>
 *   passwd  <name> <new-password>
 *   deluser <name>
 *   whoami
 *
 * That's a deliberate simplification — proper no-echo prompting
 * requires shell_io changes (#199e candidate) that don't belong in
 * the auth-correctness sub-ticket. Operators on the console see
 * what they type; on SSH (#199c) the line is encrypted in transit
 * but visible in the local terminal history.
 */

#include "passwd.h"
#include "shell.h"
#include "shell_internal.h"
#include "shell_session.h"
#include "string.h"
#include "uart.h"

#include <stdbool.h>
#include <stddef.h>

static const char *result_str(int rc)
{
    switch (rc) {
    case PASSWD_OK:           return "ok";
    case PASSWD_E_BAD_ARG:    return "bad argument";
    case PASSWD_E_NOT_FOUND:  return "user not found";
    case PASSWD_E_WRONG:      return "wrong password";
    case PASSWD_E_IO:         return "io error (no /mnt/files mount?)";
    case PASSWD_E_FULL:       return "user table full";
    case PASSWD_E_EXISTS:     return "user already exists";
    case PASSWD_E_LAST_USER:  return "refusing to remove last user";
    case PASSWD_E_FORMAT:     return "passwd file format error";
    case PASSWD_E_KDF:        return "kdf failure";
    case PASSWD_E_COST:       return "passwd entry cost factor too high (corrupt /etc/passwd?)";
    default:                  return "unknown";
    }
}

int cmd_adduser(int argc, char **argv)
{
    if (argc < 3) {
        uart_printf("usage: adduser <name> <password>\r\n");
        return -1;
    }
    int rc = passwd_adduser(argv[1], argv[2]);
    uart_printf("adduser %s: %s\r\n", argv[1], result_str(rc));
    return rc;
}

/*
 * No privilege model: any authenticated shell user can change any
 * other user's password. The whoami / per-session-identity plumbing
 * lands later in the phase (cited in the file header), and the
 * "only the current user OR an admin can call passwd" gate goes on
 * top of that. Until then, treat the shell as a single trust domain
 * — appropriate for SLM-OS's threat model where reaching the shell
 * already implies physical or authenticated access.
 */
int cmd_passwd(int argc, char **argv)
{
    if (argc < 3) {
        uart_printf("usage: passwd <name> <new-password>\r\n");
        return -1;
    }
    int rc = passwd_set(argv[1], argv[2]);
    uart_printf("passwd %s: %s\r\n", argv[1], result_str(rc));
    return rc;
}

int cmd_deluser(int argc, char **argv)
{
    if (argc < 2) {
        uart_printf("usage: deluser <name>\r\n");
        return -1;
    }
    int rc = passwd_deluser(argv[1]);
    uart_printf("deluser %s: %s\r\n", argv[1], result_str(rc));
    return rc;
}

/*
 * `whoami` returns the SSH-authenticated user when running inside an
 * SSH session, or "console" on the UART. shell_session today doesn't
 * carry the authenticated identity — #199d wires the SSH callback to
 * stash it into `shell_session.auth_user` (a new field added in this
 * patch) and falls back to "console" otherwise.
 *
 * For this initial commit, we only have the console-path semantics —
 * SSH sessions also report "console" until the per-session identity
 * plumbing lands. Documenting up-front: the bootstrap gate works
 * (passwd_any_users) but the user-identity carry-through is partial.
 */
int cmd_whoami(int argc, char **argv)
{
    (void)argc; (void)argv;
    struct shell_session *s = shell_session_current();
    if (s && s->id == 0u) {
        uart_printf("console\r\n");
    } else if (s) {
        uart_printf("ssh\r\n");   /* placeholder until #199d-2 plumbs identity */
    } else {
        uart_printf("?\r\n");
    }
    return 0;
}
