/*
 * hailo_dev.h — thin wrappers around /dev/hailo0 ioctls that
 * `hailo-ushim` uses to replay SLM-OS's VDMA setup and boundary
 * submit against real Hailo-8L hardware from Linux userspace.
 *
 * Each wrapper returns 0 on success and a negative errno on failure,
 * writing any outputs through pointer params. The intent is to
 * hide the ioctl struct marshaling so main.c reads as a sequence
 * of operations rather than a wall of struct initializers.
 *
 * References:
 *   - hailo-ioctl-common.h (vendored v4.23 copy) — struct layouts
 *   - hailo-pcie.c / hailo-vdma-common.c (docs/reference) — kernel
 *     side semantics of each ioctl
 */

#ifndef HAILO_USHIM_HAILO_DEV_H
#define HAILO_USHIM_HAILO_DEV_H

#include <limits.h>   /* hailo-ioctl-common.h enums reference INT_MAX */
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

#include "hailo-ioctl-common.h"

/* DMA direction for HAILO_VDMA_BUFFER_MAP. Passed through verbatim
 * from the ioctl enum; wrapping here so callers don't have to pull
 * in the whole ioctl header. */
enum hailo_dev_dir {
    HAILO_DEV_DIR_H2D = HAILO_DMA_TO_DEVICE,
    HAILO_DEV_DIR_D2H = HAILO_DMA_FROM_DEVICE,
    HAILO_DEV_DIR_BI  = HAILO_DMA_BIDIRECTIONAL,
};

/*
 * Map a userspace buffer into the Hailo endpoint's IOVA space.
 * On success, `*mapped_handle_out` receives the driver's opaque
 * handle to pass into subsequent DESC_LIST_PROGRAM / BUFFER_UNMAP
 * calls. `user_addr` must point at valid, page-aligned, resident
 * memory for the full `size` bytes (driver mlocks the range).
 */
int hailo_dev_buffer_map(int fd,
                         const void *user_addr,
                         size_t size,
                         enum hailo_dev_dir dir,
                         uintptr_t *mapped_handle_out);

/* Release a buffer mapping previously returned by buffer_map. */
int hailo_dev_buffer_unmap(int fd, uintptr_t mapped_handle);

/*
 * Allocate a descriptor list on the device. The driver picks a
 * 64 KB-aligned backing region for the list itself and returns the
 * Hailo IOVA in `*dma_address_out`. `*desc_handle_out` is the
 * opaque handle for subsequent ioctls.
 *
 * desc_count must be a power of 2 in [2, 65536]. desc_page_size is
 * the number of bytes each descriptor covers (boundary channels
 * on MNIST: 512 B input, 64 B output, per SLM-OS).
 */
int hailo_dev_desc_list_create(int fd,
                               size_t desc_count,
                               uint16_t desc_page_size,
                               bool is_circular,
                               uintptr_t *desc_handle_out,
                               uint64_t *dma_address_out);

int hailo_dev_desc_list_release(int fd, uintptr_t desc_handle);

/*
 * Program a range of descriptors in `desc_handle` to cover
 * `buffer_size` bytes of the buffer identified by `mapped_handle`
 * starting at `buffer_offset`. `channel_index` is the VDMA
 * channel the descriptors will belong to (SLM-OS uses ch=2 for
 * boundary input, ch=16 for boundary output). `starting_desc=0`
 * targets the head of the list.
 */
int hailo_dev_desc_list_program(int fd,
                                uintptr_t desc_handle,
                                uintptr_t mapped_handle,
                                size_t buffer_offset,
                                size_t buffer_size,
                                uint8_t channel_index,
                                uint32_t starting_desc,
                                bool should_bind);

/*
 * Enable one VDMA channel. `channel_index` is the VDMA channel
 * id (0..31). The ioctl takes a per-engine bitmap; we fill bit
 * `channel_index` for engine 0 and zero the rest. Pi 5 Hailo-8L
 * has one engine.
 */
int hailo_dev_enable_channel(int fd, uint8_t channel_index,
                             bool enable_timestamps);

int hailo_dev_disable_channel(int fd, uint8_t channel_index);

/*
 * Kick a transfer on `channel_index`. For HAILO_DMA_USER_PTR_BUFFER
 * (what this wrapper always uses), the driver's launch_transfer
 * ioctl reads `addr_or_fd` as the USERSPACE POINTER — not the
 * mapped_handle — to cross-check that the buffer matches what was
 * bound via DESC_LIST_PROGRAM. Passing the mapped_handle in the
 * addr_or_fd slot produces EFAULT because the kernel tries to
 * copy_from_user on the (small-integer) handle value. Caller
 * passes the same userspace buffer pointer it fed to buffer_map.
 *
 * Returns 0 if the ioctl accepted the launch parameters (not that
 * the device completed the transfer). Pair with interrupts_wait to
 * observe completion.
 */
int hailo_dev_launch_transfer(int fd,
                              uint8_t channel_index,
                              uintptr_t desc_handle,
                              uint32_t starting_desc,
                              const void *user_addr,
                              uint32_t transfer_size);

/*
 * Block up to `timeout_ms` waiting for an interrupt on any channel
 * in `channel_bitmap`. On return, `*channels_count_out` is the number
 * of completing channels, and `channels_out[0..count-1]` describes
 * which channels fired and their transfer counts (or error flags).
 *
 * Single-threaded only: installs a process-global SIGALRM handler +
 * setitimer to enforce the timeout. Two concurrent callers from
 * different threads would race the itimer install/restore.
 */
int hailo_dev_interrupts_wait(int fd,
                              uint32_t channel_bitmap,
                              uint32_t timeout_ms,
                              uint8_t *channels_count_out,
                              struct hailo_vdma_interrupts_channel_data
                                  *channels_out,
                              size_t channels_out_cap);

#endif /* HAILO_USHIM_HAILO_DEV_H */
