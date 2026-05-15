/*
 * shell_fs.c - Filesystem commands for SLM-OS shell
 *
 * Commands: ls, cd, pwd, cat, write, mkdir, rm, mv, df, truncate,
 *           append, cp, touch, stat, tree, wc, hexdump, grep, find
 */

#include "shell.h"
#include "shell_internal.h"
#include "shell_session.h"
#include "uart.h"
#include "vfs.h"
#include "littlefs_slm.h"
#include "string.h"
#include "timer.h"
#include "debug.h"
#include <stdint.h>
#include <stddef.h>

/* Threshold for the per-chunk LFS write timing log emitted from
 * cmd_xput chunk. A healthy 16 KB write on RAM-backed LittleFS is
 * ~30-60 ms; 250 ms is roughly 4× normal and only fires when the
 * write path is genuinely degraded (the symptom #597 documents).
 * Threshold-gated so a steady-state upload does not spam the kernel
 * UART with one INFO line per chunk. The log goes to uart_printf
 * (kernel UART), not the shell session, so it is invisible to the
 * telnet client driving the upload — i.e. it cannot break the
 * line-oriented xput protocol. */
#define XPUT_SLOW_WRITE_THRESHOLD_MS 250UL

static uint32_t shell_checksum32_update(uint32_t checksum,
                                        const uint8_t *data,
                                        size_t len)
{
    for (size_t i = 0; i < len; i++) {
        checksum ^= data[i];
        checksum *= 0x01000193u;
    }
    return checksum;
}

/* Close a held xput fd if any, then zero the session-specific
 * upload state. Safe to call when no fd is open (the close path
 * is gated on `mnt` being non-NULL — which is the authoritative
 * "fd is held" signal; do NOT use `fd < 0` as a sentinel because
 * encode_file_handle in littlefs_slm.c packs the generation into
 * bits 16-31, so any handle with `gen & 0x8000` is negative when
 * interpreted as a signed int even though it's perfectly valid).
 * Used by `xput begin` (reset before re-init), `xput abort`
 * (explicit teardown), `xput finish` (post-validation cleanup),
 * and `shell_xput_session_close_for` (session-free defensive
 * cleanup called from shell_session_free when the session is
 * being torn down — covers the case where a session task exits
 * mid-upload without going through abort/finish, so the LFS file
 * handle doesn't leak into the LFS_SLM_MAX_FILES = 4 pool). */
static void xput_close_fd(struct shell_xput_session *xput)
{
    if (xput && xput->mnt) {
        littlefs_file_close(xput->mnt, xput->fd);
    }
    if (xput) {
        xput->mnt = NULL;
        xput->fd = -1;
    }
}

static void xput_session_reset(void)
{
    struct shell_session *session = shell_session_current();
    xput_close_fd(&session->xput);
    memset(&session->xput, 0, sizeof(session->xput));
    session->xput.fd = -1;   /* explicit "no handle" sentinel */
}

void shell_xput_session_close_for(struct shell_session *s)
{
    if (!s) return;
    xput_close_fd(&s->xput);
    s->xput.active = false;
}

static struct shell_xput_session *xput_session_current(void)
{
    return &shell_session_current()->xput;
}

/*
 * Build file content from argv[2..argc-1], joining with spaces and translating
 * C-style escapes (\n, \t, \r, \\, \", \0, \xNN). Returns bytes written into
 * `out` (excluding the terminating NUL), or -1 if `out_size` is exhausted.
 *
 * Used by `write` and `append` so a Lua/script source line can carry newlines
 * and tabs through the shell's whitespace-tokenized argv.
 */
static int shell_build_content(int argc, char *argv[], int first_arg,
                               char *out, int out_size)
{
    int pos = 0;
    for (int i = first_arg; i < argc; i++) {
        if (i > first_arg) {
            if (pos >= out_size - 1) return -1;
            out[pos++] = ' ';
        }
        const char *p = argv[i];
        while (*p) {
            if (pos >= out_size - 1) return -1;
            if (*p != '\\' || p[1] == '\0') {
                out[pos++] = *p++;
                continue;
            }
            /* Escape sequence */
            char esc = p[1];
            switch (esc) {
                case 'n':  out[pos++] = '\n'; p += 2; break;
                case 't':  out[pos++] = '\t'; p += 2; break;
                case 'r':  out[pos++] = '\r'; p += 2; break;
                case '\\': out[pos++] = '\\'; p += 2; break;
                case '"':  out[pos++] = '"';  p += 2; break;
                case '\'': out[pos++] = '\''; p += 2; break;
                case '0':  out[pos++] = '\0'; p += 2; break;
                case 'x': {
                    /* \xNN — two hex digits */
                    int hi = -1, lo = -1;
                    char c1 = p[2], c2 = c1 ? p[3] : 0;
                    if (c1 >= '0' && c1 <= '9') {
                        hi = c1 - '0';
                    } else if (c1 >= 'a' && c1 <= 'f') {
                        hi = 10 + (c1 - 'a');
                    } else if (c1 >= 'A' && c1 <= 'F') {
                        hi = 10 + (c1 - 'A');
                    }
                    if (c2 >= '0' && c2 <= '9') {
                        lo = c2 - '0';
                    } else if (c2 >= 'a' && c2 <= 'f') {
                        lo = 10 + (c2 - 'a');
                    } else if (c2 >= 'A' && c2 <= 'F') {
                        lo = 10 + (c2 - 'A');
                    }
                    if (hi < 0 || lo < 0) {
                        /* Malformed — emit literally so the user sees it */
                        out[pos++] = '\\';
                        p++;
                    } else {
                        out[pos++] = (char)((hi << 4) | lo);
                        p += 4;
                    }
                    break;
                }
                default:
                    /* Unknown escape — keep the backslash literal */
                    out[pos++] = '\\';
                    p++;
                    break;
            }
        }
    }
    out[pos] = '\0';
    return pos;
}

static int shell_hex_nibble(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return 10 + (c - 'a');
    if (c >= 'A' && c <= 'F') return 10 + (c - 'A');
    return -1;
}

/*
 * Build binary content from argv[first_arg..argc-1], concatenating each
 * argument and decoding it as hex. Returns bytes written, or:
 *   -1 if `out_size` is exhausted
 *   -2 for an odd number of hex digits
 *   -3 for a non-hex character
 */
static int shell_build_hex(int argc, char *argv[], int first_arg,
                           uint8_t *out, int out_size)
{
    int pos = 0;
    int hi = -1;

    for (int i = first_arg; i < argc; i++) {
        const char *p = argv[i];
        while (*p) {
            int nibble = shell_hex_nibble(*p++);
            if (nibble < 0) {
                return -3;
            }
            if (hi < 0) {
                hi = nibble;
                continue;
            }
            if (pos >= out_size) {
                return -1;
            }
            out[pos++] = (uint8_t)((hi << 4) | nibble);
            hi = -1;
        }
    }

    if (hi >= 0) {
        return -2;
    }
    return pos;
}

/* ============================================================================
 * VFS Commands (pwd, cd, ls, cat)
 * ============================================================================ */

/*
 * pwd - Print working directory
 */
int cmd_pwd(int argc, char *argv[])
{
    (void)argc;
    (void)argv;
    struct shell_session *sess = shell_session_current();
    const char *cwd = (sess && sess->cwd[0]) ? sess->cwd : "/";
    shell_printf("%s\r\n", cwd);
    return 0;
}

/*
 * cd <path> - Change directory
 */
int cmd_cd(int argc, char *argv[])
{
    char resolved[VFS_MAX_PATH];
    const char *path = "/";  /* Default to root */

    if (argc >= 2) {
        path = argv[1];
    }

    /* Resolve the path */
    if (shell_resolve_path(path, resolved, sizeof(resolved)) < 0) {
        shell_puts("cd: path too long\r\n");
        return -1;
    }

    /* Check if path exists and is a directory */
    const char *subpath = NULL;
    struct vfs_node *node = vfs_lookup_mount(resolved, &subpath);
    if (!node) {
        shell_printf("cd: %s: No such file or directory\r\n", resolved);
        return -1;
    }

    /* Check if it's a directory or mount point */
    if (node->type != VFS_NODE_DIR && node->type != VFS_NODE_MOUNT) {
        shell_printf("cd: %s: Not a directory\r\n", resolved);
        return -1;
    }

    /* For mount points, we also need to check if subpath is a directory */
    if (node->type == VFS_NODE_MOUNT && subpath && subpath[0] != '\0' &&
        !(subpath[0] == '/' && subpath[1] == '\0')) {
        /* There's a subpath within the mount - verify it's a directory */
        struct vfs_entry_info info;
        if (vfs_stat_path(resolved, &info) < 0) {
            shell_printf("cd: %s: No such file or directory\r\n", resolved);
            return -1;
        }
        if (info.type != 1) {  /* 1 = directory */
            shell_printf("cd: %s: Not a directory\r\n", resolved);
            return -1;
        }
    }

    /* Update cwd on the current session. Each session has its own
     * cwd — changing it here affects only this session. */
    struct shell_session *sess = shell_session_current();
    if (sess) {
        strncpy(sess->cwd, resolved, sizeof(sess->cwd) - 1);
        sess->cwd[sizeof(sess->cwd) - 1] = '\0';
    }
    return 0;
}

/*
 * Callback for listing directory entries (VFS nodes).
 */
static void ls_print_entry(struct vfs_node *node, void *ctx)
{
    (void)ctx;
    if (node->type == VFS_NODE_DIR || node->type == VFS_NODE_MOUNT) {
        shell_printf("  %s/\r\n", node->name);
    } else {
        shell_printf("  %s\r\n", node->name);
    }
}

/*
 * Callback for listing directory entries (mount point entries).
 */
static void ls_print_mount_entry(const struct vfs_entry_info *info, void *ctx)
{
    (void)ctx;
    if (info->type == 1) {  /* Directory */
        shell_printf("  %s/\r\n", info->name);
    } else {
        shell_printf("  %s  (%lu bytes)\r\n", info->name, (unsigned long)info->size);
    }
}

/*
 * ls [path] - List directory contents
 * Defaults to current working directory if no path given.
 */
int cmd_ls(int argc, char *argv[])
{
    char resolved[VFS_MAX_PATH];
    const char *input_path = ".";  /* Default to cwd */

    if (argc >= 2) {
        input_path = argv[1];
    }

    /* Resolve path (handles relative paths) */
    if (shell_resolve_path(input_path, resolved, sizeof(resolved)) < 0) {
        shell_puts("ls: path too long\r\n");
        return -1;
    }

    /* Try the path-based lookup which handles mount points */
    const char *subpath = NULL;
    struct vfs_node *node = vfs_lookup_mount(resolved, &subpath);
    if (!node) {
        shell_printf("ls: %s: No such file or directory\r\n", resolved);
        return -1;
    }

    /* If it's a mount point, use the path-based listing */
    if (node->type == VFS_NODE_MOUNT) {
        shell_printf("%s:\r\n", resolved);
        int err = vfs_list_path(resolved, ls_print_mount_entry, NULL);
        if (err < 0) {
            shell_printf("ls: %s: Failed to read directory\r\n", resolved);
            return -1;
        }
        return 0;
    }

    if (node->type == VFS_NODE_FILE) {
        /* It's a file, just show its name */
        shell_printf("%s\r\n", node->name);
        return 0;
    }

    /* Regular directory */
    shell_printf("%s:\r\n", resolved);
    vfs_list(node, ls_print_entry, NULL);

    return 0;
}

/*
 * cat <path> [offset] [length] - Show file contents
 *
 * With offset and length, reads a portion of the file (useful for large files).
 * Supports relative paths.
 */
int cmd_cat(int argc, char *argv[])
{
    if (argc < 2) {
        shell_puts("Usage: cat <path> [offset] [length]\r\n");
        shell_puts("  Show contents of a file (virtual or from mount).\r\n");
        shell_puts("  Optional offset and length for large files.\r\n");
        shell_puts("  Example: cat /sys/memory\r\n");
        shell_puts("  Example: cat hello.txt  (relative to cwd)\r\n");
        shell_puts("  Example: cat /mnt/files/large.bin 0 1024\r\n");
        return -1;
    }

    char resolved[VFS_MAX_PATH];
    if (shell_resolve_path(argv[1], resolved, sizeof(resolved)) < 0) {
        shell_puts("cat: path too long\r\n");
        return -1;
    }

    size_t offset = 0;
    size_t max_len = 1024;  /* Default max read */

    /* Parse optional offset */
    if (argc >= 3) {
        uint32_t off_val;
        if (shell_parse_uint(argv[2], &off_val) == 0) {
            offset = off_val;
        }
    }

    /* Parse optional length */
    if (argc >= 4) {
        uint32_t len_val;
        if (shell_parse_uint(argv[3], &len_val) == 0) {
            max_len = len_val;
            if (max_len > 4096) max_len = 4096;  /* Cap at 4KB for safety */
        }
    }

    /* Try the path-based lookup which handles mount points */
    const char *subpath = NULL;
    struct vfs_node *node = vfs_lookup_mount(resolved, &subpath);
    if (!node) {
        shell_printf("cat: %s: No such file or directory\r\n", resolved);
        return -1;
    }

    /* If it's a mount point with subpath, use the path-based read */
    if (node->type == VFS_NODE_MOUNT) {
        /* Allocate buffer based on requested length (up to 4KB) */
        char buf[4096];
        size_t read_size = max_len < sizeof(buf) - 1 ? max_len : sizeof(buf) - 1;

        int len = vfs_read_path(resolved, buf, read_size, offset);
        if (len < 0) {
            shell_printf("cat: %s: Read error or is a directory\r\n", resolved);
            return -1;
        }

        buf[len] = '\0';

        /* Show offset info if using streaming */
        if (offset > 0 || argc >= 4) {
            shell_printf("[offset=%lu, read=%d bytes]\r\n",
                        (unsigned long)offset, len);
        }

        /* Print contents, converting \n to \r\n */
        for (int i = 0; i < len; i++) {
            if (buf[i] == '\n') {
                shell_putc('\r');
            }
            shell_putc(buf[i]);
        }

        /* Ensure newline at end */
        if (len > 0 && buf[len - 1] != '\n') {
            shell_puts("\r\n");
        }

        return 0;
    }

    if (node->type == VFS_NODE_DIR) {
        shell_printf("cat: %s: Is a directory\r\n", resolved);
        return -1;
    }

    /* Read virtual file contents */
    char buf[1024];
    int len = vfs_read(node, buf, sizeof(buf) - 1);
    if (len < 0) {
        shell_printf("cat: %s: Read error\r\n", resolved);
        return -1;
    }

    buf[len] = '\0';

    /* Print contents, converting \n to \r\n */
    for (int i = 0; i < len; i++) {
        if (buf[i] == '\n') {
            shell_putc('\r');
        }
        shell_putc(buf[i]);
    }

    /* Ensure newline at end */
    if (len > 0 && buf[len - 1] != '\n') {
        shell_puts("\r\n");
    }

    return 0;
}

/* ============================================================================
 * Filesystem Write Commands (write, mkdir, rm, mv, df)
 * ============================================================================ */

/*
 * write <path> <content> - Write content to a file
 *
 * Creates or overwrites a file in a mounted filesystem.
 * Supports relative paths.
 */
int cmd_write(int argc, char *argv[])
{
    if (argc < 3) {
        shell_puts("Usage: write <path> <content>\r\n");
        shell_puts("  Write content to a file (creates or overwrites).\r\n");
        shell_puts("  Path must be in a mounted filesystem.\r\n");
        shell_puts("  Escapes: \\n \\t \\r \\\\ \\\" \\' \\0 \\xNN\r\n");
        shell_puts("  Example: write test.txt Hello  (relative to cwd)\r\n");
        shell_puts("  Example: write demo.lua \"P('hi')\\nP('bye')\\n\"\r\n");
        return -1;
    }

    char resolved[VFS_MAX_PATH];
    if (shell_resolve_path(argv[1], resolved, sizeof(resolved)) < 0) {
        shell_puts("write: path too long\r\n");
        return -1;
    }

    /* Get the mount context and subpath */
    const char *subpath = NULL;
    struct lfs_mount *mnt = vfs_get_mount_ctx(resolved, &subpath);
    if (!mnt) {
        shell_printf("write: %s: Not a mounted filesystem\r\n", resolved);
        shell_puts("  (Only mounted filesystems support writing)\r\n");
        return -1;
    }

    /* Build content from remaining arguments (with escape translation) */
    static char content[4096];
    int pos = shell_build_content(argc, argv, 2, content, (int)sizeof(content));
    if (pos < 0) {
        shell_printf("write: content too long (max %d bytes)\r\n",
                    (int)sizeof(content) - 1);
        return -1;
    }

    /* Open file for writing (create + truncate) */
    int fd = littlefs_file_open(mnt, subpath, LFS_O_WRONLY | LFS_O_CREAT | LFS_O_TRUNC);
    if (fd < 0) {
        shell_printf("write: %s: Failed to open file\r\n", resolved);
        return -1;
    }

    /* Write content */
    int written = littlefs_file_write(mnt, fd, content, pos);
    littlefs_file_close(mnt, fd);

    if (written < 0) {
        shell_printf("write: %s: Write failed\r\n", resolved);
        return -1;
    }

    shell_printf("Wrote %d bytes to %s\r\n", written, resolved);
    return 0;
}

/*
 * put [-a] <path> <hex...> - Write binary hex to a file
 *
 * Decodes raw hex bytes over the existing shell transport. This is a
 * transport primitive for binary-safe file ingress over telnet or UART.
 * Use `put` to overwrite and `put -a` to append another chunk.
 */
int cmd_put(int argc, char *argv[])
{
    bool append_mode = false;
    int path_arg = 1;
    int data_arg = 2;

    if (argc >= 2 && strcmp(argv[1], "-a") == 0) {
        append_mode = true;
        path_arg = 2;
        data_arg = 3;
    }

    if (argc <= data_arg) {
        shell_puts("Usage: put <path> <hex...>\r\n");
        shell_puts("       put -a <path> <hex...>\r\n");
        shell_puts("  Decode hexadecimal bytes into a file.\r\n");
        shell_puts("  Default mode overwrites; -a appends another chunk.\r\n");
        shell_puts("  Example: put /mnt/files/blob.bin 000102ff\r\n");
        shell_puts("  Example: put -a /mnt/files/blob.bin aabbccdd\r\n");
        return -1;
    }

    char resolved[VFS_MAX_PATH];
    if (shell_resolve_path(argv[path_arg], resolved, sizeof(resolved)) < 0) {
        shell_puts("put: path too long\r\n");
        return -1;
    }

    const char *subpath = NULL;
    struct lfs_mount *mnt = vfs_get_mount_ctx(resolved, &subpath);
    if (!mnt) {
        shell_printf("put: %s: Not a mounted filesystem\r\n", resolved);
        return -1;
    }

    /* Decode buffer for hex-encoded chunk payload. Sized at 16 KB
     * to match the post-#581 SHELL_MAX_LINE = 32768 ceiling: the
     * `xput chunk OFFSET HEXDATA\n` line carries up to ~32 KB of
     * hex (= 16 KB binary) after subtracting the prefix and slm-put.py's
     * 16-char headroom. Static so it stays out of the 64 KB task
     * stack; the same line is parsed into argv anyway, so this
     * buffer's lifetime is bounded by the single shell command. */
    static uint8_t data[16384];
    int bytes = shell_build_hex(argc, argv, data_arg, data, (int)sizeof(data));
    if (bytes == -1) {
        shell_printf("put: hex payload too large (max %d bytes per command)\r\n",
                    (int)sizeof(data));
        return -1;
    }
    if (bytes == -2) {
        shell_puts("put: odd number of hex digits\r\n");
        return -1;
    }
    if (bytes == -3) {
        shell_puts("put: invalid hex digit\r\n");
        return -1;
    }

    int flags = LFS_O_WRONLY | LFS_O_CREAT;
    flags |= append_mode ? LFS_O_APPEND : LFS_O_TRUNC;

    int fd = littlefs_file_open(mnt, subpath, flags);
    if (fd < 0) {
        shell_printf("put: %s: Failed to open file\r\n", resolved);
        return -1;
    }

    int written = littlefs_file_write(mnt, fd, data, bytes);
    littlefs_file_close(mnt, fd);
    if (written != bytes) {
        shell_printf("put: %s: Write failed\r\n", resolved);
        return -1;
    }

    shell_printf("%s %d bytes to %s\r\n",
                append_mode ? "Appended" : "Wrote", written, resolved);
    return 0;
}

/*
 * xput begin|resume|chunk|status|finish|abort - Framed upload session
 *
 * Session-oriented upload surface for host tools. begin declares the
 * final size and truncates the target; resume picks up an existing
 * partial file without truncating (used by clients reconnecting after
 * a TCP-level disconnect, which loses the in-memory xput session).
 * chunk enforces an exact offset, finish validates the declared size,
 * and status exposes the current remote offset for resume/recovery.
 */
int cmd_xput(int argc, char *argv[])
{
    struct shell_xput_session *xput = xput_session_current();

    if (argc < 2) {
        shell_puts("Usage: xput begin <path> <size>\r\n");
        shell_puts("       xput resume <path> <size>\r\n");
        shell_puts("       xput chunk <offset> <hex...>\r\n");
        shell_puts("       xput status\r\n");
        shell_puts("       xput finish\r\n");
        shell_puts("       xput abort\r\n");
        return -1;
    }

    if (strcmp(argv[1], "status") == 0) {
        if (!xput->active) {
            shell_puts("XPUT inactive\r\n");
            return 0;
        }
        shell_printf("XPUT active path=%s size=%lu received=%lu checksum=%lu\r\n",
                     xput->path,
                     (unsigned long)xput->expected_size,
                     (unsigned long)xput->received_size,
                     (unsigned long)xput->checksum);
        return 0;
    }

    if (strcmp(argv[1], "abort") == 0) {
        xput_session_reset();
        shell_puts("XPUT aborted\r\n");
        return 0;
    }

    if (strcmp(argv[1], "begin") == 0) {
        char resolved[VFS_MAX_PATH];
        const char *subpath = NULL;
        struct lfs_mount *mnt;
        uint32_t size;
        int fd;

        if (argc < 4) {
            shell_puts("Usage: xput begin <path> <size>\r\n");
            return -1;
        }
        if (shell_resolve_path(argv[2], resolved, sizeof(resolved)) < 0) {
            shell_puts("xput begin: path too long\r\n");
            return -1;
        }
        if (shell_parse_uint(argv[3], &size) != 0) {
            shell_printf("xput begin: invalid size: %s\r\n", argv[3]);
            return -1;
        }
        mnt = vfs_get_mount_ctx(resolved, &subpath);
        if (!mnt) {
            shell_printf("xput begin: %s: Not a mounted filesystem\r\n", resolved);
            return -1;
        }
        /* Tear down any prior session state (closing a held fd if
         * one leaked through), THEN open the new file and hold the
         * fd open across all subsequent chunks. The pre-refactor
         * code opened-then-closed here and re-opened per chunk;
         * that pattern caused two failure modes on the 1 GB GGUF
         * upload: (1) ~32K open/close cycles per GB churned LFS
         * COW metadata to the point opens started failing, and
         * (2) any session task killed mid-chunk leaked a handle in
         * the LFS_SLM_MAX_FILES=4 pool, wedging the mount after
         * 4 such teardowns. Holding a single fd for the upload's
         * lifetime collapses both windows. */
        xput_session_reset();
        fd = littlefs_file_open(mnt, subpath, LFS_O_WRONLY | LFS_O_CREAT | LFS_O_TRUNC);
        if (fd < 0) {
            shell_printf("xput begin: %s: Failed to open file\r\n", resolved);
            return -1;
        }

        xput = xput_session_current();
        xput->active = true;
        strncpy(xput->path, resolved, sizeof(xput->path) - 1);
        xput->path[sizeof(xput->path) - 1] = '\0';
        xput->expected_size = size;
        xput->received_size = 0;
        xput->checksum = 0x811C9DC5u;
        xput->fd = fd;
        xput->mnt = mnt;
        shell_printf("XPUT ok begin path=%s size=%lu\r\n",
                     xput->path,
                     (unsigned long)xput->expected_size);
        return 0;
    }

    if (strcmp(argv[1], "resume") == 0) {
        /* Resume an existing partial upload without truncating. The
         * client follows up with `xput chunk` writes starting at the
         * `received` offset reported here. The xput session lives on
         * a per-shell-session basis (see shell_xput_session_close_for
         * + shell_session_free), so any TCP disconnect leaves the
         * file on disk but the in-memory session gone — `xput status`
         * will report inactive. `xput resume` reconstructs the
         * session from the on-disk state.
         *
         * Reading the existing bytes to compute the FNV-1a checksum
         * is a one-time cost on resume (bounded by LFS read
         * bandwidth — a few seconds for ~500 MB on RAM-backed disks).
         * The alternative — persisting the checksum on disk — is
         * more state to maintain across crashes / mounts and would
         * have to survive the same teardown that loses the in-memory
         * session, defeating the point. The protocol stays
         * stateless on disk by recomputing here.
         *
         * Caller-side responsibility: after `xput resume` succeeds,
         * the client MUST verify the reported `checksum` matches
         * its source data's FNV-1a over the first `received` bytes.
         * If it doesn't (file on disk is from a prior, different
         * upload), the client should fall back to `xput begin` to
         * truncate and start over. */
        char resolved[VFS_MAX_PATH];
        const char *subpath = NULL;
        struct lfs_mount *mnt;
        uint32_t size;
        int fd;
        struct lfs_entry_info info;

        if (argc < 4) {
            shell_puts("Usage: xput resume <path> <size>\r\n");
            return -1;
        }
        if (shell_resolve_path(argv[2], resolved, sizeof(resolved)) < 0) {
            shell_puts("xput resume: path too long\r\n");
            return -1;
        }
        if (shell_parse_uint(argv[3], &size) != 0) {
            shell_printf("xput resume: invalid size: %s\r\n", argv[3]);
            return -1;
        }
        mnt = vfs_get_mount_ctx(resolved, &subpath);
        if (!mnt) {
            shell_printf("xput resume: %s: Not a mounted filesystem\r\n",
                         resolved);
            return -1;
        }
        if (littlefs_stat_path(mnt, subpath, &info) != LFS_ERR_OK) {
            shell_printf("xput resume: %s: file does not exist\r\n",
                         resolved);
            return -1;
        }
        if (info.size > size) {
            shell_printf("xput resume: %s: existing %lu > requested %lu\r\n",
                         resolved,
                         (unsigned long)info.size,
                         (unsigned long)size);
            return -1;
        }

        xput_session_reset();
        fd = littlefs_file_open(mnt, subpath, LFS_O_RDWR);
        if (fd < 0) {
            shell_printf("xput resume: %s: Failed to open file\r\n",
                         resolved);
            return -1;
        }

        uint32_t checksum = 0x811C9DC5u;
        {
            static uint8_t scratch[4096];
            uint32_t left = info.size;
            while (left > 0) {
                uint32_t want =
                    left > sizeof(scratch) ? sizeof(scratch) : left;
                int got = littlefs_file_read(mnt, fd, scratch,
                                             (size_t)want);
                if (got != (int)want) {
                    littlefs_file_close(mnt, fd);
                    shell_printf("xput resume: %s: read failed at %lu\r\n",
                                 resolved,
                                 (unsigned long)(info.size - left));
                    return -1;
                }
                checksum = shell_checksum32_update(checksum, scratch,
                                                   (size_t)want);
                left -= want;
            }
        }

        if (littlefs_file_seek(mnt, fd, 0, LFS_SEEK_END) < 0) {
            littlefs_file_close(mnt, fd);
            shell_printf("xput resume: %s: seek failed\r\n", resolved);
            return -1;
        }

        xput = xput_session_current();
        xput->active = true;
        strncpy(xput->path, resolved, sizeof(xput->path) - 1);
        xput->path[sizeof(xput->path) - 1] = '\0';
        xput->expected_size = size;
        xput->received_size = info.size;
        xput->checksum = checksum;
        xput->fd = fd;
        xput->mnt = mnt;
        shell_printf("XPUT ok resume path=%s size=%lu received=%lu "
                     "checksum=%lu\r\n",
                     xput->path,
                     (unsigned long)xput->expected_size,
                     (unsigned long)xput->received_size,
                     (unsigned long)xput->checksum);
        return 0;
    }

    if (strcmp(argv[1], "chunk") == 0) {
        uint32_t offset;
        /* Decode buffer for hex-encoded chunk payload. Sized at 16 KB
         * to match the post-#581 SHELL_MAX_LINE = 32768 ceiling: the
         * `xput chunk OFFSET HEXDATA\n` line carries up to ~32 KB of
         * hex (= 16 KB binary) after subtracting the prefix and slm-put.py's
         * 16-char headroom. Static so it stays out of the 64 KB task
         * stack; the same line is parsed into argv anyway, so this
         * buffer's lifetime is bounded by the single shell command. */
        static uint8_t data[16384];
        int bytes;
        int written;

        if (argc < 4) {
            shell_puts("Usage: xput chunk <offset> <hex...>\r\n");
            return -1;
        }
        if (!xput->active) {
            shell_puts("xput chunk: no active session\r\n");
            return -1;
        }
        if (shell_parse_uint(argv[2], &offset) != 0) {
            shell_printf("xput chunk: invalid offset: %s\r\n", argv[2]);
            return -1;
        }
        if (offset != xput->received_size) {
            shell_printf("xput chunk: offset mismatch expected=%lu got=%lu\r\n",
                         (unsigned long)xput->received_size,
                         (unsigned long)offset);
            return -1;
        }
        bytes = shell_build_hex(argc, argv, 3, data, (int)sizeof(data));
        if (bytes == -1) {
            shell_printf("xput chunk: payload too large (max %d bytes)\r\n",
                         (int)sizeof(data));
            return -1;
        }
        if (bytes == -2) {
            shell_puts("xput chunk: odd number of hex digits\r\n");
            return -1;
        }
        if (bytes == -3) {
            shell_puts("xput chunk: invalid hex digit\r\n");
            return -1;
        }
        if ((uint64_t)xput->received_size + (uint32_t)bytes >
            (uint64_t)xput->expected_size) {
            shell_printf("xput chunk: exceeds declared size %lu\r\n",
                         (unsigned long)xput->expected_size);
            return -1;
        }

        /* Use the persistent fd opened by `xput begin`. The previous
         * implementation re-opened/closed per chunk; see the comment
         * in the begin handler for why that was load-bearing on
         * uploads larger than a few MB. `mnt` is the authoritative
         * "fd is held" signal — do not test `fd < 0`, see the
         * xput_close_fd comment. */
        if (!xput->mnt) {
            shell_puts("xput chunk: session has no open fd "
                       "(did `xput begin` succeed?)\r\n");
            return -1;
        }

        /* Time the LFS write to surface the degradation #597 documents
         * (transfer rate falling from ~125 KB/s to ~64 KB/s as the
         * file grows, eventually exceeding the script's prompt-read
         * timeout). Confirms whether the cost is in lfs_file_write
         * vs. somewhere else in the chunk path. CNTPCT_EL0 / TSC
         * via the same accessor shell_io_tcp.c uses for its bench
         * paths. */
        const uint64_t freq = timer_get_frequency();
        const uint64_t t_start = timer_get_count();
        if (littlefs_file_write(xput->mnt, xput->fd, data,
                                (size_t)bytes) != bytes) {
            shell_printf("xput chunk: %s: Write failed\r\n", xput->path);
            return -1;
        }
        const uint64_t elapsed_ms =
            freq ? (((timer_get_count() - t_start) * 1000ULL) / freq) : 0;
        if (elapsed_ms >= XPUT_SLOW_WRITE_THRESHOLD_MS) {
            INFO("xput: slow LFS write offset=%lu bytes=%d "
                 "elapsed_ms=%lu file_size=%lu",
                 (unsigned long)offset,
                 bytes,
                 (unsigned long)elapsed_ms,
                 (unsigned long)(xput->received_size + (uint32_t)bytes));
        }
        written = bytes;

        xput->received_size += (uint32_t)bytes;
        xput->checksum =
            shell_checksum32_update(xput->checksum, data, (size_t)bytes);
        shell_printf("XPUT ok chunk offset=%lu next=%lu checksum=%lu\r\n",
                     (unsigned long)offset,
                     (unsigned long)xput->received_size,
                     (unsigned long)xput->checksum);
        (void)written;
        return 0;
    }

    if (strcmp(argv[1], "finish") == 0) {
        if (!xput->active) {
            shell_puts("xput finish: no active session\r\n");
            return -1;
        }
        if (xput->received_size != xput->expected_size) {
            shell_printf("xput finish: size mismatch expected=%lu received=%lu\r\n",
                         (unsigned long)xput->expected_size,
                         (unsigned long)xput->received_size);
            return -1;
        }
        /* Cache the formatted summary BEFORE the reset so the close
         * happens before we report success — partial writes that
         * fail at close (e.g. metadata flush errors on full
         * underlying media) would otherwise be silently swallowed
         * after the user's tool has already moved on. */
        shell_printf("XPUT ok finish path=%s size=%lu checksum=%lu\r\n",
                     xput->path,
                     (unsigned long)xput->received_size,
                     (unsigned long)xput->checksum);
        xput_session_reset();   /* closes the persistent fd */
        return 0;
    }

    shell_puts("Usage: xput begin <path> <size>\r\n");
    shell_puts("       xput resume <path> <size>\r\n");
    shell_puts("       xput chunk <offset> <hex...>\r\n");
    shell_puts("       xput status\r\n");
    shell_puts("       xput finish\r\n");
    shell_puts("       xput abort\r\n");
    return -1;
}

/*
 * xput-bin <path> <total> - Direct binary upload (#597 Option B).
 *
 * Bypasses both the hex 2× wire-overhead AND the line-edit per-byte
 * shell-parse cost that cap the framed `xput chunk` protocol's
 * throughput at ~125 KB/s on hardware. After the command line is
 * dispatched, this handler reads `total` raw bytes from the shell
 * session's input stream straight into a 128 KB staging buffer, then
 * commits them to LFS with one `littlefs_file_write` call per
 * 128 KB block (vs. one per 16 KB chunk in the framed path —
 * amortizes the COW metadata cost).
 *
 * Wire encoding: bytes flow over the existing telnet shell session
 * AS-IS, with the standard telnet IAC escape. Any 0xFF byte in the
 * payload MUST be doubled by the sender (0xFF 0xFF) to match RFC 854.
 * The kernel's existing telnet RX parser unstuffs this transparently
 * before bytes reach the shell ring, so cmd_xput_bin sees the
 * original payload.
 *
 * Resume: if the file already exists with size <= total, the new
 * data is appended starting at the existing-file offset. The kernel
 * reports `XPUT-BIN ready offset=N` and the client streams from N
 * onwards. On size > total, the command rejects with an error so
 * the client can `xput begin` (truncate) explicitly.
 *
 * Protocol:
 *   client:  xput-bin <path> <total>\n
 *   kernel:  XPUT-BIN ready offset=<resume_offset>\r\n
 *   client:  <total - resume_offset> raw bytes (with 0xFF doubled)
 *   kernel:  XPUT-BIN done size=<total>\r\n
 *   kernel:  slmos>     (normal prompt resumes)
 */
int cmd_xput_bin(int argc, char *argv[])
{
    if (argc < 3) {
        shell_puts("Usage: xput-bin <path> <total>\r\n");
        return -1;
    }
    char resolved[VFS_MAX_PATH];
    if (shell_resolve_path(argv[1], resolved, sizeof(resolved)) < 0) {
        shell_puts("xput-bin: path too long\r\n");
        return -1;
    }
    uint32_t total;
    if (shell_parse_uint(argv[2], &total) != 0) {
        shell_printf("xput-bin: invalid total: %s\r\n", argv[2]);
        return -1;
    }

    /* 128 KB staging buffer in BSS, half the 256 KB shell rx ring so
     * each LFS write fits comfortably without contending against
     * still-arriving bytes. Reduces the total number of LFS
     * write+metadata-commit cycles per upload — the dominant cost
     * on 5+ MB transfers (#597).
     *
     * Single-flight guard: the buffer is shared across all shell
     * sessions. Two concurrent xput-bin callers would interleave
     * writes into bin_buf and corrupt both files silently. The
     * `xput_bin_active` flag makes a second concurrent caller fail
     * fast with `XPUT-BIN err reason=busy` rather than racing.
     * Atomic test-and-set so the guard holds even if shell tasks
     * ever migrate off CPU 0 or move to a preemptive policy.
     * Checked before any LFS work so a busy reject is cheap and
     * doesn't disturb the in-flight uploader's state. Cleared on
     * every return path below. */
    static uint8_t bin_buf[131072];
    static bool xput_bin_active = false;

    if (__atomic_exchange_n(&xput_bin_active, true, __ATOMIC_ACQ_REL)) {
        shell_puts("XPUT-BIN err received=0 reason=busy\r\n");
        return -1;
    }

    const char *subpath = NULL;
    struct lfs_mount *mnt = vfs_get_mount_ctx(resolved, &subpath);
    if (!mnt) {
        __atomic_store_n(&xput_bin_active, false, __ATOMIC_RELEASE);
        shell_printf("xput-bin: %s: Not a mounted filesystem\r\n", resolved);
        return -1;
    }

    /* Resume detection: stat the path. If it exists with size <=
     * total, the client's bytes get appended past `existing_size`.
     * size > total means a stale/wrong file is in the way; the client
     * must `rm` or `xput begin` to clear it explicitly. */
    struct lfs_entry_info info;
    uint32_t resume_offset = 0;
    bool resume = false;
    if (littlefs_stat_path(mnt, subpath, &info) == LFS_ERR_OK) {
        if (info.size > total) {
            __atomic_store_n(&xput_bin_active, false, __ATOMIC_RELEASE);
            shell_printf("xput-bin: %s: existing %lu > requested %lu\r\n",
                         resolved,
                         (unsigned long)info.size,
                         (unsigned long)total);
            return -1;
        }
        resume_offset = info.size;
        resume = (info.size > 0);
    }

    int fd = littlefs_file_open(mnt, subpath,
                                resume
                                  ? (LFS_O_RDWR)
                                  : (LFS_O_WRONLY | LFS_O_CREAT | LFS_O_TRUNC));
    if (fd < 0) {
        __atomic_store_n(&xput_bin_active, false, __ATOMIC_RELEASE);
        shell_printf("xput-bin: %s: failed to open\r\n", resolved);
        return -1;
    }
    if (resume) {
        if (littlefs_file_seek(mnt, fd, 0, LFS_SEEK_END) < 0) {
            littlefs_file_close(mnt, fd);
            __atomic_store_n(&xput_bin_active, false, __ATOMIC_RELEASE);
            shell_printf("xput-bin: %s: seek failed\r\n", resolved);
            return -1;
        }
    }

    /* Switch the session's telnet decoder to binary pass-through
     * BEFORE we tell the script we're ready. Without this, any
     * 0x0D 0x0A or 0x0D 0x00 pair in the binary payload would have
     * its second byte swallowed by the T_CR LF/NUL-eating state,
     * silently shifting every byte after it by one and corrupting
     * the file (~14 KB of drops on a typical 100 MB GGUF). The
     * script doesn't send binary data until it sees "ready", so
     * setting the flag here covers every byte of the upload.
     * Cleared on every return path below alongside xput_bin_active. */
    shell_session_set_binary_mode(true);

    /* Tell the client we're ready and what offset to start streaming
     * from. The client uses this for resume — sends only
     * total - resume_offset bytes. */
    shell_printf("XPUT-BIN ready offset=%lu\r\n",
                 (unsigned long)resume_offset);

    /* Cap the wait between bytes inside a block at this many ms.
     * Without it, an upload that loses its tail (peer's TCP send
     * buffer never drains, or stuffing miscount) hangs cmd_xput_bin
     * indefinitely waiting for the partial block to complete.
     * 5 seconds is comfortably above any reasonable network blip
     * and gives Linux's TCP retransmit time to get unstuck.
     *
     * On timeout we commit whatever partial bytes we have to LFS,
     * so the file ends with the last successfully-received bytes
     * intact. The script can then `xput-bin` again to resume from
     * the new offset. */
    const uint64_t freq_for_read_to = timer_get_frequency();
    const uint64_t read_to_ticks = freq_for_read_to
        ? (freq_for_read_to * 5ULL)        /* 5 seconds */
        : 0;

    uint32_t received = resume_offset;
    while (received < total) {
        uint32_t want = total - received;
        if (want > sizeof(bin_buf)) {
            want = sizeof(bin_buf);
        }

        /* Drain `want` bytes into bin_buf. shell_session_read_raw
         * blocks waiting for the first byte and returns whatever is
         * available; loop until we've assembled the full block, or
         * we've been waiting too long without any new bytes (last-
         * block stall protection). */
        uint32_t got = 0;
        uint64_t last_progress_ticks = timer_get_count();
        while (got < want) {
            int n = shell_session_read_raw((char *)(bin_buf + got),
                                           (int)(want - got));
            if (n < 0) {
                /* Connection closed or backend error mid-stream.
                 * Commit whatever partial bytes we got before
                 * closing — otherwise a peer drop near the end of
                 * the upload silently rolls back the in-flight
                 * block. */
                if (got > 0) {
                    int wp = littlefs_file_write(mnt, fd, bin_buf,
                                                 (size_t)got);
                    if (wp == (int)got) {
                        received += got;
                        INFO("xput-bin: partial commit on close: "
                             "+%u bytes => %lu",
                             got, (unsigned long)received);
                    }
                }
                littlefs_file_close(mnt, fd);
                shell_session_set_binary_mode(false);
                __atomic_store_n(&xput_bin_active, false, __ATOMIC_RELEASE);
                shell_printf("XPUT-BIN err received=%lu reason=closed\r\n",
                             (unsigned long)received);
                return -1;
            }
            if (n > 0) {
                got += (uint32_t)n;
                last_progress_ticks = timer_get_count();
                continue;
            }
            /* n == 0: shell_session_read_raw always blocks for at
             * least one byte or returns -1, so this is the stall
             * path — peer stopped sending halfway through a block
             * (TCP send buffer drained, Nagle/end-of-stream
             * interaction). Commit what we have rather than hang
             * forever. */
            if (read_to_ticks &&
                (timer_get_count() - last_progress_ticks)
                > read_to_ticks) {
                INFO("xput-bin: read stall after %u/%u (got %u of "
                     "block) — committing partial",
                     (unsigned)received, (unsigned)total,
                     (unsigned)got);
                if (got > 0) {
                    int wp = littlefs_file_write(mnt, fd, bin_buf,
                                                 (size_t)got);
                    if (wp == (int)got) received += got;
                }
                littlefs_file_close(mnt, fd);
                shell_session_set_binary_mode(false);
                __atomic_store_n(&xput_bin_active, false, __ATOMIC_RELEASE);
                shell_printf("XPUT-BIN err received=%lu reason=stall\r\n",
                             (unsigned long)received);
                return -1;
            }
        }

        int written = littlefs_file_write(mnt, fd, bin_buf, (size_t)got);
        if (written != (int)got) {
            littlefs_file_close(mnt, fd);
            shell_session_set_binary_mode(false);
            __atomic_store_n(&xput_bin_active, false, __ATOMIC_RELEASE);
            shell_printf("XPUT-BIN err received=%lu reason=write\r\n",
                         (unsigned long)received);
            return -1;
        }
        received += got;
    }

    littlefs_file_close(mnt, fd);
    shell_session_set_binary_mode(false);
    __atomic_store_n(&xput_bin_active, false, __ATOMIC_RELEASE);
    shell_printf("XPUT-BIN done size=%lu\r\n", (unsigned long)received);
    return 0;
}

/*
 * xget-bin <path> [skip] - Direct binary download (mirror of xput-bin)
 *
 * Streams the file at `path` raw over the current shell session's
 * TCP transport. No hex encoding, no per-line shell parse —
 * symmetric counterpart to xput-bin for the device→host direction.
 * `skip` is an optional resume offset; bytes at file positions
 * [skip, total) are sent. Default skip=0.
 *
 * Wire format: telnet IAC byte-stuffed (0xFF in payload doubles to
 * 0xFF 0xFF on the wire per RFC 854). The host's TelnetShell
 * un-stuffs it back to the original bytes.
 *
 * Protocol:
 *   client:  xget-bin <path> [skip]\n
 *   kernel:  XGET-BIN ready size=<total>\r\n
 *   kernel:  <total - skip> raw bytes (with 0xFF doubled)
 *   kernel:  XGET-BIN done size=<total>\r\n
 *   kernel:  slmos>     (normal prompt resumes)
 *
 * On error before the data stream begins, prints an `xget-bin: ...`
 * message and returns -1 — no `XGET-BIN ready` was sent so the
 * client knows nothing came. On error mid-stream (partial write to
 * peer, file shrank under us), prints
 * `XGET-BIN err sent=N reason=...\r\n` and returns -1.
 */
/* 64 KB chunk for the LFS→TCP streaming loop. Picked to match a
 * typical lwIP send-buffer high-water mark so the read+stuff+enqueue
 * pipeline keeps the TX ring full without one read dominating the
 * loop iteration time. Single-flight guard below means only one
 * cmd_xget_bin owns this buffer at a time. */
#define XGET_BIN_BUF_BYTES 65536u

/* Compile-time guard against a future LittleFS that widens lfs_size_t
 * past 32 bits — would silently truncate `total` in the header. */
_Static_assert(sizeof(((struct lfs_entry_info *)0)->size) == sizeof(uint32_t),
    "lfs_entry_info.size must be uint32_t — cmd_xget_bin truncates header otherwise");

int cmd_xget_bin(int argc, char *argv[])
{
    if (argc < 2) {
        shell_puts("Usage: xget-bin <path> [skip]\r\n");
        return -1;
    }

    /* Transport gate — UART/serial backends can't deliver bit-exact
     * binary (the cooked write path's CR-LF expansion would corrupt
     * the stream). Refuse upfront, before any framing header lands
     * on the wire, so the operator gets a clear error. */
    if (!shell_session_supports_write_raw()) {
        shell_puts("xget-bin: not supported on this transport "
                   "(requires TCP/telnet shell)\r\n");
        return -1;
    }

    char resolved[VFS_MAX_PATH];
    if (shell_resolve_path(argv[1], resolved, sizeof(resolved)) < 0) {
        shell_puts("xget-bin: path too long\r\n");
        return -1;
    }
    uint32_t skip = 0;
    if (argc >= 3 && shell_parse_uint(argv[2], &skip) != 0) {
        shell_printf("xget-bin: invalid skip: %s\r\n", argv[2]);
        return -1;
    }

    /* Single-flight guard. Mirrors xput-bin's pattern; serializes
     * the shared static bin_buf below. */
    static bool xget_bin_active = false;
    if (__atomic_exchange_n(&xget_bin_active, true, __ATOMIC_ACQ_REL)) {
        shell_puts("XGET-BIN err sent=0 reason=busy\r\n");
        return -1;
    }

    int rc = -1;
    int fd = -1;
    struct lfs_mount *mnt = NULL;
    uint32_t total = 0;
    uint32_t sent = 0;

    const char *subpath = NULL;
    mnt = vfs_get_mount_ctx(resolved, &subpath);
    if (!mnt) {
        shell_printf("xget-bin: %s: Not a mounted filesystem\r\n", resolved);
        goto fail;
    }

    struct lfs_entry_info info;
    if (littlefs_stat_path(mnt, subpath, &info) != LFS_ERR_OK) {
        shell_printf("xget-bin: %s: not found\r\n", resolved);
        goto fail;
    }
    total = info.size;
    if (skip > total) {
        shell_printf("xget-bin: %s: skip %lu > size %lu\r\n",
                     resolved, (unsigned long)skip, (unsigned long)total);
        goto fail;
    }

    fd = littlefs_file_open(mnt, subpath, LFS_O_RDONLY);
    if (fd < 0) {
        shell_printf("xget-bin: %s: failed to open\r\n", resolved);
        goto fail;
    }
    if (skip > 0 &&
        littlefs_file_seek(mnt, fd, (lfs_soff_t)skip, LFS_SEEK_SET) < 0) {
        shell_printf("xget-bin: %s: seek failed\r\n", resolved);
        goto fail;
    }

    /* Header: tell the client how many bytes to expect. */
    shell_printf("XGET-BIN ready size=%lu\r\n", (unsigned long)total);

    static uint8_t bin_buf[XGET_BIN_BUF_BYTES];
    sent = skip;
    while (sent < total) {
        uint32_t want = total - sent;
        if (want > sizeof(bin_buf)) want = sizeof(bin_buf);
        int got = littlefs_file_read(mnt, fd, bin_buf, (size_t)want);
        if (got <= 0) {
            shell_printf("XGET-BIN err sent=%lu reason=read\r\n",
                         (unsigned long)(sent - skip));
            goto fail;
        }
        shell_session_write_raw(bin_buf, (size_t)got);
        sent += (uint32_t)got;
    }

    shell_printf("XGET-BIN done size=%lu\r\n", (unsigned long)total);
    rc = 0;

fail:
    if (fd >= 0 && mnt) {
        littlefs_file_close(mnt, fd);
    }
    __atomic_store_n(&xget_bin_active, false, __ATOMIC_RELEASE);
    return rc;
}

/*
 * mkdir <path> - Create a directory
 * Supports relative paths.
 */
int cmd_mkdir(int argc, char *argv[])
{
    if (argc < 2) {
        shell_puts("Usage: mkdir <path>\r\n");
        shell_puts("  Create a directory in a mounted filesystem.\r\n");
        shell_puts("  Example: mkdir subdir  (relative to cwd)\r\n");
        return -1;
    }

    char resolved[VFS_MAX_PATH];
    if (shell_resolve_path(argv[1], resolved, sizeof(resolved)) < 0) {
        shell_puts("mkdir: path too long\r\n");
        return -1;
    }

    /* Get the mount context and subpath */
    const char *subpath = NULL;
    struct lfs_mount *mnt = vfs_get_mount_ctx(resolved, &subpath);
    if (!mnt) {
        shell_printf("mkdir: %s: Not a mounted filesystem\r\n", resolved);
        return -1;
    }

    int err = littlefs_mkdir(mnt, subpath);
    if (err < 0) {
        if (err == LFS_ERR_EXIST) {
            shell_printf("mkdir: %s: Already exists\r\n", resolved);
        } else {
            shell_printf("mkdir: %s: Failed (error %d)\r\n", resolved, err);
        }
        return -1;
    }

    shell_printf("Created directory %s\r\n", resolved);
    return 0;
}

/*
 * rm <path> - Remove a file or empty directory
 * Supports relative paths.
 */
int cmd_rm(int argc, char *argv[])
{
    if (argc < 2) {
        shell_puts("Usage: rm <path>\r\n");
        shell_puts("  Remove a file or empty directory.\r\n");
        shell_puts("  Example: rm test.txt  (relative to cwd)\r\n");
        return -1;
    }

    char resolved[VFS_MAX_PATH];
    if (shell_resolve_path(argv[1], resolved, sizeof(resolved)) < 0) {
        shell_puts("rm: path too long\r\n");
        return -1;
    }

    /* Get the mount context and subpath */
    const char *subpath = NULL;
    struct lfs_mount *mnt = vfs_get_mount_ctx(resolved, &subpath);
    if (!mnt) {
        shell_printf("rm: %s: Not a mounted filesystem\r\n", resolved);
        return -1;
    }

    int err = littlefs_remove(mnt, subpath);
    if (err < 0) {
        if (err == LFS_ERR_NOENT) {
            shell_printf("rm: %s: No such file or directory\r\n", resolved);
        } else if (err == LFS_ERR_NOTEMPTY) {
            shell_printf("rm: %s: Directory not empty\r\n", resolved);
        } else {
            shell_printf("rm: %s: Failed (error %d)\r\n", resolved, err);
        }
        return -1;
    }

    shell_printf("Removed %s\r\n", resolved);
    return 0;
}

/*
 * mv <src> <dst> - Move/rename a file or directory
 * Supports relative paths.
 */
int cmd_mv(int argc, char *argv[])
{
    if (argc < 3) {
        shell_puts("Usage: mv <source> <dest>\r\n");
        shell_puts("  Move or rename a file/directory.\r\n");
        shell_puts("  Both paths must be in the same filesystem.\r\n");
        shell_puts("  Example: mv old.txt new.txt  (relative to cwd)\r\n");
        return -1;
    }

    char src_resolved[VFS_MAX_PATH];
    char dst_resolved[VFS_MAX_PATH];

    if (shell_resolve_path(argv[1], src_resolved, sizeof(src_resolved)) < 0) {
        shell_puts("mv: source path too long\r\n");
        return -1;
    }
    if (shell_resolve_path(argv[2], dst_resolved, sizeof(dst_resolved)) < 0) {
        shell_puts("mv: destination path too long\r\n");
        return -1;
    }

    /* Get mount contexts for both paths */
    const char *src_subpath = NULL;
    const char *dst_subpath = NULL;
    struct lfs_mount *src_mnt = vfs_get_mount_ctx(src_resolved, &src_subpath);
    struct lfs_mount *dst_mnt = vfs_get_mount_ctx(dst_resolved, &dst_subpath);

    if (!src_mnt) {
        shell_printf("mv: %s: Not a mounted filesystem\r\n", src_resolved);
        return -1;
    }

    if (!dst_mnt) {
        shell_printf("mv: %s: Not a mounted filesystem\r\n", dst_resolved);
        return -1;
    }

    if (src_mnt != dst_mnt) {
        shell_puts("mv: Source and destination must be in the same filesystem\r\n");
        return -1;
    }

    int err = littlefs_rename(src_mnt, src_subpath, dst_subpath);
    if (err < 0) {
        if (err == LFS_ERR_NOENT) {
            shell_printf("mv: %s: No such file or directory\r\n", src_resolved);
        } else {
            shell_printf("mv: Failed (error %d)\r\n", err);
        }
        return -1;
    }

    shell_printf("Moved %s -> %s\r\n", src_resolved, dst_resolved);
    return 0;
}

/*
 * df [path] - Show filesystem statistics
 * Defaults to cwd or /mnt/files if cwd not in a mount.
 * Supports relative paths.
 */
int cmd_df(int argc, char *argv[])
{
    char resolved[VFS_MAX_PATH];
    const char *input_path = ".";  /* Default to cwd */

    if (argc >= 2) {
        input_path = argv[1];
    }

    /* Resolve path */
    if (shell_resolve_path(input_path, resolved, sizeof(resolved)) < 0) {
        shell_puts("df: path too long\r\n");
        return -1;
    }

    /* Get the mount context */
    const char *subpath = NULL;
    struct lfs_mount *mnt = vfs_get_mount_ctx(resolved, &subpath);
    if (!mnt) {
        /* If cwd isn't in a mount, try /mnt/files as fallback */
        if (argc < 2) {
            mnt = vfs_get_mount_ctx("/mnt/files", &subpath);
            if (mnt) {
                strcpy(resolved, "/mnt/files");
            }
        }
        if (!mnt) {
            shell_printf("df: %s: Not a mounted filesystem\r\n", resolved);
            return -1;
        }
    }

    uint32_t total_blocks, used_blocks;
    int err = littlefs_stat(mnt, &total_blocks, &used_blocks);
    if (err < 0) {
        shell_printf("df: Failed to get stats (error %d)\r\n", err);
        return -1;
    }

    /* Get block device info */
    struct blkdev *dev = littlefs_get_blkdev(mnt);
    uint32_t block_size = dev ? dev->block_size : 4096;
    uint32_t free_blocks = total_blocks - used_blocks;

    uint32_t total_kb = (total_blocks * block_size) / 1024;
    uint32_t used_kb = (used_blocks * block_size) / 1024;
    uint32_t free_kb = (free_blocks * block_size) / 1024;
    uint32_t pct_used = total_blocks > 0 ? (used_blocks * 100) / total_blocks : 0;

    shell_puts("Filesystem      Blocks     Used     Free   Use%\r\n");
    shell_printf("%-14s  %6lu   %6lu   %6lu   %3lu%%\r\n",
                resolved, (unsigned long)total_blocks,
                (unsigned long)used_blocks, (unsigned long)free_blocks,
                (unsigned long)pct_used);
    shell_printf("                %5luK   %5luK   %5luK\r\n",
                (unsigned long)total_kb, (unsigned long)used_kb,
                (unsigned long)free_kb);

    return 0;
}

/*
 * truncate <path> <size> - Truncate file to specified size
 * Supports relative paths.
 */
int cmd_truncate(int argc, char *argv[])
{
    if (argc < 3) {
        shell_puts("Usage: truncate <path> <size>\r\n");
        shell_puts("  Truncate or extend file to specified size (in bytes).\r\n");
        shell_puts("  Example: truncate log.txt 0  (relative to cwd)\r\n");
        return -1;
    }

    char resolved[VFS_MAX_PATH];
    if (shell_resolve_path(argv[1], resolved, sizeof(resolved)) < 0) {
        shell_puts("truncate: path too long\r\n");
        return -1;
    }

    /* Parse size */
    uint32_t size;
    if (shell_parse_uint(argv[2], &size) != 0) {
        shell_printf("truncate: Invalid size: %s\r\n", argv[2]);
        return -1;
    }

    /* Get the mount context and subpath */
    const char *subpath = NULL;
    struct lfs_mount *mnt = vfs_get_mount_ctx(resolved, &subpath);
    if (!mnt) {
        shell_printf("truncate: %s: Not a mounted filesystem\r\n", resolved);
        return -1;
    }

    /* Open file for writing */
    int fd = littlefs_file_open(mnt, subpath, LFS_O_RDWR);
    if (fd < 0) {
        shell_printf("truncate: %s: Failed to open file\r\n", resolved);
        return -1;
    }

    /* Truncate to specified size */
    int err = littlefs_file_truncate(mnt, fd, size);
    littlefs_file_close(mnt, fd);

    if (err < 0) {
        shell_printf("truncate: %s: Failed (error %d)\r\n", resolved, err);
        return -1;
    }

    shell_printf("Truncated %s to %lu bytes\r\n", resolved, (unsigned long)size);
    return 0;
}

/*
 * append <path> <content> - Append content to a file
 *
 * Creates the file if it doesn't exist.
 * Useful for logging. Supports relative paths.
 */
int cmd_append(int argc, char *argv[])
{
    if (argc < 3) {
        shell_puts("Usage: append <path> <content>\r\n");
        shell_puts("  Append content to file (creates if needed).\r\n");
        shell_puts("  A trailing newline is added automatically.\r\n");
        shell_puts("  Escapes: \\n \\t \\r \\\\ \\\" \\' \\0 \\xNN\r\n");
        shell_puts("  Example: append log.txt Entry 1  (relative to cwd)\r\n");
        return -1;
    }

    char resolved[VFS_MAX_PATH];
    if (shell_resolve_path(argv[1], resolved, sizeof(resolved)) < 0) {
        shell_puts("append: path too long\r\n");
        return -1;
    }

    /* Get the mount context and subpath */
    const char *subpath = NULL;
    struct lfs_mount *mnt = vfs_get_mount_ctx(resolved, &subpath);
    if (!mnt) {
        shell_printf("append: %s: Not a mounted filesystem\r\n", resolved);
        return -1;
    }

    /* Build content from remaining arguments (with escape translation).
     * Reserve one byte at the end for the trailing newline. */
    static char content[4096];
    int pos = shell_build_content(argc, argv, 2, content, (int)sizeof(content) - 1);
    if (pos < 0) {
        shell_printf("append: content too long (max %d bytes)\r\n",
                    (int)sizeof(content) - 2);
        return -1;
    }
    /* Add newline for log entries */
    content[pos++] = '\n';
    content[pos] = '\0';

    /* Open file for appending (create if needed) */
    int fd = littlefs_file_open(mnt, subpath, LFS_O_WRONLY | LFS_O_CREAT | LFS_O_APPEND);
    if (fd < 0) {
        shell_printf("append: %s: Failed to open file\r\n", resolved);
        return -1;
    }

    /* Write content */
    int written = littlefs_file_write(mnt, fd, content, pos);
    littlefs_file_close(mnt, fd);

    if (written < 0) {
        shell_printf("append: %s: Write failed\r\n", resolved);
        return -1;
    }

    shell_printf("Appended %d bytes to %s\r\n", written, resolved);
    return 0;
}

/*
 * cp <src> <dst> - Copy a file
 * Supports relative paths. Cross-mount copy is supported.
 */
int cmd_cp(int argc, char *argv[])
{
    if (argc < 3) {
        shell_puts("Usage: cp <source> <dest>\r\n");
        shell_puts("  Copy a file. Cross-mount copy is supported.\r\n");
        shell_puts("  Example: cp hello.txt backup.txt\r\n");
        return -1;
    }

    char src_resolved[VFS_MAX_PATH];
    char dst_resolved[VFS_MAX_PATH];

    if (shell_resolve_path(argv[1], src_resolved, sizeof(src_resolved)) < 0) {
        shell_puts("cp: source path too long\r\n");
        return -1;
    }
    if (shell_resolve_path(argv[2], dst_resolved, sizeof(dst_resolved)) < 0) {
        shell_puts("cp: destination path too long\r\n");
        return -1;
    }

    /* Get mount contexts for both paths */
    const char *src_subpath = NULL;
    const char *dst_subpath = NULL;
    struct lfs_mount *src_mnt = vfs_get_mount_ctx(src_resolved, &src_subpath);
    struct lfs_mount *dst_mnt = vfs_get_mount_ctx(dst_resolved, &dst_subpath);

    if (!src_mnt) {
        shell_printf("cp: %s: Not a mounted filesystem\r\n", src_resolved);
        return -1;
    }

    if (!dst_mnt) {
        shell_printf("cp: %s: Not a mounted filesystem\r\n", dst_resolved);
        return -1;
    }

    /* Open source for reading */
    int src_fd = littlefs_file_open(src_mnt, src_subpath, LFS_O_RDONLY);
    if (src_fd < 0) {
        shell_printf("cp: %s: Cannot open source file\r\n", src_resolved);
        return -1;
    }

    /* Get source file size */
    int src_size = littlefs_file_size(src_mnt, src_fd);
    if (src_size < 0) {
        littlefs_file_close(src_mnt, src_fd);
        shell_printf("cp: %s: Cannot get file size\r\n", src_resolved);
        return -1;
    }

    /* Open destination for writing */
    int dst_fd = littlefs_file_open(dst_mnt, dst_subpath,
                                     LFS_O_WRONLY | LFS_O_CREAT | LFS_O_TRUNC);
    if (dst_fd < 0) {
        littlefs_file_close(src_mnt, src_fd);
        shell_printf("cp: %s: Cannot create destination file\r\n", dst_resolved);
        return -1;
    }

    /* Copy in chunks */
    char buf[512];
    int total_copied = 0;
    int bytes_read;

    while ((bytes_read = littlefs_file_read(src_mnt, src_fd, buf, sizeof(buf))) > 0) {
        int written = littlefs_file_write(dst_mnt, dst_fd, buf, bytes_read);
        if (written != bytes_read) {
            littlefs_file_close(src_mnt, src_fd);
            littlefs_file_close(dst_mnt, dst_fd);
            shell_printf("cp: Write error after %d bytes\r\n", total_copied);
            return -1;
        }
        total_copied += written;
    }

    littlefs_file_close(src_mnt, src_fd);
    littlefs_file_close(dst_mnt, dst_fd);

    shell_printf("Copied %d bytes: %s -> %s\r\n", total_copied, src_resolved, dst_resolved);
    return 0;
}

/*
 * touch <path> - Create an empty file or update timestamp
 * Creates the file if it doesn't exist.
 * Supports relative paths.
 */
int cmd_touch(int argc, char *argv[])
{
    if (argc < 2) {
        shell_puts("Usage: touch <path>\r\n");
        shell_puts("  Create an empty file if it doesn't exist.\r\n");
        shell_puts("  Example: touch newfile.txt\r\n");
        return -1;
    }

    char resolved[VFS_MAX_PATH];
    if (shell_resolve_path(argv[1], resolved, sizeof(resolved)) < 0) {
        shell_puts("touch: path too long\r\n");
        return -1;
    }

    /* Get the mount context and subpath */
    const char *subpath = NULL;
    struct lfs_mount *mnt = vfs_get_mount_ctx(resolved, &subpath);
    if (!mnt) {
        shell_printf("touch: %s: Not a mounted filesystem\r\n", resolved);
        return -1;
    }

    /* Try to open existing file, or create new one */
    int fd = littlefs_file_open(mnt, subpath, LFS_O_RDWR | LFS_O_CREAT);
    if (fd < 0) {
        shell_printf("touch: %s: Failed to create file\r\n", resolved);
        return -1;
    }

    littlefs_file_close(mnt, fd);
    shell_printf("Touched %s\r\n", resolved);
    return 0;
}

/*
 * stat <path> - Show file/directory information
 * Supports relative paths.
 */
int cmd_stat(int argc, char *argv[])
{
    if (argc < 2) {
        shell_puts("Usage: stat <path>\r\n");
        shell_puts("  Show file or directory information.\r\n");
        shell_puts("  Example: stat hello.txt\r\n");
        return -1;
    }

    char resolved[VFS_MAX_PATH];
    if (shell_resolve_path(argv[1], resolved, sizeof(resolved)) < 0) {
        shell_puts("stat: path too long\r\n");
        return -1;
    }

    /* Try VFS stat first */
    struct vfs_entry_info info;
    if (vfs_stat_path(resolved, &info) == 0) {
        shell_printf("  File: %s\r\n", resolved);
        shell_printf("  Type: %s\r\n", info.type == 1 ? "directory" : "regular file");
        shell_printf("  Size: %lu bytes\r\n", (unsigned long)info.size);
        return 0;
    }

    /* Try VFS node lookup for virtual files */
    const char *subpath = NULL;
    struct vfs_node *node = vfs_lookup_mount(resolved, &subpath);
    if (node) {
        /* If it's a mount point with subpath, we already tried vfs_stat_path above */
        if (node->type == VFS_NODE_MOUNT && subpath && subpath[0] != '\0' &&
            !(subpath[0] == '/' && subpath[1] == '\0')) {
            /* Path within mount but file doesn't exist */
            shell_printf("stat: %s: No such file or directory\r\n", resolved);
            return -1;
        }

        shell_printf("  File: %s\r\n", resolved);
        const char *type_str;
        switch (node->type) {
            case VFS_NODE_DIR:   type_str = "directory"; break;
            case VFS_NODE_FILE:  type_str = "virtual file"; break;
            case VFS_NODE_MOUNT: type_str = "mount point"; break;
            default:             type_str = "unknown"; break;
        }
        shell_printf("  Type: %s\r\n", type_str);
        if (node->type == VFS_NODE_FILE) {
            /* Try to get size by reading */
            char buf[1024];
            int len = vfs_read(node, buf, sizeof(buf));
            if (len >= 0) {
                shell_printf("  Size: %d bytes\r\n", len);
            }
        }
        return 0;
    }

    shell_printf("stat: %s: No such file or directory\r\n", resolved);
    return -1;
}

/*
 * Recursive tree helper
 */
static void tree_recurse(struct lfs_mount *mnt, const char *path, int depth, int max_depth)
{
    if (depth > max_depth) return;

    /* Build indent string */
    char indent[64];
    int i;
    for (i = 0; i < depth * 2 && i < 62; i++) {
        indent[i] = ' ';
    }
    indent[i] = '\0';

    /* Open directory */
    int dh = littlefs_dir_open(mnt, path);
    if (dh < 0) return;

    struct lfs_entry_info entry;
    while (littlefs_dir_read(mnt, dh, &entry) > 0) {
        /* Skip . and .. */
        if (entry.name[0] == '.' &&
            (entry.name[1] == '\0' ||
             (entry.name[1] == '.' && entry.name[2] == '\0'))) {
            continue;
        }

        shell_printf("%s", indent);
        if (entry.type == 1) {
            shell_printf("%s/\r\n", entry.name);

            /* Recurse into subdirectory */
            char subpath[VFS_MAX_PATH];
            size_t path_len = strlen(path);
            size_t name_len = strlen(entry.name);

            if (path_len + name_len + 2 < sizeof(subpath)) {
                strcpy(subpath, path);
                if (path_len > 1) {
                    subpath[path_len] = '/';
                    strcpy(subpath + path_len + 1, entry.name);
                } else {
                    strcpy(subpath + 1, entry.name);
                }
                tree_recurse(mnt, subpath, depth + 1, max_depth);
            }
        } else {
            shell_printf("%s  (%lu bytes)\r\n", entry.name, (unsigned long)entry.size);
        }
    }

    littlefs_dir_close(mnt, dh);
}

/*
 * tree [path] [depth] - Recursive directory listing
 * Defaults to cwd. Supports relative paths.
 */
int cmd_tree(int argc, char *argv[])
{
    char resolved[VFS_MAX_PATH];
    const char *input_path = ".";
    int max_depth = 5;  /* Default max depth */

    if (argc >= 2) {
        input_path = argv[1];
    }
    if (argc >= 3) {
        uint32_t d;
        if (shell_parse_uint(argv[2], &d) == 0 && d > 0) {
            max_depth = (int)d;
        }
    }

    if (shell_resolve_path(input_path, resolved, sizeof(resolved)) < 0) {
        shell_puts("tree: path too long\r\n");
        return -1;
    }

    /* Get mount context */
    const char *subpath = NULL;
    struct lfs_mount *mnt = vfs_get_mount_ctx(resolved, &subpath);
    if (!mnt) {
        /* Try VFS listing for virtual directories */
        struct vfs_node *node = vfs_lookup_mount(resolved, &subpath);
        if (!node) {
            shell_printf("tree: %s: No such directory\r\n", resolved);
            return -1;
        }

        shell_printf("%s\r\n", resolved);
        if (node->type == VFS_NODE_DIR) {
            /* Simple VFS listing (non-recursive for virtual dirs) */
            vfs_list(node, ls_print_entry, NULL);
        }
        return 0;
    }

    shell_printf("%s\r\n", resolved);
    tree_recurse(mnt, subpath, 1, max_depth);

    return 0;
}

/*
 * wc <path> - Count lines, words, and bytes in a file
 * Supports relative paths.
 */
int cmd_wc(int argc, char *argv[])
{
    if (argc < 2) {
        shell_puts("Usage: wc <path>\r\n");
        shell_puts("  Count lines, words, and bytes in a file.\r\n");
        shell_puts("  Example: wc readme.txt\r\n");
        return -1;
    }

    char resolved[VFS_MAX_PATH];
    if (shell_resolve_path(argv[1], resolved, sizeof(resolved)) < 0) {
        shell_puts("wc: path too long\r\n");
        return -1;
    }

    /* Get mount context */
    const char *subpath = NULL;
    struct lfs_mount *mnt = vfs_get_mount_ctx(resolved, &subpath);
    if (!mnt) {
        shell_printf("wc: %s: Not a mounted filesystem\r\n", resolved);
        return -1;
    }

    int fd = littlefs_file_open(mnt, subpath, LFS_O_RDONLY);
    if (fd < 0) {
        shell_printf("wc: %s: Cannot open file\r\n", resolved);
        return -1;
    }

    char buf[256];
    int bytes_read;
    unsigned long lines = 0, words = 0, bytes = 0;
    int in_word = 0;

    while ((bytes_read = littlefs_file_read(mnt, fd, buf, sizeof(buf))) > 0) {
        for (int i = 0; i < bytes_read; i++) {
            bytes++;
            char c = buf[i];

            if (c == '\n') {
                lines++;
            }

            /* Word counting: whitespace-separated */
            if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
                in_word = 0;
            } else {
                if (!in_word) {
                    words++;
                    in_word = 1;
                }
            }
        }
    }

    littlefs_file_close(mnt, fd);

    shell_printf("  %7lu  %7lu  %7lu  %s\r\n", lines, words, bytes, resolved);
    return 0;
}

/*
 * hexdump <path> [offset] [length] - Hex dump of file contents
 * Supports relative paths.
 */
int cmd_hexdump(int argc, char *argv[])
{
    if (argc < 2) {
        shell_puts("Usage: hexdump <path> [offset] [length]\r\n");
        shell_puts("  Display file contents in hexadecimal.\r\n");
        shell_puts("  Default: first 256 bytes.\r\n");
        shell_puts("  Example: hexdump model.bin 0 64\r\n");
        return -1;
    }

    char resolved[VFS_MAX_PATH];
    if (shell_resolve_path(argv[1], resolved, sizeof(resolved)) < 0) {
        shell_puts("hexdump: path too long\r\n");
        return -1;
    }

    size_t offset = 0;
    size_t length = 256;  /* Default length */

    if (argc >= 3) {
        uint32_t val;
        if (shell_parse_uint(argv[2], &val) == 0) {
            offset = val;
        }
    }
    if (argc >= 4) {
        uint32_t val;
        if (shell_parse_uint(argv[3], &val) == 0) {
            length = val;
            if (length > 4096) length = 4096;  /* Cap at 4KB */
        }
    }

    /* Get mount context */
    const char *subpath = NULL;
    struct lfs_mount *mnt = vfs_get_mount_ctx(resolved, &subpath);
    if (!mnt) {
        shell_printf("hexdump: %s: Not a mounted filesystem\r\n", resolved);
        return -1;
    }

    int fd = littlefs_file_open(mnt, subpath, LFS_O_RDONLY);
    if (fd < 0) {
        shell_printf("hexdump: %s: Cannot open file\r\n", resolved);
        return -1;
    }

    /* Seek to offset */
    if (offset > 0) {
        littlefs_file_seek(mnt, fd, (int32_t)offset, 0);  /* SEEK_SET */
    }

    /* Read and display */
    unsigned char buf[16];
    size_t total_read = 0;

    while (total_read < length) {
        size_t to_read = 16;
        if (total_read + to_read > length) {
            to_read = length - total_read;
        }

        int bytes_read = littlefs_file_read(mnt, fd, buf, to_read);
        if (bytes_read <= 0) break;

        /* Print offset */
        shell_printf("%08lx  ", (unsigned long)(offset + total_read));

        /* Print hex bytes */
        for (int i = 0; i < 16; i++) {
            if (i < bytes_read) {
                shell_printf("%02x ", buf[i]);
            } else {
                shell_puts("   ");
            }
            if (i == 7) shell_putc(' ');
        }

        shell_puts(" |");

        /* Print ASCII */
        for (int i = 0; i < bytes_read; i++) {
            char c = buf[i];
            if (c >= 0x20 && c < 0x7F) {
                shell_putc(c);
            } else {
                shell_putc('.');
            }
        }

        shell_puts("|\r\n");
        total_read += bytes_read;
    }

    littlefs_file_close(mnt, fd);

    shell_printf("%08lx\r\n", (unsigned long)(offset + total_read));
    return 0;
}

/*
 * Simple pattern matching helper (supports * and ? wildcards)
 */
static int pattern_match(const char *pattern, const char *str)
{
    while (*pattern && *str) {
        if (*pattern == '*') {
            pattern++;
            if (*pattern == '\0') return 1;  /* Trailing * matches all */
            /* Try matching rest of pattern at each position */
            while (*str) {
                if (pattern_match(pattern, str)) return 1;
                str++;
            }
            return pattern_match(pattern, str);
        } else if (*pattern == '?' || *pattern == *str) {
            pattern++;
            str++;
        } else {
            return 0;
        }
    }

    /* Handle trailing wildcards */
    while (*pattern == '*') pattern++;

    return (*pattern == '\0' && *str == '\0');
}

/*
 * grep <pattern> <path> - Search for pattern in file
 * Simple substring search. Supports relative paths.
 */
int cmd_grep(int argc, char *argv[])
{
    if (argc < 3) {
        shell_puts("Usage: grep <pattern> <path>\r\n");
        shell_puts("  Search for pattern in file (case-sensitive substring).\r\n");
        shell_puts("  Example: grep error log.txt\r\n");
        return -1;
    }

    const char *pattern = argv[1];
    size_t pattern_len = strlen(pattern);

    char resolved[VFS_MAX_PATH];
    if (shell_resolve_path(argv[2], resolved, sizeof(resolved)) < 0) {
        shell_puts("grep: path too long\r\n");
        return -1;
    }

    /* Get mount context */
    const char *subpath = NULL;
    struct lfs_mount *mnt = vfs_get_mount_ctx(resolved, &subpath);
    if (!mnt) {
        shell_printf("grep: %s: Not a mounted filesystem\r\n", resolved);
        return -1;
    }

    int fd = littlefs_file_open(mnt, subpath, LFS_O_RDONLY);
    if (fd < 0) {
        shell_printf("grep: %s: Cannot open file\r\n", resolved);
        return -1;
    }

    /* Read line by line */
    char line[512];
    int lpos = 0;
    int line_num = 1;
    int matches = 0;
    char buf[256];
    int bytes_read;

    while ((bytes_read = littlefs_file_read(mnt, fd, buf, sizeof(buf))) > 0) {
        for (int i = 0; i < bytes_read; i++) {
            char c = buf[i];

            if (c == '\n' || lpos >= (int)sizeof(line) - 1) {
                line[lpos] = '\0';

                /* Search for pattern in line */
                int found = 0;
                for (int j = 0; j <= lpos - (int)pattern_len; j++) {
                    int match = 1;
                    for (size_t k = 0; k < pattern_len; k++) {
                        if (line[j + k] != pattern[k]) {
                            match = 0;
                            break;
                        }
                    }
                    if (match) {
                        found = 1;
                        break;
                    }
                }

                if (found) {
                    shell_printf("%d: %s\r\n", line_num, line);
                    matches++;
                }

                lpos = 0;
                line_num++;
            } else {
                line[lpos++] = c;
            }
        }
    }

    /* Check last line if no newline at end */
    if (lpos > 0) {
        line[lpos] = '\0';
        int found = 0;
        for (int j = 0; j <= lpos - (int)pattern_len; j++) {
            int match = 1;
            for (size_t k = 0; k < pattern_len; k++) {
                if (line[j + k] != pattern[k]) {
                    match = 0;
                    break;
                }
            }
            if (match) {
                found = 1;
                break;
            }
        }
        if (found) {
            shell_printf("%d: %s\r\n", line_num, line);
            matches++;
        }
    }

    littlefs_file_close(mnt, fd);

    if (matches == 0) {
        shell_puts("(no matches)\r\n");
    } else {
        shell_printf("(%d matches)\r\n", matches);
    }

    return 0;
}

/*
 * Recursive find helper
 */
static void find_recurse(struct lfs_mount *mnt, const char *base_path,
                         const char *path, const char *pattern, int *count)
{
    int dh = littlefs_dir_open(mnt, path);
    if (dh < 0) return;

    struct lfs_entry_info entry;
    while (littlefs_dir_read(mnt, dh, &entry) > 0) {
        /* Skip . and .. */
        if (entry.name[0] == '.' &&
            (entry.name[1] == '\0' ||
             (entry.name[1] == '.' && entry.name[2] == '\0'))) {
            continue;
        }

        /* Build full path for display */
        char full_path[VFS_MAX_PATH];
        size_t path_len = strlen(path);
        size_t name_len = strlen(entry.name);

        if (path_len + name_len + 2 < sizeof(full_path)) {
            strcpy(full_path, path);
            if (path_len > 1 || (path_len == 1 && path[0] != '/')) {
                full_path[path_len] = '/';
                strcpy(full_path + path_len + 1, entry.name);
            } else if (path_len == 1 && path[0] == '/') {
                strcpy(full_path + 1, entry.name);
            } else {
                strcpy(full_path, entry.name);
            }

            /* Check if name matches pattern */
            if (pattern_match(pattern, entry.name)) {
                shell_printf("%s%s%s\r\n", base_path, full_path,
                            entry.type == 1 ? "/" : "");
                (*count)++;
            }

            /* Recurse into directories */
            if (entry.type == 1) {
                find_recurse(mnt, base_path, full_path, pattern, count);
            }
        }
    }

    littlefs_dir_close(mnt, dh);
}

/*
 * find <path> <pattern> - Find files by name pattern
 * Supports wildcards: * (any chars), ? (single char)
 * Supports relative paths.
 */
int cmd_find(int argc, char *argv[])
{
    if (argc < 3) {
        shell_puts("Usage: find <path> <pattern>\r\n");
        shell_puts("  Find files matching pattern (recursive).\r\n");
        shell_puts("  Wildcards: * (any chars), ? (single char)\r\n");
        shell_puts("  Example: find /mnt/files *.txt\r\n");
        shell_puts("  Example: find . log*\r\n");
        return -1;
    }

    char resolved[VFS_MAX_PATH];
    if (shell_resolve_path(argv[1], resolved, sizeof(resolved)) < 0) {
        shell_puts("find: path too long\r\n");
        return -1;
    }

    const char *pattern = argv[2];

    /* Get mount context */
    const char *subpath = NULL;
    struct lfs_mount *mnt = vfs_get_mount_ctx(resolved, &subpath);
    if (!mnt) {
        shell_printf("find: %s: Not a mounted filesystem\r\n", resolved);
        return -1;
    }

    int count = 0;

    /* Calculate the base path prefix (mount point) */
    size_t resolved_len = strlen(resolved);
    size_t subpath_len = subpath ? strlen(subpath) : 0;
    char base_path[VFS_MAX_PATH];

    if (subpath_len > 0 && resolved_len >= subpath_len) {
        size_t base_len = resolved_len - subpath_len;
        for (size_t i = 0; i < base_len && i < sizeof(base_path) - 1; i++) {
            base_path[i] = resolved[i];
        }
        base_path[base_len] = '\0';
    } else {
        strcpy(base_path, resolved);
    }

    find_recurse(mnt, base_path, subpath, pattern, &count);

    if (count == 0) {
        shell_puts("(no files found)\r\n");
    } else {
        shell_printf("(%d files found)\r\n", count);
    }

    return 0;
}
