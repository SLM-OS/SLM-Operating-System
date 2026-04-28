/*
 * platform_x86.c - x86-64 platform initialization and stubs
 *
 * Provides boot glue, SMP boot via INIT-SIPI-SIPI, VMM/DTB/Rust stubs,
 * and the entry point wrapper that bridges the Multiboot2 entry to the
 * main kernel.
 */

#include "platform.h"

#if defined(PLATFORM_X86_64)

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "smp.h"
#include "dtb.h"
#include "uart.h"
#include "spinlock.h"
#include "pmm.h"
#include "sched.h"
#include "gic.h"
#include "timer.h"

/* ---- Boot entry bridge ---- */

/* ---- Reschedule IPI (B1 / P2-1) ----
 *
 * A tiny, high-priority IPI that nudges an idle or busy remote CPU
 * to re-check its run queue. Handler calls schedule() directly —
 * NOT scheduler_tick() — because a tick advances quantum accounting
 * and doing that on every cross-CPU dispatch would corrupt the
 * fairness policy. The LAPIC timer remains the sole source of
 * quantum ticks; the IPI just requests an immediate scheduling
 * opportunity.
 *
 * Vector 49: the idt.S stub is already in place (originally reserved
 * for "future SMP IPI"). Runs on IST1 so it cannot corrupt the
 * interrupted task's stack (A1 / P1-1).
 */
#define RESCHED_VECTOR  49

/* Diagnostic counter exported for tests. */
volatile uint64_t smp_resched_ipi_count[MAX_CPUS];

extern void schedule(void);
extern void lapic_eoi(void);
extern void lapic_send_ipi(uint32_t apic_id, uint32_t vector, uint32_t flags);

static void resched_ipi_handler(uint8_t irq)
{
    (void)irq;
    uint32_t cpu = cpu_id();
    if (cpu < MAX_CPUS)
        smp_resched_ipi_count[cpu]++;
    /* schedule() internally guards with preempt_disabled[cpu], so a
     * nested call while schedule() is already in flight no-ops
     * safely. */
    schedule();
}

extern void irq_register(uint8_t irq, void (*handler)(uint8_t));

void smp_resched_ipi_init(void)
{
    /* irq_register takes (vector - 32) as its index. */
    irq_register(RESCHED_VECTOR - 32, resched_ipi_handler);
}

/* smp_notify_cpu() — x86-64 backend (see include/smp.h).
 *
 * Sends a RESCHED_VECTOR IPI to @logical_cpu's LAPIC. If that CPU
 * is in `hlt` it wakes immediately; if it is running user code the
 * ISR runs as soon as RFLAGS.IF allows and calls schedule(). A call
 * with @logical_cpu == cpu_id() is a cheap no-op — self-IPI isn't
 * needed because the caller will hit its own timer tick or explicit
 * yield().
 */
void smp_notify_cpu(uint32_t logical_cpu)
{
    if (logical_cpu == cpu_id())
        return;
    if (logical_cpu >= cpu_count)
        return;
    uint32_t apic_id = (uint32_t)cpu_data[logical_cpu].mpidr;
    lapic_send_ipi(apic_id, RESCHED_VECTOR, 0);
}

/* Saved Multiboot2 info address (set by entry64.S → kernel_main_x86) */
uint32_t multiboot_info_addr;

/* Main kernel entry (from kernel/src/main.c) */
extern void kernel_main(void *dtb);

/*
 * x86-64 boot entry point — called from entry64.S.
 * Saves the multiboot info pointer, then calls the main kernel.
 */
/*
 * Snapshot of the GRUB-supplied Multiboot2 info structure.
 *
 * GRUB places the info immediately above the kernel's LOAD segment.
 * With a small kernel that happens to land in kernel-owned memory;
 * with a 40 MB kernel (post-Phase-E GSP firmware embedding) it
 * lands in memory that PMM later free-lists, and subsequent
 * allocations corrupt it. Copying the entire structure into a
 * static BSS buffer here — before any other init runs — makes it
 * survive PMM bring-up and any later kernel allocation.
 *
 * 64 KiB is Multiboot2's documented upper bound on info-structure
 * size (the test suite's test_multiboot2_structure_valid asserts
 * the same bound).
 */
#define MB2_SNAPSHOT_MAX (64 * 1024)
alignas(8) static uint8_t mb2_snapshot[MB2_SNAPSHOT_MAX];

void kernel_main_x86(uint32_t mb_addr)
{
    /* mb_addr == 0 means the caller had no Multiboot2 info to pass.
     * The bzImage kexec entry (bzimage_entry.S) is the first such
     * path; KVM-direct / Xen / UEFI-stub boot could be others in the
     * future. Leave mb2_snapshot zeroed (BSS default), which reads
     * back as a zero-size MB2 info struct — detect_ram_end and other
     * callers that walk the info tags will see an empty structure
     * and fall back to their defaults instead of NULL-dereffing. */
    if (mb_addr != 0) {
        /* Read the size from the live structure FIRST — before
         * anything else runs that could alter the page it lives on.
         * Then clamp to the snapshot buffer and memcpy in. */
        const uint8_t *src = (const uint8_t *)(uintptr_t)mb_addr;
        uint32_t total_size = *(const uint32_t *)src;
        if (total_size > MB2_SNAPSHOT_MAX)
            total_size = MB2_SNAPSHOT_MAX;
        for (uint32_t i = 0; i < total_size; i++)
            mb2_snapshot[i] = src[i];
    }

    /* Point the rest of the kernel at the snapshot (zero-filled if
     * mb_addr was 0). Both the assembly symbol (multiboot_ptr, read
     * by tests via extern) and the C variable (multiboot_info_addr,
     * read by detect_ram_end) now reference kernel-owned memory. */
    extern uint32_t multiboot_ptr;
    multiboot_ptr = (uint32_t)(uintptr_t)&mb2_snapshot[0];
    multiboot_info_addr = (uint32_t)(uintptr_t)&mb2_snapshot[0];

    kernel_main(NULL);  /* No DTB on x86-64 */
}

/* ---- SMP ---- */

/* ACPI interface (acpi.c) */
extern int acpi_init(void);
extern uint32_t acpi_get_enabled_cpu_count(void);
extern uint8_t acpi_get_cpu_apic_id(uint32_t logical_id);

/* LAPIC interface (lapic.c) */
extern void lapic_send_ipi(uint32_t apic_id, uint32_t vector, uint32_t flags);
extern uint32_t lapic_get_id(void);

/* AP trampoline symbols (ap_trampoline.S) */
extern char ap_trampoline_start[];
extern char ap_trampoline_end[];

/* IDT pointer — needed for AP trampoline params */
extern struct {
    uint16_t limit;
    uint64_t base;
} __attribute__((packed)) idtr;

/* GDT pointer — from trampoline32.S */
extern struct {
    uint16_t limit;
    uint32_t base;
} __attribute__((packed)) gdt64_ptr;

struct per_cpu cpu_data[MAX_CPUS];
uint64_t cpu_logical_map[MAX_CPUS];
uint32_t cpu_count = 1;
volatile uint32_t cpus_online = 1;

/*
 * AP trampoline parameters — written by BSP at TRAMP_BASE + 0xF00,
 * read by AP trampoline code. Layout must match ap_trampoline.S.
 */
#define AP_TRAMPOLINE_BASE  0x8000
#define AP_PARAMS_OFF       0xF00

struct ap_boot_params {
    uint64_t cr3;           /* +0x00: PML4 physical address */
    uint16_t gdt_limit;     /* +0x08: GDT limit */
    uint64_t gdt_base;      /* +0x0A: GDT base (note: packed, offset 0x0A not 0x10) */
    uint16_t idt_limit;     /* +0x12: IDT limit */
    uint64_t idt_base;      /* +0x14: IDT base */
    uint32_t _pad;          /* +0x1C: alignment */
    uint64_t stack;         /* +0x1C: per-CPU stack top (overwrites _pad at correct offset) */
    uint32_t cpu_id;        /* +0x24: logical CPU ID */
    uint64_t entry;         /* +0x28: 64-bit entry point */
    volatile uint32_t flag; /* +0x30: AP sets to 1 when running */
} __attribute__((packed));

/* Per-CPU stack size: 16 KB */
#define AP_STACK_SIZE  (16384)
#define AP_STACK_ORDER 2  /* 4 pages = 16 KB */

/* ICR flags for INIT and SIPI */
#define ICR_INIT            0x00000500
#define ICR_SIPI            0x00000600
#define ICR_LEVEL_ASSERT    0x00004000
#define ICR_LEVEL_DEASSERT  0x00000000

/* I/O port access */
static inline void io_outb(uint16_t port, uint8_t val)
{
    __asm__ volatile("outb %0, %1" : : "a"(val), "Nd"(port));
}

/*
 * Busy-wait delay using PIT channel 2.
 * Approximate: ~1 PIT tick = 0.838 µs, so ms * 1193 ≈ ms in PIT ticks.
 */
static void delay_ms(uint32_t ms)
{
    /* Simple busy-loop; not precise but sufficient for SIPI timing */
    for (volatile uint32_t i = 0; i < ms * 100000; i++)
        __asm__ volatile("pause");
}

/*
 * 64-bit AP entry point — called from ap_trampoline.S after mode transition.
 * Runs on the AP's own stack in long mode. Performs per-CPU init and enters
 * the scheduler.
 */
extern void tss_install_current_cpu(uint32_t cpu_id);

static void ap_entry_64(uint32_t logical_cpu_id)
{
    /* Install this AP's TSS + IST1 BEFORE any interrupt can fire.
     * The shared IDT has LAPIC timer vector 48 flagged ist=1; without
     * a loaded TR the first tick #TS-faults. Must run before
     * timer_percpu_init() enables the local LAPIC timer. A1 / P1-1. */
    tss_install_current_cpu(logical_cpu_id);

    /* Initialize per-CPU LAPIC */
    gic_percpu_init();

    /* Initialize per-CPU timer */
    timer_percpu_init();

    /* Mark CPU online (atomic increment — multiple APs may boot concurrently) */
    cpu_data[logical_cpu_id].online = true;
    __atomic_add_fetch(&cpus_online, 1, __ATOMIC_SEQ_CST);

    uart_printf("[SMP] CPU %u: online (APIC ID %u)\n",
                logical_cpu_id, (uint32_t)cpu_data[logical_cpu_id].mpidr);

    /* Wait for BSP to finish scheduler initialization */
    while (!scheduler_is_initialized())
        __asm__ volatile("pause" ::: "memory");

    /* Initialize scheduler for this CPU */
    scheduler_init_secondary(logical_cpu_id);

    /* Start per-CPU timer */
    timer_start();

    /* Enable interrupts */
    __asm__ volatile("sti");

    /* Enter scheduler — does not return */
    scheduler_start(0);

    /* Should never reach here */
    __asm__ volatile("cli; hlt");
    while (1);
}

/*
 * Boot a single AP via INIT-SIPI-SIPI sequence.
 * Returns 0 on success, -1 on timeout.
 */
static int boot_ap(uint32_t logical_cpu_id, uint8_t apic_id)
{
    /* Allocate per-CPU stack */
    void *stack_pages = pmm_alloc_pages(1 << AP_STACK_ORDER);
    if (!stack_pages) {
        uart_printf("[SMP] CPU %u: failed to allocate stack\n", logical_cpu_id);
        return -1;
    }
    void *stack_top = (char *)stack_pages + AP_STACK_SIZE;
    cpu_data[logical_cpu_id].stack_top = stack_top;

    /* Set up boot parameters at TRAMP_BASE + PARAMS_OFF */
    volatile uint8_t *params_base = (volatile uint8_t *)(AP_TRAMPOLINE_BASE + AP_PARAMS_OFF);

    /* CR3 — BSP's PML4 */
    uint64_t cr3;
    __asm__ volatile("mov %%cr3, %0" : "=r"(cr3));
    *(volatile uint64_t *)(params_base + 0x00) = cr3;

    /* GDT pointer (limit + 64-bit base) */
    *(volatile uint16_t *)(params_base + 0x08) = gdt64_ptr.limit;
    *(volatile uint64_t *)(params_base + 0x0A) = (uint64_t)gdt64_ptr.base;

    /* IDT pointer (limit + 64-bit base) — read directly via SIDT */
    struct { uint16_t limit; uint64_t base; } __attribute__((packed)) idt_val;
    __asm__ volatile("sidt %0" : "=m"(idt_val));
    *(volatile uint16_t *)(params_base + 0x12) = idt_val.limit;
    *(volatile uint64_t *)(params_base + 0x14) = idt_val.base;

    /* Per-CPU stack top */
    *(volatile uint64_t *)(params_base + 0x1C) = (uint64_t)stack_top;

    /* Logical CPU ID */
    *(volatile uint32_t *)(params_base + 0x24) = logical_cpu_id;

    /* 64-bit entry point */
    *(volatile uint64_t *)(params_base + 0x28) = (uint64_t)ap_entry_64;

    /* Clear flag — AP will set to 1 */
    *(volatile uint32_t *)(params_base + 0x30) = 0;

    /* Memory fence to ensure all params are visible */
    __asm__ volatile("mfence" ::: "memory");

    /* Send INIT IPI */
    lapic_send_ipi(apic_id, 0, ICR_INIT | ICR_LEVEL_ASSERT);
    delay_ms(1);
    lapic_send_ipi(apic_id, 0, ICR_INIT | ICR_LEVEL_DEASSERT);
    delay_ms(10);

    /* Send first SIPI — vector = page number of trampoline (0x8000 / 4096 = 8) */
    uint8_t sipi_vector = AP_TRAMPOLINE_BASE >> 12;
    lapic_send_ipi(apic_id, sipi_vector, ICR_SIPI);
    delay_ms(1);

    /* Wait for AP to signal it's running */
    for (int i = 0; i < 1000; i++) {
        if (*(volatile uint32_t *)(params_base + 0x30) != 0)
            return 0;  /* AP is up */
        delay_ms(1);
    }

    /* Timeout — send second SIPI (Intel spec allows retry) */
    lapic_send_ipi(apic_id, sipi_vector, ICR_SIPI);

    for (int i = 0; i < 1000; i++) {
        if (*(volatile uint32_t *)(params_base + 0x30) != 0)
            return 0;
        delay_ms(1);
    }

    uart_printf("[SMP] CPU %u: SIPI timeout (APIC ID %u)\n",
                logical_cpu_id, apic_id);
    return -1;
}

void smp_init(void)
{
    /* Discover CPUs via ACPI MADT */
    if (acpi_init() == 0) {
        cpu_count = acpi_get_enabled_cpu_count();
        if (cpu_count > MAX_CPUS) cpu_count = MAX_CPUS;
    } else {
        cpu_count = 1;
    }

    /* Initialize BSP (CPU 0) */
    cpu_data[0].cpu_id = 0;
    cpu_data[0].mpidr = acpi_get_cpu_apic_id(0);
    cpu_data[0].online = true;
    cpu_data[0].stack_top = NULL;
    cpu_logical_map[0] = acpi_get_cpu_apic_id(0);

    /* Store APIC IDs for all CPUs */
    for (uint32_t i = 1; i < cpu_count; i++) {
        cpu_data[i].cpu_id = i;
        cpu_data[i].mpidr = acpi_get_cpu_apic_id(i);
        cpu_data[i].online = false;
        cpu_logical_map[i] = acpi_get_cpu_apic_id(i);
    }

    cpus_online = 1;

    uart_printf("[SMP] %u CPUs detected (BSP APIC ID %u)\n",
                cpu_count, acpi_get_cpu_apic_id(0));

    if (cpu_count <= 1)
        return;

    /* Copy AP trampoline to low memory (below 1MB, at 0x8000) */
    uintptr_t tramp_size = (uintptr_t)ap_trampoline_end - (uintptr_t)ap_trampoline_start;
    volatile uint8_t *tramp_dest = (volatile uint8_t *)AP_TRAMPOLINE_BASE;
    const uint8_t *tramp_src = (const uint8_t *)ap_trampoline_start;

    /* Zero the entire trampoline page (including params area) */
    for (uintptr_t i = 0; i < 0x1000; i++)
        tramp_dest[i] = 0;

    /* Copy trampoline code */
    for (uintptr_t i = 0; i < tramp_size; i++)
        tramp_dest[i] = tramp_src[i];

    uart_printf("[SMP] Trampoline copied to 0x%x (%lu bytes)\n",
                AP_TRAMPOLINE_BASE, (unsigned long)tramp_size);

    /* Boot each AP sequentially */
    for (uint32_t i = 1; i < cpu_count; i++) {
        uint8_t apic_id = acpi_get_cpu_apic_id(i);
        uart_printf("[SMP] Booting CPU %u (APIC ID %u)...\n", i, apic_id);
        boot_ap(i, apic_id);
    }

    uart_printf("[SMP] %u/%u CPUs online\n", cpus_online, cpu_count);
}

/*
 * Look up logical CPU ID from LAPIC ID.
 * Used by cpu_id() in smp.h.
 */
int cpu_logical_id(uint64_t mpidr)
{
    for (uint32_t i = 0; i < cpu_count; i++) {
        if (cpu_logical_map[i] == mpidr)
            return (int)i;
    }
    return 0;
}

/* secondary_init stub — not used on x86-64, ap_entry_64 handles this */
void secondary_init(uint32_t cpu)
{
    (void)cpu;
}

int psci_cpu_on(uint64_t target_mpidr, uintptr_t entry_point, uintptr_t context_id)
{
    (void)target_mpidr;
    (void)entry_point;
    (void)context_id;
    return -1;  /* x86-64 uses INIT-SIPI, not PSCI */
}

void psci_cpu_off(void)
{
    __asm__ volatile("cli; hlt");
    while (1);
}

void psci_system_reset(void)
{
    /* Attempt keyboard controller reset */
    __asm__ volatile("outb %0, %1" : : "a"((uint8_t)0xFE), "Nd"((uint16_t)0x64));
    /* If that didn't work, triple fault */
    __asm__ volatile("lidt %0" : : "m"(*(uint64_t *)0));
    __asm__ volatile("int3");
    while (1);
}

void psci_system_off(void)
{
    uart_puts("System halted.\n");
    __asm__ volatile("cli");
    while (1)
        __asm__ volatile("hlt");
}

/* ---- VMM: extend page tables to map all RAM ---- */

/* spinlock_hw_enabled is referenced by spinlock.h */
volatile int spinlock_hw_enabled = 1;  /* x86-64 coherency always works */

/* Page table symbols from entry64.S */
extern uint64_t pml4[];
extern uint64_t pdpt[];
extern uint64_t pd[];      /* 20 PD pages (80KB) */

/* Detected RAM end address — used by PMM instead of hardcoded RAM_SIZE */
uintptr_t x86_detected_ram_end = 0;

/*
 * Find the highest usable address from the Multiboot2 memory map.
 */
static uintptr_t detect_ram_end(void)
{
    if (multiboot_info_addr == 0)
        return RAM_BASE + 0x3DE00000UL;  /* Fallback: ~1GB */

    uint8_t *ptr = (uint8_t *)(uintptr_t)multiboot_info_addr;
    uint32_t total_size = *(uint32_t *)ptr;
    uint8_t *end = ptr + total_size;
    uintptr_t highest = 0;

    ptr += 8;  /* Skip size + reserved */
    while (ptr < end) {
        uint32_t tag_type = *(uint32_t *)ptr;
        uint32_t tag_size = *(uint32_t *)(ptr + 4);
        if (tag_type == 0) break;

        if (tag_type == 6) {  /* Memory map */
            uint32_t entry_size = *(uint32_t *)(ptr + 8);
            uint8_t *entry = ptr + 16;
            while (entry < ptr + tag_size) {
                uint64_t base = *(uint64_t *)entry;
                uint64_t len  = *(uint64_t *)(entry + 8);
                uint32_t type = *(uint32_t *)(entry + 16);
                if (type == 1) {  /* Available */
                    uint64_t region_end = base + len;
                    if (region_end > highest)
                        highest = region_end;
                }
                entry += entry_size;
            }
            break;
        }
        ptr += (tag_size + 7) & ~7;
    }

    return (uintptr_t)highest;
}

void vmm_init(void)
{
    /* Detect total RAM from Multiboot2 memory map */
    uintptr_t ram_end = detect_ram_end();
    x86_detected_ram_end = ram_end;

    /* Calculate how many 1GB PD pages are needed.
     * Map 2 extra GB beyond actual RAM so the buddy allocator's
     * free_block headers and power-of-2 block merging can't
     * touch unmapped memory. */
    uint64_t gb_for_ram = (ram_end + 0x3FFFFFFFUL) >> 30;  /* Round up */
    uint64_t gb_needed = gb_for_ram + 2;  /* Extra margin for buddy allocator */
    if (gb_needed > 20)
        gb_needed = 20;  /* Limited by reserved PD pages */

    uart_printf("[VMM] Detected RAM end: 0x%lx (%lu GB)\n", ram_end, gb_needed);

    /* trampoline32.S already mapped 0-4GB (PDPT[0..3] → PD[0..3]).
     * Extend PDPT[4..N] → PD[4..N] for RAM above 4GB. */
    if (gb_needed > 4) {
        for (uint64_t i = 4; i < gb_needed; i++) {
            /* Point PDPT[i] at PD[i] (each PD is 4096 bytes apart) */
            uint64_t pd_phys = (uint64_t)&pd[512 * i];  /* PD[i] base (identity mapped) */
            pdpt[i] = pd_phys | 0x3;  /* Present + Writable */

            /* Fill PD[i] with 512 × 2MB pages */
            uint64_t *pd_page = &pd[512 * i];
            for (uint64_t j = 0; j < 512; j++) {
                uint64_t phys = (i << 30) | (j << 21);  /* i*1GB + j*2MB */
                pd_page[j] = phys | 0x83;  /* Present + Writable + 2MB */
            }
        }

        uart_printf("[VMM] Flushing TLB...\n");

        /* Flush TLB by reloading CR3 */
        uint64_t cr3;
        __asm__ volatile("mov %%cr3, %0" : "=r"(cr3));
        __asm__ volatile("mov %0, %%cr3" :: "r"(cr3) : "memory");

        uart_printf("[VMM] Extended identity mapping: %lu GB (%lu PD pages)\n",
                    gb_needed, gb_needed);
    } else {
        uart_printf("[VMM] Boot mapping sufficient (4 GB)\n");
    }

    /* Clamp detected RAM end to what's actually mapped.
     * Subtract one page to ensure the last free block's header
     * doesn't land on the first unmapped byte. */
    uintptr_t mapped_end = gb_needed << 30;  /* gb_needed * 1GB */
    if (x86_detected_ram_end > mapped_end)
        x86_detected_ram_end = mapped_end;
    /* Align down to page boundary */
    x86_detected_ram_end &= ~(uintptr_t)0xFFF;
}

/* ---- DTB stubs ---- */

static fdt_info_t dummy_fdt_info;

int dtb_parse(const void *dtb, fdt_info_t *info)
{
    (void)dtb;
    if (info) {
        /* Zero out — main.c checks fields. The hardcoded [0] index was a
         * typo: only byte 0 was being cleared on every iteration, leaving
         * the rest of *info as uninitialized stack memory for callers. */
        for (unsigned i = 0; i < sizeof(*info); i++)
            ((uint8_t *)info)[i] = 0;
    }
    return FDT_ERR_BADPTR;  /* No DTB on x86-64 */
}

void dtb_print_info(const fdt_info_t *info)
{
    (void)info;
}

const fdt_info_t *dtb_get_info(void)
{
    return &dummy_fdt_info;
}

const void *dtb_get_blob(void)
{
    return NULL;    /* No DTB on x86-64 */
}

/* dtb_get_memreserves and dtb_get_chosen live in
 * kernel/src/dtb_x86_stub.c (shipped earlier as the purpose-built
 * non-DTB-platform stub file). Defining them here too produced a
 * multi-definition link error after both PRs landed on main. Keep
 * the canonical version in dtb_x86_stub.c. */

/* ---- Rust FFI stubs (weak — overridden by real Rust library when linked) ---- */

__attribute__((weak)) void rust_heap_init(void *heap_start, size_t heap_size)
{
    (void)heap_start; (void)heap_size;
}

__attribute__((weak)) int rust_init(void)
{
    return 42;
}

__attribute__((weak)) void rust_hello(void) {}

__attribute__((weak)) int rust_model_mem_init(void)
{
    return 0;
}

__attribute__((weak)) int rust_run_tests(void)
{
    return 0;
}

/* ---- GPU cache stubs (gpu.h declares non-static versions) ---- */

void cache_clean_range(void *addr, size_t size)
{
    (void)addr; (void)size;
}

void cache_invalidate_range(void *addr, size_t size)
{
    (void)addr; (void)size;
}

void cache_flush_range(void *addr, size_t size)
{
    (void)addr; (void)size;
}

/* ---- Lua shell stub (for Makefile.test builds without Lua library) ---- */
#if !defined(SLM_INTEGRATED_BUILD) || !defined(__LUA_LINKED__)
void __attribute__((weak)) lua_shell_init(void) {}
#endif

/* ---- VMM stats stub ---- */

struct vmm_stats {
    uint64_t total_mappings;
    uint64_t device_mappings;
    uint64_t table_pages;
};

void vmm_get_stats(struct vmm_stats *stats)
{
    if (stats) {
        stats->total_mappings = 512;  /* 1GB identity map */
        stats->device_mappings = 0;
        stats->table_pages = 3;       /* PML4 + PDPT + PD */
    }
}

/* ---- Rust model memory stubs (weak — overridden by Rust library) ---- */

struct pool_stats {
    uint64_t total_bytes;
    uint64_t used_bytes;
    uint64_t free_bytes;
    uint32_t num_allocs;
};

__attribute__((weak)) void rust_weight_pool_stats(struct pool_stats *stats)
{
    if (stats) { stats->total_bytes = 0; stats->used_bytes = 0; stats->free_bytes = 0; stats->num_allocs = 0; }
}

__attribute__((weak)) void rust_workspace_pool_stats(struct pool_stats *stats)
{
    if (stats) { stats->total_bytes = 0; stats->used_bytes = 0; stats->free_bytes = 0; stats->num_allocs = 0; }
}

/* ---- Component system stubs (weak — overridden by Rust library) ---- */

__attribute__((weak)) uint32_t component_count(void) { return 0; }
__attribute__((weak)) int component_run(const char *name) { (void)name; return -1; }
__attribute__((weak)) int component_send_echo(const char *msg) { (void)msg; return -1; }
__attribute__((weak)) void component_list_builtins(void) {}

/* Timer diagnostic counter (defined in ARM64 timer.c, referenced by shell_sys.c) */
__attribute__((weak)) volatile uint32_t timer_handler_count = 0;

struct component_info {
    uint32_t id;
    char name[32];
    uint32_t state;
    uint32_t type;
};

__attribute__((weak)) int component_get_info(uint32_t index, struct component_info *info)
{ (void)index; (void)info; return -1; }

__attribute__((weak)) const char *component_state_name(uint32_t state)
{ (void)state; return "unknown"; }

__attribute__((weak)) const char *component_type_name(uint32_t type)
{ (void)type; return "unknown"; }

__attribute__((weak)) int component_register(const char *name, uint32_t type, void *config)
{ (void)name; (void)type; (void)config; return -1; }

__attribute__((weak)) int component_unregister(uint32_t id)
{ (void)id; return -1; }

__attribute__((weak)) int component_find(const char *name)
{ (void)name; return -1; }

/* Component system init — also provided by Rust */
__attribute__((weak)) int component_system_init(void) { return 0; }

/* ---- VMM mapping stubs ---- */

void *vmm_map_region(uint64_t phys, size_t size, uint64_t flags)
{
    (void)flags;
    (void)size;
    /* Identity mapping — virtual == physical */
    return (void *)phys;
}

void vmm_unmap_block(void *virt)
{
    (void)virt;
}

#endif /* PLATFORM_X86_64 */
