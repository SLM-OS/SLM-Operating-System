/*
 * efi.h - Minimal EFI type definitions for bare-metal boot
 *
 * Only the types and constants needed to call ExitBootServices and
 * find the DTB in the EFI configuration table. Not a full UEFI API.
 *
 * References:
 * - UEFI Specification v2.10, Chapter 4 (EFI System Table)
 * - EDK2 MdePkg/Include/Uefi/UefiSpec.h
 */

#ifndef EFI_H
#define EFI_H

#include <stdint.h>
#include <stdbool.h>

/* EFI status codes */
typedef uint64_t efi_status_t;
#define EFI_SUCCESS             0
#define EFI_BUFFER_TOO_SMALL    5
#define EFI_INVALID_PARAMETER   2

/* EFI handles and pointers */
typedef void *efi_handle_t;
typedef uint64_t efi_phys_addr_t;

/* EFI memory types (for AllocatePool) */
#define EFI_LOADER_DATA         2

/* EFI memory descriptor types (from GetMemoryMap) */
#define EFI_CONVENTIONAL_MEMORY 7

/* EFI_GUID — 16-byte globally unique identifier */
typedef struct {
    uint32_t data1;
    uint16_t data2;
    uint16_t data3;
    uint8_t  data4[8];
} efi_guid_t;

/* FDT (Flattened Device Tree) GUID for configuration table lookup */
#define EFI_FDT_GUID \
    { 0xb1b621d5, 0xf19c, 0x41a5, \
      { 0x83, 0x0b, 0xd9, 0x15, 0x2c, 0x69, 0xaa, 0xe0 } }

/* EFI System Table signature: "IBI SYST" in little-endian */
#define EFI_SYSTEM_TABLE_SIGNATURE  0x5453595320494249ULL

/*
 * EFI_TABLE_HEADER — common header for all EFI tables
 */
typedef struct {
    uint64_t signature;
    uint32_t revision;
    uint32_t header_size;
    uint32_t crc32;
    uint32_t reserved;
} efi_table_header_t;

/*
 * EFI_CONFIGURATION_TABLE — entry in the configuration table array
 */
typedef struct {
    efi_guid_t vendor_guid;     /* +0x00, 16 bytes */
    void      *vendor_table;    /* +0x10, 8 bytes */
} efi_config_table_t;

/*
 * EFI_MEMORY_DESCRIPTOR — describes a region of physical memory
 */
typedef struct {
    uint32_t          type;
    uint32_t          pad;
    efi_phys_addr_t   phys_start;
    uint64_t          virt_start;
    uint64_t          num_pages;
    uint64_t          attribute;
} efi_memory_desc_t;

/*
 * EFI_BOOT_SERVICES — minimal subset of boot services function pointers
 *
 * Only the offsets we need are defined. UEFI calling convention on ARM64
 * follows AAPCS64 (same as C), so we can call these as normal C functions.
 */
typedef struct {
    efi_table_header_t hdr;                 /* +0x00 */
    void *raise_tpl;                        /* +0x18 */
    void *restore_tpl;                      /* +0x20 */
    void *allocate_pages;                   /* +0x28 */
    void *free_pages;                       /* +0x30 */

    efi_status_t (*get_memory_map)(         /* +0x38 */
        uint64_t           *map_size,
        efi_memory_desc_t  *map,
        uint64_t           *map_key,
        uint64_t           *desc_size,
        uint32_t           *desc_version
    );

    efi_status_t (*allocate_pool)(          /* +0x40 */
        uint32_t   pool_type,
        uint64_t   size,
        void     **buffer
    );

    efi_status_t (*free_pool)(              /* +0x48 */
        void *buffer
    );

    void *_pad1[9];                         /* +0x50 to +0x90 */
    void *_pad2[7];                         /* +0x98 to +0xC8 */
    void *_pad3[3];                         /* +0xD0 to +0xE0 */

    efi_status_t (*exit_boot_services)(     /* +0xE8 */
        efi_handle_t  handle,
        uint64_t      map_key
    );
} efi_boot_services_t;

/*
 * EFI_SYSTEM_TABLE — main EFI table passed at entry
 *
 * Layout verified against EDK2 UefiSpec.h and Linux efi.h for ARM64.
 */
typedef struct {
    efi_table_header_t   hdr;               /* +0x00 */
    uint16_t            *fw_vendor;          /* +0x18 */
    uint32_t             fw_revision;        /* +0x20 */
    uint32_t             _pad1;              /* +0x24 */
    efi_handle_t         con_in_handle;      /* +0x28 */
    void                *con_in;             /* +0x30 */
    efi_handle_t         con_out_handle;     /* +0x38 */
    void                *con_out;            /* +0x40 */
    efi_handle_t         stderr_handle;      /* +0x48 */
    void                *std_err;            /* +0x50 */
    void                *runtime_services;   /* +0x58 */
    efi_boot_services_t *boot_services;      /* +0x60 */
    uint64_t             nr_tables;          /* +0x68 */
    efi_config_table_t  *config_table;       /* +0x70 */
} efi_system_table_t;

/*
 * Compare two EFI GUIDs for equality.
 */
static inline bool efi_guid_eq(const efi_guid_t *a, const efi_guid_t *b)
{
    const uint64_t *pa = (const uint64_t *)a;
    const uint64_t *pb = (const uint64_t *)b;
    return pa[0] == pb[0] && pa[1] == pb[1];
}

#endif /* EFI_H */
