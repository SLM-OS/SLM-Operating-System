/*
 * tegra_cpufreq.h - Tegra234 (Jetson Orin) CPU frequency (NDIV) control.
 *
 * Each A78AE core has a 32-bit NDIV register (CPU PLL multiplier) in the
 * CCPLEX cluster block:
 *
 *     TEGRA234_CCPLEX_BASE + 0x2000 + 8 * (cluster * 4 + MPIDR.Aff1)
 *
 * and runs at 12.8 MHz * NDIV (NDIV 9 = 115.2 MHz floor ... 135 =
 * 1728 MHz ceiling on the Orin Nano). Layout from Linux's
 * drivers/cpufreq/tegra194-cpufreq.c (tegra234_set_cpu_ndiv); cached at
 * ~/slmos-ref/linux/linux-tegra194-cpufreq.c. A cluster runs at the
 * highest NDIV requested by any of its cores.
 *
 * Why SLM-OS touches this (#1053): Linux's kexec offlines every CPU but
 * the boot CPU, and the per-core NDIV of an offlined core drops to the
 * floor. Cluster 0 keeps running at CPU 0's NDIV, but cluster 1
 * (CPUs 4-5, MPIDR 0x10200/0x10300) has no core left above the floor
 * and runs ~12x slower than CPUs 0-3 after kexec.
 *
 * Only the 32-bit word at each id * 8 offset is a real register. The
 * words in between (e.g. +0x2004) are not backed: reading one raises an
 * SError that BL31 reports as "Unhandled Exception in EL3" and the
 * board stops. Never touch anything but tegra234_cpufreq_reg_offset().
 */
#ifndef TEGRA_CPUFREQ_H
#define TEGRA_CPUFREQ_H

#include <stdint.h>
#include <stdbool.h>

#define TEGRA234_CPUFREQ_REG_BASE       0x2000u
#define TEGRA234_CPUFREQ_REG_STRIDE     8u
#define TEGRA234_CPUFREQ_MAX_PER_CLUSTER 4u
#define TEGRA234_CPUFREQ_NUM_CLUSTERS   3u
#define TEGRA234_NDIV_MASK              0x1FFu
#define TEGRA234_KHZ_PER_NDIV           12800u
#define TEGRA234_NDIV_MIN               9u      /* 115.2 MHz */
#define TEGRA234_NDIV_MAX               135u    /* 1728 MHz  */

/*
 * Register id (cluster * 4 + Aff1) for a core's MPIDR, or -1 when the
 * MPIDR is outside the Tegra234 layout (3 clusters x 4 cores).
 */
int tegra234_cpufreq_reg_id(uint64_t mpidr);

/* Byte offset from TEGRA234_CCPLEX_BASE of the NDIV register for an id. */
uint32_t tegra234_cpufreq_reg_offset(uint32_t id);

/* Frequency in kHz for an NDIV value (mask applied). */
uint32_t tegra234_ndiv_to_khz(uint32_t ndiv);

/*
 * NDIV to give every core, given the boot CPU's NDIV, or 0 to leave the
 * cores alone. The boot CPU's value is its frequency at kexec time
 * (slmos-kexec pins it to the cpufreq policy max first), so it is
 * within Linux's limits; anything
 * outside [TEGRA234_NDIV_MIN, TEGRA234_NDIV_MAX] is treated as an
 * unreadable/unexpected register and ignored.
 */
uint32_t tegra234_cpufreq_sync_target(uint32_t boot_ndiv);

#if defined(PLATFORM_JETSON_ORIN_NANO)
/* Current NDIV of logical CPU `cpu`, or 0 if it has no valid register. */
uint32_t tegra234_cpufreq_get_ndiv(uint32_t cpu);

/*
 * Raise every present core whose NDIV is below the boot CPU's to the
 * boot CPU's NDIV (#1053). Call after the secondaries are online.
 * Returns the number of cores whose NDIV was raised.
 */
uint32_t tegra234_cpufreq_sync_to_boot_cpu(void);
#endif

#endif /* TEGRA_CPUFREQ_H */
