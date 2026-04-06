/*
 * ioapic.c - I/O APIC driver for x86-64
 *
 * Manages the I/O APIC redirection table for routing external
 * interrupts (keyboard, timer, etc.) to Local APIC vectors.
 * Base address obtained from ACPI MADT (default 0xFEC00000).
 */

#include "platform.h"

#if defined(PLATFORM_X86_64)

#include <stdint.h>
#include <stdbool.h>
#include "uart.h"

/* IOAPIC uses indirect register access via REGSEL + DATAWIN */
#define IOAPIC_REGSEL       0x00
#define IOAPIC_DATAWIN      0x10

/* IOAPIC register indices (written to REGSEL) */
#define IOAPIC_ID           0x00
#define IOAPIC_VER          0x01
#define IOAPIC_ARB          0x02
#define IOAPIC_REDTBL(n)    (0x10 + 2 * (n))     /* Low 32 bits */
#define IOAPIC_REDTBL_HI(n) (0x10 + 2 * (n) + 1) /* High 32 bits */

/* Redirection entry flags */
#define REDIR_MASKED        (1 << 16)
#define REDIR_LEVEL         (1 << 15)
#define REDIR_LOW_ACTIVE    (1 << 13)

/* Default vector base for IOAPIC IRQs */
#define IOAPIC_VECTOR_BASE  32

static volatile uint32_t *ioapic_base;
static uint32_t ioapic_max_entries;

/* ---- Register Access ---- */

static uint32_t ioapic_read(uint32_t reg)
{
    ioapic_base[IOAPIC_REGSEL / 4] = reg;
    return ioapic_base[IOAPIC_DATAWIN / 4];
}

static void ioapic_write(uint32_t reg, uint32_t value)
{
    ioapic_base[IOAPIC_REGSEL / 4] = reg;
    ioapic_base[IOAPIC_DATAWIN / 4] = value;
}

/* ---- Public Interface ---- */

extern uint32_t acpi_get_ioapic_address(void);
extern uint32_t acpi_get_iso_count(void);
extern bool acpi_get_iso(uint32_t index, uint8_t *source_irq, uint32_t *global_irq, uint16_t *flags);

void ioapic_init(void)
{
    uint32_t addr = acpi_get_ioapic_address();
    ioapic_base = (volatile uint32_t *)(uintptr_t)addr;

    uint32_t ver = ioapic_read(IOAPIC_VER);
    ioapic_max_entries = ((ver >> 16) & 0xFF) + 1;
    uint32_t id = (ioapic_read(IOAPIC_ID) >> 24) & 0xF;

    uart_printf("[IOAPIC] Initialized at 0x%x (ID=%u, %u entries)\n",
                addr, id, ioapic_max_entries);

    /* Mask all entries initially */
    for (uint32_t i = 0; i < ioapic_max_entries; i++) {
        ioapic_write(IOAPIC_REDTBL(i), REDIR_MASKED | (IOAPIC_VECTOR_BASE + i));
        ioapic_write(IOAPIC_REDTBL_HI(i), 0);  /* Destination: CPU 0 */
    }

    /* Apply ACPI Interrupt Source Overrides */
    uint32_t iso_count = acpi_get_iso_count();
    for (uint32_t i = 0; i < iso_count; i++) {
        uint8_t source_irq;
        uint32_t global_irq;
        uint16_t flags;

        if (acpi_get_iso(i, &source_irq, &global_irq, &flags)) {
            if (global_irq < ioapic_max_entries) {
                uint32_t low = REDIR_MASKED | (IOAPIC_VECTOR_BASE + source_irq);

                /* Apply polarity from ISO flags (bits 0-1) */
                if ((flags & 0x03) == 0x03)  /* Active low */
                    low |= REDIR_LOW_ACTIVE;

                /* Apply trigger mode from ISO flags (bits 2-3) */
                if ((flags & 0x0C) == 0x0C)  /* Level triggered */
                    low |= REDIR_LEVEL;

                ioapic_write(IOAPIC_REDTBL(global_irq), low);
                ioapic_write(IOAPIC_REDTBL_HI(global_irq), 0);
            }
        }
    }
}

void ioapic_enable_irq(uint32_t irq)
{
    if (irq < IOAPIC_VECTOR_BASE) return;
    uint32_t pin = irq - IOAPIC_VECTOR_BASE;

    /* Check ISOs: if this ISA IRQ is redirected to a different pin, use that */
    uint32_t actual_pin = pin;
    uint32_t iso_count = acpi_get_iso_count();
    for (uint32_t i = 0; i < iso_count; i++) {
        uint8_t source;
        uint32_t global;
        if (acpi_get_iso(i, &source, &global, NULL) && source == pin) {
            actual_pin = global;
            break;
        }
    }

    if (actual_pin >= ioapic_max_entries) return;

    uint32_t low = ioapic_read(IOAPIC_REDTBL(actual_pin));
    low &= ~REDIR_MASKED;
    ioapic_write(IOAPIC_REDTBL(actual_pin), low);
}

void ioapic_disable_irq(uint32_t irq)
{
    if (irq < IOAPIC_VECTOR_BASE) return;
    uint32_t pin = irq - IOAPIC_VECTOR_BASE;

    uint32_t actual_pin = pin;
    uint32_t iso_count = acpi_get_iso_count();
    for (uint32_t i = 0; i < iso_count; i++) {
        uint8_t source;
        uint32_t global;
        if (acpi_get_iso(i, &source, &global, NULL) && source == pin) {
            actual_pin = global;
            break;
        }
    }

    if (actual_pin >= ioapic_max_entries) return;

    uint32_t low = ioapic_read(IOAPIC_REDTBL(actual_pin));
    low |= REDIR_MASKED;
    ioapic_write(IOAPIC_REDTBL(actual_pin), low);
}

#endif /* PLATFORM_X86_64 */
