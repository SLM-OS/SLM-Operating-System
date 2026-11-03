/*
 * test_ga10b_pmu.c - GA10B LS PMU RTOS host-side bring-up (#1088).
 *
 * Drives ga10b_pmu.c against a fake BAR0 + fake PMU firmware:
 *  - BROM: BR_RETCODE reads 1 (running) a few times, then 3 (pass);
 *    HWCFG2 holds the priv-lockdown bit for a few reads.
 *  - When lockdown releases, the fake posts the INIT message in the
 *    super-surface FB message queue (MSGQ_HEAD 0 -> 1, IRQ SWGEN0)
 *    and two lines in the RISC-V debug buffer (DMEM, IRQ SWGEN1).
 *  - A write to the LPQ head register makes the fake read the FBQ
 *    command element, write the RPC status back into it and post a
 *    reply message (optionally preceded by an event).
 *
 * Expected wire values are worked out from nvgpu (see ga10b_pmu.h)
 * and the jetson-nano-2 trace (queue id 1 for the LPQ, 2 tail writes
 * per message, MAILBOX1 = 0x8060_0000 | addr_hi).
 */
#include "unity.h"
#include "../gpu/nvidia/ga10b_pmu.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>

/* ---- Fake hardware ---- */

#define NREGS 64
#define NLOG  1024
#define SW_OFF   0x1234u   /* sw_managed_area_offset -> heap base 0x1240 */
#define SW_SIZE  0x0800u
#define HEAP_BASE 0x1240u

struct fake {
    struct { uint32_t addr, val; } regs[NREGS];
    int nregs;
    struct { uint32_t addr, val; } wlog[NLOG];
    int nw;
    uint8_t dmem[0x10000];
    uint32_t dmemc;
    bool started;
    int brom_running_reads;
    bool brom_fail;
    int lockdown_reads;
    bool init_posted;
    uint8_t init_unit;
    uint8_t init_function;
    bool sw_area_override;   /* use sw_off/sw_size below */
    uint16_t sw_off, sw_size;
    uint32_t pmu_msg_head;
    uint16_t seqno;
    uint32_t cmd_head;
    uint8_t rpc_status;
    bool event_first;
    bool reply_unhandled;
    bool no_reply;
    bool halt_on_cmd;
    uint64_t delay_total_us;
    uint32_t delays_ms_seen[8];
    int ndelays_ms;
    int ncmds;
    struct ga10b_falcon_fbq_hdr fbq[4];
    struct ga10b_pmu_rpc_cmd_full cmd[4];
    uint8_t rpc[4][64];
    char lines[8][160];
    int nlines;
};

static struct fake F;
static alignas(4096) uint8_t t_ss[GA10B_PMU_SUPER_SURFACE_SIZE];
static alignas(4096) uint8_t t_boot[4096];
static alignas(4096) uint8_t t_dump[GA10B_PMU_CORE_DUMP_SIZE];
static alignas(4096) uint8_t t_trace[GA10B_PMU_TRACE_BUF_SIZE];
static alignas(4096) uint32_t t_inst[GA10B_PMU_INST_BLOCK_SIZE / 4u];
static struct ga10b_pmu t_pmu;

#define BOOT_ARGS_ADDR  0x10761f000ull
#define CORE_DUMP_ADDR  0x107620000ull
#define INST_ADDR       0x10761e000ull
#define SS_VA           0x0000000000100000ull
#define TRACE_VA        0x0000000000210000ull

static uint32_t *reg_slot(uint32_t addr)
{
    for (int i = 0; i < F.nregs; i++)
        if (F.regs[i].addr == addr)
            return &F.regs[i].val;
    if (F.nregs >= NREGS)
        return nullptr;
    F.regs[F.nregs].addr = addr;
    F.regs[F.nregs].val = 0;
    return &F.regs[F.nregs++].val;
}

static uint32_t reg(uint32_t addr) { uint32_t *p = reg_slot(addr); return p ? *p : 0; }
static void set_reg(uint32_t addr, uint32_t v) { uint32_t *p = reg_slot(addr); if (p) *p = v; }

static void put_msg(uint8_t unit, uint8_t flags, uint8_t seq, const void *body, uint8_t body_len)
{
    uint8_t *e = t_ss + GA10B_SS_MSG_ELEMENT_OFFSET(F.pmu_msg_head);
    memset(e, 0, GA10B_FBQ_MSG_ELEMENT_SIZE);
    struct ga10b_falcon_fbq_msgq_hdr mh = { .sequence_number = F.seqno++, .checksum = 0 };
    struct ga10b_pmu_hdr h = { .unit_id = unit, .size = (uint8_t)(4u + body_len),
                               .ctrl_flags = flags, .seq_id = seq };
    memcpy(e, &mh, sizeof(mh));
    memcpy(e + 4, &h, sizeof(h));
    if (body_len)
        memcpy(e + 8, body, body_len);
    F.pmu_msg_head = (F.pmu_msg_head + 1u) % GA10B_FBQ_MSG_NUM_ELEMENTS;
    set_reg(GA10B_PWR_FALCON_IRQSTAT, reg(GA10B_PWR_FALCON_IRQSTAT) | GA10B_FALCON_IRQ_SWGEN0);
}

static void post_init(void)
{
    struct ga10b_pmu_init_msg m;
    memset(&m, 0, sizeof(m));
    m.hdr.unit_id = GA10B_PMU_UNIT_CMDMGMT;
    m.hdr.function = F.init_function;
    m.queue_phy_id[0] = 2;     /* HPQ */
    m.queue_phy_id[1] = 1;     /* LPQ (trace: head register 0x10a804) */
    m.queue_phy_id[2] = 0;
    m.queue_phy_id[3] = 3;     /* message queue */
    m.sw_managed_area_offset = F.sw_area_override ? F.sw_off : SW_OFF;
    m.sw_managed_area_size = F.sw_area_override ? F.sw_size : SW_SIZE;
    m.os_debug_entry_point = 0xFFFF;
    /* 4 + 56 bytes so the message fits its 60-byte slot. */
    put_msg(F.init_unit, 0, 0, &m, 56);

    /* RISC-V debug buffer: metadata in the last 16 bytes of 64 KiB
     * DMEM, 0x3f0-byte ring just below it (as on hardware). */
    static const char text[] = "Init PmuApp\0RTOS up";   /* 2 lines, NUL-terminated */
    uint32_t meta[4] = { 0, sizeof(text), 0x3f0, GA10B_NVRISCV_DEBUG_BUFFER_MAGIC };
    memcpy(F.dmem + 0xfff0, meta, sizeof(meta));
    memcpy(F.dmem + 0xfc00, text, sizeof(text));
    set_reg(GA10B_PWR_PMU_QUEUE_HEAD(GA10B_RISCV_DEBUG_BUFFER_QUEUE), sizeof(text));
    set_reg(GA10B_PWR_FALCON_IRQSTAT, reg(GA10B_PWR_FALCON_IRQSTAT) | GA10B_FALCON_IRQ_SWGEN1);
    F.init_posted = true;
}

static void on_command(uint32_t new_head)
{
    uint32_t idx = F.cmd_head;
    F.cmd_head = new_head;
    const uint8_t *e = t_ss + GA10B_SS_CMD_QUEUE_OFFSET(GA10B_PMU_COMMAND_QUEUE_LPQ) +
                       idx * GA10B_FBQ_CMD_ELEMENT_SIZE;
    int n = F.ncmds < 4 ? F.ncmds : 3;
    memcpy(&F.fbq[n], e, sizeof(F.fbq[n]));
    memcpy(&F.cmd[n], e + 8, sizeof(F.cmd[n]));
    memcpy(F.rpc[n], e + 20, sizeof(F.rpc[n]));
    F.ncmds++;
    if (F.halt_on_cmd) {
        set_reg(GA10B_PWR_FALCON_IRQSTAT, reg(GA10B_PWR_FALCON_IRQSTAT) | GA10B_FALCON_IRQ_HALT);
        return;
    }
    if (F.no_reply)
        return;
    /* The PMU writes the RPC back (status at hdr offset 3). */
    t_ss[GA10B_SS_CMD_QUEUE_OFFSET(GA10B_PMU_COMMAND_QUEUE_LPQ) +
         idx * GA10B_FBQ_CMD_ELEMENT_SIZE + 20 + 3] = F.rpc_status;
    if (F.event_first) {
        uint8_t ev[4] = { 1, 2, 3, 4 };
        put_msg(0x12 /* PERFMON */, GA10B_PMU_CMD_FLAGS_EVENT, 0, ev, 4);
    }
    if (F.reply_unhandled) {
        uint8_t rc[3] = { GA10B_PMU_RC_MSG_TYPE_UNHANDLED_CMD, GA10B_PMU_UNIT_ACR, 0 };
        put_msg(GA10B_PMU_UNIT_RC, 0, F.cmd[n].hdr.seq_id, rc, 3);
        return;
    }
    struct ga10b_pmu_rpc_msg m = { .msg_type = GA10B_PMU_RPC_MSG_ID, .flags = 0,
                                   .rpc_dmem_size = F.cmd[n].rpc.rpc_dmem_size,
                                   .rpc_dmem_ptr = F.cmd[n].rpc.rpc_dmem_ptr };
    put_msg(F.cmd[n].hdr.unit_id, GA10B_PMU_CMD_FLAGS_STATUS | GA10B_PMU_CMD_FLAGS_INTR,
            F.cmd[n].hdr.seq_id, &m, sizeof(m));
}

static uint32_t f_rd(void *ctx, uint32_t addr)
{
    (void)ctx;
    switch (addr) {
    case GA10B_PWR_RISCV_BR_RETCODE:
        if (!F.started)
            return 0;
        if (F.brom_running_reads > 0) {
            F.brom_running_reads--;
            return 1;
        }
        return F.brom_fail ? GA10B_RISCV_BR_RETCODE_FAIL : GA10B_RISCV_BR_RETCODE_PASS;
    case GA10B_PWR_FALCON_HWCFG2:
        if (F.started && F.lockdown_reads > 0) {
            F.lockdown_reads--;
            return 0x1a733u;            /* lockdown set (GSP value in trace) */
        }
        if (F.started && !F.init_posted)
            post_init();
        return 0x18733u;                /* trace: PMU after release */
    case GA10B_PWR_FALCON_DMEMD0: {
        uint32_t a = F.dmemc & 0xfffcu, v;
        memcpy(&v, F.dmem + a, 4);
        if (F.dmemc & GA10B_FALCON_DMEMC_AINCR)
            F.dmemc = (F.dmemc & ~0xfffcu) | ((a + 4u) & 0xfffcu);
        return v;
    }
    case GA10B_PWR_PMU_MSGQ_HEAD:
        return F.pmu_msg_head;
    case GA10B_PWR_PMU_QUEUE_HEAD(1):
        return F.cmd_head;
    default:
        return reg(addr);
    }
}

static void f_wr(void *ctx, uint32_t addr, uint32_t val)
{
    (void)ctx;
    if (F.nw < NLOG) {
        F.wlog[F.nw].addr = addr;
        F.wlog[F.nw].val = val;
        F.nw++;
    }
    switch (addr) {
    case GA10B_PWR_RISCV_CPUCTL:
        if (val & GA10B_RISCV_CPUCTL_STARTCPU)
            F.started = true;
        break;
    case GA10B_PWR_FALCON_IRQSCLR:
        set_reg(GA10B_PWR_FALCON_IRQSTAT, reg(GA10B_PWR_FALCON_IRQSTAT) & ~val);
        break;
    case GA10B_PWR_FALCON_DMEMC0:
        F.dmemc = val;
        break;
    case GA10B_PWR_PMU_QUEUE_HEAD(1):
        on_command(val);
        break;
    default:
        set_reg(addr, val);
        break;
    }
}

static void f_delay(void *ctx, uint32_t us)
{
    (void)ctx;
    F.delay_total_us += us;
    if (us >= 1000u && F.ndelays_ms < 8)
        F.delays_ms_seen[F.ndelays_ms++] = us / 1000u;
}

static void f_line(void *ctx, const char *s)
{
    (void)ctx;
    if (F.nlines < 8) {
        size_t n = strlen(s);
        if (n >= sizeof(F.lines[0]))
            n = sizeof(F.lines[0]) - 1u;
        memcpy(F.lines[F.nlines], s, n);
        F.lines[F.nlines][n] = '\0';
        F.nlines++;
    }
}

static const struct ga10b_pmu_ops f_ops = {
    .ctx = nullptr, .rd32 = f_rd, .wr32 = f_wr, .delay_us = f_delay,
    .dma_clean = nullptr, .dma_inval = nullptr, .log_line = f_line,
};

static struct ga10b_pmu_config make_cfg(void)
{
    struct ga10b_pmu_config c;
    memset(&c, 0, sizeof(c));
    c.boot_args = (struct ga10b_pmu_buf){ t_boot, BOOT_ARGS_ADDR, 0, sizeof(t_boot) };
    c.core_dump = (struct ga10b_pmu_buf){ t_dump, CORE_DUMP_ADDR, 0, sizeof(t_dump) };
    c.super_surface = (struct ga10b_pmu_buf){ t_ss, 0x200000000ull, SS_VA, sizeof(t_ss) };
    c.trace_buf = (struct ga10b_pmu_buf){ t_trace, 0x200100000ull, TRACE_VA, sizeof(t_trace) };
    c.inst_block_addr = INST_ADDR;
    c.sysmem_coherent = false;
    return c;
}

static void fake_reset(void)
{
    memset(&F, 0, sizeof(F));
    F.brom_running_reads = 3;
    F.lockdown_reads = 4;
    F.init_unit = GA10B_PMU_UNIT_CMDMGMT;
    set_reg(GA10B_PWR_RISCV_IRQMASK, 0x01c428d0u);   /* trace values after init */
    set_reg(GA10B_PWR_RISCV_IRQDEST, 0x014000d0u);
    set_reg(GA10B_PWR_FALCON_HWCFG, 0x800e0100u);    /* 64 KiB DMEM */
}

static int boot_ready_with_sw_area(bool override, uint16_t off, uint16_t size)
{
    fake_reset();
    F.sw_area_override = override;
    F.sw_off = off;
    F.sw_size = size;
    struct ga10b_pmu_config c = make_cfg();
    int rc = ga10b_pmu_init(&t_pmu, &f_ops, &c);
    if (rc != GA10B_PMU_OK)
        return rc;
    rc = ga10b_pmu_boot(&t_pmu);
    if (rc != GA10B_PMU_OK)
        return rc;
    return ga10b_pmu_wait_init(&t_pmu, 100);
}

static int boot_ready(void)
{
    return boot_ready_with_sw_area(false, 0, 0);
}

static uint32_t get32(const uint8_t *p)
{
    uint32_t v;
    memcpy(&v, p, 4);
    return v;
}

static int find_write(uint32_t addr, int from)
{
    for (int i = from; i < F.nw; i++)
        if (F.wlog[i].addr == addr)
            return i;
    return -1;
}

/* ---- Pure helpers ---- */

static void test_inst_block_config(void)
{
    TEST_ASSERT_EQUAL_HEX32(0x7010761eu, ga10b_pmu_inst_block_config(INST_ADDR, false));
    TEST_ASSERT_EQUAL_HEX32(0x6010761eu, ga10b_pmu_inst_block_config(INST_ADDR, true));
}

static void test_build_inst_block(void)
{
    memset(t_inst, 0xA5, sizeof(t_inst));
    ga10b_pmu_build_inst_block(t_inst, 0x1be4e1000ull, false);
    /* ncoh | vol | 64K big page | ver2 | addr[31:12] */
    TEST_ASSERT_EQUAL_HEX32(0xbe4e1000u | 0x3u | 0x4u | 0x800u | 0x400u, t_inst[128]);
    TEST_ASSERT_EQUAL_HEX32(0x1u, t_inst[129]);
    TEST_ASSERT_EQUAL_HEX32(0u, t_inst[0]);
    TEST_ASSERT_EQUAL_HEX32(0u, t_inst[131]);
    TEST_ASSERT_EQUAL_HEX32(0x1u, t_inst[166]);          /* valid mask: VEID 0 */
    TEST_ASSERT_EQUAL_HEX32(0x0u, t_inst[167]);
    TEST_ASSERT_EQUAL_HEX32(0xbe4e1000u | 0x3u | 0x4u | 0x400u | 0x800u, t_inst[168]);
    TEST_ASSERT_EQUAL_HEX32(0x1u, t_inst[169]);
    TEST_ASSERT_EQUAL_HEX32(0u, t_inst[170]);
    TEST_ASSERT_EQUAL_HEX32(0x1u, t_inst[172]);          /* VEID 1: target invalid */
    TEST_ASSERT_EQUAL_HEX32(0x0u, t_inst[173]);
    TEST_ASSERT_EQUAL_HEX32(0x1u, t_inst[168 + 4 * 63]);
    TEST_ASSERT_EQUAL_HEX32(0u, t_inst[168 + 4 * 64]);
    TEST_ASSERT_EQUAL_HEX32(0u, t_inst[1023]);
}

static void test_boot_params_wire_layout(void)
{
    struct ga10b_pmu_config c = make_cfg();
    struct ga10b_nv_pmu_boot_params bp;
    memset(&bp, 0xCC, sizeof(bp));
    ga10b_pmu_build_boot_params(&bp, &c);
    const uint8_t *r = (const uint8_t *)&bp;
    TEST_ASSERT_EQUAL_HEX32(0x4e56524du, get32(r + 0));         /* 'NVRM' */
    TEST_ASSERT_EQUAL_HEX32(184u | (1u << 16), get32(r + 4));   /* size, version */
    TEST_ASSERT_EQUAL_HEX32((uint32_t)CORE_DUMP_ADDR, get32(r + 8));
    TEST_ASSERT_EQUAL_HEX32((uint32_t)(CORE_DUMP_ADDR >> 32), get32(r + 12));
    TEST_ASSERT_EQUAL_HEX32(8192u, get32(r + 16));
    TEST_ASSERT_EQUAL_HEX32(0u, get32(r + 20));                 /* padding zeroed */
    TEST_ASSERT_EQUAL_HEX32(0u, get32(r + 24));                 /* dummy[0] */
    TEST_ASSERT_EQUAL_HEX32(204000000u, get32(r + 120));        /* cpu_freq_hz */
    TEST_ASSERT_EQUAL_HEX32((uint32_t)TRACE_VA, get32(r + 124));
    TEST_ASSERT_EQUAL_HEX32(0u, get32(r + 128));
    TEST_ASSERT_EQUAL_HEX32(0x01004000u, get32(r + 132));       /* 16K | DMAIDX_VIRT<<24 */
    TEST_ASSERT_EQUAL_HEX32(0u, get32(r + 136));                /* raise_priv_sec */
    TEST_ASSERT_EQUAL_HEX32(0u, get32(r + 140));                /* gc6_ctx */
    TEST_ASSERT_EQUAL_HEX32((uint32_t)SS_VA, get32(r + 164));
    TEST_ASSERT_EQUAL_HEX32(0u, get32(r + 168));
    TEST_ASSERT_EQUAL_HEX32(0x01100000u, get32(r + 172));       /* 1M | DMAIDX_VIRT<<24 */
    TEST_ASSERT_EQUAL_HEX32(0u, get32(r + 176));                /* flags */
    TEST_ASSERT_EQUAL_HEX32(0x7010761eu, get32(r + 180));       /* ctx_bind_addr */
    TEST_ASSERT_EQUAL_HEX64(0x806000010761f000ull, ga10b_pmu_mailbox_addr(BOOT_ARGS_ADDR));
}

static void test_init_rejects_bad_config(void)
{
    struct ga10b_pmu_config c = make_cfg();
    TEST_ASSERT_EQUAL_INT(GA10B_PMU_ERR_ARG, ga10b_pmu_init(&t_pmu, nullptr, &c));
    c.super_surface.size = GA10B_PMU_SUPER_SURFACE_SIZE - 1u;
    TEST_ASSERT_EQUAL_INT(GA10B_PMU_ERR_ARG, ga10b_pmu_init(&t_pmu, &f_ops, &c));
    c = make_cfg();
    c.inst_block_addr = INST_ADDR + 0x800u;
    TEST_ASSERT_EQUAL_INT(GA10B_PMU_ERR_ARG, ga10b_pmu_init(&t_pmu, &f_ops, &c));
    c = make_cfg();
    c.trace_buf.gpu_va = 0;
    TEST_ASSERT_EQUAL_INT(GA10B_PMU_ERR_ARG, ga10b_pmu_init(&t_pmu, &f_ops, &c));
    c = make_cfg();
    TEST_ASSERT_EQUAL_INT(GA10B_PMU_OK, ga10b_pmu_init(&t_pmu, &f_ops, &c));
    TEST_ASSERT_EQUAL_INT(GA10B_PMU_ERR_STATE, ga10b_pmu_poll(&t_pmu));
}

/* ---- Boot ---- */

static void test_boot_sequence(void)
{
    fake_reset();
    struct ga10b_pmu_config c = make_cfg();
    TEST_ASSERT_EQUAL_INT(GA10B_PMU_OK, ga10b_pmu_init(&t_pmu, &f_ops, &c));
    memset(t_ss, 0x77, sizeof(t_ss));
    TEST_ASSERT_EQUAL_INT(GA10B_PMU_OK, ga10b_pmu_boot(&t_pmu));

    /* First three writes: MAILBOX0, MAILBOX1, RISC-V CPUCTL (trace
     * 11253-11256). */
    TEST_ASSERT_TRUE(F.nw >= 3);
    TEST_ASSERT_EQUAL_HEX32(GA10B_PWR_FALCON_MAILBOX0, F.wlog[0].addr);
    TEST_ASSERT_EQUAL_HEX32(0x0761f000u, F.wlog[0].val);
    TEST_ASSERT_EQUAL_HEX32(GA10B_PWR_FALCON_MAILBOX1, F.wlog[1].addr);
    TEST_ASSERT_EQUAL_HEX32(0x80600001u, F.wlog[1].val);
    TEST_ASSERT_EQUAL_HEX32(GA10B_PWR_RISCV_CPUCTL, F.wlog[2].addr);
    TEST_ASSERT_EQUAL_HEX32(1u, F.wlog[2].val);

    /* Boot args in memory; super surface zeroed with its VA in hdr. */
    struct ga10b_nv_pmu_boot_params bp;
    ga10b_pmu_build_boot_params(&bp, &c);
    TEST_ASSERT_EQUAL_MEMORY(&bp, t_boot, sizeof(bp));
    TEST_ASSERT_EQUAL_HEX32((uint32_t)SS_VA, get32(t_ss + 0x10600));
    TEST_ASSERT_EQUAL_HEX32(0u, get32(t_ss + 0x10604));
    TEST_ASSERT_EQUAL_HEX32(0u, get32(t_ss + 0x10608));
    TEST_ASSERT_EQUAL_UINT8(0, t_ss[0x200]);
    TEST_ASSERT_EQUAL_UINT8(0, t_ss[sizeof(t_ss) - 1u]);

    /* BROM: 3 "running" reads -> 3 x 5 ms polls; then lockdown polls. */
    TEST_ASSERT_EQUAL_HEX32(GA10B_RISCV_BR_RETCODE_PASS, t_pmu.brom_retcode);
    TEST_ASSERT_EQUAL_INT(3, F.ndelays_ms);
    TEST_ASSERT_EQUAL_UINT32(5u, F.delays_ms_seen[0]);
    TEST_ASSERT_TRUE(t_pmu.booted);
    TEST_ASSERT_FALSE(t_pmu.fw_ready);
    TEST_ASSERT_EQUAL_INT(GA10B_PMU_ERR_STATE, ga10b_pmu_boot(&t_pmu));
}

static void test_boot_brom_fail_and_timeout(void)
{
    fake_reset();
    F.brom_fail = true;
    struct ga10b_pmu_config c = make_cfg();
    ga10b_pmu_init(&t_pmu, &f_ops, &c);
    TEST_ASSERT_EQUAL_INT(GA10B_PMU_ERR_BROM, ga10b_pmu_boot(&t_pmu));

    fake_reset();
    F.brom_running_reads = 1000;
    ga10b_pmu_init(&t_pmu, &f_ops, &c);
    TEST_ASSERT_EQUAL_INT(GA10B_PMU_ERR_TIMEOUT, ga10b_pmu_boot(&t_pmu));
    /* 100 ms budget in 5 ms steps. */
    TEST_ASSERT_EQUAL_UINT64(100000u, F.delay_total_us);

    fake_reset();
    F.lockdown_reads = 1 << 30;
    ga10b_pmu_init(&t_pmu, &f_ops, &c);
    TEST_ASSERT_EQUAL_INT(GA10B_PMU_ERR_TIMEOUT, ga10b_pmu_boot(&t_pmu));
}

/* ---- INIT message ---- */

static void test_init_message_and_debug_buffer(void)
{
    TEST_ASSERT_EQUAL_INT(GA10B_PMU_OK, boot_ready());
    TEST_ASSERT_TRUE(t_pmu.fw_ready);
    TEST_ASSERT_EQUAL_UINT32(1u, t_pmu.cmdq_index);           /* queue_phy_id[LPQ] */
    TEST_ASSERT_EQUAL_HEX32(HEAP_BASE, t_pmu.heap_base);
    TEST_ASSERT_EQUAL_UINT32((0x1a20u - HEAP_BASE) / 32u, t_pmu.heap_blks);
    TEST_ASSERT_EQUAL_UINT8(GA10B_PMU_UNIT_CMDMGMT, t_pmu.init_unit_id);
    TEST_ASSERT_EQUAL_HEX16(SW_OFF, t_pmu.init.sw_managed_area_offset);

    /* MSGQ tail advanced to 1 (trace: W 10a4cc 1). */
    int i = find_write(GA10B_PWR_PMU_MSGQ_TAIL, 0);
    TEST_ASSERT_TRUE(i >= 0);
    TEST_ASSERT_EQUAL_HEX32(1u, F.wlog[i].val);

    /* IRQs cleared through IRQSCLR with the masked bits. */
    TEST_ASSERT_EQUAL_HEX32(0u, reg(GA10B_PWR_FALCON_IRQSTAT));

    /* Debug buffer: both lines, read offset written back. */
    TEST_ASSERT_EQUAL_INT(2, F.nlines);
    TEST_ASSERT_EQUAL_STRING("Init PmuApp", F.lines[0]);
    TEST_ASSERT_EQUAL_STRING("RTOS up", F.lines[1]);
    i = find_write(GA10B_PWR_PMU_QUEUE_TAIL(GA10B_RISCV_DEBUG_BUFFER_QUEUE), 0);
    TEST_ASSERT_TRUE(i >= 0);
    TEST_ASSERT_EQUAL_HEX32(20u, F.wlog[i].val);
    TEST_ASSERT_EQUAL_HEX32(0xfc00u, t_pmu.dbg_dmem_offset);
    i = find_write(GA10B_PWR_FALCON_DMEMC0, 0);
    TEST_ASSERT_EQUAL_HEX32(0x0200fff0u, F.wlog[i].val);     /* trace 11265 */
    i = find_write(GA10B_PWR_FALCON_DMEMC0, i + 1);
    TEST_ASSERT_EQUAL_HEX32(0x0200fc00u, F.wlog[i].val);     /* trace 11272 */
}

/* jetson-nano-2's PMU firmware reports its sw-managed area at DMEM
 * offset 0 (size 0x800). nvgpu's bitmap allocator moves a zero base up
 * one block, so the heap starts at 0x20 and the first allocation is not
 * the 0 that means failure. */
static void test_init_message_zero_sw_area_offset(void)
{
    TEST_ASSERT_EQUAL_INT(GA10B_PMU_OK, boot_ready_with_sw_area(true, 0, 0x800));
    TEST_ASSERT_EQUAL_HEX32(0x20u, t_pmu.heap_base);
    TEST_ASSERT_EQUAL_UINT32((0x800u - 0x20u) / 32u, t_pmu.heap_blks);
    TEST_ASSERT_EQUAL_INT(GA10B_PMU_OK, ga10b_pmu_bootstrap_ctxsw_falcons(&t_pmu, 100));
}

static void test_init_message_init_dgpu_unit_accepted(void)
{
    fake_reset();
    F.init_unit = GA10B_PMU_UNIT_INIT_DGPU;
    struct ga10b_pmu_config c = make_cfg();
    ga10b_pmu_init(&t_pmu, &f_ops, &c);
    ga10b_pmu_boot(&t_pmu);
    F.init_unit = GA10B_PMU_UNIT_INIT_DGPU;
    TEST_ASSERT_EQUAL_INT(GA10B_PMU_OK, ga10b_pmu_wait_init(&t_pmu, 10));
    TEST_ASSERT_EQUAL_UINT8(GA10B_PMU_UNIT_INIT_DGPU, t_pmu.init_unit_id);
}

static void test_init_message_rejects_wrong_unit_or_function(void)
{
    fake_reset();
    F.init_unit = GA10B_PMU_UNIT_ACR;
    struct ga10b_pmu_config c = make_cfg();
    ga10b_pmu_init(&t_pmu, &f_ops, &c);
    ga10b_pmu_boot(&t_pmu);
    TEST_ASSERT_EQUAL_INT(GA10B_PMU_ERR_PROTO, ga10b_pmu_wait_init(&t_pmu, 10));
    TEST_ASSERT_FALSE(t_pmu.fw_ready);

    fake_reset();
    F.init_function = 1;
    ga10b_pmu_init(&t_pmu, &f_ops, &c);
    ga10b_pmu_boot(&t_pmu);
    TEST_ASSERT_EQUAL_INT(GA10B_PMU_ERR_PROTO, ga10b_pmu_wait_init(&t_pmu, 10));
}

static void test_wait_init_times_out(void)
{
    fake_reset();
    struct ga10b_pmu_config c = make_cfg();
    ga10b_pmu_init(&t_pmu, &f_ops, &c);
    ga10b_pmu_boot(&t_pmu);
    /* Swallow the init message: tail = head. */
    F.pmu_msg_head = 0;
    TEST_ASSERT_EQUAL_INT(GA10B_PMU_ERR_TIMEOUT, ga10b_pmu_wait_init(&t_pmu, 1));
}

static void test_poll_leaves_pmu_routed_irqs(void)
{
    TEST_ASSERT_EQUAL_INT(GA10B_PMU_OK, boot_ready());
    int w0 = F.nw;
    /* GPTMR (bit 0) is not in IRQMASK & IRQDEST (0x014000d0): it is
     * the PMU's own interrupt and a polling host must not clear it. */
    set_reg(GA10B_PWR_FALCON_IRQSTAT, 0x1u);
    TEST_ASSERT_EQUAL_INT(GA10B_PMU_OK, ga10b_pmu_poll(&t_pmu));
    TEST_ASSERT_EQUAL_HEX32(0x1u, reg(GA10B_PWR_FALCON_IRQSTAT));
    TEST_ASSERT_EQUAL_INT(-1, find_write(GA10B_PWR_FALCON_IRQSCLR, w0));
    /* A host-routed bit next to it is cleared alone. */
    set_reg(GA10B_PWR_FALCON_IRQSTAT, 0x41u);
    TEST_ASSERT_EQUAL_INT(GA10B_PMU_OK, ga10b_pmu_poll(&t_pmu));
    int i = find_write(GA10B_PWR_FALCON_IRQSCLR, w0);
    TEST_ASSERT_TRUE(i >= 0);
    TEST_ASSERT_EQUAL_HEX32(0x40u, F.wlog[i].val);
    TEST_ASSERT_EQUAL_HEX32(0x1u, reg(GA10B_PWR_FALCON_IRQSTAT));
}

/* ---- LSFM ---- */

static void test_lsfm_bootstrap_flow(void)
{
    TEST_ASSERT_EQUAL_INT(GA10B_PMU_OK, boot_ready());
    int w0 = F.nw;
    F.ndelays_ms = 0;
    TEST_ASSERT_EQUAL_INT(GA10B_PMU_OK, ga10b_pmu_bootstrap_ctxsw_falcons(&t_pmu, 100));
    TEST_ASSERT_TRUE(t_pmu.wpr_init_done);
    TEST_ASSERT_TRUE(t_pmu.falcons_loaded);
    TEST_ASSERT_EQUAL_INT(2, F.ncmds);

    /* Cmd 1: ACR INIT_WPR_REGION. heap = ALIGN4(20 + 8 + 12) = 40. */
    TEST_ASSERT_EQUAL_UINT8(0, F.fbq[0].element_index);
    TEST_ASSERT_EQUAL_UINT16(40, F.fbq[0].heap_size);
    TEST_ASSERT_EQUAL_HEX16(HEAP_BASE, F.fbq[0].heap_offset);
    TEST_ASSERT_EQUAL_UINT8(GA10B_PMU_UNIT_ACR, F.cmd[0].hdr.unit_id);
    TEST_ASSERT_EQUAL_UINT8(12, F.cmd[0].hdr.size);
    TEST_ASSERT_EQUAL_HEX8(0x03, F.cmd[0].hdr.ctrl_flags);
    TEST_ASSERT_EQUAL_UINT8(0, F.cmd[0].hdr.seq_id);
    TEST_ASSERT_EQUAL_HEX8(0x80, F.cmd[0].rpc.cmd_type);
    TEST_ASSERT_EQUAL_UINT16(20, F.cmd[0].rpc.rpc_dmem_size);
    TEST_ASSERT_EQUAL_HEX32(HEAP_BASE + 20u, F.cmd[0].rpc.rpc_dmem_ptr);
    TEST_ASSERT_EQUAL_UINT8(GA10B_PMU_UNIT_ACR, F.rpc[0][0]);
    TEST_ASSERT_EQUAL_UINT8(GA10B_PMU_RPC_ID_ACR_INIT_WPR_REGION, F.rpc[0][1]);
    TEST_ASSERT_EQUAL_HEX32(1u, get32(F.rpc[0] + 12));       /* wpr_regionId */
    TEST_ASSERT_EQUAL_HEX32(0u, get32(F.rpc[0] + 16));       /* wpr_offset */
    TEST_ASSERT_EQUAL_HEX32(0u, get32(F.rpc[0] + 20));       /* nothing past size_rpc */

    /* Cmd 2: BOOTSTRAP_GR_FALCONS; next-fit heap -> base + 64. */
    TEST_ASSERT_EQUAL_UINT8(1, F.fbq[1].element_index);
    TEST_ASSERT_EQUAL_UINT16(52, F.fbq[1].heap_size);
    TEST_ASSERT_EQUAL_HEX16(HEAP_BASE + 64u, F.fbq[1].heap_offset);
    TEST_ASSERT_EQUAL_UINT8(0, F.cmd[1].hdr.seq_id);          /* seq 0 released */
    TEST_ASSERT_EQUAL_UINT16(32, F.cmd[1].rpc.rpc_dmem_size);
    TEST_ASSERT_EQUAL_HEX32(HEAP_BASE + 64u + 20u, F.cmd[1].rpc.rpc_dmem_ptr);
    TEST_ASSERT_EQUAL_UINT8(GA10B_PMU_RPC_ID_ACR_BOOTSTRAP_GR_FALCONS, F.rpc[1][1]);
    TEST_ASSERT_EQUAL_HEX32(0xCu, get32(F.rpc[1] + 12));     /* FECS | GPCCS */
    TEST_ASSERT_EQUAL_HEX32(0u, get32(F.rpc[1] + 16));       /* RESET_YES */
    TEST_ASSERT_EQUAL_HEX32(0u, get32(F.rpc[1] + 20));       /* falcon_va_mask */

    /* Settle delays 2 ms then 5 ms after posting. */
    TEST_ASSERT_EQUAL_INT(2, F.ndelays_ms);
    TEST_ASSERT_EQUAL_UINT32(2u, F.delays_ms_seen[0]);
    TEST_ASSERT_EQUAL_UINT32(5u, F.delays_ms_seen[1]);

    /* Register pattern: LPQ head 1, reply tail writes (1 = same, 2 =
     * next), LPQ head 2, tail 2 then 3 (trace 11497-11529). */
    int i = find_write(GA10B_PWR_PMU_QUEUE_HEAD(1), w0);
    TEST_ASSERT_EQUAL_HEX32(1u, F.wlog[i].val);
    int t = find_write(GA10B_PWR_PMU_MSGQ_TAIL, i);
    TEST_ASSERT_EQUAL_HEX32(1u, F.wlog[t].val);
    t = find_write(GA10B_PWR_PMU_MSGQ_TAIL, t + 1);
    TEST_ASSERT_EQUAL_HEX32(2u, F.wlog[t].val);
    i = find_write(GA10B_PWR_PMU_QUEUE_HEAD(1), i + 1);
    TEST_ASSERT_EQUAL_HEX32(2u, F.wlog[i].val);
    TEST_ASSERT_TRUE(i > t);
    t = find_write(GA10B_PWR_PMU_MSGQ_TAIL, i);
    TEST_ASSERT_EQUAL_HEX32(2u, F.wlog[t].val);
    t = find_write(GA10B_PWR_PMU_MSGQ_TAIL, t + 1);
    TEST_ASSERT_EQUAL_HEX32(3u, F.wlog[t].val);

    /* Elements freed, sequences free. */
    TEST_ASSERT_EQUAL_HEX32(0u, t_pmu.cmdq_in_use);
    TEST_ASSERT_EQUAL_UINT32(2u, t_pmu.cmdq_tail);
    TEST_ASSERT_EQUAL_HEX32(0u, t_pmu.seq_tbl[0]);
}

static void test_events_are_skipped(void)
{
    TEST_ASSERT_EQUAL_INT(GA10B_PMU_OK, boot_ready());
    F.event_first = true;
    TEST_ASSERT_EQUAL_INT(GA10B_PMU_OK, ga10b_pmu_bootstrap_ctxsw_falcons(&t_pmu, 100));
    TEST_ASSERT_EQUAL_UINT32(2u, t_pmu.events_seen);
    TEST_ASSERT_EQUAL_UINT8(0x12, t_pmu.last_event_unit);
    TEST_ASSERT_TRUE(t_pmu.falcons_loaded);
}

static void test_rpc_failure_status(void)
{
    TEST_ASSERT_EQUAL_INT(GA10B_PMU_OK, boot_ready());
    F.rpc_status = 0x1d;
    TEST_ASSERT_EQUAL_INT(GA10B_PMU_ERR_RPC, ga10b_pmu_lsfm_init_wpr_region(&t_pmu, 100));
    TEST_ASSERT_FALSE(t_pmu.wpr_init_done);
    /* Resources released even on failure. */
    TEST_ASSERT_EQUAL_HEX32(0u, t_pmu.cmdq_in_use);
    TEST_ASSERT_EQUAL_HEX32(0u, t_pmu.seq_tbl[0]);
}

static void test_rpc_unhandled_cmd(void)
{
    TEST_ASSERT_EQUAL_INT(GA10B_PMU_OK, boot_ready());
    F.reply_unhandled = true;
    TEST_ASSERT_EQUAL_INT(GA10B_PMU_ERR_RPC,
                          ga10b_pmu_lsfm_bootstrap_gr_falcons(&t_pmu, 0xCu, 100));
}

static void test_rpc_timeout_and_halt(void)
{
    TEST_ASSERT_EQUAL_INT(GA10B_PMU_OK, boot_ready());
    F.no_reply = true;
    TEST_ASSERT_EQUAL_INT(GA10B_PMU_ERR_TIMEOUT, ga10b_pmu_lsfm_init_wpr_region(&t_pmu, 3));

    TEST_ASSERT_EQUAL_INT(GA10B_PMU_OK, boot_ready());
    F.halt_on_cmd = true;
    TEST_ASSERT_EQUAL_INT(GA10B_PMU_ERR_HALT, ga10b_pmu_lsfm_init_wpr_region(&t_pmu, 3));
    TEST_ASSERT_TRUE(t_pmu.halted);
}

static void test_rpc_argument_checks(void)
{
    fake_reset();
    struct ga10b_pmu_config c = make_cfg();
    ga10b_pmu_init(&t_pmu, &f_ops, &c);
    struct ga10b_pmu_rpc_acr_init_wpr_region rpc;
    memset(&rpc, 0, sizeof(rpc));
    rpc.hdr.unit_id = GA10B_PMU_UNIT_ACR;
    TEST_ASSERT_EQUAL_INT(GA10B_PMU_ERR_STATE, ga10b_pmu_rpc_post(&t_pmu, &rpc, 20, 0));

    TEST_ASSERT_EQUAL_INT(GA10B_PMU_OK, boot_ready());
    TEST_ASSERT_EQUAL_INT(GA10B_PMU_ERR_ARG, ga10b_pmu_rpc_post(&t_pmu, &rpc, 4, 0));
    TEST_ASSERT_EQUAL_INT(GA10B_PMU_ERR_ARG,
                          ga10b_pmu_lsfm_bootstrap_gr_falcons(&t_pmu, 0x1u, 10));
    TEST_ASSERT_EQUAL_INT(GA10B_PMU_ERR_ARG,
                          ga10b_pmu_lsfm_bootstrap_gr_falcons(&t_pmu, 0u, 10));
    TEST_ASSERT_EQUAL_INT(0, F.ncmds);

    /* Explicit post/wait with copy-back. */
    rpc.hdr.function = GA10B_PMU_RPC_ID_ACR_INIT_WPR_REGION;
    rpc.wpr_region_id = 1;
    int seq = ga10b_pmu_rpc_post(&t_pmu, &rpc, 20, 0);
    TEST_ASSERT_EQUAL_INT(0, seq);
    struct ga10b_pmu_rpc_acr_init_wpr_region back;
    memset(&back, 0xEE, sizeof(back));
    TEST_ASSERT_EQUAL_INT(GA10B_PMU_OK, ga10b_pmu_rpc_wait(&t_pmu, seq, &back, 10));
    TEST_ASSERT_EQUAL_UINT8(GA10B_PMU_UNIT_ACR, back.hdr.unit_id);
    TEST_ASSERT_EQUAL_UINT8(0, back.hdr.flcn_status);
    TEST_ASSERT_EQUAL_UINT32(1u, back.wpr_region_id);
    TEST_ASSERT_EQUAL_HEX32(0xEEEEEEEEu, back.scratch[0]);   /* only size_rpc copied */
    TEST_ASSERT_EQUAL_INT(GA10B_PMU_ERR_STATE, ga10b_pmu_rpc_wait(&t_pmu, seq, nullptr, 10));
}

int test_suite_ga10b_pmu(void)
{
    UnityBegin("test_ga10b_pmu.c");
    RUN_TEST(test_inst_block_config);
    RUN_TEST(test_build_inst_block);
    RUN_TEST(test_boot_params_wire_layout);
    RUN_TEST(test_init_rejects_bad_config);
    RUN_TEST(test_boot_sequence);
    RUN_TEST(test_boot_brom_fail_and_timeout);
    RUN_TEST(test_init_message_and_debug_buffer);
    RUN_TEST(test_init_message_zero_sw_area_offset);
    RUN_TEST(test_init_message_init_dgpu_unit_accepted);
    RUN_TEST(test_init_message_rejects_wrong_unit_or_function);
    RUN_TEST(test_wait_init_times_out);
    RUN_TEST(test_poll_leaves_pmu_routed_irqs);
    RUN_TEST(test_lsfm_bootstrap_flow);
    RUN_TEST(test_events_are_skipped);
    RUN_TEST(test_rpc_failure_status);
    RUN_TEST(test_rpc_unhandled_cmd);
    RUN_TEST(test_rpc_timeout_and_halt);
    RUN_TEST(test_rpc_argument_checks);
    return UnityEnd();
}
