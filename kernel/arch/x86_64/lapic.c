/*
 * lapic.c - Local APIC driver for x86-64
 *
 * Memory-mapped interface to the per-CPU Local APIC.
 * Base address obtained from ACPI MADT (default 0xFEE00000).
 */

#include "platform.h"

#if defined(PLATFORM_X86_64)

#include <stdint.h>
#include "uart.h"
#include "cpuid.h"

/* LAPIC register offsets */
#define LAPIC_ID            0x020
#define LAPIC_VERSION       0x030
#define LAPIC_TPR           0x080   /* Task Priority Register */
#define LAPIC_EOI           0x0B0   /* End of Interrupt */
#define LAPIC_LDR           0x0D0   /* Logical Destination */
#define LAPIC_DFR           0x0E0   /* Destination Format */
#define LAPIC_SVR           0x0F0   /* Spurious Vector Register */
#define LAPIC_ISR_BASE      0x100   /* In-Service Register (8 × 32-bit) */
#define LAPIC_ESR           0x280   /* Error Status Register */
#define LAPIC_LVT_CMCI      0x2F0   /* LVT Corrected Machine-Check Interrupt */
#define LAPIC_ICR_LO        0x300   /* Interrupt Command Register low */
#define LAPIC_ICR_HI        0x310   /* Interrupt Command Register high */
#define LAPIC_LVT_TIMER     0x320   /* LVT Timer */
#define LAPIC_LVT_THERMAL   0x330   /* LVT Thermal Sensor */
#define LAPIC_LVT_PERFMON   0x340   /* LVT Performance Counter (PMI) */
#define LAPIC_LVT_LINT0     0x350   /* LVT LINT0 */
#define LAPIC_LVT_LINT1     0x360   /* LVT LINT1 */
#define LAPIC_LVT_ERROR     0x370   /* LVT Error */
#define LAPIC_TIMER_INIT    0x380   /* Timer Initial Count */
#define LAPIC_TIMER_CURRENT 0x390   /* Timer Current Count */
#define LAPIC_TIMER_DIVIDE  0x3E0   /* Timer Divide Configuration */

/* IA32_APIC_BASE MSR (0x1B) layout — see Intel SDM Vol 3A §10.4.4.
 * We only touch this MSR to recover a known state on entry; all
 * subsequent LAPIC access is through the MMIO window (xAPIC mode). */
#define IA32_APIC_BASE_MSR    0x1B
#define APIC_BASE_EXTD        (1ULL << 10)  /* x2APIC enable */
#define APIC_BASE_EN          (1ULL << 11)  /* LAPIC global enable */

/* APIC base physical address bits. Intel SDM §10.4.4 defines bits
 * 12-51 as the base-address field. Keeping the full 40-bit width
 * (rather than the common 0xFFFFF000 shortcut) means an ACPI MADT
 * that reports a LAPIC above 4 GiB — permitted by the spec but
 * never seen on contemporary hardware — wouldn't silently truncate. */
#define APIC_BASE_ADDR_MASK   0x000FFFFFFFFFF000ULL

/* Default xAPIC MMIO base address on all modern x86 systems. */
#define LAPIC_DEFAULT_BASE    0xFEE00000ULL

/* Bounded busy-wait budget for PIT calibration paths. ~200 ms at
 * 3 GHz; comfortably longer than the legitimate ~10 ms PIT window
 * but short enough that a kexec-disabled PIT can't hang boot. Both
 * lapic_timer_calibrate (this file) and timer_init's TSC fallback
 * (timer_x86.c) reach for the same number — keep them in sync via
 * this header-style constant. */
#define TSC_PIT_TIMEOUT_CYCLES   600000000ULL

/* SVR bits */
#define SVR_APIC_ENABLE     (1 << 8)

/* LVT Timer modes */
#define LVT_MASKED          (1 << 16)
#define LVT_TIMER_PERIODIC  (1 << 17)

/* ICR delivery modes */
#define ICR_FIXED            0x00000000
#define ICR_INIT             0x00000500
#define ICR_SIPI             0x00000600
#define ICR_LEVEL_ASSERT     0x00004000
#define ICR_LEVEL_DEASSERT   0x00000000
#define ICR_DEST_ALL_EX_SELF 0x000C0000

/* Spurious interrupt vector (must have bits 0-3 set on some older APICs) */
#define SPURIOUS_VECTOR      0xFF

static volatile uint32_t *lapic_base;

/* ---- Register Access ---- */

uint32_t lapic_read(uint32_t offset)
{
    return lapic_base[offset / 4];
}

void lapic_write(uint32_t offset, uint32_t value)
{
    lapic_base[offset / 4] = value;
}

/* ---- Public Interface ---- */

extern uint32_t acpi_get_lapic_address(void);

/* Read a model-specific register. */
static inline uint64_t rdmsr(uint32_t msr)
{
    uint32_t lo, hi;
    __asm__ volatile("rdmsr" : "=a"(lo), "=d"(hi) : "c"(msr));
    return ((uint64_t)hi << 32) | lo;
}

/* Write a model-specific register. x86 serializes WRMSR, so no fence
 * is needed before the subsequent MMIO reads in xAPIC mode. */
static inline void wrmsr(uint32_t msr, uint64_t val)
{
    uint32_t lo = (uint32_t)val;
    uint32_t hi = (uint32_t)(val >> 32);
    __asm__ volatile("wrmsr" : : "c"(msr), "a"(lo), "d"(hi));
}

/*
 * Force the LAPIC into xAPIC (MMIO) mode at 0xFEE00000, regardless
 * of what a prior environment left it in. Needed when SLM-OS is
 * kexec'd from a Linux that had x2APIC enabled — Linux's
 * lapic_shutdown() disables the LAPIC via SVR but does NOT clear
 * IA32_APIC_BASE.EXTD, so the xAPIC MMIO window at 0xFEE00000
 * returns all-ones on read and all our downstream init silently
 * fails. Observed symptom: "[LAPIC] Initialized at 0xfee00000
 * (ID=255, version=0xff)" followed by a hang at timer init.
 *
 * Per Intel SDM Vol 3A §10.12.5 Table 10-6, the only way out of
 * x2APIC is through the DISABLED state (EN=0, EXTD=0) and then
 * back to xAPIC (EN=1). But writing EN=0 when already in xAPIC is
 * risky on some implementations — QEMU rejects the re-enable and
 * leaves EN clear, locking us out until RESET. So the code takes
 * the toggle path ONLY when EXTD is actually set; xAPIC and
 * disabled states are handled with a single write.
 */
/* Non-static so kernel/tests/test_x86_boot.c can drive a synthetic
 * x2APIC → xAPIC transition without going through a fresh boot. No
 * other in-kernel caller exists; if you find yourself reaching for
 * this from production code, the right move is almost always to call
 * lapic_init / lapic_percpu_init instead. */
void lapic_force_xapic_mode(void)
{
    uint64_t base = rdmsr(IA32_APIC_BASE_MSR);
    int en   = (base & APIC_BASE_EN)   ? 1 : 0;
    int extd = (base & APIC_BASE_EXTD) ? 1 : 0;

    if (en && !extd) {
        /* LOAD-BEARING early-return — do NOT refactor to "always
         * run the two-step toggle; it's idempotent on a healthy
         * xAPIC". It isn't. QEMU TCG silently rejects the second
         * EN=1 write if you first clear EN from an already-enabled
         * xAPIC, leaving the LAPIC permanently disabled until
         * RESET. Intel SDM §10.4.3 also warns that EN, once set,
         * may be cleared exactly once per CPU lifetime on some
         * implementations. Bottom line: skip the toggle when we
         * don't actually need it. */
        return;
    }

    if (extd) {
        /* x2APIC → disabled. EN=0, EXTD=0 is the only legal
         * transition out of x2APIC. */
        wrmsr(IA32_APIC_BASE_MSR, base & ~(APIC_BASE_EN | APIC_BASE_EXTD));
        base = rdmsr(IA32_APIC_BASE_MSR);   /* re-read after transit */
    }

    /* disabled → xAPIC at canonical 0xFEE00000. Preserve BSP and
     * any platform-reserved bits; clear the old ADDR + EXTD bits
     * and OR in EN + the canonical base address. */
    uint64_t xapic = (base & ~(APIC_BASE_EN | APIC_BASE_EXTD
                               | APIC_BASE_ADDR_MASK))
                   | APIC_BASE_EN
                   | LAPIC_DEFAULT_BASE;
    wrmsr(IA32_APIC_BASE_MSR, xapic);
}

/*
 * Mask every LVT entry that could carry a stale vector inherited from
 * the prior environment. Called before SVR is enabled so a leftover
 * Linux ISR address can't fire into nothing-mapped on first APIC
 * unmask. CMCI in particular fires from machine-check banks the prior
 * kernel may have armed; the others guard against vector aliasing if
 * Linux installed handlers above our IDT range.
 *
 * Same sequence is needed by both lapic_init (BSP) and
 * lapic_percpu_init (APs) — keeping it as one helper means a future
 * addition (e.g. masking a new LVT slot Intel adds in a future SDM
 * revision) only has to land in one place.
 */
static void lapic_mask_all_lvts(void)
{
    lapic_write(LAPIC_LVT_CMCI,    LVT_MASKED);
    lapic_write(LAPIC_LVT_TIMER,   LVT_MASKED);
    lapic_write(LAPIC_LVT_THERMAL, LVT_MASKED);
    lapic_write(LAPIC_LVT_PERFMON, LVT_MASKED);
    lapic_write(LAPIC_LVT_LINT0,   LVT_MASKED);
    lapic_write(LAPIC_LVT_LINT1,   LVT_MASKED);
    lapic_write(LAPIC_LVT_ERROR,   LVT_MASKED);
}

void lapic_init(void)
{
    /* Force xAPIC mode + 0xFEE00000 base. Must run BEFORE any
     * MMIO access — if Linux left us in x2APIC the MMIO reads
     * would return 0xFFFFFFFF and downstream init is garbage. */
    lapic_force_xapic_mode();

    uint32_t addr = acpi_get_lapic_address();
    /* Sanity: ACPI's reported LAPIC base should match the default
     * 0xFEE00000 we just programmed. If ACPI disagrees, trust the
     * ACPI value (some platforms relocate the LAPIC). */
    if (addr != (uint32_t)LAPIC_DEFAULT_BASE) {
        uart_printf("[LAPIC] ACPI says base=0x%x, but we forced "
                    "0x%x via APIC_BASE MSR. Using 0x%x.\n",
                    addr, (uint32_t)LAPIC_DEFAULT_BASE, addr);
        /* Re-program the MSR with the ACPI-reported base. */
        uint64_t base = rdmsr(IA32_APIC_BASE_MSR);
        base = (base & ~(APIC_BASE_EXTD | APIC_BASE_ADDR_MASK))
             | APIC_BASE_EN | ((uint64_t)addr & APIC_BASE_ADDR_MASK);
        wrmsr(IA32_APIC_BASE_MSR, base);
    }
    lapic_base = (volatile uint32_t *)(uintptr_t)addr;

    /* Mask every LVT before touching SVR — see lapic_mask_all_lvts. */
    lapic_mask_all_lvts();

    /* Now safe to enable the APIC via SVR. */
    uint32_t svr = lapic_read(LAPIC_SVR);
    svr |= SVR_APIC_ENABLE | SPURIOUS_VECTOR;
    lapic_write(LAPIC_SVR, svr);

    /* Set Task Priority to 0 (accept all interrupts) */
    lapic_write(LAPIC_TPR, 0);

    /* Set flat destination mode */
    lapic_write(LAPIC_DFR, 0xFFFFFFFF);
    lapic_write(LAPIC_LDR, (lapic_read(LAPIC_LDR) & 0x00FFFFFF) | (1 << 24));

    /* Clear any pending errors */
    lapic_write(LAPIC_ESR, 0);
    lapic_read(LAPIC_ESR);

    /* Send EOI to clear any stale interrupts */
    lapic_write(LAPIC_EOI, 0);

    uint32_t id = (lapic_read(LAPIC_ID) >> 24) & 0xFF;
    uint32_t ver = lapic_read(LAPIC_VERSION) & 0xFF;
    uart_printf("[LAPIC] Initialized at 0x%x (ID=%u, version=0x%x)\n",
                addr, id, ver);
}

void lapic_percpu_init(void)
{
    /* Force xAPIC on each AP too. INIT-IPI does NOT reset APIC_BASE
     * per Intel SDM §10.12.1, so an AP that was in x2APIC mode at
     * kexec time (Linux enables x2APIC on all CPUs) stays there
     * after our INIT-SIPI-SIPI wake. Without this dance, AP MMIO
     * reads return 0xFFFFFFFF just like the BSP case. */
    lapic_force_xapic_mode();

    lapic_mask_all_lvts();

    uint32_t svr = lapic_read(LAPIC_SVR);
    svr |= SVR_APIC_ENABLE | SPURIOUS_VECTOR;
    lapic_write(LAPIC_SVR, svr);
    lapic_write(LAPIC_TPR, 0);
    lapic_write(LAPIC_ESR, 0);
    lapic_read(LAPIC_ESR);
    lapic_write(LAPIC_EOI, 0);
}

void lapic_eoi(void)
{
    lapic_write(LAPIC_EOI, 0);
    /*
     * Serialize EOI before the handler epilogue returns. Without a fence
     * OOO speculation can retire the `iret` (and begin re-dispatching the
     * next interrupt) before the EOI store has retired to the LAPIC,
     * allowing the same ISR to be re-entered for an interrupt it has
     * already serviced. `lock; addl $0, (%rsp)` is a full fence on x86
     * (mfence would also work).
     */
    __asm__ volatile("lock; addl $0, (%%rsp)" ::: "memory", "cc");
}

uint32_t lapic_get_id(void)
{
    return (lapic_read(LAPIC_ID) >> 24) & 0xFF;
}

void lapic_send_ipi(uint32_t apic_id, uint32_t vector, uint32_t flags)
{
    /* Write destination APIC ID to ICR high */
    lapic_write(LAPIC_ICR_HI, apic_id << 24);
    /* Write vector + flags to ICR low (this triggers the IPI) */
    lapic_write(LAPIC_ICR_LO, vector | flags);

    /* Wait for delivery (poll bit 12 = Send Pending) */
    while (lapic_read(LAPIC_ICR_LO) & (1 << 12))
        ;
}

/* ---- LAPIC Timer ---- */

void lapic_timer_setup(uint8_t vector, uint32_t initial_count, uint8_t divide)
{
    /* Set divide value */
    lapic_write(LAPIC_TIMER_DIVIDE, divide);

    /* Configure LVT Timer: periodic mode, unmasked, with specified vector */
    lapic_write(LAPIC_LVT_TIMER, LVT_TIMER_PERIODIC | vector);

    /* Set initial count (starts timer) */
    lapic_write(LAPIC_TIMER_INIT, initial_count);
}

void lapic_timer_stop(void)
{
    lapic_write(LAPIC_TIMER_INIT, 0);
    lapic_write(LAPIC_LVT_TIMER, LVT_MASKED);
}

uint32_t lapic_timer_current(void)
{
    return lapic_read(LAPIC_TIMER_CURRENT);
}

/* RDTSC — used only for the calibration timeout; timer_x86.c has its
 * own copy it uses for the actual TSC exposure to the scheduler. */
static inline uint64_t rdtsc_lapic(void)
{
    uint32_t lo, hi;
    __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | lo;
}

/*
 * Read LAPIC timer bus frequency from CPUID leaf 0x15 when the CPU
 * reports it directly (Skylake+ typically do). Returns 0 if the
 * leaf is unavailable or incomplete.
 *
 * CPUID 0x15:
 *   EAX: denominator of TSC/crystal ratio
 *   EBX: numerator   of TSC/crystal ratio (0 → leaf unsupported)
 *   ECX: core crystal frequency in Hz (0 → unknown on this CPU)
 *
 * The LAPIC timer is clocked by the same core crystal as TSC, and
 * our `lapic_timer_calibrate` caller wants "LAPIC ticks/sec with
 * divide-by-16" — so we return ECX (the crystal itself), and the
 * divide-by-16 factor is applied downstream as before.
 */
static uint32_t lapic_timer_freq_cpuid(void)
{
    uint32_t eax, ebx, ecx, edx;
    /* Max leaf check first. */
    x86_cpuid(0, &eax, &ebx, &ecx, &edx);
    if (eax < 0x15)
        return 0;
    x86_cpuid(0x15, &eax, &ebx, &ecx, &edx);
    if (ebx == 0 || ecx == 0)
        return 0;
    return ecx;   /* Hz */
}

/*
 * Calibrate LAPIC timer. Prefers CPUID leaf 0x15 (deterministic,
 * works across kexec handoffs); falls back to a PIT-based busy-wait
 * only when CPUID reports nothing useful. The PIT path is bounded
 * by a TSC-based timeout so a chipset that has disabled PIT
 * channel 2 (seen on recent Intel PCHs post-kexec) can't hang the
 * boot.
 *
 * Returns LAPIC timer ticks per second, or 0 on total calibration
 * failure (caller falls back to a hardcoded default).
 */
uint32_t lapic_timer_calibrate(void)
{
    /* Preferred path: ask the CPU. */
    uint32_t from_cpuid = lapic_timer_freq_cpuid();
    if (from_cpuid != 0) {
        uart_printf("[LAPIC] Timer frequency from CPUID 0x15: %u Hz (%u MHz)\n",
                    from_cpuid, from_cpuid / 1000000);
        return from_cpuid;
    }

    /* Fallback: calibrate against PIT channel 2. */
    extern void outb(uint16_t port, uint8_t val);
    extern uint8_t inb(uint16_t port);

    lapic_write(LAPIC_TIMER_DIVIDE, 0x03);   /* divide by 16 */
    lapic_write(LAPIC_LVT_TIMER, LVT_MASKED);
    lapic_write(LAPIC_TIMER_INIT, 0xFFFFFFFF);

    uint16_t pit_count = 11932;   /* ~10 ms at 1.193182 MHz */

    outb(0x61, (inb(0x61) & 0xFD) | 0x01);   /* gate on, speaker off */
    outb(0x43, 0xB0);                        /* ch2, lo/hi, mode 0 */
    outb(0x42, pit_count & 0xFF);
    outb(0x42, (pit_count >> 8) & 0xFF);

    uint8_t tmp = inb(0x61) & 0xFE;
    outb(0x61, tmp);
    outb(0x61, tmp | 0x01);

    /* Bounded busy-wait. Cap at ~200 ms of TSC time — if the PIT
     * channel 2 output hasn't latched by then, it's not going to.
     * Without this cap, a kexec-inherited disabled PIT hangs the
     * boot indefinitely at `Initializing timer...`. */
    uint64_t tsc_start = rdtsc_lapic();
    int pit_ok = 0;
    while (1) {
        if (inb(0x61) & 0x20) { pit_ok = 1; break; }
        if (rdtsc_lapic() - tsc_start > TSC_PIT_TIMEOUT_CYCLES) break;
    }

    lapic_write(LAPIC_LVT_TIMER, LVT_MASKED);   /* stop timer */

    if (!pit_ok) {
        uart_printf("[LAPIC] PIT channel 2 never fired — calibration skipped "
                    "(likely disabled by chipset post-kexec)\n");
        return 0;
    }

    uint32_t elapsed = 0xFFFFFFFF - lapic_read(LAPIC_TIMER_CURRENT);
    uint32_t ticks_per_sec = elapsed * 100 * 16;
    uart_printf("[LAPIC] Timer calibration: %u ticks/10ms (div16), %u MHz\n",
                elapsed, ticks_per_sec / 1000000);
    return ticks_per_sec;
}

/* outb/inb for PIT calibration (duplicated to avoid header dependency) */
void __attribute__((weak)) outb(uint16_t port, uint8_t val)
{
    __asm__ volatile("outb %0, %1" : : "a"(val), "Nd"(port));
}

uint8_t __attribute__((weak)) inb(uint16_t port)
{
    uint8_t ret;
    __asm__ volatile("inb %1, %0" : "=a"(ret) : "Nd"(port));
    return ret;
}

#endif /* PLATFORM_X86_64 */
