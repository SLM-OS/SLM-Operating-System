/*
 * gpu-compute-smoke.c — Linux userspace experiment harness for
 * resolving #291 (MME_FE1 exception on AMPERE_COMPUTE_B first
 * pushbuffer).
 *
 * Shares channel setup with gpu-channel-helper.c; the only thing
 * that varies is the pushbuffer sent after setup. The caller picks
 * which MME-init prelude to try via --experiment N:
 *
 *   0: Baseline. SET_OBJECT(COMPUTE_B) + SEMAPHORE_RELEASE. This is
 *      the known-failing case (MME_FE1 exception); re-running it
 *      keeps the diff vs the other experiments honest.
 *   1: Skip SET_OBJECT. Just emit SEM methods on COMPUTE_B. Tests
 *      whether nvgpu's ALLOC_OBJ_CTX already bound the class (so
 *      a pushbuffer-side SET_OBJECT is redundant).
 *   2: Upload a 1-instruction end-next MME macro at idx 0, then
 *      SET_OBJECT + SEM. Tests whether an empty IRAM is the root
 *      cause (tu104 encoding: {0, 0, 0x00000001} = end_next=1,
 *      everything else zero).
 *   3: SHADOW_RAM_CONTROL = PASSTHROUGH first (no SET_OBJECT), then
 *      SEM. Tests whether disabling MME tracking skips FE1 lookup.
 *   4: Combined — upload 1-inst MME + SHADOW_RAM=PASSTHROUGH +
 *      SET_OBJECT + SEM. Safety-belt approach.
 *   5: Upload 28 identical 1-inst MME macros at separate IRAM
 *      slots (matching NVK_MME_COUNT), SET_MME_DATA_FIFO_CONFIG
 *      on the 3D class (NVC797), then SET_OBJECT + SEM. Closest
 *      to NVK's init-before-first-compute shape, but with
 *      skeleton macros instead of real ucode.
 *
 * Usage: sudo ./gpu-compute-smoke --experiment N
 *
 * The tool exits cleanly (no channel persistence, no kexec
 * handoff). dmesg should be tailed in a second terminal to
 * capture any MME_FE1 / PBDMA exceptions.
 *
 * Compile on Jetson: gcc -O2 -Wall -o gpu-compute-smoke gpu-compute-smoke.c
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <stdint.h>
#include <errno.h>
#include <linux/types.h>

#include "/usr/src/nvidia/nvgpu/include/uapi/linux/nvgpu.h"
#include "/usr/src/nvidia/nvgpu/include/uapi/linux/nvgpu-as.h"
#include "/usr/src/nvidia/nvgpu/include/uapi/linux/nvgpu-ctrl.h"
#include "/usr/src/nvidia/nvidia-oot/include/uapi/linux/nvmap.h"

#include "gpu-launch-common.h"

#define SEM_PAYLOAD  0x0000CAFEu

/* NV906F/NVC56F pushbuffer header opcodes (bits 31:29). */
#define HDR_OP_INC     (1u << 29)
#define HDR_OP_NON_INC (3u << 29)
#define HDR_OP_IMMD    (4u << 29)
#define HDR_OP_1INC    (5u << 29)

/* Build a method header. byte_off is the class method offset (bytes),
 * NOT a method_id — the macro divides by 4 to form method_id at [12:0]. */
#define HDR_INC(count, subch, byte_off)                              \
    (HDR_OP_INC | (((uint32_t)(count) & 0x1FFF) << 16) |             \
     (((uint32_t)(subch) & 0x7) << 13) |                             \
     (((uint32_t)(byte_off) >> 2) & 0x1FFF))

#define HDR_NONINC(count, subch, byte_off)                           \
    (HDR_OP_NON_INC | (((uint32_t)(count) & 0x1FFF) << 16) |         \
     (((uint32_t)(subch) & 0x7) << 13) |                             \
     (((uint32_t)(byte_off) >> 2) & 0x1FFF))

#define HDR_IMMD(subch, byte_off, data)                              \
    (HDR_OP_IMMD | (((uint32_t)(data) & 0x1FFF) << 16) |             \
     (((uint32_t)(subch) & 0x7) << 13) |                             \
     (((uint32_t)(byte_off) >> 2) & 0x1FFF))

#define HDR_1INC(count, subch, byte_off)                             \
    (HDR_OP_1INC | (((uint32_t)(count) & 0x1FFF) << 16) |            \
     (((uint32_t)(subch) & 0x7) << 13) |                             \
     (((uint32_t)(byte_off) >> 2) & 0x1FFF))

/* Ampere class + subchannel plan. NVK (nv_push.h) puts COMPUTE_B on
 * subch 1 and the 3D class (AMPERE_B, 0xC797) on subch 0. We follow
 * the same mapping — nvgpu binds classes to subchannels by (SET_OBJECT,
 * class_id) tracking, and there's nothing platform-specific. */
#define SUBCH_3D       0
#define SUBCH_COMPUTE  1

#define CLASS_AMPERE_B         0xC797  /* 3D */
#define CLASS_AMPERE_COMPUTE_B 0xC7C0

/* Method offsets (bytes) on COMPUTE_B (NVC7C0) from mesa-clc7c0.h. */
#define NVC7C0_SET_OBJECT                          0x0000
#define NVC7C0_LOAD_MME_INSTRUCTION_RAM_POINTER    0x0114
#define NVC7C0_LOAD_MME_INSTRUCTION_RAM            0x0118
#define NVC7C0_LOAD_MME_START_ADDRESS_RAM_POINTER  0x011C
#define NVC7C0_LOAD_MME_START_ADDRESS_RAM          0x0120
#define NVC7C0_SET_MME_SHADOW_RAM_CONTROL          0x0124
#define NVC7C0_SET_REPORT_SEMAPHORE_PAYLOAD_LOWER  0x0158
#define NVC7C0_SET_REPORT_SEMAPHORE_PAYLOAD_UPPER  0x015C
#define NVC7C0_SET_REPORT_SEMAPHORE_ADDRESS_LOWER  0x0160
#define NVC7C0_SET_REPORT_SEMAPHORE_ADDRESS_UPPER  0x0164
#define NVC7C0_REPORT_SEMAPHORE_EXECUTE            0x0168

/* Method offsets on NVC797 3D class — used by experiment 5. */
#define NVC797_SET_MME_DATA_FIFO_CONFIG            0x0574
#define NVC797_FIFO_SIZE_4KB                       1

#define SHADOW_RAM_MODE_METHOD_TRACK               0
#define SHADOW_RAM_MODE_PASSTHROUGH                2

/* NVK_MME_COUNT: 28 macros in Mesa's NVK. Matched here so experiment 5
 * emits the same number of uploads NVK would. */
#define NVK_MME_COUNT 28

/* tu104 MME encoding: 3 DW / 12 B per instruction (see mesa-mme_tu104.c:
 * mme_tu104_encode). A 1-instruction macro with end_next=1 and all
 * other fields default is {0, 0, 0x00000001} after the word-reversal
 * trick (out[0]=b[2]=0, out[1]=b[1]=0, out[2]=b[0]=end_next). */
static const uint32_t MME_NOP_END[3] = { 0u, 0u, 0x00000001u };

static int xopen(const char *path, int flags)
{
    int fd = open(path, flags);
    if (fd < 0) { perror(path); exit(1); }
    return fd;
}

static void xioctl(int fd, unsigned long req, void *arg, const char *name)
{
    if (ioctl(fd, req, arg) < 0) {
        fprintf(stderr, "%s: %s (errno=%d)\n", name, strerror(errno), errno);
        exit(1);
    }
}

static int nvmap_alloc_dmabuf(int nvmap_fd, uint32_t size, uint32_t align)
{
    struct nvmap_create_handle cr = { .size64 = size };
    xioctl(nvmap_fd, NVMAP_IOC_CREATE_64, &cr, "NVMAP_CREATE");
    uint32_t handle = cr.handle64;

    struct nvmap_alloc_handle al = {
        .handle = handle,
        .heap_mask = 0x40000000,
        .flags = 0x8000003,
        .align = (align < 0x1000) ? 0x1000 : align,
        .numa_nid = -1,
    };
    xioctl(nvmap_fd, NVMAP_IOC_ALLOC, &al, "NVMAP_ALLOC");

    struct nvmap_create_handle gf = { .handle = handle };
    xioctl(nvmap_fd, NVMAP_IOC_GET_FD, &gf, "NVMAP_GET_FD");
    return gf.fd;
}

/* Build the pushbuffer variant selected by experiment number.
 * Returns dword count. sem_gva is the GPU VA of the 4-byte sema. */
static size_t build_pb(uint32_t *pb, size_t cap, int experiment,
                       uint64_t sem_gva)
{
    uint32_t *p = pb;
    uint32_t *end = pb + cap;

#define EMIT(v)                                                      \
    do {                                                             \
        if (p >= end) { fprintf(stderr, "pb overflow\n"); exit(1); } \
        *p++ = (v);                                                  \
    } while (0)

    /* Prelude: MME-init variant based on experiment. */
    if (experiment == 2 || experiment == 4) {
        /* Upload a 1-instruction end-next macro to IRAM[0] as macro 0. */
        EMIT(HDR_INC(2, SUBCH_COMPUTE,
                     NVC7C0_LOAD_MME_START_ADDRESS_RAM_POINTER));
        EMIT(0);   /* macro id */
        EMIT(0);   /* start address in IRAM */
        EMIT(HDR_1INC(1 + 3, SUBCH_COMPUTE,
                      NVC7C0_LOAD_MME_INSTRUCTION_RAM_POINTER));
        EMIT(0);   /* IRAM write pointer */
        EMIT(MME_NOP_END[0]);
        EMIT(MME_NOP_END[1]);
        EMIT(MME_NOP_END[2]);
    }
    if (experiment == 5) {
        /* Upload 28 identical 1-inst macros at consecutive IRAM slots. */
        for (unsigned mme_id = 0; mme_id < NVK_MME_COUNT; mme_id++) {
            uint32_t iram_pos = mme_id * 3;
            EMIT(HDR_INC(2, SUBCH_COMPUTE,
                         NVC7C0_LOAD_MME_START_ADDRESS_RAM_POINTER));
            EMIT(mme_id);
            EMIT(iram_pos);
            EMIT(HDR_1INC(1 + 3, SUBCH_COMPUTE,
                          NVC7C0_LOAD_MME_INSTRUCTION_RAM_POINTER));
            EMIT(iram_pos);
            EMIT(MME_NOP_END[0]);
            EMIT(MME_NOP_END[1]);
            EMIT(MME_NOP_END[2]);
        }
        /* NVC797 SET_MME_DATA_FIFO_CONFIG = SIZE_4KB on 3D subch.
         * Must precede SET_OBJECT(COMPUTE_B) per NVK. This assumes
         * ALLOC_OBJ_CTX with AMPERE_COMPUTE_B already brought up
         * enough of the shared GR state for the 3D class to be
         * implicitly reachable too — which may not be true. If not,
         * exp 5 will fail in a different way than exp 4. */
        EMIT(HDR_IMMD(SUBCH_3D, NVC797_SET_MME_DATA_FIFO_CONFIG,
                      NVC797_FIFO_SIZE_4KB));
    }
    if (experiment == 3 || experiment == 4) {
        EMIT(HDR_IMMD(SUBCH_COMPUTE, NVC7C0_SET_MME_SHADOW_RAM_CONTROL,
                      SHADOW_RAM_MODE_PASSTHROUGH));
    }

    /* Core: SET_OBJECT on most experiments, skip for the "is SET_OBJECT
     * redundant?" cases (1, 3). */
    if (experiment == 0 || experiment == 2 || experiment == 4 ||
        experiment == 5) {
        EMIT(HDR_INC(1, SUBCH_COMPUTE, NVC7C0_SET_OBJECT));
        EMIT(CLASS_AMPERE_COMPUTE_B | (0u << 16));  /* engine_id 0 */
    }

    /* Universal sema body: program address + payload, then RELEASE. */
    EMIT(HDR_INC(1, SUBCH_COMPUTE, NVC7C0_SET_REPORT_SEMAPHORE_PAYLOAD_LOWER));
    EMIT(SEM_PAYLOAD);
    EMIT(HDR_INC(1, SUBCH_COMPUTE, NVC7C0_SET_REPORT_SEMAPHORE_PAYLOAD_UPPER));
    EMIT(0);
    EMIT(HDR_INC(1, SUBCH_COMPUTE, NVC7C0_SET_REPORT_SEMAPHORE_ADDRESS_LOWER));
    EMIT((uint32_t)(sem_gva & 0xFFFFFFFFu));
    EMIT(HDR_INC(1, SUBCH_COMPUTE, NVC7C0_SET_REPORT_SEMAPHORE_ADDRESS_UPPER));
    EMIT((uint32_t)((sem_gva >> 32) & 0xFFu));
    EMIT(HDR_INC(1, SUBCH_COMPUTE, NVC7C0_REPORT_SEMAPHORE_EXECUTE));
    /* OPERATION_RELEASE (0) | STRUCTURE_SIZE_ONE_WORD (1 << 3) */
    EMIT(0x8u);

#undef EMIT

    return p - pb;
}

int main(int argc, char **argv)
{
    setbuf(stdout, NULL);

    int experiment = 0;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--experiment") == 0 && i + 1 < argc) {
            experiment = atoi(argv[++i]);
        }
    }
    if (experiment < 0 || experiment > 5) {
        fprintf(stderr, "experiment must be 0-5 (see header comment)\n");
        return 2;
    }
    printf("[smoke] experiment=%d\n", experiment);

    int nvmap_fd = xopen("/dev/nvmap", O_RDWR);

    int ctrl_fd = open("/dev/nvgpu/igpu0/ctrl", O_RDWR);
    if (ctrl_fd < 0) ctrl_fd = xopen("/dev/nvhost-ctrl-gpu", O_RDWR);

    struct nvgpu_alloc_as_args as_args;
    memset(&as_args, 0, sizeof(as_args));
    as_args.va_range_start = 0x4000000ULL;
    as_args.va_range_end   = 0x2000000000ULL;
    xioctl(ctrl_fd, NVGPU_GPU_IOCTL_ALLOC_AS, &as_args, "ALLOC_AS");
    int as_fd = as_args.as_fd;

    struct nvgpu_gpu_open_tsg_args tsg_args;
    memset(&tsg_args, 0, sizeof(tsg_args));
    xioctl(ctrl_fd, NVGPU_GPU_IOCTL_OPEN_TSG, &tsg_args, "OPEN_TSG");
    int tsg_fd = tsg_args.tsg_fd;

    struct nvgpu_gpu_open_channel_args ch_args;
    memset(&ch_args, 0, sizeof(ch_args));
    ch_args.in.runlist_id = -1;
    xioctl(ctrl_fd, NVGPU_GPU_IOCTL_OPEN_CHANNEL, &ch_args, "OPEN_CHANNEL");
    int ch_fd = ch_args.out.channel_fd;

    struct nvgpu_as_bind_channel_args bind_as = { .channel_fd = ch_fd };
    xioctl(as_fd, NVGPU_AS_IOCTL_BIND_CHANNEL, &bind_as, "AS_BIND");

    struct nvgpu_tsg_create_subcontext_args subctx;
    memset(&subctx, 0, sizeof(subctx));
    subctx.type  = NVGPU_TSG_SUBCONTEXT_TYPE_ASYNC;
    subctx.as_fd = as_fd;
    xioctl(tsg_fd, NVGPU_TSG_IOCTL_CREATE_SUBCONTEXT, &subctx,
           "TSG_CREATE_SUBCONTEXT");

    struct nvgpu_tsg_bind_channel_ex_args bce;
    memset(&bce, 0, sizeof(bce));
    bce.channel_fd    = ch_fd;
    bce.subcontext_id = subctx.veid;
    xioctl(tsg_fd, NVGPU_TSG_IOCTL_BIND_CHANNEL_EX, &bce,
           "TSG_BIND_CHANNEL_EX");

    struct nvgpu_set_nvmap_fd_args nvm = { .fd = nvmap_fd };
    xioctl(ch_fd, NVGPU_IOCTL_CHANNEL_SET_NVMAP_FD, &nvm, "SET_NVMAP");

    struct nvgpu_channel_wdt_args wdt = {
        .wdt_status = NVGPU_IOCTL_CHANNEL_DISABLE_WDT |
                      NVGPU_IOCTL_CHANNEL_WDT_FLAG_SET_TIMEOUT,
        .timeout_ms = 0xFFFFFFFFu,
    };
    xioctl(ch_fd, NVGPU_IOCTL_CHANNEL_WDT, &wdt, "WDT_DISABLE");

    int userd_dmabuf  = nvmap_alloc_dmabuf(nvmap_fd, 4096, 4096);
    int gpfifo_dmabuf = nvmap_alloc_dmabuf(nvmap_fd,
                                           GPU_LAUNCH_GPFIFO_BYTES, 4096);

    struct nvgpu_channel_setup_bind_args sb;
    memset(&sb, 0, sizeof(sb));
    sb.num_gpfifo_entries = GPU_LAUNCH_GPFIFO_ENTRIES;
    sb.flags = NVGPU_CHANNEL_SETUP_BIND_FLAGS_DETERMINISTIC |
               NVGPU_CHANNEL_SETUP_BIND_FLAGS_USERMODE_SUPPORT;
    sb.userd_dmabuf_fd  = userd_dmabuf;
    sb.gpfifo_dmabuf_fd = gpfifo_dmabuf;
    xioctl(ch_fd, NVGPU_IOCTL_CHANNEL_SETUP_BIND, &sb, "SETUP_BIND");
    printf("[smoke] work_submit_token=0x%x\n", sb.work_submit_token);

    struct nvgpu_alloc_obj_ctx_args octx = {
        .class_num = CLASS_AMPERE_COMPUTE_B,
    };
    xioctl(ch_fd, NVGPU_IOCTL_CHANNEL_ALLOC_OBJ_CTX, &octx, "ALLOC_OBJ_CTX");

    struct nvgpu_preemption_mode_args pm = {
        .compute_preempt_mode = NVGPU_COMPUTE_PREEMPTION_MODE_CILP,
    };
    xioctl(ch_fd, NVGPU_IOCTL_CHANNEL_SET_PREEMPTION_MODE, &pm,
           "SET_PREEMPT_MODE");

    int notifier_dmabuf = nvmap_alloc_dmabuf(nvmap_fd, 4096, 4096);
    struct nvgpu_set_error_notifier en = {
        .offset = 0, .size = 4096, .mem = notifier_dmabuf,
    };
    xioctl(ch_fd, NVGPU_IOCTL_CHANNEL_SET_ERROR_NOTIFIER, &en,
           "SET_ERROR_NOTIFIER");

    int pb_dmabuf  = nvmap_alloc_dmabuf(nvmap_fd, 65536, 4096);
    int sem_dmabuf = nvmap_alloc_dmabuf(nvmap_fd, 4096, 4096);

    struct nvgpu_gpu_register_buffer_args regbuf;
    int reg_fds[] = { pb_dmabuf, sem_dmabuf };
    for (size_t i = 0; i < sizeof(reg_fds)/sizeof(reg_fds[0]); i++) {
        memset(&regbuf, 0, sizeof(regbuf));
        regbuf.dmabuf_fd = reg_fds[i];
        regbuf.comptags_alloc_control = NVGPU_GPU_COMPTAGS_ALLOC_NONE;
        ioctl(ctrl_fd, NVGPU_GPU_IOCTL_REGISTER_BUFFER, &regbuf);
    }

    struct nvgpu_as_map_buffer_ex_args pb_map;
    memset(&pb_map, 0, sizeof(pb_map));
    pb_map.compr_kind = -1;
    pb_map.incompr_kind = 0;
    pb_map.dmabuf_fd = pb_dmabuf;
    pb_map.page_size = 4096;
    xioctl(as_fd, NVGPU_AS_IOCTL_MAP_BUFFER_EX, &pb_map, "MAP_PB");

    struct nvgpu_as_map_buffer_ex_args sem_map;
    memset(&sem_map, 0, sizeof(sem_map));
    sem_map.compr_kind = -1;
    sem_map.incompr_kind = 0;
    sem_map.dmabuf_fd = sem_dmabuf;
    sem_map.page_size = 4096;
    xioctl(as_fd, NVGPU_AS_IOCTL_MAP_BUFFER_EX, &sem_map, "MAP_SEM");

    xioctl(tsg_fd, NVGPU_IOCTL_TSG_ENABLE, NULL, "TSG_ENABLE");

    void *userd_va  = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_SHARED,
                           userd_dmabuf, 0);
    void *gpfifo_va = mmap(NULL, GPU_LAUNCH_GPFIFO_BYTES,
                           PROT_READ | PROT_WRITE, MAP_SHARED,
                           gpfifo_dmabuf, 0);
    void *pb_va     = mmap(NULL, 65536, PROT_READ | PROT_WRITE, MAP_SHARED,
                           pb_dmabuf, 0);
    void *sem_va    = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_SHARED,
                           sem_dmabuf, 0);
    if (userd_va == MAP_FAILED || gpfifo_va == MAP_FAILED ||
        pb_va == MAP_FAILED || sem_va == MAP_FAILED) {
        perror("mmap");
        return 1;
    }

    /* Build the experiment pushbuffer. */
    uint32_t *pb32 = (uint32_t *)pb_va;
    size_t pb_dw = build_pb(pb32, 65536 / 4, experiment,
                            (uint64_t)sem_map.offset);
    msync(pb_va, pb_dw * 4, MS_SYNC);

    printf("[smoke] pushbuffer: %zu dwords\n", pb_dw);
    for (size_t i = 0; i < pb_dw; i++) {
        printf("  [%02zu] 0x%08x\n", i, pb32[i]);
    }

    *(volatile uint32_t *)sem_va = 0;
    msync(sem_va, 4096, MS_SYNC);

    /* GPFIFO entry 0 -> pushbuffer. */
    uint64_t pb_gva = pb_map.offset;
    uint32_t gp_e0 = (uint32_t)(pb_gva & 0xFFFFFFFCu);
    uint32_t gp_e1 = (uint32_t)((pb_gva >> 32) & 0xFFu) |
                     ((uint32_t)pb_dw << 10);
    ((uint32_t *)gpfifo_va)[0] = gp_e0;
    ((uint32_t *)gpfifo_va)[1] = gp_e1;
    msync(gpfifo_va, 8, MS_SYNC);

    /* GP_PUT = 1 in USERD word 35. */
    ((uint32_t *)userd_va)[35] = 1;
    msync(userd_va, 4096, MS_SYNC);

    /* Doorbell at ctrl-fd mmap + 0x90. */
    void *doorbell_page = mmap(NULL, 4096, PROT_READ | PROT_WRITE,
                               MAP_SHARED, ctrl_fd, 0);
    if (doorbell_page == MAP_FAILED) { perror("mmap doorbell"); return 1; }
    volatile uint32_t *doorbell =
        (volatile uint32_t *)((char *)doorbell_page + 0x90);
    *doorbell = sb.work_submit_token;
    __asm__ volatile("dsb sy" ::: "memory");

    /* Poll sema for up to 1s. */
    uint32_t sem_val = 0;
    for (int i = 0; i < 100; i++) {
        sem_val = *(volatile uint32_t *)sem_va;
        if (sem_val == SEM_PAYLOAD) break;
        usleep(10000);
    }
    uint32_t gp_get = ((volatile uint32_t *)userd_va)[34];
    printf("[smoke] result: sem=0x%08x (want 0x%x) GP_GET=%u (want 1)\n",
           sem_val, SEM_PAYLOAD, gp_get);
    if (sem_val == SEM_PAYLOAD)
        printf("[smoke] SUCCESS: sema fired, method dispatched\n");
    else if (gp_get == 1)
        printf("[smoke] PARTIAL: PBDMA consumed entry but method didn't\n");
    else
        printf("[smoke] FAIL: PBDMA didn't walk the entry\n");

    /* Explicitly close fds so the channel is torn down before exit
     * (avoid leaving a dead channel on the TSG for the next run). */
    close(ch_fd);
    close(tsg_fd);
    close(as_fd);
    close(ctrl_fd);
    close(nvmap_fd);
    return (sem_val == SEM_PAYLOAD) ? 0 : 1;
}
