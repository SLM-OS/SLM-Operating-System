/*
 * hailo8 — QEMU PCIe stub device impersonating a Hailo-8 NPU.
 *
 * Captures HailoRT's BAR0/BAR2/BAR4 MMIO traffic against a JSONL corpus
 * defined in docs/hailo-re-corpus-format.md. Reads are answered from the
 * corpus when (seq, bar, offset, size) matches; misses halt QEMU with
 *   HAILO_RE_CORPUS_EXTEND seq=N bar=B offset=X size=S reason=unknown_read
 * and exit code 1 so the Task 0.5 driver script can run hailo replay-step
 * on real hardware and extend the corpus. Writes whose shape doesn't
 * match a recorded entry halt with HAILO_RE_CORPUS_DIVERGENCE; unknown
 * writes are appended to the corpus file in place.
 *
 * Anchor: issue #795 Task 0.2.
 */

#include "qemu/osdep.h"
#include "qemu/log.h"
#include "qemu/error-report.h"
#include "qapi/error.h"
#include "hw/pci/pci.h"
#include "hw/pci/pci_device.h"
#include "hw/pci/msi.h"
#include "hw/qdev-properties.h"
#include "qom/object.h"
#include "migration/vmstate.h"

#include "hailo8_corpus.h"

#define TYPE_HAILO8 "hailo8"

#define HAILO8_VENDOR_ID         0x1e60
#define HAILO8_DEVICE_ID_HAILO8  0x2864

/* Defaults match the Hailo-8 silicon Linux's hailo_pci driver exposes
 * (BAR4 = 16 MiB fw access window per pcie_common.c max_size). BAR0/BAR2
 * sizes are reasonable placeholders — overridable via qdev properties. */
#define HAILO8_DEFAULT_BAR0_SIZE  0x4000        /* 16 KiB — NNC config */
#define HAILO8_DEFAULT_BAR2_SIZE  0x40000       /* 256 KiB — vDMA regs */
#define HAILO8_DEFAULT_BAR4_SIZE  0x1000000     /* 16 MiB — fw access */

#define HAILO8_MSI_NR_VECTORS     4

typedef struct Hailo8State Hailo8State;

typedef struct Hailo8BarCtx {
    Hailo8State *s;
    int bar_index;          /* 0, 2, or 4 — used in stdout lines */
} Hailo8BarCtx;

struct Hailo8State {
    PCIDevice parent_obj;

    /* Properties */
    char    *corpus_path;
    uint64_t bar0_size;
    uint64_t bar2_size;
    uint64_t bar4_size;

    /* Runtime state */
    MemoryRegion bar0_mr;
    MemoryRegion bar2_mr;
    MemoryRegion bar4_mr;
    Hailo8BarCtx bar0_ctx;
    Hailo8BarCtx bar2_ctx;
    Hailo8BarCtx bar4_ctx;

    hailo_corpus_t *corpus;
    uint64_t        seq;    /* monotonic; first op is seq=1 */
};

OBJECT_DECLARE_SIMPLE_TYPE(Hailo8State, HAILO8)

/* -------------------------------------------------------------------------- */
/* stdout / stderr line emitters — see docs/hailo-re-corpus-format.md          */
/* -------------------------------------------------------------------------- */

static void emit_corpus_extend(uint64_t seq, int bar,
                               uint64_t offset, uint32_t size)
{
    fprintf(stdout,
            "HAILO_RE_CORPUS_EXTEND seq=%" PRIu64
            " bar=%d offset=%" PRIu64 " size=%u reason=unknown_read\n",
            seq, bar, offset, size);
    fflush(stdout);
}

/* Emit a value-divergence line (reason = write_value_mismatch /
 * inline_read_mismatch — the value fields carry the actual divergence). */
static void emit_corpus_divergence_value(uint64_t seq, int bar,
                                         uint64_t offset, uint32_t size,
                                         const char *dir,
                                         uint64_t expected, uint64_t observed,
                                         const char *reason)
{
    char exp_hex[32], obs_hex[32];
    if (hailo_value_to_hex(expected, size, exp_hex, sizeof(exp_hex)) != 0) {
        snprintf(exp_hex, sizeof(exp_hex), "??");
    }
    if (hailo_value_to_hex(observed, size, obs_hex, sizeof(obs_hex)) != 0) {
        snprintf(obs_hex, sizeof(obs_hex), "??");
    }
    fprintf(stdout,
            "HAILO_RE_CORPUS_DIVERGENCE seq=%" PRIu64
            " bar=%d offset=%" PRIu64 " size=%u dir=%s "
            "expected=%s observed=%s source=qemu reason=%s\n",
            seq, bar, offset, size, dir, exp_hex, obs_hex, reason);
    fflush(stdout);
}

/* Emit a shape-divergence line (reason = op_shape_mismatch — bar / offset /
 * size / dir don't match the corpus entry at this seq). The value fields
 * aren't the divergence axis here; emit zero-filled placeholders at the
 * spec-required width, then append explicit expected_* tokens so the
 * Task 0.5 driver script can recover the corpus-side shape without a
 * round-trip back through the corpus file. */
static void emit_corpus_divergence_shape(uint64_t seq,
                                         int obs_bar, uint64_t obs_offset,
                                         uint32_t obs_size, const char *obs_dir,
                                         int exp_bar, uint64_t exp_offset,
                                         uint32_t exp_size, const char *exp_dir)
{
    char zero_hex[32];
    if (hailo_value_to_hex(0, obs_size, zero_hex, sizeof(zero_hex)) != 0) {
        snprintf(zero_hex, sizeof(zero_hex), "00000000");
    }
    fprintf(stdout,
            "HAILO_RE_CORPUS_DIVERGENCE seq=%" PRIu64
            " bar=%d offset=%" PRIu64 " size=%u dir=%s "
            "expected=%s observed=%s source=qemu reason=op_shape_mismatch "
            "expected_bar=%d expected_offset=%" PRIu64
            " expected_size=%u expected_dir=%s\n",
            seq, obs_bar, obs_offset, obs_size, obs_dir,
            zero_hex, zero_hex,
            exp_bar, exp_offset, exp_size, exp_dir);
    fflush(stdout);
}

static void emit_msi_fire(uint64_t after_seq, uint32_t vector)
{
    /* stderr per the worktree README: MSI events are a separate causal
     * track and shouldn't interleave with the corpus stdout protocols. */
    fprintf(stderr,
            "HAILO_RE_MSI_FIRE after_seq=%" PRIu64 " vector=%u\n",
            after_seq, vector);
    fflush(stderr);
}

/* -------------------------------------------------------------------------- */
/* BAR R/W callbacks                                                           */
/* -------------------------------------------------------------------------- */

static const char *bar_dir_str(bool is_write)
{
    return is_write ? "write" : "read";
}

static uint64_t hailo8_bar_read(void *opaque, hwaddr addr, unsigned size)
{
    Hailo8BarCtx *ctx = opaque;
    Hailo8State *s = ctx->s;
    uint64_t seq = ++s->seq;

    const hailo_op_entry_t *e = hailo_corpus_get(s->corpus, seq);
    if (!e) {
        emit_corpus_extend(seq, ctx->bar_index, (uint64_t)addr, size);
        exit(1);
    }

    /* Step 0 — op shape mismatch. */
    if (e->is_write || e->bar != ctx->bar_index ||
        e->offset != (uint64_t)addr || e->size != size) {
        emit_corpus_divergence_shape(seq,
                                     ctx->bar_index, (uint64_t)addr,
                                     size, bar_dir_str(false),
                                     e->bar, e->offset,
                                     e->size, bar_dir_str(e->is_write));
        exit(1);
    }

    /* Step 1-3 — hit. Fire optional MSI after-return event. */
    if (e->msi_after_present) {
        emit_msi_fire(seq, e->msi_after_vector);
        msi_notify(&s->parent_obj, e->msi_after_vector);
    }
    return e->value;
}

static void hailo8_bar_write(void *opaque, hwaddr addr,
                             uint64_t data, unsigned size)
{
    Hailo8BarCtx *ctx = opaque;
    Hailo8State *s = ctx->s;
    uint64_t seq = ++s->seq;

    /* Mask data to access width — QEMU passes the full uint64_t. */
    uint64_t mask = (size == 8) ? UINT64_MAX : ((1ULL << (size * 8)) - 1);
    data &= mask;

    const hailo_op_entry_t *e = hailo_corpus_get(s->corpus, seq);
    if (e) {
        /* Step 1 of write logging — shape + value check. */
        if (!e->is_write || e->bar != ctx->bar_index ||
            e->offset != (uint64_t)addr || e->size != size) {
            emit_corpus_divergence_shape(seq,
                                         ctx->bar_index, (uint64_t)addr,
                                         size, bar_dir_str(true),
                                         e->bar, e->offset,
                                         e->size, bar_dir_str(e->is_write));
            exit(1);
        }
        if (e->value != data) {
            emit_corpus_divergence_value(seq, ctx->bar_index, (uint64_t)addr,
                                         size, bar_dir_str(true),
                                         e->value, data,
                                         "write_value_mismatch");
            exit(1);
        }
        return;
    }

    /* Step 2 — unknown write. Append to corpus. */
    char err[256];
    if (hailo_corpus_append_write(s->corpus, seq, ctx->bar_index,
                                  (uint64_t)addr, size, data,
                                  err, sizeof(err)) != 0) {
        error_report("hailo8: corpus append failed at seq=%" PRIu64
                     " bar=%d offset=%" PRIu64 ": %s",
                     seq, ctx->bar_index, (uint64_t)addr, err);
        exit(1);
    }
}

static const MemoryRegionOps hailo8_bar_ops = {
    .read       = hailo8_bar_read,
    .write      = hailo8_bar_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .impl  = { .min_access_size = 1, .max_access_size = 8 },
    .valid = { .min_access_size = 1, .max_access_size = 8 },
};

/* -------------------------------------------------------------------------- */
/* Lifecycle                                                                   */
/* -------------------------------------------------------------------------- */

static bool bar_size_is_pow2(uint64_t v)
{
    return v != 0 && (v & (v - 1)) == 0;
}

static void hailo8_realize(PCIDevice *pci_dev, Error **errp)
{
    Hailo8State *s = HAILO8(pci_dev);
    Error *local_err = NULL;

    if (!s->corpus_path || !*s->corpus_path) {
        error_setg(errp, "hailo8: 'corpus' property is required");
        return;
    }

    /* PCI BARs require power-of-2 sizes; reject early with a clear
     * message rather than letting QEMU's memory subsystem complain later. */
    if (!bar_size_is_pow2(s->bar0_size)) {
        error_setg(errp, "hailo8: bar0_size must be a non-zero power of 2 "
                         "(got 0x%" PRIx64 ")", s->bar0_size);
        return;
    }
    if (!bar_size_is_pow2(s->bar2_size)) {
        error_setg(errp, "hailo8: bar2_size must be a non-zero power of 2 "
                         "(got 0x%" PRIx64 ")", s->bar2_size);
        return;
    }
    if (!bar_size_is_pow2(s->bar4_size)) {
        error_setg(errp, "hailo8: bar4_size must be a non-zero power of 2 "
                         "(got 0x%" PRIx64 ")", s->bar4_size);
        return;
    }

    char err[256];
    s->corpus = hailo_corpus_load(s->corpus_path,
                                  /* append_writable = */ true,
                                  err, sizeof(err));
    if (!s->corpus) {
        error_setg(errp, "hailo8: failed to load corpus '%s': %s",
                   s->corpus_path, err);
        return;
    }
    s->seq = 0;

    /* PCI config: enable bus-master via the kernel driver's enable path;
     * we only need to advertise the BARs and MSI capability. */
    pci_config_set_class(pci_dev->config, PCI_CLASS_PROCESSOR_CO);
    pci_config_set_interrupt_pin(pci_dev->config, 1);

    s->bar0_ctx.s = s;
    s->bar0_ctx.bar_index = 0;
    s->bar2_ctx.s = s;
    s->bar2_ctx.bar_index = 2;
    s->bar4_ctx.s = s;
    s->bar4_ctx.bar_index = 4;

    memory_region_init_io(&s->bar0_mr, OBJECT(s), &hailo8_bar_ops,
                          &s->bar0_ctx, "hailo8.bar0", s->bar0_size);
    memory_region_init_io(&s->bar2_mr, OBJECT(s), &hailo8_bar_ops,
                          &s->bar2_ctx, "hailo8.bar2", s->bar2_size);
    memory_region_init_io(&s->bar4_mr, OBJECT(s), &hailo8_bar_ops,
                          &s->bar4_ctx, "hailo8.bar4", s->bar4_size);

    /* Hailo-8 BARs are 64-bit memory BARs (BAR0+1, BAR2+3, BAR4+5). */
    pci_register_bar(pci_dev, 0,
                     PCI_BASE_ADDRESS_SPACE_MEMORY |
                     PCI_BASE_ADDRESS_MEM_TYPE_64,
                     &s->bar0_mr);
    pci_register_bar(pci_dev, 2,
                     PCI_BASE_ADDRESS_SPACE_MEMORY |
                     PCI_BASE_ADDRESS_MEM_TYPE_64,
                     &s->bar2_mr);
    pci_register_bar(pci_dev, 4,
                     PCI_BASE_ADDRESS_SPACE_MEMORY |
                     PCI_BASE_ADDRESS_MEM_TYPE_64,
                     &s->bar4_mr);

    int rc = msi_init(pci_dev, /*offset=*/ 0,
                      HAILO8_MSI_NR_VECTORS,
                      /*msi64bit=*/ true,
                      /*msi_per_vector_mask=*/ false,
                      &local_err);
    if (rc < 0) {
        /* QEMU does not call .exit on a failed realize, so the corpus
         * append handle would leak across repeated `device_add` attempts
         * from the monitor. Tear it down explicitly here. */
        hailo_corpus_free(s->corpus);
        s->corpus = NULL;
        error_propagate(errp, local_err);
        return;
    }
}

static void hailo8_exit(PCIDevice *pci_dev)
{
    Hailo8State *s = HAILO8(pci_dev);
    msi_uninit(pci_dev);
    hailo_corpus_free(s->corpus);
    s->corpus = NULL;
}

static void hailo8_reset(DeviceState *dev)
{
    (void)dev;
    /* On guest-driven reset, do NOT reset the seq counter — the corpus is
     * keyed on a session-monotonic seq, and HailoRT may reset BARs during
     * its normal init flow without that meaning "restart the capture". */
}

static Property hailo8_properties[] = {
    DEFINE_PROP_STRING("corpus", Hailo8State, corpus_path),
    DEFINE_PROP_UINT64("bar0_size", Hailo8State, bar0_size,
                       HAILO8_DEFAULT_BAR0_SIZE),
    DEFINE_PROP_UINT64("bar2_size", Hailo8State, bar2_size,
                       HAILO8_DEFAULT_BAR2_SIZE),
    DEFINE_PROP_UINT64("bar4_size", Hailo8State, bar4_size,
                       HAILO8_DEFAULT_BAR4_SIZE),
    DEFINE_PROP_END_OF_LIST(),
};

static const VMStateDescription vmstate_hailo8 = {
    .name = TYPE_HAILO8,
    /* The stub is not migratable — corpus + seq counter pin it to its
     * launch. Provide a stub VMStateDescription so QEMU doesn't crash
     * if someone tries; failure is loud. */
    .unmigratable = 1,
};

static void hailo8_class_init(ObjectClass *klass, void *data)
{
    DeviceClass    *dc = DEVICE_CLASS(klass);
    PCIDeviceClass *k  = PCI_DEVICE_CLASS(klass);

    k->realize   = hailo8_realize;
    k->exit      = hailo8_exit;
    k->vendor_id = HAILO8_VENDOR_ID;
    k->device_id = HAILO8_DEVICE_ID_HAILO8;
    k->revision  = 0x01;
    k->class_id  = PCI_CLASS_PROCESSOR_CO;

    dc->desc    = "Hailo-8 NPU RE capture stub";
    dc->reset   = hailo8_reset;
    dc->vmsd    = &vmstate_hailo8;
    dc->user_creatable = true;
    device_class_set_props(dc, hailo8_properties);
    set_bit(DEVICE_CATEGORY_MISC, dc->categories);
}

static const TypeInfo hailo8_info = {
    .name          = TYPE_HAILO8,
    .parent        = TYPE_PCI_DEVICE,
    .instance_size = sizeof(Hailo8State),
    .class_init    = hailo8_class_init,
    .interfaces    = (InterfaceInfo[]) {
        { INTERFACE_CONVENTIONAL_PCI_DEVICE },
        { },
    },
};

static void hailo8_register_types(void)
{
    type_register_static(&hailo8_info);
}

type_init(hailo8_register_types)
