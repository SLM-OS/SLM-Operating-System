/*
 * ioapic.h — x86-64 IOAPIC driver interface (kernel/arch/x86_64/ioapic.c).
 *
 * Used by the gic.h shim in pic.c and by test_x86_boot.c. IRQ numbers
 * are IOAPIC vectors (IOAPIC_VECTOR_BASE + pin); ACPI interrupt source
 * overrides are applied when mapping a vector to its redirection pin.
 */
#ifndef SLMOS_ARCH_X86_64_IOAPIC_H
#define SLMOS_ARCH_X86_64_IOAPIC_H

#include "platform.h"

#if defined(PLATFORM_X86_64)

#include <stdint.h>
#include <stdbool.h>

void ioapic_init(void);
void ioapic_enable_irq(uint32_t irq);
void ioapic_disable_irq(uint32_t irq);

/* True iff the IRQ's redirection entry is masked, or the IRQ has no
 * IOAPIC pin. */
bool ioapic_irq_is_masked(uint32_t irq);

#endif /* PLATFORM_X86_64 */

#endif /* SLMOS_ARCH_X86_64_IOAPIC_H */
