/*
 * hailo_dev.c — ioctl wrappers for /dev/hailo0.
 *
 * Every function returns 0 on success and -errno on ioctl failure.
 * Callers use these as building blocks for the --submit-probe flow
 * that exercises SLM-OS's VDMA descriptor layout against the
 * official hailo_pci driver.
 */

#include "hailo_dev.h"

#include <errno.h>
#include <limits.h>    /* INT_MAX used by hailo-ioctl-common.h enums */
#include <signal.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/time.h>  /* setitimer for the interrupts_wait timeout */
#include <unistd.h>

int hailo_dev_buffer_map(int fd,
                         const void *user_addr,
                         size_t size,
                         enum hailo_dev_dir dir,
                         uintptr_t *mapped_handle_out)
{
    if (!user_addr || !size || !mapped_handle_out) return -EINVAL;

    struct hailo_vdma_buffer_map_params p;
    memset(&p, 0, sizeof(p));
    p.user_address            = (uintptr_t)user_addr;
    p.size                    = size;
    p.data_direction          = (enum hailo_dma_data_direction)dir;
    p.buffer_type             = HAILO_DMA_USER_PTR_BUFFER;
    /* allocated_buffer_handle is a hint for the driver when the
     * buffer came from HAILO_VDMA_LOW_MEMORY_BUFFER_ALLOC; for raw
     * userspace memory it's zero. */
    p.allocated_buffer_handle = 0;

    if (ioctl(fd, HAILO_VDMA_BUFFER_MAP, &p) < 0) return -errno;
    *mapped_handle_out = (uintptr_t)p.mapped_handle;
    return 0;
}

int hailo_dev_buffer_unmap(int fd, uintptr_t mapped_handle)
{
    struct hailo_vdma_buffer_unmap_params p;
    memset(&p, 0, sizeof(p));
    p.mapped_handle = (size_t)mapped_handle;
    if (ioctl(fd, HAILO_VDMA_BUFFER_UNMAP, &p) < 0) return -errno;
    return 0;
}

int hailo_dev_desc_list_create(int fd,
                               size_t desc_count,
                               uint16_t desc_page_size,
                               bool is_circular,
                               uintptr_t *desc_handle_out,
                               uint64_t *dma_address_out)
{
    if (!desc_handle_out) return -EINVAL;

    struct hailo_desc_list_create_params p;
    memset(&p, 0, sizeof(p));
    p.desc_count     = desc_count;
    p.desc_page_size = desc_page_size;
    p.is_circular    = is_circular;

    if (ioctl(fd, HAILO_DESC_LIST_CREATE, &p) < 0) return -errno;
    *desc_handle_out = p.desc_handle;
    if (dma_address_out) *dma_address_out = p.dma_address;
    return 0;
}

int hailo_dev_desc_list_release(int fd, uintptr_t desc_handle)
{
    struct hailo_desc_list_release_params p;
    memset(&p, 0, sizeof(p));
    p.desc_handle = desc_handle;
    if (ioctl(fd, HAILO_DESC_LIST_RELEASE, &p) < 0) return -errno;
    return 0;
}

int hailo_dev_desc_list_program(int fd,
                                uintptr_t desc_handle,
                                uintptr_t mapped_handle,
                                size_t buffer_offset,
                                size_t buffer_size,
                                uint8_t channel_index,
                                uint32_t starting_desc,
                                bool should_bind)
{
    struct hailo_desc_list_program_params p;
    memset(&p, 0, sizeof(p));
    p.buffer_handle           = (size_t)mapped_handle;
    p.buffer_size             = buffer_size;
    p.buffer_offset           = buffer_offset;
    p.batch_size              = 1;
    p.desc_handle             = desc_handle;
    p.channel_index           = channel_index;
    p.starting_desc           = starting_desc;
    p.should_bind             = should_bind;
    p.last_interrupts_domain  = HAILO_VDMA_INTERRUPTS_DOMAIN_HOST;
    p.is_debug                = false;
    p.stride                  = 0; /* use desc_page_size */

    if (ioctl(fd, HAILO_DESC_LIST_PROGRAM, &p) < 0) return -errno;
    return 0;
}

static void fill_channel_bitmap(uint32_t *bitmap_per_engine,
                                uint8_t channel_index)
{
    /* Pi 5 Hailo-8L has one VDMA engine (engine 0). */
    for (int i = 0; i < MAX_VDMA_ENGINES; i++) bitmap_per_engine[i] = 0;
    bitmap_per_engine[0] = (1u << channel_index);
}

int hailo_dev_enable_channel(int fd, uint8_t channel_index,
                             bool enable_timestamps)
{
    struct hailo_vdma_enable_channels_params p;
    memset(&p, 0, sizeof(p));
    fill_channel_bitmap(p.channels_bitmap_per_engine, channel_index);
    p.enable_timestamps_measure = enable_timestamps;
    if (ioctl(fd, HAILO_VDMA_ENABLE_CHANNELS, &p) < 0) return -errno;
    return 0;
}

int hailo_dev_disable_channel(int fd, uint8_t channel_index)
{
    struct hailo_vdma_disable_channels_params p;
    memset(&p, 0, sizeof(p));
    fill_channel_bitmap(p.channels_bitmap_per_engine, channel_index);
    if (ioctl(fd, HAILO_VDMA_DISABLE_CHANNELS, &p) < 0) return -errno;
    return 0;
}

int hailo_dev_launch_transfer(int fd,
                              uint8_t channel_index,
                              uintptr_t desc_handle,
                              uint32_t starting_desc,
                              uintptr_t mapped_handle,
                              uint32_t transfer_size)
{
    struct hailo_vdma_launch_transfer_params p;
    memset(&p, 0, sizeof(p));
    p.engine_index             = 0;
    p.channel_index            = channel_index;
    p.desc_handle              = desc_handle;
    p.starting_desc            = starting_desc;
    p.should_bind              = false;  /* buffer already bound by
                                           * desc_list_program with
                                           * should_bind=true */
    p.buffers_count            = 1;
    p.buffers[0].buffer_type   = HAILO_DMA_USER_PTR_BUFFER;
    p.buffers[0].addr_or_fd    = mapped_handle;
    p.buffers[0].size          = transfer_size;
    p.first_interrupts_domain  = HAILO_VDMA_INTERRUPTS_DOMAIN_NONE;
    p.last_interrupts_domain   = HAILO_VDMA_INTERRUPTS_DOMAIN_HOST;
    p.is_debug                 = false;

    if (ioctl(fd, HAILO_VDMA_LAUNCH_TRANSFER, &p) < 0) return -errno;
    return 0;
}

/* SIGALRM handler that does nothing. Used to interrupt the
 * blocking HAILO_VDMA_INTERRUPTS_WAIT ioctl so callers can
 * specify a wall-clock timeout — the kernel returns -EINTR on
 * signal, which we treat as timeout. */
static void alarm_noop(int sig) { (void)sig; }

int hailo_dev_interrupts_wait(int fd,
                              uint32_t channel_bitmap,
                              uint32_t timeout_ms,
                              uint8_t *channels_count_out,
                              struct hailo_vdma_interrupts_channel_data
                                  *channels_out,
                              size_t channels_out_cap)
{
    if (!channels_count_out) return -EINVAL;

    struct hailo_vdma_interrupts_wait_params p;
    memset(&p, 0, sizeof(p));
    p.channels_bitmap_per_engine[0] = channel_bitmap;

    /* Install a SIGALRM handler that interrupts the blocking ioctl.
     * Save + restore the previous handler so we don't clobber
     * caller signal setup. */
    struct sigaction sa, old_sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = alarm_noop;
    sigaction(SIGALRM, &sa, &old_sa);

    /* Use setitimer(ITIMER_REAL) rather than alarm() for two reasons:
     *   1. Millisecond granularity (alarm() is 1 s integer seconds).
     *   2. Periodic re-fire defeats the signal-race hang: if the
     *      very first SIGALRM arrives in the tiny window between
     *      setitimer() returning and the ioctl entering the kernel,
     *      the handler no-ops and the ioctl starts blocking. The
     *      periodic it_interval retries at ~timeout_ms/4 so a
     *      missed first signal is picked up on the next tick;
     *      total wait stays within [timeout_ms, 1.25 * timeout_ms].
     *
     * This is a userspace diagnostic tool: we'd rather pay a tiny
     * worst-case overshoot than risk a silent hang that needs
     * Ctrl-C or a fresh SSH session to recover from. */
    struct itimerval it = {0}, old_it = {0};
    uint32_t clamped_ms = timeout_ms > 0 ? timeout_ms : 1u;
    it.it_value.tv_sec   = (time_t)(clamped_ms / 1000u);
    it.it_value.tv_usec  = (suseconds_t)((clamped_ms % 1000u) * 1000u);
    uint32_t retry_ms    = clamped_ms / 4u;
    if (retry_ms == 0) retry_ms = 1u;
    it.it_interval.tv_sec  = (time_t)(retry_ms / 1000u);
    it.it_interval.tv_usec = (suseconds_t)((retry_ms % 1000u) * 1000u);
    setitimer(ITIMER_REAL, &it, &old_it);

    int rc = ioctl(fd, HAILO_VDMA_INTERRUPTS_WAIT, &p);
    int saved_errno = errno;

    /* Disarm the timer and restore the caller's state before we
     * return, regardless of ioctl outcome. */
    struct itimerval stop = {0};
    setitimer(ITIMER_REAL, &stop, NULL);
    setitimer(ITIMER_REAL, &old_it, NULL);
    sigaction(SIGALRM, &old_sa, NULL);

    if (rc < 0) {
        errno = saved_errno;
        return -saved_errno;   /* -EINTR on timeout, -errno otherwise */
    }

    uint8_t count = p.channels_count;
    if (count > channels_out_cap) count = (uint8_t)channels_out_cap;
    if (channels_out && count > 0) {
        memcpy(channels_out, p.irq_data,
               count * sizeof(channels_out[0]));
    }
    *channels_count_out = count;
    return 0;
}
