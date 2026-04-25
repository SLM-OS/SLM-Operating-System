/*
 * camera_mock_embed.S — Embed the QEMU-side mock camera frame.
 *
 * The bytes are produced at build time by
 * scripts/generate-mock-camera-frame.py from
 * scripts/fixtures/mock_camera_digit.bin (a 28x28 fp32 MNIST digit).
 * Layout: 1640x1232 RAW10 RGGB packed, 2,525,600 bytes total.
 *
 * Only compiled when MOCK_CAMERA_FRAME=ON in CMake — production
 * hardware builds can drop the 2.5 MB by setting it OFF.
 * MOCK_CAMERA_FRAME_PATH is fed in via set_source_files_properties
 * COMPILE_DEFINITIONS as an unquoted path token; the macro below
 * stringifies it into a single token .incbin will accept.
 */

#if !defined(MOCK_CAMERA_FRAME_PATH)
# error "MOCK_CAMERA_FRAME_PATH must be supplied by the build system"
#endif

#define xstr(s) str(s)
#define str(s) #s

    .section .rodata
    .global mock_camera_frame_start
    .global mock_camera_frame_end
    .align 4
mock_camera_frame_start:
    .incbin xstr(MOCK_CAMERA_FRAME_PATH)
mock_camera_frame_end:

#if defined(__ELF__) && (defined(__x86_64__) || defined(__i386__))
    .section .note.GNU-stack, "", @progbits
#endif
