/*
 * hailo_trace.h — runtime-gated boundary trace for the Hailo path.
 *
 * Emits one line per transaction (MMIO, PCI config-space, RPC, IRQ,
 * DMA, busy-wait) when both the current phase bit and the call site's
 * mechanism bit are armed. Format matches the Linux hailo_pci
 * `trace_mmio` patch so captures diff cleanly across both sides.
 *
 * Defaults to silent (both masks 0). Three control surfaces:
 *
 *   1. Build-time defaults via `-DHAILO_TRACE_PHASE_DEFAULT=...` and
 *      `-DHAILO_TRACE_MECH_DEFAULT=...` — masks come up non-zero at
 *      kernel start. Use this when you need Phase 1 (link train)
 *      traces, which run before any shell exists.
 *
 *   2. Cmdline.txt → DTB /chosen/bootargs — firmware delivers the
 *      property before kernel_main even runs; parsed once early in
 *      boot via `hailo_trace_cmdline_parse()`. Format:
 *          hailo_trace.phase=link,fw_boot,postboot
 *          hailo_trace.mech=mmio,pci
 *      Accepts `all` and `off` shortcuts.
 *
 *   3. `hailo trace ...` shell subcommand — overrides at any point
 *      post-shell. Same name tables as the cmdline parser.
 *
 * The hot-path guard `hailo_trace_active()` is one load + one test +
 * one branch when the masks are clear. Trace call sites compile to
 * the same code regardless of whether tracing is ever enabled at
 * runtime; the cost when disabled is dominated by the branch
 * predictor's "always not taken" path.
 */

#ifndef HAILO_TRACE_H
#define HAILO_TRACE_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

/* -------------------------------------------------------------------------- */
/* Phase bits — driver lifecycle phases as defined in docs/hailo-lifecycle.md */
/* -------------------------------------------------------------------------- */

enum hailo_trace_phase {
    HAILO_TRACE_PHASE_NONE       = 0,
    HAILO_TRACE_PHASE_LINKUP     = 1,  /* PCIe link train + BAR mapping */
    HAILO_TRACE_PHASE_FW_BOOT    = 2,  /* fw upload + trigger + magic poll */
    HAILO_TRACE_PHASE_POSTBOOT   = 3,  /* IRQ disarm + settle + RPC handshake + PMCSR */
    HAILO_TRACE_PHASE_MODEL_LOAD = 4,  /* CONFIG_STREAM, CCW upload, CS handshake */
    HAILO_TRACE_PHASE_INFERENCE  = 5,  /* num_avail bumps, DMA, completions */
    HAILO_TRACE_PHASE_TEARDOWN   = 6,  /* DRIVER_SHUTDOWN signal */
};

#define HAILO_TRACE_PHASE_BIT(p)        (1u << ((p) - 1))

#define HAILO_TRACE_PHASE_LINKUP_BIT     HAILO_TRACE_PHASE_BIT(HAILO_TRACE_PHASE_LINKUP)
#define HAILO_TRACE_PHASE_FW_BOOT_BIT    HAILO_TRACE_PHASE_BIT(HAILO_TRACE_PHASE_FW_BOOT)
#define HAILO_TRACE_PHASE_POSTBOOT_BIT   HAILO_TRACE_PHASE_BIT(HAILO_TRACE_PHASE_POSTBOOT)
#define HAILO_TRACE_PHASE_MODEL_LOAD_BIT HAILO_TRACE_PHASE_BIT(HAILO_TRACE_PHASE_MODEL_LOAD)
#define HAILO_TRACE_PHASE_INFERENCE_BIT  HAILO_TRACE_PHASE_BIT(HAILO_TRACE_PHASE_INFERENCE)
#define HAILO_TRACE_PHASE_TEARDOWN_BIT   HAILO_TRACE_PHASE_BIT(HAILO_TRACE_PHASE_TEARDOWN)
#define HAILO_TRACE_PHASE_ALL            0x3Fu

/* -------------------------------------------------------------------------- */
/* Mechanism bits — call-site-fixed; compiler folds into the guard test       */
/* -------------------------------------------------------------------------- */

#define HAILO_TRACE_MECH_MMIO      (1u << 0)   /* BAR0/2/4 read/write */
#define HAILO_TRACE_MECH_PCI_CFG   (1u << 1)   /* ECAM config space read/write */
#define HAILO_TRACE_MECH_RPC       (1u << 2)   /* FW_CONTROL wire bytes (md5+len+body) */
#define HAILO_TRACE_MECH_IRQ       (1u << 3)   /* MIP1 → MSI → ISTATUS handler entry, plus polled drains */
#define HAILO_TRACE_MECH_DMA       (1u << 4)   /* descriptor list arm/start, num_avail */
#define HAILO_TRACE_MECH_BUSY_WAIT (1u << 5)   /* udelay call sites */
#define HAILO_TRACE_MECH_ALL       0x3Fu

/* IRQ trace note: emits fire from BOTH the MSI handler tail AND the
 * polled drain in hailo_control_drain_pending_irqs. The driver's
 * fast-fw RPC path uses cooperative polling that clears ISTATUS
 * before the hardware-delivered MSI handler runs, so on fast paths
 * the trace lines come from the polled drain (spi=0); on slow paths
 * (notifications, ECC events, async fw signals) they come from the
 * handler. Both paths emit the same line shape with the same
 * ISTATUS/SRC/DEST values, so consumers can treat them
 * interchangeably — spi=0 just disambiguates the source. */

/* -------------------------------------------------------------------------- */
/* Globals + hot-path guard                                                    */
/* -------------------------------------------------------------------------- */

extern uint32_t                hailo_trace_phase_mask;
extern uint32_t                hailo_trace_mech_mask;
extern enum hailo_trace_phase  hailo_trace_current_phase;

/* Returns true when both:
 *   (1) the current phase bit is armed in the phase mask, AND
 *   (2) the call site's mechanism bit is armed in the mech mask.
 * Inlines to a single load + and + and + branch in the common
 * disarmed case. */
static inline bool hailo_trace_active(uint32_t mech_bit)
{
    if (hailo_trace_mech_mask == 0u || hailo_trace_phase_mask == 0u)
        return false;
    if ((hailo_trace_mech_mask & mech_bit) == 0u) return false;
    const uint32_t phase_bit = (hailo_trace_current_phase == HAILO_TRACE_PHASE_NONE)
        ? 0u
        : HAILO_TRACE_PHASE_BIT(hailo_trace_current_phase);
    return (hailo_trace_phase_mask & phase_bit) != 0u;
}

/* -------------------------------------------------------------------------- */
/* Lifecycle + control                                                         */
/* -------------------------------------------------------------------------- */

/* Advances the current phase. Emits a `[trc] phase=<old>→<new>` marker
 * line if either the old or new phase has tracing armed, so the
 * transition is locatable in captured output. Safe to call from any
 * context that the trace emit helpers themselves are safe in (no
 * locks, no allocations). */
void hailo_trace_set_phase(enum hailo_trace_phase next);

/* Parses a kernel cmdline string (typically from DTB /chosen/bootargs)
 * for `hailo_trace.phase=...` and `hailo_trace.mech=...` tokens,
 * arming the matching mask bits. Comma-separated values; `all`/`off`
 * shortcuts. Unknown tokens log a warning and are skipped.
 *
 * Safe to call with NULL or empty input — both masks stay at their
 * build-default values. Idempotent; later calls OR-merge into the
 * existing mask (so the build default plus a cmdline addition both
 * end up armed). */
void hailo_trace_cmdline_parse(const char *bootargs);

/* Shell-side mask manipulators. Each accepts the same comma-separated
 * name lists as the cmdline parser plus `all` and `off`. Returns 0 on
 * success, -1 on parse error (unknown token). The reset variant
 * clears both masks regardless of input. */
int  hailo_trace_set_phase_mask(const char *names);
int  hailo_trace_set_mech_mask(const char *names);
void hailo_trace_reset(void);

/* Pretty-prints current trace state to UART. Used by `hailo trace`
 * with no arguments. */
void hailo_trace_print_state(void);

/* -------------------------------------------------------------------------- */
/* Emit helpers — one per mechanism                                            */
/* -------------------------------------------------------------------------- */
/*
 * Each helper assumes the caller has already gated on
 * `hailo_trace_active(MECH)` — they don't re-check. This keeps the
 * disabled path cheap (the caller's `if` is the only branch) and lets
 * the emit code path be straight-line. The phase tag in the output
 * line comes from the global `hailo_trace_current_phase`.
 *
 * Output format (matches Linux trace_mmio patch where applicable):
 *
 *   [trc] phase=<P> mech=MMIO    WR32 bar=<n> off=0x<NNNN> val=0x<NNNNNNNN>
 *   [trc] phase=<P> mech=MMIO    RD32 bar=<n> off=0x<NNNN> val=0x<NNNNNNNN>
 *   [trc] phase=<P> mech=PCI     WR<w> bdf=<b>:<d>.<f> off=0x<NN> val=0x<...>
 *   [trc] phase=<P> mech=PCI     RD<w> bdf=<b>:<d>.<f> off=0x<NN> val=0x<...>
 *   [trc] phase=<P> mech=RPC     tx op=0x<NN> len=<n> cpu=<APP|CORE> md5=<...8 hex>
 *   [trc] phase=<P> mech=RPC     rx status=<major>:<minor> len=<n>
 *   [trc] phase=<P> mech=IRQ     spi=<n> istatus=0x<NNNNNNNN> src=0x<...> dst=0x<...>
 *   [trc] phase=<P> mech=DMA     <event> ch=<n> <fields>
 *   [trc] phase=<P> mech=BUSY    udelay us=<n> tag=<label>
 */

void hailo_trace_emit_mmio_w(int bar, uint32_t off, uint32_t val);
void hailo_trace_emit_mmio_r(int bar, uint32_t off, uint32_t val);

/* width: 8, 16, or 32. bdf packed as ((bus << 8) | (dev << 3) | func). */
void hailo_trace_emit_cfg_w(uint16_t bdf, uint16_t off, unsigned width, uint32_t val);
void hailo_trace_emit_cfg_r(uint16_t bdf, uint16_t off, unsigned width, uint32_t val);

/* cpu: 1=APP, 2=CORE (matches the BAR4+0x1684 doorbell mask values).
 * md5_first8: first 8 bytes of the RPC md5 (full 16 is overkill in a
 * trace line; collision risk is negligible across a single boot). */
void hailo_trace_emit_rpc_tx(uint8_t op, uint32_t len, uint8_t cpu,
                             const uint8_t md5_first8[8]);
void hailo_trace_emit_rpc_rx(uint8_t status_major, uint8_t status_minor,
                             uint32_t len);

void hailo_trace_emit_irq(uint32_t spi, uint32_t istatus,
                          uint32_t per_src, uint32_t per_dst);

/* Free-form event tag for DMA — channels, descriptor arms, num_avail
 * bumps don't share a single payload shape, so the caller passes a
 * short label and up to four hex u32s. Unused slots may be 0. */
void hailo_trace_emit_dma(const char *event, int ch,
                          uint32_t a, uint32_t b, uint32_t c, uint32_t d);

void hailo_trace_emit_busy_wait(uint32_t us, const char *tag);

#endif /* HAILO_TRACE_H */
