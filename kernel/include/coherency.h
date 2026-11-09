/*
 * coherency.h - `coherency` shell verb and litmus driver (#1193)
 *
 * Runs the litmus patterns from coherency_litmus.h on chosen CPUs, prints
 * result rows in the format shared with the Linux harness, collects
 * per-CPU cache/MMU control registers, and runs the negative controls.
 * Spec: docs/archive/coherency-gauntlet/litmus-spec.md.
 *
 * ARM64 only (nothing is declared on x86-64).
 */

#ifndef COHERENCY_H
#define COHERENCY_H

#include <stdint.h>
#include <stdbool.h>
#include "config.h"     /* MAX_CPUS */

#if !defined(PLATFORM_X86_64)

/* Printf-shaped sink: shell_printf for the verb, uart_printf in tests. */
typedef int (*coh_out_fn)(const char *fmt, ...);

enum coh_pattern {
    COH_P1 = 0,     /* MP+rel+acq */
    COH_P2,         /* MP+dmbs */
    COH_P3,         /* SB+dmbs */
    COH_P4,         /* CoRR */
    COH_P5,         /* ping-pong */
    COH_P6,         /* atomic sum */
    COH_S1,         /* MP+none, sensitivity for P2: reported, never judged */
    COH_S2,         /* SB+none, sensitivity for P3: reported, never judged */
    COH_PATTERN_COUNT
};

/* Bit for @p in struct coh_run_opts.patterns. */
#define COH_PAT_BIT(p)          (1u << (unsigned)(p))
#define COH_PAT_ALL             ((1u << (unsigned)COH_PATTERN_COUNT) - 1u)

/* Spec iteration counts (scale 1). `coherency litmus <scale>` divides. */
#define COH_ITERS_MP            1000000ull  /* P1, P2, S1 per pair; P4 minimum writes */
#define COH_ITERS_ROUNDS        100000ull   /* P3, P5, S2 rounds per pair */
#define COH_ITERS_SUM           1000000ull  /* P6 per CPU */
#define COH_ITERS_NEG           100000ull   /* negative-control P2 / P4 */
#define COH_MIN_ITERS           16ull
#define COH_MAX_SCALE           1000000u

/*
 * P4 coverage (spec revision 2): after its minimum writes the P4 writer
 * keeps going until every reader has COH_P4_STRADDLE_TARGET straddles or
 * COH_P4_BUDGET_MS have passed. A scaled run divides the target by the
 * scale too (floor 1); the budget stays 2 s.
 */
#define COH_P4_STRADDLE_TARGET  100000ull
#define COH_P4_BUDGET_MS        2000u

/* No-progress budget (spec: 1 s) and the start-barrier budget, which
 * also covers the time a worker task needs to be scheduled. */
#define COH_TIMEOUT_MS          1000u
#define COH_START_TIMEOUT_MS    5000u

/*
 * Fixed kernel VA for the negative-control alias mapping: the last 2 MB
 * of the kbuf window (kbuf.h), which the kbuf bump allocator only reaches
 * after 32 GB of allocations. Mapped only while a control runs.
 */
#define COH_ALIAS_VA            (0xFFFFFF9000000000UL - (2UL * 1024UL * 1024UL))

struct coh_summary {
    uint32_t rows;                              /* rows printed */
    uint32_t failed_rows;                       /* judged rows that failed */
    uint64_t forbidden[COH_PATTERN_COUNT];      /* totals per pattern */
    uint64_t timeouts[COH_PATTERN_COUNT];
    uint32_t p4_weak_rows;                      /* P4 rows with a 0-straddle reader */
    uint64_t p4_min_straddles;                  /* fewest straddles of any P4 reader */
    uint64_t p4_target;                         /* straddle target this run used */
    /* S1 / S2 relaxed outcomes per ordered pair [a][b], for the SENS lines. */
    uint64_t s1_pair[MAX_CPUS][MAX_CPUS];
    uint64_t s2_pair[MAX_CPUS][MAX_CPUS];
};

/*
 * Self-test fault injection (kernel/tests/test_coherency.c). Each one
 * replaces a role with a fake or corrupts the start state so that a known
 * checker must count the fault and the run must end in FAIL.
 */
enum coh_inject {
    COH_INJECT_NONE = 0,
    COH_INJECT_P3_STALE_PARTNER,    /* P3 T1 never stores y and reports x as r - 1 */
    COH_INJECT_P4_DESCENDING,       /* P4 writer stores descending values */
    COH_INJECT_P4_NO_WRITES,        /* P4 writer makes no writes: every row weak */
    COH_INJECT_P6_SUM_OFFSET,       /* P6 sum starts at 1 */
    COH_INJECT_COUNT
};

struct coh_run_opts {
    uint32_t scale;                 /* divides the spec counts; 0 means 1 */
    uint32_t patterns;              /* COH_PAT_BIT() mask; 0 means COH_PAT_ALL */
    enum coh_inject inject;
};

enum coh_neg_control {
    COH_NEG_NONSHARE = 0,   /* (a) reader on a Normal WB Non-shareable alias */
    COH_NEG_NONCACHE = 1,   /* (b) reader on a Normal Non-cacheable alias */
    COH_NEG_DCACHE_OFF = 2, /* (c) reader with its data cache off (SCTLR.C=0) */
    COH_NEG_COUNT
};

/* Control (c) loads per phase, per pair (kept short: IRQs are masked). */
#define COH_DCOFF_LOADS         1024ull

struct coh_neg_summary {
    /* Controls a and b: [control][0] = P2 rows, [control][1] = P4 rows. */
    uint64_t forbidden[COH_NEG_COUNT][2];
    uint64_t timeouts[COH_NEG_COUNT][2];
    uint64_t alias_pte[COH_NEG_COUNT];          /* L2 descriptor used for the alias */
    /*
     * Control c totals over all pairs. Phase 1 reads after the writer
     * stored P1 without a clean (P0 is at PoC): stale = reads of P0.
     * Phase 2 reads after the writer cleaned P1: fresh = reads of P1.
     */
    uint64_t dcoff_loads;                       /* loads per phase, all pairs */
    uint64_t dcoff_p1_stale;
    uint64_t dcoff_p1_fresh;
    uint64_t dcoff_p2_stale;
    uint64_t dcoff_p2_fresh;
    uint64_t dcoff_timeouts;
    bool ran[COH_NEG_COUNT];
};

#define COH_MAX_CACHE_LEVELS    7u

struct coh_cpu_regs {
    bool valid;                 /* this CPU reported */
    uint32_t ran_on;            /* cpu_id() seen by the collector */
    uint64_t current_el;        /* CurrentEL >> 2 */
    uint64_t sctlr_el1;         /* EL1 copy (SCTLR_EL12 when E2H=1) */
    uint64_t sctlr_el2;         /* EL2 only */
    uint64_t hcr_el2;           /* EL2 only */
    uint64_t tcr;               /* TCR in use (EL2 or EL1) */
    uint64_t mair;              /* MAIR in use (EL2 or EL1) */
    uint64_t clidr;
    uint64_t midr;
    uint64_t mpidr;
    uint64_t ctr;
    uint32_t ncache;            /* data/unified levels read */
    uint8_t level[COH_MAX_CACHE_LEVELS];     /* 1-based cache level */
    uint8_t ctype[COH_MAX_CACHE_LEVELS];     /* CLIDR Ctype for that level */
    uint64_t ccsidr[COH_MAX_CACHE_LEVELS];
    bool impdef_read;           /* CPUECTLR_EL1 / CLUSTERECTLR_EL1 were read */
    uint64_t cpuectlr;          /* raw, read-only */
    uint64_t clusterectlr;      /* raw, read-only */
};

/*
 * Run P1-P6, S1 and S2 over every ordered pair of online CPUs (P4, P6 over
 * all CPUs) with the spec counts divided by @scale (1 = spec). Prints rows
 * to @out (NULL = silent), then one SENS line per ordered pair and the
 * verdict. Returns 0 on PASS, 1 on FAIL, -1 if the run could not start
 * (busy, a stray worker from an earlier run, or < 2 CPUs).
 */
int coherency_litmus_run(uint32_t scale, coh_out_fn out, struct coh_summary *sum);

/* As coherency_litmus_run, for a subset of patterns and with an optional
 * fault injected. SENS lines print only when S1 and S2 both ran. */
int coherency_litmus_run_opts(const struct coh_run_opts *opts, coh_out_fn out,
                              struct coh_summary *sum);

/*
 * Negative controls, report only: (a) P2 and P4 with the reader on a
 * Non-shareable alias and (b) on a Non-cacheable alias of the page that
 * holds the variables, then (c) a reader with its data cache off, over
 * every ordered pair. Returns 0 if all controls ran, -1 otherwise.
 */
int coherency_neg_run(uint32_t scale, coh_out_fn out, struct coh_neg_summary *sum);

/*
 * Collect control registers from every online CPU into @regs[0..cpu_count).
 * @impdef also reads CPUECTLR_EL1 and CLUSTERECTLR_EL1 (IMPLEMENTATION
 * DEFINED, read-only here) on Pi 5 and Jetson cores whose MIDR is a
 * Cortex-A76 / A78 family part. The #1193 C0 research found reads safe at
 * NS-EL2 on both boards (ACTLR_EL3.ECTLREN gates writes only). They are
 * never written. QEMU builds never read them. Returns the number of CPUs
 * that reported, or -1 if busy.
 */
int coherency_regs_collect(struct coh_cpu_regs regs[MAX_CPUS], bool impdef);
void coherency_regs_print(coh_out_fn out, const struct coh_cpu_regs regs[MAX_CPUS],
                          uint32_t ncpus);

/* True when a row with these counts fails the board. S1 and S2 never
 * fail; a P4 row also fails when @weak (a reader saw no straddle). */
bool coherency_row_fails(enum coh_pattern p, uint64_t forbidden, uint64_t timeouts, bool weak);

/* P4 straddle target at @scale (COH_P4_STRADDLE_TARGET / scale, floor 1). */
uint64_t coherency_p4_target(uint32_t scale);

/* Iterations a pattern runs at @scale (spec count / scale, floor 16). */
uint64_t coherency_iters(enum coh_pattern p, uint32_t scale);

const char *coherency_pattern_name(enum coh_pattern p);

/* Shell entry point: coherency regs | litmus [scale] | neg [scale] */
int cmd_coherency(int argc, char **argv);

#endif /* !PLATFORM_X86_64 */

#endif /* COHERENCY_H */
