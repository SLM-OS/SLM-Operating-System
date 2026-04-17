/*
 * NVGPU Public Interface Header
 *
 * Copyright (c) 2011-2022, NVIDIA CORPORATION.  All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms and conditions of the GNU General Public License,
 * version 2, as published by the Free Software Foundation.
 *
 * This program is distributed in the hope it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 * more details.
 *
 * Fetched from: https://raw.githubusercontent.com/OE4T/linux-nvgpu/l4t/l4t-r36.5/include/uapi/linux/nvgpu.h
 */

#ifndef _UAPI__LINUX_NVGPU_IOCTL_H
#define _UAPI__LINUX_NVGPU_IOCTL_H

#include "nvgpu-ctrl.h"
#include "nvgpu-event.h"
#include "nvgpu-as.h"

#include "nvgpu-nvs.h"

/*
 * /dev/nvhost-tsg-gpu device
 *
 * Opening a '/dev/nvhost-tsg-gpu' device node creates a way to
 * bind/unbind a channel to/from TSG group
 */

#define NVGPU_TSG_IOCTL_MAGIC 'T'

struct nvgpu_tsg_bind_channel_ex_args {
	/* in: channel fd */
	__s32 channel_fd;

	/* in: VEID in Volta */
	__u32 subcontext_id;
	__u8 reserved[16];
};

struct nvgpu_tsg_bind_scheduling_domain_args {
	/* in: id of the domain this tsg will be bound to */
	__s32 domain_fd;
	/* Must be set to 0 */
	__s32 reserved0;
	/* Must be set to 0 */
	__u64 reserved[3];
};

struct nvgpu_tsg_sm_error_state_record {
	__u32 global_esr;
	__u32 warp_esr;
	__u64 warp_esr_pc;
	__u32 global_esr_report_mask;
	__u32 warp_esr_report_mask;
};

struct nvgpu_tsg_read_single_sm_error_state_args {
	__u32 sm_id;
	__u32 reserved;
	__u64 record_mem;
	__u64 record_size;
};

struct nvgpu_tsg_read_all_sm_error_state_args {
	__u32 num_sm;
	__u32 reserved;
	__u64 buffer_mem;
	__u64 buffer_size;
};

struct nvgpu_tsg_l2_max_ways_evict_last_args {
	__u32 max_ways;
	__u32 reserved;
};

#define NVGPU_GPU_IOCTL_TSG_L2_SECTOR_PROMOTE_FLAG_NONE		(1U << 0U)
#define NVGPU_GPU_IOCTL_TSG_L2_SECTOR_PROMOTE_FLAG_64B		(1U << 1U)
#define NVGPU_GPU_IOCTL_TSG_L2_SECTOR_PROMOTE_FLAG_128B		(1U << 2U)

struct nvgpu_tsg_set_l2_sector_promotion_args {
	__u32 promotion_flag;
	__u32 reserved;
};

#define NVGPU_TSG_SUBCONTEXT_TYPE_SYNC               (0x0U)
#define NVGPU_TSG_SUBCONTEXT_TYPE_ASYNC              (0x1U)

struct nvgpu_tsg_create_subcontext_args {
	__u32 type;
	__s32 as_fd;
	__u32 veid;
	__u32 reserved;
};

struct nvgpu_tsg_delete_subcontext_args {
	__u32 veid;
	__u32 reserved;
};

struct nvgpu_tsg_get_share_token_args {
	__u64 source_device_instance_id;
	__u64 target_device_instance_id;
	__u64 share_token;
};

struct nvgpu_tsg_revoke_share_token_args {
	__u64 source_device_instance_id;
	__u64 target_device_instance_id;
	__u64 share_token;
};

#define NVGPU_TSG_IOCTL_BIND_CHANNEL \
	_IOW(NVGPU_TSG_IOCTL_MAGIC, 1, int)
#define NVGPU_TSG_IOCTL_UNBIND_CHANNEL \
	_IOW(NVGPU_TSG_IOCTL_MAGIC, 2, int)
#define NVGPU_IOCTL_TSG_ENABLE \
	_IO(NVGPU_TSG_IOCTL_MAGIC, 3)
#define NVGPU_IOCTL_TSG_DISABLE \
	_IO(NVGPU_TSG_IOCTL_MAGIC, 4)
#define NVGPU_IOCTL_TSG_PREEMPT \
	_IO(NVGPU_TSG_IOCTL_MAGIC, 5)
#define NVGPU_IOCTL_TSG_EVENT_ID_CTRL \
	_IOWR(NVGPU_TSG_IOCTL_MAGIC, 7, struct nvgpu_event_id_ctrl_args)
#define NVGPU_IOCTL_TSG_SET_RUNLIST_INTERLEAVE \
	_IOW(NVGPU_TSG_IOCTL_MAGIC, 8, struct nvgpu_runlist_interleave_args)
#define NVGPU_IOCTL_TSG_SET_TIMESLICE \
	_IOW(NVGPU_TSG_IOCTL_MAGIC, 9, struct nvgpu_timeslice_args)
#define NVGPU_IOCTL_TSG_GET_TIMESLICE \
	_IOR(NVGPU_TSG_IOCTL_MAGIC, 10, struct nvgpu_timeslice_args)
#define NVGPU_TSG_IOCTL_BIND_CHANNEL_EX \
	_IOWR(NVGPU_TSG_IOCTL_MAGIC, 11, struct nvgpu_tsg_bind_channel_ex_args)
#define NVGPU_TSG_IOCTL_READ_SINGLE_SM_ERROR_STATE \
	_IOWR(NVGPU_TSG_IOCTL_MAGIC, 12, \
			struct nvgpu_tsg_read_single_sm_error_state_args)
#define NVGPU_TSG_IOCTL_SET_L2_MAX_WAYS_EVICT_LAST \
	_IOW(NVGPU_TSG_IOCTL_MAGIC, 13, \
			struct nvgpu_tsg_l2_max_ways_evict_last_args)
#define NVGPU_TSG_IOCTL_GET_L2_MAX_WAYS_EVICT_LAST \
	_IOR(NVGPU_TSG_IOCTL_MAGIC, 14, \
			struct nvgpu_tsg_l2_max_ways_evict_last_args)
#define NVGPU_TSG_IOCTL_SET_L2_SECTOR_PROMOTION \
	_IOW(NVGPU_TSG_IOCTL_MAGIC, 15, \
			struct nvgpu_tsg_set_l2_sector_promotion_args)
#define NVGPU_TSG_IOCTL_BIND_SCHEDULING_DOMAIN \
	_IOW(NVGPU_TSG_IOCTL_MAGIC, 16, \
			struct nvgpu_tsg_bind_scheduling_domain_args)
#define NVGPU_TSG_IOCTL_READ_ALL_SM_ERROR_STATES \
	_IOWR(NVGPU_TSG_IOCTL_MAGIC, 17, \
			struct nvgpu_tsg_read_all_sm_error_state_args)
#define NVGPU_TSG_IOCTL_CREATE_SUBCONTEXT \
	_IOWR(NVGPU_TSG_IOCTL_MAGIC, 18, \
			struct nvgpu_tsg_create_subcontext_args)
#define NVGPU_TSG_IOCTL_DELETE_SUBCONTEXT \
	_IOW(NVGPU_TSG_IOCTL_MAGIC, 19, \
			struct nvgpu_tsg_delete_subcontext_args)
#define NVGPU_TSG_IOCTL_GET_SHARE_TOKEN \
	_IOWR(NVGPU_TSG_IOCTL_MAGIC, 20, \
			struct nvgpu_tsg_get_share_token_args)
#define NVGPU_TSG_IOCTL_REVOKE_SHARE_TOKEN \
	_IOW(NVGPU_TSG_IOCTL_MAGIC, 21, \
			struct nvgpu_tsg_revoke_share_token_args)
#define NVGPU_TSG_IOCTL_MAX_ARG_SIZE	\
		sizeof(struct nvgpu_tsg_bind_scheduling_domain_args)
#define NVGPU_TSG_IOCTL_LAST		\
	_IOC_NR(NVGPU_TSG_IOCTL_REVOKE_SHARE_TOKEN)

/*
 * /dev/nvhost-gpu device
 */

#define NVGPU_IOCTL_MAGIC 'H'
#define NVGPU_NO_TIMEOUT ((__u32)~0U)
#define NVGPU_TIMEOUT_FLAG_DISABLE_DUMP		0

/* this is also the hardware memory format */
struct nvgpu_gpfifo {
	__u32 entry0; /* first word of gpfifo entry */
	__u32 entry1; /* second word of gpfifo entry */
};

struct nvgpu_get_param_args {
	__u32 value;
};

struct nvgpu_channel_open_args {
	union {
		__s32 channel_fd; /* deprecated: use out.channel_fd instead */
		struct {
			 /* runlist_id is the runlist for the
			  * channel. Basically, the runlist specifies the target
			  * engine(s) for which the channel is
			  * opened. Runlist_id -1 is synonym for the primary
			  * graphics runlist. */
			__s32 runlist_id;
		} in;
		struct {
			__s32 channel_fd;
		} out;
	};
};

struct nvgpu_set_nvmap_fd_args {
	__u32 fd;
};

#define NVGPU_ALLOC_OBJ_FLAGS_LOCKBOOST_ZERO	(1 << 0)
#define NVGPU_ALLOC_OBJ_FLAGS_GFXP		(1 << 1)
#define NVGPU_ALLOC_OBJ_FLAGS_CILP		(1 << 2)

struct nvgpu_alloc_obj_ctx_args {
	__u32 class_num; /* kepler3d, 2d, compute, etc       */
	__u32 flags;     /* input, output */
	__u64 obj_id;    /* output, used to free later       */
};

/* Deprecated. Use the SETUP_BIND IOCTL instead. */
struct nvgpu_alloc_gpfifo_ex_args {
	__u32 num_entries;
	__u32 num_inflight_jobs;
#define NVGPU_ALLOC_GPFIFO_EX_FLAGS_VPR_ENABLED		(1 << 0)
#define NVGPU_ALLOC_GPFIFO_EX_FLAGS_DETERMINISTIC	(1 << 1)
	__u32 flags;
	__u32 reserved[5];
};

/*
 * Setup the channel and bind it (enable).
 */
struct nvgpu_channel_setup_bind_args {
	__u32 num_gpfifo_entries;
	__u32 num_inflight_jobs;
#define NVGPU_CHANNEL_SETUP_BIND_FLAGS_VPR_ENABLED		(1 << 0)
#define NVGPU_CHANNEL_SETUP_BIND_FLAGS_DETERMINISTIC	(1 << 1)
#define NVGPU_CHANNEL_SETUP_BIND_FLAGS_REPLAYABLE_FAULTS_ENABLE   (1 << 2)
#define NVGPU_CHANNEL_SETUP_BIND_FLAGS_USERMODE_SUPPORT	(1 << 3)
#define NVGPU_CHANNEL_SETUP_BIND_FLAGS_USERMODE_GPU_MAP_RESOURCES_SUPPORT	(1 << 4)
	__u32 flags;
	__s32 userd_dmabuf_fd;	/* in */
	__s32 gpfifo_dmabuf_fd;	/* in */
	__u32 work_submit_token; /* out */
	__u64 userd_dmabuf_offset; /* in */
	__u64 gpfifo_dmabuf_offset; /* in */
	__u64 gpfifo_gpu_va; /* out */
	__u64 userd_gpu_va; /* out */
	__u64 usermode_mmio_gpu_va; /* out */
	__u32 reserved[9];
};

struct nvgpu_fence {
	__u32 id;        /* syncpoint id or sync fence fd */
	__u32 value;     /* syncpoint value (discarded when using sync fence) */
};

#define NVGPU_SUBMIT_GPFIFO_FLAGS_FENCE_WAIT	(1 << 0)
#define NVGPU_SUBMIT_GPFIFO_FLAGS_FENCE_GET	(1 << 1)
#define NVGPU_SUBMIT_GPFIFO_FLAGS_HW_FORMAT	(1 << 2)
#define NVGPU_SUBMIT_GPFIFO_FLAGS_SYNC_FENCE	(1 << 3)
#define NVGPU_SUBMIT_GPFIFO_FLAGS_SUPPRESS_WFI	(1 << 4)
#define NVGPU_SUBMIT_GPFIFO_FLAGS_SKIP_BUFFER_REFCOUNTING	(1 << 5)

struct nvgpu_submit_gpfifo_args {
	__u64 gpfifo;
	__u32 num_entries;
	__u32 flags;
	struct nvgpu_fence fence;
};

struct nvgpu_wait_args {
#define NVGPU_WAIT_TYPE_NOTIFIER	0x0
#define NVGPU_WAIT_TYPE_SEMAPHORE	0x1
	__u32 type;
	__u32 timeout;
	union {
		struct {
			__u32 dmabuf_fd;
			__u32 offset;
			__u32 padding1;
			__u32 padding2;
		} notifier;
		struct {
			__u32 dmabuf_fd;
			__u32 offset;
			__u32 payload;
			__u32 padding;
		} semaphore;
	} condition;
};

struct nvgpu_set_timeout_args {
	__u32 timeout;
};

struct nvgpu_set_timeout_ex_args {
	__u32 timeout;
	__u32 flags;
};

struct nvgpu_zcull_bind_args {
	__u64 gpu_va;
	__u32 mode;
	__u32 padding;
};

struct nvgpu_set_error_notifier {
	__u64 offset;
	__u64 size;
	__u32 mem;
	__u32 padding;
};

struct nvgpu_notification {
	struct {
		__u32 nanoseconds[2];
	} time_stamp;
	__u32 info32;
#define	NVGPU_CHANNEL_FIFO_ERROR_IDLE_TIMEOUT		8
#define	NVGPU_CHANNEL_GR_ERROR_SW_METHOD		12
#define	NVGPU_CHANNEL_GR_ERROR_SW_NOTIFY		13
#define	NVGPU_CHANNEL_GR_EXCEPTION			13
#define	NVGPU_CHANNEL_GR_SEMAPHORE_TIMEOUT		24
#define	NVGPU_CHANNEL_GR_ILLEGAL_NOTIFY			25
#define	NVGPU_CHANNEL_FIFO_ERROR_MMU_ERR_FLT		31
#define	NVGPU_CHANNEL_PBDMA_ERROR			32
#define NVGPU_CHANNEL_FECS_ERR_UNIMP_FIRMWARE_METHOD	37
#define	NVGPU_CHANNEL_RESETCHANNEL_VERIF_ERROR		43
#define	NVGPU_CHANNEL_PBDMA_PUSHBUFFER_CRC_MISMATCH	80
	__u16 info16;
	__u16 status;
#define	NVGPU_CHANNEL_SUBMIT_TIMEOUT		1
};

struct nvgpu_channel_wdt_args {
	__u32 wdt_status;
	__u32 timeout_ms;
};
#define NVGPU_IOCTL_CHANNEL_DISABLE_WDT		  (1 << 0)
#define NVGPU_IOCTL_CHANNEL_ENABLE_WDT		  (1 << 1)
#define NVGPU_IOCTL_CHANNEL_WDT_FLAG_SET_TIMEOUT  (1 << 2)
#define NVGPU_IOCTL_CHANNEL_WDT_FLAG_DISABLE_DUMP (1 << 3)

struct nvgpu_runlist_interleave_args {
	__u32 level;
	__u32 reserved;
};
#define NVGPU_RUNLIST_INTERLEAVE_LEVEL_LOW	0
#define NVGPU_RUNLIST_INTERLEAVE_LEVEL_MEDIUM	1
#define NVGPU_RUNLIST_INTERLEAVE_LEVEL_HIGH	2
#define NVGPU_RUNLIST_INTERLEAVE_NUM_LEVELS	3

struct nvgpu_timeslice_args {
	__u32 timeslice_us;
	__u32 reserved;
};

struct nvgpu_event_id_ctrl_args {
	__u32 cmd;
	__u32 event_id;
	__s32 event_fd;
	__u32 padding;
};

struct nvgpu_preemption_mode_args {
#define NVGPU_GRAPHICS_PREEMPTION_MODE_WFI              (1 << 0)
#define NVGPU_GRAPHICS_PREEMPTION_MODE_GFXP		(1 << 1)
	__u32 graphics_preempt_mode;
#define NVGPU_COMPUTE_PREEMPTION_MODE_WFI               (1 << 0)
#define NVGPU_COMPUTE_PREEMPTION_MODE_CTA               (1 << 1)
#define NVGPU_COMPUTE_PREEMPTION_MODE_CILP		(1 << 2)
	__u32 compute_preempt_mode;
};

struct nvgpu_boosted_ctx_args {
#define NVGPU_BOOSTED_CTX_MODE_NORMAL			(0U)
#define NVGPU_BOOSTED_CTX_MODE_BOOSTED_EXECUTION	(1U)
	__u32 boost;
	__u32 padding;
};

struct nvgpu_get_user_syncpoint_args {
	__u64 gpu_va;
	__u32 syncpoint_id;
	__u32 syncpoint_max;
};

struct nvgpu_reschedule_runlist_args {
#define NVGPU_RESCHEDULE_RUNLIST_PREEMPT_NEXT           (1 << 0)
	__u32 flags;
};

#define NVGPU_IOCTL_CHANNEL_SET_NVMAP_FD	\
	_IOW(NVGPU_IOCTL_MAGIC, 5, struct nvgpu_set_nvmap_fd_args)
#define NVGPU_IOCTL_CHANNEL_SET_TIMEOUT	\
	_IOW(NVGPU_IOCTL_MAGIC, 11, struct nvgpu_set_timeout_args)
#define NVGPU_IOCTL_CHANNEL_GET_TIMEDOUT	\
	_IOR(NVGPU_IOCTL_MAGIC, 12, struct nvgpu_get_param_args)
#define NVGPU_IOCTL_CHANNEL_SET_TIMEOUT_EX	\
	_IOWR(NVGPU_IOCTL_MAGIC, 18, struct nvgpu_set_timeout_ex_args)
#define NVGPU_IOCTL_CHANNEL_WAIT		\
	_IOWR(NVGPU_IOCTL_MAGIC, 102, struct nvgpu_wait_args)
#define NVGPU_IOCTL_CHANNEL_SUBMIT_GPFIFO	\
	_IOWR(NVGPU_IOCTL_MAGIC, 107, struct nvgpu_submit_gpfifo_args)
#define NVGPU_IOCTL_CHANNEL_ALLOC_OBJ_CTX	\
	_IOWR(NVGPU_IOCTL_MAGIC, 108, struct nvgpu_alloc_obj_ctx_args)
#define NVGPU_IOCTL_CHANNEL_ZCULL_BIND		\
	_IOWR(NVGPU_IOCTL_MAGIC, 110, struct nvgpu_zcull_bind_args)
#define NVGPU_IOCTL_CHANNEL_SET_ERROR_NOTIFIER  \
	_IOWR(NVGPU_IOCTL_MAGIC, 111, struct nvgpu_set_error_notifier)
#define NVGPU_IOCTL_CHANNEL_OPEN	\
	_IOR(NVGPU_IOCTL_MAGIC,  112, struct nvgpu_channel_open_args)
#define NVGPU_IOCTL_CHANNEL_ENABLE	\
	_IO(NVGPU_IOCTL_MAGIC,  113)
#define NVGPU_IOCTL_CHANNEL_DISABLE	\
	_IO(NVGPU_IOCTL_MAGIC,  114)
#define NVGPU_IOCTL_CHANNEL_PREEMPT	\
	_IO(NVGPU_IOCTL_MAGIC,  115)
#define NVGPU_IOCTL_CHANNEL_FORCE_RESET	\
	_IO(NVGPU_IOCTL_MAGIC,  116)
#define NVGPU_IOCTL_CHANNEL_EVENT_ID_CTRL \
	_IOWR(NVGPU_IOCTL_MAGIC, 117, struct nvgpu_event_id_ctrl_args)
#define NVGPU_IOCTL_CHANNEL_WDT \
	_IOW(NVGPU_IOCTL_MAGIC, 119, struct nvgpu_channel_wdt_args)
#define NVGPU_IOCTL_CHANNEL_SET_RUNLIST_INTERLEAVE \
	_IOW(NVGPU_IOCTL_MAGIC, 120, struct nvgpu_runlist_interleave_args)
#define NVGPU_IOCTL_CHANNEL_SET_PREEMPTION_MODE \
	_IOW(NVGPU_IOCTL_MAGIC, 122, struct nvgpu_preemption_mode_args)
#define NVGPU_IOCTL_CHANNEL_ALLOC_GPFIFO_EX	\
	_IOW(NVGPU_IOCTL_MAGIC, 123, struct nvgpu_alloc_gpfifo_ex_args)
#define NVGPU_IOCTL_CHANNEL_SET_BOOSTED_CTX	\
	_IOW(NVGPU_IOCTL_MAGIC, 124, struct nvgpu_boosted_ctx_args)
#define NVGPU_IOCTL_CHANNEL_GET_USER_SYNCPOINT \
	_IOR(NVGPU_IOCTL_MAGIC, 126, struct nvgpu_get_user_syncpoint_args)
#define NVGPU_IOCTL_CHANNEL_RESCHEDULE_RUNLIST	\
	_IOW(NVGPU_IOCTL_MAGIC, 127, struct nvgpu_reschedule_runlist_args)
#define NVGPU_IOCTL_CHANNEL_SETUP_BIND	\
	_IOWR(NVGPU_IOCTL_MAGIC, 128, struct nvgpu_channel_setup_bind_args)

#define NVGPU_IOCTL_CHANNEL_LAST	\
	_IOC_NR(NVGPU_IOCTL_CHANNEL_SETUP_BIND)
#define NVGPU_IOCTL_CHANNEL_MAX_ARG_SIZE \
	sizeof(struct nvgpu_channel_setup_bind_args)

#endif
