/*
 * fb_console.h - Framebuffer console driver interface
 *
 * This provides text output via UEFI GOP framebuffer for x86-64 platform.
 */

#ifndef FB_CONSOLE_H
#define FB_CONSOLE_H

#include <stdint.h>

/*
 * Initialize framebuffer console from Multiboot2 info structure
 */
void fb_console_init(void *multiboot_info);

/*
 * Output a single character
 */
void fb_console_putc(char c);

/*
 * Output a string
 */
void fb_console_puts(const char *s);

/*
 * Get framebuffer info
 */
void fb_console_get_info(uint32_t *width, uint32_t *height, uint32_t *cols, uint32_t *rows);

#endif /* FB_CONSOLE_H */
