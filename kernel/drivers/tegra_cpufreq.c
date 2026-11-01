/*
 * tegra_cpufreq.c - Tegra234 CPU frequency (NDIV) control (#1053).
 *
 * See tegra_cpufreq.h for the register layout and why SLM-OS needs it.
 * The pure helpers build on every platform so kernel/tests can cover
 * them on QEMU; the MMIO half is Jetson-only.
 */
#include "tegra_cpufreq.h"
#include "platform.h"

int tegra234_cpufreq_reg_id(uint64_t mpidr)
{
    uint32_t aff1 = (uint32_t)((mpidr >> 8) & 0xFF);
    uint32_t aff2 = (uint32_t)((mpidr >> 16) & 0xFF);

    if (aff1 >= TEGRA234_CPUFREQ_MAX_PER_CLUSTER ||
        aff2 >= TEGRA234_CPUFREQ_NUM_CLUSTERS) {
        return -1;
    }
    return (int)(aff2 * TEGRA234_CPUFREQ_MAX_PER_CLUSTER + aff1);
}

uint32_t tegra234_cpufreq_reg_offset(uint32_t id)
{
    return TEGRA234_CPUFREQ_REG_BASE + id * TEGRA234_CPUFREQ_REG_STRIDE;
}

uint32_t tegra234_ndiv_to_khz(uint32_t ndiv)
{
    return (ndiv & TEGRA234_NDIV_MASK) * TEGRA234_KHZ_PER_NDIV;
}

uint32_t tegra234_cpufreq_sync_target(uint32_t boot_ndiv)
{
    uint32_t ndiv = boot_ndiv & TEGRA234_NDIV_MASK;

    if (ndiv < TEGRA234_NDIV_MIN || ndiv > TEGRA234_NDIV_MAX) {
        return 0;
    }
    return ndiv;
}

#if defined(PLATFORM_JETSON_ORIN_NANO)
#include "smp.h"
#include "debug.h"

static volatile uint32_t *ndiv_reg(uint32_t cpu)
{
    if (cpu >= cpu_count || cpu >= MAX_CPUS) {
        return NULL;
    }
    int id = tegra234_cpufreq_reg_id(cpu_logical_map[cpu]);
    if (id < 0) {
        return NULL;
    }
    return (volatile uint32_t *)(uintptr_t)(TEGRA234_CCPLEX_BASE +
                                            tegra234_cpufreq_reg_offset((uint32_t)id));
}

uint32_t tegra234_cpufreq_get_ndiv(uint32_t cpu)
{
    volatile uint32_t *reg = ndiv_reg(cpu);

    if (!reg) {
        return 0;
    }
    return *reg & TEGRA234_NDIV_MASK;
}

uint32_t tegra234_cpufreq_sync_to_boot_cpu(void)
{
    uint32_t target = tegra234_cpufreq_sync_target(tegra234_cpufreq_get_ndiv(0));
    uint32_t raised = 0;

    if (target == 0) {
        WARN("cpufreq: CPU 0 NDIV unreadable or out of range; leaving CPU clocks alone");
        return 0;
    }

    for (uint32_t cpu = 1; cpu < cpu_count; cpu++) {
        volatile uint32_t *reg = ndiv_reg(cpu);
        if (!reg) {
            continue;
        }
        uint32_t cur = *reg & TEGRA234_NDIV_MASK;
        if (cur >= target) {
            continue;
        }
        *reg = target;
        __asm__ volatile("dsb sy" ::: "memory");
        INFO("cpufreq: CPU %u NDIV %u (%u MHz) -> %u (%u MHz), matching CPU 0",
             cpu, cur, tegra234_ndiv_to_khz(cur) / 1000,
             target, tegra234_ndiv_to_khz(target) / 1000);
        raised++;
    }
    return raised;
}
#endif /* PLATFORM_JETSON_ORIN_NANO */
