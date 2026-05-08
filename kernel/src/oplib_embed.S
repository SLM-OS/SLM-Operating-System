/*
 * oplib_embed.S — Embed the operator library blob via .incbin.
 *
 * The blob at OPLIB_BLOB_PATH (set by CMake) holds the packed
 * (op_kind, tier, dtype) → SASS table that drives GPU dispatch
 * (#663). On Jetson, kernel/gpu/oplib_pool.c parses this at boot,
 * stages the SASS region in PMM-allocated pages, and GMMU-maps
 * it to a fixed GPU VA so the dispatcher can reference any
 * kernel by `(base_gpu_va + sass_offset)`.
 *
 * If no real operator_library.bin is available at build time,
 * CMake supplies a stub produced by
 * `scripts/build-operator-library-stub.py` (24-byte outer
 * header + 8-byte inner header, op_count=0). The kernel-side
 * parser accepts the stub; every lookup just returns ERR_NOT_FOUND
 * so dispatch falls back to CPU.
 *
 * OPLIB_BLOB_PATH is supplied by `set_source_files_properties`
 * COMPILE_DEFINITIONS as an unquoted path token; the macro below
 * stringifies it into a single token .incbin will accept (same
 * pattern as sched_hef_embed.S / camera_mock_embed.S).
 */

#if !defined(OPLIB_BLOB_PATH)
# error "OPLIB_BLOB_PATH must be supplied by the build system"
#endif

#define xstr(s) str(s)
#define str(s) #s

    .section .rodata

    .global oplib_blob_start
    .global oplib_blob_end
    .align 4
oplib_blob_start:
    .incbin xstr(OPLIB_BLOB_PATH)
oplib_blob_end:

#if defined(__ELF__) && (defined(__x86_64__) || defined(__i386__))
    .section .note.GNU-stack, "", @progbits
#endif
