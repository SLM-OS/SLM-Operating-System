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
 *   slm stats <s> [opts]     — rust_slm_telemetry pretty-print; live during a prompt
 *   slm gpu                  — per-op tier + dispatch counters
 *   slm runner start|stop|status — the slm-runner component (slm_runner.c)
 *   slm ask [--timeout S] "<text>"
 *                            — publish on /slm/prompt, print the reply from
 *                              /slm/token and /slm/done
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
#include "slm_runner.h"
#include "slm_prog.h"
#include "boot_models.h"
#include "string.h"
#include "timer.h"
#include "task.h"
#include "shell_session.h"

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
        /* BUSY is a running prompt, or a session's GPU program (even an
         * idle one) mapping the weights (#475 W5), or a closed program
         * whose close waits for the GPU program lock (#1159). */
        shell_printf("slm unload: handle %lu is in use: a prompt is running, "
                     "or a session's GPU program maps it (slm stop the prompt, "
                     "or close that session or relaunch it with --gpu cpu; "
                     "`slm runner stop` closes slm-runner's session; "
                     "a GPU session closed while the GPU was busy keeps it "
                     "mapped for a few seconds more)\r\n",
                     (unsigned long)idx);
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

/*
 * The session options `slm launch` and `slm runner start` share, parsed from
 * argv[first] on into `o` over the caller's defaults. `runner` also accepts
 * --handle, --max and --raw. Returns 0, or -1 after printing what's wrong.
 */
static int slm_parse_session_opts(const char *who, int argc, char *argv[],
                                  int first, slm_runner_config_t *o, bool runner)
{
    for (int i = first; i < argc; i++) {
        const char *a = argv[i];
        const char *v = (i + 1 < argc) ? argv[i + 1] : NULL;
        if (strcmp(a, "--ctx") == 0 && v) {
            if (shell_parse_uint(v, &o->max_ctx) != 0) {
                shell_printf("%s: bad --ctx '%s'\r\n", who, v); return -1;
            }
            i++;
        } else if (strcmp(a, "--sampler") == 0 && v) {
            if (strcmp(v, "greedy") == 0) {
                o->sampler_kind = SLM_SAMPLER_GREEDY;
            } else if (strcmp(v, "temp") == 0) {
                o->sampler_kind = SLM_SAMPLER_TEMPERATURE;
            } else if (strcmp(v, "topk") == 0) {
                o->sampler_kind = SLM_SAMPLER_TOP_K;
            } else if (strcmp(v, "topp") == 0) {
                o->sampler_kind = SLM_SAMPLER_TOP_P;
            } else if (strcmp(v, "topkp") == 0 ||
                       strcmp(v, "topktopp") == 0) {
                o->sampler_kind = SLM_SAMPLER_TOP_K_TOP_P;
            } else {
                shell_printf("%s: unknown sampler '%s' "
                             "(want greedy|temp|topk|topp|topkp)\r\n", who, v);
                return -1;
            }
            i++;
        } else if (strcmp(a, "--temp") == 0 && v) {
            if (parse_f32(v, &o->temperature) != 0) {
                shell_printf("%s: bad --temp '%s'\r\n", who, v); return -1;
            }
            i++;
        } else if (strcmp(a, "--topk") == 0 && v) {
            if (shell_parse_uint(v, &o->top_k) != 0) {
                shell_printf("%s: bad --topk '%s'\r\n", who, v); return -1;
            }
            i++;
        } else if (strcmp(a, "--topp") == 0 && v) {
            if (parse_f32(v, &o->top_p) != 0) {
                shell_printf("%s: bad --topp '%s'\r\n", who, v); return -1;
            }
            i++;
        } else if (strcmp(a, "--seed") == 0 && v) {
            uint32_t s32;
            if (shell_parse_uint(v, &s32) != 0) {
                shell_printf("%s: bad --seed '%s'\r\n", who, v); return -1;
            }
            o->seed = (uint64_t)s32;
            i++;
        } else if (strcmp(a, "--gpu") == 0 && v) {
            if (strcmp(v, "auto") == 0) {
                o->gpu_mode = SLM_GPU_AUTO;
            } else if (strcmp(v, "cpu") == 0) {
                o->gpu_mode = SLM_GPU_CPU;
            } else if (strcmp(v, "on") == 0) {
                o->gpu_mode = SLM_GPU_ON;
            } else {
                shell_printf("%s: bad --gpu '%s' "
                             "(want auto|cpu|on)\r\n", who, v);
                return -1;
            }
            i++;
        } else if (strcmp(a, "--threads") == 0 && v) {
            if (shell_parse_uint(v, &o->threads) != 0 ||
                o->threads == 0 || o->threads > 64u) {
                shell_printf("%s: bad --threads '%s' "
                             "(want 1..64)\r\n", who, v);
                return -1;
            }
            i++;
        } else if (runner && strcmp(a, "--raw") == 0) {
            o->prompt_flags = SLM_PROMPT_RAW;
        } else if (runner && strcmp(a, "--handle") == 0 && v) {
            uint32_t h;
            if (shell_parse_uint(v, &h) != 0 || h > 0x7FFFFFFFu) {
                shell_printf("%s: bad --handle '%s'\r\n", who, v);
                return -1;
            }
            o->model_handle = (int32_t)h;
            i++;
        } else if (runner && strcmp(a, "--max") == 0 && v) {
            if (shell_parse_uint(v, &o->max_new_tokens) != 0) {
                shell_printf("%s: bad --max '%s'\r\n", who, v);
                return -1;
            }
            i++;
        } else {
            shell_printf("%s: unknown option '%s'\r\n", who, a);
            return -1;
        }
    }

    return 0;
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
    slm_runner_config_t o;
    slm_runner_default_config(&o);
    o.max_ctx = 2048u;
    if (slm_parse_session_opts("slm launch", argc, argv, 3, &o, false) != 0) {
        return -1;
    }
    uint32_t max_ctx       = o.max_ctx;
    uint32_t sampler_kind  = o.sampler_kind;
    float    temperature   = o.temperature;
    uint32_t top_k         = o.top_k;
    float    top_p         = o.top_p;
    uint64_t seed          = o.seed;
    uint32_t gpu_mode      = o.gpu_mode;
    uint32_t threads       = o.threads;


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

/*
 * The line after a prompt. ttft runs from the prompt's start to its first
 * token, so it covers tokenizing and prefill; decode runs from that token to
 * the end. total is the two together: it printed decode alone, so on the CPU
 * path, where prefill is most of the time, total came out below ttft
 * (ttft=73100 ms, total=7331 ms on hardware).
 */
void slm_prompt_print_summary(const SlmStatsC *st, uint64_t tokens, uint64_t bytes)
{
    unsigned long ttft_ms  = (unsigned long)(st->last_ttft_ns / 1000000ull);
    unsigned long total_ms =
        (unsigned long)((st->last_ttft_ns + st->last_decode_ns) / 1000000ull);
    /* last_decode_ns starts after the first token, so it covers one
     * forward step per later token, plus the one that sampled EOS. */
    uint64_t steps = tokens > 0 ? tokens - 1u : 0;
    if (st->last_stop_reason == SLM_STOP_EOS && tokens > 0) {
        steps += 1u;
    }
    float tok_per_s = 0.0f;
    if (st->last_decode_ns > 0 && steps > 0) {
        double sec = (double)st->last_decode_ns / 1.0e9;
        tok_per_s = (float)((double)steps / sec);
    }
    shell_printf("[slm] ttft=%lu ms  decode=", ttft_ms);
    print_f32_3(tok_per_s);
    shell_printf(" tok/s  total=%lu ms tokens=%lu bytes=%lu stop=%s\r\n",
                 total_ms,
                 (unsigned long)tokens,
                 (unsigned long)bytes,
                 slm_stop_reason_name(st->last_stop_reason));
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
        slm_prompt_print_summary(&st, ctx.total_tokens, ctx.total_bytes);
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
static char *slm_join_words(char *argv[], int first, int last)
{
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
    return text;
}

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

    return run_prompt(sid, slm_join_words(argv, first, last), flags, max_new);
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

/* ---- slm stats ----
 *
 * Everything comes from rust_slm_telemetry, which never waits for a running
 * prompt. So nothing here may call rust_slm_session_info while one runs: that
 * takes the registry lock, which a CPU forward step holds throughout. */

/* n tokens over ns as tenths of a token per second. */
static uint64_t slm_rate_x10(uint64_t n, uint64_t ns)
{
    if (ns == 0u) {
        return 0u;
    }
    /* n * 1e10 fits u64 up to about 1.8e9 tokens. */
    return n * 10000000000ull / ns;
}

static void slm_print_x10(uint64_t v)
{
    shell_printf("%llu.%llu", (unsigned long long)(v / 10u), (unsigned long long)(v % 10u));
}

/* Nanoseconds as milliseconds with two decimals. */
static void slm_print_ms(uint64_t ns)
{
    uint64_t c = ns / 10000u;
    shell_printf("%llu.%02llu ms", (unsigned long long)(c / 100u), (unsigned long long)(c % 100u));
}

static void slm_print_bp(uint32_t bp)
{
    shell_printf("%lu.%02lu %%", (unsigned long)(bp / 100u), (unsigned long)(bp % 100u));
}

static const char *slm_tier_name(uint32_t tier)
{
    switch (tier) {
    case SLM_TIER_HMMA: return "tier-1 HMMA";
    case SLM_TIER_SIMT: return "tier-2 SIMT";
    default:            return "CPU";
    }
}

static const char *const slm_class_names[SLM_OPCLASS_COUNT] = {
    [SLM_OPCLASS_EMBED]   = "embed",
    [SLM_OPCLASS_RMSNORM] = "rmsnorm",
    [SLM_OPCLASS_MM_ATTN] = "matmul-attn",
    [SLM_OPCLASS_ROPE]    = "rope",
    [SLM_OPCLASS_GQA]     = "gqa",
    [SLM_OPCLASS_MM_MLP]  = "matmul-mlp",
    [SLM_OPCLASS_SWIGLU]  = "swiglu",
    [SLM_OPCLASS_ADD]     = "add",
    [SLM_OPCLASS_LM_HEAD] = "lm_head",
};

static void slm_stats_verbose(const SlmTelemetryC *t)
{
    if (t->itl_count == 0u) {
        shell_puts("  itl      : none yet\r\n");
        return;
    }
    shell_puts("  itl      : min ");
    slm_print_ms(t->itl_min_ns);
    shell_puts("  avg ");
    slm_print_ms(t->itl_avg_ns);
    shell_puts("  max ");
    slm_print_ms(t->itl_max_ns);
    shell_printf(" over %llu tokens\r\n", (unsigned long long)t->itl_count);
}

static void slm_stats_profile(const SlmTelemetryC *t)
{
    shell_printf("  profile  : %s, %llu tokens timed\r\n", t->profile ? "on" : "off",
                 (unsigned long long)t->profiled_tokens);
    if (t->itl_count != 0u) {
        shell_puts("  rolling  : last ");
        slm_print_ms(t->itl_last_ns);
        shell_puts(", last 32 avg ");
        slm_print_ms(t->itl_rolling_ns);
        shell_puts(" = ");
        slm_print_x10(slm_rate_x10(1u, t->itl_rolling_ns));
        shell_puts(" tok/s\r\n");
    }
    if (t->profiled_tokens == 0u) {
        return;
    }
    shell_printf("  per token (%s):\r\n",
                 t->path == SLM_PATH_PROGRAM ? "GPU timestamps" : "CPU timers");
    shell_puts("    class        ops  tier          last        avg\r\n");
    for (unsigned i = 0; i < SLM_OPCLASS_COUNT; i++) {
        const SlmOpClassC *c = &t->classes[i];
        shell_printf("    %-11s %4lu  %-11s ", slm_class_names[i],
                     (unsigned long)c->ops, slm_tier_name(c->tier));
        slm_print_ms(c->last_ns);
        shell_puts("  ");
        slm_print_ms(c->total_ns / t->profiled_tokens);
        shell_puts("\r\n");
    }
}

/* Tensor-core utilization. No number here says more than it is: the two derived
 * measures are named for what they divide, and the counter the spec asks for is
 * n/a until something reads it from hardware (docs/design/slm-integration.md,
 * "Tensor-core utilization"). */
static void slm_stats_tensor_core(const SlmTelemetryC *t)
{
    shell_puts("  tensor core utilization (information only, ADR 0005; prefill is where it is meaningful):\r\n");
    shell_puts("    mma issue time / matmul wall (derived): ");
    if (t->tc_issue_bp != SLM_TELEMETRY_NA) {
        slm_print_bp(t->tc_issue_bp);
        shell_printf(" (%llu mma per token; 3.44 G mma/s measured under JetPack at 1020 MHz, "
                     "scaled to GPC clock)\r\n", (unsigned long long)t->mma);
    } else if (t->mma != 0u) {
        shell_puts("n/a (needs --profile tokens and a known GPC clock)\r\n");
    } else {
        shell_puts("n/a (no op at tier 1)\r\n");
    }
    shell_puts("    tensor-core share of matmul+GQA MACs (derived): ");
    if (t->tc_mac_share_bp != SLM_TELEMETRY_NA) {
        slm_print_bp(t->tc_mac_share_bp);
        shell_puts("\r\n");
    } else {
        shell_puts("n/a (no op at tier 1)\r\n");
    }
    shell_puts("    SM tensor-pipe counter: n/a (not read: signal selects are not public)\r\n");
    for (unsigned i = 0; i < SLM_OPCLASS_COUNT; i++) {
        const SlmOpClassC *c = &t->classes[i];
        if (c->ops != 0u && c->tier != SLM_TIER_HMMA) {
            shell_printf("    %-11s n/a (%s)\r\n", slm_class_names[i], slm_tier_name(c->tier));
        }
    }
}

/* GPU time this prompt, split at the first token: prefill time is part of
 * the TTFT, decode time part of the decode wall, so neither exceeds its wall. */
void slm_stats_print_gpu_busy(const SlmTelemetryC *t)
{
    uint64_t steps = t->gpu_prefill_steps + t->gpu_decode_steps;
    shell_puts("  gpu busy : decode ");
    slm_print_ms(t->gpu_decode_ns);
    shell_printf(" over %llu steps (decode wall %lu ms), prefill ",
                 (unsigned long long)t->gpu_decode_steps,
                 (unsigned long)(t->decode_ns / 1000000u));
    slm_print_ms(t->gpu_prefill_ns);
    shell_printf(" over %llu steps\r\n", (unsigned long long)t->gpu_prefill_steps);
    shell_puts("             last step ");
    slm_print_ms(t->gpu_last_ns);
    shell_printf("; evict %llu us per step\r\n",
                 (unsigned long long)(steps ? t->gpu_evict_us / steps : 0u));
}

/* Jetson Orin Nano LPDDR5: 128-bit bus, two transfers per EMC clock. */
#define SLM_DRAM_BYTES_PER_EMC_CLOCK 32u
/* ADR 0005 target: llama.cpp CUDA's decode bandwidth fraction on the same
 * board and clocks, for the same bytes: this session's plan bytes per pass at
 * llama.cpp's decode rate (ADR 0001, MAXN_SUPER median 43.03 tok/s). Both
 * sides use the same bytes, so the test comes down to decode passes per
 * second against 43.03. That rate was measured on 64-token prompts; at long
 * context llama.cpp slows down too, so there the target is high and errs
 * toward "not met". */
#define SLM_LLAMA_DECODE_MILLITOK_S 43030u

/* Decode DRAM bandwidth (ADR 0005): the plan's bytes per decode forward pass
 * (weights plus KV at the current position, before any L2 hit) times the
 * number of decode forward passes (itl_count; the first token comes from the
 * prefill logits) over the decode wall, against the LPDDR5 peak (3199 MHz EMC
 * is 102.37 GB/s, printed truncated as 102.3). Derived, not a counter; the KV
 * term uses the final position, so it slightly overstates early tokens. */
void slm_stats_print_dram_bw(const SlmTelemetryC *t, uint64_t emc_hz)
{
    shell_puts("  dram bw  : decode ");
    if (t->itl_count == 0u || t->decode_ns == 0u || emc_hz == 0u) {
        shell_puts("n/a (no decode steps or EMC clock unknown)\r\n");
        return;
    }
    uint64_t per_pass = t->weight_bytes + t->kv_read_bytes;
    /* Bytes per microsecond is MB/s (decimal). per_pass (about 1e9) times
     * decode steps (at most max_ctx, 32768) stays far inside 64 bits. */
    uint64_t us = t->decode_ns / 1000u;
    uint64_t mb_s = us ? per_pass * t->itl_count / us : 0u;
    uint64_t peak_mb_s = emc_hz / 1000000u * SLM_DRAM_BYTES_PER_EMC_CLOCK;
    uint64_t bp = peak_mb_s ? mb_s * 10000u / peak_mb_s : 0u;
    shell_printf("%llu.%01llu GB/s = ", (unsigned long long)(mb_s / 1000u),
                 (unsigned long long)((mb_s % 1000u) / 100u));
    slm_print_bp((uint32_t)bp);
    shell_printf(" of %llu.%01llu GB/s LPDDR5 peak (derived from plan bytes per pass); "
                 "target llama.cpp parity ",
                 (unsigned long long)(peak_mb_s / 1000u),
                 (unsigned long long)((peak_mb_s % 1000u) / 100u));
    uint64_t parity_mb_s = per_pass * SLM_LLAMA_DECODE_MILLITOK_S / 1000000000u;
    uint64_t parity_bp = peak_mb_s ? parity_mb_s * 10000u / peak_mb_s : 0u;
    slm_print_bp((uint32_t)parity_bp);
    shell_puts(bp >= parity_bp ? ": met\r\n" : ": not met\r\n");
}

static void slm_stats_gpu(const SlmTelemetryC *t, uint64_t emc_hz, uint64_t gpc_hz)
{
    shell_printf("  clocks   : EMC %llu MHz, GPC %llu MHz\r\n",
                 (unsigned long long)(emc_hz / 1000000u),
                 (unsigned long long)(gpc_hz / 1000000u));
    shell_puts("  op tiers :\r\n");
    for (unsigned i = 0; i < SLM_OPCLASS_COUNT; i++) {
        shell_printf("    %-11s %4lu  %s\r\n", slm_class_names[i],
                     (unsigned long)t->classes[i].ops, slm_tier_name(t->classes[i].tier));
    }
    if (t->path == SLM_PATH_PROGRAM) {
        shell_printf("  launches : %lu per token, %lu barriers, %lu timestamps with --profile\r\n",
                     (unsigned long)t->launches, (unsigned long)t->barriers,
                     (unsigned long)t->marks);
        shell_printf("  dram est : %llu MB weights + %llu MB KV per token (plan, before L2)\r\n",
                     (unsigned long long)(t->weight_bytes >> 20),
                     (unsigned long long)(t->kv_read_bytes >> 20));
        slm_stats_print_dram_bw(t, emc_hz);
        slm_stats_print_gpu_busy(t);
    } else {
        shell_puts("  gpu busy : n/a (not on a GPU program)\r\n");
    }
    slm_stats_tensor_core(t);
}

static void slm_stats_print(uint32_t sid, const SlmTelemetryC *t)
{
    shell_printf("session %lu  prompts=%lu  tokens_in=%llu  tokens_out=%llu%s\r\n",
                 (unsigned long)sid, (unsigned long)t->prompts,
                 (unsigned long long)t->tokens_in, (unsigned long long)t->tokens_out,
                 t->running ? "  [prompt running: live]" : "");
    shell_printf("  ttft     : %lu ms\r\n", (unsigned long)(t->ttft_ns / 1000000u));
    shell_printf("  prefill  : %llu tokens, %lu ms, ",
                 (unsigned long long)t->prefill_tokens, (unsigned long)(t->prefill_ns / 1000000u));
    slm_print_x10(slm_rate_x10(t->prefill_tokens, t->prefill_ns));
    shell_puts(" tok/s\r\n");
    shell_printf("  decode   : %llu tokens, %lu ms total, ",
                 (unsigned long long)t->decode_tokens, (unsigned long)(t->decode_ns / 1000000u));
    slm_print_x10(slm_rate_x10(t->itl_count, t->decode_ns));
    shell_puts(" tok/s\r\n");
    if (!t->running) {
        shell_printf("  stop     : %s\r\n", slm_stop_reason_name(t->stop_reason));
    }
    shell_printf("  kv_used  : %lu / %lu\r\n", (unsigned long)t->kv_used,
                 (unsigned long)t->max_ctx);
    shell_printf("  peak mem : %llu MB (weights + KV + GPU arena)\r\n",
                 (unsigned long long)(t->peak_resident_bytes >> 20));
    shell_printf("  ops/token: tier-1 %lu, tier-2 %lu, CPU %lu\r\n",
                 (unsigned long)t->ops_tier1, (unsigned long)t->ops_tier2,
                 (unsigned long)t->ops_cpu);
    shell_printf("  oov      : %llu text pieces; sampler retries: n/a (the sampler never retries)\r\n",
                 (unsigned long long)t->oov_pieces);
}

/* One line per interval until the prompt ends or Ctrl-C. Reads only. */
static int slm_stats_follow(uint32_t sid, uint32_t ms)
{
    SlmTelemetryC t;
    if (rust_slm_telemetry(sid, 0u, &t) != 0) {
        return -1;
    }
    if (!t.running) {
        shell_puts("no prompt running\r\n");
        return 0;
    }
    while (t.running && !shell_session_poll_interrupt()) {
        shell_printf("decode %llu tok  ", (unsigned long long)t.decode_tokens);
        slm_print_x10(slm_rate_x10(t.itl_count, t.decode_ns));
        shell_puts(" tok/s  last ");
        slm_print_ms(t.itl_last_ns);
        shell_puts("  last-32 ");
        slm_print_ms(t.itl_rolling_ns);
        shell_printf("  kv %lu/%lu", (unsigned long)t.kv_used, (unsigned long)t.max_ctx);
        if (t.profiled_tokens != 0u) {
            shell_puts("  matmul ");
            slm_print_ms(t.matmul_avg_ns);
        }
        shell_puts("\r\n");
        task_sleep_ms(ms);
        if (rust_slm_telemetry(sid, 0u, &t) != 0) {
            break;
        }
    }
    shell_puts("follow ended\r\n");
    return 0;
}

static void slm_print_temperatures(void)
{
#ifdef PLATFORM_JETSON_ORIN_NANO
    shell_thermal_print_line("    ");
#else
    shell_puts("    n/a (no BPMP on this platform)\r\n");
#endif
}

#define SLM_STATS_USAGE \
    "Usage: slm stats <session> [--verbose] [--gpu] [--power] " \
    "[--profile [on|off]] [--follow [ms]]\r\n"

static int slm_stats(int argc, char *argv[])
{
    if (argc < 3) {
        shell_puts(SLM_STATS_USAGE);
        return -1;
    }
    uint32_t sid;
    if (shell_parse_uint(argv[2], &sid) != 0) {
        shell_printf("slm stats: invalid session '%s'\r\n", argv[2]);
        return -1;
    }
    bool verbose = false, gpu = false, power = false, profile = false, follow = false;
    int profile_set = -1;
    uint32_t follow_ms = 1000u;
    for (int i = 3; i < argc; i++) {
        if (strcmp(argv[i], "--verbose") == 0) {
            verbose = true;
        } else if (strcmp(argv[i], "--gpu") == 0) {
            gpu = true;
        } else if (strcmp(argv[i], "--power") == 0) {
            power = true;
        } else if (strcmp(argv[i], "--profile") == 0) {
            profile = true;
            profile_set = 1;
            if (i + 1 < argc && strcmp(argv[i + 1], "on") == 0) {
                i++;
            } else if (i + 1 < argc && strcmp(argv[i + 1], "off") == 0) {
                profile_set = 0;
                i++;
            }
        } else if (strcmp(argv[i], "--follow") == 0) {
            follow = true;
            if (i + 1 < argc && argv[i + 1][0] != '-') {
                if (shell_parse_uint(argv[i + 1], &follow_ms) != 0 || follow_ms == 0u) {
                    shell_printf("slm stats: bad interval '%s'\r\n", argv[i + 1]);
                    return -1;
                }
                i++;
            }
        } else {
            shell_printf("slm stats: unknown option '%s'\r\n", argv[i]);
            shell_puts(SLM_STATS_USAGE);
            return -1;
        }
    }
    if (profile_set >= 0 && rust_slm_telemetry_profile(sid, (uint32_t)profile_set) != 0) {
        shell_printf("slm stats: session %lu not open\r\n", (unsigned long)sid);
        return -1;
    }
    if (follow) {
        if (slm_stats_follow(sid, follow_ms) != 0) {
            shell_printf("slm stats: session %lu not open\r\n", (unsigned long)sid);
            return -1;
        }
        return 0;
    }

    /* A BPMP query is too slow to make every plain `slm stats` pay for it. */
    uint64_t emc = 0, gpc = 0;
    if (gpu || profile) {
        slm_prog_clocks(&emc, &gpc);
    }
    SlmTelemetryC t;
    if (rust_slm_telemetry(sid, gpc, &t) != 0) {
        shell_printf("slm stats: session %lu not open\r\n", (unsigned long)sid);
        return -1;
    }
    slm_stats_print(sid, &t);
    if (verbose) slm_stats_verbose(&t);
    if (profile) slm_stats_profile(&t);
    if (gpu) slm_stats_gpu(&t, emc, gpc);
    if (power) {
        shell_puts("  power    : n/a (no rail sensor is read); temperatures only, in C:\r\n");
        slm_print_temperatures();
    }
    /* Takes the registry lock, so only when no forward step can hold it. */
    SlmSessionInfoC info;
    if (!t.running && rust_slm_session_busy(sid) == 0 &&
        rust_slm_session_info(sid, &info) == 0) {
        shell_puts("  ");
        slm_print_backend(sid, info.gpu_mode);
        shell_printf("\r\n  threads  : %lu (stored; forward is "
                     "single-threaded)\r\n", (unsigned long)info.threads);
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
    if (strcmp(argv[3], "wfi-flush") == 0) {
        barrier = 0;
    } else if (strcmp(argv[3], "flush") == 0) {
        barrier = 1;
    } else if (strcmp(argv[3], "wfi") == 0) {
        barrier = 2;
    } else {
        shell_printf("slm gpu opts: unknown barrier '%s'\r\n", argv[3]);
        return -1;
    }
    uint32_t flags = 0;
    for (int i = 4; i < argc; i++) {
        if (strcmp(argv[i], "lean") == 0) {
            flags |= SLM_PROG_FLAG_LEAN_QMD;
        } else if (strcmp(argv[i], "profile") == 0) {
            flags |= SLM_PROG_FLAG_PROFILE;
        } else if (strcmp(argv[i], "simt") == 0) {
            flags |= SLM_PROG_FLAG_SIMT;
        } else {
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
    bool ok = r.first_bad_step < 0 && r.exact_first_bad_step < 0 && r.argmax_matches == r.steps;
    shell_printf("slm gpu golden: %s, %lu steps, %lu same top token, vs llama.cpp max %d milli "
                 "(first over 1500 at step %d), vs exact max %d milli (first over 100 at step %d), "
                 "%llu us/token\r\n",
                 ok ? "PASS" : "FAIL", (unsigned long)r.steps, (unsigned long)r.argmax_matches,
                 (int)r.max_err_milli, (int)r.first_bad_step, (int)r.exact_max_err_milli,
                 (int)r.exact_first_bad_step, (unsigned long long)r.gpu_us_per_token);
    return ok ? 0 : -1;
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
        int32_t rc = shell_parse_uint(argv[3], &sid) == 0 ? rust_slm_session_gpu_profile(sid) : -1;
        if (rc == SLM_ERR_BUSY) {
            shell_puts("slm gpu profile: a prompt is running on that session\r\n");
            return -1;
        }
        if (rc != 0) {
            shell_puts("slm gpu profile: no session on a GPU program there\r\n");
            return -1;
        }
        return 0;
    }
    if (argc >= 3 && strcmp(argv[2], "smoke") == 0) return slm_gpu_smoke(argc, argv);
    if (argc >= 4 && strcmp(argv[2], "inject") == 0) {
        uint32_t kind = strcmp(argv[3], "hang") == 0 ? 1u : strcmp(argv[3], "fault") == 0 ? 2u : 0u;
        rust_slm_gpu_set_inject(kind);
        shell_printf("next `slm launch`: decode %s\r\n",
                     kind == 1u ? "hangs" : kind == 2u ? "faults" : "runs normally");
        return 0;
    }
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

/*
 * slm runner start [slm launch's options] [--handle H] [--max N] [--raw]
 * slm runner stop
 * slm runner status
 *
 * No --handle: each prompt uses the most recently loaded model.
 */
static int slm_runner_verb(int argc, char *argv[])
{
    static const char *const gpu_names[] = { "auto", "cpu", "on" };
    const char *sub = argc >= 3 ? argv[2] : "";

    if (strcmp(sub, "start") == 0) {
        slm_runner_config_t cfg;
        slm_runner_default_config(&cfg);
        if (slm_parse_session_opts("slm runner start", argc, argv, 3, &cfg,
                                   true) != 0) {
            return -1;
        }
        slm_runner_status_t now;
        slm_runner_get_status(&now);
        if (now.running) {
            shell_puts("slm runner start: already running "
                       "(`slm runner stop` first)\r\n");
            return -1;
        }
        /* One call, so a second shell's start can't come between the
         * configure and the start (#475 W9). */
        int rc = slm_runner_start_with(&cfg);
        if (rc == SLM_RUNNER_ERR_BUSY) {
            shell_puts("slm runner start: another start or stop is in "
                       "progress\r\n");
            return -1;
        }
        if (rc != 0) {
            shell_puts("slm runner start: failed\r\n");
            return -1;
        }
    } else if (strcmp(sub, "stop") == 0) {
        int rc = slm_runner_stop();
        if (rc == SLM_RUNNER_ERR_BUSY) {
            shell_puts("slm runner stop: another start or stop is in "
                       "progress\r\n");
            return -1;
        }
        if (rc == -1) {
            shell_puts("slm runner stop: not running\r\n");
            return -1;
        }
        if (rc != 0) {
            shell_puts("slm runner stop: still running after 60 s\r\n");
            return -1;
        }
    } else if (strcmp(sub, "status") != 0) {
        shell_puts("Usage: slm runner start [--handle H] [--max N] [--raw] "
                   "[slm launch options]\r\n"
                   "       slm runner stop\r\n"
                   "       slm runner status\r\n");
        return -1;
    }

    slm_runner_status_t st;
    slm_runner_get_status(&st);
    if (!st.running) {
        shell_printf("slm-runner: stopped prompts=%lu dropped=%lu\r\n",
                     (unsigned long)st.prompts, (unsigned long)st.dropped);
        return 0;
    }
    shell_printf("slm-runner: running component=%ld session=%ld model=%ld "
                 "gpu=%s busy=%lu prompts=%lu dropped=%lu\r\n",
                 (long)st.component_idx, (long)st.session, (long)st.model,
                 st.gpu_mode < 3u ? gpu_names[st.gpu_mode] : "?",
                 (unsigned long)st.busy, (unsigned long)st.prompts,
                 (unsigned long)st.dropped);
    return 0;
}

/*
 * slm ask [--timeout S] "<text>"
 *
 * Long enough by default to outlast a CPU prefill of a long prompt on the
 * real model: at 0.4 tok/s a 64-token prompt takes about four minutes before
 * its first token.
 */
#define SLM_ASK_DEFAULT_TIMEOUT_S 600u

static int slm_ask(int argc, char *argv[])
{
    int first = 2;
    uint32_t timeout_s = SLM_ASK_DEFAULT_TIMEOUT_S;
    if (argc >= 4 && strcmp(argv[2], "--timeout") == 0) {
        if (shell_parse_uint(argv[3], &timeout_s) != 0 || timeout_s == 0) {
            shell_printf("slm ask: bad --timeout '%s'\r\n", argv[3]);
            return -1;
        }
        first = 4;
    }
    if (first >= argc) {
        shell_puts("Usage: slm ask [--timeout S] \"<text>\"\r\n");
        return -1;
    }
    return slm_runner_ask(slm_join_words(argv, first, argc - 1), timeout_s);
}

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
    shell_puts("  stats  <session> [--verbose] [--gpu] [--power] [--profile [on|off]] [--follow [ms]]\r\n"
               "                                 Per-session telemetry, live during a prompt\r\n");
    shell_puts("  gpu                            GPU dispatch state\r\n");
    shell_puts("  runner start|stop|status       The slm-runner bus service\r\n");
    shell_puts("  ask    [--timeout S] \"<text>\"  Ask slm-runner over the "
               "message bus\r\n");
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

    /* These skip the mutation lock and must answer while another session's
     * command holds it. `stop` above all: the one thing it must never do is
     * wait for what it's stopping. Most only read, under the runtime's own
     * short locks. Some `gpu` verbs change state (profile turns a session's
     * profiling on, inject arms a fault, golden loads a model, smoke runs a
     * GPU program), relying on the runtime's own locks, not this one. */
    if (strcmp(verb, "stop")   == 0) return slm_stop(argc, argv);
    if (strcmp(verb, "list")   == 0) return slm_list();
    if (strcmp(verb, "info")   == 0) return slm_info(argc, argv);
    if (strcmp(verb, "status") == 0) return slm_status();
    if (strcmp(verb, "stats")  == 0) return slm_stats(argc, argv);
    if (strcmp(verb, "gpu")    == 0) return slm_gpu(argc, argv);
    /* Same: they wait on a task, and the task can need the shell to run. */
    if (strcmp(verb, "runner") == 0) return slm_runner_verb(argc, argv);
    if (strcmp(verb, "ask")    == 0) return slm_ask(argc, argv);
    if (strcmp(verb, "help")   == 0) { slm_print_usage(); return 0; }

    bool locked = shell_mutation_lock();
    int ret = slm_dispatch_mutating(verb, argc, argv);
    shell_mutation_unlock(locked);
    return ret;
}
