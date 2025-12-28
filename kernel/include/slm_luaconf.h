/**
 * slm_luaconf.h - Lua Configuration for SLM-OS Freestanding Environment
 *
 * This header is included before Lua's luaconf.h to override settings
 * for bare-metal operation without libc.
 */

#ifndef SLM_LUACONF_H
#define SLM_LUACONF_H

/* Use C89 mode - avoid non-portable features */
#define LUA_USE_C89 1

/* Disable OS-specific features */
#undef LUA_USE_POSIX
#undef LUA_USE_WINDOWS
#undef LUA_USE_LINUX
#undef LUA_USE_MACOSX
#undef LUA_USE_IOS
#undef LUA_USE_DLOPEN
#undef LUA_DL_DLL

/* Use 32-bit integers and floats (simpler, smaller) */
#define LUA_32BITS 1

/* Disable locales (not available in freestanding) */
#define LUA_USE_LOCALE 0

/* Custom memory allocator - we provide our own */
#define LUA_USER_H "slm_lua_stubs.h"

/* Disable longjmp-based error handling - use our stub */
/* Note: Lua requires setjmp/longjmp for pcall error recovery */

/* I/O redirection to UART */
#define lua_writestring(s, l)   slm_lua_writestring(s, l)
#define lua_writeline()         slm_lua_writeline()
#define lua_writestringerror(s, p) slm_lua_writeerror(s, p)

/* Disable readline (no terminal library) */
#undef LUA_USE_READLINE

/* Stack and memory limits */
#define LUAI_MAXSTACK       15000   /* Maximum Lua stack size */
#define LUA_MINBUFFER       32      /* Minimum buffer for strings */

/* Disable features we don't need */
#define LUA_NOCVTN2S        0       /* Allow number to string conversion */
#define LUA_NOCVTS2N        0       /* Allow string to number conversion */

/* Function declarations for our stubs */
void slm_lua_writestring(const char *s, size_t l);
void slm_lua_writeline(void);
void slm_lua_writeerror(const char *s, const char *p);

#endif /* SLM_LUACONF_H */
