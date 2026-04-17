/*
 * Copyright (c) 2021-2023, NVIDIA CORPORATION.  All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms and conditions of the GNU General Public License,
 * version 2, as published by the Free Software Foundation.
 *
 * Fetched from: https://raw.githubusercontent.com/OE4T/linux-nvgpu/l4t/l4t-r36.5/include/uapi/linux/nvgpu-as.h
 *
 * /dev/nvhost-as-gpu device
 */

#ifndef _UAPI__LINUX_NVGPU_AS_H__
#define _UAPI__LINUX_NVGPU_AS_H__

#include "nvgpu-uapi-common.h"

#define NVGPU_AS_IOCTL_MAGIC 'A'

struct nvgpu32_as_alloc_space_args {
	__u32 pages;     /* in, pages */
	__u32 page_size; /* in, bytes */
	__u32 flags;     /* in */
#define NVGPU_AS_ALLOC_SPACE_FLAGS_FIXED_OFFSET 0x1
#define NVGPU_AS_ALLOC_SPACE_FLAGS_SPARSE 0x2
	union {
		__u64 offset; /* inout, byte address valid iff _FIXED_OFFSET */
		__u64 align;  /* in, alignment multiple (0:={1 or n/a}) */
	} o_a;
};

struct nvgpu_as_alloc_space_args {
	__u64 pages;     /* in, pages */
	__u32 page_size; /* in, bytes */
	__u32 flags;     /* in */
	union {
		__u64 offset; /* inout, byte address valid iff _FIXED_OFFSET */
		__u64 align;  /* in, alignment multiple (0:={1 or n/a}) */
	} o_a;
	__u32 padding[2];     /* in */
};

struct nvgpu_as_free_space_args {
	__u64 offset; /* in, byte address */
	__u64 pages;     /* in, pages */
	__u32 page_size; /* in, bytes */
	__u32 padding[3];
};

struct nvgpu_as_bind_channel_args {
	__u32 channel_fd; /* in */
};

#define NVGPU_AS_MAP_BUFFER_FLAGS_FIXED_OFFSET		(1 << 0)
#define NVGPU_AS_MAP_BUFFER_FLAGS_CACHEABLE		(1 << 2)
#define NVGPU_AS_MAP_BUFFER_FLAGS_UNMAPPED_PTE		(1 << 5)
#define NVGPU_AS_MAP_BUFFER_FLAGS_MAPPABLE_COMPBITS	(1 << 6)
#define NVGPU_AS_MAP_BUFFER_FLAGS_L3_ALLOC		(1 << 7)
#define NVGPU_AS_MAP_BUFFER_FLAGS_SYSTEM_COHERENT	(1 << 9)
#define NVGPU_AS_MAP_BUFFER_FLAGS_TEGRA_RAW		(1 << 12)

#define NVGPU_AS_MAP_BUFFER_FLAGS_ACCESS_BITMASK_OFFSET    10U
#define NVGPU_AS_MAP_BUFFER_FLAGS_ACCESS_BITMASK_SIZE      2U

#define NVGPU_AS_MAP_BUFFER_ACCESS_DEFAULT                 0U
#define NVGPU_AS_MAP_BUFFER_ACCESS_READ_ONLY               1U
#define NVGPU_AS_MAP_BUFFER_ACCESS_READ_WRITE              2U

struct nvgpu_as_map_buffer_ex_args {
	__u32 flags;		/* in/out */
#define NV_KIND_INVALID -1
	__s16 compr_kind;
	__s16 incompr_kind;
	__u32 dmabuf_fd;	/* in */
	__u32 page_size;	/* inout, 0:= best fit to buffer */
	__u64 buffer_offset;	/* in, offset of mapped buffer region */
	__u64 mapping_size;	/* in, size of mapped buffer region */
	__u64 offset;		/* in/out, virtual address */
};

struct nvgpu_as_unmap_buffer_args {
	__u64 offset; /* in, byte address */
};

struct nvgpu_as_va_region {
	__u64 offset;
	__u32 page_size;
	__u32 reserved;
	__u64 pages;
};

struct nvgpu_as_get_va_regions_args {
	__u64 buf_addr;
	__u32 buf_size;
	__u32 reserved;
};

struct nvgpu_as_map_buffer_batch_args {
	__u64 unmaps; /* ptr to array of nvgpu_as_unmap_buffer_args */
	__u64 maps;   /* ptr to array of nvgpu_as_map_buffer_ex_args */
	__u32 num_unmaps;
	__u32 num_maps;
	__u64 reserved;
};

struct nvgpu_as_get_sync_ro_map_args {
	__u64 base_gpuva;
	__u32 sync_size;
	__u32 num_syncpoints;
};

struct nvgpu_as_mapping_modify_args {
	__s16 compr_kind;
	__s16 incompr_kind;
	__u64 buffer_offset;
	__u64 buffer_size;
	__u64 map_address;
};

struct nvgpu_as_remap_op {
#define NVGPU_AS_REMAP_OP_FLAGS_CACHEABLE               (1 << 2)
#define NVGPU_AS_REMAP_OP_FLAGS_ACCESS_NO_WRITE         (1 << 10)
#define NVGPU_AS_REMAP_OP_FLAGS_PAGESIZE_4K             (1 << 15)
#define NVGPU_AS_REMAP_OP_FLAGS_PAGESIZE_64K            (1 << 16)
#define NVGPU_AS_REMAP_OP_FLAGS_PAGESIZE_128K           (1 << 17)
	__u32 flags;
	__s16 compr_kind;
	__s16 incompr_kind;
	__u32 mem_handle;
	__s32 reserved;
	__u64 mem_offset_in_pages;
	__u64 virt_offset_in_pages;
	__u64 num_pages;
};

struct nvgpu_as_remap_args {
	__u64 ops;
	__u32 num_ops;
};

#define NVGPU_AS_IOCTL_BIND_CHANNEL \
	_IOWR(NVGPU_AS_IOCTL_MAGIC, 1, struct nvgpu_as_bind_channel_args)
#define NVGPU32_AS_IOCTL_ALLOC_SPACE \
	_IOWR(NVGPU_AS_IOCTL_MAGIC, 2, struct nvgpu32_as_alloc_space_args)
#define NVGPU_AS_IOCTL_FREE_SPACE \
	_IOWR(NVGPU_AS_IOCTL_MAGIC, 3, struct nvgpu_as_free_space_args)
#define NVGPU_AS_IOCTL_UNMAP_BUFFER \
	_IOWR(NVGPU_AS_IOCTL_MAGIC, 5, struct nvgpu_as_unmap_buffer_args)
#define NVGPU_AS_IOCTL_ALLOC_SPACE \
	_IOWR(NVGPU_AS_IOCTL_MAGIC, 6, struct nvgpu_as_alloc_space_args)
#define NVGPU_AS_IOCTL_MAP_BUFFER_EX \
	_IOWR(NVGPU_AS_IOCTL_MAGIC, 7, struct nvgpu_as_map_buffer_ex_args)
#define NVGPU_AS_IOCTL_GET_VA_REGIONS \
	_IOWR(NVGPU_AS_IOCTL_MAGIC, 8, struct nvgpu_as_get_va_regions_args)
#define NVGPU_AS_IOCTL_GET_BUFFER_COMPBITS_INFO \
	_IOWR(NVGPU_AS_IOCTL_MAGIC, 9, struct nvgpu_as_get_buffer_compbits_info_args)
#define NVGPU_AS_IOCTL_MAP_BUFFER_COMPBITS \
	_IOWR(NVGPU_AS_IOCTL_MAGIC, 10, struct nvgpu_as_map_buffer_compbits_args)
#define NVGPU_AS_IOCTL_MAP_BUFFER_BATCH	\
	_IOWR(NVGPU_AS_IOCTL_MAGIC, 11, struct nvgpu_as_map_buffer_batch_args)
#define NVGPU_AS_IOCTL_GET_SYNC_RO_MAP	\
	_IOR(NVGPU_AS_IOCTL_MAGIC,  12, struct nvgpu_as_get_sync_ro_map_args)
#define NVGPU_AS_IOCTL_MAPPING_MODIFY	\
	_IOWR(NVGPU_AS_IOCTL_MAGIC,  13, struct nvgpu_as_mapping_modify_args)
#define NVGPU_AS_IOCTL_REMAP		\
	_IOWR(NVGPU_AS_IOCTL_MAGIC, 14, struct nvgpu_as_remap_args)

#define NVGPU_AS_IOCTL_LAST		\
	_IOC_NR(NVGPU_AS_IOCTL_REMAP)
#define NVGPU_AS_IOCTL_MAX_ARG_SIZE	\
	sizeof(struct nvgpu_as_map_buffer_ex_args)

#endif /* _UAPI__LINUX_NVGPU_AS_H__ */
