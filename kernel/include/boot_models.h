/*
 * boot_models.h - Model files the EFI stub read before ExitBootServices.
 *
 * Jetson has no storage driver at runtime, so on a UEFI-direct boot the
 * stub reads \slmos\models\*.gguf through the firmware's file protocols
 * and hands the pages to the kernel (docs/adr/0002-uefi-model-delivery.md).
 *
 * The record lives in .data, like slmos_boot_source: the stub writes it
 * in the image UEFI loaded, boot.S copies code + .data to the link
 * address, and primary_cpu clears .bss afterwards.
 *
 * Flow in the kernel:
 *   boot_models_reserve()  after vmm_init, before pmm_init: checks each
 *                          record against the MMU map and the file's
 *                          magic and hash, reserves what passes. A record
 *                          that fails is dropped and its pages stay free
 *                          for the buddy allocator.
 *   boot_models_mount()    after vfs_init: read-only /boot/models.
 *   boot_models_slm_load() from `slm load`: zero-copy registry load.
 */
#ifndef BOOT_MODELS_H
#define BOOT_MODELS_H

#include "platform.h"
#include <stddef.h>
#include <stdint.h>

/* Files per boot. Matches the SLM registry's SLM_MAX_SLOTS. */
#define BOOT_MODELS_MAX         4
/* File name length, NUL included. Equal to VFS_MAX_NAME. */
#define BOOT_MODEL_NAME_LEN     64
/* Mirrors the registry's MAX_PLAUSIBLE_GGUF_BYTES: anything larger could
 * never be loaded, so the stub does not spend boot time reading it. */
#define BOOT_MODEL_MAX_BYTES    (2ULL * 1024 * 1024 * 1024)
#define BOOT_MODELS_VFS_DIR     "/boot/models"

/* Physical alignment of a model: the identity map's block size. */
#define BOOT_MODEL_ALIGN        (2ULL * 1024 * 1024)

/*
 * The physical window a model may occupy. Jetson: the third RAM region,
 * whose end is the RAM identity map's end (vmm.c) and which is clear of
 * the fixed low-memory users (NC region, camera carveout, OP-TEE).
 */
#if defined(PLATFORM_JETSON_ORIN_NANO)
#define EFI_MODEL_FLOOR         0x100000000ULL
#define EFI_MODEL_LIMIT         0x240000000ULL
#else
#define EFI_MODEL_FLOOR         ((uint64_t)RAM_BASE)
#define EFI_MODEL_LIMIT         ((uint64_t)RAM_BASE + (uint64_t)RAM_SIZE)
#endif

struct boot_model {
    uint64_t phys;      /* 2 MiB aligned; identity-mapped */
    uint64_t size;      /* file size in bytes */
    uint64_t hash;      /* boot_model_hash() of the file, from the stub */
    uint32_t read_ms;   /* stub: firmware read time */
    uint32_t hash_ms;   /* stub: hash time */
    uint32_t clean_ms;  /* stub: DC CIVAC time */
    uint32_t pad;
    char name[BOOT_MODEL_NAME_LEN];
};

struct boot_model_table {
    uint32_t count;
    uint32_t pad;
    struct boot_model models[BOOT_MODELS_MAX];
};

/* Defined in kernel/src/boot_models.c, in .data. */
extern struct boot_model_table slmos_boot_models;

#define BOOT_MODEL_GGUF_MAGIC   0x46554747U     /* "GGUF", little-endian */

/*
 * Whole-file hash: four 64-bit lanes over little-endian words, so it
 * keeps up with memory bandwidth rather than a byte loop. It detects
 * corruption across the hand-off (a cache or relocation bug, a bad
 * read); it is not a security measure.
 *
 * Inline so the stub, which runs from the load address before the
 * kernel is relocated, has no call into kernel code for it.
 */
#define BOOT_MODEL_HASH_P1  0x9E3779B185EBCA87ULL
#define BOOT_MODEL_HASH_P2  0xC2B2AE3D27D4EB4FULL

static inline uint64_t boot_model_rotl(uint64_t v, unsigned r)
{
    return (v << r) | (v >> (64u - r));
}

static inline uint64_t boot_model_hash(const void *data, uint64_t len)
{
    const uint8_t *p = (const uint8_t *)data;
    uint64_t l0 = BOOT_MODEL_HASH_P1 + BOOT_MODEL_HASH_P2;
    uint64_t l1 = BOOT_MODEL_HASH_P2;
    uint64_t l2 = 0;
    uint64_t l3 = 0 - BOOT_MODEL_HASH_P1;
    uint64_t left = len;

    while (left >= 32) {
        uint64_t w0, w1, w2, w3;
        __builtin_memcpy(&w0, p, 8);
        __builtin_memcpy(&w1, p + 8, 8);
        __builtin_memcpy(&w2, p + 16, 8);
        __builtin_memcpy(&w3, p + 24, 8);
        l0 = boot_model_rotl(l0 + w0 * BOOT_MODEL_HASH_P2, 31) * BOOT_MODEL_HASH_P1;
        l1 = boot_model_rotl(l1 + w1 * BOOT_MODEL_HASH_P2, 31) * BOOT_MODEL_HASH_P1;
        l2 = boot_model_rotl(l2 + w2 * BOOT_MODEL_HASH_P2, 31) * BOOT_MODEL_HASH_P1;
        l3 = boot_model_rotl(l3 + w3 * BOOT_MODEL_HASH_P2, 31) * BOOT_MODEL_HASH_P1;
        p += 32;
        left -= 32;
    }

    uint64_t h = boot_model_rotl(l0, 1) + boot_model_rotl(l1, 7) +
                 boot_model_rotl(l2, 12) + boot_model_rotl(l3, 18) + len;
    while (left > 0) {
        h = boot_model_rotl(h ^ (*p * BOOT_MODEL_HASH_P1), 11) * BOOT_MODEL_HASH_P2;
        p++;
        left--;
    }
    h ^= h >> 33;
    h *= BOOT_MODEL_HASH_P2;
    h ^= h >> 29;
    return h;
}

/*
 * Kernel side (kernel/src/boot_models.c). The _table forms take the
 * table explicitly so tests can drive them with synthetic records; the
 * boot path uses slmos_boot_models.
 */

/* Does this record describe a mapped, intact GGUF? 0 if so, else < 0.
 * Needs the MMU on: the hash reads the file through the identity map. */
int  boot_model_verify(const struct boot_model *m);

/* Verify + reserve every record; failures are removed from the table.
 * Must run between vmm_init and pmm_init (see pmm_user_reserve_add). */
void boot_models_reserve_table(struct boot_model_table *t);
void boot_models_reserve(void);

/* Mount read-only /boot/models if any model survived. */
void boot_models_mount(void);

/* The filesystem behind /boot/models, for slmos_boot_models. */
struct vfs_fs_ops;
extern const struct vfs_fs_ops boot_models_vfs_ops;

/* The record for a VFS path, or NULL if the path isn't a boot model. */
const struct boot_model *boot_models_find_path(const struct boot_model_table *t,
                                               const char *path);

/* Return of boot_models_slm_load when `path` is not a boot model. */
#define BOOT_MODEL_NOT_HANDLED  1

/*
 * `slm load` hook. When `path` is a boot model, load it into the SLM
 * registry without copying (the registry borrows the reserved pages)
 * and report on the shell: returns 0, or -1 on failure. Otherwise
 * returns BOOT_MODEL_NOT_HANDLED and `slm load` carries on as usual.
 */
int  boot_models_slm_load(const char *path);

/*
 * Registry entry point (runtime/src/lib.rs). Like
 * rust_slm_load_take_pages, except that the registry never frees the
 * pages: they stay reserved after `slm unload`, and the same model can
 * be loaded again. Returns the slot index, or -1.
 */
extern int rust_slm_load_borrow_pages(const uint8_t *name,
                                      const uint8_t *data,
                                      size_t pages,
                                      size_t data_len);

#endif /* BOOT_MODELS_H */
