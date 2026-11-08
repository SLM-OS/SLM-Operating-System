/*
 * litmus_patterns.h - #1193 coherency litmus pattern bodies (AArch64).
 *
 * Implements P1-P6 and S1 of docs/archive/coherency-gauntlet/litmus-spec.md.
 *
 * Freestanding by design: no libc, no allocation, no OS calls. The only
 * dependencies are the AArch64 instructions issued through inline asm
 * (LDR/STR, LDAR/STLR, DMB ISH*, LDXR/STXR, LDADD) and the generic timer
 * (CNTVCT_EL0/CNTFRQ_EL0) for the 1 s no-progress timeout. A host harness
 * (scripts/coherency/litmus.c) and a kernel test can both include this file:
 * the caller allocates one struct lit_shared on cacheable Normal Write-Back
 * Inner Shareable memory, starts one thread per role pinned to the chosen
 * CPU, and calls lit_run_role() on each.
 *
 * Every shared variable sits on its own LIT_LINE-byte line. No pattern
 * issues a cache maintenance instruction.
 *
 * Roles:
 *   P1, P2, S1, P3, P5: role 0 and role 1 (two CPUs, nthreads == 2).
 *     P1/P2/S1: role 0 writer, role 1 reader.
 *     P3:       role 0 is T0, role 1 is T1.
 *     P5:       role 0 is CPU A (waits for even), role 1 is CPU B (odd).
 *   P4: role 0 is the writer, roles 1..nthreads-1 are readers.
 *   P6, P6L: every role adds (P6 with LDXR/STXR, P6L with LSE LDADD).
 *
 * Result accounting: each role writes its own struct lit_result. The
 * caller sums forbidden and timeouts over all roles of one run; role 0
 * reports elapsed ticks between the start and end barriers.
 */
#ifndef LITMUS_PATTERNS_H
#define LITMUS_PATTERNS_H

typedef unsigned long lit_u64;
typedef unsigned int lit_u32;

#define LIT_LINE 128 /* 64 B is the A76/A78AE line; 128 B is allowed */

#define LIT_MAX_THREADS 16

enum lit_pattern {
	LIT_P1 = 0, /* MP+rel+acq */
	LIT_P2,     /* MP+dmbs */
	LIT_P3,     /* SB+dmbs */
	LIT_P4,     /* CoRR, one writer, all others read */
	LIT_P5,     /* ping-pong */
	LIT_P6,     /* atomic sum, LDXR/STXR */
	LIT_P6L,    /* atomic sum, LSE LDADD (extra row, same criterion as P6) */
	LIT_S1,     /* MP+none, sensitivity only */
	LIT_NPATTERNS
};

struct lit_line {
	volatile lit_u64 v;
	char pad[LIT_LINE - sizeof(lit_u64)];
} __attribute__((aligned(LIT_LINE)));

/*
 * All state shared between roles. The caller zeroes it (lit_init) before
 * starting threads; thread creation must publish the zeroed state, which
 * pthread_create and any kernel IPI-based start path both do.
 */
struct lit_shared {
	struct lit_line data;    /* P1/P2/S1 data, P3 x, P4 x, P5 counter, P6 sum */
	struct lit_line flag;    /* P1/P2/S1 flag, P3 y */
	struct lit_line res[2];  /* P3 per-round results of T0, T1 */
	struct lit_line bar_count;
	struct lit_line bar_sense;
	struct lit_line abort;   /* set by any role that timed out */
	lit_u64 iters;           /* N, read-only after lit_init */
	lit_u32 nthreads;        /* read-only after lit_init */
	lit_u32 pattern;         /* read-only after lit_init */
} __attribute__((aligned(LIT_LINE)));

struct lit_result {
	lit_u64 forbidden;   /* forbidden outcomes seen (S1: relaxed outcomes) */
	lit_u64 timeouts;    /* wait loops with no progress for 1 s */
	lit_u64 ticks;       /* role 0 only: CNTVCT ticks start barrier to end */
	lit_u64 final_value; /* P5/P6: final counter value (role 0) */
};

/* ---- instruction helpers ------------------------------------------------ */

static inline lit_u64 lit_ldr(volatile lit_u64 *p)
{
	lit_u64 v;
	__asm__ volatile("ldr %0, [%1]" : "=r"(v) : "r"(p) : "memory");
	return v;
}

static inline void lit_str(volatile lit_u64 *p, lit_u64 v)
{
	__asm__ volatile("str %0, [%1]" : : "r"(v), "r"(p) : "memory");
}

static inline lit_u64 lit_ldar(volatile lit_u64 *p)
{
	lit_u64 v;
	__asm__ volatile("ldar %0, [%1]" : "=r"(v) : "r"(p) : "memory");
	return v;
}

static inline void lit_stlr(volatile lit_u64 *p, lit_u64 v)
{
	__asm__ volatile("stlr %0, [%1]" : : "r"(v), "r"(p) : "memory");
}

static inline void lit_dmb_ish(void)
{
	__asm__ volatile("dmb ish" : : : "memory");
}

static inline void lit_dmb_ishst(void)
{
	__asm__ volatile("dmb ishst" : : : "memory");
}

static inline void lit_dmb_ishld(void)
{
	__asm__ volatile("dmb ishld" : : : "memory");
}

/* Compiler-only barrier; S1 uses it so the compiler keeps program order. */
static inline void lit_cbar(void)
{
	__asm__ volatile("" : : : "memory");
}

/* Exclusive add with no ordering (P6 counts only; barriers order the rest). */
static inline void lit_add_excl(volatile lit_u64 *p, lit_u64 inc)
{
	lit_u64 tmp;
	lit_u32 fail;
	__asm__ volatile("1: ldxr %0, [%2]\n"
			 "   add %0, %0, %3\n"
			 "   stxr %w1, %0, [%2]\n"
			 "   cbnz %w1, 1b\n"
			 : "=&r"(tmp), "=&r"(fail)
			 : "r"(p), "r"(inc)
			 : "memory");
}

/* Exclusive fetch-add with acquire+release (barrier arrival). */
static inline lit_u64 lit_fetch_add_acqrel(volatile lit_u64 *p, lit_u64 inc)
{
	lit_u64 old, tmp;
	lit_u32 fail;
	__asm__ volatile("1: ldaxr %0, [%3]\n"
			 "   add %1, %0, %4\n"
			 "   stlxr %w2, %1, [%3]\n"
			 "   cbnz %w2, 1b\n"
			 : "=&r"(old), "=&r"(tmp), "=&r"(fail)
			 : "r"(p), "r"(inc)
			 : "memory");
	return old;
}

/* LSE atomic add (Armv8.1). Only called when the caller saw LSE support. */
static inline void lit_add_lse(volatile lit_u64 *p, lit_u64 inc)
{
	__asm__ volatile(".arch_extension lse\n"
			 "stadd %0, [%1]\n"
			 : : "r"(inc), "r"(p) : "memory");
}

static inline lit_u64 lit_cntvct(void)
{
	lit_u64 v;
	__asm__ volatile("isb\n mrs %0, cntvct_el0" : "=r"(v) : : "memory");
	return v;
}

static inline lit_u64 lit_cntfrq(void)
{
	lit_u64 v;
	__asm__ volatile("mrs %0, cntfrq_el0" : "=r"(v));
	return v;
}

/* ---- no-progress timeout ------------------------------------------------ */

/*
 * A waiter tracks the last observed value of the location being waited on.
 * lit_wait_step() returns 1 once the value has not changed for one second
 * (one CNTFRQ period). The counter is read only every 1024 calls, so the
 * hot loop issues no ISB/MRS between samples.
 */
struct lit_waiter {
	lit_u64 last;
	lit_u64 since;
	lit_u64 limit;
	lit_u32 spins;
	lit_u32 progressed;
};

static inline void lit_wait_begin(struct lit_waiter *w, lit_u64 first)
{
	w->last = first;
	w->limit = lit_cntfrq();
	w->since = lit_cntvct();
	w->spins = 0;
	w->progressed = 0;
}

static inline int lit_wait_step(struct lit_waiter *w, lit_u64 seen)
{
	lit_u64 now;

	if (seen != w->last) {
		w->last = seen;
		w->progressed = 1;
	}
	if (++w->spins < 1024) {
		return 0;
	}
	w->spins = 0;
	now = lit_cntvct();
	if (w->progressed) {
		w->progressed = 0;
		w->since = now;
		return 0;
	}
	return (now - w->since) > w->limit;
}

static inline int lit_aborted(struct lit_shared *s)
{
	return lit_ldar(&s->abort.v) != 0;
}

static inline void lit_raise_abort(struct lit_shared *s, struct lit_result *r)
{
	r->timeouts++;
	lit_stlr(&s->abort.v, 1);
}

/* ---- sense-reversing spin barrier ------------------------------------------ */

/*
 * Returns 0 on success, -1 on timeout or abort. *local_sense is per role
 * and starts at 0. The last arriver resets the count with a plain store and
 * publishes the new sense with STLR, which orders the reset before it.
 *
 * Progress for the timeout is any change of the sense, the arrival count,
 * or *work (when non-null): a role that finished early and waits for slower
 * roles still working on *work is not a stall.
 */
static inline lit_u64 lit_bar_state(struct lit_shared *s, volatile lit_u64 *work)
{
	lit_u64 v = lit_ldar(&s->bar_sense.v);

	v ^= lit_ldr(&s->bar_count.v) * 0x9E3779B97F4A7C15UL;
	if (work) {
		v ^= lit_ldr(work) * 0xC2B2AE3D27D4EB4FUL;
	}
	return v;
}

static inline int lit_barrier_w(struct lit_shared *s, lit_u32 *local_sense,
				volatile lit_u64 *work, struct lit_result *r)
{
	lit_u64 sense = (*local_sense ^= 1);
	lit_u64 old = lit_fetch_add_acqrel(&s->bar_count.v, 1);
	struct lit_waiter w;

	if (old == (lit_u64)s->nthreads - 1) {
		lit_str(&s->bar_count.v, 0);
		lit_stlr(&s->bar_sense.v, sense);
		return 0;
	}
	lit_wait_begin(&w, lit_bar_state(s, work));
	for (;;) {
		if (lit_ldar(&s->bar_sense.v) == sense) {
			return 0;
		}
		if (lit_aborted(s)) {
			return -1;
		}
		if (lit_wait_step(&w, lit_bar_state(s, work))) {
			lit_raise_abort(s, r);
			return -1;
		}
	}
}

static inline int lit_barrier(struct lit_shared *s, lit_u32 *local_sense,
			      struct lit_result *r)
{
	return lit_barrier_w(s, local_sense, (volatile lit_u64 *)0, r);
}

/* ---- setup ---------------------------------------------------------------- */

static inline void lit_init(struct lit_shared *s, enum lit_pattern p,
			    lit_u64 iters, lit_u32 nthreads)
{
	s->data.v = 0;
	s->flag.v = 0;
	s->res[0].v = 0;
	s->res[1].v = 0;
	s->bar_count.v = 0;
	s->bar_sense.v = 0;
	s->abort.v = 0;
	s->iters = iters;
	s->nthreads = nthreads;
	s->pattern = (lit_u32)p;
}

/* ---- P1, P2, S1: message passing ------------------------------------------- */

static inline void lit_mp_writer(struct lit_shared *s, enum lit_pattern p)
{
	lit_u64 n = s->iters;

	for (lit_u64 i = 1; i <= n; i++) {
		if ((i & 0xfff) == 0 && lit_aborted(s)) {
			return;
		}
		lit_str(&s->data.v, i);
		if (p == LIT_P1) {
			lit_stlr(&s->flag.v, i);
		} else if (p == LIT_P2) {
			lit_dmb_ishst();
			lit_str(&s->flag.v, i);
		} else {
			lit_cbar();
			lit_str(&s->flag.v, i);
		}
	}
}

static inline void lit_mp_reader(struct lit_shared *s, enum lit_pattern p,
				 struct lit_result *r)
{
	lit_u64 n = s->iters;
	lit_u64 f, d;
	struct lit_waiter w;

	lit_wait_begin(&w, 0);
	for (;;) {
		if (p == LIT_P1) {
			f = lit_ldar(&s->flag.v);
			d = lit_ldr(&s->data.v);
		} else if (p == LIT_P2) {
			f = lit_ldr(&s->flag.v);
			lit_dmb_ishld();
			d = lit_ldr(&s->data.v);
		} else {
			f = lit_ldr(&s->flag.v);
			lit_cbar();
			d = lit_ldr(&s->data.v);
		}
		if (d < f) {
			r->forbidden++;
		}
		if (f >= n) {
			return;
		}
		if (lit_wait_step(&w, f)) {
			lit_raise_abort(s, r);
			return;
		}
	}
}

/* ---- P3: store buffering with DMB ISH -------------------------------------- */

static inline void lit_sb(struct lit_shared *s, lit_u32 role, lit_u32 *sense,
			  struct lit_result *r)
{
	lit_u64 n = s->iters;
	volatile lit_u64 *mine = role == 0 ? &s->data.v : &s->flag.v; /* x : y */
	volatile lit_u64 *other = role == 0 ? &s->flag.v : &s->data.v;

	for (lit_u64 round = 1; round <= n; round++) {
		lit_u64 seen;

		if (lit_barrier(s, sense, r) != 0) {
			return;
		}
		lit_str(mine, round);
		lit_dmb_ish();
		seen = lit_ldr(other);
		lit_str(&s->res[role].v, seen);
		if (lit_barrier(s, sense, r) != 0) {
			return;
		}
		if (role == 0) {
			lit_u64 r0 = lit_ldr(&s->res[0].v);
			lit_u64 r1 = lit_ldr(&s->res[1].v);

			if (r0 < round && r1 < round) {
				r->forbidden++;
			}
		}
	}
}

/* ---- P4: CoRR -------------------------------------------------------------- */

static inline void lit_corr(struct lit_shared *s, lit_u32 role,
			    struct lit_result *r)
{
	lit_u64 n = s->iters;
	struct lit_waiter w;

	if (role == 0) {
		for (lit_u64 i = 1; i <= n; i++) {
			if ((i & 0xfff) == 0 && lit_aborted(s)) {
				return;
			}
			lit_str(&s->data.v, i);
		}
		return;
	}
	lit_wait_begin(&w, 0);
	for (;;) {
		lit_u64 a = lit_ldr(&s->data.v);
		lit_u64 b = lit_ldr(&s->data.v);

		if (b < a) {
			r->forbidden++;
		}
		if (b >= n) {
			return;
		}
		if (lit_aborted(s)) {
			return;
		}
		if (lit_wait_step(&w, b)) {
			lit_raise_abort(s, r);
			return;
		}
	}
}

/* ---- P5: ping-pong --------------------------------------------------------- */

/*
 * Role 0 (A) owns even values and role 1 (B) owns odd values. Each role
 * knows exactly which value it must see next; seeing any other value of
 * its own parity is a lost or duplicated step and is counted. Plain loads,
 * STLR stores.
 */
static inline void lit_pingpong(struct lit_shared *s, lit_u32 role,
				struct lit_result *r)
{
	lit_u64 n = s->iters;
	lit_u64 expect = role; /* A expects 0, 2, 4...; B expects 1, 3, 5... */
	struct lit_waiter w;

	lit_wait_begin(&w, lit_ldr(&s->data.v));
	for (lit_u64 k = 0; k < n; k++) {
		lit_u64 v;

		for (;;) {
			v = lit_ldr(&s->data.v);
			if ((v & 1) == role) {
				break;
			}
			if (lit_aborted(s)) {
				return;
			}
			if (lit_wait_step(&w, v)) {
				lit_raise_abort(s, r);
				return;
			}
		}
		if (v != expect) {
			r->forbidden++;
		}
		lit_stlr(&s->data.v, v + 1);
		expect = v + 2;
	}
}

/* ---- P6: atomic sum -------------------------------------------------------- */

static inline void lit_atomic_sum(struct lit_shared *s, enum lit_pattern p)
{
	lit_u64 n = s->iters;

	for (lit_u64 i = 0; i < n; i++) {
		if (p == LIT_P6L) {
			lit_add_lse(&s->data.v, 1);
		} else {
			lit_add_excl(&s->data.v, 1);
		}
	}
}

/* ---- entry point ----------------------------------------------------------- */

/*
 * Run one role of the pattern set up by lit_init(). All roles must be
 * started; each returns after the end barrier (or after an abort). The
 * caller sums the per-role results. For P5 and P6 role 0 also checks the
 * final value and counts a mismatch as one forbidden outcome.
 */
static inline void lit_run_role(struct lit_shared *s, lit_u32 role,
				struct lit_result *r)
{
	enum lit_pattern p = (enum lit_pattern)s->pattern;
	lit_u32 sense = 0;
	lit_u64 t0;

	r->forbidden = 0;
	r->timeouts = 0;
	r->ticks = 0;
	r->final_value = 0;

	if (lit_barrier(s, &sense, r) != 0) {
		return;
	}
	t0 = lit_cntvct();

	switch (p) {
	case LIT_P1:
	case LIT_P2:
	case LIT_S1:
		if (role == 0) {
			lit_mp_writer(s, p);
		} else {
			lit_mp_reader(s, p, r);
		}
		break;
	case LIT_P3:
		lit_sb(s, role, &sense, r);
		break;
	case LIT_P4:
		lit_corr(s, role, r);
		break;
	case LIT_P5:
		lit_pingpong(s, role, r);
		break;
	case LIT_P6:
	case LIT_P6L:
		lit_atomic_sum(s, p);
		break;
	default:
		break;
	}

	if (lit_barrier_w(s, &sense, &s->data.v, r) != 0) {
		return;
	}
	if (role != 0) {
		return;
	}
	r->ticks = lit_cntvct() - t0;
	if (p == LIT_P5) {
		r->final_value = lit_ldr(&s->data.v);
		if (r->final_value != 2 * s->iters) {
			r->forbidden++;
		}
	} else if (p == LIT_P6 || p == LIT_P6L) {
		r->final_value = lit_ldr(&s->data.v);
		if (r->final_value != (lit_u64)s->nthreads * s->iters) {
			r->forbidden++;
		}
	}
}

#endif /* LITMUS_PATTERNS_H */
