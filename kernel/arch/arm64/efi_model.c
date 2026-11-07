/*
 * efi_model.c - Read model files through UEFI before ExitBootServices.
 *
 * Jetson has no block driver at runtime, so this is how a model gets
 * into memory on a cold boot with no host involved
 * (docs/adr/0002-uefi-model-delivery.md). The stub walks every
 * SimpleFileSystem volume for \slmos\models\*.gguf, reads each file into
 * physically contiguous LoaderData pages, hashes it, cleans it to the
 * PoC by VA and records it in slmos_boot_models.
 *
 * Every failure here is non-fatal: the file is skipped with a console
 * line saying why, its pages go back to the firmware, and the boot
 * continues with whatever else was found. A later volume holding the
 * same file name is the fallback.
 *
 * This code runs from wherever UEFI loaded the image, before the
 * relocation: no absolute addresses (no static tables of pointers), and
 * nothing in .bss survives.
 */

#include "efi.h"
#include "boot_models.h"
#include <stddef.h>

#define EFI_MODEL_DIR_ENTRIES_MAX   64      /* per volume */
#define EFI_MODEL_VOLUMES_MAX       32
#define EFI_MODEL_DEFAULT_CHUNK     (32ULL * 1024 * 1024)
#define EFI_MODEL_PROGRESS_STEP     (128ULL * 1024 * 1024)
#define EFI_MODEL_ALLOC_TRIES       4
/* Well under the firmware's 5-minute boot-option watchdog, which remains
 * armed as the backstop for a Read call that never returns. */
#define EFI_MODEL_DEFAULT_BUDGET_S  180ULL

static uint64_t efi_model_now(void)
{
    uint64_t t;
    __asm__ volatile("isb\n mrs %0, cntpct_el0" : "=r"(t));
    return t;
}

static uint64_t efi_model_freq(void)
{
    uint64_t f;
    __asm__ volatile("mrs %0, cntfrq_el0" : "=r"(f));
    return f ? f : 1;
}

static uint32_t efi_model_ms_since(uint64_t start)
{
    return (uint32_t)((efi_model_now() - start) * 1000 / efi_model_freq());
}

static void efi_model_print_dec(efi_system_table_t *st, uint64_t v)
{
    efi_char16_t buf[21];
    int i = 20;
    buf[i] = 0;
    do {
        buf[--i] = (efi_char16_t)('0' + v % 10);
        v /= 10;
    } while (v != 0);
    efi_print(st, &buf[i]);
}

static void efi_model_print_hex(efi_system_table_t *st, uint64_t v)
{
    efi_char16_t buf[19];
    buf[0] = '0';
    buf[1] = 'x';
    for (int i = 0; i < 16; i++) {
        unsigned nibble = (v >> (60 - 4 * i)) & 0xF;
        buf[2 + i] = (efi_char16_t)(nibble < 10 ? '0' + nibble : 'A' + nibble - 10);
    }
    buf[18] = 0;
    efi_print(st, buf);
}

static void efi_model_print_ascii(efi_system_table_t *st, const char *s)
{
    efi_char16_t buf[2];
    buf[1] = 0;
    for (; *s != 0; s++) {
        buf[0] = (efi_char16_t)(uint8_t)*s;
        efi_print(st, buf);
    }
}

/* "[slmos] model <name>: <what>\r\n" */
static void efi_model_say(efi_system_table_t *st, const char *name,
                          const efi_char16_t *what)
{
    efi_print(st, u"[slmos] model ");
    efi_model_print_ascii(st, name);
    efi_print(st, u": ");
    efi_print(st, what);
    efi_print(st, u"\r\n");
}

static inline uint64_t efi_model_round_page(uint64_t size)
{
    return (size + EFI_PAGE_SIZE - 1) & ~(EFI_PAGE_SIZE - 1);
}

/*
 * Try candidates top-down within [lo, hi): the highest aligned start
 * that clears every exclusion. Returns 0 if none does.
 */
static uint64_t efi_model_fit(uint64_t lo, uint64_t hi, uint64_t size,
                              const efi_models_cfg_t *cfg)
{
    while (hi > lo && hi - lo >= size) {
        uint64_t cand = (hi - size) & ~(BOOT_MODEL_ALIGN - 1);
        uint64_t lower = 0;

        if (cand < lo || cand == 0) {
            return 0;
        }
        for (int i = 0; i < EFI_MODEL_MAX_EXCL; i++) {
            uint64_t xs = cfg->excl_start[i];
            uint64_t xl = cfg->excl_len[i];
            if (xl != 0 && cand < xs + xl && xs < cand + size && xs > lower) {
                lower = xs;
            }
            /* An exclusion that starts below cand and reaches into it
             * sets `lower` too, so the next candidate ends below it. */
        }
        if (lower == 0) {
            return cand;
        }
        hi = lower;
    }
    return 0;
}

uint64_t efi_model_pick_range(const efi_memory_desc_t *map, uint64_t map_size,
                              uint64_t desc_size, uint64_t size,
                              const efi_models_cfg_t *cfg)
{
    uint64_t best = 0;

    if (size == 0 || desc_size < sizeof(efi_memory_desc_t)) {
        return 0;
    }
    for (uint64_t off = 0; off + desc_size <= map_size; off += desc_size) {
        const efi_memory_desc_t *d =
            (const efi_memory_desc_t *)((const uint8_t *)map + off);
        if (d->type != EFI_CONVENTIONAL_MEMORY ||
            d->num_pages > UINT64_MAX / EFI_PAGE_SIZE) {
            continue;
        }
        uint64_t len = d->num_pages * EFI_PAGE_SIZE;
        if (d->phys_start > UINT64_MAX - len) {
            continue;
        }
        uint64_t lo = d->phys_start > cfg->floor ? d->phys_start : cfg->floor;
        uint64_t end = d->phys_start + len;
        uint64_t hi = end < cfg->limit ? end : cfg->limit;
        if (hi <= lo) {
            continue;
        }
        uint64_t cand = efi_model_fit(lo, hi, size, cfg);
        if (cand > best) {
            best = cand;
        }
    }
    return best;
}

/*
 * Reserve pages for `size` bytes. A range the firmware refuses is not
 * tried again: the search moves below it, a bounded number of times.
 */
static uint64_t efi_model_alloc(efi_system_table_t *st,
                                const efi_models_cfg_t *cfg, uint64_t size)
{
    efi_boot_services_t *bs = st->boot_services;
    efi_models_cfg_t search = *cfg;
    uint64_t rounded = efi_model_round_page(size);

    for (int attempt = 0; attempt < EFI_MODEL_ALLOC_TRIES; attempt++) {
        efi_memory_desc_t *map = NULL;
        uint64_t buf_size = 0, map_size = 0, map_key = 0, desc_size = 0;

        if (efi_read_map(bs, &map, &buf_size, &map_size, &map_key,
                         &desc_size) != EFI_SUCCESS) {
            return 0;
        }
        uint64_t addr = efi_model_pick_range(map, map_size, desc_size,
                                             rounded, &search);
        bs->free_pool(map);
        if (addr == 0) {
            return 0;
        }
        efi_phys_addr_t mem = addr;
        if (bs->allocate_pages(EFI_ALLOCATE_ADDRESS, EFI_LOADER_DATA,
                               rounded / EFI_PAGE_SIZE, &mem) == EFI_SUCCESS) {
            return addr;
        }
        search.limit = addr;
    }
    return 0;
}

/* Copy a UTF-16 name to ASCII. False if it isn't ASCII or doesn't fit. */
static bool efi_model_name_ascii(const uint16_t *in, char *out, size_t cap)
{
    size_t i = 0;
    for (; in[i] != 0; i++) {
        if (i + 1 >= cap || in[i] >= 0x7F || in[i] <= 0x20 || in[i] == '/') {
            return false;
        }
        out[i] = (char)in[i];
    }
    out[i] = 0;
    return true;
}

/* Case-insensitive ".gguf": FAT names can come back in upper case. */
static bool efi_model_has_gguf_suffix(const uint16_t *name)
{
    size_t n = 0;
    while (name[n] != 0) {
        n++;
    }
    if (n <= 5) {
        return false;
    }
    const uint16_t *s = &name[n - 5];
    return s[0] == '.' && (s[1] | 0x20) == 'g' && (s[2] | 0x20) == 'g' &&
           (s[3] | 0x20) == 'u' && (s[4] | 0x20) == 'f';
}

static char efi_model_ascii_lower(char c)
{
    return (c >= 'A' && c <= 'Z') ? (char)(c + ('a' - 'A')) : c;
}

/* Case-insensitive, as FAT is: `M.GGUF` on one volume and `m.gguf` on
 * another are the same model, and only the first volume's copy loads. */
static bool efi_model_table_has(const struct boot_model_table *t, const char *name)
{
    for (uint32_t i = 0; i < t->count; i++) {
        const char *a = t->models[i].name;
        const char *b = name;
        while (*a != 0 && efi_model_ascii_lower(*a) == efi_model_ascii_lower(*b)) {
            a++;
            b++;
        }
        if (efi_model_ascii_lower(*a) == efi_model_ascii_lower(*b)) {
            return true;
        }
    }
    return false;
}

/* Run state shared by every file in one efi_models_load call. */
struct efi_model_run {
    efi_system_table_t *st;
    const efi_models_cfg_t *cfg;
    struct boot_model_table *table;
    efi_models_stats_t *stats;
    uint64_t chunk;
    uint64_t deadline;
    uint64_t total_bytes;
    uint64_t total_cap;
};

/*
 * Read the whole file into [mem, mem + size). Returns true when every
 * byte arrived; a failure has already been counted and reported.
 */
static bool efi_model_read_all(struct efi_model_run *r, efi_file_protocol_t *f,
                               const char *name, uint8_t *mem, uint64_t size)
{
    uint64_t done = 0;
    uint64_t next_note = EFI_MODEL_PROGRESS_STEP;

    while (done < size) {
        uint64_t n = size - done < r->chunk ? size - done : r->chunk;
        efi_status_t status = f->read(f, &n, mem + done);
        if (status != EFI_SUCCESS || n == 0) {
            efi_print(r->st, u"[slmos] model ");
            efi_model_print_ascii(r->st, name);
            efi_print(r->st, u": read error ");
            efi_model_print_hex(r->st, status);
            efi_print(r->st, u" at offset ");
            efi_model_print_dec(r->st, done);
            efi_print(r->st, u"\r\n");
            r->stats->read_errors++;
            return false;
        }
        done += n;
        if ((int64_t)(efi_model_now() - r->deadline) > 0 && done < size) {
            efi_model_say(r->st, name, u"read budget exceeded");
            r->stats->over_budget++;
            return false;
        }
        if (done >= next_note) {
            efi_print(r->st, u"[slmos] model ");
            efi_model_print_dec(r->st, done >> 20);
            efi_print(r->st, u" / ");
            efi_model_print_dec(r->st, size >> 20);
            efi_print(r->st, u" MiB\r\n");
            next_note += EFI_MODEL_PROGRESS_STEP;
        }
    }
    return true;
}

/* Read one file. The directory entry says it is a *.gguf regular file. */
static void efi_model_load_one(struct efi_model_run *r,
                               efi_file_protocol_t *dir,
                               const efi_file_info_t *info)
{
    efi_system_table_t *st = r->st;
    efi_models_stats_t *stats = r->stats;
    char name[BOOT_MODEL_NAME_LEN];
    uint32_t magic = 0;
    uint64_t magic_len = sizeof(magic);

    if (!efi_model_name_ascii(info->file_name, name, sizeof(name))) {
        efi_print(st, u"[slmos] model: skipping a file with an unusable name\r\n");
        stats->bad_name++;
        return;
    }
    if (efi_model_table_has(r->table, name)) {
        return;     /* an earlier volume already provided it */
    }
    if (r->table->count >= BOOT_MODELS_MAX) {
        efi_model_say(st, name, u"skipped, too many models");
        stats->table_full++;
        return;
    }
    uint64_t size = info->file_size;
    if (size == 0 || size > BOOT_MODEL_MAX_BYTES ||
        size > r->total_cap - r->total_bytes) {
        efi_model_say(st, name, u"too big, skipped");
        stats->too_big++;
        return;
    }

    efi_file_protocol_t *f = NULL;
    if (dir->open(dir, &f, info->file_name, EFI_FILE_MODE_READ, 0) != EFI_SUCCESS) {
        efi_model_say(st, name, u"read error (open)");
        stats->read_errors++;
        return;
    }
    if (f->read(f, &magic_len, &magic) != EFI_SUCCESS ||
        magic_len != sizeof(magic) || magic != BOOT_MODEL_GGUF_MAGIC) {
        efi_model_say(st, name, u"not a GGUF, skipped");
        stats->bad_magic++;
        f->close(f);
        return;
    }
    if (f->set_position(f, 0) != EFI_SUCCESS) {
        efi_model_say(st, name, u"read error (seek)");
        stats->read_errors++;
        f->close(f);
        return;
    }

    uint64_t addr = efi_model_alloc(st, r->cfg, size);
    if (addr == 0) {
        efi_model_say(st, name, u"no memory, skipped");
        stats->no_memory++;
        f->close(f);
        return;
    }
    uint64_t pages = efi_model_round_page(size) / EFI_PAGE_SIZE;

    efi_print(st, u"[slmos] model ");
    efi_model_print_ascii(st, name);
    efi_print(st, u": reading ");
    efi_model_print_dec(st, size);
    efi_print(st, u" bytes to ");
    efi_model_print_hex(st, addr);
    efi_print(st, u"\r\n");

    uint64_t t0 = efi_model_now();
    uint8_t *mem = (uint8_t *)(uintptr_t)addr;
    if (!efi_model_read_all(r, f, name, mem, size)) {
        f->close(f);
        st->boot_services->free_pages(addr, pages);
        return;
    }
    f->close(f);
    uint32_t read_ms = efi_model_ms_since(t0);

    t0 = efi_model_now();
    uint64_t hash = boot_model_hash(mem, size);
    uint32_t hash_ms = efi_model_ms_since(t0);

    t0 = efi_model_now();
    efi_dcache_clean_inval(addr, size);
    uint32_t clean_ms = efi_model_ms_since(t0);

    struct boot_model *m = &r->table->models[r->table->count];
    m->phys = addr;
    m->size = size;
    m->hash = hash;
    m->read_ms = read_ms;
    m->hash_ms = hash_ms;
    m->clean_ms = clean_ms;
    m->pad = 0;
    for (size_t i = 0; i < sizeof(m->name); i++) {
        m->name[i] = name[i];
        if (name[i] == 0) {
            break;
        }
    }
    r->table->count++;
    r->total_bytes += size;
    stats->loaded++;

    efi_print(st, u"[slmos] model ");
    efi_model_print_ascii(st, name);
    efi_print(st, u": read ");
    efi_model_print_dec(st, read_ms);
    efi_print(st, u" ms (");
    efi_model_print_dec(st, read_ms ? (size >> 20) * 1000 / read_ms : 0);
    efi_print(st, u" MiB/s), hash ");
    efi_model_print_dec(st, hash_ms);
    efi_print(st, u" ms, clean ");
    efi_model_print_dec(st, clean_ms);
    efi_print(st, u" ms\r\n");
}

/* Walk \slmos\models on one volume. */
static void efi_model_scan_volume(struct efi_model_run *r, efi_handle_t handle)
{
    efi_guid_t sfs_guid = EFI_SIMPLE_FILE_SYSTEM_GUID;
    efi_simple_file_system_protocol_t *sfs = NULL;
    efi_file_protocol_t *root = NULL;
    efi_file_protocol_t *dir = NULL;
    efi_boot_services_t *bs = r->st->boot_services;
    /* EFI_FILE_INFO record: 80 bytes plus a 255-character UTF-16 name. */
    uint64_t info_buf[(0x50 + 512) / sizeof(uint64_t)];

    if (bs->handle_protocol(handle, &sfs_guid, (void **)&sfs) != EFI_SUCCESS ||
        sfs == NULL) {
        return;
    }
    if (sfs->open_volume(sfs, &root) != EFI_SUCCESS) {
        return;
    }
    r->stats->volumes_seen++;
    if (root->open(root, &dir, u"\\slmos\\models", EFI_FILE_MODE_READ, 0)
        != EFI_SUCCESS) {
        root->close(root);
        return;
    }

    for (int i = 0; i < EFI_MODEL_DIR_ENTRIES_MAX; i++) {
        uint64_t n = sizeof(info_buf);
        if (dir->read(dir, &n, info_buf) != EFI_SUCCESS || n == 0) {
            break;      /* end of directory, or one we can't walk */
        }
        const efi_file_info_t *info = (const efi_file_info_t *)info_buf;
        if ((info->attribute & EFI_FILE_ATTR_DIRECTORY) != 0 ||
            !efi_model_has_gguf_suffix(info->file_name)) {
            continue;
        }
        efi_model_load_one(r, dir, info);
    }
    dir->close(dir);
    root->close(root);
}

/*
 * Rank of a volume's device path: 0 for NVMe, which is the faster medium
 * on Jetson (measured in ADR 0002), 1 for everything else.
 */
static int efi_model_volume_rank(efi_boot_services_t *bs, efi_handle_t handle)
{
    efi_guid_t dp_guid = EFI_DEVICE_PATH_GUID;
    const efi_device_path_t *dp = NULL;

    if (bs->handle_protocol(handle, &dp_guid, (void **)&dp) != EFI_SUCCESS ||
        dp == NULL) {
        return 1;
    }
    for (int i = 0; i < 16; i++) {
        uint16_t len = (uint16_t)(dp->length[0] | (dp->length[1] << 8));
        if (dp->type == EFI_DP_TYPE_END || len < sizeof(*dp)) {
            break;
        }
        if (dp->type == EFI_DP_TYPE_MESSAGING && dp->subtype == EFI_DP_MSG_NVME) {
            return 0;
        }
        dp = (const efi_device_path_t *)((const uint8_t *)dp + len);
    }
    return 1;
}

/* One pass over the volumes the firmware has now, best medium first. */
static void efi_model_scan_all(struct efi_model_run *r)
{
    efi_guid_t sfs_guid = EFI_SIMPLE_FILE_SYSTEM_GUID;
    efi_boot_services_t *bs = r->st->boot_services;
    efi_handle_t *handles = NULL;
    uint64_t count = 0;

    if (bs->locate_handle_buffer(EFI_LOCATE_BY_PROTOCOL, &sfs_guid, NULL,
                                 &count, &handles) != EFI_SUCCESS) {
        return;
    }
    if (count > EFI_MODEL_VOLUMES_MAX) {
        count = EFI_MODEL_VOLUMES_MAX;
    }
    for (int rank = 0; rank < 2; rank++) {
        for (uint64_t i = 0; i < count; i++) {
            if (efi_model_volume_rank(bs, handles[i]) == rank) {
                efi_model_scan_volume(r, handles[i]);
            }
        }
    }
    bs->free_pool(handles);
}

/*
 * The boot manager connects only what it needs for the boot option, so
 * another partition's filesystem may not be bound yet. Connect every
 * block device's children, which binds the firmware's file system
 * drivers.
 */
static void efi_model_connect_block_devices(efi_boot_services_t *bs)
{
    efi_guid_t blk_guid = EFI_BLOCK_IO_GUID;
    efi_handle_t *handles = NULL;
    uint64_t count = 0;

    if (bs->locate_handle_buffer(EFI_LOCATE_BY_PROTOCOL, &blk_guid, NULL,
                                 &count, &handles) != EFI_SUCCESS) {
        return;
    }
    for (uint64_t i = 0; i < count; i++) {
        (void)bs->connect_controller(handles[i], NULL, NULL, true);
    }
    bs->free_pool(handles);
}

void efi_models_load(efi_system_table_t *sys_table,
                     const efi_models_cfg_t *cfg,
                     struct boot_model_table *table,
                     efi_models_stats_t *stats)
{
    efi_models_stats_t local_stats = { 0 };
    uint64_t budget_s = cfg->budget_secs ? cfg->budget_secs
                                         : EFI_MODEL_DEFAULT_BUDGET_S;
    struct efi_model_run run = {
        .st = sys_table,
        .cfg = cfg,
        .table = table,
        .stats = &local_stats,
        .chunk = cfg->chunk_bytes ? cfg->chunk_bytes : EFI_MODEL_DEFAULT_CHUNK,
        .deadline = efi_model_now() + budget_s * efi_model_freq(),
        .total_bytes = 0,
        /* Keep at least half of the window for the kernel. */
        .total_cap = (cfg->limit - cfg->floor) / 2,
    };

    table->count = 0;
    /* Up front, not only when nothing is found: a volume bound late could
     * be the faster medium, or the fallback for a read that fails. */
    efi_model_connect_block_devices(sys_table->boot_services);
    efi_model_scan_all(&run);

    if (table->count == 0) {
        efi_print(sys_table, u"[slmos] model: none loaded (volumes scanned: ");
        efi_model_print_dec(sys_table, local_stats.volumes_seen);
        efi_print(sys_table, u")\r\n");
    }
    if (stats != NULL) {
        *stats = local_stats;
    }
}

void efi_models_release(efi_system_table_t *sys_table,
                        struct boot_model_table *table)
{
    for (uint32_t i = 0; i < table->count; i++) {
        const struct boot_model *m = &table->models[i];
        sys_table->boot_services->free_pages(
            m->phys, efi_model_round_page(m->size) / EFI_PAGE_SIZE);
    }
    table->count = 0;
}
