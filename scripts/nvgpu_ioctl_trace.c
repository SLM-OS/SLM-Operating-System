/*
 * nvgpu_ioctl_trace.c — LD_PRELOAD interposer for nvgpu ioctls.
 *
 * Dumps the arg buffer (IN + OUT) for every ioctl with nvgpu magic
 * bytes ('G'=0x47 ctrl, 'H'=0x48 channel, 'A'=0x41 AS, 'T'=0x54 TSG).
 * Captures the raw struct contents CUDA passes so we can compare to
 * what the helper in scripts/gpu-channel-helper.c passes.
 *
 * Build: gcc -fPIC -shared -o nvgpu_ioctl_trace.so nvgpu_ioctl_trace.c -ldl
 * Use:   LD_PRELOAD=./nvgpu_ioctl_trace.so ./minikick 2>ioctl.log
 */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdint.h>
#include <sys/ioctl.h>
#include <string.h>

static int (*real_ioctl)(int fd, unsigned long req, void *arg) = NULL;

static void dump_hex(const void *buf, size_t n, const char *indent)
{
    const unsigned char *p = buf;
    for (size_t i = 0; i < n; i++) {
        if (i % 16 == 0) {
            if (i) fprintf(stderr, "\n");
            fprintf(stderr, "%s", indent);
        }
        fprintf(stderr, "%02x ", p[i]);
    }
    fprintf(stderr, "\n");
}

int ioctl(int fd, unsigned long req, ...)
{
    va_list ap;
    va_start(ap, req);
    void *arg = va_arg(ap, void *);
    va_end(ap);

    if (!real_ioctl)
        real_ioctl = dlsym(RTLD_NEXT, "ioctl");

    /* Decode _IOC fields (from <asm-generic/ioctl.h>):
     *   bits [31:30] dir, [29:16] size, [15:8] type, [7:0] nr */
    unsigned int dir  = (req >> 30) & 0x3;
    unsigned int size = (req >> 16) & 0x3fff;
    unsigned int type = (req >>  8) & 0xff;
    unsigned int nr   = (req >>  0) & 0xff;

    int is_nvgpu = (type == 0x47 || type == 0x48 ||
                    type == 0x41 || type == 0x54);

    if (is_nvgpu && arg && size && size <= 512 && (dir & 1)) {
        /* IN side: caller-provided data (WRITE bit in dir means
         * userspace writes to kernel = arg is input). */
        fprintf(stderr,
            "[trace] fd=%d magic=0x%02x nr=0x%02x(%u) size=%u dir=%u IN:\n",
            fd, type, nr, nr, size, dir);
        dump_hex(arg, size, "  ");
    }

    int rc = real_ioctl(fd, req, arg);

    if (is_nvgpu && arg && size && size <= 512 && (dir & 2) && rc >= 0) {
        /* OUT side: kernel-written data on successful return. */
        fprintf(stderr,
            "[trace] fd=%d magic=0x%02x nr=0x%02x(%u) size=%u rc=%d OUT:\n",
            fd, type, nr, nr, size, rc);
        dump_hex(arg, size, "  ");
    }

    if (is_nvgpu && rc < 0) {
        fprintf(stderr,
            "[trace] fd=%d magic=0x%02x nr=0x%02x(%u) size=%u FAILED rc=%d\n",
            fd, type, nr, nr, size, rc);
    }

    return rc;
}
