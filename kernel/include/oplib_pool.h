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

/* Layout of the helper-staged SASS pool. The Linux-side helper
 * allocates a 1 MB region and maps it into the GA10B channel's
 * GMMU; the pool is divided into four 64 KB slots. Slot 0 holds
 * the staged SASS bytes (consumed by `oplib_pool_stage_to_gpu`'s
 * helper-staged fast path); slots 1-3 are scratch I/O buffers
 * carved by the per-op smoke verbs in shell_sys.c (input/gamma/
 * output for rmsnorm; vec/positions/cos_sin for rope). The
 * `OPLIB_POOL_MIN_BYTES` threshold guards `h->shader_size` to
 * make sure the helper actually allocated all four slots. */
#define OPLIB_POOL_SLOT_BYTES   0x10000u           /* 64 KB per slot */
#define OPLIB_POOL_MIN_BYTES    (4u * OPLIB_POOL_SLOT_BYTES)
#define OPLIB_POOL_OFF_SASS     0x00000ull          /* slot 0 */
#define OPLIB_POOL_OFF_SCRATCH0 0x10000ull          /* slot 1 */
#define OPLIB_POOL_OFF_SCRATCH1 0x20000ull          /* slot 2 */
#define OPLIB_POOL_OFF_SCRATCH2 0x30000ull          /* slot 3 */

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

/* ============================================================================
 * GPU-VA staging (#718, A.1.5)
 * ============================================================================
 *
 * The .incbin blob lives in kernel .rodata, which is CPU-readable but not
 * GPU-readable on Jetson — the GA10B GMMU has its own page tables (the
 * inherited Linux ones, post-kexec) and only sees the VAs Linux/SLM-OS
 * maps for it. `oplib_pool_stage_to_gpu()` copies the SASS region into
 * fresh PMM pages and GMMU-maps them at a dedicated GPU VA range so the
 * dispatcher (B follow-on, #714) can reference any kernel by
 * `(g_sass_pool_gpu_va + entry.sass_offset)`.
 *
 * Staging is Jetson-only (calls into ga10b_gmmu_alloc, which only exists
 * on PLATFORM_JETSON_ORIN_NANO). On other platforms the function is a
 * stub that returns -1 and `slm_oplib_get_sass_gpu_va` always returns
 * NOT_AVAILABLE.
 */

/* Stage the SASS region into GPU VA. Allocates PMM pages, copies the
 * region's bytes, GMMU-maps via `ga10b_gmmu_alloc`, caches the resulting
 * base GPU VA.
 *
 * `inst_block_phys` identifies the GPU channel whose page tables receive
 * the new mapping. Caller obtains it from a real handoff
 * (`ga10b_bringup_handoff()->inst_block_phys`) or via FECS_CURRENT_CTX
 * read-back (`ga10b_gmmu_discover_inst_block_phys()`).
 *
 * Idempotent: a second call with the same (or any) inst_block_phys
 * after a successful first call returns the cached rc without
 * re-allocating. The staging is per-channel today — if a future use
 * case rebinds the channel (different inst block), reset state via a
 * yet-unbuilt `oplib_pool_unstage()` first.
 *
 * Returns:
 *   0 on success (or sass_region_len == 0 stub case — staging is a no-op).
 *   negative on `ga10b_gmmu_alloc` failure or PMM exhaustion.
 *   -1 on Jetson-only-platform stub fallback.
 */
int oplib_pool_stage_to_gpu(uint64_t inst_block_phys);

/* Look up a SASS kernel's GPU VA by (op_kind, tier, dtype).
 *
 * On success returns 0 and *out_gpu_va / *out_size give the GPU virtual
 * address and byte length of the kernel inside the staged SASS pool.
 * Returns OPERATOR_LIBRARY_ERR_NOT_FOUND on lookup miss,
 * OPERATOR_LIBRARY_ERR_NULL on uninitialized handle or unstaged pool.
 */
int oplib_pool_get_sass_gpu_va(uint32_t op_kind, uint32_t tier, uint32_t dtype,
                                uint64_t *out_gpu_va, size_t *out_size);

/* Inspector: GPU VA where the staged SASS region begins, or 0 if not
 * staged. Used by the status verb. */
uint64_t oplib_pool_gpu_va_base(void);

#endif /* OPLIB_POOL_H */
