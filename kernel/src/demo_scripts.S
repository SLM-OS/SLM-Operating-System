/*
 * demo_scripts.S — Embed Lua demo scripts from scripts/ via .incbin.
 *
 * scripts/demo.lua and scripts/demo_menu.lua are the single source of
 * truth; this file embeds them verbatim into the kernel's read-only
 * data section at link time. demo_init.c uses the _start / _end
 * symbols (and computes length via pointer arithmetic) to write each
 * script into the LittleFS RAM disk at boot.
 *
 * Only compiled and linked when EMBED_DEMO_SCRIPTS is ON in CMake
 * (#14). DEMO_LUA_PATH and DEMO_MENU_LUA_PATH are supplied by
 * CMakeLists.txt via set_source_files_properties COMPILE_DEFINITIONS.
 *
 * Resolves issues #13 (embedded script content outside demo_init.c)
 * and #14 (conditional embedding via build flag).
 */

#if !defined(DEMO_LUA_PATH) || !defined(DEMO_MENU_LUA_PATH)
# error "DEMO_LUA_PATH and DEMO_MENU_LUA_PATH must be supplied by the build system"
#endif

#define xstr(s) str(s)
#define str(s) #s

    .section .rodata

    .global demo_lua_start
    .global demo_lua_end
    .align 4
demo_lua_start:
    .incbin xstr(DEMO_LUA_PATH)
demo_lua_end:

    .global demo_menu_lua_start
    .global demo_menu_lua_end
    .align 4
demo_menu_lua_start:
    .incbin xstr(DEMO_MENU_LUA_PATH)
demo_menu_lua_end:

#if defined(__ELF__) && (defined(__x86_64__) || defined(__i386__))
    .section .note.GNU-stack, "", @progbits
#endif
