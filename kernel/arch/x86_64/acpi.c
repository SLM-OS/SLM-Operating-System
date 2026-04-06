/*
 * acpi.c - ACPI table parsing for x86-64
 *
 * Finds the RSDP (Root System Description Pointer) via Multiboot2 tag
 * or BIOS memory scan, then parses the MADT (Multiple APIC Description
 * Table) to discover CPUs, LAPIC base, and IOAPIC configuration.
 */

#include "platform.h"

#if defined(PLATFORM_X86_64)

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "uart.h"

/* ---- ACPI Table Structures ---- */

struct acpi_rsdp {
    char     signature[8];      /* "RSD PTR " */
    uint8_t  checksum;
    char     oem_id[6];
    uint8_t  revision;          /* 0 = ACPI 1.0, 2 = ACPI 2.0+ */
    uint32_t rsdt_address;
    /* ACPI 2.0+ fields */
    uint32_t length;
    uint64_t xsdt_address;
    uint8_t  extended_checksum;
    uint8_t  reserved[3];
} __attribute__((packed));

struct acpi_sdt_header {
    char     signature[4];
    uint32_t length;
    uint8_t  revision;
    uint8_t  checksum;
    char     oem_id[6];
    char     oem_table_id[8];
    uint32_t oem_revision;
    uint32_t creator_id;
    uint32_t creator_revision;
} __attribute__((packed));

/* MADT entry types */
#define MADT_ENTRY_LAPIC        0
#define MADT_ENTRY_IOAPIC       1
#define MADT_ENTRY_ISO          2   /* Interrupt Source Override */
#define MADT_ENTRY_NMI          3
#define MADT_ENTRY_LAPIC_NMI    4
#define MADT_ENTRY_LAPIC_ADDR   5   /* LAPIC address override */

struct madt_header {
    struct acpi_sdt_header header;
    uint32_t lapic_address;
    uint32_t flags;             /* bit 0: dual 8259 PICs present */
} __attribute__((packed));

struct madt_entry_header {
    uint8_t type;
    uint8_t length;
} __attribute__((packed));

struct madt_lapic {
    struct madt_entry_header header;
    uint8_t  acpi_processor_id;
    uint8_t  apic_id;
    uint32_t flags;             /* bit 0: enabled, bit 1: online capable */
} __attribute__((packed));

struct madt_ioapic {
    struct madt_entry_header header;
    uint8_t  ioapic_id;
    uint8_t  reserved;
    uint32_t ioapic_address;
    uint32_t global_irq_base;
} __attribute__((packed));

struct madt_iso {
    struct madt_entry_header header;
    uint8_t  bus;               /* 0 = ISA */
    uint8_t  source;            /* ISA IRQ number */
    uint32_t global_irq;        /* IOAPIC pin */
    uint16_t flags;             /* polarity + trigger mode */
} __attribute__((packed));

/* ---- ACPI Discovery Results ---- */

#define ACPI_MAX_CPUS       16
#define ACPI_MAX_ISOS       16

struct acpi_cpu_info {
    uint8_t  apic_id;
    uint8_t  acpi_id;
    bool     enabled;
};

struct acpi_iso_info {
    uint8_t  source_irq;        /* ISA IRQ */
    uint32_t global_irq;        /* IOAPIC pin */
    uint16_t flags;
};

/* Saved RSDP pointer for acpi_find_table() */
static const struct acpi_rsdp *saved_rsdp;

static struct {
    bool     valid;
    uint32_t lapic_address;
    uint32_t ioapic_address;
    uint8_t  ioapic_id;
    uint32_t ioapic_gsi_base;

    struct acpi_cpu_info cpus[ACPI_MAX_CPUS];
    uint32_t cpu_count;

    struct acpi_iso_info isos[ACPI_MAX_ISOS];
    uint32_t iso_count;
} acpi_info;

/* ---- Checksum Validation ---- */

static bool acpi_checksum_valid(const void *data, size_t len)
{
    const uint8_t *bytes = data;
    uint8_t sum = 0;
    for (size_t i = 0; i < len; i++)
        sum += bytes[i];
    return sum == 0;
}

/* ---- RSDP Discovery ---- */

static const struct acpi_rsdp *find_rsdp_in_multiboot2(void)
{
    extern uint32_t multiboot_info_addr;
    if (multiboot_info_addr == 0) return NULL;

    uint8_t *ptr = (uint8_t *)(uintptr_t)multiboot_info_addr;
    uint32_t total_size = *(uint32_t *)ptr;
    uint8_t *end = ptr + total_size;

    ptr += 8;
    while (ptr < end) {
        uint32_t tag_type = *(uint32_t *)ptr;
        uint32_t tag_size = *(uint32_t *)(ptr + 4);
        if (tag_type == 0) break;

        /* Tag type 14 = ACPI old RSDP, type 15 = ACPI new RSDP */
        if (tag_type == 14 || tag_type == 15) {
            return (const struct acpi_rsdp *)(ptr + 8);
        }

        ptr += (tag_size + 7) & ~7;
    }
    return NULL;
}

static const struct acpi_rsdp *find_rsdp_by_scan(void)
{
    /* Scan BIOS ROM area: 0xE0000 - 0xFFFFF */
    for (uintptr_t addr = 0xE0000; addr < 0x100000; addr += 16) {
        const char *p = (const char *)addr;
        if (p[0] == 'R' && p[1] == 'S' && p[2] == 'D' && p[3] == ' ' &&
            p[4] == 'P' && p[5] == 'T' && p[6] == 'R' && p[7] == ' ') {
            if (acpi_checksum_valid(p, 20))
                return (const struct acpi_rsdp *)p;
        }
    }
    return NULL;
}

/* ---- MADT Parsing ---- */

static bool sig_match(const char *a, const char *b, int len)
{
    for (int i = 0; i < len; i++)
        if (a[i] != b[i]) return false;
    return true;
}

static void parse_madt(const struct madt_header *madt)
{
    acpi_info.lapic_address = madt->lapic_address;

    const uint8_t *ptr = (const uint8_t *)madt + sizeof(struct madt_header);
    const uint8_t *end = (const uint8_t *)madt + madt->header.length;

    while (ptr < end) {
        const struct madt_entry_header *entry = (const struct madt_entry_header *)ptr;
        if (entry->length == 0) break;

        switch (entry->type) {
        case MADT_ENTRY_LAPIC: {
            const struct madt_lapic *lapic = (const struct madt_lapic *)entry;
            if (acpi_info.cpu_count < ACPI_MAX_CPUS) {
                acpi_info.cpus[acpi_info.cpu_count].apic_id = lapic->apic_id;
                acpi_info.cpus[acpi_info.cpu_count].acpi_id = lapic->acpi_processor_id;
                acpi_info.cpus[acpi_info.cpu_count].enabled = (lapic->flags & 1) != 0;
                acpi_info.cpu_count++;
            }
            break;
        }
        case MADT_ENTRY_IOAPIC: {
            const struct madt_ioapic *ioapic = (const struct madt_ioapic *)entry;
            acpi_info.ioapic_address = ioapic->ioapic_address;
            acpi_info.ioapic_id = ioapic->ioapic_id;
            acpi_info.ioapic_gsi_base = ioapic->global_irq_base;
            break;
        }
        case MADT_ENTRY_ISO: {
            const struct madt_iso *iso = (const struct madt_iso *)entry;
            if (acpi_info.iso_count < ACPI_MAX_ISOS) {
                acpi_info.isos[acpi_info.iso_count].source_irq = iso->source;
                acpi_info.isos[acpi_info.iso_count].global_irq = iso->global_irq;
                acpi_info.isos[acpi_info.iso_count].flags = iso->flags;
                acpi_info.iso_count++;
            }
            break;
        }
        case MADT_ENTRY_LAPIC_ADDR: {
            /* 64-bit LAPIC address override */
            if (entry->length >= 12) {
                uint64_t addr = *(const uint64_t *)(ptr + 4);
                acpi_info.lapic_address = (uint32_t)addr;
            }
            break;
        }
        default:
            break;
        }

        ptr += entry->length;
    }
}

/* ---- Public Interface ---- */

int acpi_init(void)
{
    /* Find RSDP */
    const struct acpi_rsdp *rsdp = find_rsdp_in_multiboot2();
    if (!rsdp) {
        rsdp = find_rsdp_by_scan();
    }
    if (!rsdp) {
        uart_printf("[ACPI] RSDP not found\n");
        return -1;
    }

    saved_rsdp = rsdp;

    uart_printf("[ACPI] RSDP found at 0x%lx (revision %u)\n",
                (uintptr_t)rsdp, rsdp->revision);

    /* Find MADT in RSDT or XSDT */
    const struct madt_header *madt = NULL;

    if (rsdp->revision >= 2 && rsdp->xsdt_address != 0) {
        /* Use XSDT (64-bit pointers) */
        const struct acpi_sdt_header *xsdt =
            (const struct acpi_sdt_header *)(uintptr_t)rsdp->xsdt_address;
        uint32_t entries = (xsdt->length - sizeof(*xsdt)) / 8;
        const uint64_t *ptrs = (const uint64_t *)((uintptr_t)xsdt + sizeof(*xsdt));

        for (uint32_t i = 0; i < entries; i++) {
            const struct acpi_sdt_header *hdr =
                (const struct acpi_sdt_header *)(uintptr_t)ptrs[i];
            if (sig_match(hdr->signature, "APIC", 4)) {
                madt = (const struct madt_header *)hdr;
                break;
            }
        }
    } else {
        /* Use RSDT (32-bit pointers) */
        const struct acpi_sdt_header *rsdt =
            (const struct acpi_sdt_header *)(uintptr_t)rsdp->rsdt_address;
        uint32_t entries = (rsdt->length - sizeof(*rsdt)) / 4;
        const uint32_t *ptrs = (const uint32_t *)((uintptr_t)rsdt + sizeof(*rsdt));

        for (uint32_t i = 0; i < entries; i++) {
            const struct acpi_sdt_header *hdr =
                (const struct acpi_sdt_header *)(uintptr_t)ptrs[i];
            if (sig_match(hdr->signature, "APIC", 4)) {
                madt = (const struct madt_header *)hdr;
                break;
            }
        }
    }

    if (!madt) {
        uart_printf("[ACPI] MADT not found\n");
        return -1;
    }

    /* Parse MADT */
    parse_madt(madt);
    acpi_info.valid = true;

    /* Count enabled CPUs */
    uint32_t enabled = 0;
    for (uint32_t i = 0; i < acpi_info.cpu_count; i++)
        if (acpi_info.cpus[i].enabled) enabled++;

    uart_printf("[ACPI] MADT: %u CPUs (%u enabled), LAPIC=0x%x, IOAPIC=0x%x\n",
                acpi_info.cpu_count, enabled,
                acpi_info.lapic_address, acpi_info.ioapic_address);

    for (uint32_t i = 0; i < acpi_info.iso_count; i++) {
        uart_printf("[ACPI]   IRQ override: ISA %u -> GSI %u (flags 0x%x)\n",
                    acpi_info.isos[i].source_irq,
                    acpi_info.isos[i].global_irq,
                    acpi_info.isos[i].flags);
    }

    return 0;
}

/* Accessors for other modules */

uint32_t acpi_get_lapic_address(void)
{
    return acpi_info.valid ? acpi_info.lapic_address : 0xFEE00000;
}

uint32_t acpi_get_ioapic_address(void)
{
    return acpi_info.valid ? acpi_info.ioapic_address : 0xFEC00000;
}

uint32_t acpi_get_enabled_cpu_count(void)
{
    if (!acpi_info.valid) return 1;
    uint32_t count = 0;
    for (uint32_t i = 0; i < acpi_info.cpu_count; i++)
        if (acpi_info.cpus[i].enabled) count++;
    return count > 0 ? count : 1;
}

uint8_t acpi_get_cpu_apic_id(uint32_t logical_id)
{
    if (!acpi_info.valid || logical_id >= acpi_info.cpu_count)
        return 0;
    /* Return the nth enabled CPU's APIC ID */
    uint32_t n = 0;
    for (uint32_t i = 0; i < acpi_info.cpu_count; i++) {
        if (acpi_info.cpus[i].enabled) {
            if (n == logical_id)
                return acpi_info.cpus[i].apic_id;
            n++;
        }
    }
    return 0;
}

uint32_t acpi_get_iso_count(void)
{
    return acpi_info.iso_count;
}

bool acpi_get_iso(uint32_t index, uint8_t *source_irq, uint32_t *global_irq, uint16_t *flags)
{
    if (index >= acpi_info.iso_count) return false;
    if (source_irq) *source_irq = acpi_info.isos[index].source_irq;
    if (global_irq) *global_irq = acpi_info.isos[index].global_irq;
    if (flags) *flags = acpi_info.isos[index].flags;
    return true;
}

/*
 * Find an ACPI table by its 4-byte signature (e.g., "MCFG", "HPET").
 * Must be called after acpi_init().
 * Returns pointer to the SDT header, or NULL if not found.
 */
const void *acpi_find_table(const char *sig)
{
    if (!saved_rsdp) return NULL;

    if (saved_rsdp->revision >= 2 && saved_rsdp->xsdt_address != 0) {
        const struct acpi_sdt_header *xsdt =
            (const struct acpi_sdt_header *)(uintptr_t)saved_rsdp->xsdt_address;
        uint32_t entries = (xsdt->length - sizeof(*xsdt)) / 8;
        const uint64_t *ptrs = (const uint64_t *)((uintptr_t)xsdt + sizeof(*xsdt));
        for (uint32_t i = 0; i < entries; i++) {
            const struct acpi_sdt_header *hdr =
                (const struct acpi_sdt_header *)(uintptr_t)ptrs[i];
            if (sig_match(hdr->signature, sig, 4))
                return hdr;
        }
    } else {
        const struct acpi_sdt_header *rsdt =
            (const struct acpi_sdt_header *)(uintptr_t)saved_rsdp->rsdt_address;
        uint32_t entries = (rsdt->length - sizeof(*rsdt)) / 4;
        const uint32_t *ptrs = (const uint32_t *)((uintptr_t)rsdt + sizeof(*rsdt));
        for (uint32_t i = 0; i < entries; i++) {
            const struct acpi_sdt_header *hdr =
                (const struct acpi_sdt_header *)(uintptr_t)ptrs[i];
            if (sig_match(hdr->signature, sig, 4))
                return hdr;
        }
    }

    return NULL;
}

#endif /* PLATFORM_X86_64 */
