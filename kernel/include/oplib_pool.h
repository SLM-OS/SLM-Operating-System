/*
 * oplib_pool.h - In-kernel handle for the embedded operator-library blob (#714).
 *
 * The kernel image embeds an `operator_library.bin` via .incbin
 * (kernel/src/oplib_embed.S). At boot, `oplib_pool_init()` parses the
 * blob and caches the handle so dispatch paths can look up SASS
 * kernels by (op_kind, tier, dtype) without re-validating the
 * outer/inner headers on every call.
 *
 * The lookup API mirrors `operator_library_lookup` exactly: returns
 * a CPU pointer + size into the embedded blob's SASS region. The GPU-
 * VA staging path (allocate PMM, copy SASS region, GMMU-map, return
 * GPU VAs) is the next layer's responsibility — it lands as a small
 * follow-on PR once #666 GMMU work merges to main. Until then, the
 * dispatcher takes the CPU pointer + a separately-staged GPU buffer
 * and the launch helper handles the upload.
 */

#ifndef OPLIB_POOL_H
#define OPLIB_POOL_H

#include <stddef.h>
#include <stdint.h>

/* Initialize the operator-library handle from the embedded blob.
 *
 * Called once during kernel boot, after early UART is up. If the
 * embedded blob is the stub variant produced by
 * `scripts/build-operator-library-stub.py`, parsing succeeds with
 * `op_count = 0` and every subsequent lookup returns NOT_FOUND. The
 * function is idempotent — calling it twice has the same observable
 * effect as calling it once.
 *
 * Returns 0 on success, the negative OPERATOR_LIBRARY_ERR_* code from
 * `operator_library_open` on failure (NULL blob pointer, magic /
 * version / checksum mismatch, malformed entries). On failure,
 * subsequent `oplib_pool_lookup` calls return ERR_NULL until the
 * handle is reinitialized.
 */
int oplib_pool_init(void);

/* Look up a SASS kernel by (op_kind, tier, dtype).
 *
 * On success returns 0 and *out_sass / *out_size point to the kernel
 * bytes inside the embedded blob's SASS region. On miss returns
 * OPERATOR_LIBRARY_ERR_NOT_FOUND. If `oplib_pool_init` hasn't been
 * called or failed, returns OPERATOR_LIBRARY_ERR_NULL. */
int oplib_pool_lookup(uint32_t op_kind, uint32_t tier, uint32_t dtype,
                      const uint8_t **out_sass, size_t *out_size);

/* Diagnostic dump for the `nvgpu oplib status` shell verb.
 *
 * Prints (via uart_printf): blob start/end addresses, blob size,
 * declared op_count, total SASS region size, and one line per
 * registered (op_kind, tier, dtype) entry with its sass_offset and
 * sass_size. No-op if `oplib_pool_init` hasn't been called or failed
 * — prints an "uninitialized / parse failed" line instead. */
void oplib_pool_status_print(void);

/* Total size of the embedded blob in bytes. Useful for tests and the
 * status verb; intentionally cheap (just a pointer subtraction over
 * the linker-script symbols). */
size_t oplib_pool_blob_size(void);

#endif /* OPLIB_POOL_H */
