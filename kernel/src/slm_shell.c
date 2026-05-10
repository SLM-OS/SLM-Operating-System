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
 *   slm prompt <s> "<text>"  — rust_slm_prompt with UART token cb
 *   slm stream <s>           — read prompt from UART line, then prompt
 *   slm stop <s>             — rust_slm_stop
 *   slm reset <s>            — rust_slm_session_reset
 *   slm unload <h>           — rust_slm_unload
 *   slm close <s>            — rust_slm_session_close
 *   slm status               — model count + session count summary
 *   slm stats <s>            — rust_slm_stats pretty-print
 *   slm gpu                  — placeholder (M6.A-3 not yet wired)
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
#include "slm_ffi.h"
#include "string.h"
#include "timer.h"

#include <stdint.h>
#include <stddef.h>

/* ============================================================================
 * Helpers
 * ========================================================================= */

/* Trim leading/trailing whitespace in place; returns the (possibly
 * advanced) start of the resulting string. NUL-terminates the trimmed
 * end. */
static char *trim_inplace(char *s)
{
    if (!s) return s;
    while (*s == ' ' || *s == '\t') s++;
    size_t n = strlen(s);
    while (n > 0 && (s[n - 1] == ' ' || s[n - 1] == '\t' ||
                     s[n - 1] == '\r' || s[n - 1] == '\n')) {
        s[--n] = '\0';
    }
    return s;
}

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
    uint8_t  stop_requested;
} slm_token_cb_ctx_t;

static int32_t slm_prompt_token_cb(
    void *user,
    uint32_t token_id,
    const uint8_t *bytes,
    size_t bytes_len)
{
    (void)token_id;
    slm_token_cb_ctx_t *ctx = (slm_token_cb_ctx_t *)user;
    if (!ctx) {
        return 0; /* defensive — should never happen */
    }
    /* Stream raw bytes. shell_putc honors UART back-pressure via the
     * underlying uart_putc / shell_io_uart sink. */
    for (size_t i = 0; i < bytes_len; i++) {
        shell_putc((char)bytes[i]);
    }
    ctx->total_bytes  += bytes_len;
    ctx->total_tokens += 1u;
    return ctx->stop_requested ? 0 : 1;
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
    uint8_t *buf = (uint8_t *)pmm_alloc_pages(pages);
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
        pmm_free_pages(buf, pages);
        return -1;
    }
    shell_printf("SLM-XLOAD done received=%lu\r\n", (unsigned long)total);

    /* Transfer ownership of the streaming buffer to the registry. On
     * success the registry will free the pages on `slm unload`; we
     * MUST NOT call pmm_free_pages here. On failure (-1) the caller
     * still owns the pages and we free them ourselves. The take-pages
     * variant exists specifically because the original `rust_slm_load`
     * allocates a SECOND buffer of the same size to copy into — on a
     * Jetson 8 GB system the buddy allocator can only produce one
     * order-19 (2 GB) block at a time, so the alloc+copy path fails
     * for a 1.04 GB Q4_K_M GGUF even though the file itself fits in
     * available PMM. */
    int idx = rust_slm_load_take_pages((const uint8_t *)name_buf,
                                       buf, pages, (size_t)total);
    if (idx < 0) {
        pmm_free_pages(buf, pages);
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
    if (rust_slm_unload(idx) != 0) {
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
 *
 * Default sampler: TopKTopP (per the spec). Defaults match the spec
 * "demo" example: ctx=2048, temp=0.7, topk=40, topp=0.9, seed=1.
 */
static int slm_launch(int argc, char *argv[])
{
    if (argc < 3) {
        shell_puts("Usage: slm launch <handle> [--ctx N] [--sampler "
                   "greedy|temp|topk|topp|topkp] [--temp F] [--topk N] "
                   "[--topp F] [--seed N]\r\n");
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

    for (int i = 3; i < argc; i++) {
        const char *a = argv[i];
        const char *v = (i + 1 < argc) ? argv[i + 1] : NULL;
        if (strcmp(a, "--ctx") == 0 && v) {
            if (shell_parse_uint(v, &max_ctx) != 0) {
                shell_printf("slm launch: bad --ctx '%s'\r\n", v); return -1;
            }
            i++;
        } else if (strcmp(a, "--sampler") == 0 && v) {
            if (strcmp(v, "greedy") == 0)        sampler_kind = SLM_SAMPLER_GREEDY;
            else if (strcmp(v, "temp") == 0)     sampler_kind = SLM_SAMPLER_TEMPERATURE;
            else if (strcmp(v, "topk") == 0)     sampler_kind = SLM_SAMPLER_TOP_K;
            else if (strcmp(v, "topp") == 0)     sampler_kind = SLM_SAMPLER_TOP_P;
            else if (strcmp(v, "topkp") == 0 ||
                     strcmp(v, "topktopp") == 0) sampler_kind = SLM_SAMPLER_TOP_K_TOP_P;
            else {
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

    const char *sname =
        (sampler_kind == SLM_SAMPLER_GREEDY)        ? "greedy" :
        (sampler_kind == SLM_SAMPLER_TEMPERATURE)   ? "temp"   :
        (sampler_kind == SLM_SAMPLER_TOP_K)         ? "topk"   :
        (sampler_kind == SLM_SAMPLER_TOP_P)         ? "topp"   :
        (sampler_kind == SLM_SAMPLER_TOP_K_TOP_P)   ? "topkp"  :
                                                      "?";
    shell_printf("[slm] session=%ld backend=CPU(M5.2 stub) sampler=%s "
                 "ctx=%lu",
                 (long)sid, sname, (unsigned long)max_ctx);
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
    if (rust_slm_session_close(sid) != 0) {
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
    if (rust_slm_session_reset(sid) != 0) {
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
    shell_printf("Stop signaled for session %lu\r\n", (unsigned long)sid);
    return 0;
}

/*
 * Run a prompt against an open session. `text` may be NULL/empty
 * for the M5.2 state-machine path that exercises stop / EOS without
 * a tokenizer (real text path lands in M5.3). `max_new_tokens` of
 * 0 means "unlimited" per the FFI contract.
 */
static int run_prompt(uint32_t sid, const char *text, uint32_t max_new_tokens)
{
    slm_token_cb_ctx_t ctx = {
        .total_bytes    = 0,
        .total_tokens   = 0,
        .stop_requested = 0,
    };

    size_t plen = (text != NULL) ? strlen(text) : 0;
    int32_t rc = rust_slm_prompt(sid,
                                 (const uint8_t *)text,
                                 plen,
                                 max_new_tokens,
                                 slm_prompt_token_cb,
                                 &ctx);

    /* Always end the streamed line cleanly so the next prompt is
     * on its own row. */
    shell_puts("\r\n");

    if (rc != 0) {
        shell_printf("slm prompt: decode failed rc=%ld "
                     "(session not open, invalid UTF-8, or M5.2 "
                     "tokenizer not yet plumbed)\r\n", (long)rc);
        return -1;
    }

    /* Telemetry summary (decode rate intentionally rounded to 1 dec). */
    SlmStatsC st;
    if (rust_slm_stats(sid, &st) == 0) {
        unsigned long ttft_ms   = (unsigned long)(st.last_ttft_ns / 1000000ull);
        unsigned long decode_ms = (unsigned long)(st.last_decode_ns / 1000000ull);
        /* tokens / sec — guard against div-by-zero. */
        float tok_per_s = 0.0f;
        if (st.last_decode_ns > 0 && ctx.total_tokens > 0) {
            double sec = (double)st.last_decode_ns / 1.0e9;
            tok_per_s = (float)((double)ctx.total_tokens / sec);
        }
        shell_printf("[slm] ttft=%lu ms  decode=", ttft_ms);
        print_f32_3(tok_per_s);
        shell_printf(" tok/s  total=%lu ms tokens=%lu bytes=%lu\r\n",
                     decode_ms,
                     (unsigned long)ctx.total_tokens,
                     (unsigned long)ctx.total_bytes);
    }
    return 0;
}

/*
 * slm prompt <session> "<text>"
 *
 * The shell tokenizes argv on whitespace, so a quoted prompt arrives
 * as multiple argv slots. Re-stitch them back together into a single
 * prompt buffer (still bounded by SHELL_MAX_LINE).
 */
static int slm_prompt(int argc, char *argv[])
{
    if (argc < 4) {
        shell_puts("Usage: slm prompt <session> <text...>\r\n");
        return -1;
    }
    uint32_t sid;
    if (shell_parse_uint(argv[2], &sid) != 0) {
        shell_printf("slm prompt: invalid session '%s'\r\n", argv[2]);
        return -1;
    }

    /* Stitch argv[3..] into one line buffer. */
    char text[SHELL_MAX_LINE];
    size_t pos = 0;
    for (int i = 3; i < argc; i++) {
        size_t wl = strlen(argv[i]);
        if (i > 3) {
            if (pos + 1 >= sizeof(text)) break;
            text[pos++] = ' ';
        }
        if (pos + wl >= sizeof(text)) {
            wl = sizeof(text) - pos - 1;
        }
        memcpy(text + pos, argv[i], wl);
        pos += wl;
    }
    text[pos] = '\0';

    /* Strip a single pair of surrounding quotes if present (the
     * shell parser doesn't strip them itself). */
    char *p = trim_inplace(text);
    size_t pl = strlen(p);
    if (pl >= 2 && ((p[0] == '"' && p[pl - 1] == '"') ||
                    (p[0] == '\'' && p[pl - 1] == '\''))) {
        p[pl - 1] = '\0';
        p++;
    }

    /* 256 is the spec default — see "max_new_tokens" in the M5
     * decode loop documentation. */
    return run_prompt(sid, p, 256u);
}

/*
 * slm stream <session>
 *
 * Read a prompt from the shell I/O until a blank line, then run it.
 * This is the convenience path for serial users who don't want to
 * paste a large quoted block into the verb invocation.
 */
static int slm_stream(int argc, char *argv[])
{
    if (argc < 3) {
        shell_puts("Usage: slm stream <session>\r\n");
        return -1;
    }
    uint32_t sid;
    if (shell_parse_uint(argv[2], &sid) != 0) {
        shell_printf("slm stream: invalid session '%s'\r\n", argv[2]);
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
        int n = shell_read_line(buf, sizeof(buf));
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

    return run_prompt(sid, accum, 256u);
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
    shell_printf("models=%lu sessions=%lu (M5.2 stub forward — "
                 "decoder emits zero-logit tokens)\r\n",
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
    return 0;
}

static int slm_gpu(void)
{
    /* M6.A-3 (per-op tier dispatch) is deferred per the integration
     * plan. When wired, this verb walks rust_slm_gpu_* to print the
     * per-op tier table. For now it's a structured "not yet" line so
     * scripted demos can detect the state. */
    shell_puts("GPU dispatch not yet wired (M6.A-3 deferred — "
               "see docs/plans/slm-integration-plan.md)\r\n");
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
               "(--ctx --sampler --temp --topk --topp --seed)\r\n");
    shell_puts("  prompt <session> <text...>     Run a prompt\r\n");
    shell_puts("  stream <session>               Read a prompt from UART\r\n");
    shell_puts("  stop   <session>               Cancel an in-flight decode\r\n");
    shell_puts("  reset  <session>               Reset KV cache\r\n");
    shell_puts("  close  <session>               Close a session\r\n");
    shell_puts("  status                         One-line summary\r\n");
    shell_puts("  stats  <session>               Per-session telemetry\r\n");
    shell_puts("  gpu                            GPU dispatch state\r\n");
}

int cmd_slm(int argc, char *argv[])
{
    if (argc < 2) {
        slm_print_usage();
        return -1;
    }
    const char *verb = argv[1];

    if (strcmp(verb, "load")   == 0) return slm_load(argc, argv);
    if (strcmp(verb, "xload")  == 0) return slm_xload(argc, argv);
    if (strcmp(verb, "list")   == 0) return slm_list();
    if (strcmp(verb, "info")   == 0) return slm_info(argc, argv);
    if (strcmp(verb, "unload") == 0) return slm_unload(argc, argv);
    if (strcmp(verb, "launch") == 0) return slm_launch(argc, argv);
    if (strcmp(verb, "prompt") == 0) return slm_prompt(argc, argv);
    if (strcmp(verb, "stream") == 0) return slm_stream(argc, argv);
    if (strcmp(verb, "stop")   == 0) return slm_stop(argc, argv);
    if (strcmp(verb, "reset")  == 0) return slm_reset(argc, argv);
    if (strcmp(verb, "close")  == 0) return slm_close(argc, argv);
    if (strcmp(verb, "status") == 0) return slm_status();
    if (strcmp(verb, "stats")  == 0) return slm_stats(argc, argv);
    if (strcmp(verb, "gpu")    == 0) return slm_gpu();
    if (strcmp(verb, "help")   == 0) { slm_print_usage(); return 0; }

    shell_printf("slm: unknown verb '%s'\r\n", verb);
    slm_print_usage();
    return -1;
}
