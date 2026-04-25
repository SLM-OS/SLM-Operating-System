/*
 * main.c — hailo-ushim entry + test driver.
 *
 * v1 scope: sanity-check /dev/hailo0 access via FW_CONTROL identify.
 * Subsequent revisions add VDMA buffer map + desc-list program +
 * launch_transfer so we can replay SLM-OS's boundary-submit sequence.
 */

#include <endian.h>
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
 *
 * EVP_Q_digest is the OpenSSL 3.0 one-shot helper; the legacy
 * MD5_Init/Update/Final API is deprecated since OpenSSL 3.0 and
 * emits warnings on Pi OS bookworm.
 */
#include <openssl/evp.h>

static void md5_compute(const void *data, size_t len, uint8_t out[16])
{
    size_t outlen = 16;
    EVP_Q_digest(NULL, "MD5", NULL, data, len, out, &outlen);
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
#include <sys/stat.h>
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
    /* Sequence number per request. Seeded from time(NULL) so each invocation
     * starts from a different base, matching HailoRT's non-zero seeding (fw
     * v4.23 does not reject low values today, but matching libhailort
     * keeps wire captures comparable). Single-threaded by construction. */
    static uint32_t seq;
    static bool seq_initialized;
    if (!seq_initialized) {
        seq = (uint32_t)time(NULL);
        seq_initialized = true;
    }
    hdr->version  = htobe32(HAILO_CONTROL_PROTOCOL_VERSION);
    hdr->flags    = 0;
    hdr->sequence = htobe32(++seq);
    hdr->opcode   = htobe32(opcode);

    uint32_t param_count_be = htobe32(parameter_count);
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
        uint32_t be = htobe32((v)); \
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

    /* Tripwire only — fires AFTER an EMIT-macro overflow has already
     * corrupted the stack at `body[22]`. Real overflow protection
     * comes from gcc's -fstack-protector (Linux default), which
     * panics on the corrupted canary at function exit. This check
     * gives a clean error code on first re-run after a regression. */
    if (off != sizeof(body)) {
        fprintf(stderr, "cmd_cs_change_status: body off=%zu != %zu\n",
                off, sizeof(body));
        return -EINVAL;
    }

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

/* ------------------------------------------------------------------------ */
/* Captured ctxsmoke action bodies (from SLM-OS HAILO_WIRE_DEBUG=ON, 2026-04-24) */
/* ------------------------------------------------------------------------ */
/* See host-tools/hailo-ushim/CAPTURED_CTXSMOKE_BYTES.md for the full        */
/* documentation. These are the exact bytes SLM-OS sends — fw accepts each   */
/* with rc=0. The IOVA fields are patched at runtime by patch_iova_le64()    */
/* to point at desc-list IOVAs allocated through hailo_pci ioctls.           */

/* SET_NETWORK_GROUP_HEADER body (32 bytes after the parameter_count + length
 * prefix, which send_fw_control_with_lenprefix builds for us). Pulled from
 * HailoRT MNIST trace (full bytes — the cap was on the ioctl trace, not the
 * RPC body itself). HailoRT-equivalent ctxsmoke produces an identical 32-B
 * body for the synthetic 1-network-group HEF SLM-OS uses. */
static const uint8_t NG_HEADER_BODY[32] = {
    0x01, 0x00, 0x01, 0x00, 0x01, 0x00, 0x01, 0x00,
    0x02, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x02, 0x00, 0x01,
};

static const uint8_t ACTIVATION_BODY[63] = {
    /*  0 */ 0x1e, 0xff, 0xff, 0xff, 0xff,        /* BURST_CREDITS_TASK_RESET */
    /*  5 */ 0x21, 0xff, 0xff, 0xff, 0xff,        /* OpenBoundaryOutput hdr */
    /* 10 */ 0x10,                                /* packed_vdma_channel_id */
    /* 11 */ 0x00,                                /* buffer_type */
    /* 12 */ 0x00, 0x00, 0xaf, 0x00, 0x10, 0x00, 0x00, 0x00, /* IOVA bnd_out (PATCH) */
    /* 20 */ 0x00, 0x10,                          /* desc_page_size = 4096 */
    /* 22 */ 0x40, 0x00, 0x00, 0x00,              /* total_desc_count = 64 */
    /* 26 */ 0x00, 0x01, 0x00, 0x00,              /* bytes_in_pattern = 256 */
    /* 30 */ 0x20, 0xff, 0xff, 0xff, 0xff,        /* OpenBoundaryInput hdr */
    /* 35 */ 0x02,                                /* packed_vdma_channel_id */
    /* 36 */ 0x00,                                /* buffer_type */
    /* 37 */ 0x00, 0x00, 0xae, 0x00, 0x10, 0x00, 0x00, 0x00, /* IOVA bnd_in (PATCH) */
    /* 45 */ 0x00, 0x10,                          /* desc_page_size = 4096 */
    /* 47 */ 0x40, 0x00, 0x00, 0x00,              /* total_desc_count = 64 */
    /* 51 */ 0x00, 0x01, 0x00, 0x00,              /* bytes_in_pattern = 256 */
    /* 55 */ 0x01,                                /* stream_index */
    /* 56 */ 0x00,                                /* network_index */
    /* 57 */ 0x00, 0x01,                          /* periph_bytes_per_buffer LE */
    /* 59 */ 0x00, 0x01, 0x00, 0x00,              /* frame_periph_size LE = 256 */
};
#define ACTIVATION_OFFSET_BND_OUT_IOVA  12
#define ACTIVATION_OFFSET_BND_IN_IOVA   37
_Static_assert(ACTIVATION_OFFSET_BND_OUT_IOVA + 8 <= sizeof(ACTIVATION_BODY),
               "ACTIVATION bnd_out IOVA patch would overflow body");
_Static_assert(ACTIVATION_OFFSET_BND_IN_IOVA  + 8 <= sizeof(ACTIVATION_BODY),
               "ACTIVATION bnd_in IOVA patch would overflow body");

static const uint8_t BATCH_SWITCHING_BODY[16] = {
    0x1f, 0xff, 0xff, 0xff, 0xff,
    0x25, 0xff, 0xff, 0xff, 0xff, 0x02,
    0x1d, 0xff, 0xff, 0xff, 0xff,
};

static const uint8_t PRELIMINARY_BODY[37] = {
    /*  0 */ 0x16, 0xff, 0xff, 0xff, 0xff,        /* ACTIVATE_CFG_CHANNEL hdr */
    /*  5 */ 0x01,                                /* packed_vdma_channel_id */
    /*  6 */ 0x00,                                /* config_stream_index */
    /*  7 */ 0x00,                                /* buffer_type */
    /*  8 */ 0x00, 0x00, 0xab, 0x00, 0x10, 0x00, 0x00, 0x00, /* IOVA ccw (PATCH) */
    /* 16 */ 0x00, 0x02,                          /* desc_page_size = 512 */
    /* 18 */ 0x02, 0x00, 0x00, 0x00,              /* total_desc_count = 2 */
    /* 22 */ 0x00, 0x00, 0x00, 0x00,              /* bytes_in_pattern */
    /* 26 */ 0x18, 0xff, 0xff, 0xff, 0xff,        /* REPEATED_ACTION? hdr */
    /* 31 */ 0x01, 0x00, 0x00, 0x02, 0x00, 0x01,
};
#define PRELIMINARY_OFFSET_CCW_IOVA     8
_Static_assert(PRELIMINARY_OFFSET_CCW_IOVA + 8 <= sizeof(PRELIMINARY_BODY),
               "PRELIMINARY CCW IOVA patch would overflow body");

static const uint8_t DYNAMIC_BODY[103] = {
    /*  0 */ 0x07, 0xff, 0xff, 0xff, 0xff,        /* ACTIVATE_BOUNDARY_OUTPUT hdr */
    /*  5 */ 0x10,                                /* packed_vdma_channel_id */
    /*  6 */ 0x02,                                /* stream_index */
    /*  7 */ 0x00,                                /* network_index */
    /*  8 */ 0x00, 0x01, 0x01, 0x00, 0x00, 0x01, 0x01, 0x00, /* stream_reg_info */
    /* 16 */ 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x01, /* stream_reg_info */
    /* 25 */ 0x00,                                /* host_buffer_info.buffer_type */
    /* 26 */ 0x00, 0x00, 0xaf, 0x00, 0x10, 0x00, 0x00, 0x00, /* IOVA bnd_out (PATCH) */
    /* 34 */ 0x00, 0x10,                          /* desc_page_size = 4096 */
    /* 36 */ 0x40, 0x00, 0x00, 0x00,              /* total_desc_count = 64 */
    /* 40 */ 0x00, 0x01, 0x00, 0x00,              /* bytes_in_pattern = 256 */
    /* 44 */ 0x06, 0xff, 0xff, 0xff, 0xff,        /* ACTIVATE_BOUNDARY_INPUT hdr */
    /* 49 */ 0x02,                                /* packed_vdma_channel_id */
    /* 50 */ 0x01,                                /* stream_index */
    /* 51 */ 0x00, 0x01, 0x01, 0x00, 0x00, 0x01, 0x01, 0x00, /* stream_reg_info */
    /* 59 */ 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, /* stream_reg_info */
    /* 68 */ 0x00,                                /* buffer_type */
    /* 69 */ 0x00, 0x00, 0xae, 0x00, 0x10, 0x00, 0x00, 0x00, /* IOVA bnd_in (PATCH) */
    /* 77 */ 0x00, 0x10,                          /* desc_page_size = 4096 */
    /* 79 */ 0x40, 0x00, 0x00, 0x00,              /* total_desc_count = 64 */
    /* 83 */ 0x00, 0x01, 0x00, 0x00,              /* bytes_in_pattern = 256 */
    /* 87 */ 0x00, 0x00, 0x01, 0x00,              /* initial_credit_size = 65536 */
    /* 91 */ 0x27, 0xff, 0xff, 0xff, 0xff,        /* action 0x27 hdr */
    /* 96 */ 0x02, 0x01,                          /* action 0x27 body */
    /* 98 */ 0x15, 0xff, 0xff, 0xff, 0xff,        /* action 0x15 hdr (no body) */
};
_Static_assert(sizeof(DYNAMIC_BODY) == 103, "DYNAMIC body must be 103 bytes");
#define DYNAMIC_OFFSET_BND_OUT_IOVA     26
#define DYNAMIC_OFFSET_BND_IN_IOVA      69
_Static_assert(DYNAMIC_OFFSET_BND_OUT_IOVA + 8 <= sizeof(DYNAMIC_BODY),
               "DYNAMIC bnd_out IOVA patch would overflow body");
_Static_assert(DYNAMIC_OFFSET_BND_IN_IOVA  + 8 <= sizeof(DYNAMIC_BODY),
               "DYNAMIC bnd_in IOVA patch would overflow body");

/* Patch a little-endian u64 IOVA into a byte buffer at the given
 * offset. Used to overwrite the captured ctxsmoke IOVAs with
 * hailo_pci-allocated desc-list IOVAs at runtime. */
static void patch_iova_le64(uint8_t *body, size_t offset, uint64_t iova)
{
    for (int i = 0; i < 8; i++) {
        body[offset + i] = (uint8_t)((iova >> (i * 8)) & 0xff);
    }
}

/*
 * SET_CONTEXT_INFO wire layout per SLM-OS hailo_control.c:1436-1447
 * (struct hailo_cs_set_ctx_info_req_prefix_wire). 4-parameter body
 * after the common header:
 *   [BE u32 length=1][u8 is_first_chunk_per_context]
 *   [BE u32 length=1][u8 is_last_chunk_per_context]
 *   [BE u32 length=1][u8 context_type]
 *   [BE u32 length=N][N raw context_network_data bytes]
 *
 * Note: there is NO application_index field — that's only in the
 * CHANGE_CONTEXT_SWITCH_STATUS RPC. Earlier guess was wrong; first
 * hardware run got UNEXPECTED_CONTEXT_ORDER from fw because the
 * context_type byte was being read from a wrong offset.
 */
/* Authoritative values from SLM-OS hailo_control.h:491-495. The
 * second-iteration probe run had DYNAMIC and BATCH_SWITCHING
 * swapped, which made fw see ACTIVATION → DYNAMIC → ... and reject
 * with UNEXPECTED_CONTEXT_ORDER. */
enum {
    CONTEXT_TYPE_PRELIMINARY     = 0,
    CONTEXT_TYPE_DYNAMIC         = 1,
    CONTEXT_TYPE_BATCH_SWITCHING = 2,
    CONTEXT_TYPE_ACTIVATION      = 3,
};

/* Maximum req_body bytes any RPC may pass to send_fw_control:
 * the wire ceiling in `struct hailo_fw_control.buffer`
 * (MAX_CONTROL_LENGTH = 1500) minus the 16-byte common_hdr +
 * 4-byte parameter_count that send_fw_control prepends. */
#define MAX_FW_CONTROL_BODY  (MAX_CONTROL_LENGTH - 16 - 4)

static int cmd_set_context_info(int fd, uint8_t context_type,
                                const uint8_t *body, uint32_t body_len,
                                const char *label)
{
    /* Sized so over-sized requests fail at this call site rather than
     * slipping through to send_fw_control's identical bounds check. */
    uint8_t buf[MAX_FW_CONTROL_BODY];
    size_t  off = 0;
    #define EMIT_BE32(v)  do { uint32_t be = htobe32((v)); \
        memcpy(buf + off, &be, 4); off += 4; } while (0)
    #define EMIT_U8(v)    do { buf[off++] = (uint8_t)(v); } while (0)

    EMIT_BE32(1);  EMIT_U8(1);                /* is_first_chunk_per_context */
    EMIT_BE32(1);  EMIT_U8(1);                /* is_last_chunk_per_context */
    EMIT_BE32(1);  EMIT_U8(context_type);
    EMIT_BE32(body_len);                       /* body length */
    if (off + body_len > sizeof(buf)) return -EINVAL;
    memcpy(buf + off, body, body_len);
    off += body_len;

    #undef EMIT_BE32
    #undef EMIT_U8

    uint8_t  resp[256];
    uint32_t resp_len = sizeof(resp);
    printf("  SET_CONTEXT_INFO(%s, type=%u, %u B body) → ",
           label, context_type, body_len);
    fflush(stdout);
    int rc = send_fw_control(fd, OPCODE_CS_SET_CONTEXT_INFO,
                             /*parameter_count=*/4u,
                             buf, (uint32_t)off,
                             resp, &resp_len, /*core_cpu=*/true);
    if (rc != 0) {
        printf("ioctl rc=%d (%s)\n", rc, strerror(-rc));
        return rc;
    }
    print_resp_status("    ", resp, resp_len);
    return 0;
}

/*
 * SET_NETWORK_GROUP_HEADER (opcode 0x20). Wire body:
 *   parameter_count = 1
 *   [BE u32 length=N][N raw header bytes]
 */
static int cmd_set_network_group_header(int fd,
                                        const uint8_t *body,
                                        uint32_t body_len)
{
    uint8_t buf[256];
    size_t  off = 0;
    uint32_t be_len = htobe32(body_len);
    memcpy(buf + off, &be_len, 4); off += 4;
    if (off + body_len > sizeof(buf)) return -EINVAL;
    memcpy(buf + off, body, body_len);
    off += body_len;

    uint8_t  resp[128];
    uint32_t resp_len = sizeof(resp);
    printf("  SET_NETWORK_GROUP_HEADER(%u B) → ", body_len);
    fflush(stdout);
    int rc = send_fw_control(fd, OPCODE_CS_SET_NETWORK_GROUP_HEADER,
                             /*parameter_count=*/1u,
                             buf, (uint32_t)off,
                             resp, &resp_len, /*core_cpu=*/true);
    if (rc != 0) {
        printf("ioctl rc=%d (%s)\n", rc, strerror(-rc));
        return rc;
    }
    print_resp_status("    ", resp, resp_len);
    return 0;
}

/*
 * --full-handshake: end-to-end replay of SLM-OS's CS handshake +
 * boundary submit through hailo_pci. Allocates 3 desc lists + buffers,
 * patches the captured ctxsmoke action bodies with the IOVAs the
 * driver returned, fires the full RPC sequence, then launches a
 * boundary-input transfer. If LAUNCH_TRANSFER advances num_proc,
 * SLM-OS's bytes are correct end-to-end and #253 lives in the
 * bare-metal bringup. If it fails, we've localized to a specific
 * RPC the driver path also rejects.
 */
static int cmd_full_handshake(int fd)
{
    int rc = 0;
    void *ccw_buf = NULL, *bnd_in_buf = NULL, *bnd_out_buf = NULL;
    uintptr_t ccw_mh = 0, bnd_in_mh = 0, bnd_out_mh = 0;
    uintptr_t ccw_dh = 0, bnd_in_dh = 0, bnd_out_dh = 0;
    uint64_t  ccw_iova = 0, bnd_in_iova = 0, bnd_out_iova = 0;
    bool ccw_mh_set = false, bnd_in_mh_set = false, bnd_out_mh_set = false;
    bool ccw_dh_set = false, bnd_in_dh_set = false, bnd_out_dh_set = false;
    bool ch_in_enabled = false, ch_ccw_enabled = false, ch_out_enabled = false;

    long page_sz = sysconf(_SC_PAGESIZE);
    if (page_sz <= 0) page_sz = 4096;
    size_t map_size = (size_t)page_sz;

    printf("=== full-handshake: SLM-OS CS RPC chain + LAUNCH_TRANSFER ===\n");

    /* 1. Allocate 3 buffers (CCW, bnd_in, bnd_out) + map them. */
    #define MMAP_BUF(var) do { \
        var = mmap(NULL, map_size, PROT_READ | PROT_WRITE, \
                   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0); \
        if (var == MAP_FAILED) { fprintf(stderr, "mmap " #var \
            " failed: %s\n", strerror(errno)); rc = -errno; goto cleanup; } \
        memset(var, 0xA5, map_size); \
    } while (0)
    MMAP_BUF(ccw_buf);
    MMAP_BUF(bnd_in_buf);
    MMAP_BUF(bnd_out_buf);
    #undef MMAP_BUF

    /* Fill the CCW buffer with real MNIST CCW bytes from mnist.hef
     * if the file is present alongside the probe. This closes the
     * last apples-to-apples gap with SLM-OS's hailo_backend_run —
     * with real microcode, fw configures a valid inference graph
     * and subsequent boundary traffic actually processes. Without
     * the HEF, falls back to 0xA5 filler (previous behavior).
     *
     * HEF V2 layout (confirmed from mnist.hef): common_header (12 B)
     * + V2 trailer (20 B) = 32-byte header. Then proto (BE u32
     * proto_size at bytes 8-11 of header). Then CCWS region runs
     * to end-of-file. Match ctxsmoke's sizing — copy only the first
     * 256 bytes so fw doesn't reject due to length mismatch vs
     * PRELIMINARY's total_desc_count=2 × desc_page_size=512. */
    const char *hef_paths[] = {
        "mnist.hef",
        "/home/pi/hailo-ushim/mnist.hef",
        "/opt/hailort/models/mnist.hef",
        NULL,
    };
    int hef_fd = -1;
    for (int i = 0; hef_paths[i]; i++) {
        hef_fd = open(hef_paths[i], O_RDONLY | O_CLOEXEC);
        if (hef_fd >= 0) {
            printf("[1.5] using MNIST HEF: %s\n", hef_paths[i]);
            break;
        }
    }
    if (hef_fd >= 0) {
        struct stat st;
        if (fstat(hef_fd, &st) == 0 && st.st_size > 32) {
            void *hef = mmap(NULL, st.st_size, PROT_READ, MAP_PRIVATE,
                             hef_fd, 0);
            if (hef != MAP_FAILED) {
                const uint8_t *p = hef;
                uint32_t magic = ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16)
                               | ((uint32_t)p[2] <<  8) |  (uint32_t)p[3];
                if (magic == 0x01484546u) {
                    uint32_t proto_size =
                        ((uint32_t)p[8] << 24) | ((uint32_t)p[9] << 16)
                      | ((uint32_t)p[10] <<  8) |  (uint32_t)p[11];
                    /* V2: 12-byte common + 20-byte trailer */
                    size_t ccws_off = 32 + proto_size;
                    /* Guard against a malformed HEF whose declared
                     * proto_size pushes ccws_off past EOF — without
                     * this, the size_t subtraction below wraps and
                     * memcpy reads OOB from the mmap. */
                    if (ccws_off >= (size_t)st.st_size) {
                        printf("[1.5] HEF proto_size=%u pushes ccws past "
                               "EOF (size=%lld); using filler\n",
                               proto_size, (long long)st.st_size);
                    } else {
                        /* Fill the full 4 KB page with real CCW
                         * microcode. ctxsmoke's PRELIMINARY declares
                         * desc_count=2 × page_size=512 = 1024 B of
                         * CCW; our 4 KB buffer comfortably holds that
                         * plus headroom if we decide to upload more
                         * descriptors later. */
                        size_t avail = (size_t)st.st_size - ccws_off;
                        size_t ccw_copy = avail < 4096 ? avail : 4096;
                        memcpy(ccw_buf, p + ccws_off, ccw_copy);
                        printf("[1.5] copied %zu B real MNIST CCW "
                               "(from HEF offset 0x%lx)\n", ccw_copy,
                               (unsigned long)ccws_off);
                    }
                } else {
                    printf("[1.5] HEF magic mismatch, using filler\n");
                }
                munmap(hef, st.st_size);
            }
        }
        close(hef_fd);
    } else {
        printf("[1.5] no mnist.hef found, using 0xA5 filler "
               "(last apples-to-apples gap remains open)\n");
    }

    rc = hailo_dev_buffer_map(fd, ccw_buf, map_size,
                              HAILO_DEV_DIR_H2D, &ccw_mh);
    if (rc < 0) { fprintf(stderr, "[1] BUFFER_MAP ccw: %s\n",
        strerror(-rc)); goto cleanup; }
    ccw_mh_set = true;
    rc = hailo_dev_buffer_map(fd, bnd_in_buf, map_size,
                              HAILO_DEV_DIR_H2D, &bnd_in_mh);
    if (rc < 0) { fprintf(stderr, "[1] BUFFER_MAP bnd_in: %s\n",
        strerror(-rc)); goto cleanup; }
    bnd_in_mh_set = true;
    rc = hailo_dev_buffer_map(fd, bnd_out_buf, map_size,
                              HAILO_DEV_DIR_D2H, &bnd_out_mh);
    if (rc < 0) { fprintf(stderr, "[1] BUFFER_MAP bnd_out: %s\n",
        strerror(-rc)); goto cleanup; }
    bnd_out_mh_set = true;

    /* 2. Create 3 desc lists matching SLM-OS's ctxsmoke geometry. */
    rc = hailo_dev_desc_list_create(fd, /*count=*/2, /*page=*/512, false,
                                    &ccw_dh, &ccw_iova);
    if (rc < 0) { fprintf(stderr, "[2] DESC_LIST_CREATE ccw: %s\n",
        strerror(-rc)); goto cleanup; }
    ccw_dh_set = true;
    rc = hailo_dev_desc_list_create(fd, /*count=*/64, /*page=*/4096, false,
                                    &bnd_in_dh, &bnd_in_iova);
    if (rc < 0) { fprintf(stderr, "[2] DESC_LIST_CREATE bnd_in: %s\n",
        strerror(-rc)); goto cleanup; }
    bnd_in_dh_set = true;
    rc = hailo_dev_desc_list_create(fd, /*count=*/64, /*page=*/4096, false,
                                    &bnd_out_dh, &bnd_out_iova);
    if (rc < 0) { fprintf(stderr, "[2] DESC_LIST_CREATE bnd_out: %s\n",
        strerror(-rc)); goto cleanup; }
    bnd_out_dh_set = true;
    printf("[1-2] allocated: ccw_iova=0x%lx bnd_in_iova=0x%lx bnd_out_iova=0x%lx\n",
           (unsigned long)ccw_iova, (unsigned long)bnd_in_iova,
           (unsigned long)bnd_out_iova);

    /* 3. Program desc lists (binds buffer to channel). */
    rc = hailo_dev_desc_list_program(fd, ccw_dh, ccw_mh, 0, 256,
                                     /*ch=*/1, 0, true);
    if (rc < 0) { fprintf(stderr, "[3] DESC_LIST_PROGRAM ccw: %s\n",
        strerror(-rc)); goto cleanup; }
    rc = hailo_dev_desc_list_program(fd, bnd_in_dh, bnd_in_mh, 0, 784,
                                     /*ch=*/2, 0, true);
    if (rc < 0) { fprintf(stderr, "[3] DESC_LIST_PROGRAM bnd_in: %s\n",
        strerror(-rc)); goto cleanup; }
    rc = hailo_dev_desc_list_program(fd, bnd_out_dh, bnd_out_mh, 0, 16,
                                     /*ch=*/16, 0, true);
    if (rc < 0) { fprintf(stderr, "[3] DESC_LIST_PROGRAM bnd_out: %s\n",
        strerror(-rc)); goto cleanup; }

    /* 4. Enable boundary input channel (ch=2) for the launch_transfer
     * later. CCW + bnd_out are managed by fw via the CS actions. */
    rc = hailo_dev_enable_channel(fd, 2, false);
    if (rc < 0) { fprintf(stderr, "[4] ENABLE_CHANNELS: %s\n",
        strerror(-rc)); goto cleanup; }
    ch_in_enabled = true;

    /* 5. Patch IOVAs into per-context body copies. */
    uint8_t activation[63], preliminary[37], dynamic[103];
    memcpy(activation,  ACTIVATION_BODY,  sizeof(activation));
    memcpy(preliminary, PRELIMINARY_BODY, sizeof(preliminary));
    memcpy(dynamic,     DYNAMIC_BODY,     sizeof(dynamic));
    patch_iova_le64(activation,  ACTIVATION_OFFSET_BND_OUT_IOVA, bnd_out_iova);
    patch_iova_le64(activation,  ACTIVATION_OFFSET_BND_IN_IOVA,  bnd_in_iova);
    patch_iova_le64(preliminary, PRELIMINARY_OFFSET_CCW_IOVA,    ccw_iova);
    patch_iova_le64(dynamic,     DYNAMIC_OFFSET_BND_OUT_IOVA,    bnd_out_iova);
    patch_iova_le64(dynamic,     DYNAMIC_OFFSET_BND_IN_IOVA,     bnd_in_iova);
    printf("[5] patched IOVAs into context bodies\n");

    /* 6. Fire the full handshake. */
    printf("[6] firing full CS handshake...\n");
    rc = cmd_cs_change_status(fd, CS_STATE_RESET, 0xff, 0, 0, "RESET");
    if (rc != 0) goto cleanup;
    rc = cmd_empty_rpc(fd, OPCODE_CS_CLEAR_CONFIGURED_APPS,
                       "CLEAR_CONFIGURED_APPS");
    if (rc != 0) goto cleanup;
    rc = cmd_empty_rpc(fd, OPCODE_GET_HW_CONSTS, "GET_HW_CONSTS");
    if (rc != 0) goto cleanup;
    rc = cmd_set_network_group_header(fd, NG_HEADER_BODY,
                                      sizeof(NG_HEADER_BODY));
    if (rc != 0) goto cleanup;
    rc = cmd_set_context_info(fd, CONTEXT_TYPE_ACTIVATION,
                              activation, sizeof(activation),
                              "ACTIVATION");
    if (rc != 0) goto cleanup;
    rc = cmd_set_context_info(fd, CONTEXT_TYPE_BATCH_SWITCHING,
                              BATCH_SWITCHING_BODY,
                              sizeof(BATCH_SWITCHING_BODY),
                              "BATCH_SWITCHING");
    if (rc != 0) goto cleanup;
    rc = cmd_set_context_info(fd, CONTEXT_TYPE_PRELIMINARY,
                              preliminary, sizeof(preliminary),
                              "PRELIMINARY");
    if (rc != 0) goto cleanup;
    rc = cmd_set_context_info(fd, CONTEXT_TYPE_DYNAMIC,
                              dynamic, sizeof(dynamic),
                              "DYNAMIC");
    if (rc != 0) goto cleanup;
    rc = cmd_cs_change_status(fd, CS_STATE_ENABLED, 0, 0, 0, "ENABLED");
    if (rc != 0) goto cleanup;
    printf("[6] CS handshake complete — channel 2 should be configured\n");

    /* Brief sleep: fw sets each VDMA channel's CONTROL byte to START
     * asynchronously after CHANGE_STATUS(ENABLED). LAUNCH_TRANSFER
     * returns ECONNRESET while CONTROL is still 0. SLM-OS polls via
     * hailo_vdma_channel_wait_armed(); userspace probe takes the
     * cheap route and just sleeps 200 ms (plenty for Hailo-8L). */
    usleep(200 * 1000);
    printf("[6.5] waited 200 ms for fw to arm VDMA channels\n");

    /* 7a. Enable CCW channel 1 + output channel 16 (ch=2 already
     * enabled in step 4). SLM-OS's production hailo_backend_run
     * flow writes num_avail on ch=1 to upload CCW weights, then
     * pre-arms ch=16 for output, then LAUNCH_TRANSFERs ch=2. */
    rc = hailo_dev_enable_channel(fd, 1, false);
    if (rc < 0) { fprintf(stderr, "[7a] ENABLE ch=1: %s\n",
        strerror(-rc)); goto cleanup; }
    ch_ccw_enabled = true;
    rc = hailo_dev_enable_channel(fd, 16, false);
    if (rc < 0) { fprintf(stderr, "[7a] ENABLE ch=16: %s\n",
        strerror(-rc)); goto cleanup; }
    ch_out_enabled = true;
    printf("[7a] enabled channels 1 (CCW) + 16 (bnd_out)\n");

    /* 7b. Kick CCW upload on channel 1. Buffer content is garbage
     * bytes (ctxsmoke doesn't carry real weights); the point is to
     * give fw something to DMA-pull so the channel 1 state machine
     * advances. For a real #253 apples-to-apples reproduction,
     * this would want actual MNIST CCW bytes from the HEF. */
    /* 256 B upload fits 1 × 512 B descriptor. Empirically: hardware
     * run with real MNIST CCW bytes at 256 B → fw processes, num_proc
     * advances to 1 within 1 s. At 1024 B (2 descriptors) the wait
     * timed out — fw may need a longer settle OR may reject multi-
     * descriptor upload when the CS handshake's PRELIMINARY didn't
     * declare enough buffering. Sticking with 256 B for the known-
     * working case; tune later if needed. */
    printf("[7b] LAUNCH_TRANSFER ch=1 (CCW upload, 256 B real MNIST)...\n");
    rc = hailo_dev_launch_transfer(fd, 1, ccw_dh, 0, ccw_buf, 256);
    if (rc < 0) {
        fprintf(stderr, "[7b] ch=1 LAUNCH_TRANSFER: %s\n", strerror(-rc));
        goto cleanup;
    }
    /* Wait briefly for CCW to process. If fw is healthy, this
     * advances num_proc on ch=1 within tens of ms. */
    uint8_t ccw_count = 0;
    struct hailo_vdma_interrupts_channel_data ccw_irqs[8];
    rc = hailo_dev_interrupts_wait(fd, (1u << 1), 1000, &ccw_count,
                                   ccw_irqs,
                                   sizeof(ccw_irqs)/sizeof(ccw_irqs[0]));
    if (rc == -EINTR) {
        printf("[7b] CCW wait timed out (num_proc on ch=1 didn't advance)\n");
    } else if (rc == 0) {
        printf("[7b] CCW completed: %u event(s)", ccw_count);
        for (uint8_t i = 0; i < ccw_count; i++) {
            printf(", ch=%u data=0x%02x", ccw_irqs[i].channel_index,
                   ccw_irqs[i].data);
        }
        printf("\n");
    }

    /* 7c. Pre-arm output channel 16 with a LAUNCH_TRANSFER. HailoRT's
     * order per the VDMA trace is: pre-arm output BEFORE input. */
    printf("[7c] LAUNCH_TRANSFER ch=16 (bnd_out pre-arm)...\n");
    rc = hailo_dev_launch_transfer(fd, 16, bnd_out_dh, 0, bnd_out_buf, 16);
    if (rc < 0) {
        fprintf(stderr, "[7c] ch=16 LAUNCH_TRANSFER: %s\n", strerror(-rc));
        goto cleanup;
    }

    /* 8. LAUNCH_TRANSFER on channel 2. */
    printf("[8] LAUNCH_TRANSFER on channel 2 (boundary input)...\n");
    rc = hailo_dev_launch_transfer(fd, 2, bnd_in_dh, 0, bnd_in_buf, 784);
    if (rc < 0) {
        fprintf(stderr, "[8] LAUNCH_TRANSFER: %s\n", strerror(-rc));
        goto cleanup;
    }
    printf("[8] launched\n");

    /* 9. Wait for completion on ch=2 (and ch=16 while we're at it). */
    uint8_t count = 0;
    struct hailo_vdma_interrupts_channel_data irqs[8];
    rc = hailo_dev_interrupts_wait(fd, (1u << 2) | (1u << 16),
                                   5000, &count, irqs,
                                   sizeof(irqs)/sizeof(irqs[0]));
    if (rc == -EINTR) {
        printf("[9] timeout — fw did NOT advance num_proc on ch=2 "
               "(this is the #253 reproduction!)\n");
        rc = 0;
    } else if (rc < 0) {
        fprintf(stderr, "[9] INTERRUPTS_WAIT: %s\n", strerror(-rc));
    } else {
        printf("[9] %u completion(s):\n", count);
        for (uint8_t i = 0; i < count; i++) {
            printf("    engine=%u channel=%u data=0x%02x\n",
                   irqs[i].engine_index, irqs[i].channel_index,
                   irqs[i].data);
        }
        printf("    *** transfer completed — channel is alive ***\n");
    }

cleanup:
    /* Disable channels we actually enabled. Calling disable on a
     * never-enabled channel logs a harmless -EINVAL via dmesg, which
     * is noise we'd rather not produce on early-failure paths. */
    if (ch_in_enabled)  hailo_dev_disable_channel(fd, 2);
    if (ch_ccw_enabled) hailo_dev_disable_channel(fd, 1);
    if (ch_out_enabled) hailo_dev_disable_channel(fd, 16);
    if (bnd_out_dh_set) hailo_dev_desc_list_release(fd, bnd_out_dh);
    if (bnd_in_dh_set)  hailo_dev_desc_list_release(fd, bnd_in_dh);
    if (ccw_dh_set)     hailo_dev_desc_list_release(fd, ccw_dh);
    if (bnd_out_mh_set) hailo_dev_buffer_unmap(fd, bnd_out_mh);
    if (bnd_in_mh_set)  hailo_dev_buffer_unmap(fd, bnd_in_mh);
    if (ccw_mh_set)     hailo_dev_buffer_unmap(fd, ccw_mh);
    if (ccw_buf     && ccw_buf     != MAP_FAILED) munmap(ccw_buf,     map_size);
    if (bnd_in_buf  && bnd_in_buf  != MAP_FAILED) munmap(bnd_in_buf,  map_size);
    if (bnd_out_buf && bnd_out_buf != MAP_FAILED) munmap(bnd_out_buf, map_size);
    return rc;
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
        "                   against the official driver path\n"
        "  --full-handshake Replay SLM-OS's full CS handshake (RESET ->\n"
        "                   ... -> ENABLED) plus LAUNCH_TRANSFER on\n"
        "                   channel 2. Patches captured ctxsmoke action\n"
        "                   bodies with hailo_pci-allocated IOVAs.\n"
        "                   The decisive #253 bisect: if num_proc\n"
        "                   advances, SLM-OS bytes are correct end-\n"
        "                   to-end; the bug is in bare-metal bringup.\n",
        prog);
}

int main(int argc, char **argv)
{
    if (argc < 2) { usage(argv[0]); return 2; }

    int fd = open(HAILO_DEV_PATH, O_RDWR | O_CLOEXEC);
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
    } else if (strcmp(argv[1], "--full-handshake") == 0) {
        rc = cmd_full_handshake(fd);
    } else {
        usage(argv[0]);
    }

    close(fd);
    return rc == 0 ? 0 : 1;
}
