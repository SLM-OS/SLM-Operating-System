/*
 * boot_models.c - Kernel side of UEFI model delivery.
 *
 * The EFI stub (kernel/arch/arm64/efi_model.c) leaves model files in
 * LoaderData pages and records them in slmos_boot_models. This file
 * checks them, keeps them out of the buddy allocator, exposes them
 * read-only at /boot/models and lets `slm load` borrow them without a
 * copy. See boot_models.h and docs/adr/0002-uefi-model-delivery.md.
 *
 * On any other boot path the table is empty and all of this is a no-op.
 */

#include "boot_models.h"
#include "debug.h"
#include "pmm.h"
#include "shell.h"
#include "slm_ffi.h"
#include "string.h"
#include "vfs.h"
#include "vmm.h"
#include <stdbool.h>

_Static_assert(BOOT_MODEL_NAME_LEN == VFS_MAX_NAME,
               "a boot model name must fit a VFS entry name");

/* Not zero-initialised storage: see boot_models.h for why .data. */
__attribute__((section(".data")))
struct boot_model_table slmos_boot_models = { .count = 0 };

/*
 * Milliseconds from the raw counter. boot_models_reserve_table runs
 * before timer_init, so timer_get_frequency() can't be used for it.
 */
static uint64_t boot_models_now_ticks(void)
{
#if defined(__aarch64__)
    uint64_t t;
    __asm__ volatile("isb\n mrs %0, cntpct_el0" : "=r"(t));
    return t;
#else
    return 0;   /* no boot models on this architecture */
#endif
}

static uint64_t boot_models_ms_since(uint64_t start)
{
#if defined(__aarch64__)
    uint64_t f;
    __asm__ volatile("mrs %0, cntfrq_el0" : "=r"(f));
    return f ? (boot_models_now_ticks() - start) * 1000 / f : 0;
#else
    (void)start;
    return 0;
#endif
}

static bool boot_models_name_ok(const char *name)
{
    if (name[0] == '\0') {
        return false;
    }
    for (size_t i = 1; i < BOOT_MODEL_NAME_LEN; i++) {
        if (name[i] == '\0') {
            return true;
        }
    }
    return false;
}

int boot_model_verify(const struct boot_model *m)
{
    if (m->size == 0 || m->size > BOOT_MODEL_MAX_BYTES ||
        (m->phys & (BOOT_MODEL_ALIGN - 1)) != 0 ||
        !boot_models_name_ok(m->name)) {
        return -1;
    }
    /* The identity map uses 2 MiB blocks: check each one the file spans. */
    for (uint64_t pa = m->phys; pa < m->phys + m->size; pa += BOOT_MODEL_ALIGN) {
        if (!vmm_is_mapped(pa)) {
            return -2;
        }
    }
    uint32_t magic;
    memcpy(&magic, (const void *)(uintptr_t)m->phys, sizeof(magic));
    if (magic != BOOT_MODEL_GGUF_MAGIC) {
        return -3;
    }
    if (boot_model_hash((const void *)(uintptr_t)m->phys, m->size) != m->hash) {
        return -4;
    }
    return 0;
}

void boot_models_reserve_table(struct boot_model_table *t)
{
    uint32_t kept = 0;

    for (uint32_t i = 0; i < t->count && i < BOOT_MODELS_MAX; i++) {
        struct boot_model *m = &t->models[i];
        uint64_t t0 = boot_models_now_ticks();
        int rc = boot_model_verify(m);
        uint64_t verify_ms = boot_models_ms_since(t0);

        if (rc != 0) {
            /* The name is only printable when the check got past it. */
            WARN("Boot model %s failed its check (%d), dropped",
                 boot_models_name_ok(m->name) ? m->name : "?", rc);
            continue;
        }
        /* Page-rounded by the carve helper, so the file's last partial
         * page is protected too. */
        if (pmm_user_reserve_add(m->phys, m->size) != 0) {
            WARN("Boot model %s: PMM reserve table full, dropped", m->name);
            continue;
        }
        INFO("Boot model %s: %llu MiB at 0x%llx (stub read %u ms, hash %u ms, "
             "clean %u ms; kernel verify %llu ms)",
             m->name, (unsigned long long)(m->size >> 20),
             (unsigned long long)m->phys, m->read_ms, m->hash_ms, m->clean_ms,
             (unsigned long long)verify_ms);
        if (kept != i) {
            t->models[kept] = *m;
        }
        kept++;
    }
    t->count = kept;
}

void boot_models_reserve(void)
{
    boot_models_reserve_table(&slmos_boot_models);
}

/* ---- VFS: read-only flat directory over slmos_boot_models ---- */

/* A path inside the mount: "/name", "name", "/" or "". */
static const struct boot_model *boot_models_find_name(
    const struct boot_model_table *t, const char *sub)
{
    while (*sub == '/') {
        sub++;
    }
    for (uint32_t i = 0; i < t->count; i++) {
        if (strcmp(t->models[i].name, sub) == 0) {
            return &t->models[i];
        }
    }
    return NULL;
}

static bool boot_models_is_root(const char *path)
{
    if (path == NULL) {
        return true;
    }
    while (*path == '/') {
        path++;
    }
    return *path == '\0';
}

static int boot_models_vfs_read(void *ctx, const char *path, char *buf,
                                size_t size, size_t offset)
{
    (void)ctx;
    const struct boot_model *m = boot_models_find_name(&slmos_boot_models, path);
    if (m == NULL || buf == NULL) {
        return -1;
    }
    if (offset >= m->size) {
        return 0;
    }
    size_t n = m->size - offset;
    if (n > size) {
        n = size;
    }
    if (n > (size_t)INT32_MAX) {
        n = (size_t)INT32_MAX;
    }
    memcpy(buf, (const char *)(uintptr_t)m->phys + offset, n);
    return (int)n;
}

static int boot_models_vfs_readdir(void *ctx, const char *path,
                                   void (*callback)(const struct vfs_entry_info *info,
                                                    void *cb_ctx),
                                   void *cb_ctx)
{
    (void)ctx;
    if (callback == NULL || !boot_models_is_root(path)) {
        return -1;
    }
    for (uint32_t i = 0; i < slmos_boot_models.count; i++) {
        const struct boot_model *m = &slmos_boot_models.models[i];
        struct vfs_entry_info info;
        strncpy(info.name, m->name, VFS_MAX_NAME);
        info.name[VFS_MAX_NAME - 1] = '\0';
        info.type = 0;
        info.size = (uint32_t)m->size;
        callback(&info, cb_ctx);
    }
    return 0;
}

static int boot_models_vfs_stat(void *ctx, const char *path,
                                struct vfs_entry_info *info)
{
    (void)ctx;
    if (info == NULL) {
        return -1;
    }
    if (boot_models_is_root(path)) {
        strncpy(info->name, "models", VFS_MAX_NAME);
        info->type = 1;
        info->size = 0;
        return 0;
    }
    const struct boot_model *m = boot_models_find_name(&slmos_boot_models, path);
    if (m == NULL) {
        return -1;
    }
    strncpy(info->name, m->name, VFS_MAX_NAME);
    info->name[VFS_MAX_NAME - 1] = '\0';
    info->type = 0;
    info->size = (uint32_t)m->size;
    return 0;
}

const struct vfs_fs_ops boot_models_vfs_ops = {
    .read = boot_models_vfs_read,
    .readdir = boot_models_vfs_readdir,
    .stat = boot_models_vfs_stat,
};

void boot_models_mount(void)
{
    if (slmos_boot_models.count == 0) {
        return;
    }
    /*
     * The context stays NULL on purpose. Shell verbs that write (put,
     * rm, mkdir, ...) get their target from vfs_get_mount_ctx() and cast
     * it to struct lfs_mount *. NULL makes each of them answer "not a
     * mounted filesystem", which is what keeps this read-only.
     */
    if (vfs_mount(BOOT_MODELS_VFS_DIR, &boot_models_vfs_ops, NULL) == NULL) {
        WARN("Could not mount %s", BOOT_MODELS_VFS_DIR);
        return;
    }
    INFO("  %u boot model(s) at %s", slmos_boot_models.count, BOOT_MODELS_VFS_DIR);
}

const struct boot_model *boot_models_find_path(const struct boot_model_table *t,
                                               const char *path)
{
    size_t dir_len = strlen(BOOT_MODELS_VFS_DIR);
    if (path == NULL || strncmp(path, BOOT_MODELS_VFS_DIR, dir_len) != 0 ||
        path[dir_len] != '/') {
        return NULL;
    }
    return boot_models_find_name(t, path + dir_len);
}

/* ---- `slm load` ---- */

/* Registry display name: the file name without ".gguf", 31 bytes at most
 * (the registry's SLM_NAME_LEN - 1). */
static void boot_models_display_name(const char *file, char *out, size_t cap)
{
    size_t len = strlen(file);
    if (len > 5 && file[len - 5] == '.') {
        len -= 5;
    }
    if (len >= cap) {
        len = cap - 1;
    }
    memcpy(out, file, len);
    out[len] = '\0';
}

int boot_models_slm_load(const char *path)
{
    const struct boot_model *m = boot_models_find_path(&slmos_boot_models, path);
    if (m == NULL) {
        return BOOT_MODEL_NOT_HANDLED;
    }

    char name[SLM_MODEL_NAME_LEN];
    boot_models_display_name(m->name, name, sizeof(name));

    uint64_t t0 = boot_models_now_ticks();
    int idx = rust_slm_load_borrow_pages((const uint8_t *)name,
                                         (const uint8_t *)(uintptr_t)m->phys,
                                         (m->size + 4095u) / 4096u,
                                         (size_t)m->size);
    uint64_t load_ms = boot_models_ms_since(t0);
    if (idx < 0) {
        shell_printf("slm load: failed to parse %s "
                     "(not a GGUF or unsupported architecture)\r\n", path);
        return -1;
    }

    SlmModelInfoC mi;
    if (rust_slm_get_info((uint32_t)idx, &mi) != 0) {
        shell_printf("[slm] loaded handle=%d (info unavailable)\r\n", idx);
        return 0;
    }
    char arch[sizeof(mi.architecture) + 1];
    memcpy(arch, mi.architecture, sizeof(mi.architecture));
    arch[sizeof(mi.architecture)] = '\0';
    shell_printf("[slm] loaded handle=%d  arch=%s  blocks=%lu hidden=%lu "
                 "head=%lu/%lu head_dim=%lu vocab=%lu ctx=%lu source=%lu MB\r\n",
                 idx, arch,
                 (unsigned long)mi.block_count,
                 (unsigned long)mi.embedding_length,
                 (unsigned long)mi.head_count,
                 (unsigned long)mi.head_count_kv,
                 (unsigned long)mi.head_dim,
                 (unsigned long)mi.vocab_size,
                 (unsigned long)mi.context_length,
                 (unsigned long)(mi.source_bytes / (1024u * 1024u)));
    shell_printf("[slm] zero-copy from boot memory, %llu ms\r\n",
                 (unsigned long long)load_ms);
    return 0;
}
