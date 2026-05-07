/*
 * hailo_shell.c — `hailo` shell command.
 *
 * Usage:
 *   hailo                  — dump driver state, IDs, BAR map.
 *   hailo probe            — re-run hailo_probe and print the result.
 *   hailo boot             — upload embedded firmware and bring NPU to RUNNING.
 *   hailo load P [upload B]— read a `.hef` at VFS path P, dump header,
 *                            pad shapes, and CCW summary. If `upload B`
 *                            is given, also issue the CCW upload via
 *                            WRITE_MEMORY against device base addr B.
 *   hailo load P sched     — load `.hef` into the "hailo-8" inference
 *                            device and arm ai_policy_hailo with the
 *                            resulting handle. (Mutually exclusive
 *                            with `upload B` for now — extend if both
 *                            are ever needed together.)
 *   hailo fw               — report firmware version (post-boot only).
 *   hailo peek A [N]       — READ_MEMORY N bytes (default 16, max 64) at
 *                            device-side address A; hex-dump.
 *   hailo poke A V         — WRITE_MEMORY a single 32-bit value V at
 *                            device-side address A (little-endian).
 *   hailo cfgstream D C    — CONFIG_STREAM probe (D in {in, out};
 *                            C = pcie channel hex u4); prints
 *                            assigned dataflow_manager_id.
 *   hailo infer B          — run an end-to-end VDMA inference smoke test
 *                            with a synthetic B-byte tensor.
 *   hailo cfgdump          — (Pi 5 only) raw 64-byte bus 1 config dump.
 *
 * Safe to run on any platform. On non-RASPI5 builds the driver is
 * never installed (stub returns -ENODEV) so `hailo` just reports
 * "driver not available".
 */

#include "hailo.h"
#include "hailo_control.h"
#include "hailo_cs_actions.h"
#include "hailo_cs_builder.h"
#include "hailo_cs_translator.h"
#include "hailo_infer.h"
#include "hailo_internal.h"
#include "hailo_tensor.h"
#include "hailo_vdma.h"
#include "hef_header.h"
#include "hef_parser.h"
#include "pmm.h"
#include "shell.h"
#include "uart.h"
#include "vfs.h"
#ifdef CONFIG_AI_SCHEDULER
#include "inference_device.h"
#include "inference_device_hailo.h"
#include "ai_policy_hailo.h"
#include "ai_types.h"
#include "timer.h"
#endif
#include <stdint.h>
#include <string.h>

/* Maximum bytes `hailo peek` will fetch in a single call. The
 * peek handler's stack buffer is sized to this, and the len
 * argument is rejected if it exceeds it — keeping the constant
 * and the buffer tied together so they can't drift. */
#define HAILO_PEEK_MAX_BYTES 64u

/* Tiny hex parser for the peek/poke shell commands. Accepts an
 * optional "0x" / "0X" prefix, returns 0 on success and the parsed
 * value in *out. Returns -1 if the string is empty, a digit is
 * invalid, or the accumulated value would overflow 32 bits. */
static int parse_hex_u32(const char *s, uint32_t *out)
{
    if (!s || !*s) return -1;
    if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) s += 2;
    if (!*s) return -1;
    uint32_t v = 0;
    for (; *s; s++) {
        uint32_t d;
        if (*s >= '0' && *s <= '9') {
            d = (uint32_t)(*s - '0');
        } else if (*s >= 'a' && *s <= 'f') {
            d = (uint32_t)(*s - 'a' + 10);
        } else if (*s >= 'A' && *s <= 'F') {
            d = (uint32_t)(*s - 'A' + 10);
        } else {
            return -1;
        }
        /* Caps the input at 8 hex digits (0xFFFFFFFF). Once the
         * accumulator has bit 28 set, the next left-shift-by-4
         * would push the MSB out and silently truncate. Reject
         * rather than wrap. */
        if (v > 0x0FFFFFFFu) return -1;
        v = (v << 4) | d;
    }
    *out = v;
    return 0;
}

/* Hex-dump `len` bytes from `p` over the shell with a "[--] <prefix>[NN]:"
 * line prefix and 16 bytes per line. Used by ctxsmoke for wire-byte
 * inspection. Bounded by `len`; caller picks the cap. */
static void shell_hex_dump_bytes(const char *prefix,
                                 const uint8_t *p, uint32_t len)
{
    for (uint32_t off = 0; off < len; off += 16) {
        shell_printf("  [--] %s[%02u]:", prefix, (unsigned)off);
        for (uint32_t i = 0; i < 16 && off + i < len; i++) {
            shell_printf(" %02x", (unsigned)p[off + i]);
        }
        shell_puts("\n");
    }
}

/*
 * Firmware blob linked in at build time via the CMake HAILO_FW_BLOB
 * option (default: no blob; `hailo boot` reports "firmware not
 * embedded"). When set, CMakeLists wraps the file with
 * `.incbin` + `hailo_fw_start`/`hailo_fw_end` symbols. Declared weak
 * here so default builds link cleanly without a strong definition.
 */
extern const uint8_t hailo_fw_start[] __attribute__((weak));
extern const uint8_t hailo_fw_end[]   __attribute__((weak));

/*
 * Upper bound for the proto body the `hailo load` shell path will
 * decode. hef_header.c's HEF_PROTO_MAX_SIZE stays at 256 MB as the
 * schema bound, but the shell refuses anything above this cap so a
 * corrupted-but-magic-matching .hef can't commandeer hundreds of MB
 * of RAM via a fake proto size. 16 MB covers every compiled Hailo
 * Model Zoo entry today (yolov5m is the largest at ~17 MB total
 * file; its proto body is ~2 MB).
 */
#define HAILO_LOAD_MAX_PROTO_MB  16u

/* `hailo ctxsmoke [variant [dcc0]]` — exercise the full Hailo
 * context-switch handshake against live firmware via the translator.
 *
 * Drives the 4-context load sequence (ACTIVATION → BATCH_SWITCHING →
 * PRELIMINARY → DYNAMIC) using a synthetic hef_info instead of a real
 * HEF, so the transport + translator + firmware-side validation can
 * be probed without parsing a model. Used as the regression check
 * for #180.
 *
 * Variants (argv[2]):
 *   full (default)  BURST + OPEN_BOUNDARY_OUTPUT + OPEN_BOUNDARY_INPUT
 *   min             BURST_CREDITS_TASK_RESET only (8-byte ACTIVATION)
 *   out             BURST + OPEN_BOUNDARY_OUTPUT
 *   in              BURST + OPEN_BOUNDARY_INPUT
 *
 * Optional dcc0 (argv[3]): override application_header.dynamic_contexts_
 * count to 0 and skip the DYNAMIC SET_CONTEXT_INFO call (legal only
 * when dcc=0 by firmware's "dcc+3 contexts" rule).
 *
 * Carries permanent diagnostic scaffolding (IOVA alignment dump,
 * 80-byte ACTIVATION hexdump, post-failure BAR4 scope scan + BAR0
 * bridge health check, 500ms inter-context delay). All four
 * SET_CONTEXT_INFO calls return rc=0 today on pi-5-1 fw v4.23 after
 * the #180 fixes; the diagnostics are kept so future regressions
 * surface where they happen instead of as a single rc=-3 line.
 * Phase 6.10 plans to retire the scaffolding once full inference
 * dispatch is implemented and the load path itself is the regression
 * surface. */
static int cmd_hailo_ctxsmoke(int argc, char *argv[])
{
    /* #180 bisection: argv[2] selects which boundary actions to
     * include in the synthesized ACTIVATION body. Default is
     * "full" (BURST + OUT + IN), matching the load path. */
    bool include_out = true;
    bool include_in  = true;
    const char *variant = "full";
    if (argc >= 3) {
        variant = argv[2];
        if      (strcmp(argv[2], "min")  == 0) { include_out = false; include_in = false; }
        else if (strcmp(argv[2], "out")  == 0) { include_out = true;  include_in = false; }
        else if (strcmp(argv[2], "in")   == 0) { include_out = false; include_in = true;  }
        else if (strcmp(argv[2], "full") == 0) { include_out = true;  include_in = true;  }
        else {
            shell_printf("hailo: ctxsmoke variant '%s' unknown — use min|out|in|full\n",
                         argv[2]);
            return 0;
        }
    }
    /* #361 hypothesis test (disconfirmed 2026-05-06): scan trailing
     * argv[] for `hwcN` where N is the GET_HW_CONSTS repeat count
     * (default 1). HailoRT v4.23 calls GET_HW_CONSTS 4× during HEF
     * load; the question was whether the count itself silences the
     * CPU_ECC_FATAL events fw fires on RPCs after RESET. Hardware
     * A/B on pi-5-1: x4 still fires CPU_ECC_FATAL at ENABLED and
     * generates additional ECC events during the GET_HW_CONSTS
     * sequence itself. Kept as a tunable knob so future investigation
     * can re-test cheaply (e.g., x4 plus settle-pings combination). */
    uint32_t hwc_repeat = 1u;
    /* #361 settle-ping hypothesis: when `pings` is set, fire APP-CPU
     * IDENTIFY + GET_DEVICE_INFORMATION before each CORE-CPU step.
     * HailoRT v4.23's wire capture (hailort-v4.23.0-wire-capture-mnist
     * -pi5.txt) shows ~3 APP-CPU RPCs interleaved before/after every
     * major CORE step; SLM-OS sends none. The question is whether the
     * pings silence the CPU_ECC_FATAL events fw fires after RESET. */
    bool inject_pings = false;
    for (int ai = 3; ai < argc; ai++) {
        if (strncmp(argv[ai], "hwc", 3) == 0) {
            int n = 0;
            for (const char *p = argv[ai] + 3; *p >= '0' && *p <= '9'; p++)
                n = n * 10 + (*p - '0');
            if (n >= 1 && n <= 16) hwc_repeat = (uint32_t)n;
        } else if (strcmp(argv[ai], "pings") == 0) {
            inject_pings = true;
        }
    }
    shell_printf("hailo: ctxsmoke variant=%s (out=%d in=%d) hwc_repeat=%u "
                 "pings=%d\n",
                 variant, (int)include_out, (int)include_in,
                 (unsigned)hwc_repeat, (int)inject_pings);
    /* Phase 6.3d/6.4 hardware probe: exercise the three context-
     * switch opcodes (CHANGE_CONTEXT_SWITCH_STATUS,
     * SET_NETWORK_GROUP_HEADER, SET_CONTEXT_INFO) against live
     * firmware. Phase 6.4f: driven by hailo_cs_translate_*
     * instead of hand-rolled context bytes — this exercises the
     * translator end-to-end alongside validating the transport.
     * A synthetic hef_info (ccw_action_count=1) provides the
     * minimum input; a real HEF is not required. */
    if (hailo_get_state() != HAILO_STATE_RUNNING) {
        shell_printf("hailo: ctxsmoke needs firmware booted (state=%s)\n",
                     hailo_state_str(hailo_get_state()));
        return 0;
    }

    /* Allocate a 512-byte CCW DMA buffer + VDMA descriptor list.
     * The translator consumes the IOVA + page_size + desc_count
     * through its cfg struct and bakes them into
     * host_buffer_info in the ACTIVATE_CFG_CHANNEL action. */
    struct hailo_tensor ccw_tensor = {0};
    struct hailo_vdma_desc_list ccw_list = {0};
    const uint32_t ccw_bytes = 512u;
    const uint16_t ccw_page_size = 512u;
    int trc = hailo_tensor_alloc(ccw_bytes, &ccw_tensor);
    if (trc != HAILO_OK) {
        shell_printf("  [--] SKIP: CCW tensor alloc failed (%d)\n", trc);
        shell_puts("hailo: ctxsmoke done\n");
        return 0;
    }
    memset(ccw_tensor.cpu_addr, 0xA5, ccw_bytes);
    hailo_tensor_prepare_for_device(&ccw_tensor);

    int drc = hailo_vdma_desc_list_alloc(/*desc_count=*/2,
                                         ccw_page_size,
                                         /*circular=*/false,
                                         &ccw_list);
    if (drc != HAILO_OK) {
        shell_printf("  [--] SKIP: vdma desc_list alloc failed (%d)\n", drc);
        hailo_tensor_free(&ccw_tensor);
        shell_puts("hailo: ctxsmoke done\n");
        return 0;
    }
    int programmed = hailo_vdma_program_buffer(&ccw_list, 0,
                                               ccw_tensor.iova,
                                               ccw_bytes,
                                               /*data_id=*/0);
    if (programmed < 0) {
        shell_printf("  [--] SKIP: vdma program_buffer failed (%d)\n", programmed);
        hailo_vdma_desc_list_free(&ccw_list);
        hailo_tensor_free(&ccw_tensor);
        shell_puts("hailo: ctxsmoke done\n");
        return 0;
    }

    /* #180 (resolved): allocate boundary input/output DMA tensors
     * + desc lists, populate synthetic pads with has_stream_info=
     * true so translate_activation emits BURST_CREDITS_TASK_RESET
     * + OPEN_BOUNDARY_OUTPUT + OPEN_BOUNDARY_INPUT (HailoRT order:
     * outputs first). With the 5-byte header + INIT timestamp +
     * D2H channel range fixes, firmware accepts all four contexts
     * and ctxsmoke completes cleanly. */
    const uint32_t bnd_bytes = 256u;
    const uint16_t bnd_page  = 4096u;
    struct hailo_tensor bnd_in_tensor  = {0};
    struct hailo_tensor bnd_out_tensor = {0};
    struct hailo_vdma_desc_list bnd_in_list  = {0};
    struct hailo_vdma_desc_list bnd_out_list = {0};
    int brc = hailo_tensor_alloc(bnd_bytes, &bnd_in_tensor);
    if (brc == HAILO_OK) brc = hailo_tensor_alloc(bnd_bytes, &bnd_out_tensor);
    if (brc == HAILO_OK) {
        memset(bnd_in_tensor.cpu_addr, 0, bnd_bytes);
        memset(bnd_out_tensor.cpu_addr, 0, bnd_bytes);
        hailo_tensor_prepare_for_device(&bnd_in_tensor);
        hailo_tensor_prepare_for_device(&bnd_out_tensor);
        /* #180 bisect: HailoRT typically allocates 64-256 descriptors
         * per boundary channel; 2 is the hardware minimum but may
         * not be what firmware expects. Try 64. */
        brc = hailo_vdma_desc_list_alloc(64, bnd_page, false, &bnd_in_list);
    }
    if (brc == HAILO_OK) brc = hailo_vdma_desc_list_alloc(64, bnd_page, false, &bnd_out_list);
    if (brc == HAILO_OK) {
        (void)hailo_vdma_program_buffer(&bnd_in_list,  0, bnd_in_tensor.iova,  bnd_bytes, 0);
        (void)hailo_vdma_program_buffer(&bnd_out_list, 0, bnd_out_tensor.iova, bnd_bytes, 0);
    }
    /* #180 Path A diag: print desc-list IOVAs + low-16-bit residue.
     * Firmware's HOST_DESCRIPTOR_BASE_ADDRESS_IS_NOT_64KB_ALIGNED
     * status (0x402d0004) keys off (iova & 0xFFFF). If residue != 0
     * here, the allocator is silently degrading alignment. */
    if (brc == HAILO_OK) {
        shell_printf("  [--] DIAG: ccw_iova=0x%lx (lo16=0x%04x)\n",
                     (unsigned long)ccw_list.iova,
                     (unsigned)(ccw_list.iova & 0xFFFFu));
        shell_printf("  [--] DIAG: bnd_in_iova=0x%lx (lo16=0x%04x)\n",
                     (unsigned long)bnd_in_list.iova,
                     (unsigned)(bnd_in_list.iova & 0xFFFFu));
        shell_printf("  [--] DIAG: bnd_out_iova=0x%lx (lo16=0x%04x)\n",
                     (unsigned long)bnd_out_list.iova,
                     (unsigned)(bnd_out_list.iova & 0xFFFFu));
    }
    if (brc != HAILO_OK) {
        shell_printf("  [--] SKIP: boundary DMA alloc failed (%d)\n", brc);
        hailo_vdma_desc_list_free(&bnd_out_list);
        hailo_vdma_desc_list_free(&bnd_in_list);
        hailo_tensor_free(&bnd_out_tensor);
        hailo_tensor_free(&bnd_in_tensor);
        hailo_vdma_desc_list_free(&ccw_list);
        hailo_tensor_free(&ccw_tensor);
        return 0;
    }

    /* Run the translator. Synthetic hef_info with 1 CCW action
     * + 2 boundary pads (1 input + 1 output) drives the full
     * ACTIVATION: BURST_CREDITS + OpenBoundaryInput + OpenBoundaryOutput. */
    struct hef_info info;
    memset(&info, 0, sizeof(info));
    info.ccw_action_count = 1;
    info.pad_count = 2;
    info.pads[0].is_input              = true;
    info.pads[0].has_stream_info       = include_in;
    info.pads[0].sys_index             = 1;
    info.pads[0].core_bytes_per_buffer = bnd_bytes;
    info.pads[1].is_input              = false;
    info.pads[1].has_stream_info       = include_out;
    info.pads[1].sys_index             = 2;
    info.pads[1].core_bytes_per_buffer = bnd_bytes;

    struct hailo_cs_translate_cfg tcfg = {
        .config_vdma_channel              = HAILO_CS_DEFAULT_CONFIG_VDMA_CHANNEL,
        .config_stream_index              = 0,
        .ccw_desc_list_iova               = ccw_list.iova,
        .ccw_desc_page_size               = ccw_page_size,
        .ccw_total_desc_count             = ccw_list.desc_count,
        .boundary_input_desc_list_iova    = bnd_in_list.iova,
        .boundary_input_total_desc_count  = bnd_in_list.desc_count,
        .boundary_output_desc_list_iova   = bnd_out_list.iova,
        .boundary_output_total_desc_count = bnd_out_list.desc_count,
        .boundary_desc_page_size          = bnd_page,
    };

    struct hailo_cs_application_header hdr;
    int terr = hailo_cs_translate_application_header(&info, &tcfg, &hdr);
    if (terr != HAILO_OK) {
        shell_printf("  [--] SKIP: translate_application_header failed (%d)\n", terr);
        hailo_vdma_desc_list_free(&bnd_out_list);
        hailo_vdma_desc_list_free(&bnd_in_list);
        hailo_tensor_free(&bnd_out_tensor);
        hailo_tensor_free(&bnd_in_tensor);
        hailo_vdma_desc_list_free(&ccw_list);
        hailo_tensor_free(&ccw_tensor);
        return 0;
    }
    /* #180 bisect: argv[3] == "dcc0" overrides dynamic_contexts_count
     * to 0. Tests whether earlier 2026-04-19 "BURST-only rc=0"
     * observation was possible because dcc=0 changes firmware's
     * expectation of ACTIVATION content. */
    bool dcc0 = (argc >= 4 && strcmp(argv[3], "dcc0") == 0);
    if (dcc0) {
        hdr.dynamic_contexts_count = 0;
        shell_puts("  [--] DIAG: dynamic_contexts_count overridden to 0\n");
    }

    struct hailo_cs_context_buffers bufs;
    terr = hailo_cs_translate_contexts(&info, &tcfg, &bufs);
    if (terr != HAILO_OK) {
        shell_printf("  [--] SKIP: translate_contexts failed (%d)\n", terr);
        hailo_vdma_desc_list_free(&bnd_out_list);
        hailo_vdma_desc_list_free(&bnd_in_list);
        hailo_tensor_free(&bnd_out_tensor);
        hailo_tensor_free(&bnd_in_tensor);
        hailo_vdma_desc_list_free(&ccw_list);
        hailo_tensor_free(&ccw_tensor);
        return 0;
    }

    shell_puts("hailo: ctxsmoke:\n");
    int rc;

    /* Settle-ping helper. Fires APP-CPU IDENTIFY + GET_DEVICE_INFORMATION
     * (two RPCs HailoRT interleaves before/after every CORE step) when
     * the `pings` knob is on, then drains d2h. Uses block scope and a
     * label so the existing rc/struct names stay reachable. */
#define CTXSMOKE_SETTLE_PINGS(_label)                                   \
    do {                                                                \
        if (inject_pings) {                                             \
            struct hailo_control_identify_response __idr;               \
            int __irc = hailo_control_identify(&__idr);                 \
            shell_printf("  [ping " _label "] IDENTIFY rc=%d\n", __irc);\
            uint32_t __dlen = 0;                                        \
            int __drc = hailo_control_get_device_information(&__dlen);  \
            shell_printf("  [ping " _label "] GET_DEVICE_INFO rc=%d "   \
                         "len=%u\n", __drc, (unsigned)__dlen);          \
        }                                                               \
    } while (0)

    CTXSMOKE_SETTLE_PINGS("pre-RESET");
    shell_puts("  [1/8] CHANGE_CONTEXT_SWITCH_STATUS(RESET)...\n");
    rc = hailo_control_change_context_switch_status(
        HAILO_CS_STATE_RESET,
        HAILO_CS_IGNORE_APPLICATION_INDEX,
        /*batch_size=*/0, /*batch_count=*/0);
    shell_printf("        rc=%d\n", rc);
#ifdef HAILO_WIRE_DEBUG
    shell_puts("  [d2h after RESET]\n");
    hailo_fw_drain_d2h_notifications(4);
#endif

    /* #180 pre-configure handshake (2026-04-19 wire capture): HailoRT
     * calls CLEAR_CONFIGURED_APPS then GET_HW_CONSTS between
     * CHANGE_CONTEXT_SWITCH_STATUS(RESET) and SET_NETWORK_GROUP_HEADER.
     * Skipping these left firmware's context-switch bookkeeping stale
     * and BATCH_SWITCHING walked into uninitialized state. */
    CTXSMOKE_SETTLE_PINGS("pre-CLEAR_APPS");
    shell_puts("  [2/8] CONTEXT_SWITCH_CLEAR_CONFIGURED_APPS...\n");
    rc = hailo_control_context_switch_clear_configured_apps();
    shell_printf("        rc=%d\n", rc);
#ifdef HAILO_WIRE_DEBUG
    shell_puts("  [d2h after CLEAR_CONFIGURED_APPS]\n");
    hailo_fw_drain_d2h_notifications(4);
#endif

    /* #361: GET_HW_CONSTS hypothesis test — repeat hwc_repeat times.
     * HailoRT v4.23 issues 4 back-to-back GET_HW_CONSTS RPCs during
     * HEF load; SLM-OS production used 1 until #361. Drain d2h after
     * each call to count CPU_ECC_FATAL events as the count varies. */
    shell_printf("  [3/8] GET_HW_CONSTS x%u...\n", (unsigned)hwc_repeat);
    uint32_t hw_consts_resp_len = 0;
    for (uint32_t i = 0; i < hwc_repeat; i++) {
        CTXSMOKE_SETTLE_PINGS("pre-GET_HW_CONSTS");
        rc = hailo_control_get_hw_consts(&hw_consts_resp_len);
        shell_printf("        [%u/%u] rc=%d resp_len=%u\n",
                     (unsigned)(i + 1), (unsigned)hwc_repeat,
                     rc, hw_consts_resp_len);
#ifdef HAILO_WIRE_DEBUG
        shell_printf("  [d2h after GET_HW_CONSTS #%u]\n", (unsigned)(i + 1));
        hailo_fw_drain_d2h_notifications(4);
#endif
    }

    CTXSMOKE_SETTLE_PINGS("pre-SET_NG_HEADER");
    shell_puts("  [4/8] SET_NETWORK_GROUP_HEADER...\n");
    rc = hailo_control_set_network_group_header(&hdr);
    shell_printf("        rc=%d\n", rc);
#ifdef HAILO_WIRE_DEBUG
    shell_puts("  [d2h after SET_NETWORK_GROUP_HEADER]\n");
    hailo_fw_drain_d2h_notifications(4);
#endif

    CTXSMOKE_SETTLE_PINGS("pre-CTX(ACT)");
    shell_printf("  [5/8] SET_CONTEXT_INFO(ACTIVATION, %u bytes)\n",
                 (unsigned)bufs.activation_len);
    /* #180 diag: dump the first 80 ACTIVATION bytes — the three
     * actions translate_activation emits in HailoRT order
     * (OUT before IN) total 63 B for a single-stream HEF:
     *   BURST_CREDITS_TASK_RESET   = 5 (hdr) + 0  (body) = 5
     *   OPEN_BOUNDARY_OUTPUT       = 5 (hdr) + 20 (body) = 25
     *   OPEN_BOUNDARY_INPUT        = 5 (hdr) + 28 (body) = 33
     * 80 covers the "full" variant with margin for tweaks. */
    {
        uint32_t dump_len = (bufs.activation_len > 80u)
                          ? 80u : (uint32_t)bufs.activation_len;
        shell_hex_dump_bytes("ACT",
                             (const uint8_t *)bufs.activation,
                             dump_len);
    }
    rc = hailo_control_set_context_info(HAILO_CS_CONTEXT_TYPE_ACTIVATION,
                                        bufs.activation,
                                        (uint32_t)bufs.activation_len);
    shell_printf("        rc=%d\n", rc);
#ifdef HAILO_WIRE_DEBUG
    shell_puts("  [d2h after SET_CONTEXT_INFO(ACTIVATION)]\n");
    hailo_fw_drain_d2h_notifications(4);
#endif

    /* Diagnostic: probe an APP-CPU opcode (IDENTIFY) right after
     * ACTIVATION. Hardware-verified on pi-5-1 fw v4.23 that this
     * returns rc=0 while the next CORE-CPU RPC (BATCH_SWITCHING)
     * times out — confirming the control channel as a whole is
     * healthy; firmware's CORE task specifically is busy
     * processing ACTIVATION's burst-credits reset asynchronously.
     * Kept as a permanent diagnostic so future regressions can
     * distinguish "CORE-busy" from "channel-wedged" at a glance. */
    struct hailo_control_identify_response idr;
    int irc = hailo_control_identify(&idr);
    shell_printf("  [--] DIAG: IDENTIFY(APP) rc=%d fw=%u.%u\n",
                 irc, (unsigned)idr.fw_version.major,
                 (unsigned)idr.fw_version.minor);

    /* #180 experiment A — async-busy test. Give firmware
     * up to 500 ms after ACTIVATION completes before sending
     * the next CORE-CPU RPC. If the CORE task was busy
     * processing ACTIVATION async, this delay should let it
     * catch up. */
    shell_puts("  [--] DIAG: sleeping 500 ms before BATCH_SWITCHING\n");
    hailo_platform->udelay(500000u);

    CTXSMOKE_SETTLE_PINGS("pre-CTX(BS)");
    shell_printf("  [6/8] SET_CONTEXT_INFO(BATCH_SWITCHING, %u bytes)\n",
                 (unsigned)bufs.batch_switching_len);
#ifdef HAILO_WIRE_DEBUG
    /* PR #359 follow-up: dump full BATCH_SWITCHING body so the
     * userspace probe can replay the exact bytes. Cap at 256 B
     * (BATCH_SWITCHING is typically tiny — 16 B for ctxsmoke). */
    {
        uint32_t dump_len = (bufs.batch_switching_len > 256u)
                          ? 256u : (uint32_t)bufs.batch_switching_len;
        shell_hex_dump_bytes("BSW",
                             (const uint8_t *)bufs.batch_switching,
                             dump_len);
    }
#endif
    rc = hailo_control_set_context_info(HAILO_CS_CONTEXT_TYPE_BATCH_SWITCHING,
                                        bufs.batch_switching,
                                        (uint32_t)bufs.batch_switching_len);
    shell_printf("        rc=%d\n", rc);
#ifdef HAILO_WIRE_DEBUG
    shell_puts("  [d2h after SET_CONTEXT_INFO(BATCH_SWITCHING)]\n");
    hailo_fw_drain_d2h_notifications(4);
#endif

    /* #180 experiment C — post-failure BAR4 scope scan. Findings
     * from experiment B: BAR4+0x640 stays 0xFFFFFFFF for >1 s AND
     * subsequent APP-CPU RPCs also return 0xFFFFFFFF. Is the wedge
     * confined to the response slot, or is the entire BAR4 window
     * broken? Read three distinct offsets:
     *   - 0x000: request slot we just wrote (should echo our request
     *            bytes if the window is intact).
     *   - 0x640: response slot (known 0xFFFFFFFF post-failure).
     *   - 0x100: firmware-owned region that doesn't alias either.
     *
     * If all three read 0xFFFFFFFF → ATR window entirely gone.
     * If 0x000 echoes and 0x640 is 0xFFFFFFFF → firmware stopped
     * writing responses but the PCIe↔device mapping is intact.
     * If 0x000 and 0x100 show sensible data but 0x640 is unique →
     * firmware-side panic/reset of the response slot only. */
    if (rc != HAILO_OK) {
        shell_puts("  [--] DIAG: BAR4 window scope scan after failure:\n");
        uint32_t probes[3] = { 0x000u, 0x100u, 0x640u };
        for (int i = 0; i < 3; i++) {
            uint32_t w[4];
            hailo_platform->bar4_read(probes[i], w, sizeof(w));
            shell_printf("        [+0x%03x] %08x %08x %08x %08x\n",
                         probes[i], w[0], w[1], w[2], w[3]);
        }
        /* Diag D: BAR0 (PLDA bridge + ATRs) health check. If BAR0
         * reads also return 0xFFFFFFFF, the PCIe link dropped. If
         * BAR0 reads reasonable values, the link is alive and the
         * ATR[0] translation (BAR4 → firmware memory) specifically
         * is what broke. Read ATR[0].TRSL_ADDR_LO + ISTATUS + IMASK
         * and compare against the values we programmed at init. */
        shell_puts("  [--] DIAG: BAR0 bridge health:\n");
        uint32_t atr_lo = hailo_platform->read32(HAILO_BAR_CONFIG,
                              HAILO_ATR_BASE + HAILO_ATR_OFF_TRSL_ADDR_LO);
        uint32_t istatus = hailo_platform->read32(HAILO_BAR_CONFIG,
                                                   HAILO_BCS_ISTATUS_HOST);
        uint32_t imask   = hailo_platform->read32(HAILO_BAR_CONFIG,
                                                   HAILO_BSC_IMASK_HOST);
        shell_printf("        ATR[0].TRSL_LO=0x%08x (init=0x%08x)\n",
                     atr_lo, (unsigned)HAILO_CONTROL_SECTION_ADDR_H8);
        shell_printf("        BCS_ISTATUS_HOST=0x%08x\n", istatus);
        shell_printf("        BSC_IMASK_HOST=0x%08x\n", imask);
    }

    CTXSMOKE_SETTLE_PINGS("pre-CTX(PRE)");
    shell_printf("  [7/8] SET_CONTEXT_INFO(PRELIMINARY, %u bytes)\n",
                 (unsigned)bufs.preliminary_len);
    shell_printf("        CCW buffer iova=0x%lx\n",
                 (unsigned long)ccw_list.iova);
#ifdef HAILO_WIRE_DEBUG
    {
        uint32_t dump_len = (bufs.preliminary_len > 256u)
                          ? 256u : (uint32_t)bufs.preliminary_len;
        shell_hex_dump_bytes("PRE",
                             (const uint8_t *)bufs.preliminary,
                             dump_len);
    }
#endif
    rc = hailo_control_set_context_info(HAILO_CS_CONTEXT_TYPE_PRELIMINARY,
                                        bufs.preliminary,
                                        (uint32_t)bufs.preliminary_len);
    shell_printf("        rc=%d\n", rc);
#ifdef HAILO_WIRE_DEBUG
    shell_puts("  [d2h after SET_CONTEXT_INFO(PRELIMINARY)]\n");
    hailo_fw_drain_d2h_notifications(4);
#endif

    if (!dcc0) {
        CTXSMOKE_SETTLE_PINGS("pre-CTX(DYN)");
        shell_printf("  [8/8] SET_CONTEXT_INFO(DYNAMIC, %u bytes)\n",
                     (unsigned)bufs.dynamic_len);
#ifdef HAILO_WIRE_DEBUG
        /* DYNAMIC can be large (528 B observed in HailoRT trace).
         * Print all bytes — the probe needs the full sequence. */
        {
            uint32_t dump_len = (bufs.dynamic_len > 1024u)
                              ? 1024u : (uint32_t)bufs.dynamic_len;
            shell_hex_dump_bytes("DYN",
                                 (const uint8_t *)bufs.dynamic,
                                 dump_len);
        }
#endif
        rc = hailo_control_set_context_info(HAILO_CS_CONTEXT_TYPE_DYNAMIC,
                                            bufs.dynamic,
                                            (uint32_t)bufs.dynamic_len);
        shell_printf("        rc=%d\n", rc);
#ifdef HAILO_WIRE_DEBUG
        shell_puts("  [d2h after SET_CONTEXT_INFO(DYNAMIC)]\n");
        hailo_fw_drain_d2h_notifications(4);
#endif
    } else {
        shell_puts("  [8/8] SKIP: DYNAMIC (dcc0 mode)\n");
    }

    /* Phase 6.10 step 3: flip firmware out of config mode into RUN.
     * application_index = 0 (first and only network group in our
     * synthetic load). dynamic_batch_size = batch_count = 0 uses
     * the header-supplied batch (we set batch_size=1 in the app
     * header). If firmware accepts all 4 SET_CONTEXT_INFO calls,
     * this transition unlocks per-frame VDMA submission. */
    CTXSMOKE_SETTLE_PINGS("pre-ENABLED");
    shell_puts("  [-/8] CHANGE_CONTEXT_SWITCH_STATUS(ENABLED)...\n");
    rc = hailo_control_change_context_switch_status(
        HAILO_CS_STATE_ENABLED,
        /*application_index=*/0,
        /*batch_size=*/0, /*batch_count=*/0);
    shell_printf("        rc=%d\n", rc);
#ifdef HAILO_WIRE_DEBUG
    shell_puts("  [d2h after CHANGE_STATUS(ENABLED)]\n");
    hailo_fw_drain_d2h_notifications(4);
#endif

#undef CTXSMOKE_SETTLE_PINGS

    hailo_vdma_desc_list_free(&bnd_out_list);
    hailo_vdma_desc_list_free(&bnd_in_list);
    hailo_tensor_free(&bnd_out_tensor);
    hailo_tensor_free(&bnd_in_tensor);
    hailo_vdma_desc_list_free(&ccw_list);
    hailo_tensor_free(&ccw_tensor);

    shell_puts("hailo: ctxsmoke done\n");
    return 0;
}

static int cmd_hailo(int argc, char *argv[])
{
    if (argc >= 2 && strcmp(argv[1], "probe") == 0) {
        uint16_t v = 0, d = 0;
        int rc = hailo_probe(&v, &d);
        if (rc == HAILO_OK) {
            shell_printf("hailo: probe OK, vendor=0x%04x device=0x%04x, "
                        "state=%s\n", v, d,
                        hailo_state_str(hailo_get_state()));
        } else if (rc == HAILO_ERR_NODEV) {
            shell_puts("hailo: no device (probe returned NODEV)\n");
        } else {
            shell_printf("hailo: probe failed (%d)\n", rc);
        }
        return 0;
    }

    if (argc >= 2 && strcmp(argv[1], "cfgdump") == 0) {
#if defined(PLATFORM_RASPI5)
        /* PCIe1 EXT_CFG_INDEX at RC_base + 0x9000, EXT_CFG_DATA at
         * RC_base + 0x8000 (NOT 0x9004 — see pcie_bcm2712.c comment
         * on PCIE1_EXT_CFG_DATA for why the variant table's 0x9004
         * is vestigial and only 0x8000 returns real data past
         * config offset 0x07). */
        volatile uint32_t *idx  = (volatile uint32_t *)0x1000119000UL;
        volatile uint8_t  *data = (volatile uint8_t  *)0x1000118000UL;
        uart_puts("Dumping bus 1 dev 0 func 0 config (INDEX pinned):\n");
        *idx = 0x00100000u;
        __asm__ volatile("dsb sy" ::: "memory");
        for (uint32_t off = 0; off < 0x40; off += 4) {
            uint32_t v = *(volatile uint32_t *)(data + off);
            uart_printf("  [0x%02x]=0x%08lx\n", off, (unsigned long)v);
        }
#else
        uart_puts("hailo: cfgdump is Pi 5-only (requires BCM2712 pcie1 RC)\n");
#endif
        return 0;
    }

    if (argc >= 2 && strcmp(argv[1], "boot") == 0) {
        /* Cast to (const void *) before comparing — treating extern
         * char[] symbols as arrays trips -Warray-compare. The weak
         * symbols resolve to NULL when no blob is linked. */
        const void *fw_lo = (const void *)hailo_fw_start;
        const void *fw_hi = (const void *)hailo_fw_end;
        if (!fw_lo || !fw_hi || fw_hi <= fw_lo) {
            shell_puts("hailo: firmware not embedded -- rebuild with "
                       "-DHAILO_FW_BLOB=path/to/hailo8_fw.bin\n");
            return 0;
        }
        size_t fw_size = (size_t)((const uint8_t *)fw_hi
                                - (const uint8_t *)fw_lo);
        shell_printf("hailo: booting from embedded firmware (%lu bytes)...\n",
                     (unsigned long)fw_size);
        int rc = hailo_boot(fw_lo, fw_size);
        if (rc == HAILO_OK) {
            shell_printf("hailo: boot OK, state=%s\n",
                         hailo_state_str(hailo_get_state()));
            /* #682 checkpoint 1: IN ch=2 base register state right
             * after fw flash, before any model load. Establishes
             * baseline — if avail!=0 here, the stale-SRAM hypothesis
             * is in play. */
            hailo_vdma_dump_channel_regs(2, "[682-cp1] post boot");
        } else {
            shell_printf("hailo: boot failed (%d), state=%s\n", rc,
                         hailo_state_str(hailo_get_state()));
        }
        return 0;
    }

    if (argc >= 2 && strcmp(argv[1], "load") == 0) {
        if (argc < 3) {
            shell_puts("usage: hailo load <vfs-path> "
                       "[upload <hex-device-base>]\n");
            return 0;
        }
        const char *path = argv[2];

        /* Optional: `hailo load <path> upload <base>` kicks off the
         * CCW upload right after the parse, targeting `base` as the
         * device-side starting address for the first action. Each
         * subsequent action is appended at base + cumulative bytes.
         * Without a real CONFIG_STREAM response feeding the right
         * base, callers pick one manually for bring-up.
         *
         * Optional: `hailo load <path> sched` loads the HEF into the
         * "hailo-8" inference_device backend and installs the handle
         * into ai_policy_hailo so scheduler decisions can route
         * through the NPU. Placeholder quantization parameters
         * (scale=1/128, zp=0) are used until HEF quant-metadata
         * extraction lands. */
        bool     do_upload = false;
        uint32_t upload_base = 0;
        bool     do_sched = false;
        if (argc >= 5 && strcmp(argv[3], "upload") == 0) {
            if (parse_hex_u32(argv[4], &upload_base) != 0) {
                shell_printf("hailo: load upload: bad hex base '%s'\n",
                             argv[4]);
                return 0;
            }
            do_upload = true;
        } else if (argc >= 4 && strcmp(argv[3], "sched") == 0) {
            do_sched = true;
        }

        struct vfs_entry_info info = {0};
        if (vfs_stat_path(path, &info) != 0) {
            shell_printf("hailo: stat '%s' failed\n", path);
            return 0;
        }
        if (info.size < 12) {
            shell_printf("hailo: '%s' too small (%lu bytes)\n",
                         path, (unsigned long)info.size);
            return 0;
        }

        /* Two-pass read: the outer header (up to 32 bytes) tells us
         * the proto body size, which may be multi-MB. Stage 1 pulls
         * just the header into a stack buffer; stage 2 PMM-allocates
         * the body. */
        uint8_t hdr_buf[64];
        size_t hdr_read = info.size < sizeof(hdr_buf) ? info.size : sizeof(hdr_buf);
        int n = vfs_read_path(path, (char *)hdr_buf, hdr_read, 0);
        if (n < 0) {
            shell_printf("hailo: read '%s' failed\n", path);
            return 0;
        }

        struct hef_outer_header outer = {0};
        /* hef_parse_outer_header's truncation check verifies the proto
         * body fits within the supplied `size`. We've only read the
         * first 64 bytes into hdr_buf, but info.size is the true file
         * length — pass that so the check passes, while the parser's
         * actual reads stay within the 32-byte header region (well
         * inside the 64 bytes we buffered). */
        int rc = hef_parse_outer_header(hdr_buf, info.size, &outer);
        (void)n;    /* read was validated above; silence unused-var warning */
        if (rc != HEF_OK) {
            shell_printf("hailo: outer-header parse failed (%d)\n", rc);
            return 0;
        }
        shell_printf("hailo: hef v%u proto_size=%u (total %lu bytes)\n",
                     outer.version, outer.proto_size,
                     (unsigned long)info.size);

        if (outer.proto_size > HAILO_LOAD_MAX_PROTO_MB * 1024u * 1024u) {
            shell_printf("hailo: refusing to load proto body of %u bytes "
                         "(> %u MB shell cap; if this is legitimate, raise "
                         "HAILO_LOAD_MAX_PROTO_MB in hailo_shell.c)\n",
                         outer.proto_size, HAILO_LOAD_MAX_PROTO_MB);
            return 0;
        }

        /* Allocate a contiguous page-aligned buffer for the proto
         * body. PMM rounds up to the next power-of-2 page count. */
        size_t body_pages = (outer.proto_size + PAGE_SIZE - 1) / PAGE_SIZE;
        if (body_pages == 0) body_pages = 1;
        void *body = pmm_alloc_pages(body_pages);
        if (!body) {
            shell_printf("hailo: pmm_alloc_pages(%lu) failed for proto body\n",
                         (unsigned long)body_pages);
            return 0;
        }

        n = vfs_read_path(path, (char *)body, outer.proto_size,
                          outer.proto_offset);
        if (n < 0 || (uint32_t)n < outer.proto_size) {
            shell_printf("hailo: read proto body failed (%d of %u)\n",
                         n, outer.proto_size);
            pmm_free_pages(body, body_pages);
            return 0;
        }

        struct hef_info meta;
        rc = hef_parse_body(body, outer.proto_size, &meta);
        if (rc != HEF_PARSER_OK) {
            shell_printf("hailo: proto decode failed (%d)\n", rc);
            pmm_free_pages(body, body_pages);
            return 0;
        }
        /* NOTE: body is NOT freed yet — meta.ccw_actions[i].
         * data_offset_in_blob references into it and the optional
         * upload path below needs the blob live. Freed at the end
         * of this handler. */

        shell_printf("  hw_arch = %s (%u)\n",
                     meta.hw_arch_known ?
                         (meta.hw_arch == HEF_HW_ARCH_HAILO8   ? "hailo8"  :
                          meta.hw_arch == HEF_HW_ARCH_HAILO8P  ? "hailo8p" :
                          meta.hw_arch == HEF_HW_ARCH_HAILO8R  ? "hailo8r" :
                          meta.hw_arch == HEF_HW_ARCH_HAILO8L  ? "hailo8l" :
                          meta.hw_arch == HEF_HW_ARCH_HAILO15M ? "hailo15m":
                          meta.hw_arch == HEF_HW_ARCH_HAILO15L ? "hailo15l":
                          meta.hw_arch == HEF_HW_ARCH_HAILO1XH ? "hailo1xh":
                          "unknown") : "absent",
                     meta.hw_arch);
        if (meta.sdk_version[0]) {
            shell_printf("  sdk_version = %s\n", meta.sdk_version);
        }
        shell_printf("  network_groups = %u\n", meta.network_group_count);
        if (meta.first_network_group[0]) {
            shell_printf("  first network group = %s\n",
                         meta.first_network_group);
        }
        if (meta.op_count > 0 || meta.pad_count > 0) {
            shell_printf("  first NG ops = %u, pads captured = %u%s\n",
                         meta.op_count, meta.pad_count,
                         meta.pads_truncated ? " (truncated)" : "");
            for (uint32_t i = 0; i < meta.pad_count; i++) {
                const struct hef_pad_info *p = &meta.pads[i];
                const char *name = p->name[0] ? p->name : "<unnamed>";
                if (p->has_tensor_shape) {
                    shell_printf("    %s pad[%u] \"%s\" shape=%ux%ux%u"
                                 " (padded %ux%ux%u)\n",
                                 p->is_input ? "in" : "out",
                                 p->index, name,
                                 p->height, p->width, p->features,
                                 p->padded_height, p->padded_width,
                                 p->padded_features);
                } else {
                    shell_printf("    %s pad[%u] \"%s\" (no tensor_shape)\n",
                                 p->is_input ? "in" : "out",
                                 p->index, name);
                }
                if (p->has_stream_info) {
                    shell_printf("      stream: sys_index=%u "
                                 "core_bytes=%u core_buffers=%u\n",
                                 p->sys_index, p->core_bytes_per_buffer,
                                 p->core_buffers_per_frame);
                }
                if (p->has_quant_info) {
                    shell_printf("      quant: scale_raw=0x%08x "
                                 "zp_raw=0x%08x\n",
                                 p->qp_scale_raw, p->qp_zp_raw);
                }
            }
        }
        if (meta.string_truncated) {
            shell_printf("  (note: at least one string was truncated "
                         "at %u bytes)\n", (unsigned)HEF_PARSER_MAX_STR);
        }
        /* CCW write-action summary (Phase 5.3). Shows how much weight
         * data the first network group's preliminary_config would push
         * through WRITE_MEMORY during a future upload. */
        if (meta.ccw_action_count > 0) {
            shell_printf("  ccw: %u action(s), total %lu bytes%s\n",
                         meta.ccw_action_count,
                         (unsigned long)meta.ccw_total_bytes,
                         meta.ccw_actions_truncated
                             ? " (truncated)" : "");
            /* #253 Phase 8: tally CCW bytes per cfg_channel_index so
             * we can decide whether HailoRT's multi-cfg-channel flow
             * needs to be mirrored. Emit the first few entries + a
             * per-channel byte breakdown. */
            uint32_t per_ch[8] = {0};
            uint32_t per_ch_count[8] = {0};
            for (uint32_t i = 0; i < meta.ccw_action_count; i++) {
                uint32_t ci = meta.ccw_actions[i].cfg_channel_index;
                if (ci < 8) {
                    per_ch[ci] += meta.ccw_actions[i].data_size;
                    per_ch_count[ci]++;
                }
            }
            for (uint32_t c = 0; c < 8; c++) {
                if (per_ch[c] == 0 && per_ch_count[c] == 0) continue;
                shell_printf("    cfg_channel[%u]: %u action(s), %lu bytes\n",
                             c, per_ch_count[c], (unsigned long)per_ch[c]);
            }
        }

        if (do_upload) {
            uint64_t uploaded = 0;
            /* hailo load <path> upload <base> is the manual v0/v1
             * path — no CCWS block needed. Pass NULL for ccws_base;
             * the upload function will reject any v2+ action with a
             * clear error if one snuck in. blob_size is the proto
             * body we've already allocated + read into `body`. */
            int urc = hailo_control_upload_ccw(&meta,
                                               body, outer.proto_size,
                                               NULL, 0,
                                               upload_base, &uploaded);
            if (urc == HAILO_OK) {
                shell_printf("  ccw: uploaded %lu bytes starting at 0x%08x\n",
                             (unsigned long)uploaded, upload_base);
            } else {
                shell_printf("  ccw: upload failed (%d)\n", urc);
            }
        }

#ifdef CONFIG_AI_SCHEDULER
        if (do_sched) {
            /* Hand the full HEF blob (header + proto + CCWS) to the
             * Hailo inference_device backend for load_model. Size
             * includes the CCWS region (HEF v2 appends CCWS after
             * the proto body, no explicit size field — see
             * hef_header.c v2 handler). Without CCWS the NPU has no
             * weights and inference_run times out. */
            size_t proto_end = (size_t)outer.proto_offset + outer.proto_size;
            size_t total     = proto_end + (size_t)outer.ccws_size;
            size_t full_pages = (total + PAGE_SIZE - 1) / PAGE_SIZE;
            uint8_t *full = NULL;
            /* Defensive bound: hdr_buf is 64 B. v0/v1/v2/v3 proto_offset
             * values (32/28/44/52) all fit. A future HEF version with
             * a larger trailer would otherwise silently copy past
             * hdr_buf's tail; reject cleanly before that happens. */
            if (outer.proto_offset > sizeof(hdr_buf)) {
                shell_printf("hailo: sched: proto_offset=%u > "
                             "hdr_buf size %zu (new HEF version?)\n",
                             outer.proto_offset, sizeof(hdr_buf));
            } else {
                full = pmm_alloc_pages(full_pages);
            }
            if (!full && outer.proto_offset <= sizeof(hdr_buf)) {
                shell_printf("hailo: sched: pmm_alloc_pages(%lu) failed\n",
                             (unsigned long)full_pages);
            }
            if (full) {
                memcpy(full, hdr_buf, outer.proto_offset);
                memcpy(full + outer.proto_offset, body, outer.proto_size);
                /* Read CCWS (the rest of the file) directly from VFS
                 * into the tail of the buffer. No need to stage
                 * through a separate body allocation — CCWS is
                 * device-bound, not parsed. */
                if (outer.ccws_size > 0) {
                    int ccws_read = vfs_read_path(path,
                        (char *)(full + proto_end),
                        (size_t)outer.ccws_size, proto_end);
                    if (ccws_read < 0 ||
                        (size_t)ccws_read != (size_t)outer.ccws_size) {
                        shell_printf("hailo: sched: CCWS read short "
                                     "(%d of %lu at offset %lu)\n",
                                     ccws_read,
                                     (unsigned long)outer.ccws_size,
                                     (unsigned long)proto_end);
                        pmm_free_pages(full, full_pages);
                        full = NULL;
                    }
                }

                struct inference_device *dev = NULL;
                if (full) dev = inference_device_find("hailo-8");
                if (!dev) {
                    shell_puts("hailo: sched: 'hailo-8' device not registered\n");
                } else {
                    inference_model_handle_t h = INF_INVALID_HANDLE;
                    int lrc = inference_load_model(dev, full, total, &h);
                    if (lrc != INF_OK) {
                        shell_printf("hailo: sched: load_model failed (%d)\n", lrc);
                    } else {
                        /* Audit F-08 (2026-04-24): query the backend for
                         * the actual transport byte counts, instead of
                         * pinning input_n/output_n to AI_STATE_DIM /
                         * AI_SCHED_N_ACTIONS. The semantic policy
                         * dimension stays AI_STATE_DIM/AI_SCHED_N_ACTIONS;
                         * the backend's transport buffers can be larger
                         * (HEF padding) and `ai_schedule_mlp_via_hailo`
                         * already pads/clamps appropriately. */
                        uint32_t in_b = AI_STATE_DIM;
                        uint32_t out_b = AI_SCHED_N_ACTIONS;
                        int sz_rc = hailo_backend_model_sizes(h, &in_b, &out_b);
                        if (sz_rc != 0) {
                            in_b = AI_STATE_DIM;
                            out_b = AI_SCHED_N_ACTIONS;
                        }
                        if (in_b < AI_STATE_DIM || out_b < AI_SCHED_N_ACTIONS) {
                            shell_printf("hailo: sched: HEF transport too "
                                         "small for policy (in=%u<%u or "
                                         "out=%u<%u); falling back to "
                                         "policy dims\n",
                                         in_b, AI_STATE_DIM,
                                         out_b, AI_SCHED_N_ACTIONS);
                            in_b = AI_STATE_DIM;
                            out_b = AI_SCHED_N_ACTIONS;
                        }

                        /* Prefer HEF-derived quant (Phase 6.2b) when
                         * the parser captured per-pad scale+zp. Walk
                         * the pads to find the first input + first
                         * output with has_quant_info. Falls back to
                         * placeholder scale=1/128 / zp=0 if either
                         * pad lacks quant_info, or if set_from_raw
                         * rejects the scale as invalid (zero/NaN). */
                        const struct hef_pad_info *qin  = NULL;
                        const struct hef_pad_info *qout = NULL;
                        for (uint32_t i = 0; i < meta.pad_count; i++) {
                            if (!meta.pads[i].has_tensor_shape) continue;
                            if (meta.pads[i].is_input && !qin)  qin  = &meta.pads[i];
                            if (!meta.pads[i].is_input && !qout) qout = &meta.pads[i];
                        }
                        int rc_quant = -1;
                        if (qin && qout &&
                            qin->has_quant_info && qout->has_quant_info) {
                            rc_quant = ai_policy_hailo_set_model_from_raw(
                                h,
                                qin->qp_scale_raw,  qin->qp_zp_raw,
                                qout->qp_scale_raw, qout->qp_zp_raw,
                                in_b, out_b);
                        }
                        if (rc_quant == 0) {
                            shell_printf("hailo: sched: model loaded "
                                         "(handle=%d), ai_policy_hailo armed "
                                         "with HEF quant (in=%u out=%u)\n",
                                         (int)h, in_b, out_b);
                        } else {
                            ai_policy_hailo_set_model_placeholder(
                                h, in_b, out_b);
                            shell_printf("hailo: sched: model loaded "
                                         "(handle=%d), ai_policy_hailo armed "
                                         "with placeholder quant (in=%u out=%u, "
                                         "HEF quant_info %s)\n",
                                         (int)h, in_b, out_b,
                                         (qin && qin->has_quant_info &&
                                          qout && qout->has_quant_info)
                                             ? "rejected (invalid scale)"
                                             : "absent");
                        }
                    }
                }
                pmm_free_pages(full, full_pages);
            }
        }
#else
        (void)do_sched;
#endif

        pmm_free_pages(body, body_pages);
        return 0;
    }

    if (argc >= 2 && strcmp(argv[1], "fw") == 0) {
        uint32_t maj = 0, min = 0, rev = 0;
        int rc = hailo_get_firmware_version(&maj, &min, &rev);
        if (rc == HAILO_OK) {
            shell_printf("hailo: firmware %u.%u.%u\n", maj, min, rev);
        } else {
            shell_printf("hailo: firmware version unavailable (%d — "
                        "device must be booted first)\n", rc);
        }
        return 0;
    }

    if (argc >= 3 && strcmp(argv[1], "peek") == 0) {
        /* `hailo peek <addr> [<len>]` — READ_MEMORY on the running
         * firmware at device-side address `addr` for `len` bytes
         * (default 16). Output is hex bytes on one line. */
        uint32_t addr = 0;
        if (parse_hex_u32(argv[2], &addr) != 0) {
            shell_printf("hailo: peek: invalid address '%s' "
                         "(expected 0xNNNN)\n", argv[2]);
            return 0;
        }
        uint32_t len = 16;
        if (argc >= 4 && parse_hex_u32(argv[3], &len) != 0) {
            shell_printf("hailo: peek: invalid length '%s'\n", argv[3]);
            return 0;
        }
        if (len == 0 || len > HAILO_PEEK_MAX_BYTES) {
            shell_printf("hailo: peek: length %u out of range (1..%u)\n",
                         len, (unsigned)HAILO_PEEK_MAX_BYTES);
            return 0;
        }
        uint8_t buf[HAILO_PEEK_MAX_BYTES];
        int rc = hailo_control_read_memory(addr, buf, len);
        if (rc != HAILO_OK) {
            shell_printf("hailo: peek failed (%d)\n", rc);
            return 0;
        }
        shell_printf("hailo: [0x%08x] =", addr);
        for (uint32_t i = 0; i < len; i++) {
            shell_printf(" %02x", buf[i]);
        }
        shell_puts("\n");
        return 0;
    }

    if (argc >= 4 && strcmp(argv[1], "poke") == 0) {
        /* `hailo poke <addr> <hex-u32>` — WRITE_MEMORY a single 32-bit
         * word at device-side `addr`. Minimal path for round-trip
         * verification from the shell; larger writes are programmatic.
         * Value is written little-endian (matches how firmware memcpys
         * raw bytes; HailoRT does the same). */
        uint32_t addr = 0, value = 0;
        if (parse_hex_u32(argv[2], &addr) != 0) {
            shell_printf("hailo: poke: invalid address '%s'\n", argv[2]);
            return 0;
        }
        if (parse_hex_u32(argv[3], &value) != 0) {
            shell_printf("hailo: poke: invalid value '%s'\n", argv[3]);
            return 0;
        }
        uint8_t bytes[4] = {
            (uint8_t)(value & 0xFF),
            (uint8_t)((value >>  8) & 0xFF),
            (uint8_t)((value >> 16) & 0xFF),
            (uint8_t)((value >> 24) & 0xFF),
        };
        int rc = hailo_control_write_memory(addr, bytes, sizeof(bytes));
        if (rc != HAILO_OK) {
            shell_printf("hailo: poke failed (%d)\n", rc);
            return 0;
        }
        shell_printf("hailo: wrote 0x%08x to [0x%08x]\n", value, addr);
        return 0;
    }

    if (argc >= 2 && strcmp(argv[1], "cfgstream") == 0) {
        /* `hailo cfgstream <dir> <channel>` — issue a minimal PCIe
         * CONFIG_STREAM (opcode 0x03) to pi-5-1 firmware. Most
         * nn_stream_config fields are left zero because this
         * command is primarily a transport-liveness probe — a real
         * caller would pull these values from the .hef. */
        if (argc < 4) {
            shell_puts("usage: hailo cfgstream <in|out> <channel-hex>\n");
            return 0;
        }
        bool is_input;
        if (strcmp(argv[2], "in") == 0) {
            is_input = true;
        } else if (strcmp(argv[2], "out") == 0) {
            is_input = false;
        } else {
            shell_printf("hailo: cfgstream: direction '%s' not in "
                         "{in, out}\n", argv[2]);
            return 0;
        }
        uint32_t ch = 0;
        if (parse_hex_u32(argv[3], &ch) != 0 || ch >= 16) {
            shell_printf("hailo: cfgstream: channel '%s' not a hex u4\n",
                         argv[3]);
            return 0;
        }

        struct hailo_stream_pcie_config cfg = {0};
        cfg.stream_index          = 0;
        cfg.is_input              = is_input;
        cfg.skip_nn_stream_config = true;   /* minimal probe */
        cfg.pcie_channel_index    = (uint8_t)ch;
        if (is_input) {
            cfg.pcie_dataflow_type = (uint8_t)HAILO_PCIE_DATAFLOW_TYPE_CONTINUOUS;
        } else {
            cfg.desc_page_size = 512;
        }

        uint8_t dmid = 0;
        int rc = hailo_control_config_stream_pcie(&cfg, &dmid);
        if (rc != HAILO_OK) {
            shell_printf("hailo: cfgstream failed (%d)\n", rc);
            return 0;
        }
        shell_printf("hailo: cfgstream %s ch=%u -> dataflow_manager_id=0x%02x\n",
                     is_input ? "in" : "out", (unsigned)ch, dmid);
        return 0;
    }

    if (argc >= 2 && strcmp(argv[1], "infer") == 0) {
        /* `hailo infer <hex-bytes>` — run an end-to-end inference
         * smoke test with a synthetic tensor of the given size.
         * Uses channel 0 (input) / 1 (output) and data_id 0 by
         * default — these would be HEF-derived in a real call.
         * Fails on Pi 5 today (no active stream context); exercises
         * the hailo_infer_run pipeline end-to-end in QEMU tests. */
        if (argc < 3) {
            shell_puts("usage: hailo infer <hex-bytes>\n");
            return 0;
        }
        uint32_t bytes = 0;
        if (parse_hex_u32(argv[2], &bytes) != 0 || bytes == 0) {
            shell_printf("hailo: infer: bad byte count '%s'\n", argv[2]);
            return 0;
        }
        if (bytes > 64u * 1024u) {
            shell_printf("hailo: infer: %u bytes > 64 KB shell cap\n", bytes);
            return 0;
        }
        size_t pages = (bytes + PAGE_SIZE - 1) / PAGE_SIZE;
        void *in = pmm_alloc_pages(pages);
        void *out = pmm_alloc_pages(pages);
        if (!in || !out) {
            shell_puts("hailo: infer: scratch alloc failed\n");
            if (in)  pmm_free_pages(in, pages);
            if (out) pmm_free_pages(out, pages);
            return 0;
        }
        memset(in, 0xA5, bytes);
        struct hailo_infer_config cfg = {
            .input_bytes     = bytes,
            .output_bytes    = bytes,
            .input_channel   = 0,
            .output_channel  = 1,
            .input_data_id   = 0,
            .output_data_id  = 0,
            .input_page_size = 512,
            .output_page_size = 512,
            .timeout_us      = 500000u,
        };
        uint64_t elapsed = 0;
        int rc = hailo_infer_run(&cfg, in, out, &elapsed);
        if (rc == HAILO_OK) {
            shell_printf("hailo: infer OK (%u bytes, %lu us)\n",
                         bytes, (unsigned long)elapsed);
        } else {
            shell_printf("hailo: infer failed (%d) — expected until "
                         "CONFIG_STREAM context lands\n", rc);
        }
        pmm_free_pages(in,  pages);
        pmm_free_pages(out, pages);
        return 0;
    }

    if (argc >= 2 && strcmp(argv[1], "ctxsmoke") == 0) {
        return cmd_hailo_ctxsmoke(argc, argv);
    }

    /* Phase 8: dump the cs_load progress counter. Updated by the
     * inference backend at each stage of context_switch_load so we
     * can diagnose wedges without relying on live serial output.
     * See kernel/inference/inference_device_hailo.c for stage codes. */
    if (argc >= 2 && strcmp(argv[1], "stage") == 0) {
        extern int hailo_backend_get_cs_load_stage(void);
        shell_printf("hailo: cs_load_stage=%d\n",
                     hailo_backend_get_cs_load_stage());
        return 0;
    }

    /* `hailo d2h` — drain whatever notifications fw has queued in the
     * D2H mailbox without running an inference. Used to pinpoint when
     * critical events (e.g., HEALTH_MONITOR_CPU_ECC_ERROR) actually
     * fire — at boot, after load, or only on submit. */
    if (argc >= 2 && strcmp(argv[1], "d2h") == 0) {
        hailo_fw_drain_d2h_notifications(8);
        return 0;
    }

#ifdef CONFIG_AI_SCHEDULER
    /* Phase 8: run inference on a previously-loaded handle with
     * zeroed input. Measures end-to-end latency including our
     * cache-clean/submit/MSI-wait/cache-invalidate pipeline.
     *
     * Wrapped in CONFIG_AI_SCHEDULER because it depends on
     * inference_device_find / inference_run / inference_tensor_t,
     * all of which live in the AI scheduler path. Without the
     * scheduler (e.g. `make test` QEMU build) those symbols aren't
     * compiled in and this block would fail to link.
     *
     *   hailo runmodel <handle> [iterations]
     */
    if (argc >= 2 && strcmp(argv[1], "runmodel") == 0) {
        if (argc < 3) {
            shell_puts("usage: hailo runmodel <handle> [iterations]\n");
            return 0;
        }
        /* Small decimal parser — no libc. */
        int32_t handle = 0;
        for (const char *p = argv[2]; *p >= '0' && *p <= '9'; p++) {
            handle = handle * 10 + (*p - '0');
        }
        uint32_t iters = 1;
        if (argc >= 4) {
            iters = 0;
            for (const char *p = argv[3]; *p >= '0' && *p <= '9'; p++) {
                iters = iters * 10u + (uint32_t)(*p - '0');
            }
        }
        if (iters == 0) iters = 1;

        struct inference_device *dev = inference_device_find("hailo-8");
        if (!dev) {
            shell_puts("hailo: no inference device registered\n");
            return 0;
        }

        uint32_t in_bytes = 0, out_bytes = 0;
        int rc = hailo_backend_model_sizes(handle, &in_bytes, &out_bytes);
        if (rc != 0) {
            shell_printf("hailo: invalid handle %d (rc=%d)\n", handle, rc);
            return 0;
        }
        shell_printf("hailo: runmodel handle=%d in=%u out=%u iters=%u\n",
                     handle, in_bytes, out_bytes, iters);

        /* Allocate input+output from PMM to keep the 16 KB task
         * stack safe. Zero-init the input. */
        size_t in_pages  = (in_bytes  + 4095) / 4096;
        size_t out_pages = (out_bytes + 4095) / 4096;
        uint8_t *in_buf  = (uint8_t *)pmm_alloc_pages(in_pages);
        uint8_t *out_buf = (uint8_t *)pmm_alloc_pages(out_pages);
        if (!in_buf || !out_buf) {
            shell_puts("hailo: alloc failed\n");
            if (in_buf)  pmm_free_pages(in_buf,  in_pages);
            if (out_buf) pmm_free_pages(out_buf, out_pages);
            return 0;
        }
        memset(in_buf, 0, in_bytes);

        inference_tensor_t in_t = {
            .data = in_buf, .n_elems = in_bytes,
            .dtype = 3 /* INT8 */, .rank = 1,
            .shape = { (uint16_t)(in_bytes > 0xFFFFu ? 0u : in_bytes), 0, 0, 0 },
        };
        inference_tensor_t out_t = {
            .data = out_buf, .n_elems = out_bytes,
            .dtype = 3, .rank = 1,
            .shape = { (uint16_t)(out_bytes > 0xFFFFu ? 0u : out_bytes), 0, 0, 0 },
        };

        uint64_t min_us = 0xFFFFFFFFFFFFFFFFULL, max_us = 0, sum_us = 0;
        uint32_t ok = 0, fail = 0;
        for (uint32_t i = 0; i < iters; i++) {
            uint64_t t0 = timer_get_count();
            int r = inference_run(dev, handle, &in_t, &out_t);
            uint64_t t1 = timer_get_count();
            uint64_t us = (t1 - t0) * 1000000ULL / timer_get_frequency();
            if (r == 0) {
                ok++;
                if (us < min_us) min_us = us;
                if (us > max_us) max_us = us;
                sum_us += us;
            } else {
                fail++;
                if (fail <= 3) {
                    shell_printf("  iter %u: inference_run rc=%d\n", i, r);
                }
            }
        }
        if (ok > 0) {
            shell_printf("hailo: runmodel ok=%u fail=%u  "
                         "latency min=%lu us avg=%lu us max=%lu us\n",
                         ok, fail, (unsigned long)min_us,
                         (unsigned long)(sum_us / ok),
                         (unsigned long)max_us);
            /* Show first 16 bytes of last output */
            shell_puts("  out[0..15]: ");
            for (uint32_t i = 0; i < 16 && i < out_bytes; i++) {
                shell_printf("%02x ", out_buf[i]);
            }
            shell_puts("\n");
        } else {
            shell_printf("hailo: runmodel all %u iterations failed\n", fail);
        }
        pmm_free_pages(in_buf,  in_pages);
        pmm_free_pages(out_buf, out_pages);
        return 0;
    }
#endif /* CONFIG_AI_SCHEDULER */

    /* Phase 8: dump per-edge-layer details of first 8 entries. Shows
     * direction, pad_index, sys_index, and shape flags for each
     * edge_layer the walker processed. Helps identify which entry
     * IS the output (and why it wasn't picked up as such). */
    if (argc >= 2 && strcmp(argv[1], "edges") == 0) {
        struct hef_edge_debug {
            uint32_t direction;
            uint32_t pad_index;
            uint32_t sys_index;
            uint32_t csi_edge_connection_type;
            uint32_t csi_connected_sys_index;
            uint32_t csi_connected_ctx_sys_index;
            uint8_t  seen_direction : 1;
            uint8_t  seen_pad_index : 1;
            uint8_t  seen_sys_index : 1;
            uint8_t  seen_shape     : 1;
            uint8_t  seen_csi       : 1;
            uint8_t  seen_csi_connected_sys_index : 1;
            uint8_t  seen_csi_connected_ctx_sys_index : 1;
        };
        extern struct hef_edge_debug hef_edge_debug_slots[];
        extern uint32_t hef_edge_debug_count;
        shell_printf("hailo: edge_debug count=%u\n", hef_edge_debug_count);
        for (uint32_t i = 0; i < hef_edge_debug_count; i++) {
            const struct hef_edge_debug *d = &hef_edge_debug_slots[i];
            const char *conn = "?";
            if (d->seen_csi) {
                switch (d->csi_edge_connection_type) {
                case 0: conn = "BOUNDARY"; break;
                case 1: conn = "INTERMED"; break;
                case 2: conn = "DDR";      break;
                case 3: conn = "CACHE";    break;
                default: conn = "??";      break;
                }
            } else {
                conn = "nocsi";
            }
            shell_printf("  [%u] dir=%s pad=%s(%u) sys=%s(%u) shape=%s "
                         "csi=%s conn_sys=%s(%u) cctx_sys=%s(%u)\n",
                         i,
                         d->seen_direction
                            ? (d->direction == 1 ? "D2H" : "H2D")
                            : "unset",
                         d->seen_pad_index ? "yes" : "no ",
                         d->pad_index,
                         d->seen_sys_index ? "yes" : "no ",
                         d->sys_index,
                         d->seen_shape ? "yes" : "no",
                         conn,
                         d->seen_csi_connected_sys_index ? "yes" : "no ",
                         d->csi_connected_sys_index,
                         d->seen_csi_connected_ctx_sys_index ? "yes" : "no ",
                         d->csi_connected_ctx_sys_index);
        }
        return 0;
    }

    /* Phase 8: dump edge_layer walker tallies. Populated by
     * decode_edge_layer_cb in hef_parser.c — shows how many edge
     * layers the walker saw, how many had no pad_key (skipped),
     * and how many landed as input vs output pads. */
    if (argc >= 2 && strcmp(argv[1], "edgeinfo") == 0) {
        extern uint32_t hef_edge_layer_calls;
        extern uint32_t hef_edge_layer_no_key;
        extern uint32_t hef_edge_layer_kept_h2d;
        extern uint32_t hef_edge_layer_kept_d2h;
        extern uint32_t hef_edge_layer_deduped;
        shell_printf("hailo: edge_layer calls=%u no_key=%u "
                     "kept_input=%u kept_output=%u deduped=%u\n",
                     hef_edge_layer_calls,
                     hef_edge_layer_no_key,
                     hef_edge_layer_kept_h2d,
                     hef_edge_layer_kept_d2h,
                     hef_edge_layer_deduped);
        return 0;
    }

    /* Phase 8 #253 (2026-04-25): on-demand dump of the fw CORE + APP
     * debug-log rings. Useful immediately post-boot (before any RPCs)
     * to see what fw reports about its own init state — historically
     * we only saw these after a runmodel timeout, which mixes init
     * traffic with the failure path. */
    if (argc >= 2 && strcmp(argv[1], "fwlog") == 0) {
        hailo_fw_dump_logs();
        return 0;
    }

    /* Hex variant: dump raw bytes 16/line so a Hailo support engineer
     * can decode the (opaque-binary) fwlog format. Default cap 256 B
     * (HailoRT writes ~600 B post-boot to APP CPU, but printing 8 KB
     * over UART blocks for ~8 s on the Pi 5 PL011 we use). Pass
     * `hailo fwloghex 0` to dump the full ring. */
    if (argc >= 2 && strcmp(argv[1], "fwloghex") == 0) {
        uint32_t cap = 256;
        if (argc >= 3) {
            uint32_t v = 0;
            for (const char *p = argv[2]; *p >= '0' && *p <= '9'; p++) {
                v = v * 10u + (uint32_t)(*p - '0');
            }
            cap = v;  /* 0 → full ring */
        }
        hailo_fw_dump_logs_hex(cap);
        return 0;
    }

    /* Phase 8 #253 CPU_ECC investigation (2026-04-25). Run RUN_BIST_TEST
     * (opcode 0x3C) and dump the response payload. Usage:
     *   hailo bist            — top test, no bypass (test all whitelist
     *                            blocks: bits 2..5 = L4 banks)
     *   hailo bist <bypass>   — top test, hex bypass mask
     *
     * BIST is destructive — fw scribbles patterns into memory then
     * reads them back. Always reboot the chip after running BIST
     * before doing anything else. */
    if (argc >= 2 && strcmp(argv[1], "bist") == 0) {
        uint32_t top_bypass = 0;
        if (argc >= 3) {
            if (parse_hex_u32(argv[2], &top_bypass) != 0) {
                shell_printf("hailo: bist: bad hex bypass '%s'\n", argv[2]);
                return 0;
            }
        }
        uint8_t  body[256];
        uint32_t body_len = 0;
        shell_printf("hailo: bist top=true bypass=0x%08x cluster=0 "
                     "cluster_bypass=(0,0)\n", top_bypass);
        int rc = hailo_control_run_bist_test(/*is_top_test=*/true,
                                             top_bypass,
                                             /*cluster_index=*/0,
                                             /*cluster_bypass_0=*/0,
                                             /*cluster_bypass_1=*/0,
                                             body, sizeof(body),
                                             &body_len);
        shell_printf("hailo: bist rc=%d body_len=%u\n", rc, body_len);
        if (rc == HAILO_OK && body_len > 0) {
            uint32_t cap = body_len > 64u ? 64u : body_len;
            shell_printf("[bist] body[0..%u]:", cap);
            for (uint32_t i = 0; i < cap; i++) {
                if ((i & 0xf) == 0) shell_printf("\n  [%02x]", i);
                shell_printf(" %02x", body[i]);
            }
            shell_puts("\n");
        }
#ifdef HAILO_WIRE_DEBUG
        shell_puts("[bisect] post BIST:\n");
        hailo_fw_drain_d2h_notifications(4);
#endif
        return 0;
    }

    /* Phase 8: dump last firmware-reject reason. Populated by
     * control_check_response_header whenever a SET_CONTEXT_INFO /
     * CHANGE_STATUS / similar RPC returns a non-zero major_status.
     * Cleared on kernel boot. */
    if (argc >= 2 && strcmp(argv[1], "last_err") == 0) {
        extern volatile uint32_t hailo_control_last_err_major;
        extern volatile uint32_t hailo_control_last_err_minor;
        extern volatile uint32_t hailo_control_last_err_opcode;
        shell_printf("hailo: last_err major=0x%08x minor=0x%08x "
                     "opcode_echo=0x%08x\n",
                     hailo_control_last_err_major,
                     hailo_control_last_err_minor,
                     hailo_control_last_err_opcode);
        return 0;
    }

    /* Default: one-line status. */
    shell_printf("hailo: state=%s\n", hailo_state_str(hailo_get_state()));
    return 0;
}

static const shell_cmd_t hailo_cmd = {
    .name     = "hailo",
    .handler  = cmd_hailo,
    .help     = "Hailo NPU control (hailo, probe, boot, load <path>, fw, peek, poke, cfgstream <in|out> <ch>, cfgdump, ctxsmoke [min|out|in|full])",
    .mutates  = true,   /* probe/fw mutate driver state; status is a whole-command tag */
    .category = SHELL_CAT_HARDWARE,
};

void hailo_register_shell_commands(void)
{
    shell_register_command(&hailo_cmd);
}
