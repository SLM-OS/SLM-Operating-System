/*
 * hailo_fw.S — Embed the Hailo-8 boot firmware via .incbin.
 *
 * Only compiled when the CMake option HAILO_FW_BLOB is set to a path;
 * the path is forwarded to this file as HAILO_FW_BLOB_PATH via
 * COMPILE_DEFINITIONS (GAS .incbin takes a string literal, so the
 * full path has to be a preprocessor define rather than a variable).
 *
 * The kernel references hailo_fw_start / hailo_fw_end as weak
 * externs in hailo_shell.c — default builds link cleanly without a
 * strong definition, and `hailo boot` reports "firmware not
 * embedded". Setting HAILO_FW_BLOB=path/to/hailo8_fw.bin flips those
 * symbols strong and the blob is available at runtime.
 */

#if !defined(HAILO_FW_BLOB_PATH)
# error "HAILO_FW_BLOB_PATH must be supplied by the build system"
#endif

#define xstr(s) str(s)
#define str(s) #s

    .section .rodata

    .global hailo_fw_start
    .global hailo_fw_end
    /* 64 B = one ARM64 cache line. Keeps the blob's start at a
     * cache-line boundary so any future code path that DMAs directly
     * from the embedded image (instead of copying first) doesn't
     * straddle a line. Today's path goes through BAR4 MMIO writes
     * where alignment is only required at 4 B.
     *
     * `.balign` (literal-byte align) instead of `.align` because GAS
     * resolves `.align N` differently per arch: ARM64 takes N as the
     * power-of-2 exponent (so `.align 6` = 64 B), x86 takes N as a
     * literal byte count and rejects non-power-of-2 values (so
     * `.align 6` = "alignment not a power of 2"). `.balign` is always
     * literal-byte regardless of arch. */
    .balign 64
hailo_fw_start:
    .incbin xstr(HAILO_FW_BLOB_PATH)
hailo_fw_end:

#if defined(__ELF__) && (defined(__x86_64__) || defined(__i386__))
    .section .note.GNU-stack, "", @progbits
#endif
