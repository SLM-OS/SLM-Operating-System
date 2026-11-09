/*
 * test_litmus.c - fault-injection checks of the litmus_patterns.h detectors.
 *
 * Every forbidden-outcome predicate (P1-P6, litmus-spec.md revision 2) is
 * driven through the real pattern code with an injected fault and must
 * fire; the clean case must not. Faults come from a crafted shared state
 * (single role) or from a fake partner role that follows the barrier
 * protocol but breaks the pattern (P3, P4, P6/P6L). The end-to-end path
 * from a bad row to VERDICT FAIL is covered by the Makefile, which runs
 * litmus -I P6 and -I P4W. Runs natively on AArch64 or under
 * qemu-aarch64-static (make -C scripts/coherency test).
 */
#include <pthread.h>
#include <stdio.h>
#include <string.h>

#include "litmus_patterns.h"

static struct lit_shared s __attribute__((aligned(LIT_LINE)));
static int failures;

#define EXPECT(cond, what)                                                    \
	do {                                                                  \
		if (!(cond)) {                                                \
			printf("FAIL %s (%s:%d)\n", what, __FILE__, __LINE__); \
			failures++;                                           \
		} else {                                                      \
			printf("ok   %s\n", what);                            \
		}                                                             \
	} while (0)

static void mp_reader_case(enum lit_pattern p, const char *name)
{
	struct lit_result r;
	char what[96];

	memset(&r, 0, sizeof(r));
	lit_init(&s, p, 5, 2);
	s.flag.v = 5;
	s.data.v = 4; /* injected: data older than flag, the MP forbidden outcome */
	lit_mp_reader(&s, p, &r);
	snprintf(what, sizeof(what), "%s inject d < f: reader counts it", name);
	EXPECT(r.forbidden == 1 && r.timeouts == 0, what);

	memset(&r, 0, sizeof(r));
	lit_init(&s, p, 5, 2);
	s.flag.v = 5;
	s.data.v = 5;
	lit_mp_reader(&s, p, &r);
	snprintf(what, sizeof(what), "%s clean d == f: reader accepts it", name);
	EXPECT(r.forbidden == 0 && r.timeouts == 0, what);
}

/* ---- threaded helpers ------------------------------------------------------ */

struct th {
	lit_u32 role;
	struct lit_result r;
};

static void *real_role(void *p)
{
	struct th *t = p;

	lit_run_role(&s, t->role, &t->r);
	return NULL;
}

/* Fake P3 T1: joins every barrier but never stores y and reports r1 = 0. */
static void *fake_sb_t1(void *p)
{
	struct th *t = p;
	lit_u32 sense = 0;

	if (lit_barrier(&s, &sense, &t->r) != 0) {
		return NULL;
	}
	for (lit_u64 round = 1; round <= s.iters; round++) {
		if (lit_barrier(&s, &sense, &t->r) != 0) {
			return NULL;
		}
		lit_str(&s.res[1].v, 0);
		if (lit_barrier(&s, &sense, &t->r) != 0) {
			return NULL;
		}
	}
	lit_barrier_w(&s, &sense, &s.data.v, &t->r);
	return NULL;
}

/* Fake P6/P6L partner: joins the barriers but adds 2 per step, not 1. */
static void *fake_sum_partner(void *p)
{
	struct th *t = p;
	lit_u32 sense = 0;

	if (lit_barrier(&s, &sense, &t->r) != 0) {
		return NULL;
	}
	for (lit_u64 i = 0; i < s.iters; i++) {
		lit_add_excl(&s.data.v, 2);
	}
	lit_barrier_w(&s, &sense, &s.data.v, &t->r);
	return NULL;
}

/* Fake P4 writer: stores a descending sequence, so every straddle is b < a. */
static void *fake_desc_writer(void *p)
{
	(void)p;
	lit_u64 v = 1UL << 40;
	lit_u64 start = lit_cntvct();

	while (lit_cntvct() - start < lit_cntfrq() / 5) {
		for (int k = 0; k < 1024; k++) {
			lit_str(&s.data.v, v--);
		}
	}
	lit_stlr(&s.done.v, 1);
	return NULL;
}

static void *p4_reader(void *p)
{
	struct th *t = p;

	lit_corr_reader(&s, t->role, &t->r);
	return NULL;
}

static void *p4_writer(void *p)
{
	struct th *t = p;

	lit_corr_writer(&s, &t->r);
	return NULL;
}

static void run_pair(void *(*f0)(void *), void *(*f1)(void *), struct th *t)
{
	pthread_t a, b;

	memset(t, 0, 2 * sizeof(*t));
	t[0].role = 0;
	t[1].role = 1;
	pthread_create(&a, NULL, f0, &t[0]);
	pthread_create(&b, NULL, f1, &t[1]);
	pthread_join(a, NULL);
	pthread_join(b, NULL);
}

int main(void)
{
	struct lit_result r;
	struct th t[2];
	lit_u64 t0, secs_x10;
	lit_u32 sense = 0;

	/* P1, P2, S1 predicate d < f */
	mp_reader_case(LIT_P1, "P1");
	mp_reader_case(LIT_P2, "P2");
	mp_reader_case(LIT_S1, "S1");

	lit_init(&s, LIT_P1, 1000, 2);
	lit_mp_writer(&s, LIT_P1);
	EXPECT(s.data.v == 1000 && s.flag.v == 1000, "P1 writer reaches N");

	/* Timeouts */
	memset(&r, 0, sizeof(r));
	lit_init(&s, LIT_P2, 5, 2);
	t0 = lit_cntvct();
	lit_mp_reader(&s, LIT_P2, &r);
	secs_x10 = (lit_cntvct() - t0) * 10 / lit_cntfrq();
	EXPECT(r.timeouts == 1 && s.abort.v == 1, "stale flag times out and aborts");
	EXPECT(secs_x10 >= 10 && secs_x10 <= 30, "timeout fires between 1 s and 3 s");

	memset(&r, 0, sizeof(r));
	lit_init(&s, LIT_P3, 1, 2);
	EXPECT(lit_barrier(&s, &sense, &r) == -1 && r.timeouts == 1,
	       "barrier with a missing partner times out");

	/* P3 predicate r0 < r && r1 < r */
	EXPECT(lit_sb_relaxed(0, 0, 1) && !lit_sb_relaxed(1, 0, 1) &&
		       !lit_sb_relaxed(0, 1, 1),
	       "P3 predicate truth table");
	lit_init(&s, LIT_P3, 1000, 2);
	run_pair(real_role, fake_sb_t1, t);
	EXPECT(t[0].r.forbidden == 1000 && t[0].r.timeouts == 0,
	       "P3 inject (T1 never stores y): every round counted");
	lit_init(&s, LIT_P3, 1000, 2);
	run_pair(real_role, real_role, t);
	EXPECT(t[0].r.forbidden == 0 && t[0].r.timeouts == 0 && t[1].r.timeouts == 0,
	       "P3 clean two-thread run: no forbidden outcome");
	lit_init(&s, LIT_S2, 1000, 2);
	run_pair(real_role, real_role, t);
	printf("info S2 two-thread run: relaxed=%lu (reported, never judged)\n",
	       t[0].r.forbidden);
	EXPECT(t[0].r.timeouts == 0 && t[1].r.timeouts == 0, "S2 runs to completion");

	/* P4 predicate b < a, and the coverage counters */
	EXPECT(lit_corr_forbidden(5, 4) && !lit_corr_forbidden(4, 5) &&
		       !lit_corr_forbidden(4, 4),
	       "P4 predicate truth table");
	memset(&r, 0, sizeof(r));
	lit_init(&s, LIT_P4, 7, 2);
	s.data.v = 7;
	s.done.v = 1;
	lit_corr_reader(&s, 1, &r);
	EXPECT(r.forbidden == 0 && r.pairs > 0 && r.straddles == 0 && r.timeouts == 0,
	       "P4 reader on a stable x: pairs counted, zero straddles");
	lit_init(&s, LIT_P4, 0, 2);
	run_pair(fake_desc_writer, p4_reader, t);
	EXPECT(t[1].r.forbidden > 0 && t[1].r.forbidden == t[1].r.straddles,
	       "P4 inject (descending writer): every straddle counted as b < a");
	printf("info P4 inject: pairs=%lu straddles=%lu forbidden=%lu\n",
	       t[1].r.pairs, t[1].r.straddles, t[1].r.forbidden);
	lit_init(&s, LIT_P4, 100000, 2);
	s.straddle_target = 1000;
	run_pair(p4_writer, p4_reader, t);
	EXPECT(t[1].r.forbidden == 0 && t[0].r.final_value >= 100000 &&
		       t[1].r.timeouts == 0,
	       "P4 clean run: writer makes at least N writes, no b < a");
	printf("info P4 clean: writes=%lu pairs=%lu straddles=%lu\n",
	       t[0].r.final_value, t[1].r.pairs, t[1].r.straddles);
	lit_init(&s, LIT_P4, 0, 2);
	s.budget_ticks = 0;
	run_pair(p4_writer, p4_reader, t);
	EXPECT(t[0].r.final_value == 0 && t[1].r.straddles == 0,
	       "P4 zero budget: no writes, zero straddles (a weak row)");

	/* P5 lost step and final-value check */
	memset(&r, 0, sizeof(r));
	lit_init(&s, LIT_P5, 1, 2);
	s.data.v = 2;
	lit_pingpong(&s, 0, &r);
	EXPECT(r.forbidden == 1 && s.data.v == 3, "P5 inject lost step: counted");
	memset(&r, 0, sizeof(r));
	lit_init(&s, LIT_P5, 1, 2);
	lit_pingpong(&s, 0, &r);
	EXPECT(r.forbidden == 0 && s.data.v == 1, "P5 clean step: accepted");
	lit_init(&s, LIT_P5, 1, 1);
	lit_run_role(&s, 0, &r);
	EXPECT(r.forbidden >= 1, "P5 inject final != 2N: counted");

	/* P6 / P6L final-sum mismatch */
	EXPECT(lit_final_mismatch(LIT_P6, 7, 3, 2) && !lit_final_mismatch(LIT_P6, 6, 3, 2) &&
		       lit_final_mismatch(LIT_P6L, 5, 3, 2) &&
		       !lit_final_mismatch(LIT_P6L, 6, 3, 2),
	       "P6/P6L final-sum predicate truth table");
	lit_init(&s, LIT_P6, 100000, 1);
	lit_run_role(&s, 0, &r);
	EXPECT(r.forbidden == 0 && r.final_value == 100000, "P6 clean single CPU sum");
	lit_init(&s, LIT_P6L, 100000, 1);
	lit_run_role(&s, 0, &r);
	EXPECT(r.forbidden == 0 && r.final_value == 100000, "P6L clean single CPU sum");
	lit_init(&s, LIT_P6, 100000, 2);
	run_pair(real_role, fake_sum_partner, t);
	EXPECT(t[0].r.forbidden == 1 && t[0].r.final_value == 300000,
	       "P6 inject (partner adds 2): final-sum mismatch counted");
	lit_init(&s, LIT_P6L, 100000, 2);
	run_pair(real_role, fake_sum_partner, t);
	EXPECT(t[0].r.forbidden == 1 && t[0].r.final_value == 300000,
	       "P6L inject (partner adds 2): final-sum mismatch counted");
	lit_init(&s, LIT_P6, 100000, 2);
	run_pair(real_role, real_role, t);
	EXPECT(t[0].r.forbidden == 0 && t[0].r.final_value == 200000,
	       "P6 clean two-thread sum");
	lit_init(&s, LIT_P6L, 100000, 2);
	run_pair(real_role, real_role, t);
	EXPECT(t[0].r.forbidden == 0 && t[0].r.final_value == 200000,
	       "P6L clean two-thread sum");

	printf("%s: %d failure(s)\n", failures ? "FAILED" : "PASSED", failures);
	return failures ? 1 : 0;
}
