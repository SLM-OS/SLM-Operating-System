/*
 * smp.c - Symmetric Multi-Processing for SLM-OS
 *
 * Handles secondary core boot via PSCI and per-CPU data management.
 */

#include "smp.h"
#include "spinlock.h"
#include "platform.h"
#include "debug.h"
#include "gic.h"
#include "timer.h"
#include "sched.h"
#include "dtb.h"
#include "cache.h"

#include <stddef.h>

/* Per-CPU data array */
struct per_cpu cpu_data[MAX_CPUS];

/* Maps logical CPU ID -> MPIDR value */
uint64_t cpu_logical_map[MAX_CPUS];

/* Number of CPUs in the system */
uint32_t cpu_count = 0;

/* Number of CPUs currently online */
volatile uint32_t cpus_online = 0;

/* Per-CPU boot handshake flags — cacheline-aligned to avoid corrupting
 * adjacent data during DC CIVAC cache maintenance operations. */
static volatile uint32_t cpu_boot_flag[MAX_CPUS] __attribute__((aligned(64)));

/* Per-CPU boot stacks (16 KB each, 16-byte aligned) */
/* NOT static - needs to be visible to smp_boot.S */
_Alignas(16) uint8_t cpu_stacks[MAX_CPUS][STACK_SIZE];

/*
 * Invoke PSCI function.
 * Pi 5 uses SMC (Secure Monitor Call) — TF-A handles PSCI at EL3.
 * QEMU virt uses HVC (Hypervisor Call) — QEMU's PSCI handler at EL2.
 */
static int64_t psci_call(uint64_t fn, uint64_t arg1,
                         uint64_t arg2, uint64_t arg3)
{
    register uint64_t x0 __asm__("x0") = fn;
    register uint64_t x1 __asm__("x1") = arg1;
    register uint64_t x2 __asm__("x2") = arg2;
    register uint64_t x3 __asm__("x3") = arg3;

#if defined(PLATFORM_QEMU_VIRT)
    /* QEMU: PSCI handler at EL2, use HVC */
    __asm__ volatile("hvc #0"
        : "+r"(x0)
        : "r"(x1), "r"(x2), "r"(x3)
        : "memory");
#else
    /* Pi 5 and Jetson: PSCI handler at EL3 (TF-A), use SMC */
    __asm__ volatile("smc #0"
        : "+r"(x0)
        : "r"(x1), "r"(x2), "r"(x3)
        : "memory");
#endif

    return (int64_t)x0;
}

/*
 * Get logical CPU ID from MPIDR value.
 */
int cpu_logical_id(uint64_t mpidr)
{
    uint64_t aff = mpidr & MPIDR_AFF_MASK;

    for (uint32_t i = 0; i < cpu_count; i++) {
        if (cpu_logical_map[i] == aff) {
            return (int)i;
        }
    }

    return -1;  /* Unknown CPU */
}

/*
 * Power on a secondary CPU via PSCI.
 */
int psci_cpu_on(uint64_t target_mpidr, uintptr_t entry_point,
                uintptr_t context_id)
{
    return (int)psci_call(PSCI_CPU_ON_64, target_mpidr,
                          entry_point, context_id);
}

/*
 * Power off the calling CPU via PSCI.
 */
void psci_cpu_off(void)
{
    psci_call(PSCI_CPU_OFF, 0, 0, 0);

    /* Should not return, but loop if it does */
    while (1) {
        __asm__ volatile("wfi");
    }
}

/*
 * Reset the system via PSCI.
 */
void psci_system_reset(void)
{
    psci_call(PSCI_SYSTEM_RESET, 0, 0, 0);

    /* Should not return */
    while (1) {
        __asm__ volatile("wfi");
    }
}

/*
 * Power off the system via PSCI.
 */
void psci_system_off(void)
{
    psci_call(PSCI_SYSTEM_OFF, 0, 0, 0);

    /* Should not return */
    while (1) {
        __asm__ volatile("wfi");
    }
}

/*
 * Get stack top for a given CPU.
 */
static void *cpu_stack_top(uint32_t cpu)
{
    return &cpu_stacks[cpu][STACK_SIZE];
}

/*
 * Initialize CPU logical map.
 * Uses DTB if available, otherwise falls back to platform defaults.
 * Assumes contiguous MPIDR values (valid for QEMU virt and most ARM64 systems).
 */
static void init_cpu_map(void)
{
    /* CPU 0 is always the boot CPU - get its real MPIDR */
    cpu_logical_map[0] = cpu_get_mpidr() & MPIDR_AFF_MASK;
    cpu_count = 1;

    /*
     * Determine number of CPUs:
     * 1. Use DTB cpu_count if available and valid
     * 2. Otherwise fall back to platform's CPU_MAX
     */
    uint32_t expected_cpus = CPU_MAX;  /* Platform default */

    const fdt_info_t *fdt = dtb_get_info();
    if (fdt && fdt->valid && fdt->cpu_count > 0) {
        expected_cpus = fdt->cpu_count;
        DEBUG_PRINT("CPU count from DTB: %u", expected_cpus);
    } else {
        DEBUG_PRINT("CPU count from platform.h: %u", expected_cpus);
    }

    /* Clamp to MAX_CPUS (kernel limit) */
    if (expected_cpus > MAX_CPUS) {
        WARN("DTB reports %u CPUs, limiting to MAX_CPUS=%u", expected_cpus, MAX_CPUS);
        expected_cpus = MAX_CPUS;
    }

    /* Populate logical map with MPIDR affinity values.
     * Pi 5 BCM2712: CPU ID in Aff1 (0x000, 0x100, 0x200, 0x300)
     * QEMU virt:    CPU ID in Aff0 (0, 1, 2, 3) */
    for (uint32_t i = 1; i < expected_cpus; i++) {
#if defined(PLATFORM_RASPI5)
        cpu_logical_map[i] = (uint64_t)i << 8;   /* Aff1 encoding */
#else
        cpu_logical_map[i] = i;                    /* Aff0 encoding */
#endif
        cpu_count++;
    }

    INFO("CPU map: %u CPUs configured", cpu_count);
}

/*
 * Initialize per-CPU data for a given CPU.
 */
static void init_cpu_data(uint32_t cpu)
{
    struct per_cpu *p = &cpu_data[cpu];

    p->cpu_id = cpu;
    p->mpidr = cpu_logical_map[cpu];
    p->online = false;
    p->stack_top = cpu_stack_top(cpu);
    p->boot_time_ns = 0;
}

/*
 * Translate PSCI error code to string for debugging.
 */
static const char *psci_error_str(int err)
{
    switch (err) {
    case PSCI_SUCCESS:          return "SUCCESS";
    case PSCI_NOT_SUPPORTED:    return "NOT_SUPPORTED";
    case PSCI_INVALID_PARAMS:   return "INVALID_PARAMS";
    case PSCI_DENIED:           return "DENIED";
    case PSCI_ALREADY_ON:       return "ALREADY_ON";
    case PSCI_ON_PENDING:       return "ON_PENDING";
    case PSCI_INTERNAL_FAILURE: return "INTERNAL_FAILURE";
    case PSCI_NOT_PRESENT:      return "NOT_PRESENT";
    case PSCI_DISABLED:         return "DISABLED";
    case PSCI_INVALID_ADDRESS:  return "INVALID_ADDRESS";
    default:                    return "UNKNOWN";
    }
}

/*
 * Secondary CPU initialization (called from smp_boot.S).
 * This runs on each secondary CPU after it wakes up.
 */
void secondary_init(uint32_t logical_cpu_id)
{
    DEBUG_PRINT("CPU %u: secondary_init starting", logical_cpu_id);

    /* Verify SMPEN was set from EL2 on this secondary core */
    if (cpu_has_smpen()) {
        DEBUG_PRINT("CPU %u: SMPEN set", logical_cpu_id);
    } else {
        WARN("CPU %u: SMPEN NOT set", logical_cpu_id);
    }

    /* Initialize per-CPU GIC interface */
    DEBUG_PRINT("CPU %u: GIC percpu init...", logical_cpu_id);
    gic_percpu_init();
    DEBUG_PRINT("CPU %u: GIC percpu done", logical_cpu_id);

    /* Initialize per-CPU timer */
    DEBUG_PRINT("CPU %u: timer percpu init...", logical_cpu_id);
    timer_percpu_init();
    DEBUG_PRINT("CPU %u: timer percpu done", logical_cpu_id);

    /* Signal the primary CPU that we're online.
     * Explicit cache clean (DC CVAC) pushes the write from L1 to the
     * Point of Coherency (PoC / main memory). This is needed because
     * CPUECTLR_EL1.SMPEN may not be set by TF-A for secondary cores,
     * meaning regular cache coherency doesn't propagate L1 writes. */
    cpu_data[logical_cpu_id].online = true;
    cpu_boot_flag[logical_cpu_id] = 1;
    /* Clean cachelines to PoC so primary CPU can see them */
    cache_clean(&cpu_boot_flag[logical_cpu_id]);
    cache_clean(&cpu_data[logical_cpu_id].online);
    cpus_online++;

    INFO("CPU %u: online", logical_cpu_id);

    /*
     * Wait for CPU 0 to initialize the scheduler.
     * This is necessary because smp_init() runs before scheduler_init().
     */
    while (!scheduler_is_initialized()) {
        __asm__ volatile("wfe" ::: "memory");
    }

    /* Initialize scheduler for this CPU (creates idle task) */
    scheduler_init_secondary(logical_cpu_id);

    /* Start the per-CPU timer */
    timer_start();

    /* Enable interrupts */
    __asm__ volatile("msr daifclr, #0x2");  /* Clear IRQ mask */

    /* Start scheduler - this does not return */
    scheduler_start();

    /* Should never reach here */
    panic("CPU %u: scheduler_start returned!", logical_cpu_id);
}

/*
 * Bring up a single secondary CPU.
 */
static int boot_secondary(uint32_t cpu)
{
    uint64_t mpidr = cpu_logical_map[cpu];
    uintptr_t entry = (uintptr_t)secondary_entry;
    int ret;

    /* Clear boot flag before starting the CPU */
    cpu_boot_flag[cpu] = 0;
    __asm__ volatile("dsb sy" ::: "memory");

    DEBUG_PRINT("CPU %u: booting (MPIDR=0x%lx, entry=0x%lx)",
                cpu, mpidr, entry);

    /*
     * Call PSCI CPU_ON.
     * - target_mpidr: the CPU to wake
     * - entry_point: address of secondary_entry in smp_boot.S
     * - context_id: pass the logical CPU ID for easy lookup
     */
    ret = psci_cpu_on(mpidr, entry, cpu);

    if (ret != PSCI_SUCCESS) {
        WARN("CPU %u: PSCI CPU_ON failed: %s (%d)",
             cpu, psci_error_str(ret), ret);
        return ret;
    }

    /*
     * Wait for the CPU to come online.
     * Timeout after ~100ms to avoid hanging if something goes wrong.
     */
    /*
     * Wait for secondary CPU to signal via boot flag.
     * The flag is a separate volatile uint32_t for reliable cross-core visibility.
     */
    for (volatile int timeout = 0; timeout < 5000; timeout++) {
        /* Invalidate our cached copy to force re-read from PoC */
        cache_invalidate(&cpu_boot_flag[cpu]);
        if (cpu_boot_flag[cpu]) {
            return PSCI_SUCCESS;
        }
        /* Brief delay between checks (~1ms) */
        for (volatile int d = 0; d < 100000; d++);
    }

    WARN("CPU %u: boot timeout (flag=%u, online=%d)", cpu,
         cpu_boot_flag[cpu], (int)cpu_data[cpu].online);
    return PSCI_INTERNAL_FAILURE;
}

/*
 * Run spinlock validation tests.
 * Tests basic spinlock and ticket lock functionality on a single core.
 * Returns 0 on success, non-zero on failure.
 */
static int spinlock_run_tests(void)
{
    int errors = 0;
    spinlock_t test_lock = SPINLOCK_INIT;
    ticket_lock_t test_ticket = TICKET_LOCK_INIT;

    uart_puts("\nSpinlock Validation Tests:\n");

    /* Test 1: Spinlock starts unlocked */
    if (!spin_is_locked(&test_lock)) {
        uart_printf("  [PASS] Spinlock initialized unlocked\n");
    } else {
        uart_printf("  [FAIL] Spinlock should start unlocked\n");
        errors++;
    }

    /* Test 2: spin_lock acquires the lock */
    spin_lock(&test_lock);
    if (spin_is_locked(&test_lock)) {
        uart_printf("  [PASS] spin_lock acquires lock\n");
    } else {
        uart_printf("  [FAIL] spin_lock did not acquire lock\n");
        errors++;
    }

    /* Test 3: spin_trylock fails when lock is held */
    if (spin_trylock(&test_lock) == 0) {
        uart_printf("  [PASS] spin_trylock returns 0 when lock held\n");
    } else {
        uart_printf("  [FAIL] spin_trylock should return 0 when lock held\n");
        errors++;
    }

    /* Test 4: spin_unlock releases the lock */
    spin_unlock(&test_lock);
    if (!spin_is_locked(&test_lock)) {
        uart_printf("  [PASS] spin_unlock releases lock\n");
    } else {
        uart_printf("  [FAIL] spin_unlock did not release lock\n");
        errors++;
    }

    /* Test 5: spin_trylock succeeds when lock is free */
    if (spin_trylock(&test_lock) == 1) {
        uart_printf("  [PASS] spin_trylock returns 1 when lock free\n");
        spin_unlock(&test_lock);  /* Clean up */
    } else {
        uart_printf("  [FAIL] spin_trylock should return 1 when lock free\n");
        errors++;
    }

    /* Test 6: Ticket lock basic acquire/release */
    ticket_lock(&test_ticket);
    ticket_unlock(&test_ticket);
    uart_printf("  [PASS] Ticket lock acquire/release works\n");

    /* Test 7: IRQ save/restore */
    {
        irq_flags_t flags = irq_save();
        irq_restore(flags);
        uart_printf("  [PASS] IRQ save/restore works\n");
    }

    /* Test 8: spin_lock_irqsave/spin_unlock_irqrestore */
    {
        irq_flags_t flags = spin_lock_irqsave(&test_lock);
        if (spin_is_locked(&test_lock)) {
            spin_unlock_irqrestore(&test_lock, flags);
            if (!spin_is_locked(&test_lock)) {
                uart_printf("  [PASS] IRQ-safe spinlock works\n");
            } else {
                uart_printf("  [FAIL] IRQ-safe unlock failed\n");
                errors++;
            }
        } else {
            uart_printf("  [FAIL] IRQ-safe lock failed\n");
            errors++;
        }
    }

    /* Test 9: Memory barrier compilation (just verify they compile and run) */
    smp_mb();
    smp_rmb();
    smp_wmb();
    barrier();
    uart_printf("  [PASS] Memory barriers execute without fault\n");

    if (errors == 0) {
        INFO("Spinlock tests passed");
    } else {
        ERROR("Spinlock tests failed: %d errors", errors);
    }

    return errors;
}

/*
 * Run SMP validation tests.
 * Returns 0 on success, non-zero on failure.
 */
static int smp_run_tests(void)
{
    int errors = 0;

    uart_puts("\nSMP Validation Tests:\n");

    /* Test 1: Expected number of CPUs online */
    if (cpus_online == cpu_count) {
        uart_printf("  [PASS] All %u CPUs online\n", cpus_online);
    } else {
        uart_printf("  [FAIL] Expected %u CPUs online, got %u\n",
                    cpu_count, cpus_online);
        errors++;
    }

    /* Test 2: Each CPU's online flag set */
    for (uint32_t i = 0; i < cpu_count; i++) {
        cache_invalidate(&cpu_data[i].online);
        if (cpu_data[i].online) {
            uart_printf("  [PASS] CPU %u online flag set\n", i);
        } else {
            uart_printf("  [FAIL] CPU %u not online\n", i);
            errors++;
        }
    }

    /* Test 3: CPU logical map populated correctly */
    {
        bool map_valid = true;
        for (uint32_t i = 0; i < cpu_count; i++) {
            if (cpu_logical_id(cpu_logical_map[i]) != (int)i) {
                map_valid = false;
                uart_printf("  [FAIL] CPU %u logical map lookup failed\n", i);
                errors++;
            }
        }
        if (map_valid) {
            uart_printf("  [PASS] CPU logical map valid\n");
        }
    }

    /* Test 4: Boot CPU (CPU 0) MPIDR matches what we read */
    {
        uint64_t boot_mpidr = cpu_get_mpidr() & MPIDR_AFF_MASK;
        if (cpu_logical_map[0] == boot_mpidr) {
            uart_printf("  [PASS] Boot CPU MPIDR correct (0x%lx)\n", boot_mpidr);
        } else {
            uart_printf("  [FAIL] Boot CPU MPIDR mismatch: map=0x%lx, actual=0x%lx\n",
                        cpu_logical_map[0], boot_mpidr);
            errors++;
        }
    }

    if (errors == 0) {
        INFO("SMP tests passed");
    } else {
        ERROR("SMP tests failed: %d errors", errors);
    }

    return errors;
}

/*
 * Initialize SMP subsystem.
 * Called by primary CPU after basic kernel init.
 */
void smp_init(void)
{
    uint32_t booted = 0;

    INFO("SMP: initializing");

    /* Check if SMPEN was successfully set from EL2 during boot */
    if (cpu_has_smpen()) {
        INFO("SMP: SMPEN set on CPU 0 — hardware cache coherency active");
    } else {
        WARN("SMP: SMPEN NOT set — using DC CVAC/CIVAC workaround");
    }

    DEBUG_PRINT("  About to run spinlock tests...");

    /* Run spinlock tests before booting secondary cores */
    spinlock_run_tests();

    /* Set up CPU logical map */
    init_cpu_map();

    /* Initialize per-CPU data for all CPUs */
    for (uint32_t i = 0; i < cpu_count; i++) {
        init_cpu_data(i);
    }

    /* Mark CPU 0 (boot CPU) as online */
    cpu_data[0].online = true;
    cpus_online = 1;

    /*
     * Clean all cpu_data cachelines to PoC before booting secondaries.
     * Without SMPEN, DC CIVAC (used later to read secondary updates)
     * first writes back any dirty data from this CPU's L1. If cpu_data
     * is still dirty here (from init_cpu_data writing online=false),
     * CIVAC would overwrite the secondary's online=true at PoC.
     */
    cache_clean_range(cpu_data, sizeof(cpu_data));

    /* Boot secondary CPUs via PSCI CPU_ON */
    for (uint32_t cpu = 1; cpu < cpu_count; cpu++) {
        if (boot_secondary(cpu) == PSCI_SUCCESS) {
            booted++;
        }
    }

    INFO("SMP: %u/%u secondary CPUs online", booted, cpu_count - 1);
    cpus_online = 1 + booted;  /* Set from primary's count, not secondary writes */
    INFO("SMP: total %u CPUs active", cpus_online);

    /* Run SMP validation tests */
    smp_run_tests();
}
