/*
 * xusb_t234_cold.h - Tegra234 XUSB cold bring-up after UEFI-direct boot (#1084)
 *
 * See xusb_t234_cold.c for the sequence and its references.
 */

#ifndef XUSB_T234_COLD_H
#define XUSB_T234_COLD_H

#include <stdint.h>

/* Steps: 1 clocks + padctl reset, 2 power partitions, 3 pads,
 * 4 FPCI config, 5 wait for USBSTS.CNR to clear. */
#define XUSB_T234_COLD_STEPS 5

/*
 * Run the steps that have not completed yet. Returns 0 when the
 * controller reports ready (CNR clear, HCE clear), negative otherwise.
 * Safe to call again after a partial run: completed steps are not
 * repeated.
 */
int xusb_t234_cold_bringup(void);

#endif /* XUSB_T234_COLD_H */
