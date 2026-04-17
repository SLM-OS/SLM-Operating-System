/*
 * NVGPU Control UAPI Header (partial - key structures and ioctl numbers)
 *
 * Copyright (c) 2021-2023, NVIDIA CORPORATION.  All rights reserved.
 * Licensed under GPLv2.
 *
 * Fetched from: https://raw.githubusercontent.com/OE4T/linux-nvgpu/l4t/l4t-r36.5/include/uapi/linux/nvgpu-ctrl.h
 *
 * NOTE: This is a partial extraction focused on the ioctl numbers and
 * key structures needed for channel creation. The full file is ~2000 lines.
 */

#ifndef _UAPI__LINUX_NVGPU_CTRL_H
#define _UAPI__LINUX_NVGPU_CTRL_H

#define NVGPU_GPU_IOCTL_MAGIC 'G'

/* --- Key structures for channel creation --- */

struct nvgpu_gpu_open_tsg_args {
	__u32 tsg_fd;		/* out: tsg fd */
	__u32 flags;		/* in: NVGPU_GPU_IOCTL_OPEN_TSG_FLAGS_* */
	__u64 source_device_instance_id;
	__u64 share_token;
};

struct nvgpu_gpu_open_channel_args {
	union {
		__s32 channel_fd;
		struct {
			__s32 runlist_id;
		} in;
		struct {
			__s32 channel_fd;
		} out;
	};
};

struct nvgpu_alloc_as_args {
	__u32 big_page_size;
	__s32 as_fd;
	__u32 flags;
	__u32 reserved;
	__u64 va_range_start;
	__u64 va_range_end;
	__u64 va_range_split;
	__u32 padding[6];
};

struct nvgpu_gpu_get_characteristics {
	__u32 arch;
	__u32 impl;
	__u32 rev;
	__u32 num_gpc;
	__s32 numa_domain_id;
	__u64 L2_cache_size;
	__u64 on_board_video_memory_size;
	__u32 num_tpc_per_gpc;
	__u32 bus_type;
	__u32 big_page_size;
	__u32 compression_page_size;
	__u32 pde_coverage_bit_count;
	__u32 available_big_page_sizes;
	__u64 flags;
	__u32 twod_class;
	__u32 threed_class;
	__u32 compute_class;
	__u32 gpfifo_class;
	__u32 inline_to_memory_class;
	__u32 dma_copy_class;
	__u32 gpc_mask;
	__u32 sm_arch_sm_version;
	__u32 sm_arch_spa_version;
	__u32 sm_arch_warp_count;
	__s16 gpu_ioctl_nr_last;
	__s16 tsg_ioctl_nr_last;
	__s16 dbg_gpu_ioctl_nr_last;
	__s16 ioctl_channel_nr_last;
	__s16 as_ioctl_nr_last;
	__u8 gpu_va_bit_count;
	__u8 reserved;
	__u32 max_fbps_count;
	__u32 fbp_en_mask;
	__u32 emc_en_mask;
	__u32 max_ltc_per_fbp;
	__u32 max_lts_per_ltc;
	__u32 max_tex_per_tpc;
	__u32 max_gpc_count;
	__u32 rop_l2_en_mask_DEPRECATED[2];
	__u8 chipname[8];
	__u64 gr_compbit_store_base_hw;
	__u32 gr_gobs_per_comptagline_per_slice;
	__u32 num_ltc;
	__u32 lts_per_ltc;
	__u32 cbc_cache_line_size;
	__u32 cbc_comptags_per_line;
	__u32 map_buffer_batch_limit;
	__u64 max_freq;
	__u32 graphics_preemption_mode_flags;
	__u32 compute_preemption_mode_flags;
	__u32 default_graphics_preempt_mode;
	__u32 default_compute_preempt_mode;
	__u64 local_video_memory_size;
	__u16 pci_vendor_id, pci_device_id;
	__u16 pci_subsystem_vendor_id, pci_subsystem_device_id;
	__u16 pci_class;
	__u8  pci_revision;
	__u8  vbios_oem_version;
	__u32 vbios_version;
	__u32 reg_ops_limit;
	__u32 reserved1;
	__s16 event_ioctl_nr_last;
	__u16 pad;
	__u32 max_css_buffer_size;
	__s16 ctxsw_ioctl_nr_last;
	__s16 prof_ioctl_nr_last;
	__s16 nvs_ioctl_nr_last;
	__u8 reserved2[2];
	__u32 max_ctxsw_ring_buffer_size;
	__u32 reserved3;
	__u64 per_device_identifier;
	__u32 num_ppc_per_gpc;
	__u32 max_veid_count_per_tsg;
	__u32 num_sub_partition_per_fbpa;
	__u32 gpu_instance_id;
	__u32 gr_instance_id;
	__u32 max_gpfifo_entries;
	__u32 max_dbg_tsg_timeslice;
	__u32 reserved5;
	__u64 device_instance_id;
};

/* --- IOCTL numbers --- */

#define NVGPU_GPU_IOCTL_ZCULL_GET_CTX_SIZE \
	_IOR(NVGPU_GPU_IOCTL_MAGIC, 1, struct nvgpu_gpu_zcull_get_ctx_size_args)
#define NVGPU_GPU_IOCTL_ZCULL_GET_INFO \
	_IOR(NVGPU_GPU_IOCTL_MAGIC, 2, struct nvgpu_gpu_zcull_get_info_args)
#define NVGPU_GPU_IOCTL_ZBC_SET_TABLE	\
	_IOW(NVGPU_GPU_IOCTL_MAGIC, 3, struct nvgpu_gpu_zbc_set_table_args)
#define NVGPU_GPU_IOCTL_ZBC_QUERY_TABLE	\
	_IOWR(NVGPU_GPU_IOCTL_MAGIC, 4, struct nvgpu_gpu_zbc_query_table_args)
#define NVGPU_GPU_IOCTL_GET_CHARACTERISTICS   \
	_IOWR(NVGPU_GPU_IOCTL_MAGIC, 5, struct nvgpu_gpu_get_characteristics)
#define NVGPU_GPU_IOCTL_PREPARE_COMPRESSIBLE_READ \
	_IOWR(NVGPU_GPU_IOCTL_MAGIC, 6, struct nvgpu_gpu_prepare_compressible_read_args)
#define NVGPU_GPU_IOCTL_MARK_COMPRESSIBLE_WRITE \
	_IOWR(NVGPU_GPU_IOCTL_MAGIC, 7, struct nvgpu_gpu_mark_compressible_write_args)
#define NVGPU_GPU_IOCTL_ALLOC_AS \
	_IOWR(NVGPU_GPU_IOCTL_MAGIC, 8, struct nvgpu_alloc_as_args)
#define NVGPU_GPU_IOCTL_OPEN_TSG \
	_IOWR(NVGPU_GPU_IOCTL_MAGIC, 9, struct nvgpu_gpu_open_tsg_args)
#define NVGPU_GPU_IOCTL_GET_TPC_MASKS \
	_IOWR(NVGPU_GPU_IOCTL_MAGIC, 10, struct nvgpu_gpu_get_tpc_masks_args)
#define NVGPU_GPU_IOCTL_OPEN_CHANNEL \
	_IOWR(NVGPU_GPU_IOCTL_MAGIC, 11, struct nvgpu_gpu_open_channel_args)
#define NVGPU_GPU_IOCTL_FLUSH_L2 \
	_IOWR(NVGPU_GPU_IOCTL_MAGIC, 12, struct nvgpu_gpu_l2_fb_args)
#define NVGPU_GPU_IOCTL_SET_MMUDEBUG_MODE \
	_IOWR(NVGPU_GPU_IOCTL_MAGIC, 14, struct nvgpu_gpu_mmu_debug_mode_args)
#define NVGPU_GPU_IOCTL_SET_SM_DEBUG_MODE \
	_IOWR(NVGPU_GPU_IOCTL_MAGIC, 15, struct nvgpu_gpu_sm_debug_mode_args)
#define NVGPU_GPU_IOCTL_GET_ENGINE_INFO \
	_IOWR(NVGPU_GPU_IOCTL_MAGIC, 26, struct nvgpu_gpu_get_engine_info_args)
#define NVGPU_GPU_IOCTL_ALLOC_VIDMEM \
	_IOWR(NVGPU_GPU_IOCTL_MAGIC, 27, struct nvgpu_gpu_alloc_vidmem_args)

#endif /* _UAPI__LINUX_NVGPU_CTRL_H */
