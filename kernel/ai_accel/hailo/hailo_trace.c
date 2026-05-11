/*
 * hailo_trace.c — implementation. See hailo_trace.h for the public
 * contract.
 *
 * Three concerns live here:
 *   1. The mask globals and phase tracker, plus the cmdline / shell
 *      parsers that arm them (free-form string in → bit mask out).
 *   2. The pretty-printer for the `hailo trace` shell command's
 *      no-arg status form.
 *   3. The nine `hailo_trace_emit_*` helpers — one per mechanism per
 *      direction — that format and emit the actual transaction lines.
 *
 * Output format is shared with the Linux trace_mmio patch where the
 * mechanisms overlap (MMIO + PCI cfg) so cross-side captures diff
 * cleanly. Lines are prefixed with `[trc] phase=<P> mech=<M>` so a
 * grep over a serial capture can isolate by either dimension.
 *
 * No locks. Trace emission happens from arbitrary contexts
 * (interrupt handlers, RPC drains, idle paths). The mask globals are
 * plain `uint32_t` reads — a torn read on an unaligned platform
 * could in principle drop one trace line, which is acceptable;
 * tracing is observability, not correctness. ARM64 32-bit aligned
 * loads are atomic so the question is moot in practice.
 */

#include "hailo_trace.h"

#include "debug.h"
#include "uart.h"

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

/* -------------------------------------------------------------------------- */
/* Build-time defaults                                                         */
/* -------------------------------------------------------------------------- */

#ifndef HAILO_TRACE_PHASE_DEFAULT
#define HAILO_TRACE_PHASE_DEFAULT 0u
#endif
#ifndef HAILO_TRACE_MECH_DEFAULT
#define HAILO_TRACE_MECH_DEFAULT 0u
#endif

/* -------------------------------------------------------------------------- */
/* Globals                                                                     */
/* -------------------------------------------------------------------------- */

volatile uint32_t hailo_trace_phase_mask = (uint32_t)HAILO_TRACE_PHASE_DEFAULT;
volatile uint32_t hailo_trace_mech_mask  = (uint32_t)HAILO_TRACE_MECH_DEFAULT;
volatile enum hailo_trace_phase hailo_trace_current_phase = HAILO_TRACE_PHASE_NONE;

/* -------------------------------------------------------------------------- */
/* Name lookup tables                                                          */
/* -------------------------------------------------------------------------- */

struct name_bit {
    const char *name;
    uint32_t    bit;
};

/* Multiple synonyms map to the same bit so users can type the form
 * that comes to mind. The first synonym in each group is the
 * canonical form printed by `hailo_trace_print_state`. */
static const struct name_bit phase_names[] = {
    { "link",       HAILO_TRACE_PHASE_LINKUP_BIT     },
    { "linkup",     HAILO_TRACE_PHASE_LINKUP_BIT     },
    { "fw_boot",    HAILO_TRACE_PHASE_FW_BOOT_BIT    },
    { "fwboot",     HAILO_TRACE_PHASE_FW_BOOT_BIT    },
    { "postboot",   HAILO_TRACE_PHASE_POSTBOOT_BIT   },
    { "post_boot",  HAILO_TRACE_PHASE_POSTBOOT_BIT   },
    { "model_load", HAILO_TRACE_PHASE_MODEL_LOAD_BIT },
    { "modelload",  HAILO_TRACE_PHASE_MODEL_LOAD_BIT },
    { "load",       HAILO_TRACE_PHASE_MODEL_LOAD_BIT },
    { "inference",  HAILO_TRACE_PHASE_INFERENCE_BIT  },
    { "infer",      HAILO_TRACE_PHASE_INFERENCE_BIT  },
    { "teardown",   HAILO_TRACE_PHASE_TEARDOWN_BIT   },
    { NULL, 0 }
};

static const struct name_bit mech_names[] = {
    { "mmio",       HAILO_TRACE_MECH_MMIO      },
    { "pci",        HAILO_TRACE_MECH_PCI_CFG   },
    { "pci_cfg",    HAILO_TRACE_MECH_PCI_CFG   },
    { "cfg",        HAILO_TRACE_MECH_PCI_CFG   },
    { "rpc",        HAILO_TRACE_MECH_RPC       },
    { "irq",        HAILO_TRACE_MECH_IRQ       },
    { "dma",        HAILO_TRACE_MECH_DMA       },
    { "busy",       HAILO_TRACE_MECH_BUSY_WAIT },
    { "busy_wait",  HAILO_TRACE_MECH_BUSY_WAIT },
    { NULL, 0 }
};

/* Returns the canonical name of a phase enum value (for marker lines
 * + status print). Independent of the mask state. */
static const char *phase_name(enum hailo_trace_phase p)
{
    switch (p) {
    case HAILO_TRACE_PHASE_NONE:       return "none";
    case HAILO_TRACE_PHASE_LINKUP:     return "link";
    case HAILO_TRACE_PHASE_FW_BOOT:    return "fw_boot";
    case HAILO_TRACE_PHASE_POSTBOOT:   return "postboot";
    case HAILO_TRACE_PHASE_MODEL_LOAD: return "model_load";
    case HAILO_TRACE_PHASE_INFERENCE:  return "inference";
    case HAILO_TRACE_PHASE_TEARDOWN:   return "teardown";
    }
    return "?";
}

/* -------------------------------------------------------------------------- */
/* String helpers (no libc)                                                    */
/* -------------------------------------------------------------------------- */

static bool ch_is_token(char c)
{
    /* Inside a comma-separated list: anything that's not a separator,
     * not whitespace, and not a terminator. We accept `_`, alphanum,
     * and `:` (the last for future expansion if we want phase:mech
     * compound tokens). */
    if (c == ',' || c == ' ' || c == '\t' || c == '\0') return false;
    if (c == '\n' || c == '\r') return false;
    return true;
}

static bool tok_equals(const char *tok, size_t len, const char *name)
{
    for (size_t i = 0; i < len; i++) {
        if (name[i] == '\0') return false;
        char a = tok[i];
        char b = name[i];
        if (a >= 'A' && a <= 'Z') a = (char)(a - 'A' + 'a');
        if (b >= 'A' && b <= 'Z') b = (char)(b - 'A' + 'a');
        if (a != b) return false;
    }
    return name[len] == '\0';
}

/* Walk a comma-separated list, OR-ing in each recognized bit. Returns
 * 0 on success, -1 if any token was unrecognized (other tokens still
 * applied — partial success rather than all-or-nothing, so a typo
 * doesn't silently disarm everything). Special-cases `all` (= ALL
 * mask) and `off` (= force 0, overrides any other tokens). */
static int parse_list(const char *list, const struct name_bit *tbl,
                      uint32_t all_mask, uint32_t *out_mask)
{
    if (!list) return 0;
    uint32_t armed = 0;
    bool saw_off = false;
    bool error = false;

    const char *p = list;
    while (*p != '\0') {
        while (*p == ',' || *p == ' ' || *p == '\t') p++;
        if (*p == '\0') break;
        const char *start = p;
        while (ch_is_token(*p)) p++;
        size_t len = (size_t)(p - start);
        if (len == 0) continue;

        bool matched = false;
        if (tok_equals(start, len, "all")) {
            armed |= all_mask;
            matched = true;
        } else if (tok_equals(start, len, "off") ||
                   tok_equals(start, len, "none")) {
            saw_off = true;
            matched = true;
        } else {
            for (const struct name_bit *e = tbl; e->name != NULL; e++) {
                if (tok_equals(start, len, e->name)) {
                    armed |= e->bit;
                    matched = true;
                    break;
                }
            }
        }
        if (!matched) {
            WARN("hailo_trace: unknown token in list (skipped, %u chars)",
                 (unsigned)len);
            error = true;
        }
    }

    *out_mask = saw_off ? 0u : armed;
    return error ? -1 : 0;
}

/* -------------------------------------------------------------------------- */
/* Cmdline + shell entry points                                                */
/* -------------------------------------------------------------------------- */

/* Look for a `key=value` token in the cmdline string. Returns a
 * pointer to the start of `value` (one char past the `=`) and the
 * length of the value (up to next whitespace), or NULL if `key` is
 * absent. Whitespace-bounded so multi-token cmdlines parse correctly. */
static const char *find_kv(const char *cmdline, const char *key,
                           size_t *out_val_len)
{
    if (!cmdline || !key) return NULL;
    size_t klen = 0;
    while (key[klen] != '\0') klen++;

    const char *p = cmdline;
    while (*p != '\0') {
        /* Tokenize on whitespace. */
        while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') p++;
        if (*p == '\0') break;
        const char *tok = p;
        while (*p != '\0' && *p != ' ' && *p != '\t' &&
               *p != '\n' && *p != '\r') p++;
        size_t tlen = (size_t)(p - tok);

        /* Match: tok starts with `key=`. */
        if (tlen > klen && tok[klen] == '=') {
            bool eq = true;
            for (size_t i = 0; i < klen; i++) {
                if (tok[i] != key[i]) { eq = false; break; }
            }
            if (eq) {
                *out_val_len = tlen - klen - 1;
                return tok + klen + 1;
            }
        }
    }
    return NULL;
}

/* Bounded local copy so the value can be passed to parse_list (which
 * walks the value as a C string). Bootargs values are short; 128
 * bytes per key is comfortably above what we'd ever type. */
#define HAILO_TRACE_KV_MAX 128

static void copy_bounded(char *dst, const char *src, size_t src_len, size_t cap)
{
    if (cap == 0) return;                 /* defensive: cap-1 would underflow */
    size_t n = src_len < cap - 1 ? src_len : cap - 1;
    for (size_t i = 0; i < n; i++) dst[i] = src[i];
    dst[n] = '\0';
}

void hailo_trace_cmdline_parse(const char *bootargs)
{
    if (!bootargs || bootargs[0] == '\0') return;

    size_t vlen = 0;
    const char *v;

    v = find_kv(bootargs, "hailo_trace.phase", &vlen);
    if (v) {
        char buf[HAILO_TRACE_KV_MAX];
        copy_bounded(buf, v, vlen, sizeof(buf));
        uint32_t m = 0;
        (void)parse_list(buf, phase_names, HAILO_TRACE_PHASE_ALL, &m);
        hailo_trace_phase_mask |= m;  /* OR-merge with build default */
    }

    v = find_kv(bootargs, "hailo_trace.mech", &vlen);
    if (v) {
        char buf[HAILO_TRACE_KV_MAX];
        copy_bounded(buf, v, vlen, sizeof(buf));
        uint32_t m = 0;
        (void)parse_list(buf, mech_names, HAILO_TRACE_MECH_ALL, &m);
        hailo_trace_mech_mask |= m;
    }

    if (hailo_trace_phase_mask != 0u || hailo_trace_mech_mask != 0u) {
        INFO("hailo_trace: cmdline armed phase=0x%02x mech=0x%02x",
             (unsigned)hailo_trace_phase_mask,
             (unsigned)hailo_trace_mech_mask);
    }
}

int hailo_trace_set_phase_mask(const char *names)
{
    uint32_t m = 0;
    int rc = parse_list(names, phase_names, HAILO_TRACE_PHASE_ALL, &m);
    hailo_trace_phase_mask = m;
    return rc;
}

int hailo_trace_set_mech_mask(const char *names)
{
    uint32_t m = 0;
    int rc = parse_list(names, mech_names, HAILO_TRACE_MECH_ALL, &m);
    hailo_trace_mech_mask = m;
    return rc;
}

void hailo_trace_reset(void)
{
    hailo_trace_phase_mask = 0u;
    hailo_trace_mech_mask  = 0u;
}

/* -------------------------------------------------------------------------- */
/* Phase tracker                                                               */
/* -------------------------------------------------------------------------- */

void hailo_trace_set_phase(enum hailo_trace_phase next)
{
    enum hailo_trace_phase prev = hailo_trace_current_phase;
    if (prev == next) return;

    /* Marker line emits if either old or new is armed — keeps the
     * transition locatable in capture even when only one side has
     * tracing on. */
    const uint32_t prev_bit = hailo_trace_phase_bit(prev);
    const uint32_t next_bit = hailo_trace_phase_bit(next);
    bool emit = (hailo_trace_phase_mask & (prev_bit | next_bit)) != 0u;

    hailo_trace_current_phase = next;

    if (emit) {
        uart_printf("[trc] phase=%s→%s\r\n",
                    phase_name(prev), phase_name(next));
    }
}

/* -------------------------------------------------------------------------- */
/* Status print (`hailo trace` with no args)                                   */
/* -------------------------------------------------------------------------- */

static void print_armed_names(const struct name_bit *tbl, uint32_t mask)
{
    if (mask == 0u) { uart_printf("(none)"); return; }
    /* Track which bits we've already printed so synonyms in the table
     * don't double-print. */
    uint32_t seen = 0;
    bool first = true;
    for (const struct name_bit *p = tbl; p->name != NULL; p++) {
        if ((mask & p->bit) == 0u) continue;
        if ((seen & p->bit) != 0u) continue;
        uart_printf("%s%s", first ? "" : ",", p->name);
        seen |= p->bit;
        first = false;
    }
}

void hailo_trace_print_state(void)
{
    uart_printf("hailo_trace: phase_mask=0x%02x [",
                (unsigned)hailo_trace_phase_mask);
    print_armed_names(phase_names, hailo_trace_phase_mask);
    uart_printf("] mech_mask=0x%02x [",
                (unsigned)hailo_trace_mech_mask);
    print_armed_names(mech_names, hailo_trace_mech_mask);
    uart_printf("] current_phase=%s\r\n",
                phase_name(hailo_trace_current_phase));
}

/* -------------------------------------------------------------------------- */
/* Emit helpers                                                                */
/* -------------------------------------------------------------------------- */
/*
 * Caller has already gated on hailo_trace_active(MECH). Each helper
 * is straight-line and reads the current phase once for the line tag.
 */

#define TRC_PREFIX_FMT  "[trc] phase=%-10s mech=%-4s "

void hailo_trace_emit_mmio_w(int bar, uint32_t off, uint32_t val)
{
    uart_printf(TRC_PREFIX_FMT "WR32 bar=%d off=0x%04x val=0x%08x\r\n",
                phase_name(hailo_trace_current_phase), "MMIO",
                bar, (unsigned)off, (unsigned)val);
}

void hailo_trace_emit_mmio_r(int bar, uint32_t off, uint32_t val)
{
    uart_printf(TRC_PREFIX_FMT "RD32 bar=%d off=0x%04x val=0x%08x\r\n",
                phase_name(hailo_trace_current_phase), "MMIO",
                bar, (unsigned)off, (unsigned)val);
}

void hailo_trace_emit_cfg_w(uint16_t bdf, uint16_t off, unsigned width,
                            uint32_t val)
{
    const unsigned bus  = (bdf >> 8) & 0xFFu;
    const unsigned dev  = (bdf >> 3) & 0x1Fu;
    const unsigned func =  bdf       & 0x07u;
    uart_printf(TRC_PREFIX_FMT
                "WR%u  bdf=%02x:%02x.%x off=0x%02x val=0x%08x\r\n",
                phase_name(hailo_trace_current_phase), "PCI",
                width, bus, dev, func, (unsigned)off, (unsigned)val);
}

void hailo_trace_emit_cfg_r(uint16_t bdf, uint16_t off, unsigned width,
                            uint32_t val)
{
    const unsigned bus  = (bdf >> 8) & 0xFFu;
    const unsigned dev  = (bdf >> 3) & 0x1Fu;
    const unsigned func =  bdf       & 0x07u;
    uart_printf(TRC_PREFIX_FMT
                "RD%u  bdf=%02x:%02x.%x off=0x%02x val=0x%08x\r\n",
                phase_name(hailo_trace_current_phase), "PCI",
                width, bus, dev, func, (unsigned)off, (unsigned)val);
}

void hailo_trace_emit_rpc_tx(uint8_t op, uint32_t len, uint8_t cpu,
                             const uint8_t md5_first8[8])
{
    const char *cpu_s = (cpu == 0x2u) ? "CORE" : (cpu == 0x1u) ? "APP" : "?";
    if (md5_first8) {
        uart_printf(TRC_PREFIX_FMT
                    "tx op=0x%02x len=%u cpu=%s "
                    "md5=%02x%02x%02x%02x%02x%02x%02x%02x\r\n",
                    phase_name(hailo_trace_current_phase), "RPC",
                    (unsigned)op, (unsigned)len, cpu_s,
                    md5_first8[0], md5_first8[1], md5_first8[2], md5_first8[3],
                    md5_first8[4], md5_first8[5], md5_first8[6], md5_first8[7]);
    } else {
        uart_printf(TRC_PREFIX_FMT
                    "tx op=0x%02x len=%u cpu=%s md5=-\r\n",
                    phase_name(hailo_trace_current_phase), "RPC",
                    (unsigned)op, (unsigned)len, cpu_s);
    }
}

void hailo_trace_emit_rpc_rx(uint8_t status_major, uint8_t status_minor,
                             uint32_t len)
{
    uart_printf(TRC_PREFIX_FMT "rx status=%u:%u len=%u\r\n",
                phase_name(hailo_trace_current_phase), "RPC",
                (unsigned)status_major, (unsigned)status_minor,
                (unsigned)len);
}

void hailo_trace_emit_irq(uint32_t spi, uint32_t istatus,
                          uint32_t per_src, uint32_t per_dst)
{
    uart_printf(TRC_PREFIX_FMT
                "spi=%u istatus=0x%08x src=0x%08x dst=0x%08x\r\n",
                phase_name(hailo_trace_current_phase), "IRQ",
                (unsigned)spi, (unsigned)istatus,
                (unsigned)per_src, (unsigned)per_dst);
}

void hailo_trace_emit_dma(const char *event, int ch,
                          uint32_t a, uint32_t b, uint32_t c, uint32_t d)
{
    uart_printf(TRC_PREFIX_FMT
                "%s ch=%d a=0x%08x b=0x%08x c=0x%08x d=0x%08x\r\n",
                phase_name(hailo_trace_current_phase), "DMA",
                event ? event : "?", ch,
                (unsigned)a, (unsigned)b, (unsigned)c, (unsigned)d);
}

void hailo_trace_emit_busy_wait(uint32_t us, const char *tag)
{
    uart_printf(TRC_PREFIX_FMT "udelay us=%u tag=%s\r\n",
                phase_name(hailo_trace_current_phase), "BUSY",
                (unsigned)us, tag ? tag : "-");
}
