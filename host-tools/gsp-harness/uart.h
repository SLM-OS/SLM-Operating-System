/*
 * uart.h — host-harness compat shim.
 *
 * The shared GSP-RM core (kernel/gpu/nvidia/gsp.c) prints diagnostics
 * via uart_puts() / uart_printf(). In SLM-OS these route to the 16550
 * serial port; in the host-side harness they route to stderr so the
 * normal Linux tooling (pipes, redirection, grep) can consume them.
 *
 * This header only applies when building the host harness — guarded
 * by SLM_HOST_HARNESS=1 set in the Makefile's CFLAGS. The bare-metal
 * build uses kernel/include/uart.h instead.
 */

#ifndef HOST_HARNESS_UART_H
#define HOST_HARNESS_UART_H

#include <stdio.h>

#define uart_puts(s)           fputs((s), stderr)
#define uart_printf(fmt, ...)  fprintf(stderr, (fmt), ##__VA_ARGS__)

#endif
