/*
 * GPU Driver Interface
 *
 * Platform-agnostic GPU abstraction for SLM-OS. Provides:
 * - GPU initialization and shutdown
 * - GPU-accessible memory allocation
 * - Cache coherency for CPU/GPU data sharing
 * - Command submission (future)
 *
 * Implementations:
 * - gpu_stub.c   : No-op driver for QEMU (no GPU hardware)
 * - gpu_tegra.c  : Jetson Orin Nano driver (future)
 */

#ifndef GPU_H
#define GPU_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

/*
 * GPU Driver Capabilities
 *
 * Flags indicating what features the GPU driver supports.
 * The stub driver has no capabilities; real drivers set appropriate flags.
 */
#define GPU_CAP_NONE            0x00000000
#define GPU_CAP_COMPUTE         0x00000001  /* Can execute compute shaders */
#define GPU_CAP_TENSOR_CORES    0x00000002  /* Has tensor cores for AI */
#define GPU_CAP_DMA             0x00000004  /* Can DMA to/from system memory */
#define GPU_CAP_UNIFIED_MEMORY  0x00000008  /* CPU/GPU share physical memory */

/*
 * GPU Memory Flags
 *
 * Flags for gpu_alloc() specifying memory properties.
 */
#define GPU_MEM_READ            0x00000001  /* GPU will read this memory */
#define GPU_MEM_WRITE           0x00000002  /* GPU will write this memory */
#define GPU_MEM_READWRITE       (GPU_MEM_READ | GPU_MEM_WRITE)
#define GPU_MEM_CACHED          0x00000004  /* CPU-cacheable (needs coherency) */
#define GPU_MEM_UNCACHED        0x00000008  /* Not CPU-cached (coherent) */
#define GPU_MEM_ALIGN_2MB       0x00000010  /* 2MB alignment for huge pages */

/*
 * GPU Error Codes
 */
#define GPU_OK                  0
#define GPU_ERR_NOT_INIT       -1   /* gpu_init() not called */
#define GPU_ERR_NO_DEVICE      -2   /* No GPU hardware found */
#define GPU_ERR_NO_MEMORY      -3   /* Out of GPU-accessible memory */
#define GPU_ERR_INVALID_PARAM  -4   /* Invalid parameter */
#define GPU_ERR_NOT_SUPPORTED  -5   /* Operation not supported */
#define GPU_ERR_FIRMWARE       -6   /* Firmware load/init failed */
#define GPU_ERR_TIMEOUT        -7   /* Operation timed out */

/*
 * GPU Buffer Handle
 *
 * Opaque handle returned by gpu_alloc(). Contains both CPU virtual address
 * and GPU-visible physical address for DMA operations.
 */
typedef struct gpu_buffer {
    void     *cpu_addr;     /* CPU virtual address */
    uint64_t  gpu_addr;     /* GPU physical/IOVA address */
    size_t    size;         /* Buffer size in bytes */
    uint32_t  flags;        /* Allocation flags */
} gpu_buffer_t;

/*
 * GPU Driver Info
 *
 * Information about the GPU hardware and driver.
 */
typedef struct gpu_info {
    const char *name;           /* Driver name (e.g., "tegra-orin") */
    const char *device;         /* Device name (e.g., "NVIDIA Orin Nano") */
    uint32_t    capabilities;   /* GPU_CAP_* flags */
    uint32_t    cuda_cores;     /* Number of CUDA cores (0 if N/A) */
    uint32_t    tensor_cores;   /* Number of tensor cores (0 if N/A) */
    size_t      memory_size;    /* GPU memory size (0 if unified) */
    bool        unified_memory; /* True if CPU/GPU share memory */
} gpu_info_t;

/*
 * GPU Driver Operations
 *
 * Function pointers for platform-specific GPU driver implementation.
 * The active driver is selected at boot based on platform detection.
 */
struct gpu_driver {
    const char *name;

    /* Initialization */
    int (*init)(void);
    void (*shutdown)(void);

    /* Information */
    int (*get_info)(gpu_info_t *info);

    /* Memory Management */
    int (*alloc)(size_t size, uint32_t flags, gpu_buffer_t *buf);
    void (*free)(gpu_buffer_t *buf);

    /* Cache Coherency */
    void (*sync_for_gpu)(gpu_buffer_t *buf);   /* CPU done, GPU will access */
    void (*sync_for_cpu)(gpu_buffer_t *buf);   /* GPU done, CPU will access */

    /* Command Submission (future) */
    int (*submit)(void *cmd_buffer, size_t size);
    int (*wait)(uint64_t timeout_ns);
};

/*
 * Global GPU Driver API
 *
 * These functions use the currently registered driver.
 */

/* Register a GPU driver (called by platform init) */
void gpu_register_driver(const struct gpu_driver *drv);

/* Initialize the GPU subsystem */
int gpu_init(void);

/* Shutdown the GPU subsystem */
void gpu_shutdown(void);

/* Get GPU information */
int gpu_get_info(gpu_info_t *info);

/* Allocate GPU-accessible memory */
int gpu_alloc(size_t size, uint32_t flags, gpu_buffer_t *buf);

/* Free GPU-accessible memory */
void gpu_free(gpu_buffer_t *buf);

/* Sync buffer for GPU access (clean CPU caches) */
void gpu_sync_for_gpu(gpu_buffer_t *buf);

/* Sync buffer for CPU access (invalidate CPU caches) */
void gpu_sync_for_cpu(gpu_buffer_t *buf);

/* Check if GPU is available */
bool gpu_available(void);

/*
 * ARM64 Cache Maintenance Operations
 *
 * Low-level cache operations for CPU/GPU coherency.
 * Used by GPU drivers and can be called directly if needed.
 */

/* Clean data cache by virtual address range (write back dirty lines) */
void cache_clean_range(void *addr, size_t size);

/* Invalidate data cache by virtual address range (discard cached data) */
void cache_invalidate_range(void *addr, size_t size);

/* Clean and invalidate data cache by virtual address range */
void cache_flush_range(void *addr, size_t size);

#endif /* GPU_H */
