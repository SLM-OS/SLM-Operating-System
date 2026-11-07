/*
 * slm_shell.c — `slm` shell command family (Phase SLM, M7.1).
 *
 * Wires the M1.4 (loader) + M5.2 (session/decoder) Rust FFI through
 * the kernel shell. See docs/design/slm-integration.md "CLI Surface
 * (Shell)" for the verb table; the dispatcher below mirrors the
 * existing `model` verb in kernel/src/shell_sys.c (cmd_model).
 *
 * Verbs:
 *   slm load <path>          — VFS read → rust_slm_load
 *   slm list                 — iterate rust_slm_count × rust_slm_get_info
 *   slm info <h>             — rust_slm_get_info pretty-print
 *   slm launch <h> [opts]    — rust_slm_session_open with sampler params
 *   slm prompt <s> [--raw] [--max N] "<text>"
 *                            — rust_slm_prompt_ex with UART token cb
 *   slm stream <s> [--raw] [--max N]
 *                            — read prompt from UART line, then prompt
 *   slm stop <s>             — rust_slm_stop; no lock, so it gets through
 *                              while a prompt runs in another session
 *   slm reset <s>            — rust_slm_session_reset
 *   slm unload <h>           — rust_slm_unload
 *   slm close <s>            — rust_slm_session_close
 *   slm status               — model count + session count summary
 *   slm stats <s>            — rust_slm_stats pretty-print
 *   slm gpu                  — per-op tier + dispatch counters
 *
 * NOTE on -mgeneral-regs-only: rust_slm_session_open and
 * rust_slm_prompt take f32 by value. AAPCS64 puts those in V0/V1 —
 * which the kernel's default `-mgeneral-regs-only` forbids the C
 * compiler from materialising. CMakeLists.txt overrides
 * COMPILE_OPTIONS for this file only so it compiles with FP/NEON
 * available. No other kernel TU is allowed to do FP arithmetic.
 */

#include "slm_shell.h"
#include "shell.h"
#include "shell_internal.h"
#include "uart.h"
#include "vfs.h"
#include "pmm.h"
#if !defined(PLATFORM_X86_64)
#include "kbuf.h"
#endif
#include "slm_ffi.h"
#include "slm_prog.h"
#include "boot_models.h"
#include "string.h"
#include "timer.h"

#include <stdint.h>
#include <stddef.h>

/* ============================================================================
 * Helpers
 * ========================================================================= */

/*
 * Parse a decimal float string ("0.7", "1.0", "0.95") into f32.
 * Returns 0 on success, -1 on parse error. Accepts an optional sign,
 * an integer part, an optional fractional part. No exponent support
 * (callers don't need it for sampler params).
 */
static int parse_f32(const char *s, float *out)
{
    if (!s || !*s || !out) return -1;

    int neg = 0;
    if (*s == '+' || *s == '-') {
        if (*s == '-') neg = 1;
        s++;
    }
    if (!*s) return -1;

    /* Integer part */
    double v = 0.0;
    int saw_digit = 0;
    while (*s >= '0' && *s <= '9') {
        v = v * 10.0 + (double)(*s - '0');
        saw_digit = 1;
        s++;
    }

    /* Fractional part */
    if (*s == '.') {
        s++;
        double scale = 0.1;
        while (*s >= '0' && *s <= '9') {
            v += (double)(*s - '0') * scale;
            scale *= 0.1;
            saw_digit = 1;
            s++;
        }
    }

    if (!saw_digit || *s != '\0') return -1;
    if (neg) v = -v;
    *out = (float)v;
    return 0;
}

/*
 * Print a float as "I.FFF" with 3 fractional digits. Avoids %f in
 * shell_printf because the kprintf backend doesn't implement it.
 */
static void print_f32_3(float f)
{
    if (f != f) { /* NaN */
        shell_puts("nan");
        return;
    }
    int neg = 0;
    if (f < 0.0f) { neg = 1; f = -f; }
    /* Round half-up to 3 decimals. */
    uint32_t scaled = (uint32_t)(f * 1000.0f + 0.5f);
    uint32_t whole  = scaled / 1000u;
    uint32_t frac   = scaled % 1000u;
    if (neg) shell_puts("-");
    shell_printf("%lu.%03lu", (unsigned long)whole, (unsigned long)frac);
}

/*
 * Render an SLM_ARCH-name buffer (null-padded ASCII, fixed length).
 * Returns the number of bytes written (excluding terminator).
 */
static size_t fixed_name_print(const uint8_t *buf, size_t cap)
{
    char tmp[64];
    size_t n = 0;
    for (size_t i = 0; i < cap && i + 1 < sizeof(tmp); i++) {
        if (buf[i] == 0) break;
        tmp[n++] = (char)buf[i];
    }
    tmp[n] = '\0';
    shell_puts(tmp);
    return n;
}

/* ============================================================================
 * Token streaming callback
 *
 * rust_slm_prompt invokes this per-decoded-token. We dump the UTF-8
 * bytes straight to the shell's I/O so the user sees output stream
 * one token at a time. Returning 1 keeps decoding; 0 stops early.
 * ========================================================================= */

typedef struct {
    uint64_t total_bytes;
    uint64_t total_tokens;
    bool     interrupted;   /* Ctrl-C or the peer went away */
} slm_token_cb_ctx_t;

static int32_t slm_prompt_token_cb(
    void *user,
    uint32_t token_id,
    const uint8_t *bytes,
    size_t bytes_len)
{
    slm_token_cb_ctx_t *ctx = (slm_token_cb_ctx_t *)user;
    if (!ctx) {
        return 0; /* defensive — should never happen */
    }
    /* Stream raw bytes. shell_putc honors UART back-pressure via the
     * underlying uart_putc / shell_io_uart sink. */
    for (size_t i = 0; i < bytes_len; i++) {
        shell_putc((char)bytes[i]);
    }
    if (bytes_len > 0) {
        shell_flush();
    }
    ctx->total_bytes += bytes_len;
    if (token_id != SLM_TOKEN_ID_FLUSH) {
        ctx->total_tokens += 1u;
    }
    return 1;
}

/* Called before every forward step, prefill included. */
static int32_t slm_prompt_poll_cb(void *user)
{
    slm_token_cb_ctx_t *ctx = (slm_token_cb_ctx_t *)user;
    if (ctx && shell_session_poll_interrupt()) {
        ctx->interrupted = true;
        return 1;
    }
    return 0;
}

/* ============================================================================
 * Verb handlers
 * ========================================================================= */

/*
 * slm load <vfs-path>
 *
 * Read the entire file via vfs_read_path into a pmm-backed buffer,
 * then hand the bytes to rust_slm_load. Mirrors the model_load path
 * in kernel/src/shell_sys.c.
 */
static int slm_load(int argc, char *argv[])
{
    if (argc < 3) {
        shell_puts("Usage: slm load <vfs-path>\r\n");
        return -1;
    }

    char resolved[VFS_MAX_PATH];
    if (shell_resolve_path(argv[2], resolved, sizeof(resolved)) < 0) {
        shell_puts("slm load: path too long\r\n");
        return -1;
    }

    /* Models the EFI stub delivered load without a read or a copy. */
    int boot_rc = boot_models_slm_load(resolved);
    if (boot_rc != BOOT_MODEL_NOT_HANDLED) {
        return boot_rc;
    }

    struct vfs_entry_info info;
    if (vfs_stat_path(resolved, &info) != 0) {
        shell_printf("slm load: %s: file not found\r\n", resolved);
        return -1;
    }
    if (info.type != 0) {
        shell_printf("slm load: %s: not a file\r\n", resolved);
        return -1;
    }
    if (info.size == 0) {
        shell_puts("slm load: file is empty\r\n");
        return -1;
    }
    /*
     * GGUF size cap is owned by the Rust registry
     * (`MAX_PLAUSIBLE_GGUF_BYTES`, 2 GiB today — matches PMM buddy
     * max-order = 19). Querying via FFI keeps the C shell in lockstep
     * when #550's multi-block allocator lifts the ceiling further, so
     * a stale hardcode here can't accept a file the registry will
     * then reject with `CorruptedData`.
     */
    const uint64_t cap = rust_slm_max_gguf_bytes();
    if ((uint64_t)info.size > cap) {
        shell_printf("slm load: file too large (%llu bytes, cap %llu)\r\n",
                     (unsigned long long)info.size,
                     (unsigned long long)cap);
        return -1;
    }

    size_t pages = (info.size + 4095u) / 4096u;
    uint8_t *buf = (uint8_t *)pmm_alloc_pages(pages);
    if (!buf) {
        shell_puts("slm load: out of memory for read buffer\r\n");
        return -1;
    }

    int rd = vfs_read_path(resolved, (char *)buf, info.size, 0);
    if (rd <= 0) {
        shell_printf("slm load: failed to read %s\r\n", resolved);
        pmm_free_pages(buf, pages);
        return -1;
    }

    /* Derive the model name from the basename, stripped of any
     * extension. Bound to 31 bytes (registry caps at SLM_MODEL_NAME_LEN). */
    const char *base = resolved;
    for (const char *p = resolved; *p; p++) {
        if (*p == '/') base = p + 1;
    }
    char name_buf[SLM_MODEL_NAME_LEN];
    size_t nl = 0;
    for (const char *p = base; *p && *p != '.' && nl + 1 < sizeof(name_buf); p++) {
        name_buf[nl++] = *p;
    }
    name_buf[nl] = '\0';
    if (nl == 0) {
        /* Fallback name when basename starts with '.'. */
        strcpy(name_buf, "slm");
    }

    int idx = rust_slm_load((const uint8_t *)name_buf, buf, (size_t)rd);
    pmm_free_pages(buf, pages);
    if (idx < 0) {
        shell_printf("slm load: failed to parse %s "
                     "(not a GGUF or unsupported architecture)\r\n", resolved);
        return -1;
    }

    SlmModelInfoC mi;
    if (rust_slm_get_info((uint32_t)idx, &mi) != 0) {
        shell_printf("[slm] loaded handle=%d (info unavailable)\r\n", idx);
        return 0;
    }

    shell_printf("[slm] loaded handle=%d  arch=", idx);
    fixed_name_print(mi.architecture, sizeof(mi.architecture));
    shell_printf("  blocks=%lu hidden=%lu head=%lu/%lu head_dim=%lu vocab=%lu "
                 "ctx=%lu source=%lu MB\r\n",
                 (unsigned long)mi.block_count,
                 (unsigned long)mi.embedding_length,
                 (unsigned long)mi.head_count,
                 (unsigned long)mi.head_count_kv,
                 (unsigned long)mi.head_dim,
                 (unsigned long)mi.vocab_size,
                 (unsigned long)mi.context_length,
                 (unsigned long)(mi.source_bytes / (1024u * 1024u)));
    return 0;
}

/*
 * Read exactly `total` bytes from the active shell session into
 * `buf`. Returns true on success (`buf` filled with `total` bytes),
 * false on any failure (peer close mid-stream, 5-second stall with
 * no incoming bytes, or `total == 0`). The failure-case wire reply
 * `SLM-XLOAD err received=<N> reason=<cause>` is printed inside
 * this helper so the caller doesn't need to discriminate; on `true`
 * the caller emits the success reply.
 *
 * Caller is responsible for switching the session into binary mode
 * before calling and back out after. This helper does NOT touch the
 * binary-mode flag because callers (`slm xload` here, future stream
 * loaders) have varying needs around when the protocol header is
 * exchanged relative to the mode switch.
 *
 * Reads in 128 KB slices to amortise per-call overhead; matches the
 * `xput-bin` staging-buffer size in `kernel/src/shell_fs.c` so a
 * 1 GB upload stays at ~8K read calls.
 */
#define SLM_XLOAD_CHUNK_BYTES   131072u

/*
 * Stream-buffer alloc/free for `slm xload`. Centralises the
 * platform split so the body of `slm_xload` stays platform-neutral:
 * on ARM64 we go through `kbuf_alloc` (chunked-VA fallback when the
 * PMM buddy can't satisfy as a single block, #789); on x86-64 we
 * keep the pre-#789 direct `pmm_alloc_pages` path because the
 * fragmentation trigger hasn't been seen there and the kbuf module
 * isn't compiled in.
 */
static inline void *xload_buf_alloc(size_t total, size_t pages)
{
#if !defined(PLATFORM_X86_64)
    (void)pages;
    return kbuf_alloc(total);
#else
    (void)total;
    return pmm_alloc_pages(pages);
#endif
}

static inline void xload_buf_free(void *buf, size_t pages)
{
#if !defined(PLATFORM_X86_64)
    (void)pages;
    kbuf_free(buf);
#else
    pmm_free_pages(buf, pages);
#endif
}

static bool slm_xload_drain_session(uint8_t *buf, uint32_t total)
{
    if (total == 0) {
        shell_printf("SLM-XLOAD err received=0 reason=zero_total\r\n");
        return false;
    }
    const uint64_t freq = timer_get_frequency();
    const uint64_t stall_ticks = freq ? (freq * 5ULL) : 0;  /* 5 s */

    uint32_t received = 0;
    uint64_t last_progress = timer_get_count();
    while (received < total) {
        uint32_t want = total - received;
        if (want > SLM_XLOAD_CHUNK_BYTES) {
            want = SLM_XLOAD_CHUNK_BYTES;
        }

        int n = shell_session_read_raw((char *)(buf + received), (int)want);
        if (n < 0) {
            shell_printf("SLM-XLOAD err received=%lu reason=closed\r\n",
                         (unsigned long)received);
            return false;
        }
        if (n > 0) {
            received += (uint32_t)n;
            last_progress = timer_get_count();
            continue;
        }
        if (stall_ticks &&
            (timer_get_count() - last_progress) > stall_ticks) {
            shell_printf("SLM-XLOAD err received=%lu reason=stall\r\n",
                         (unsigned long)received);
            return false;
        }
    }
    return true;
}

/* Print the post-load info banner that mirrors `slm load`'s success
 * output. Pulled into its own helper so the main `slm xload` body
 * stays inside the 80-line guideline. */
static void slm_xload_print_loaded(int idx)
{
    SlmModelInfoC mi;
    if (rust_slm_get_info((uint32_t)idx, &mi) != 0) {
        shell_printf("[slm] loaded handle=%d (info unavailable)\r\n", idx);
        return;
    }
    shell_printf("[slm] loaded handle=%d  arch=", idx);
    fixed_name_print(mi.architecture, sizeof(mi.architecture));
    shell_printf("  blocks=%lu hidden=%lu head=%lu/%lu head_dim=%lu vocab=%lu "
                 "ctx=%lu source=%lu MB\r\n",
                 (unsigned long)mi.block_count,
                 (unsigned long)mi.embedding_length,
                 (unsigned long)mi.head_count,
                 (unsigned long)mi.head_count_kv,
                 (unsigned long)mi.head_dim,
                 (unsigned long)mi.vocab_size,
                 (unsigned long)mi.context_length,
                 (unsigned long)(mi.source_bytes / (1024u * 1024u)));
}

/*
 * slm xload <name> <total>
 *
 * Streaming GGUF load that bypasses the LittleFS round-trip used by
 * `slm load`. The default path needs the file in the ramdisk AND a
 * second PMM buffer of equal size to feed rust_slm_load — i.e. 2× the
 * file size resident at once. With a 1.04 GB Q4_K_M GGUF + 1 GB Rust
 * heap + 1.28 GB ramdisk, the PMM buddy can run out of an order-19
 * block even though there are gigabytes "free" in smaller orders.
 *
 * xload skips LittleFS: receive `total` raw bytes from the telnet
 * shell session straight into a single PMM buffer, hand that to
 * rust_slm_load, and free the buffer. Peak memory == file size.
 *
 * Wire format mirrors `xput-bin` so the existing telnet binary-mode
 * machinery (IAC unstuffing, 0xFF doubling) is reused unchanged:
 *
 *   client:  slm xload <name> <total>\n
 *   kernel:  SLM-XLOAD ready name=<name> total=<N>\r\n
 *   client:  <total> raw bytes (with 0xFF doubled per RFC 854)
 *   kernel:  SLM-XLOAD done received=<total>\r\n
 *   kernel:  [slm] loaded handle=<idx> ...   (rust_slm_load output)
 *   kernel:  slmos>     (normal prompt resumes)
 */
static int slm_xload(int argc, char *argv[])
{
    if (argc < 4) {
        shell_puts("Usage: slm xload <name> <total>\r\n");
        return -1;
    }
    const char *name = argv[2];
    uint32_t total;
    if (shell_parse_uint(argv[3], &total) != 0) {
        shell_printf("slm xload: invalid total: %s\r\n", argv[3]);
        return -1;
    }
    if (total == 0) {
        shell_puts("slm xload: total must be > 0\r\n");
        return -1;
    }

    const uint64_t cap = rust_slm_max_gguf_bytes();
    if ((uint64_t)total > cap) {
        shell_printf("slm xload: too large (%lu bytes, cap %llu)\r\n",
                     (unsigned long)total, (unsigned long long)cap);
        return -1;
    }

    /* Bound the model name like slm_load does: registry caps at
     * SLM_MODEL_NAME_LEN (32). */
    char name_buf[SLM_MODEL_NAME_LEN];
    size_t nl = 0;
    for (const char *p = name; *p && nl + 1 < sizeof(name_buf); p++) {
        name_buf[nl++] = *p;
    }
    name_buf[nl] = '\0';
    if (nl == 0) {
        shell_puts("slm xload: empty name\r\n");
        return -1;
    }

    size_t pages = (total + 4095u) / 4096u;
    /*
     * Stream buffer is allocated via `xload_buf_alloc`, which on
     * ARM64 goes through `kbuf_alloc` (chunked-VA fallback when the
     * PMM buddy can't satisfy a single contiguous block — fixes
     * #789 for Qwen-1.5B + 1.5 GB IOVMM pool configs) and on x86-64
     * falls back to plain `pmm_alloc_pages`.
     *
     * `slm_free_pages` (the C-side free hook the Rust registry
     * calls on `slm unload`) dispatches transparently through
     * `kbuf_owns_va` for the take-pages-ownership case below.
     */
    uint8_t *buf = (uint8_t *)xload_buf_alloc((size_t)total, pages);
    if (!buf) {
        shell_puts("slm xload: out of memory for stream buffer\r\n");
        return -1;
    }

    shell_session_set_binary_mode(true);
    shell_printf("SLM-XLOAD ready name=%s total=%lu\r\n",
                 name_buf, (unsigned long)total);

    bool ok = slm_xload_drain_session(buf, total);
    shell_session_set_binary_mode(false);
    if (!ok) {
        xload_buf_free(buf, pages);
        return -1;
    }
    shell_printf("SLM-XLOAD done received=%lu\r\n", (unsigned long)total);

    /* Transfer ownership of the streaming buffer to the registry. On
     * success the registry will free the pages on `slm unload`; we
     * MUST NOT call pmm_free_pages / kbuf_free here. On failure (-1)
     * the caller still owns the pages and we free them ourselves. */
    int idx = rust_slm_load_take_pages((const uint8_t *)name_buf,
                                       buf, pages, (size_t)total);
    if (idx < 0) {
        xload_buf_free(buf, pages);
        shell_puts("slm xload: failed to parse stream "
                   "(not a GGUF or unsupported architecture)\r\n");
        return -1;
    }
    slm_xload_print_loaded(idx);
    return 0;
}

/*
 * slm list
 *
 * Walk indices 0..N looking for valid slots. The Rust registry caps
 * out at a small fixed table — we walk a generous upper bound (32)
 * and skip empty slots so we don't depend on knowing the maximum.
 */
static int slm_list(void)
{
    uint32_t count = rust_slm_count();
    if (count == 0) {
        shell_puts("No SLMs loaded.\r\n");
        return 0;
    }
    shell_printf("Loaded SLMs (%lu):\r\n", (unsigned long)count);
    shell_puts("  Idx  Arch              Vocab     Source(MB)  Name\r\n");
    shell_puts("  ---  ----              -----     ----------  ----\r\n");
    SlmModelInfoC mi;
    for (uint32_t i = 0; i < 32u; i++) {
        if (rust_slm_get_info(i, &mi) != 0) continue;
        shell_printf("  %lu    ", (unsigned long)i);
        size_t arch_w = fixed_name_print(mi.architecture, sizeof(mi.architecture));
        for (size_t pad = arch_w; pad < 18; pad++) shell_putc(' ');
        shell_printf("%-9lu %-11lu ",
                     (unsigned long)mi.vocab_size,
                     (unsigned long)(mi.source_bytes / (1024u * 1024u)));
        fixed_name_print(mi.name, sizeof(mi.name));
        shell_puts("\r\n");
    }
    return 0;
}

static int slm_info(int argc, char *argv[])
{
    if (argc < 3) {
        shell_puts("Usage: slm info <handle>\r\n");
        return -1;
    }
    uint32_t idx;
    if (shell_parse_uint(argv[2], &idx) != 0) {
        shell_printf("slm info: invalid handle '%s'\r\n", argv[2]);
        return -1;
    }
    SlmModelInfoC mi;
    if (rust_slm_get_info(idx, &mi) != 0) {
        shell_printf("slm info: handle %lu is empty\r\n",
                     (unsigned long)idx);
        return -1;
    }

    shell_puts("arch         : "); fixed_name_print(mi.architecture, sizeof(mi.architecture)); shell_puts("\r\n");
    shell_puts("name         : "); fixed_name_print(mi.name, sizeof(mi.name)); shell_puts("\r\n");
    shell_printf("block_count  : %lu\r\n", (unsigned long)mi.block_count);
    shell_printf("embedding    : %lu\r\n", (unsigned long)mi.embedding_length);
    shell_printf("head_count   : %lu\r\n", (unsigned long)mi.head_count);
    shell_printf("head_count_kv: %lu\r\n", (unsigned long)mi.head_count_kv);
    shell_printf("head_dim     : %lu\r\n", (unsigned long)mi.head_dim);
    shell_printf("ff_length    : %lu\r\n", (unsigned long)mi.feed_forward_length);
    shell_printf("context_len  : %lu\r\n", (unsigned long)mi.context_length);
    shell_printf("vocab_size   : %lu\r\n", (unsigned long)mi.vocab_size);
    shell_printf("tensor_count : %lu\r\n", (unsigned long)mi.tensor_count);
    shell_printf("source_bytes : %lu MB (%lu bytes)\r\n",
                 (unsigned long)(mi.source_bytes / (1024u * 1024u)),
                 (unsigned long)mi.source_bytes);
    shell_puts("rope_freq    : ");
    print_f32_3(mi.rope_freq_base);
    shell_puts("\r\n");
    return 0;
}

static int slm_unload(int argc, char *argv[])
{
    if (argc < 3) {
        shell_puts("Usage: slm unload <handle>\r\n");
        return -1;
    }
    uint32_t idx;
    if (shell_parse_uint(argv[2], &idx) != 0) {
        shell_printf("slm unload: invalid handle '%s'\r\n", argv[2]);
        return -1;
    }
    int rc = rust_slm_unload(idx);
    if (rc == SLM_ERR_BUSY) {
        shell_printf("slm unload: a prompt is running on handle %lu "
                     "(slm stop it first)\r\n", (unsigned long)idx);
        return -1;
    }
    if (rc != 0) {
        shell_printf("slm unload: failed for handle %lu\r\n",
                     (unsigned long)idx);
        return -1;
    }
    shell_printf("Unloaded SLM handle %lu\r\n", (unsigned long)idx);
    return 0;
}

/*
 * slm launch <handle> [--ctx N] [--sampler ...] [--temp F]
 *                    [--topk N] [--topp F] [--seed N]
 *                    [--gpu auto|cpu|on] [--threads K]
 *
 * Default sampler: TopKTopP (per the spec). Defaults match the spec
 * "demo" example: ctx=2048, temp=0.7, topk=40, topp=0.9, seed=1,
 * gpu=auto, threads=4.
 */
/* Print the truth about where this session's ops run: the --gpu mode, and
 * for anything but cpu, how many ops the tier table sends to the GPU and how
 * many weight tensors are staged there. Any dispatch can still fall back to
 * the CPU per op; `slm gpu` counts those. */
static void slm_print_backend(uint32_t sid, uint32_t gpu_mode)
{
    SlmSessionInfoC info;
    if (rust_slm_session_info(sid, &info) != 0) {
        shell_puts("backend=unknown");
        return;
    }
    SlmSessionGpuC g;
    if (gpu_mode == SLM_GPU_CPU) {
        shell_puts("backend=CPU(--gpu cpu)");
    } else if (rust_slm_session_gpu_info(sid, &g) == 0 && g.path == SLM_PATH_PROGRAM) {
        shell_printf("backend=GPU program(launches=%lu barriers=%lu batch=%lu arena=%luMB "
                     "weights=%s)",
                     (unsigned long)g.launches, (unsigned long)g.barriers,
                     (unsigned long)g.max_batch, (unsigned long)(g.arena_bytes >> 20),
                     g.weights_big ? "64KB" : "4KB");
    } else if (rust_slm_session_gpu_info(sid, &g) == 0 && g.path == SLM_PATH_CPU) {
        shell_printf("backend=CPU(%s)", g.note[0] ? g.note : "no GPU program");
    } else if (info.gpu_ops == 0) {
        shell_puts("backend=CPU(no GPU ops available)");
    } else {
        shell_printf("backend=GPU+CPU(gpu_ops=%lu/8 staged_tensors=%lu)",
                     (unsigned long)info.gpu_ops,
                     (unsigned long)info.gpu_tensors);
    }
}

static int slm_launch(int argc, char *argv[])
{
    if (argc < 3) {
        shell_puts("Usage: slm launch <handle> [--ctx N] [--sampler "
                   "greedy|temp|topk|topp|topkp] [--temp F] [--topk N] "
                   "[--topp F] [--seed N] [--gpu auto|cpu|on] "
                   "[--threads K]\r\n");
        return -1;
    }
    uint32_t handle;
    if (shell_parse_uint(argv[2], &handle) != 0) {
        shell_printf("slm launch: invalid handle '%s'\r\n", argv[2]);
        return -1;
    }

    /* Defaults — match the demo line in docs/design/slm-integration.md. */
    uint32_t max_ctx       = 2048u;
    uint32_t sampler_kind  = SLM_SAMPLER_TOP_K_TOP_P;
    float    temperature   = 0.7f;
    uint32_t top_k         = 40u;
    float    top_p         = 0.9f;
    uint64_t seed          = 1u;
    uint32_t gpu_mode      = SLM_GPU_AUTO;
    uint32_t threads       = 4u;

    for (int i = 3; i < argc; i++) {
        const char *a = argv[i];
        const char *v = (i + 1 < argc) ? argv[i + 1] : NULL;
        if (strcmp(a, "--ctx") == 0 && v) {
            if (shell_parse_uint(v, &max_ctx) != 0) {
                shell_printf("slm launch: bad --ctx '%s'\r\n", v); return -1;
            }
            i++;
        } else if (strcmp(a, "--sampler") == 0 && v) {
            if (strcmp(v, "greedy") == 0) {
                sampler_kind = SLM_SAMPLER_GREEDY;
            } else if (strcmp(v, "temp") == 0) {
                sampler_kind = SLM_SAMPLER_TEMPERATURE;
            } else if (strcmp(v, "topk") == 0) {
                sampler_kind = SLM_SAMPLER_TOP_K;
            } else if (strcmp(v, "topp") == 0) {
                sampler_kind = SLM_SAMPLER_TOP_P;
            } else if (strcmp(v, "topkp") == 0 ||
                       strcmp(v, "topktopp") == 0) {
                sampler_kind = SLM_SAMPLER_TOP_K_TOP_P;
            } else {
                shell_printf("slm launch: unknown sampler '%s' "
                             "(want greedy|temp|topk|topp|topkp)\r\n", v);
                return -1;
            }
            i++;
        } else if (strcmp(a, "--temp") == 0 && v) {
            if (parse_f32(v, &temperature) != 0) {
                shell_printf("slm launch: bad --temp '%s'\r\n", v); return -1;
            }
            i++;
        } else if (strcmp(a, "--topk") == 0 && v) {
            if (shell_parse_uint(v, &top_k) != 0) {
                shell_printf("slm launch: bad --topk '%s'\r\n", v); return -1;
            }
            i++;
        } else if (strcmp(a, "--topp") == 0 && v) {
            if (parse_f32(v, &top_p) != 0) {
                shell_printf("slm launch: bad --topp '%s'\r\n", v); return -1;
            }
            i++;
        } else if (strcmp(a, "--seed") == 0 && v) {
            uint32_t s32;
            if (shell_parse_uint(v, &s32) != 0) {
                shell_printf("slm launch: bad --seed '%s'\r\n", v); return -1;
            }
            seed = (uint64_t)s32;
            i++;
        } else if (strcmp(a, "--gpu") == 0 && v) {
            if (strcmp(v, "auto") == 0) {
                gpu_mode = SLM_GPU_AUTO;
            } else if (strcmp(v, "cpu") == 0) {
                gpu_mode = SLM_GPU_CPU;
            } else if (strcmp(v, "on") == 0) {
                gpu_mode = SLM_GPU_ON;
            } else {
                shell_printf("slm launch: bad --gpu '%s' "
                             "(want auto|cpu|on)\r\n", v);
                return -1;
            }
            i++;
        } else if (strcmp(a, "--threads") == 0 && v) {
            if (shell_parse_uint(v, &threads) != 0 ||
                threads == 0 || threads > 64u) {
                shell_printf("slm launch: bad --threads '%s' "
                             "(want 1..64)\r\n", v);
                return -1;
            }
            i++;
        } else {
            shell_printf("slm launch: unknown option '%s'\r\n", a);
            return -1;
        }
    }

    int32_t sid = rust_slm_session_open(handle, max_ctx, sampler_kind,
                                        temperature, top_k, top_p, seed);
    if (sid < 0) {
        shell_puts("slm launch: session_open failed (model not loaded, "
                   "session table full, or KV cache OOM)\r\n");
        return -1;
    }
    int32_t crc = rust_slm_session_configure((uint32_t)sid, gpu_mode, threads);
    if (crc == SLM_ERR_GPU_PROGRAM) {
        SlmSessionGpuC g;
        const char *why = rust_slm_session_gpu_info((uint32_t)sid, &g) == 0 ? g.note : "unknown";
        shell_printf("slm launch: --gpu on, but no GPU program: %s; "
                     "use --gpu auto or cpu\r\n", why);
        rust_slm_session_close((uint32_t)sid);
        return -1;
    }
    if (crc != 0) {
        rust_slm_session_close((uint32_t)sid);
        shell_puts("slm launch: could not apply --gpu/--threads\r\n");
        return -1;
    }

    const char *sname =
        (sampler_kind == SLM_SAMPLER_GREEDY)        ? "greedy" :
        (sampler_kind == SLM_SAMPLER_TEMPERATURE)   ? "temp"   :
        (sampler_kind == SLM_SAMPLER_TOP_K)         ? "topk"   :
        (sampler_kind == SLM_SAMPLER_TOP_P)         ? "topp"   :
        (sampler_kind == SLM_SAMPLER_TOP_K_TOP_P)   ? "topkp"  :
                                                      "?";
    shell_printf("[slm] session=%ld ", (long)sid);
    slm_print_backend((uint32_t)sid, gpu_mode);
    /* The worker pool isn't here yet: the forward path runs on one core
     * whatever --threads says, so the banner shouldn't imply otherwise. */
    shell_printf(" threads=%lu(stored; forward is single-threaded) "
                 "sampler=%s ctx=%lu",
                 (unsigned long)threads, sname, (unsigned long)max_ctx);
    if (sampler_kind != SLM_SAMPLER_GREEDY) {
        shell_puts(" temp=");
        print_f32_3(temperature);
    }
    if (sampler_kind == SLM_SAMPLER_TOP_K ||
        sampler_kind == SLM_SAMPLER_TOP_K_TOP_P) {
        shell_printf(" top_k=%lu", (unsigned long)top_k);
    }
    if (sampler_kind == SLM_SAMPLER_TOP_P ||
        sampler_kind == SLM_SAMPLER_TOP_K_TOP_P) {
        shell_puts(" top_p=");
        print_f32_3(top_p);
    }
    shell_printf(" seed=%lu\r\n", (unsigned long)seed);
    return 0;
}

static int slm_close(int argc, char *argv[])
{
    if (argc < 3) {
        shell_puts("Usage: slm close <session>\r\n");
        return -1;
    }
    uint32_t sid;
    if (shell_parse_uint(argv[2], &sid) != 0) {
        shell_printf("slm close: invalid session '%s'\r\n", argv[2]);
        return -1;
    }
    int rc = rust_slm_session_close(sid);
    if (rc == SLM_ERR_BUSY) {
        shell_printf("slm close: a prompt is running on session %lu "
                     "(slm stop it first)\r\n", (unsigned long)sid);
        return -1;
    }
    if (rc != 0) {
        shell_printf("slm close: failed for session %lu\r\n",
                     (unsigned long)sid);
        return -1;
    }
    shell_printf("Closed session %lu\r\n", (unsigned long)sid);
    return 0;
}

static int slm_reset(int argc, char *argv[])
{
    if (argc < 3) {
        shell_puts("Usage: slm reset <session>\r\n");
        return -1;
    }
    uint32_t sid;
    if (shell_parse_uint(argv[2], &sid) != 0) {
        shell_printf("slm reset: invalid session '%s'\r\n", argv[2]);
        return -1;
    }
    int rc = rust_slm_session_reset(sid);
    if (rc == SLM_ERR_BUSY) {
        shell_printf("slm reset: a prompt is running on session %lu "
                     "(slm stop it first)\r\n", (unsigned long)sid);
        return -1;
    }
    if (rc != 0) {
        shell_printf("slm reset: failed for session %lu\r\n",
                     (unsigned long)sid);
        return -1;
    }
    shell_printf("Reset session %lu\r\n", (unsigned long)sid);
    return 0;
}

static int slm_stop(int argc, char *argv[])
{
    if (argc < 3) {
        shell_puts("Usage: slm stop <session>\r\n");
        return -1;
    }
    uint32_t sid;
    if (shell_parse_uint(argv[2], &sid) != 0) {
        shell_printf("slm stop: invalid session '%s'\r\n", argv[2]);
        return -1;
    }
    if (rust_slm_stop(sid) != 0) {
        shell_printf("slm stop: failed for session %lu\r\n",
                     (unsigned long)sid);
        return -1;
    }
    /* Not rust_slm_session_info: that takes the registry lock, which a
     * running prompt holds for each forward step. */
    if (rust_slm_session_busy(sid) == 0) {
        shell_printf("Session %lu has no prompt running\r\n",
                     (unsigned long)sid);
        return 0;
    }
    shell_printf("Stop signaled for session %lu\r\n", (unsigned long)sid);
    return 0;
}

/* Words for SlmStatsC.last_stop_reason. */
static const char *slm_stop_reason_name(uint32_t reason)
{
    switch (reason) {
    case SLM_STOP_EOS:          return "eos";
    case SLM_STOP_MAX_TOKENS:   return "max";
    case SLM_STOP_USER:         return "stopped";
    case SLM_STOP_CALLBACK:     return "callback";
    case SLM_STOP_CONTEXT_FULL: return "ctx-full";
    case SLM_STOP_NO_LOGITS:    return "no-logits";
    case SLM_STOP_EMPTY_PROMPT: return "empty";
    default:                    return "none";
    }
}

/*
 * Run a prompt against an open session. `flags` is 0 or SLM_PROMPT_RAW.
 * `max_new_tokens` of 0 means "unlimited" per the FFI contract.
 */
static int run_prompt(uint32_t sid, const char *text, uint32_t flags,
                      uint32_t max_new_tokens)
{
    slm_token_cb_ctx_t ctx = {
        .total_bytes  = 0,
        .total_tokens = 0,
        .interrupted  = false,
    };

    size_t plen = (text != NULL) ? strlen(text) : 0;

    /* A decode runs for minutes. Don't hold the shell mutation lock for it:
     * a second session waiting on the lock would boost this task above
     * net_pump, which starves that session's own input (#1103). Sessions
     * have their own busy state, so nothing here depends on the lock. */
    bool paused = shell_mutation_pause();
    int32_t rc = rust_slm_prompt_ex(sid,
                                    (const uint8_t *)text,
                                    plen,
                                    flags,
                                    max_new_tokens,
                                    slm_prompt_token_cb,
                                    slm_prompt_poll_cb,
                                    &ctx);
    shell_mutation_resume(paused);

    /* Always end the streamed line cleanly so the next prompt is
     * on its own row. */
    shell_puts(ctx.interrupted ? "^C\r\n" : "\r\n");

    if (rc == SLM_ERR_BUSY) {
        shell_puts("slm prompt: a prompt is already running "
                   "(one at a time; slm stop it first)\r\n");
        return -1;
    }
    if (rc != 0) {
        shell_printf("slm prompt: failed rc=%ld (no such session, invalid "
                     "UTF-8, or its model was unloaded)\r\n", (long)rc);
        return -1;
    }

    /* Telemetry summary (decode rate intentionally rounded to 1 dec). */
    SlmStatsC st;
    if (rust_slm_stats(sid, &st) == 0) {
        unsigned long ttft_ms   = (unsigned long)(st.last_ttft_ns / 1000000ull);
        unsigned long decode_ms = (unsigned long)(st.last_decode_ns / 1000000ull);
        /* last_decode_ns starts after the first token, so it covers one
         * forward step per later token, plus the one that sampled EOS. */
        uint64_t steps = ctx.total_tokens > 0 ? ctx.total_tokens - 1u : 0;
        if (st.last_stop_reason == SLM_STOP_EOS && ctx.total_tokens > 0) {
            steps += 1u;
        }
        float tok_per_s = 0.0f;
        if (st.last_decode_ns > 0 && steps > 0) {
            double sec = (double)st.last_decode_ns / 1.0e9;
            tok_per_s = (float)((double)steps / sec);
        }
        shell_printf("[slm] ttft=%lu ms  decode=", ttft_ms);
        print_f32_3(tok_per_s);
        shell_printf(" tok/s  total=%lu ms tokens=%lu bytes=%lu stop=%s\r\n",
                     decode_ms,
                     (unsigned long)ctx.total_tokens,
                     (unsigned long)ctx.total_bytes,
                     slm_stop_reason_name(st.last_stop_reason));
        if (st.last_stop_reason == SLM_STOP_CONTEXT_FULL) {
            shell_puts("[slm] context full: `slm reset` for a fresh "
                       "conversation, or `slm launch --ctx` larger\r\n");
        } else if (st.last_stop_reason == SLM_STOP_NO_LOGITS) {
            shell_puts("[slm] the forward pass produced no output "
                       "(model lacks layer weights?)\r\n");
        } else if (st.last_stop_reason == SLM_STOP_USER) {
            shell_puts("[slm] stopped: the next prompt continues this "
                       "conversation; `slm reset` for a fresh one\r\n");
        }
    }
    return 0;
}

/*
 * Turn the two-character sequences \n, \t and \\ into what they name, in
 * place. A shell line can't hold a newline, and a pre-rendered ChatML prompt
 * is full of them.
 */
static void slm_unescape_inplace(char *s)
{
    char *out = s;
    for (const char *p = s; *p; p++) {
        if (*p == '\\' && (p[1] == 'n' || p[1] == 't' || p[1] == '\\')) {
            p++;
            *out++ = (*p == 'n') ? '\n' : (*p == 't') ? '\t' : '\\';
        } else {
            *out++ = *p;
        }
    }
    *out = '\0';
}

#define SLM_DEFAULT_MAX_NEW_TOKENS 256u /* the spec's --max default */

/*
 * Read [--raw] [--max N] from argv[first..*last], from the front until the
 * first word that isn't an option, then from the back so a trailing
 * `"text" --max 8` works too. `--` ends the options, for text that starts
 * with dashes. Returns the index of the first text word; *last shrinks past
 * trailing options. Returns -1 after printing an error.
 */
static int slm_parse_prompt_opts(const char *verb, int first, int *last,
                                 char *argv[], uint32_t *flags, uint32_t *max)
{
    int i = first;
    bool ended = false;
    while (i <= *last) {
        const char *a = argv[i];
        if (strcmp(a, "--") == 0) {
            i++;
            ended = true;
            break;
        } else if (strcmp(a, "--raw") == 0) {
            *flags |= SLM_PROMPT_RAW;
            i++;
        } else if (strcmp(a, "--max") == 0) {
            if (i == *last || shell_parse_uint(argv[i + 1], max) != 0) {
                shell_printf("slm %s: --max needs a number\r\n", verb);
                return -1;
            }
            i += 2;
        } else if (a[0] == '-' && a[1] == '-' && !strchr(a, ' ')) {
            shell_printf("slm %s: unknown option '%s' (use -- before text "
                         "that starts with dashes)\r\n", verb, a);
            return -1;
        } else {
            break;
        }
    }
    while (!ended && *last > i) {
        if (strcmp(argv[*last], "--raw") == 0) {
            *flags |= SLM_PROMPT_RAW;
            *last -= 1;
        } else if (*last - 1 > i && strcmp(argv[*last - 1], "--max") == 0 &&
                   shell_parse_uint(argv[*last], max) == 0) {
            *last -= 2;
        } else {
            break;
        }
    }
    return i;
}

/*
 * slm prompt <session> [--raw] [--max N] "<text>"
 *
 * The parser hands a quoted prompt over as one word. Several words are
 * joined with single spaces, so an unquoted prompt still works. Empty
 * text is valid and generates nothing.
 */
static int slm_prompt(int argc, char *argv[])
{
    if (argc < 4) {
        shell_puts("Usage: slm prompt <session> [--raw] [--max N] "
                   "\"<text>\"\r\n");
        return -1;
    }
    uint32_t sid;
    if (shell_parse_uint(argv[2], &sid) != 0) {
        shell_printf("slm prompt: invalid session '%s'\r\n", argv[2]);
        return -1;
    }

    uint32_t flags = 0;
    uint32_t max_new = SLM_DEFAULT_MAX_NEW_TOKENS;
    int last = argc - 1;
    int first = slm_parse_prompt_opts("prompt", 3, &last, argv, &flags, &max_new);
    if (first < 0) {
        return -1;
    }
    if (first > last) {
        shell_puts("slm prompt: no text\r\n");
        return -1;
    }

    /* Join the words in place, which also keeps a second line-sized buffer
     * off the shell task's stack. The words sit in ascending order in the
     * parser's buffer with a gap between each, so `out` never overtakes the
     * word being moved. */
    char *text = argv[first];
    char *out = text + strlen(text);
    for (int i = first + 1; i <= last; i++) {
        size_t wl = strlen(argv[i]);
        *out++ = ' ';
        memmove(out, argv[i], wl);
        out += wl;
    }
    *out = '\0';
    slm_unescape_inplace(text);

    return run_prompt(sid, text, flags, max_new);
}

/*
 * slm stream <session> [--raw] [--max N]
 *
 * Read a prompt from the shell I/O until a blank line, then run it.
 * This is the convenience path for serial users who don't want to
 * paste a large quoted block into the verb invocation, and it can hold
 * real newlines, so nothing is unescaped.
 */
static int slm_stream(int argc, char *argv[])
{
    if (argc < 3) {
        shell_puts("Usage: slm stream <session> [--raw] [--max N]\r\n");
        return -1;
    }
    uint32_t sid;
    if (shell_parse_uint(argv[2], &sid) != 0) {
        shell_printf("slm stream: invalid session '%s'\r\n", argv[2]);
        return -1;
    }

    uint32_t flags = 0;
    uint32_t max_new = SLM_DEFAULT_MAX_NEW_TOKENS;
    int last = argc - 1;
    int first = slm_parse_prompt_opts("stream", 3, &last, argv, &flags, &max_new);
    if (first < 0) {
        return -1;
    }
    if (first <= last) {
        shell_printf("slm stream: unexpected '%s'\r\n", argv[first]);
        return -1;
    }

    shell_puts("[slm] stream: type prompt; finish with a blank line.\r\n");

    char accum[SHELL_MAX_LINE];
    size_t apos = 0;
    accum[0] = '\0';

    /* Read up to ~16 lines of text. The accumulator is bounded so a
     * runaway paste can't overrun. */
    for (int line = 0; line < 16; line++) {
        char buf[SHELL_MAX_LINE];
        /* `slm` is a mutating command: don't wait for input holding the
         * shell mutation lock (#1103). run_prompt checks the session id
         * after the lock is back. */
        bool paused = shell_mutation_pause();
        int n = shell_read_line(buf, sizeof(buf));
        shell_mutation_resume(paused);
        if (n < 0) {
            shell_puts("[slm] stream: session closed\r\n");
            return -1;
        }
        if (n == 0) break; /* blank line ends the prompt */
        if (apos + (size_t)n + 2u >= sizeof(accum)) {
            shell_puts("[slm] stream: prompt truncated\r\n");
            break;
        }
        if (apos > 0) accum[apos++] = '\n';
        memcpy(accum + apos, buf, (size_t)n);
        apos += (size_t)n;
        accum[apos] = '\0';
    }

    if (apos == 0) {
        shell_puts("[slm] stream: empty prompt\r\n");
        return 0;
    }

    return run_prompt(sid, accum, flags, max_new);
}

static int slm_status(void)
{
    uint32_t models = rust_slm_count();
    /* The session count isn't exposed through M5.2's FFI directly —
     * walk an upper bound (matching the kernel's session-pool cap).
     * Empty stat() returns -1 so we just count successes. */
    uint32_t sessions = 0;
    SlmStatsC st;
    for (uint32_t i = 0; i < 32u; i++) {
        if (rust_slm_stats(i, &st) == 0) sessions++;
    }
    shell_printf("models=%lu sessions=%lu (run `slm gpu` for per-op "
                 "tier state)\r\n",
                 (unsigned long)models, (unsigned long)sessions);
    return 0;
}

static int slm_stats(int argc, char *argv[])
{
    if (argc < 3) {
        shell_puts("Usage: slm stats <session>\r\n");
        return -1;
    }
    uint32_t sid;
    if (shell_parse_uint(argv[2], &sid) != 0) {
        shell_printf("slm stats: invalid session '%s'\r\n", argv[2]);
        return -1;
    }
    SlmStatsC st;
    if (rust_slm_stats(sid, &st) != 0) {
        shell_printf("slm stats: session %lu not open\r\n",
                     (unsigned long)sid);
        return -1;
    }
    unsigned long ttft_ms   = (unsigned long)(st.last_ttft_ns / 1000000ull);
    unsigned long decode_ms = (unsigned long)(st.last_decode_ns / 1000000ull);
    shell_printf("session %lu  prompts=%lu  tokens_in=%lu  tokens_out=%lu\r\n",
                 (unsigned long)sid,
                 (unsigned long)st.prompts_completed,
                 (unsigned long)st.tokens_in,
                 (unsigned long)st.tokens_out);
    shell_printf("  ttft     : %lu ms\r\n", ttft_ms);
    shell_printf("  decode   : %lu ms total\r\n", decode_ms);
    shell_printf("  stop     : %s\r\n",
                 slm_stop_reason_name(st.last_stop_reason));
    SlmSessionInfoC info;
    if (rust_slm_session_info(sid, &info) == 0) {
        shell_printf("  kv_used  : %lu / %lu\r\n",
                     (unsigned long)info.kv_len,
                     (unsigned long)info.max_ctx);
        shell_puts("  ");
        slm_print_backend(sid, info.gpu_mode);
        shell_printf("\r\n  threads  : %lu (stored; forward is "
                     "single-threaded)\r\n", (unsigned long)info.threads);
        if (info.busy) {
            shell_puts("  prompt running: figures are from before it "
                       "started\r\n");
        }
    }
    return 0;
}

/* Each open session: the path it's on and how many tokens ran where. */
static void slm_gpu_sessions(void)
{
    uint64_t emc = 0, gpc = 0;
    slm_prog_clocks(&emc, &gpc);
    shell_printf("clocks: EMC %llu MHz, GPC %llu MHz\r\n",
                 (unsigned long long)(emc / 1000000u),
                 (unsigned long long)(gpc / 1000000u));
    for (uint32_t sid = 0; sid < 4u; sid++) {
        SlmSessionGpuC g;
        if (rust_slm_session_gpu_info(sid, &g) != 0) continue;
        const char *path = g.path == SLM_PATH_PROGRAM ? "GPU program"
                         : g.path == SLM_PATH_PER_OP ? "per-op tier table" : "CPU";
        shell_printf("session %lu: %s, gpu_tokens=%llu cpu_tokens=%llu gpu_us/token=%llu",
                     (unsigned long)sid, path,
                     (unsigned long long)g.gpu_tokens, (unsigned long long)g.cpu_tokens,
                     (unsigned long long)g.gpu_us_per_token);
        if (g.path == SLM_PATH_PROGRAM) {
            shell_printf(" launches=%lu barriers=%lu batch=%lu weights=%s",
                         (unsigned long)g.launches, (unsigned long)g.barriers,
                         (unsigned long)g.max_batch, g.weights_big ? "64KB" : "4KB");
        }
        if (g.note[0]) shell_printf(" (%s)", g.note);
        shell_puts("\r\n");
    }
}

/* slm gpu opts <wfi-flush|flush|wfi> [lean] [profile] [simt]: applies to the next
 * launch. Exists to measure barriers on hardware, not to be tuned by hand. */
static int slm_gpu_opts(int argc, char *argv[])
{
    if (argc < 4) {
        shell_puts("Usage: slm gpu opts <wfi-flush|flush|wfi> [lean] [profile] [simt]\r\n");
        return -1;
    }
    uint32_t barrier;
    if (strcmp(argv[3], "wfi-flush") == 0) barrier = 0;
    else if (strcmp(argv[3], "flush") == 0) barrier = 1;
    else if (strcmp(argv[3], "wfi") == 0) barrier = 2;
    else {
        shell_printf("slm gpu opts: unknown barrier '%s'\r\n", argv[3]);
        return -1;
    }
    uint32_t flags = 0;
    for (int i = 4; i < argc; i++) {
        if (strcmp(argv[i], "lean") == 0) flags |= SLM_PROG_FLAG_LEAN_QMD;
        else if (strcmp(argv[i], "profile") == 0) flags |= SLM_PROG_FLAG_PROFILE;
        else if (strcmp(argv[i], "simt") == 0) flags |= SLM_PROG_FLAG_SIMT;
        else {
            shell_printf("slm gpu opts: unknown option '%s'\r\n", argv[i]);
            return -1;
        }
    }
    rust_slm_gpu_set_options(barrier, flags);
    shell_puts("ok, for the next `slm launch`\r\n");
    return 0;
}

#ifdef SLM_GPU_GOLDEN
extern const uint8_t slm_gpu_golden_tiny_start[], slm_gpu_golden_tiny_end[];
extern const uint8_t slm_gpu_golden_rec_start[], slm_gpu_golden_rec_end[];

/* slm gpu golden [batch]: the tiny Qwen2 model on the GPU program, every
 * step's logits against llama.cpp. Needs a build with SLM_GPU_GOLDEN=ON. */
static int slm_gpu_golden(int argc, char *argv[])
{
    bool batched = argc >= 4 && strcmp(argv[3], "batch") == 0;
    int idx = rust_slm_load((const uint8_t *)"tiny-qwen2", slm_gpu_golden_tiny_start,
                            (size_t)(slm_gpu_golden_tiny_end - slm_gpu_golden_tiny_start));
    if (idx < 0) {
        shell_puts("slm gpu golden: could not load the fixture\r\n");
        return -1;
    }
    SlmGpuGoldenReport r;
    int32_t rc = rust_slm_gpu_golden((uint32_t)idx, slm_gpu_golden_rec_start,
                                     (size_t)(slm_gpu_golden_rec_end - slm_gpu_golden_rec_start),
                                     batched ? 1u : 0u, &r);
    (void)rust_slm_unload((uint32_t)idx);
    if (rc != 0) {
        shell_puts("slm gpu golden: FAIL, the program did not run\r\n");
        return -1;
    }
    if (r.first_bad_step < 0) {
        shell_printf("slm gpu golden: PASS, %lu steps, %lu same top token, max error %d.%03d, "
                     "%llu us/token\r\n",
                     (unsigned long)r.steps, (unsigned long)r.argmax_matches,
                     (int)(r.max_err_milli / 1000), (int)(r.max_err_milli % 1000),
                     (unsigned long long)r.gpu_us_per_token);
        return 0;
    }
    shell_printf("slm gpu golden: FAIL at step %d logit %d: got %d milli, want %d milli "
                 "(%lu steps, %lu same top token, max error %d milli)\r\n",
                 (int)r.first_bad_step, (int)r.first_bad_index,
                 (int)r.first_bad_got_milli, (int)r.first_bad_want_milli,
                 (unsigned long)r.steps, (unsigned long)r.argmax_matches, (int)r.max_err_milli);
    return -1;
}
#endif

/* slm gpu smoke [n] [width]: launch, barrier, semaphore and evict with no model. */
static int slm_gpu_smoke(int argc, char *argv[])
{
    uint32_t n = 400u, width = 1536u;
    if ((argc >= 4 && (shell_parse_uint(argv[3], &n) != 0 || n == 0u || n > 2048u)) ||
        (argc >= 5 && (shell_parse_uint(argv[4], &width) != 0 || width == 0u || (width & 31u) ||
                       width > 8960u))) {
        shell_puts("Usage: slm gpu smoke [launches, 1..2048] [width, a multiple of 32]\r\n");
        return -1;
    }
    SlmGpuSmokeReport r;
    if (rust_slm_gpu_smoke(n, width, &r) != 0) {
        shell_puts("slm gpu smoke: FAIL, the program did not run\r\n");
        return -1;
    }
    shell_printf("slm gpu smoke: %s, %lu launches, %lu bad elements, gpu %llu us "
                 "(%llu ns/launch), wait %llu us, evict %llu us\r\n",
                 r.bad_elements == 0u ? "PASS" : "FAIL",
                 (unsigned long)r.launches, (unsigned long)r.bad_elements,
                 (unsigned long long)(r.gpu_ns / 1000u),
                 (unsigned long long)(r.gpu_ns / r.launches),
                 (unsigned long long)r.wait_us, (unsigned long long)r.evict_us);
    return r.bad_elements == 0u ? 0 : -1;
}

/* slm gpu opcheck <handle> [simt]: each kernel against the CPU reference. */
static int slm_gpu_opcheck(int argc, char *argv[])
{
    uint32_t h = 0;
    if (argc < 4 || shell_parse_uint(argv[3], &h) != 0) {
        shell_puts("Usage: slm gpu opcheck <handle> [simt]\r\n");
        return -1;
    }
    bool simt = argc >= 5 && strcmp(argv[4], "simt") == 0;
    int32_t rc = rust_slm_gpu_opcheck(h, simt ? 1u : 0u);
    shell_printf("slm gpu opcheck: %ld failed\r\n", (long)rc);
    return rc == 0 ? 0 : -1;
}

static int slm_gpu(int argc, char *argv[])
{
    if (argc >= 3 && strcmp(argv[2], "opts") == 0) return slm_gpu_opts(argc, argv);
    if (argc >= 3 && strcmp(argv[2], "opcheck") == 0) return slm_gpu_opcheck(argc, argv);
    if (argc >= 4 && strcmp(argv[2], "profile") == 0) {
        uint32_t sid = 0;
        if (shell_parse_uint(argv[3], &sid) != 0 || rust_slm_session_gpu_profile(sid) != 0) {
            shell_puts("slm gpu profile: no session on a GPU program there\r\n");
            return -1;
        }
        return 0;
    }
    if (argc >= 3 && strcmp(argv[2], "smoke") == 0) return slm_gpu_smoke(argc, argv);
#ifdef SLM_GPU_GOLDEN
    if (argc >= 3 && strcmp(argv[2], "golden") == 0) return slm_gpu_golden(argc, argv);
#endif
    slm_gpu_sessions();
    /* Walk the 8 first-class SLM op kinds and report which tier the
     * forward path will route each one through. The tier table is
     * populated at boot by `oplib_probe_run`, which flips an entry
     * from CPU to SIMT once the op's metadata + SASS pass the static
     * smoke probe. HMMA tier is reserved for M6.B kernels not yet
     * shipped. */
    static const struct { uint32_t op; const char *name; } ops[] = {
        { 0u, "RMSNORM"   },
        { 1u, "ROPE"      },
        { 2u, "EMBEDDING" },
        { 3u, "Q4K_DOT"   },
        { 4u, "Q4K_GEMM"  },
        { 5u, "GQA_ATTN"  },
        { 6u, "SWIGLU"    },
        { 7u, "LM_HEAD"   },
    };
    shell_puts("op           tier  attempts          ok          fallback\r\n");
    shell_puts("-----------  ----  ----------------  ----------  --------\r\n");
    for (size_t i = 0; i < sizeof(ops) / sizeof(ops[0]); i++) {
        int tier = slm_runtime_get_tier(ops[i].op);
        const char *tname =
            (tier == 0) ? "AUTO" :
            (tier == 1) ? "HMMA" :
            (tier == 2) ? "SIMT" :
            (tier == 3) ? "CPU"  : "ERR";
        uint64_t attempts = 0, ok = 0;
        slm_runtime_dispatch_stats(ops[i].op, &attempts, &ok);
        uint64_t fallback = attempts > ok ? (attempts - ok) : 0;
        shell_printf("%-11s  %-4s  %-16llu  %-10llu  %-llu\r\n",
                     ops[i].name, tname,
                     (unsigned long long)attempts,
                     (unsigned long long)ok,
                     (unsigned long long)fallback);
    }
    return 0;
}

/* ============================================================================
 * Top-level dispatch
 * ========================================================================= */

static void slm_print_usage(void)
{
    shell_puts("Usage: slm <verb> [args]\r\n");
    shell_puts("  load   <path>                  Load a GGUF model from VFS\r\n");
    shell_puts("  xload  <name> <total>          Stream-load a GGUF over telnet "
               "(no LittleFS round-trip)\r\n");
    shell_puts("  list                           List loaded SLMs\r\n");
    shell_puts("  info   <handle>                Show model metadata\r\n");
    shell_puts("  unload <handle>                Unload a model\r\n");
    shell_puts("  launch <handle> [opts]         Open a session "
               "(--ctx --sampler --temp --topk --topp --seed --gpu --threads)\r\n");
    shell_puts("  prompt <session> [--raw] [--max N] \"<text>\"  Run a prompt "
               "(Ctrl-C stops it)\r\n");
    shell_puts("  stream <session> [--raw] [--max N]  Read a prompt from UART\r\n");
    shell_puts("  stop   <session>               Stop a running prompt from "
               "another session\r\n");
    shell_puts("  reset  <session>               Reset KV cache\r\n");
    shell_puts("  close  <session>               Close a session\r\n");
    shell_puts("  status                         One-line summary\r\n");
    shell_puts("  stats  <session>               Per-session telemetry\r\n");
    shell_puts("  gpu                            GPU dispatch state\r\n");
}

/* Verbs that change global state: the registry, the session table, memory
 * pools. They serialize on the shell mutation lock. */
static int slm_dispatch_mutating(const char *verb, int argc, char *argv[])
{
    if (strcmp(verb, "load")   == 0) return slm_load(argc, argv);
    if (strcmp(verb, "xload")  == 0) return slm_xload(argc, argv);
    if (strcmp(verb, "unload") == 0) return slm_unload(argc, argv);
    if (strcmp(verb, "launch") == 0) return slm_launch(argc, argv);
    if (strcmp(verb, "prompt") == 0) return slm_prompt(argc, argv);
    if (strcmp(verb, "stream") == 0) return slm_stream(argc, argv);
    if (strcmp(verb, "reset")  == 0) return slm_reset(argc, argv);
    if (strcmp(verb, "close")  == 0) return slm_close(argc, argv);

    shell_printf("slm: unknown verb '%s'\r\n", verb);
    slm_print_usage();
    return -1;
}

int cmd_slm(int argc, char *argv[])
{
    if (argc < 2) {
        slm_print_usage();
        return -1;
    }
    const char *verb = argv[1];

    /* These only read, under the runtime's own short locks, and must answer
     * while another session's command holds the mutation lock. `stop` above
     * all: the one thing it must never do is wait for what it's stopping. */
    if (strcmp(verb, "stop")   == 0) return slm_stop(argc, argv);
    if (strcmp(verb, "list")   == 0) return slm_list();
    if (strcmp(verb, "info")   == 0) return slm_info(argc, argv);
    if (strcmp(verb, "status") == 0) return slm_status();
    if (strcmp(verb, "stats")  == 0) return slm_stats(argc, argv);
    if (strcmp(verb, "gpu")    == 0) return slm_gpu(argc, argv);
    if (strcmp(verb, "help")   == 0) { slm_print_usage(); return 0; }

    bool locked = shell_mutation_lock();
    int ret = slm_dispatch_mutating(verb, argc, argv);
    shell_mutation_unlock(locked);
    return ret;
}
