/*
 * help.h - File-Driven Help System for SLM-OS
 *
 * Provides a centralized, file-based help system. Help text is written
 * to the filesystem at boot and read on demand, minimizing RAM usage.
 */

#ifndef HELP_H
#define HELP_H

/*
 * Initialize the help system.
 * Creates /help/ directory and writes help files for all commands.
 * Must be called after LittleFS is mounted.
 *
 * Returns: 0 on success, -1 on error
 */
int help_init(void);

/*
 * Display help for a specific command.
 * Reads from /help/<command>.txt and prints to UART.
 *
 * @param command: Command name (e.g., "cp", "grep")
 * Returns: 0 if help displayed, -1 if not found
 */
int help_show(const char *command);

/*
 * Check if help exists for a command.
 *
 * @param command: Command name
 * Returns: 1 if help exists, 0 if not
 */
int help_exists(const char *command);

#endif /* HELP_H */
