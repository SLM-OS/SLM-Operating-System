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

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

/* EFI status codes */
typedef uint64_t efi_status_t;
#define EFI_SUCCESS             0
#define EFI_BUFFER_TOO_SMALL    5
#define EFI_INVALID_PARAMETER   2
#define EFI_ERROR_BIT           (1ULL << 63)
#define EFI_LOAD_ERROR          (EFI_ERROR_BIT | 1)
#define EFI_DEVICE_ERROR        (EFI_ERROR_BIT | 7)
#define EFI_OUT_OF_RESOURCES    (EFI_ERROR_BIT | 9)
#define EFI_NOT_FOUND           (EFI_ERROR_BIT | 14)

/* EFI_GUID — 16-byte globally unique identifier */
typedef struct {
    uint32_t data1;
    uint16_t data2;
    uint16_t data3;
    uint8_t  data4[8];
} efi_guid_t;

/* EFI handles and pointers */
typedef void *efi_handle_t;
typedef uint64_t efi_phys_addr_t;

#define EFI_PAGE_SIZE           4096ULL

/*
 * EFI_MEMORY_TYPE values (UEFI spec v2.10 §7.2, Table 7-5). Used both
 * as the AllocatePool pool type and as the descriptor type returned by
 * GetMemoryMap.
 */
#define EFI_RESERVED_MEMORY_TYPE    0
#define EFI_LOADER_CODE             1
#define EFI_LOADER_DATA             2
#define EFI_BOOT_SERVICES_CODE      3
#define EFI_BOOT_SERVICES_DATA      4
#define EFI_RUNTIME_SERVICES_CODE   5
#define EFI_RUNTIME_SERVICES_DATA   6
#define EFI_CONVENTIONAL_MEMORY     7
#define EFI_UNUSABLE_MEMORY         8
#define EFI_ACPI_RECLAIM_MEMORY     9
#define EFI_ACPI_MEMORY_NVS         10
#define EFI_MEMORY_MAPPED_IO        11
#define EFI_MEMORY_MAPPED_IO_PORT   12
#define EFI_PAL_CODE                13
#define EFI_PERSISTENT_MEMORY       14

/*
 * True for memory types that belong to the OS once ExitBootServices
 * succeeds (UEFI spec v2.10 §7.4.6: "the OS loader owns all available
 * memory in the system" — LoaderCode/Data, BootServicesCode/Data and
 * ConventionalMemory). Everything else (runtime services, ACPI, MMIO,
 * reserved) must survive the hand-off untouched.
 */
static inline bool efi_mem_type_free_after_ebs(uint32_t type)
{
    switch (type) {
    case EFI_LOADER_CODE:
    case EFI_LOADER_DATA:
    case EFI_BOOT_SERVICES_CODE:
    case EFI_BOOT_SERVICES_DATA:
    case EFI_CONVENTIONAL_MEMORY:
        return true;
    default:
        return false;
    }
}

/*
 * Return value efi_stub_entry() uses to tell boot.S "abort before
 * ExitBootServices and return to the firmware". NULL is a legitimate
 * "no DTB found" result, so the abort marker must be distinct from it
 * and can never be a real (page-aligned, in-RAM) DTB address.
 */
#define EFI_STUB_ABORT          ((void *)~0UL)

/* Outcome of efi_check_relocation(). */
typedef enum {
    EFI_RELOC_OK = 0,          /* dst is free after EBS — copy there */
    EFI_RELOC_NOT_NEEDED,      /* already loaded at the link address */
    EFI_RELOC_OVERLAP,         /* src and dst overlap (unsupported) */
    EFI_RELOC_FDT_IN_DEST,     /* the DTB lives inside dst */
    EFI_RELOC_DEST_BUSY,       /* dst holds firmware-owned memory */
    EFI_RELOC_DEST_NOT_RAM,    /* part of dst is not in the map */
    EFI_RELOC_BAD_MAP,         /* descriptor size smaller than spec */
} efi_reloc_verdict_t;

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

    efi_status_t (*allocate_pages)(         /* +0x28 */
        uint32_t         alloc_type,        /* EFI_ALLOCATE_* */
        uint32_t         memory_type,
        uint64_t         num_pages,
        efi_phys_addr_t *memory             /* IN (address) / OUT */
    );

    efi_status_t (*free_pages)(             /* +0x30 */
        efi_phys_addr_t memory,
        uint64_t        num_pages
    );

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

    efi_status_t (*handle_protocol)(        /* +0x98 */
        efi_handle_t      handle,
        const efi_guid_t *protocol,
        void            **interface
    );

    void *_pad2[6];                         /* +0xA0 to +0xC8 */
    void *_pad3[3];                         /* +0xD0 to +0xE0 */

    efi_status_t (*exit_boot_services)(     /* +0xE8 */
        efi_handle_t  handle,
        uint64_t      map_key
    );

    void *_pad4[3];                         /* +0xF0 to +0x100 */

    efi_status_t (*connect_controller)(     /* +0x108 */
        efi_handle_t  controller,
        efi_handle_t *driver_image,         /* NULL: all drivers */
        void         *remaining_path,       /* NULL: all children */
        bool          recursive
    );

    void *_pad5[5];                         /* +0x110 to +0x130 */

    efi_status_t (*locate_handle_buffer)(   /* +0x138 */
        uint32_t          search_type,      /* EFI_LOCATE_* */
        const efi_guid_t *protocol,
        void             *search_key,
        uint64_t         *num_handles,
        efi_handle_t    **buffer            /* FreePool'd by the caller */
    );
} efi_boot_services_t;

/* EFI_ALLOCATE_TYPE and EFI_LOCATE_SEARCH_TYPE values used by the stub. */
#define EFI_ALLOCATE_ADDRESS    2
#define EFI_LOCATE_BY_PROTOCOL  2

/* Protocol GUIDs the model loader needs (UEFI spec v2.10 ch. 10, 13). */
#define EFI_SIMPLE_FILE_SYSTEM_GUID \
    { 0x964e5b22, 0x6459, 0x11d2, \
      { 0x8e, 0x39, 0x00, 0xa0, 0xc9, 0x69, 0x72, 0x3b } }
#define EFI_BLOCK_IO_GUID \
    { 0x964e5b21, 0x6459, 0x11d2, \
      { 0x8e, 0x39, 0x00, 0xa0, 0xc9, 0x69, 0x72, 0x3b } }
#define EFI_DEVICE_PATH_GUID \
    { 0x09576e91, 0x6d3f, 0x11d2, \
      { 0x8e, 0x39, 0x00, 0xa0, 0xc9, 0x69, 0x72, 0x3b } }

/* EFI_DEVICE_PATH_PROTOCOL node header (UEFI spec v2.10 §10.2). */
typedef struct {
    uint8_t type;
    uint8_t subtype;
    uint8_t length[2];                      /* little-endian, >= 4 */
} efi_device_path_t;

#define EFI_DP_TYPE_MESSAGING   0x03
#define EFI_DP_TYPE_END         0x7F
#define EFI_DP_MSG_NVME         23          /* NVMe namespace */

/* EFI_FILE_PROTOCOL (UEFI spec v2.10 §13.5). */
#define EFI_FILE_MODE_READ      1ULL
#define EFI_FILE_ATTR_DIRECTORY 0x10ULL

typedef struct efi_file_protocol {
    uint64_t revision;                      /* +0x00 */
    efi_status_t (*open)(                   /* +0x08 */
        struct efi_file_protocol  *self,
        struct efi_file_protocol **new_handle,
        const uint16_t            *file_name,
        uint64_t                   open_mode,
        uint64_t                   attributes
    );
    efi_status_t (*close)(                  /* +0x10 */
        struct efi_file_protocol *self
    );
    void *_delete;                          /* +0x18 */
    efi_status_t (*read)(                   /* +0x20 */
        struct efi_file_protocol *self,
        uint64_t                 *buffer_size,  /* IN/OUT */
        void                     *buffer
    );
    void *_write;                           /* +0x28 */
    void *_get_position;                    /* +0x30 */
    efi_status_t (*set_position)(           /* +0x38 */
        struct efi_file_protocol *self,
        uint64_t                  position
    );
    void *_get_info;                        /* +0x40 */
    void *_set_info;                        /* +0x48 */
    void *_flush;                           /* +0x50 */
} efi_file_protocol_t;

/* EFI_SIMPLE_FILE_SYSTEM_PROTOCOL (UEFI spec v2.10 §13.4). */
typedef struct efi_simple_file_system_protocol {
    uint64_t revision;                      /* +0x00 */
    efi_status_t (*open_volume)(            /* +0x08 */
        struct efi_simple_file_system_protocol *self,
        efi_file_protocol_t                   **root
    );
} efi_simple_file_system_protocol_t;

/*
 * EFI_FILE_INFO, as returned by reading a directory handle. FileName
 * (UTF-16, NUL-terminated) starts at offset 0x50. The three EFI_TIME
 * fields are not used; they are kept as opaque bytes for the layout.
 */
typedef struct {
    uint64_t size;                          /* +0x00 of this record */
    uint64_t file_size;                     /* +0x08 */
    uint64_t physical_size;                 /* +0x10 */
    uint8_t  times[48];                     /* +0x18 create/access/modify */
    uint64_t attribute;                     /* +0x48 */
    uint16_t file_name[];                   /* +0x50 */
} efi_file_info_t;

/*
 * EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL — minimal vtable for `con_out`
 *
 * UEFI strings are UTF-16 (CHAR16). Only `output_string` is used by the
 * EFI stub; the other fields are padded to preserve the vtable layout
 * so the function-pointer offsets match what UEFI publishes.
 *
 * Reference: UEFI Spec v2.10 §12.4 EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL.
 */
typedef uint16_t efi_char16_t;
struct efi_simple_text_output_protocol;   /* forward decl for the typedef */

typedef efi_status_t (*efi_text_string_fn)(
    struct efi_simple_text_output_protocol *self,
    efi_char16_t *string
);

typedef struct efi_simple_text_output_protocol {
    void                 *reset;             /* +0x00 */
    efi_text_string_fn    output_string;     /* +0x08 */
    void                 *test_string;       /* +0x10 */
    void                 *query_mode;        /* +0x18 */
    void                 *set_mode;          /* +0x20 */
    void                 *set_attribute;     /* +0x28 */
    void                 *clear_screen;      /* +0x30 */
    void                 *set_cursor_pos;    /* +0x38 */
    void                 *enable_cursor;     /* +0x40 */
    void                 *mode;              /* +0x48 */
} efi_simple_text_output_protocol_t;

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
    efi_simple_text_output_protocol_t *con_out; /* +0x40 */
    efi_handle_t         stderr_handle;      /* +0x48 */
    void                *std_err;            /* +0x50 */
    void                *runtime_services;   /* +0x58 */
    efi_boot_services_t *boot_services;      /* +0x60 */
    uint64_t             nr_tables;          /* +0x68 */
    efi_config_table_t  *config_table;       /* +0x70 */
} efi_system_table_t;

/*
 * Compile-time layout tests — run on every kernel build for every
 * platform (including QEMU ARM64 in `make test`), so an accidental
 * field reorder in one of these structs breaks the build rather than
 * silently mis-indexing UEFI memory at run time. The offsets are the
 * UEFI spec v2.10 values that EDK2 publishes; the EFI stub and boot.S
 * rely on them. Kept here at the header level so every translation
 * unit that includes efi.h enforces them.
 */
static_assert(offsetof(efi_system_table_t, con_out) == 0x40,
              "UEFI spec: EFI_SYSTEM_TABLE.ConOut at offset 0x40");
static_assert(offsetof(efi_system_table_t, boot_services) == 0x60,
              "UEFI spec: EFI_SYSTEM_TABLE.BootServices at offset 0x60");
static_assert(offsetof(efi_system_table_t, nr_tables) == 0x68,
              "UEFI spec: EFI_SYSTEM_TABLE.NumberOfTableEntries at 0x68");
static_assert(offsetof(efi_system_table_t, config_table) == 0x70,
              "UEFI spec: EFI_SYSTEM_TABLE.ConfigurationTable at 0x70");

static_assert(offsetof(efi_boot_services_t, allocate_pages) == 0x28,
              "UEFI spec: AllocatePages at 0x28 in BootServices");
static_assert(offsetof(efi_boot_services_t, free_pages) == 0x30,
              "UEFI spec: FreePages at 0x30 in BootServices");
static_assert(offsetof(efi_boot_services_t, handle_protocol) == 0x98,
              "UEFI spec: HandleProtocol at 0x98 in BootServices");
static_assert(offsetof(efi_boot_services_t, connect_controller) == 0x108,
              "UEFI spec: ConnectController at 0x108 in BootServices");
static_assert(offsetof(efi_boot_services_t, locate_handle_buffer) == 0x138,
              "UEFI spec: LocateHandleBuffer at 0x138 in BootServices");
static_assert(offsetof(efi_file_protocol_t, open) == 0x08,
              "UEFI spec: EFI_FILE_PROTOCOL.Open at 0x08");
static_assert(offsetof(efi_file_protocol_t, close) == 0x10,
              "UEFI spec: EFI_FILE_PROTOCOL.Close at 0x10");
static_assert(offsetof(efi_file_protocol_t, read) == 0x20,
              "UEFI spec: EFI_FILE_PROTOCOL.Read at 0x20");
static_assert(offsetof(efi_file_protocol_t, set_position) == 0x38,
              "UEFI spec: EFI_FILE_PROTOCOL.SetPosition at 0x38");
static_assert(offsetof(efi_simple_file_system_protocol_t, open_volume) == 0x08,
              "UEFI spec: OpenVolume at 0x08");
static_assert(offsetof(efi_file_info_t, attribute) == 0x48,
              "UEFI spec: EFI_FILE_INFO.Attribute at 0x48");
static_assert(offsetof(efi_file_info_t, file_name) == 0x50,
              "UEFI spec: EFI_FILE_INFO.FileName at 0x50");

static_assert(offsetof(efi_boot_services_t, get_memory_map) == 0x38,
              "UEFI spec: GetMemoryMap at 0x38 in BootServices");
static_assert(offsetof(efi_boot_services_t, allocate_pool) == 0x40,
              "UEFI spec: AllocatePool at 0x40 in BootServices");
static_assert(offsetof(efi_boot_services_t, free_pool) == 0x48,
              "UEFI spec: FreePool at 0x48 in BootServices");
static_assert(offsetof(efi_boot_services_t, exit_boot_services) == 0xE8,
              "UEFI spec: ExitBootServices at 0xE8 in BootServices");

static_assert(offsetof(efi_simple_text_output_protocol_t, reset) == 0x00,
              "UEFI spec: SimpleTextOutput.Reset at 0x00");
static_assert(offsetof(efi_simple_text_output_protocol_t, output_string)
                  == 0x08,
              "UEFI spec: SimpleTextOutput.OutputString at 0x08");
static_assert(offsetof(efi_simple_text_output_protocol_t, test_string)
                  == 0x10,
              "UEFI spec: SimpleTextOutput.TestString at 0x10");
static_assert(offsetof(efi_simple_text_output_protocol_t, mode) == 0x48,
              "UEFI spec: SimpleTextOutput.Mode at 0x48");

/*
 * Compare two EFI GUIDs for equality.
 */
static inline bool efi_guid_eq(const efi_guid_t *a, const efi_guid_t *b)
{
    const uint64_t *pa = (const uint64_t *)a;
    const uint64_t *pb = (const uint64_t *)b;
    return pa[0] == pb[0] && pa[1] == pb[1];
}

/*
 * Forward declaration — the descriptor type is defined above. See
 * efi_stub.c for the rules. Pure function; safe to call from tests.
 */
efi_reloc_verdict_t efi_check_relocation(const efi_memory_desc_t *map,
                                         uint64_t map_size,
                                         uint64_t desc_size,
                                         uint64_t src, uint64_t dst,
                                         uint64_t size,
                                         uint64_t fdt, uint64_t fdt_size);

/*
 * Shared between efi_stub.c and efi_model.c. All usable only before
 * ExitBootServices, except efi_dcache_clean_inval. See efi_stub.c.
 */
void efi_print(efi_system_table_t *sys_table, const efi_char16_t *str);
efi_status_t efi_read_map(efi_boot_services_t *bs,
                          efi_memory_desc_t **map_out,
                          uint64_t *buf_size_out,
                          uint64_t *map_size_out,
                          uint64_t *map_key_out,
                          uint64_t *desc_size_out);
void efi_dcache_clean_inval(uint64_t start, uint64_t len);

/*
 * Model delivery (efi_model.c, docs/adr/0002-uefi-model-delivery.md).
 *
 * efi_models_load() walks every SimpleFileSystem volume for
 * \slmos\models\*.gguf and reads what it finds into physically
 * contiguous LoaderData pages inside [floor, limit), avoiding the
 * `excl` ranges. Every failure is non-fatal: the file is skipped and
 * the boot continues. The results go in `table`; counters in `stats`
 * say what was skipped and why.
 */
#define EFI_MODEL_MAX_EXCL  3

typedef struct {
    uint64_t floor;     /* lowest allowed physical address */
    uint64_t limit;     /* one past the highest allowed address */
    uint64_t excl_start[EFI_MODEL_MAX_EXCL];
    uint64_t excl_len[EFI_MODEL_MAX_EXCL];
    uint64_t chunk_bytes;   /* bytes per Read call; 0: the default */
    uint64_t budget_secs;   /* total read time limit; 0: the default */
} efi_models_cfg_t;

typedef struct {
    uint32_t volumes_seen;
    uint32_t loaded;
    uint32_t too_big;
    uint32_t bad_magic;
    uint32_t read_errors;
    uint32_t no_memory;     /* no range, or AllocatePages refused */
    uint32_t over_budget;
    uint32_t bad_name;
    uint32_t table_full;
} efi_models_stats_t;

struct boot_model_table;

/*
 * Highest 2 MiB-aligned start for `size` bytes inside one
 * ConventionalMemory descriptor, within [floor, limit) and clear of the
 * excluded ranges. Returns 0 when nothing fits. Pure, for the tests.
 */
uint64_t efi_model_pick_range(const efi_memory_desc_t *map, uint64_t map_size,
                              uint64_t desc_size, uint64_t size,
                              const efi_models_cfg_t *cfg);

void efi_models_load(efi_system_table_t *sys_table,
                     const efi_models_cfg_t *cfg,
                     struct boot_model_table *table,
                     efi_models_stats_t *stats);

/* Give every recorded model's pages back to the firmware and empty the
 * table: the boot is returning to the firmware, not continuing. */
void efi_models_release(efi_system_table_t *sys_table,
                        struct boot_model_table *table);

#endif /* EFI_H */
