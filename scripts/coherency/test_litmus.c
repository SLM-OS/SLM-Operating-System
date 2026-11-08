/*
 * test_litmus.c - deterministic checks of the litmus_patterns.h detectors.
 *
 * Each case builds a shared state that a correct pattern must flag (or must
 * not flag) and runs one role on the calling thread, so the result does not
 * depend on hardware reordering. The suite proves that the forbidden-outcome
 * checks, the final-value checks and the 1 s no-progress timeout fire; the
 * real hardware runs prove the rest. Runs natively on AArch64 or under
 * qemu-aarch64-static (make -C scripts/coherency test).
 */
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
	s.data.v = 4; /* data older than flag: the MP forbidden outcome */
	lit_mp_reader(&s, p, &r);
	snprintf(what, sizeof(what), "%s reader counts d < f", name);
	EXPECT(r.forbidden == 1 && r.timeouts == 0, what);

	memset(&r, 0, sizeof(r));
	lit_init(&s, p, 5, 2);
	s.flag.v = 5;
	s.data.v = 5;
	lit_mp_reader(&s, p, &r);
	snprintf(what, sizeof(what), "%s reader accepts d == f", name);
	EXPECT(r.forbidden == 0 && r.timeouts == 0, what);
}

int main(void)
{
	struct lit_result r;
	lit_u64 t0, secs_x10;
	lit_u32 sense = 0;

	mp_reader_case(LIT_P1, "P1");
	mp_reader_case(LIT_P2, "P2");
	mp_reader_case(LIT_S1, "S1");

	/* Writer completes all N iterations: final data and flag are N. */
	lit_init(&s, LIT_P1, 1000, 2);
	lit_mp_writer(&s, LIT_P1);
	EXPECT(s.data.v == 1000 && s.flag.v == 1000, "P1 writer reaches N");

	/* Stale flag: the reader must time out after about 1 s, not hang. */
	memset(&r, 0, sizeof(r));
	lit_init(&s, LIT_P2, 5, 2);
	t0 = lit_cntvct();
	lit_mp_reader(&s, LIT_P2, &r);
	secs_x10 = (lit_cntvct() - t0) * 10 / lit_cntfrq();
	EXPECT(r.timeouts == 1 && s.abort.v == 1, "stale flag times out and aborts");
	EXPECT(secs_x10 >= 10 && secs_x10 <= 30, "timeout fires between 1 s and 3 s");

	/* A barrier whose partner never arrives must time out. */
	memset(&r, 0, sizeof(r));
	lit_init(&s, LIT_P3, 1, 2);
	EXPECT(lit_barrier(&s, &sense, &r) == -1 && r.timeouts == 1,
	       "barrier with a missing partner times out");

	/* CoRR reader: a stable final value passes. */
	memset(&r, 0, sizeof(r));
	lit_init(&s, LIT_P4, 7, 2);
	s.data.v = 7;
	lit_corr(&s, 1, &r);
	EXPECT(r.forbidden == 0 && r.timeouts == 0, "P4 reader accepts stable x");

	/* Ping-pong: role A expecting 0 but seeing 2 is a lost step. */
	memset(&r, 0, sizeof(r));
	lit_init(&s, LIT_P5, 1, 2);
	s.data.v = 2;
	lit_pingpong(&s, 0, &r);
	EXPECT(r.forbidden == 1 && s.data.v == 3, "P5 counts a lost step");

	memset(&r, 0, sizeof(r));
	lit_init(&s, LIT_P5, 1, 2);
	lit_pingpong(&s, 0, &r);
	EXPECT(r.forbidden == 0 && s.data.v == 1, "P5 accepts the expected value");

	/* Full single-role runs exercise the final-value checks. */
	lit_init(&s, LIT_P6, 100000, 1);
	lit_run_role(&s, 0, &r);
	EXPECT(r.forbidden == 0 && r.final_value == 100000, "P6 single CPU sum");

	lit_init(&s, LIT_P6L, 100000, 1);
	lit_run_role(&s, 0, &r);
	EXPECT(r.forbidden == 0 && r.final_value == 100000, "P6L single CPU sum");

	/* P5 with one role can only make N steps, so final != 2N must fail. */
	lit_init(&s, LIT_P5, 1, 1);
	lit_run_role(&s, 0, &r);
	EXPECT(r.forbidden >= 1, "P5 final-value check fires on final != 2N");

	printf("%s: %d failure(s)\n", failures ? "FAILED" : "PASSED", failures);
	return failures ? 1 : 0;
}
