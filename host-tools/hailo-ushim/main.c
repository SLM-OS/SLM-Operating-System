/*
 * main.c — hailo-ushim entry + test driver.
 *
 * v1 scope: sanity-check /dev/hailo0 access via FW_CONTROL identify.
 * Subsequent revisions add VDMA buffer map + desc-list program +
 * launch_transfer so we can replay SLM-OS's boundary-submit sequence.
 */

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

/*
 * Use OpenSSL's MD5 — available on Pi OS and all Linux dev hosts. Build
 * with -lcrypto. HailoRT's fw_control requires MD5 of the request
 * buffer in the `expected_md5` field; fw validates it before parsing
 * the payload. SLM-OS does the same via its vendored md5.c.
 */
#include <openssl/md5.h>

static void md5_compute(const void *data, size_t len, uint8_t out[16])
{
    MD5_CTX ctx;
    MD5_Init(&ctx);
    MD5_Update(&ctx, data, len);
    MD5_Final(out, &ctx);
}

/*
 * Pull the public hailo_pci IOCTL definitions from our cached copy of
 * the v4.23 driver source. The header's Linux-userspace branch
 * (selected by __linux__ && !__KERNEL__) gives us char-based IOCTL
 * magics ('g'/'v'/'n') and Linux's _IOW/_IOR/_IOWR macros.
 */
#include "hailo-ioctl-common.h"

static const char *HAILO_DEV_PATH = "/dev/hailo0";

/*
 * Hailo control-protocol common header — same 16-byte layout as
 * hailo_control.h in SLM-OS. BE-encoded in the FW_CONTROL buffer so
 * the firmware (running on the NPU's APP/CORE CPU) reads it in its
 * native byte order after bswap at the host.
 */
struct ctrl_common_hdr {
    uint32_t version;
    uint32_t flags;
    uint32_t sequence;
    uint32_t opcode;
} __attribute__((packed));

static uint32_t htobe32_(uint32_t x)
{
    return  (x << 24)
          | ((x & 0x0000FF00u) << 8)
          | ((x & 0x00FF0000u) >> 8)
          |  (x >> 24);
}

#define HAILO_CONTROL_PROTOCOL_VERSION        2u
#define HAILO_CONTROL_OPCODE_IDENTIFY         0x00
/* PCIE_EXPECTED_MD5_LENGTH comes from hailo-ioctl-common.h */

static int send_fw_control(int fd, uint32_t opcode,
                           const void *req_body, uint32_t req_body_len,
                           void *resp_body, uint32_t *resp_body_len,
                           bool core_cpu)
{
    struct hailo_fw_control cmd;
    memset(&cmd, 0, sizeof(cmd));

    /* Wire format for a request: [common_hdr (16)][parameter_count
     * BE u32 (4)][body]. Even opcodes with no parameters (like
     * IDENTIFY) still include parameter_count=0 — the fw's decoder
     * reads past it before the body. Without it, all body fields
     * read 4 bytes earlier than expected. */
    struct ctrl_common_hdr *hdr = (struct ctrl_common_hdr *)cmd.buffer;
    static uint32_t seq = 0;
    hdr->version  = htobe32_(HAILO_CONTROL_PROTOCOL_VERSION);
    hdr->flags    = 0;
    hdr->sequence = htobe32_(++seq);
    hdr->opcode   = htobe32_(opcode);

    /* parameter_count = (body_len > 0) ? 1 : 0. In practice we have
     * one body blob per opcode for Phase 8 work, so match HailoRT's
     * convention: 0 for empty requests, 1 for body-carrying ones. */
    uint32_t param_count_be = htobe32_(req_body_len > 0 ? 1u : 0u);
    memcpy(cmd.buffer + sizeof(*hdr), &param_count_be, sizeof(param_count_be));

    if (req_body_len > 0 && req_body) {
        size_t off = sizeof(*hdr) + sizeof(param_count_be);
        if (off + req_body_len > sizeof(cmd.buffer)) {
            fprintf(stderr, "send_fw_control: req too large (%u)\n", req_body_len);
            return -EINVAL;
        }
        memcpy(cmd.buffer + off, req_body, req_body_len);
    }

    cmd.buffer_len = sizeof(*hdr) + sizeof(param_count_be) + req_body_len;
    cmd.timeout_ms = 5000;
    cmd.cpu_id     = core_cpu ? HAILO_CPU_ID_CPU1 : HAILO_CPU_ID_CPU0;

    /* MD5 of the payload bytes. fw rejects requests whose md5 doesn't
     * match — silent timeout on the reader side (no response ever
     * returned). See SLM-OS hailo_control.c's request_build for the
     * same computation. */
    md5_compute(cmd.buffer, cmd.buffer_len, cmd.expected_md5);

    /* The ioctl is IN+OUT: driver copies request, waits for fw to
     * produce a response, copies response back into the same buffer. */
    if (ioctl(fd, HAILO_FW_CONTROL, &cmd) < 0) {
        fprintf(stderr, "HAILO_FW_CONTROL ioctl failed: %s\n", strerror(errno));
        return -errno;
    }

    if (resp_body && resp_body_len) {
        uint32_t body_off = sizeof(struct ctrl_common_hdr);
        /* Response layout: same common_hdr (16 B), then response body. */
        if (cmd.buffer_len < body_off) {
            fprintf(stderr, "response too short (%u)\n", cmd.buffer_len);
            return -EIO;
        }
        uint32_t rlen = cmd.buffer_len - body_off;
        if (rlen > *resp_body_len) rlen = *resp_body_len;
        memcpy(resp_body, cmd.buffer + body_off, rlen);
        *resp_body_len = rlen;
    }
    return 0;
}

static int cmd_identify(int fd)
{
    uint8_t  resp[256];
    uint32_t resp_len = sizeof(resp);

    printf("sending IDENTIFY (opcode 0x%02x) to /dev/hailo0...\n",
           HAILO_CONTROL_OPCODE_IDENTIFY);
    int rc = send_fw_control(fd, HAILO_CONTROL_OPCODE_IDENTIFY,
                             NULL, 0, resp, &resp_len, /*core_cpu=*/false);
    if (rc != 0) return rc;

    printf("response %u bytes:\n", resp_len);
    for (uint32_t i = 0; i < resp_len; i += 16) {
        printf("  [%03x]", i);
        for (uint32_t j = i; j < i + 16 && j < resp_len; j++) {
            printf(" %02x", resp[j]);
        }
        printf("\n");
    }
    return 0;
}

static void usage(const char *prog)
{
    fprintf(stderr,
        "usage: %s <command>\n"
        "commands:\n"
        "  --identify       Send FW_CONTROL IDENTIFY to /dev/hailo0\n",
        prog);
}

int main(int argc, char **argv)
{
    if (argc < 2) { usage(argv[0]); return 2; }

    int fd = open(HAILO_DEV_PATH, O_RDWR);
    if (fd < 0) {
        fprintf(stderr, "open %s: %s\n", HAILO_DEV_PATH, strerror(errno));
        fprintf(stderr, "(is hailo_pci loaded? does the device exist?)\n");
        return 1;
    }

    int rc = 1;
    if (strcmp(argv[1], "--identify") == 0) {
        rc = cmd_identify(fd);
    } else {
        usage(argv[0]);
    }

    close(fd);
    return rc == 0 ? 0 : 1;
}
