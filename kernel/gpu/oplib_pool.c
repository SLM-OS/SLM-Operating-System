/*
 * oplib_pool.c - In-kernel handle for the embedded operator-library blob (#714).
 *
 * Caches a parsed `operator_library` handle over the .incbin'ed blob
 * so dispatchers (B follow-on; #714) can look up SASS kernels by
 * (op_kind, tier, dtype) without re-validating headers on every
 * call.
 *
 * GPU-VA staging is intentionally out of scope here — that's the
 * dispatcher's job and lands once #666 GMMU work merges to main.
 * This file is just the parser-handle owner.
 */

#include "oplib_pool.h"

#include "operator_library.h"
#include "uart.h"

/* Linker-supplied symbols from kernel/src/oplib_embed.S. */
extern const uint8_t oplib_blob_start[];
extern const uint8_t oplib_blob_end[];

/* Cached parser handle. Zero-initialized = uninitialized; `g_init_rc`
 * captures the open() result so `oplib_pool_lookup` can return the
 * right error class without re-running the parser. */
static struct operator_library g_handle;
static int g_init_rc = OPERATOR_LIBRARY_ERR_NULL;
static bool g_initialized;

size_t oplib_pool_blob_size(void)
{
    return (size_t)(oplib_blob_end - oplib_blob_start);
}

int oplib_pool_init(void)
{
    if (g_initialized) {
        return g_init_rc;
    }

    size_t len = oplib_pool_blob_size();
    g_init_rc = operator_library_open(&g_handle, oplib_blob_start, len);
    g_initialized = true;

    if (g_init_rc == 0) {
        uart_printf("[oplib] embedded blob: %zu B, op_count=%u\n",
                    len, (unsigned)g_handle.op_count);
    } else {
        uart_printf("[oplib] embedded blob parse FAILED: rc=%d "
                    "(blob_size=%zu)\n", g_init_rc, len);
    }
    return g_init_rc;
}

int oplib_pool_lookup(uint32_t op_kind, uint32_t tier, uint32_t dtype,
                      const uint8_t **out_sass, size_t *out_size)
{
    if (!g_initialized || g_init_rc != 0) {
        return OPERATOR_LIBRARY_ERR_NULL;
    }
    return operator_library_lookup(&g_handle, op_kind, tier, dtype,
                                   out_sass, out_size);
}

/* `nvgpu oplib status` helper. Output is intentionally compact —
 * one summary line plus one line per entry. */
void oplib_pool_status_print(void)
{
    size_t len = oplib_pool_blob_size();

    if (!g_initialized) {
        uart_printf("[oplib] not initialized (oplib_pool_init not called)\n");
        return;
    }
    if (g_init_rc != 0) {
        uart_printf("[oplib] init failed: rc=%d, blob_size=%zu\n",
                    g_init_rc, len);
        return;
    }

    uart_printf("[oplib] blob: start=%p end=%p size=%zu\n",
                (const void *)oplib_blob_start,
                (const void *)oplib_blob_end, len);
    uart_printf("[oplib] op_count=%u, sass_region_len=%zu\n",
                (unsigned)g_handle.op_count, g_handle.sass_region_len);

    /* Walk the entries array — same 32-byte layout
     * `operator_library.h` documents. */
    for (uint32_t i = 0; i < g_handle.op_count; i++) {
        const uint8_t *e = g_handle.entries + (size_t)i *
                           OPERATOR_LIBRARY_ENTRY_LEN;
        uint32_t op_kind = (uint32_t)e[0]  | ((uint32_t)e[1]  <<  8) |
                           ((uint32_t)e[2]  << 16) | ((uint32_t)e[3]  << 24);
        uint32_t tier    = (uint32_t)e[4]  | ((uint32_t)e[5]  <<  8) |
                           ((uint32_t)e[6]  << 16) | ((uint32_t)e[7]  << 24);
        uint32_t dtype   = (uint32_t)e[8]  | ((uint32_t)e[9]  <<  8) |
                           ((uint32_t)e[10] << 16) | ((uint32_t)e[11] << 24);
        uint64_t off     = (uint64_t)e[16] | ((uint64_t)e[17] <<  8) |
                           ((uint64_t)e[18] << 16) | ((uint64_t)e[19] << 24) |
                           ((uint64_t)e[20] << 32) | ((uint64_t)e[21] << 40) |
                           ((uint64_t)e[22] << 48) | ((uint64_t)e[23] << 56);
        uint64_t sz      = (uint64_t)e[24] | ((uint64_t)e[25] <<  8) |
                           ((uint64_t)e[26] << 16) | ((uint64_t)e[27] << 24) |
                           ((uint64_t)e[28] << 32) | ((uint64_t)e[29] << 40) |
                           ((uint64_t)e[30] << 48) | ((uint64_t)e[31] << 56);
        uart_printf("[oplib]   [%u] op_kind=%u tier=%u dtype=%u "
                    "sass_offset=0x%llx size=%llu\n",
                    (unsigned)i, (unsigned)op_kind, (unsigned)tier,
                    (unsigned)dtype, (unsigned long long)off,
                    (unsigned long long)sz);
    }
}
