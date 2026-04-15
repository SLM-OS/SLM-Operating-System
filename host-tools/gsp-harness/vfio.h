/*
 * vfio.h — persistent VFIO session for the GSP-RM harness (E3.2).
 *
 * Gives the harness two capabilities the `/sys/.../resourceN` mmap
 * path can't: (1) DMA — bind userspace buffers to the IOMMU so the
 * GPU can read/write them through its own address space; (2) config
 * space writes — needed by a handful of VBIOS / reset sequences that
 * sysfs won't let us do without root-level PCI access.
 *
 * Lifetime: open once in `linux_gsp_platform_init` via `vfio_open`,
 * close once at process exit. BAR mapping and DMA allocation go
 * through the same container/group/device triple so one IOMMU
 * context stays live for the whole run.
 */

#ifndef GSP_HARNESS_VFIO_H
#define GSP_HARNESS_VFIO_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct vfio_session;

/*
 * Open a VFIO container + group + device for @pci_path
 * (e.g. "/sys/bus/pci/devices/0000:01:00.0"). Resolves the IOMMU
 * group via the device's `iommu_group` symlink, sets VFIO_TYPE1_IOMMU,
 * and returns the device fd.
 *
 * Returns NULL if the device isn't vfio-bound, the IOMMU isn't
 * usable, or the container can't be opened. The caller's fallback
 * path (sysfs) handles those cases for read-only work.
 */
struct vfio_session *vfio_open(const char *pci_path, bool trace);

/* Accessors on an open session. */
int vfio_device_fd(const struct vfio_session *s);
int vfio_container_fd(const struct vfio_session *s);

/*
 * Look up a BAR / config / ROM region by VFIO_PCI_*_REGION_INDEX.
 * Writes the (offset, size) that the device fd exposes for that
 * region. On success the caller mmap()s on the device fd at @offset
 * with @size bytes.
 *
 * Returns 0 on success, -1 if the region doesn't exist or the
 * lookup fails.
 */
int vfio_region_info(const struct vfio_session *s, uint32_t index,
                     uint64_t *out_offset, uint64_t *out_size);

/*
 * Allocate DMA-capable memory and map it into the device's IOMMU
 * address space.
 *
 * @size      must be a multiple of 4 KB (rounded up if not).
 * @align     desired VA alignment in bytes. Ignored if smaller than
 *            4 KB — mmap always returns page-aligned.
 * @out_iova  populated with the GPU-visible IOVA on success.
 *
 * Returns the CPU VA on success (writable by host, readable by the
 * GPU via the IOVA). NULL on any failure. Buffer is mlocked so the
 * kernel can't swap it out under us.
 */
void *vfio_dma_alloc(struct vfio_session *s, size_t size, size_t align,
                     uint64_t *out_iova);

/* Unmap + free a buffer returned by vfio_dma_alloc. Safe to call
 * on NULL. */
void vfio_dma_free(struct vfio_session *s, void *va, size_t size);

/*
 * Total bytes currently mapped in the IOMMU. Useful for leak checks
 * in the --dma-test action.
 */
size_t vfio_dma_inflight_bytes(const struct vfio_session *s);

/* Tear down. Safe to call on NULL. */
void vfio_close(struct vfio_session *s);

#endif /* GSP_HARNESS_VFIO_H */
