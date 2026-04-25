/*
 * mnist_digit_embed.S — Embed MNIST test digit fixtures via .incbin.
 *
 * Each fixture is a 1×1×28×28 fp32 image (3,136 bytes) extracted from
 * the MNIST test set by scripts/mnist-extract-test-digits.py. The
 * bytes land in .rodata so mnist_digit_init.c can write them into
 * /mnt/files/digits/ at boot, where slm.model_infer_file() can read
 * them back.
 *
 * Only compiled and linked when ENABLE_MNIST_DIGITS_EMBED=ON in CMake.
 * MNIST_DIGITS_DIR is supplied via set_source_files_properties
 * COMPILE_DEFINITIONS as an unquoted path token; the macros below
 * use C preprocessor token concatenation + stringification to build
 * the per-digit path and feed it to .incbin as a single quoted token.
 */

#if !defined(MNIST_DIGITS_DIR)
# error "MNIST_DIGITS_DIR must be supplied by the build system"
#endif

#define xstr(s) str(s)
#define str(s) #s

/* xstr(MNIST_DIGITS_DIR/digit_<class>_idx<idx>.bin) preprocesses
 * via macro substitution to "<MNIST_DIGITS_DIR>/digit_<class>_idx<idx>.bin"
 * — a single double-quoted string token .incbin will accept. */
#define EMBED_DIGIT(class, idx)                                               \
    .global mnist_digit_##class##_start;                                      \
    .global mnist_digit_##class##_end;                                        \
    .align 4;                                                                 \
mnist_digit_##class##_start:                                                  \
    .incbin xstr(MNIST_DIGITS_DIR/digit_##class##_idx##idx##.bin);            \
mnist_digit_##class##_end:

    .section .rodata

    EMBED_DIGIT(0, 3)
    EMBED_DIGIT(1, 2)
    EMBED_DIGIT(2, 1)
    EMBED_DIGIT(3, 18)
    EMBED_DIGIT(4, 4)
    EMBED_DIGIT(5, 8)
    EMBED_DIGIT(6, 11)
    EMBED_DIGIT(7, 0)
    EMBED_DIGIT(8, 61)
    EMBED_DIGIT(9, 7)

#if defined(__ELF__) && (defined(__x86_64__) || defined(__i386__))
    .section .note.GNU-stack, "", @progbits
#endif
