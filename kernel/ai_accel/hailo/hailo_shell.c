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
#include "ai_policy_hailo.h"
#include "ai_types.h"
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
        if (*s >= '0' && *s <= '9')      d = (uint32_t)(*s - '0');
        else if (*s >= 'a' && *s <= 'f') d = (uint32_t)(*s - 'a' + 10);
        else if (*s >= 'A' && *s <= 'F') d = (uint32_t)(*s - 'A' + 10);
        else return -1;
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
                          meta.hw_arch == HEF_HW_ARCH_HAILO8L  ? "hailo8l" :
                          meta.hw_arch == HEF_HW_ARCH_HAILO15H ? "hailo15h":
                          meta.hw_arch == HEF_HW_ARCH_HAILO15M ? "hailo15m":
                          meta.hw_arch == HEF_HW_ARCH_HAILO10H ? "hailo10h":
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
            /* Hand the full HEF blob (header + proto) to the Hailo
             * inference_device backend for load_model. Allocate a
             * contiguous buffer sized to the whole file, populate
             * header + body (body is already resident), and release
             * it immediately after load_model returns — the backend
             * does not keep a pointer. */
            size_t total = (size_t)outer.proto_offset + outer.proto_size;
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

                struct inference_device *dev = inference_device_find("hailo-8");
                if (!dev) {
                    shell_puts("hailo: sched: 'hailo-8' device not registered\n");
                } else {
                    inference_model_handle_t h = INF_INVALID_HANDLE;
                    int lrc = inference_load_model(dev, full, total, &h);
                    if (lrc != INF_OK) {
                        shell_printf("hailo: sched: load_model failed (%d)\n", lrc);
                    } else {
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
                                AI_STATE_DIM, AI_SCHED_N_ACTIONS);
                        }
                        if (rc_quant == 0) {
                            shell_printf("hailo: sched: model loaded "
                                         "(handle=%d), ai_policy_hailo armed "
                                         "with HEF quant\n", (int)h);
                        } else {
                            ai_policy_hailo_set_model_placeholder(
                                h, AI_STATE_DIM, AI_SCHED_N_ACTIONS);
                            shell_printf("hailo: sched: model loaded "
                                         "(handle=%d), ai_policy_hailo armed "
                                         "with placeholder quant "
                                         "(HEF quant_info %s)\n",
                                         (int)h,
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
        if (strcmp(argv[2], "in") == 0)       is_input = true;
        else if (strcmp(argv[2], "out") == 0) is_input = false;
        else {
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

        /* #180 hypothesis test: allocate boundary input/output DMA
         * tensors + desc lists, populate synthetic pads with
         * has_stream_info=true. This forces translate_activation to
         * emit OPEN_BOUNDARY_INPUT_CHANNEL + OPEN_BOUNDARY_OUTPUT_
         * CHANNEL alongside BURST_CREDITS_TASK_RESET, producing an
         * ACTIVATION richer than the 8-byte credits-only stream
         * firmware rejected the next CORE-CPU RPC after. If this
         * unblocks BATCH_SWITCHING, #180's "insufficient ACTIVATION
         * content" hypothesis is confirmed. */
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
            brc = hailo_vdma_desc_list_alloc(2, bnd_page, false, &bnd_in_list);
        }
        if (brc == HAILO_OK) brc = hailo_vdma_desc_list_alloc(2, bnd_page, false, &bnd_out_list);
        if (brc == HAILO_OK) {
            (void)hailo_vdma_program_buffer(&bnd_in_list,  0, bnd_in_tensor.iova,  bnd_bytes, 0);
            (void)hailo_vdma_program_buffer(&bnd_out_list, 0, bnd_out_tensor.iova, bnd_bytes, 0);
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
        info.pads[0].has_stream_info       = true;
        info.pads[0].sys_index             = 1;
        info.pads[0].core_bytes_per_buffer = bnd_bytes;
        info.pads[1].is_input              = false;
        info.pads[1].has_stream_info       = true;
        info.pads[1].sys_index             = 2;
        info.pads[1].core_bytes_per_buffer = bnd_bytes;

        struct hailo_cs_translate_cfg tcfg = {
            .config_vdma_channel              = 0x01,   /* engine 0 channel 1 */
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
        shell_puts("  [1/6] CHANGE_CONTEXT_SWITCH_STATUS(RESET)...\n");
        int rc = hailo_control_change_context_switch_status(
            HAILO_CS_STATE_RESET,
            HAILO_CS_IGNORE_APPLICATION_INDEX,
            /*batch_size=*/0, /*batch_count=*/0);
        shell_printf("        rc=%d\n", rc);

        shell_puts("  [2/6] SET_NETWORK_GROUP_HEADER...\n");
        rc = hailo_control_set_network_group_header(&hdr);
        shell_printf("        rc=%d\n", rc);

        shell_printf("  [3/6] SET_CONTEXT_INFO(ACTIVATION, %u bytes)\n",
                     (unsigned)bufs.activation_len);
        rc = hailo_control_set_context_info(HAILO_CS_CONTEXT_TYPE_ACTIVATION,
                                            bufs.activation,
                                            (uint32_t)bufs.activation_len);
        shell_printf("        rc=%d\n", rc);

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

        shell_printf("  [4/6] SET_CONTEXT_INFO(BATCH_SWITCHING, %u bytes)\n",
                     (unsigned)bufs.batch_switching_len);
        rc = hailo_control_set_context_info(HAILO_CS_CONTEXT_TYPE_BATCH_SWITCHING,
                                            bufs.batch_switching,
                                            (uint32_t)bufs.batch_switching_len);
        shell_printf("        rc=%d\n", rc);

        shell_printf("  [5/6] SET_CONTEXT_INFO(PRELIMINARY, %u bytes)\n",
                     (unsigned)bufs.preliminary_len);
        shell_printf("        CCW buffer iova=0x%lx\n",
                     (unsigned long)ccw_list.iova);
        rc = hailo_control_set_context_info(HAILO_CS_CONTEXT_TYPE_PRELIMINARY,
                                            bufs.preliminary,
                                            (uint32_t)bufs.preliminary_len);
        shell_printf("        rc=%d\n", rc);

        shell_printf("  [6/6] SET_CONTEXT_INFO(DYNAMIC, %u bytes)\n",
                     (unsigned)bufs.dynamic_len);
        rc = hailo_control_set_context_info(HAILO_CS_CONTEXT_TYPE_DYNAMIC,
                                            bufs.dynamic,
                                            (uint32_t)bufs.dynamic_len);
        shell_printf("        rc=%d\n", rc);

        hailo_vdma_desc_list_free(&bnd_out_list);
        hailo_vdma_desc_list_free(&bnd_in_list);
        hailo_tensor_free(&bnd_out_tensor);
        hailo_tensor_free(&bnd_in_tensor);
        hailo_vdma_desc_list_free(&ccw_list);
        hailo_tensor_free(&ccw_tensor);

        shell_puts("hailo: ctxsmoke done\n");
        return 0;
    }

    /* Default: one-line status. */
    shell_printf("hailo: state=%s\n", hailo_state_str(hailo_get_state()));
    return 0;
}

static const shell_cmd_t hailo_cmd = {
    .name    = "hailo",
    .handler = cmd_hailo,
    .help    = "Hailo NPU control (hailo, probe, boot, load <path>, fw, peek, poke, cfgstream <in|out> <ch>, cfgdump, ctxsmoke)",
    .mutates = true,   /* probe/fw mutate driver state; status is a whole-command tag */
};

void hailo_register_shell_commands(void)
{
    shell_register_command(&hailo_cmd);
}
