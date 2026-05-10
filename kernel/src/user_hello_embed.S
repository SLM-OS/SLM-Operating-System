/*
 * user_hello_embed.S — embed the EL0 hello ELF via .incbin.
 *
 * The user binary at USER_HELLO_ELF_PATH is built by
 * `add_custom_command` in CMakeLists.txt: aarch64-none-elf-gcc
 * compiles user/hello/{start.S,main.c} against user/user.ld and
 * objcopy --strip-all produces the stripped ELF embedded here.
 *
 * USER_HELLO_ELF_PATH is supplied by `set_source_files_properties`
 * COMPILE_DEFINITIONS as an unquoted token; the xstr/str dance
 * stringifies it for the assembler's .incbin (same pattern as
 * oplib_embed.S).
 */

#if !defined(USER_HELLO_ELF_PATH)
# error "USER_HELLO_ELF_PATH must be supplied by the build system"
#endif

#define xstr(s) str(s)
#define str(s) #s

    .section .rodata
    .global user_hello_elf_start
    .global user_hello_elf_end
    .align 4
user_hello_elf_start:
    .incbin xstr(USER_HELLO_ELF_PATH)
user_hello_elf_end:

#if defined(__ELF__) && (defined(__x86_64__) || defined(__i386__))
    .section .note.GNU-stack, "", @progbits
#endif
