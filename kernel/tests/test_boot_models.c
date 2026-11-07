/*
 * test_boot_models.c - UEFI model delivery (ADR 0002)
 *
 * Two halves:
 *   - The EFI stub's loader (efi_model.c), run for real against a fake
 *     firmware: a boot services table, volumes and files whose failures
 *     are chosen per test. Real firmware is covered by `make
 *     test-qemu-uefi`, but it can only refuse an allocation or fail a
 *     read in the ways this table can make it do on demand, so the
 *     exact failure paths are pinned here.
 *   - The kernel side (boot_models.c): checks, reservation, the read-only
 *     filesystem and the zero-copy `slm load`.
 *
 * Built for the same platforms as efi_stub.c.
 */

#include "unity.h"
#include "../arch/arm64/efi.h"
#include "../include/boot_models.h"
#include "../include/pmm.h"
#include "../include/slm_ffi.h"
#include "../include/string.h"
#include "../include/vfs.h"
#include <stdint.h>
#include <stddef.h>

/* ------------------------------------------------------------------ */
/* Fake firmware                                                       */
/* ------------------------------------------------------------------ */

#define WINDOW_BYTES    (16ULL * 1024 * 1024)
#define WINDOW_PAGES    (WINDOW_BYTES / EFI_PAGE_SIZE)
#define MAX_FILES       6
#define MAX_VOLS        3
#define MAX_ALLOCS      8
#define MAX_OPEN        32
#define POOL_BYTES      (64 * 1024)
#define NO_FAULT        UINT64_MAX

struct fake_file {
    uint16_t name[80];
    const uint8_t *data;
    uint64_t size;
    uint64_t reported_size;     /* what the directory entry claims */
    uint64_t read_error_at;     /* EFI_DEVICE_ERROR at this offset */
    uint64_t eof_at;            /* pretend the file ends here */
    uint32_t stall_ms;          /* per Read call */
    bool is_dir;
};

struct fake_volume {
    bool nvme;
    bool has_dir;
    bool hidden;                /* appears only after ConnectController */
    uint32_t n_files;
    struct fake_file files[MAX_FILES];
};

struct fake_handle {
    efi_file_protocol_t p;      /* first: the protocol pointer is this */
    struct fake_volume *vol;
    int file;                   /* -1 root, -2 models dir, else index */
    uint64_t pos;
    uint32_t cursor;
    bool open;
};

static struct {
    struct fake_volume vols[MAX_VOLS];
    uint32_t n_vols;
    uint64_t base;              /* start of the window (PMM pages) */
    struct { uint64_t addr, pages; } allocs[MAX_ALLOCS];
    uint32_t n_allocs;
    uint32_t alloc_calls;
    uint32_t refuse_allocs;     /* refuse this many, then succeed */
    uint32_t connect_calls;
    uint32_t reads;
    uint32_t reads_past_magic;
    struct fake_handle handles[MAX_OPEN];
    uint8_t pool[POOL_BYTES];
    uint32_t pool_used;
    int32_t pools_out;
} fw;

/* Unity asserts `return;`, so the value-returning fakes can't use them.
 * They count broken expectations here and assert_tidy() checks it. */
static uint32_t fw_bad;
#define FAKE_CHECK(cond) do { if (!(cond)) { fw_bad++; } } while (0)

static efi_simple_file_system_protocol_t fake_sfs[MAX_VOLS];
static efi_boot_services_t fake_bs;
static efi_system_table_t fake_st;

static bool guid_is(const efi_guid_t *g, uint32_t data1)
{
    return g->data1 == data1;
}
#define SFS_DATA1   0x964e5b22U
#define BLK_DATA1   0x964e5b21U
#define DP_DATA1    0x09576e91U

/* Conventional memory with the outstanding allocations cut out. */
static uint32_t fake_build_map(efi_memory_desc_t *out)
{
    uint32_t n = 0;
    uint64_t at = fw.base;
    uint64_t end = fw.base + WINDOW_BYTES;

    /* Allocations in address order. Few enough for a selection walk. */
    for (;;) {
        uint64_t best = UINT64_MAX, best_pages = 0;
        for (uint32_t i = 0; i < fw.n_allocs; i++) {
            if (fw.allocs[i].pages != 0 && fw.allocs[i].addr >= at &&
                fw.allocs[i].addr < best) {
                best = fw.allocs[i].addr;
                best_pages = fw.allocs[i].pages;
            }
        }
        uint64_t stop = best == UINT64_MAX ? end : best;
        if (stop > at && out != NULL) {
            out[n] = (efi_memory_desc_t){
                .type = EFI_CONVENTIONAL_MEMORY, .phys_start = at,
                .num_pages = (stop - at) / EFI_PAGE_SIZE };
        }
        if (stop > at) {
            n++;
        }
        if (best == UINT64_MAX) {
            return n;
        }
        if (out != NULL) {
            out[n] = (efi_memory_desc_t){
                .type = EFI_LOADER_DATA, .phys_start = best,
                .num_pages = best_pages };
        }
        n++;
        at = best + best_pages * EFI_PAGE_SIZE;
    }
}

static efi_status_t fake_get_memory_map(uint64_t *map_size,
                                        efi_memory_desc_t *map,
                                        uint64_t *map_key, uint64_t *desc_size,
                                        uint32_t *desc_version)
{
    uint64_t need = fake_build_map(NULL) * sizeof(efi_memory_desc_t);
    *desc_size = sizeof(efi_memory_desc_t);
    *desc_version = 1;
    *map_key = 1;
    if (*map_size < need || map == NULL) {
        *map_size = need;
        return EFI_BUFFER_TOO_SMALL;
    }
    fake_build_map(map);
    *map_size = need;
    return EFI_SUCCESS;
}

static efi_status_t fake_allocate_pool(uint32_t type, uint64_t size, void **buf)
{
    (void)type;
    size = (size + 15) & ~15ULL;
    if (fw.pool_used + size > POOL_BYTES) {
        fw.pool_used = 0;   /* a bump arena is enough: nothing here outlives a call */
    }
    FAKE_CHECK(size <= POOL_BYTES);
    *buf = &fw.pool[fw.pool_used];
    fw.pool_used += (uint32_t)size;
    fw.pools_out++;
    return EFI_SUCCESS;
}

static efi_status_t fake_free_pool(void *buf)
{
    (void)buf;
    fw.pools_out--;
    return EFI_SUCCESS;
}

static efi_status_t fake_allocate_pages(uint32_t alloc_type, uint32_t mem_type,
                                        uint64_t pages, efi_phys_addr_t *mem)
{
    fw.alloc_calls++;
    FAKE_CHECK(alloc_type == EFI_ALLOCATE_ADDRESS);
    FAKE_CHECK(mem_type == EFI_LOADER_DATA);
    if (fw.refuse_allocs > 0) {
        fw.refuse_allocs--;
        return EFI_OUT_OF_RESOURCES;
    }
    if (fw.n_allocs >= MAX_ALLOCS) {
        fw_bad++;
        return EFI_OUT_OF_RESOURCES;
    }
    fw.allocs[fw.n_allocs].addr = *mem;
    fw.allocs[fw.n_allocs].pages = pages;
    fw.n_allocs++;
    return EFI_SUCCESS;
}

static efi_status_t fake_free_pages(efi_phys_addr_t mem, uint64_t pages)
{
    for (uint32_t i = 0; i < fw.n_allocs; i++) {
        if (fw.allocs[i].addr == mem && fw.allocs[i].pages == pages) {
            fw.allocs[i].pages = 0;
            return EFI_SUCCESS;
        }
    }
    fw_bad++;   /* FreePages on a range never allocated */
    return EFI_NOT_FOUND;
}

static uint32_t outstanding_allocs(void)
{
    uint32_t n = 0;
    for (uint32_t i = 0; i < fw.n_allocs; i++) {
        if (fw.allocs[i].pages != 0) {
            n++;
        }
    }
    return n;
}

static uint32_t open_handles(void)
{
    uint32_t n = 0;
    for (uint32_t i = 0; i < MAX_OPEN; i++) {
        if (fw.handles[i].open) {
            n++;
        }
    }
    return n;
}

static efi_status_t fake_read(efi_file_protocol_t *self, uint64_t *size, void *buf);
static efi_status_t fake_close(efi_file_protocol_t *self);
static efi_status_t fake_open(efi_file_protocol_t *self,
                              efi_file_protocol_t **out, const uint16_t *name,
                              uint64_t mode, uint64_t attrs);
static efi_status_t fake_set_position(efi_file_protocol_t *self, uint64_t pos);

static struct fake_handle *fake_new_handle(struct fake_volume *v, int file)
{
    for (uint32_t i = 0; i < MAX_OPEN; i++) {
        struct fake_handle *h = &fw.handles[i];
        if (!h->open) {
            *h = (struct fake_handle){ .vol = v, .file = file, .open = true };
            h->p.open = fake_open;
            h->p.close = fake_close;
            h->p.read = fake_read;
            h->p.set_position = fake_set_position;
            return h;
        }
    }
    fw_bad++;   /* too many fake handles */
    return &fw.handles[0];
}

static bool wide_eq(const uint16_t *a, const uint16_t *b)
{
    while (*a != 0 && *a == *b) {
        a++;
        b++;
    }
    return *a == *b;
}

static efi_status_t fake_open(efi_file_protocol_t *self,
                              efi_file_protocol_t **out, const uint16_t *name,
                              uint64_t mode, uint64_t attrs)
{
    struct fake_handle *h = (struct fake_handle *)self;
    (void)attrs;
    FAKE_CHECK(mode == EFI_FILE_MODE_READ);
    if (h->file == -1) {
        static const uint16_t dir[] = u"\\slmos\\models";
        if (!h->vol->has_dir || !wide_eq(name, dir)) {
            return EFI_NOT_FOUND;
        }
        *out = &fake_new_handle(h->vol, -2)->p;
        return EFI_SUCCESS;
    }
    for (uint32_t i = 0; i < h->vol->n_files; i++) {
        if (wide_eq(name, h->vol->files[i].name)) {
            *out = &fake_new_handle(h->vol, (int)i)->p;
            return EFI_SUCCESS;
        }
    }
    return EFI_NOT_FOUND;
}

static efi_status_t fake_close(efi_file_protocol_t *self)
{
    ((struct fake_handle *)self)->open = false;
    return EFI_SUCCESS;
}

static efi_status_t fake_set_position(efi_file_protocol_t *self, uint64_t pos)
{
    ((struct fake_handle *)self)->pos = pos;
    return EFI_SUCCESS;
}

static void fake_spin_ms(uint32_t ms)
{
    uint64_t f, t0, t;
    __asm__ volatile("mrs %0, cntfrq_el0" : "=r"(f));
    __asm__ volatile("mrs %0, cntpct_el0" : "=r"(t0));
    do {
        __asm__ volatile("isb\n mrs %0, cntpct_el0" : "=r"(t));
    } while ((t - t0) * 1000 / f < ms);
}

static efi_status_t fake_read(efi_file_protocol_t *self, uint64_t *size, void *buf)
{
    struct fake_handle *h = (struct fake_handle *)self;

    fw.reads++;
    if (h->file == -2) {
        efi_file_info_t *info = (efi_file_info_t *)buf;
        if (h->cursor >= h->vol->n_files) {
            *size = 0;
            return EFI_SUCCESS;
        }
        struct fake_file *f = &h->vol->files[h->cursor++];
        FAKE_CHECK(*size >= sizeof(*info) + sizeof(f->name));
        memset(info, 0, sizeof(*info));
        info->file_size = f->reported_size;
        info->attribute = f->is_dir ? EFI_FILE_ATTR_DIRECTORY : 0;
        memcpy(info->file_name, f->name, sizeof(f->name));
        *size = sizeof(*info) + sizeof(f->name);
        return EFI_SUCCESS;
    }

    struct fake_file *f = &h->vol->files[h->file];
    uint64_t end = f->eof_at < f->size ? f->eof_at : f->size;
    if (f->stall_ms != 0) {
        fake_spin_ms(f->stall_ms);
    }
    if (h->pos > 0) {
        fw.reads_past_magic++;
    }
    if (f->read_error_at <= h->pos ||
        (f->read_error_at < h->pos + *size)) {
        *size = 0;
        return EFI_DEVICE_ERROR;
    }
    uint64_t avail = h->pos < end ? end - h->pos : 0;
    uint64_t n = *size < avail ? *size : avail;
    memcpy(buf, f->data + h->pos, n);
    h->pos += n;
    *size = n;
    return EFI_SUCCESS;
}

static efi_status_t fake_open_volume(efi_simple_file_system_protocol_t *self,
                                     efi_file_protocol_t **root)
{
    struct fake_volume *v = &fw.vols[self - fake_sfs];
    *root = &fake_new_handle(v, -1)->p;
    return EFI_SUCCESS;
}

/* Device paths: one node (NVMe or SD) plus End. */
static const efi_device_path_t dp_nvme[2] = {
    { EFI_DP_TYPE_MESSAGING, EFI_DP_MSG_NVME, { 4, 0 } },
    { EFI_DP_TYPE_END, 0xFF, { 4, 0 } },
};
static const efi_device_path_t dp_sd[2] = {
    { EFI_DP_TYPE_MESSAGING, 26, { 4, 0 } },
    { EFI_DP_TYPE_END, 0xFF, { 4, 0 } },
};

static efi_status_t fake_handle_protocol(efi_handle_t handle,
                                         const efi_guid_t *guid, void **out)
{
    struct fake_volume *v = (struct fake_volume *)handle;
    uint32_t idx = (uint32_t)(v - fw.vols);

    FAKE_CHECK(idx < fw.n_vols);
    if (guid_is(guid, SFS_DATA1)) {
        *out = &fake_sfs[idx];
        return EFI_SUCCESS;
    }
    if (guid_is(guid, DP_DATA1)) {
        *out = (void *)(v->nvme ? dp_nvme : dp_sd);
        return EFI_SUCCESS;
    }
    return EFI_NOT_FOUND;
}

static efi_status_t fake_locate_handle_buffer(uint32_t search,
                                              const efi_guid_t *guid, void *key,
                                              uint64_t *n, efi_handle_t **out)
{
    (void)key;
    FAKE_CHECK(search == EFI_LOCATE_BY_PROTOCOL);
    bool blk = guid_is(guid, BLK_DATA1);
    FAKE_CHECK(blk || guid_is(guid, SFS_DATA1));

    void *mem;
    fake_allocate_pool(0, sizeof(efi_handle_t) * MAX_VOLS, &mem);
    efi_handle_t *list = mem;
    uint64_t count = 0;
    for (uint32_t i = 0; i < fw.n_vols; i++) {
        if (blk || !fw.vols[i].hidden) {
            list[count++] = &fw.vols[i];
        }
    }
    *n = count;
    *out = list;
    return EFI_SUCCESS;
}

static efi_status_t fake_connect_controller(efi_handle_t h, efi_handle_t *d,
                                            void *rp, bool recursive)
{
    (void)d;
    (void)rp;
    FAKE_CHECK(recursive);
    fw.connect_calls++;
    ((struct fake_volume *)h)->hidden = false;
    return EFI_SUCCESS;
}

/* ------------------------------------------------------------------ */
/* Fixtures                                                            */
/* ------------------------------------------------------------------ */

static uint8_t *window_pages;
static uint8_t *src_pages;              /* file contents live here */
#define SRC_BYTES   (256 * 1024)
#define SRC_PAGES   (SRC_BYTES / 4096)

static struct boot_model_table table;
static efi_models_stats_t stats;

static efi_models_cfg_t test_cfg(void)
{
    efi_models_cfg_t c = { 0 };
    c.floor = fw.base;
    c.limit = fw.base + WINDOW_BYTES;
    c.chunk_bytes = 16 * 1024;
    return c;
}

static void fw_reset(void)
{
    memset(&fw, 0, sizeof(fw));
    fw_bad = 0;
    memset(&table, 0, sizeof(table));
    memset(&stats, 0, sizeof(stats));
    fw.base = (uint64_t)(uintptr_t)window_pages;
    fake_bs = (efi_boot_services_t){
        .allocate_pages = fake_allocate_pages,
        .free_pages = fake_free_pages,
        .get_memory_map = fake_get_memory_map,
        .allocate_pool = fake_allocate_pool,
        .free_pool = fake_free_pool,
        .handle_protocol = fake_handle_protocol,
        .connect_controller = fake_connect_controller,
        .locate_handle_buffer = fake_locate_handle_buffer,
    };
    fake_st = (efi_system_table_t){ .boot_services = &fake_bs };
    for (uint32_t i = 0; i < MAX_VOLS; i++) {
        fake_sfs[i].open_volume = fake_open_volume;
    }
    /* Host-independent file content with a GGUF magic. */
    for (uint32_t i = 0; i < SRC_BYTES; i++) {
        src_pages[i] = (uint8_t)(i * 31u + (i >> 8));
    }
    memcpy(src_pages, "GGUF", 4);
}

static struct fake_volume *add_volume(bool nvme, bool has_dir)
{
    struct fake_volume *v = &fw.vols[fw.n_vols++];
    v->nvme = nvme;
    v->has_dir = has_dir;
    return v;
}

static struct fake_file *add_file(struct fake_volume *v, const char *name,
                                  const uint8_t *data, uint64_t size)
{
    struct fake_file *f = &v->files[v->n_files++];
    size_t i = 0;
    for (; name[i] != 0; i++) {
        f->name[i] = (uint16_t)(uint8_t)name[i];
    }
    f->name[i] = 0;
    f->data = data;
    f->size = size;
    f->reported_size = size;
    f->read_error_at = NO_FAULT;
    f->eof_at = NO_FAULT;
    return f;
}

static void run_loader(void)
{
    efi_models_cfg_t c = test_cfg();
    efi_models_load(&fake_st, &c, &table, &stats);
}

/* No file left open, no pool leaked, on every path. */
static void assert_tidy(void)
{
    TEST_ASSERT_EQUAL_UINT32(0, fw_bad);
    TEST_ASSERT_EQUAL_UINT32(0, open_handles());
    TEST_ASSERT_EQUAL_INT32(0, fw.pools_out);
}

/* ------------------------------------------------------------------ */
/* Stub: normal paths                                                  */
/* ------------------------------------------------------------------ */

static void test_stub_loads_a_file_intact(void)
{
    fw_reset();
    add_file(add_volume(true, true), "a.gguf", src_pages, SRC_BYTES);
    run_loader();
    assert_tidy();

    TEST_ASSERT_EQUAL_UINT32(1, stats.loaded);
    TEST_ASSERT_EQUAL_UINT32(1, table.count);
    const struct boot_model *m = &table.models[0];
    TEST_ASSERT_EQUAL_STRING("a.gguf", m->name);
    TEST_ASSERT_EQUAL_UINT64(SRC_BYTES, m->size);
    TEST_ASSERT_EQUAL_UINT64(0, m->phys & (BOOT_MODEL_ALIGN - 1));
    TEST_ASSERT_TRUE(m->phys >= fw.base);
    TEST_ASSERT_TRUE(m->phys + m->size <= fw.base + WINDOW_BYTES);
    TEST_ASSERT_EQUAL_INT(0, memcmp((void *)(uintptr_t)m->phys, src_pages, SRC_BYTES));
    TEST_ASSERT_EQUAL_UINT64(boot_model_hash(src_pages, SRC_BYTES), m->hash);
    TEST_ASSERT_EQUAL_UINT32(1, outstanding_allocs());
}

static void test_stub_prefers_nvme_and_dedupes_by_name(void)
{
    fw_reset();
    /* SD enumerates first; its copy must lose to the NVMe one. */
    static uint8_t other[4096];
    memset(other, 0xAB, sizeof(other));
    memcpy(other, "GGUF", 4);
    add_file(add_volume(false, true), "m.gguf", other, sizeof(other));
    add_file(add_volume(true, true), "m.gguf", src_pages, 4096);
    run_loader();
    assert_tidy();

    TEST_ASSERT_EQUAL_UINT32(1, table.count);
    TEST_ASSERT_EQUAL_INT(0, memcmp((void *)(uintptr_t)table.models[0].phys,
                                    src_pages, 4096));
    TEST_ASSERT_EQUAL_UINT32(1, outstanding_allocs());
}

/* FAT names are case-insensitive: the SD's M.GGUF is the NVMe's m.gguf, and
 * loading both put the model in the table twice. */
static void test_stub_dedupes_names_that_differ_only_in_case(void)
{
    fw_reset();
    static uint8_t other[4096];
    memset(other, 0xCD, sizeof(other));
    memcpy(other, "GGUF", 4);
    add_file(add_volume(false, true), "M.GGUF", other, sizeof(other));
    add_file(add_volume(true, true), "m.gguf", src_pages, 4096);
    run_loader();
    assert_tidy();

    TEST_ASSERT_EQUAL_UINT32(1, table.count);
    TEST_ASSERT_EQUAL_STRING("m.gguf", table.models[0].name);
    TEST_ASSERT_EQUAL_INT(0, memcmp((void *)(uintptr_t)table.models[0].phys,
                                    src_pages, 4096));
    TEST_ASSERT_EQUAL_UINT32(1, outstanding_allocs());
}

static void test_stub_falls_back_to_the_next_volume(void)
{
    fw_reset();
    struct fake_file *f = add_file(add_volume(true, true), "m.gguf", src_pages, SRC_BYTES);
    f->read_error_at = 64 * 1024;
    add_file(add_volume(false, true), "m.gguf", src_pages, SRC_BYTES);
    run_loader();
    assert_tidy();

    TEST_ASSERT_EQUAL_UINT32(1, stats.read_errors);
    TEST_ASSERT_EQUAL_UINT32(1, stats.loaded);
    TEST_ASSERT_EQUAL_UINT32(1, table.count);
    TEST_ASSERT_EQUAL_UINT32(1, outstanding_allocs());
}

static void test_stub_ignores_what_is_not_a_model(void)
{
    fw_reset();
    struct fake_volume *v = add_volume(true, true);
    add_file(v, "README.txt", src_pages, 4096);
    add_file(v, ".gguf", src_pages, 4096);
    add_file(v, "dir.gguf", src_pages, 4096)->is_dir = true;
    add_file(v, "UPPER.GGUF", src_pages, 4096);
    run_loader();
    assert_tidy();

    TEST_ASSERT_EQUAL_UINT32(1, table.count);
    TEST_ASSERT_EQUAL_STRING("UPPER.GGUF", table.models[0].name);
}

static void test_stub_bounds_the_file_count(void)
{
    fw_reset();
    struct fake_volume *v = add_volume(true, true);
    add_file(v, "1.gguf", src_pages, 4096);
    add_file(v, "2.gguf", src_pages, 4096);
    add_file(v, "3.gguf", src_pages, 4096);
    add_file(v, "4.gguf", src_pages, 4096);
    add_file(v, "5.gguf", src_pages, 4096);
    run_loader();
    assert_tidy();

    TEST_ASSERT_EQUAL_UINT32(BOOT_MODELS_MAX, table.count);
    TEST_ASSERT_EQUAL_UINT32(1, stats.table_full);
    TEST_ASSERT_TRUE(table.models[0].phys != table.models[1].phys);
}

static void test_stub_connects_block_devices_first(void)
{
    fw_reset();
    struct fake_volume *v = add_volume(true, true);
    v->hidden = true;
    add_file(v, "m.gguf", src_pages, 4096);
    run_loader();
    assert_tidy();

    TEST_ASSERT_TRUE(fw.connect_calls > 0);
    TEST_ASSERT_EQUAL_UINT32(1, table.count);
}

/* A volume that binds only on connect can still be the preferred one,
 * though another volume already offers the file. */
static void test_stub_late_bound_nvme_still_wins(void)
{
    fw_reset();
    static uint8_t other[4096];
    memset(other, 0xCD, sizeof(other));
    memcpy(other, "GGUF", 4);
    add_file(add_volume(false, true), "m.gguf", other, sizeof(other));
    struct fake_volume *v = add_volume(true, true);
    v->hidden = true;
    add_file(v, "m.gguf", src_pages, 4096);
    run_loader();
    assert_tidy();
    TEST_ASSERT_EQUAL_UINT32(1, table.count);
    TEST_ASSERT_EQUAL_INT(0, memcmp((void *)(uintptr_t)table.models[0].phys,
                                    src_pages, 4096));
}

static void test_stub_release_returns_pages(void)
{
    fw_reset();
    add_file(add_volume(true, true), "m.gguf", src_pages, 4096);
    run_loader();
    TEST_ASSERT_EQUAL_UINT32(1, outstanding_allocs());
    efi_models_release(&fake_st, &table);
    TEST_ASSERT_EQUAL_UINT32(0, outstanding_allocs());
    TEST_ASSERT_EQUAL_UINT32(0, table.count);
}

/* ------------------------------------------------------------------ */
/* Stub: failure paths. All non-fatal, all leave the pages free.        */
/* ------------------------------------------------------------------ */

static void test_stub_no_volumes(void)
{
    fw_reset();
    run_loader();
    assert_tidy();
    TEST_ASSERT_EQUAL_UINT32(0, table.count);
    TEST_ASSERT_EQUAL_UINT32(0, fw.alloc_calls);
}

static void test_stub_no_directory(void)
{
    fw_reset();
    add_volume(true, false);
    add_volume(false, false);
    run_loader();
    assert_tidy();
    TEST_ASSERT_EQUAL_UINT32(0, table.count);
    TEST_ASSERT_EQUAL_UINT32(0, fw.alloc_calls);
    TEST_ASSERT_EQUAL_UINT32(2, stats.volumes_seen);
}

static void test_stub_read_error_frees_pages(void)
{
    fw_reset();
    add_file(add_volume(true, true), "m.gguf", src_pages, SRC_BYTES)
        ->read_error_at = 128 * 1024;
    run_loader();
    assert_tidy();
    TEST_ASSERT_EQUAL_UINT32(1, stats.read_errors);
    TEST_ASSERT_EQUAL_UINT32(0, table.count);
    TEST_ASSERT_EQUAL_UINT32(1, fw.alloc_calls);
    TEST_ASSERT_EQUAL_UINT32(0, outstanding_allocs());
}

static void test_stub_file_shorter_than_its_entry_frees_pages(void)
{
    fw_reset();
    add_file(add_volume(true, true), "m.gguf", src_pages, SRC_BYTES)
        ->eof_at = 100 * 1024;
    run_loader();
    assert_tidy();
    TEST_ASSERT_EQUAL_UINT32(1, stats.read_errors);
    TEST_ASSERT_EQUAL_UINT32(0, table.count);
    TEST_ASSERT_EQUAL_UINT32(0, outstanding_allocs());
}

static void test_stub_too_big_over_the_registry_cap(void)
{
    fw_reset();
    add_file(add_volume(true, true), "m.gguf", src_pages, SRC_BYTES)
        ->reported_size = BOOT_MODEL_MAX_BYTES + 1;
    run_loader();
    assert_tidy();
    TEST_ASSERT_EQUAL_UINT32(1, stats.too_big);
    TEST_ASSERT_EQUAL_UINT32(0, fw.alloc_calls);
    TEST_ASSERT_EQUAL_UINT32(0, fw.reads_past_magic);
}

static void test_stub_too_big_over_half_the_window(void)
{
    fw_reset();
    struct fake_volume *v = add_volume(true, true);
    add_file(v, "big.gguf", src_pages, SRC_BYTES)
        ->reported_size = WINDOW_BYTES / 2 + 1;
    add_file(v, "small.gguf", src_pages, 4096);
    run_loader();
    assert_tidy();
    TEST_ASSERT_EQUAL_UINT32(1, stats.too_big);
    /* Skipping one file doesn't stop the next. */
    TEST_ASSERT_EQUAL_UINT32(1, table.count);
    TEST_ASSERT_EQUAL_STRING("small.gguf", table.models[0].name);
}

static void test_stub_empty_file_is_skipped(void)
{
    fw_reset();
    add_file(add_volume(true, true), "m.gguf", src_pages, 0);
    run_loader();
    assert_tidy();
    TEST_ASSERT_EQUAL_UINT32(0, table.count);
    TEST_ASSERT_EQUAL_UINT32(0, fw.alloc_calls);
}

static void test_stub_alloc_refused_every_time(void)
{
    fw_reset();
    fw.refuse_allocs = 1000;
    add_file(add_volume(true, true), "m.gguf", src_pages, SRC_BYTES);
    run_loader();
    assert_tidy();
    TEST_ASSERT_EQUAL_UINT32(1, stats.no_memory);
    TEST_ASSERT_EQUAL_UINT32(0, table.count);
    /* Bounded: EFI_MODEL_ALLOC_TRIES in efi_model.c. */
    TEST_ASSERT_EQUAL_UINT32(4, fw.alloc_calls);
}

static void test_stub_alloc_refused_once_moves_lower(void)
{
    fw_reset();
    fw.refuse_allocs = 1;
    add_file(add_volume(true, true), "m.gguf", src_pages, SRC_BYTES);
    run_loader();
    assert_tidy();
    TEST_ASSERT_EQUAL_UINT32(1, table.count);
    TEST_ASSERT_EQUAL_UINT32(2, fw.alloc_calls);
    /* Below the first (top) candidate. */
    TEST_ASSERT_TRUE(table.models[0].phys + BOOT_MODEL_ALIGN + SRC_BYTES
                     <= fw.base + WINDOW_BYTES);
}

static void test_stub_not_a_gguf_costs_no_memory(void)
{
    fw_reset();
    static uint8_t junk[4096];
    memset(junk, 0x55, sizeof(junk));
    add_file(add_volume(true, true), "m.gguf", junk, sizeof(junk));
    run_loader();
    assert_tidy();
    TEST_ASSERT_EQUAL_UINT32(1, stats.bad_magic);
    TEST_ASSERT_EQUAL_UINT32(0, fw.alloc_calls);
}

static void test_stub_read_budget_stops_a_crawling_file(void)
{
    fw_reset();
    add_file(add_volume(true, true), "m.gguf", src_pages, SRC_BYTES)
        ->stall_ms = 1100;
    efi_models_cfg_t c = test_cfg();
    c.budget_secs = 1;
    c.chunk_bytes = 64 * 1024;
    efi_models_load(&fake_st, &c, &table, &stats);
    assert_tidy();
    TEST_ASSERT_EQUAL_UINT32(1, stats.over_budget);
    TEST_ASSERT_EQUAL_UINT32(0, table.count);
    TEST_ASSERT_EQUAL_UINT32(0, outstanding_allocs());
}

static void test_stub_name_that_does_not_fit_is_skipped(void)
{
    fw_reset();
    /* 68 characters: longer than BOOT_MODEL_NAME_LEN - 1. */
    add_file(add_volume(true, true),
             "0123456789012345678901234567890123456789012345678901234567890123.gguf"
             , src_pages, 4096);
    run_loader();
    assert_tidy();
    TEST_ASSERT_EQUAL_UINT32(1, stats.bad_name);
    TEST_ASSERT_EQUAL_UINT32(0, table.count);
}

/* ------------------------------------------------------------------ */
/* Range picker: the Jetson-shaped map                                  */
/* ------------------------------------------------------------------ */

#define J_LINK      0x80000000ULL
#define J_IMAGE     0x01950000ULL
#define J_FLOOR     0x100000000ULL
#define J_LIMIT     0x240000000ULL
#define J_MODEL     0x42000000ULL   /* 1.03 GiB */

static efi_memory_desc_t pdesc(uint32_t type, uint64_t start, uint64_t bytes)
{
    return (efi_memory_desc_t){ .type = type, .phys_start = start,
                                .num_pages = bytes / EFI_PAGE_SIZE };
}

static uint64_t pick(const efi_memory_desc_t *map, size_t n, uint64_t size,
                     const efi_models_cfg_t *c)
{
    return efi_model_pick_range(map, n * sizeof(*map), sizeof(*map), size, c);
}

static efi_models_cfg_t jetson_cfg(void)
{
    efi_models_cfg_t c = { .floor = J_FLOOR, .limit = J_LIMIT };
    c.excl_start[0] = J_LINK;
    c.excl_len[0] = J_IMAGE;
    return c;
}

static void test_pick_stays_inside_the_identity_map(void)
{
    /* Conventional memory runs past the map's end, as it does on the
     * board: the pick must not use the part above 0x240000000. */
    efi_memory_desc_t map[] = {
        pdesc(EFI_CONVENTIONAL_MEMORY, J_LINK, 0x3E000000ULL),
        pdesc(EFI_CONVENTIONAL_MEMORY, J_FLOOR, 0x15C000000ULL),
    };
    efi_models_cfg_t c = jetson_cfg();
    uint64_t a = pick(map, 2, J_MODEL, &c);
    TEST_ASSERT_TRUE(a >= J_FLOOR);
    TEST_ASSERT_TRUE(a + J_MODEL <= J_LIMIT);
    TEST_ASSERT_EQUAL_UINT64(0, a & (BOOT_MODEL_ALIGN - 1));
    /* As high as it can go. */
    TEST_ASSERT_EQUAL_UINT64((J_LIMIT - J_MODEL) & ~(BOOT_MODEL_ALIGN - 1), a);
}

static void test_pick_never_uses_low_ram_or_the_link_range(void)
{
    /* Only low RAM is free: inside the kernel link range and below the
     * floor. Nothing is acceptable. */
    efi_memory_desc_t map[] = {
        pdesc(EFI_CONVENTIONAL_MEMORY, J_LINK, 0x7E000000ULL),
    };
    efi_models_cfg_t c = jetson_cfg();
    TEST_ASSERT_EQUAL_UINT64(0, pick(map, 1, J_MODEL, &c));

    /* With the floor lowered, the exclusion is what keeps it clear. */
    c.floor = J_LINK;
    uint64_t a = pick(map, 1, 0x7E000000ULL - 0x2000000ULL, &c);
    TEST_ASSERT_TRUE(a >= J_LINK + J_IMAGE || a == 0);
}

static void test_pick_goes_below_an_excluded_range(void)
{
    efi_memory_desc_t map[] = {
        pdesc(EFI_CONVENTIONAL_MEMORY, 0x100000000ULL, 0x100000000ULL),
    };
    efi_models_cfg_t c = { .floor = 0x100000000ULL, .limit = 0x200000000ULL };
    c.excl_start[0] = 0x1F0000000ULL;   /* DTB-like, near the top */
    c.excl_len[0] = 0x100000ULL;
    uint64_t a = pick(map, 1, 0x10000000ULL, &c);
    TEST_ASSERT_TRUE(a != 0);
    TEST_ASSERT_TRUE(a + 0x10000000ULL <= 0x1F0000000ULL);
}

static void test_pick_ignores_memory_the_firmware_owns(void)
{
    efi_memory_desc_t map[] = {
        pdesc(EFI_BOOT_SERVICES_DATA, J_FLOOR, 0x80000000ULL),
        pdesc(EFI_RUNTIME_SERVICES_DATA, J_FLOOR + 0x80000000ULL, 0x80000000ULL),
        pdesc(EFI_LOADER_DATA, J_FLOOR + 0x100000000ULL, 0x40000000ULL),
    };
    efi_models_cfg_t c = jetson_cfg();
    TEST_ASSERT_EQUAL_UINT64(0, pick(map, 3, 0x1000000ULL, &c));
}

static void test_pick_needs_one_contiguous_descriptor(void)
{
    efi_memory_desc_t map[] = {
        pdesc(EFI_CONVENTIONAL_MEMORY, J_FLOOR, 0x20000000ULL),
        pdesc(EFI_RESERVED_MEMORY_TYPE, J_FLOOR + 0x20000000ULL, 0x200000ULL),
        pdesc(EFI_CONVENTIONAL_MEMORY, J_FLOOR + 0x20200000ULL, 0x20000000ULL),
    };
    efi_models_cfg_t c = jetson_cfg();
    TEST_ASSERT_EQUAL_UINT64(0, pick(map, 3, 0x30000000ULL, &c));
    TEST_ASSERT_EQUAL_UINT64(0, pick(map, 3, 0, &c));
}

static void test_pick_survives_a_wrapping_descriptor(void)
{
    efi_memory_desc_t map[] = {
        pdesc(EFI_CONVENTIONAL_MEMORY, J_FLOOR, 0),
    };
    map[0].num_pages = UINT64_MAX / 2;
    efi_models_cfg_t c = jetson_cfg();
    TEST_ASSERT_EQUAL_UINT64(0, pick(map, 1, 0x1000000ULL, &c));
}

/* A model is LoaderData at hand-off and must not disturb the relocation
 * check; it sits far above the link range. */
static void test_relocation_check_accepts_a_loaded_model(void)
{
    efi_memory_desc_t map[] = {
        pdesc(EFI_CONVENTIONAL_MEMORY, J_LINK, 0x3E000000ULL),
        pdesc(EFI_LOADER_DATA, 0x1FE000000ULL, J_MODEL),
        pdesc(EFI_LOADER_CODE, 0x25C863000ULL, 0x1960000ULL),
    };
    TEST_ASSERT_EQUAL_INT(EFI_RELOC_OK,
        efi_check_relocation(map, sizeof(map), sizeof(map[0]), 0x25C870000ULL,
                             J_LINK, J_IMAGE, 0x2679DF000ULL, 0xF39DCULL));
}

/* ------------------------------------------------------------------ */
/* Hash                                                                 */
/* ------------------------------------------------------------------ */

/* From the reference in scripts/gguf-boot-fixture.py. */
#define HASH_GOLDEN_0   0xB1B6A2946D646C1DULL
#define HASH_GOLDEN_31  0xCC7085BC4386227BULL
#define HASH_GOLDEN_71  0x5900E5FC8FBDD408ULL

static void test_hash_golden_values(void)
{
    uint8_t buf[71];
    for (uint32_t i = 0; i < sizeof(buf); i++) {
        buf[i] = (uint8_t)i;
    }
    /* Pinned: the stub and the kernel are separate compilations of this
     * inline, and a change in either would drop every model at boot. */
    TEST_ASSERT_EQUAL_HEX64(HASH_GOLDEN_0, boot_model_hash(buf, 0));
    TEST_ASSERT_EQUAL_HEX64(HASH_GOLDEN_31, boot_model_hash(buf, 31));
    TEST_ASSERT_EQUAL_HEX64(HASH_GOLDEN_71, boot_model_hash(buf, 71));
}

static void test_hash_sees_a_flip_anywhere_and_the_length(void)
{
    static uint8_t buf[4096];
    memset(buf, 0x11, sizeof(buf));
    uint64_t h = boot_model_hash(buf, sizeof(buf));
    TEST_ASSERT_TRUE(h != boot_model_hash(buf, sizeof(buf) - 1));
    for (uint32_t i = 0; i < sizeof(buf); i += 97) {
        buf[i] ^= 0x01;
        TEST_ASSERT_TRUE(h != boot_model_hash(buf, sizeof(buf)));
        buf[i] ^= 0x01;
    }
    TEST_ASSERT_EQUAL_HEX64(h, boot_model_hash(buf, sizeof(buf)));
}

/* ------------------------------------------------------------------ */
/* Kernel side                                                          */
/* ------------------------------------------------------------------ */

/* A real-enough GGUF in 2 MiB-aligned PMM pages, recorded in a table. */
#define KM_PAGES    512     /* 2 MiB, order 9: aligned to its size */
static uint8_t *km_pages;

static void km_record(struct boot_model *m, const char *name, size_t len)
{
    memset(m, 0, sizeof(*m));
    m->phys = (uint64_t)(uintptr_t)km_pages;
    m->size = len;
    m->hash = boot_model_hash(km_pages, len);
    strncpy(m->name, name, sizeof(m->name) - 1);
}

static void km_fixture(size_t *len_out)
{
    size_t n = 0;
    memset(km_pages, 0, KM_PAGES * 4096);
    /* Several pages, so a stray free would show in the PMM's count. */
    int rc = rust_slm_test_build_qwen_fixture(2048, km_pages, KM_PAGES * 4096, &n);
    TEST_ASSERT_EQUAL_INT(0, rc);
    *len_out = n;
}

static void test_verify_accepts_a_good_record(void)
{
    size_t n;
    km_fixture(&n);
    struct boot_model m;
    km_record(&m, "x.gguf", n);
    TEST_ASSERT_EQUAL_INT(0, boot_model_verify(&m));
}

static void test_verify_rejects_bad_records(void)
{
    size_t n;
    km_fixture(&n);
    struct boot_model m;

    km_record(&m, "x.gguf", n);
    m.size = 0;
    TEST_ASSERT_EQUAL_INT(-1, boot_model_verify(&m));

    km_record(&m, "x.gguf", n);
    m.size = BOOT_MODEL_MAX_BYTES + 1;
    TEST_ASSERT_EQUAL_INT(-1, boot_model_verify(&m));

    km_record(&m, "x.gguf", n);
    m.phys += 4096;     /* not 2 MiB aligned */
    TEST_ASSERT_EQUAL_INT(-1, boot_model_verify(&m));

    km_record(&m, "", n);
    TEST_ASSERT_EQUAL_INT(-1, boot_model_verify(&m));

    /* A name with no terminator: 64 bytes of 'a'. */
    km_record(&m, "x.gguf", n);
    memset(m.name, 'a', sizeof(m.name));
    TEST_ASSERT_EQUAL_INT(-1, boot_model_verify(&m));

    /* Outside anything the MMU maps. */
    km_record(&m, "x.gguf", n);
    m.phys = 0x7000000000ULL;
    TEST_ASSERT_EQUAL_INT(-2, boot_model_verify(&m));

    /* Hash mismatch: what a lost or stale cache line would look like. */
    km_record(&m, "x.gguf", n);
    km_pages[n / 2] ^= 0x40;
    TEST_ASSERT_EQUAL_INT(-4, boot_model_verify(&m));
    km_pages[n / 2] ^= 0x40;

    /* Bad magic, with a hash that matches the damage. */
    km_pages[0] = 'X';
    km_record(&m, "x.gguf", n);
    TEST_ASSERT_EQUAL_INT(-3, boot_model_verify(&m));
    km_pages[0] = 'G';
}

static void test_reserve_keeps_good_and_drops_bad(void)
{
    size_t n;
    km_fixture(&n);
    static struct boot_model_table t;

    pmm_user_reserve_reset();
    t.count = 3;
    km_record(&t.models[0], "bad.gguf", n);
    t.models[0].hash ^= 1;
    km_record(&t.models[1], "good.gguf", n);
    km_record(&t.models[2], "also-bad.gguf", n);
    t.models[2].phys = 0x7000000000ULL;

    boot_models_reserve_table(&t);

    TEST_ASSERT_EQUAL_UINT32(1, t.count);
    TEST_ASSERT_EQUAL_STRING("good.gguf", t.models[0].name);
    TEST_ASSERT_EQUAL_UINT64(1, pmm_user_reserve_count());
    pmm_user_reserve_reset();
}

static void test_find_path_matches_only_exact_boot_paths(void)
{
    static struct boot_model_table t;
    size_t n;
    km_fixture(&n);
    t.count = 1;
    km_record(&t.models[0], "qwen2.5-1.5b-instruct-q4_k_m.gguf", n);

    TEST_ASSERT_NOT_NULL(boot_models_find_path(
        &t, "/boot/models/qwen2.5-1.5b-instruct-q4_k_m.gguf"));
    TEST_ASSERT_NULL(boot_models_find_path(&t, "/boot/models/other.gguf"));
    TEST_ASSERT_NULL(boot_models_find_path(&t, "/boot/models"));
    TEST_ASSERT_NULL(boot_models_find_path(&t, "/boot/modelsx/qwen.gguf"));
    TEST_ASSERT_NULL(boot_models_find_path(
        &t, "/mnt/files/qwen2.5-1.5b-instruct-q4_k_m.gguf"));
    TEST_ASSERT_NULL(boot_models_find_path(&t, NULL));
}

/* Files in slmos_boot_models for one test, gone afterwards. */
#define LONG_NAME   "qwen2.5-1.5b-instruct-q4_k_m.gguf"
#define LONG_PATH   "/boot/models/" LONG_NAME

static void vfs_setup(size_t n)
{
    slmos_boot_models.count = 1;
    km_record(&slmos_boot_models.models[0], LONG_NAME, n);
    boot_models_mount();
    TEST_ASSERT_NOT_NULL(vfs_lookup("/boot/models"));
}

static void vfs_teardown(void)
{
    TEST_ASSERT_EQUAL_INT(0, vfs_unmount(vfs_lookup("/boot/models")));
    memset(&slmos_boot_models, 0, sizeof(slmos_boot_models));
}

static uint32_t list_seen;
static uint32_t list_size;
static char list_name[VFS_MAX_NAME];
static void list_cb(const struct vfs_entry_info *info, void *ctx)
{
    (void)ctx;
    list_seen++;
    list_size = info->size;
    strncpy(list_name, info->name, sizeof(list_name));
}

static void test_vfs_stat_read_list(void)
{
    size_t n;
    km_fixture(&n);
    vfs_setup(n);

    struct vfs_entry_info info;
    TEST_ASSERT_EQUAL_INT(0, vfs_stat_path(LONG_PATH, &info));
    TEST_ASSERT_EQUAL_UINT8(0, info.type);
    TEST_ASSERT_EQUAL_UINT32((uint32_t)n, info.size);
    TEST_ASSERT_EQUAL_STRING(LONG_NAME, info.name);
    TEST_ASSERT_EQUAL_INT(-1, vfs_stat_path("/boot/models/none.gguf", &info));

    /* Read from the middle, and past the end. */
    static char buf[64];
    TEST_ASSERT_EQUAL_INT(64, vfs_read_path(LONG_PATH, buf, sizeof(buf), 16));
    TEST_ASSERT_EQUAL_INT(0, memcmp(buf, km_pages + 16, sizeof(buf)));
    TEST_ASSERT_EQUAL_INT(10, vfs_read_path(LONG_PATH, buf, sizeof(buf), n - 10));
    TEST_ASSERT_EQUAL_INT(0, vfs_read_path(LONG_PATH, buf, sizeof(buf), n + 10));
    TEST_ASSERT_EQUAL_INT(-1, vfs_read_path("/boot/models/none", buf, 4, 0));

    list_seen = 0;
    TEST_ASSERT_EQUAL_INT(0, vfs_list_path("/boot/models", list_cb, NULL));
    TEST_ASSERT_EQUAL_UINT32(1, list_seen);
    TEST_ASSERT_EQUAL_UINT32((uint32_t)n, list_size);
    TEST_ASSERT_EQUAL_STRING(LONG_NAME, list_name);

    vfs_teardown();
}

/* Write verbs reach a filesystem through this call and treat NULL as
 * "not a mounted filesystem". Non-NULL here would be cast to an LFS
 * mount by `rm` and friends. */
static void test_vfs_is_read_only(void)
{
    size_t n;
    km_fixture(&n);
    vfs_setup(n);
    const char *sub = NULL;
    TEST_ASSERT_NULL(vfs_get_mount_ctx(LONG_PATH, &sub));
    vfs_teardown();
}

static void test_no_models_means_no_mount(void)
{
    memset(&slmos_boot_models, 0, sizeof(slmos_boot_models));
    boot_models_mount();
    TEST_ASSERT_NULL(vfs_lookup("/boot/models"));
    TEST_ASSERT_EQUAL_INT(BOOT_MODEL_NOT_HANDLED,
                          boot_models_slm_load("/boot/models/x.gguf"));
}

static void test_slm_load_is_zero_copy_and_unload_keeps_pages(void)
{
    size_t n;
    km_fixture(&n);
    rust_slm_test_reset();
    vfs_setup(n);
    const uint64_t hash = slmos_boot_models.models[0].hash;

    size_t free_before = pmm_get_free_pages();
    TEST_ASSERT_EQUAL_INT(0, boot_models_slm_load(LONG_PATH));
    TEST_ASSERT_EQUAL_UINT32(1, rust_slm_count());

    SlmModelInfoC info;
    TEST_ASSERT_EQUAL_INT(0, rust_slm_get_info(0, &info));
    TEST_ASSERT_EQUAL_UINT32(28u, info.block_count);
    TEST_ASSERT_EQUAL_UINT32(2048u, info.vocab_size);
    /* The full name minus the extension, not "qwen2". */
    TEST_ASSERT_EQUAL_INT(0, memcmp(info.name, "qwen2.5-1.5b-instruct-q4_k_m", 28));
    TEST_ASSERT_EQUAL_UINT8(0, info.name[28]);

    /* Unload: the pages stay allocated (not handed to the buddy) and
     * intact, and the file can be loaded again. A free would have grown
     * the free count by the file's pages. */
    TEST_ASSERT_TRUE(n > 4096);
    TEST_ASSERT_EQUAL_INT(0, rust_slm_unload(0));
    TEST_ASSERT_EQUAL_UINT64(free_before, pmm_get_free_pages());
    TEST_ASSERT_EQUAL_HEX64(hash, boot_model_hash(km_pages, n));
    TEST_ASSERT_EQUAL_INT(0, boot_models_slm_load(LONG_PATH));
    TEST_ASSERT_EQUAL_INT(0, rust_slm_unload(0));

    /* Damaged file: a shell error and no slot, and still no free. */
    km_pages[0] = 'X';
    TEST_ASSERT_EQUAL_INT(-1, boot_models_slm_load(LONG_PATH));
    TEST_ASSERT_EQUAL_UINT32(0, rust_slm_count());
    km_pages[0] = 'G';

    TEST_ASSERT_EQUAL_INT(BOOT_MODEL_NOT_HANDLED,
                          boot_models_slm_load("/mnt/files/x.gguf"));
    vfs_teardown();
    rust_slm_test_reset();
}

/* The default table stays empty on a non-UEFI boot, and in .data so the
 * stub's write survives the relocation and the BSS clear. */
static void test_table_is_empty_and_in_data(void)
{
    extern char __bss_start[], __bss_end[];
    const char *p = (const char *)&slmos_boot_models;
    TEST_ASSERT_EQUAL_UINT32(0, slmos_boot_models.count);
    TEST_ASSERT_TRUE(p + sizeof(slmos_boot_models) <= __bss_start || p >= __bss_end);
}

static void test_setup_buffers(void)
{
    TEST_ASSERT_NOT_NULL(window_pages);
    TEST_ASSERT_NOT_NULL(src_pages);
    TEST_ASSERT_NOT_NULL(km_pages);
}

int test_suite_boot_models(void)
{
    UnityBegin("test_boot_models.c");

    window_pages = pmm_alloc_pages(WINDOW_PAGES);
    src_pages = pmm_alloc_pages(SRC_PAGES);
    km_pages = pmm_alloc_pages(KM_PAGES);
    RUN_TEST(test_setup_buffers);
    if (window_pages == NULL || src_pages == NULL || km_pages == NULL) {
        return UnityEnd();
    }

    RUN_TEST(test_stub_loads_a_file_intact);
    RUN_TEST(test_stub_prefers_nvme_and_dedupes_by_name);
    RUN_TEST(test_stub_dedupes_names_that_differ_only_in_case);
    RUN_TEST(test_stub_falls_back_to_the_next_volume);
    RUN_TEST(test_stub_ignores_what_is_not_a_model);
    RUN_TEST(test_stub_bounds_the_file_count);
    RUN_TEST(test_stub_connects_block_devices_first);
    RUN_TEST(test_stub_late_bound_nvme_still_wins);
    RUN_TEST(test_stub_release_returns_pages);

    RUN_TEST(test_stub_no_volumes);
    RUN_TEST(test_stub_no_directory);
    RUN_TEST(test_stub_read_error_frees_pages);
    RUN_TEST(test_stub_file_shorter_than_its_entry_frees_pages);
    RUN_TEST(test_stub_too_big_over_the_registry_cap);
    RUN_TEST(test_stub_too_big_over_half_the_window);
    RUN_TEST(test_stub_empty_file_is_skipped);
    RUN_TEST(test_stub_alloc_refused_every_time);
    RUN_TEST(test_stub_alloc_refused_once_moves_lower);
    RUN_TEST(test_stub_not_a_gguf_costs_no_memory);
    RUN_TEST(test_stub_read_budget_stops_a_crawling_file);
    RUN_TEST(test_stub_name_that_does_not_fit_is_skipped);

    RUN_TEST(test_pick_stays_inside_the_identity_map);
    RUN_TEST(test_pick_never_uses_low_ram_or_the_link_range);
    RUN_TEST(test_pick_goes_below_an_excluded_range);
    RUN_TEST(test_pick_ignores_memory_the_firmware_owns);
    RUN_TEST(test_pick_needs_one_contiguous_descriptor);
    RUN_TEST(test_pick_survives_a_wrapping_descriptor);
    RUN_TEST(test_relocation_check_accepts_a_loaded_model);

    RUN_TEST(test_hash_golden_values);
    RUN_TEST(test_hash_sees_a_flip_anywhere_and_the_length);

    RUN_TEST(test_verify_accepts_a_good_record);
    RUN_TEST(test_verify_rejects_bad_records);
    RUN_TEST(test_reserve_keeps_good_and_drops_bad);
    RUN_TEST(test_find_path_matches_only_exact_boot_paths);
    /* First: an unmount leaves the node behind as a plain directory. */
    RUN_TEST(test_no_models_means_no_mount);
    RUN_TEST(test_vfs_stat_read_list);
    RUN_TEST(test_vfs_is_read_only);
    RUN_TEST(test_slm_load_is_zero_copy_and_unload_keeps_pages);
    RUN_TEST(test_table_is_empty_and_in_data);

    pmm_free_pages(window_pages, WINDOW_PAGES);
    pmm_free_pages(src_pages, SRC_PAGES);
    pmm_free_pages(km_pages, KM_PAGES);
    return UnityEnd();
}
