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
#include "hailo_dev.h"

#include <sys/mman.h>
#include <time.h>

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
                           uint32_t parameter_count,
                           const void *req_body, uint32_t req_body_len,
                           void *resp_body, uint32_t *resp_body_len,
                           bool core_cpu)
{
    struct hailo_fw_control cmd;
    memset(&cmd, 0, sizeof(cmd));

    /* Wire format for a request: [common_hdr (16)][parameter_count
     * BE u32 (4)][body]. The body is parameter-count-specific:
     *   parameter_count=0 → body is empty (IDENTIFY, CLEAR_APPS, ...)
     *   parameter_count=1 → body is a single length-prefixed blob
     *     (SET_CONTEXT_INFO pre-body: 4 BE length + raw bytes)
     *   parameter_count=N → body is N length-prefixed values
     *     (CHANGE_CONTEXT_SWITCH_STATUS has 4: state, app, batch_sz,
     *     batch_cnt). Callers assemble the full body themselves;
     *     this helper just prepends the common header + count. */
    struct ctrl_common_hdr *hdr = (struct ctrl_common_hdr *)cmd.buffer;
    static uint32_t seq = 0;
    hdr->version  = htobe32_(HAILO_CONTROL_PROTOCOL_VERSION);
    hdr->flags    = 0;
    hdr->sequence = htobe32_(++seq);
    hdr->opcode   = htobe32_(opcode);

    uint32_t param_count_be = htobe32_(parameter_count);
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

/*
 * Decode and print the fw response status. The response buffer
 * `resp` starts AFTER the 16-byte common_header — its first 8 bytes
 * are fw's (major_status, minor_status) pair per SLM-OS's
 * hailo_control.h response_header layout. major_status=0 means fw
 * accepted the RPC; anything else is a fw-side error code (see
 * hailo_errors.h for the taxonomy).
 */
static void print_resp_status(const char *prefix,
                              const uint8_t *resp, uint32_t resp_len)
{
    printf("%sresp_len=%u", prefix, resp_len);
    if (resp_len >= 8) {
        uint32_t major_status, minor_status;
        memcpy(&major_status, resp + 0, 4);
        memcpy(&minor_status, resp + 4, 4);
        printf(" major_status=0x%08x minor_status=0x%08x",
               major_status, minor_status);
        if (major_status == 0) {
            printf(" [ACCEPTED]");
        } else {
            printf(" [REJECTED]");
        }
    }
    printf("\n");
}

/* ------------------------------------------------------------------------ */
/* Context-switch RPC opcodes                                                  */
/* Extracted from kernel/ai_accel/hailo/hailo_control.h — SLM-OS uses the     */
/* same values. All CS opcodes target CPU_ID_CORE_CPU (core_cpu=true).         */
/* ------------------------------------------------------------------------ */
#define OPCODE_CS_SET_NETWORK_GROUP_HEADER  0x20u
#define OPCODE_CS_SET_CONTEXT_INFO          0x21u
#define OPCODE_CS_CHANGE_STATUS             0x25u
#define OPCODE_CS_CLEAR_CONFIGURED_APPS     0x47u
#define OPCODE_GET_HW_CONSTS                0x48u

/* CS state-machine targets — passed as the first parameter of
 * CHANGE_CONTEXT_SWITCH_STATUS. Values match SLM-OS's
 * hailo_cs_state enum in hailo_control.h:
 *   RESET   = 0 — tear down any previous configuration
 *   ENABLED = 1 — arm the network group for inference */
#define CS_STATE_RESET    0u
#define CS_STATE_ENABLED  1u

/*
 * CHANGE_CONTEXT_SWITCH_STATUS (opcode 0x25, CORE CPU).
 *
 * Wire body after common_hdr + parameter_count=4:
 *   [BE u32 length=1][u8 state]
 *   [BE u32 length=1][u8 application_index]
 *   [BE u32 length=2][LE u16 dynamic_batch_size]
 *   [BE u32 length=2][LE u16 batch_count]
 *
 * Total body bytes: 5+5+6+6 = 22.
 *
 * SLM-OS's ctxsmoke calls this twice:
 *   RESET   — state=0, app=0xff, batch_size=0, batch_count=0
 *   ENABLED — state=1, app=0,    batch_size=0, batch_count=0
 * Both return rc=0 when fw is happy; fw rejects ENABLED if the four
 * SET_CONTEXT_INFO calls haven't been fired first.
 */
static int cmd_cs_change_status(int fd, uint8_t state, uint8_t app_index,
                                uint16_t batch_size, uint16_t batch_count,
                                const char *label)
{
    uint8_t body[22];
    size_t  off = 0;
    #define EMIT_BE32(v)  do { \
        uint32_t be = htobe32_((v)); \
        memcpy(body + off, &be, 4); off += 4; \
    } while (0)
    #define EMIT_U8(v)    do { body[off++] = (uint8_t)(v); } while (0)
    #define EMIT_LE16(v)  do { \
        uint16_t le = (uint16_t)(v); \
        body[off++] = (uint8_t)(le & 0xff); \
        body[off++] = (uint8_t)(le >> 8); \
    } while (0)

    EMIT_BE32(1);  EMIT_U8(state);
    EMIT_BE32(1);  EMIT_U8(app_index);
    EMIT_BE32(2);  EMIT_LE16(batch_size);
    EMIT_BE32(2);  EMIT_LE16(batch_count);

    #undef EMIT_BE32
    #undef EMIT_U8
    #undef EMIT_LE16

    uint8_t  resp[128];
    uint32_t resp_len = sizeof(resp);
    printf("  CHANGE_STATUS(%s, state=%u app=0x%02x bs=%u bc=%u) → ",
           label, state, app_index, batch_size, batch_count);
    fflush(stdout);
    int rc = send_fw_control(fd, OPCODE_CS_CHANGE_STATUS,
                             /*parameter_count=*/4u,
                             body, (uint32_t)off,
                             resp, &resp_len, /*core_cpu=*/true);
    if (rc != 0) {
        printf("ioctl rc=%d (%s)\n", rc, strerror(-rc));
        return rc;
    }
    /* Response layout (fw side, post-common-header):
     *   [0..3]  major_status (LE u32) — 0 = fw accepted
     *   [4..7]  minor_status (LE u32)
     *   [8..11] parameter_count (BE u32)
     *   [12..] parameter values (empty for CHANGE_STATUS)
     * Non-zero major_status = fw rejected even if the ioctl succeeded. */
    print_resp_status("    ", resp, resp_len);
    return 0;
}

/* CONTEXT_SWITCH_CLEAR_CONFIGURED_APPS (opcode 0x47, CORE CPU) and
 * GET_HW_CONSTS (opcode 0x48, CORE CPU) are both empty-body RPCs.
 * Wire: [common_hdr][parameter_count=0]. fw always returns a small
 * response. */
static int cmd_empty_rpc(int fd, uint32_t opcode, const char *name)
{
    uint8_t  resp[256];
    uint32_t resp_len = sizeof(resp);
    printf("  %s(opcode 0x%02x) → ", name, opcode);
    fflush(stdout);
    int rc = send_fw_control(fd, opcode,
                             /*parameter_count=*/0u,
                             NULL, 0, resp, &resp_len, /*core_cpu=*/true);
    if (rc != 0) {
        printf("ioctl rc=%d (%s)\n", rc, strerror(-rc));
        return rc;
    }
    print_resp_status("    ", resp, resp_len);
    return 0;
}

/*
 * --cs-handshake: fire SLM-OS's pre-context-info CS RPCs against a
 * HailoRT-booted Hailo-8L and report each response. Does NOT replay
 * SET_CONTEXT_INFO (that needs IOVA-patched action bytes — future
 * work); this just tests the wire-format transport for the empty-
 * body and 4-parameter opcodes.
 *
 * If fw accepts this sequence with rc=0 all the way through, the
 * SLM-OS control-channel wire format is validated against the
 * official driver path. Any rejection at this layer would point
 * at a SLM-OS wire-format bug the ctxsmoke self-test missed.
 */
static int cmd_cs_handshake(int fd)
{
    printf("=== cs-handshake: SLM-OS pre-context-info CS RPCs via hailo_pci ===\n");

    /* Step 1: CHANGE_CONTEXT_SWITCH_STATUS(RESET). SLM-OS uses
     * app=0xff here (meaning "no specific app") because no
     * network group is loaded yet. */
    int rc = cmd_cs_change_status(fd, CS_STATE_RESET, 0xff, 0, 0, "RESET");
    if (rc != 0) return rc;

    /* Step 2: CONTEXT_SWITCH_CLEAR_CONFIGURED_APPS. Expected rc=0;
     * clears any leftover state from a previous run (HailoRT's boot
     * may have left something configured). */
    rc = cmd_empty_rpc(fd, OPCODE_CS_CLEAR_CONFIGURED_APPS,
                       "CLEAR_CONFIGURED_APPS");
    if (rc != 0) return rc;

    /* Step 3: GET_HW_CONSTS. Returns hw-specific constants; we
     * don't consume the response body but the rc tells us fw is
     * still happy. */
    rc = cmd_empty_rpc(fd, OPCODE_GET_HW_CONSTS, "GET_HW_CONSTS");
    if (rc != 0) return rc;

    printf("=== cs-handshake pre-context-info phase complete ===\n");
    printf("(SET_NETWORK_GROUP_HEADER / SET_CONTEXT_INFO not yet "
           "replayed — those need IOVA patching, next session)\n");
    return 0;
}

/*
 * --submit-probe: replay SLM-OS's boundary-input VDMA setup through
 * the official hailo_pci ioctl path and see whether the driver
 * accepts the parameters.
 *
 * What it tests: SLM-OS's descriptor geometry (desc_count=64,
 * desc_page_size=512, non-circular) matched to boundary channel 2.
 *
 * What it does NOT test (yet): the firmware configuration that
 * would make num_proc actually advance. That requires the full
 * CS-handshake replay (RESET → ACTIVATION → ... → ENABLED) which
 * is planned as a follow-up.
 *
 * A successful probe means: ioctl surface accepts SLM-OS's layout,
 * the bug is deeper than kernel-side parameter validation. A
 * rejected ioctl means: we've localized the bug to the parameter
 * the driver complained about.
 *
 * Default geometry mirrors hailo_backend_run on MNIST:
 *   - buffer size: 784 bytes (MNIST 28×28×1 input)
 *   - desc_count:  64 (HAILO_CS_DEFAULT_BOUNDARY_DESC_COUNT)
 *   - page_size:   512 (HAILO_CS_DEFAULT_BOUNDARY_PAGE_SIZE)
 *   - channel:     2  (boundary input, per ACTIVATION)
 */
#define PROBE_BUFFER_BYTES    784u
#define PROBE_DESC_COUNT      64u
#define PROBE_DESC_PAGE_SIZE  512u
#define PROBE_CHANNEL_INDEX   2u
#define PROBE_WAIT_TIMEOUT_MS 2000u

static int cmd_submit_probe(int fd)
{
    int rc = 0;
    void *user_buf = NULL;
    uintptr_t mapped_handle = 0;
    uintptr_t desc_handle = 0;
    uint64_t  list_iova = 0;
    bool channel_enabled = false;
    /* Track allocation explicitly rather than checking `handle != 0`.
     * The Hailo kernel driver's handles are slab-pointer-backed and
     * never zero in practice, but a defensive flag keeps the cleanup
     * guard unambiguous. */
    bool mapped_handle_set = false;
    bool desc_handle_set   = false;

    printf("=== submit-probe: SLM-OS boundary-input layout via hailo_pci ioctls ===\n");
    printf("channel=%u desc_count=%u page_size=%u buffer_bytes=%u\n",
           PROBE_CHANNEL_INDEX, PROBE_DESC_COUNT,
           PROBE_DESC_PAGE_SIZE, PROBE_BUFFER_BYTES);

    /* 1. Allocate a page-aligned userspace buffer. hailo_pci's
     *    BUFFER_MAP wants the kernel to pin the range; non-page-
     *    aligned starts are accepted but cleaner to force. */
    size_t buf_size = PROBE_BUFFER_BYTES;
    /* Round up to page size for mmap. */
    long page_sz = sysconf(_SC_PAGESIZE);
    if (page_sz <= 0) page_sz = 4096;
    size_t map_size = (buf_size + (size_t)page_sz - 1)
                      & ~((size_t)page_sz - 1);
    user_buf = mmap(NULL, map_size, PROT_READ | PROT_WRITE,
                    MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (user_buf == MAP_FAILED) {
        fprintf(stderr, "[1] mmap failed: %s\n", strerror(errno));
        return -errno;
    }
    /* Fill with an identifiable pattern so we can tell on the
     * firmware side whether it actually DMAed our bytes. */
    memset(user_buf, 0xA5, buf_size);
    printf("[1] user buffer at %p (mapped %zu B)\n", user_buf, map_size);

    /* 2. VDMA_BUFFER_MAP — kernel pins the pages and programs an
     *    IOMMU (or, on Pi 5, inbound-window) mapping. */
    rc = hailo_dev_buffer_map(fd, user_buf, buf_size,
                              HAILO_DEV_DIR_H2D, &mapped_handle);
    if (rc < 0) {
        fprintf(stderr, "[2] HAILO_VDMA_BUFFER_MAP failed: %s\n",
                strerror(-rc));
        goto cleanup;
    }
    mapped_handle_set = true;
    printf("[2] mapped_handle=0x%lx\n", (unsigned long)mapped_handle);

    /* 3. DESC_LIST_CREATE — driver picks a 64 KB-aligned backing
     *    region and returns the Hailo IOVA. */
    rc = hailo_dev_desc_list_create(fd, PROBE_DESC_COUNT,
                                    (uint16_t)PROBE_DESC_PAGE_SIZE,
                                    /*is_circular=*/false,
                                    &desc_handle, &list_iova);
    if (rc < 0) {
        fprintf(stderr, "[3] HAILO_DESC_LIST_CREATE failed: %s\n",
                strerror(-rc));
        goto cleanup;
    }
    desc_handle_set = true;
    printf("[3] desc_handle=0x%lx list_iova=0x%lx\n",
           (unsigned long)desc_handle, (unsigned long)list_iova);
    if ((list_iova & 0xFFFF) != 0) {
        fprintf(stderr, "    WARNING: list_iova not 64 KB-aligned — "
                "fw rejects HOST_DESCRIPTOR_BASE_ADDRESS "
                "misalignment\n");
    }

    /* 4. DESC_LIST_PROGRAM — bind the buffer to the descriptors on
     *    channel_index. should_bind=true tells the driver to both
     *    program the list contents AND register the buffer-to-list
     *    binding so LAUNCH_TRANSFER can skip re-binding. */
    rc = hailo_dev_desc_list_program(fd, desc_handle, mapped_handle,
                                     /*buffer_offset=*/0,
                                     /*buffer_size=*/buf_size,
                                     PROBE_CHANNEL_INDEX,
                                     /*starting_desc=*/0,
                                     /*should_bind=*/true);
    if (rc < 0) {
        fprintf(stderr, "[4] HAILO_DESC_LIST_PROGRAM failed: %s\n",
                strerror(-rc));
        goto cleanup;
    }
    printf("[4] desc list programmed (channel %u, %u B)\n",
           PROBE_CHANNEL_INDEX, (unsigned)buf_size);

    /* 5. VDMA_ENABLE_CHANNELS — driver arms per-channel regs. Until
     *    this returns, LAUNCH_TRANSFER would be rejected. */
    rc = hailo_dev_enable_channel(fd, PROBE_CHANNEL_INDEX,
                                  /*enable_timestamps=*/false);
    if (rc < 0) {
        fprintf(stderr, "[5] HAILO_VDMA_ENABLE_CHANNELS failed: %s\n",
                strerror(-rc));
        goto cleanup;
    }
    channel_enabled = true;
    printf("[5] channel %u enabled\n", PROBE_CHANNEL_INDEX);

    /* 6. VDMA_LAUNCH_TRANSFER — this is the "kick": driver programs
     *    num_avail on the channel's register, firmware is expected
     *    to fetch the descriptors. */
    rc = hailo_dev_launch_transfer(fd, PROBE_CHANNEL_INDEX,
                                   desc_handle, /*starting_desc=*/0,
                                   user_buf,
                                   (uint32_t)buf_size);
    if (rc < 0) {
        fprintf(stderr, "[6] HAILO_VDMA_LAUNCH_TRANSFER failed: %s\n",
                strerror(-rc));
        goto cleanup;
    }
    printf("[6] transfer launched\n");

    /* 7. VDMA_INTERRUPTS_WAIT — give fw up to PROBE_WAIT_TIMEOUT_MS
     *    to advance num_proc. No fw configuration = timeout is
     *    expected. The pass/fail we care about is whether steps
     *    1-6 all accepted our parameters. */
    uint8_t count = 0;
    struct hailo_vdma_interrupts_channel_data irqs[8];
    rc = hailo_dev_interrupts_wait(fd,
                                   (1u << PROBE_CHANNEL_INDEX),
                                   PROBE_WAIT_TIMEOUT_MS,
                                   &count, irqs,
                                   sizeof(irqs) / sizeof(irqs[0]));
    if (rc == -EINTR) {
        printf("[7] interrupts_wait timed out after %u ms — fw did "
               "not advance num_proc (expected without CS-handshake "
               "configuration)\n", PROBE_WAIT_TIMEOUT_MS);
        rc = 0;   /* not a probe failure — the kernel path worked */
    } else if (rc < 0) {
        fprintf(stderr, "[7] HAILO_VDMA_INTERRUPTS_WAIT failed: %s\n",
                strerror(-rc));
    } else {
        printf("[7] interrupts_wait reported %u completion(s):\n", count);
        for (uint8_t i = 0; i < count; i++) {
            const char *state =
                irqs[i].data == HAILO_VDMA_TRANSFER_DATA_CHANNEL_NOT_ACTIVE
                    ? "NOT_ACTIVE"
                    : irqs[i].data == HAILO_VDMA_TRANSFER_DATA_CHANNEL_WITH_ERROR
                          ? "ERROR"
                          : "progress";
            printf("    engine=%u channel=%u data=0x%02x (%s)\n",
                   irqs[i].engine_index, irqs[i].channel_index,
                   irqs[i].data, state);
        }
    }

    printf("=== probe complete: all ioctls accepted SLM-OS geometry ===\n");

cleanup:
    if (channel_enabled) {
        int drc = hailo_dev_disable_channel(fd, PROBE_CHANNEL_INDEX);
        if (drc < 0) fprintf(stderr, "cleanup: disable_channel: %s\n",
                             strerror(-drc));
    }
    if (desc_handle_set) {
        int drc = hailo_dev_desc_list_release(fd, desc_handle);
        if (drc < 0) fprintf(stderr, "cleanup: desc_list_release: %s\n",
                             strerror(-drc));
    }
    if (mapped_handle_set) {
        int drc = hailo_dev_buffer_unmap(fd, mapped_handle);
        if (drc < 0) fprintf(stderr, "cleanup: buffer_unmap: %s\n",
                             strerror(-drc));
    }
    if (user_buf && user_buf != MAP_FAILED) {
        munmap(user_buf, map_size);
    }
    return rc;
}

static int cmd_identify(int fd)
{
    uint8_t  resp[256];
    uint32_t resp_len = sizeof(resp);

    printf("sending IDENTIFY (opcode 0x%02x) to /dev/hailo0...\n",
           HAILO_CONTROL_OPCODE_IDENTIFY);
    int rc = send_fw_control(fd, HAILO_CONTROL_OPCODE_IDENTIFY,
                             /*parameter_count=*/0,
                             NULL, 0, resp, &resp_len,
                             /*core_cpu=*/false);
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
        "  --identify       Send FW_CONTROL IDENTIFY to /dev/hailo0\n"
        "  --submit-probe   Replay SLM-OS boundary-input VDMA layout\n"
        "                   through hailo_pci ioctls (diagnostic probe\n"
        "                   for #253; no HEF required)\n"
        "  --cs-handshake   Fire SLM-OS pre-context-info CS RPCs\n"
        "                   (RESET, CLEAR_CONFIGURED_APPS, GET_HW_CONSTS)\n"
        "                   through HAILO_FW_CONTROL; tests wire format\n"
        "                   against the official driver path\n",
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
    } else if (strcmp(argv[1], "--submit-probe") == 0) {
        rc = cmd_submit_probe(fd);
    } else if (strcmp(argv[1], "--cs-handshake") == 0) {
        rc = cmd_cs_handshake(fd);
    } else {
        usage(argv[0]);
    }

    close(fd);
    return rc == 0 ? 0 : 1;
}
