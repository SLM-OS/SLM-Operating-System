/*
 * litmus.c - Linux user-space harness for the #1193 coherency litmus set.
 *
 * Pattern bodies live in litmus_patterns.h (freestanding, shared with a
 * future kernel test). This file only provides the pthread and affinity
 * glue, the run loop and the text output.
 *
 * Build: aarch64-linux-gnu-gcc -O2 -static -pthread -o litmus litmus.c
 * Usage: litmus [-m minutes] [-d divisor] [-1]
 *   -m minutes  loop the whole set until this much time has elapsed
 *               (default 10; the pass in progress always completes)
 *   -d divisor  divide every iteration count by this (smoke tests only;
 *               a run with a divisor other than 1 is not a spec run)
 *   -1          run exactly one pass
 *
 * Output lines (one per run, fixed key order):
 *   ROW pass=<k> pattern=<id> cpus=<set> iters=<n> forbidden=<n>
 *       timeouts=<n> ms=<t>
 * cpus is "a>b" for pair patterns (a = role 0: writer, T0 or A),
 * "w<W>/<lo>-<hi>" for P4 (writer W, every online CPU lo..hi takes part)
 * and "<lo>-<hi>" for P6/P6L; this spelling is shared with the SLM-OS
 * coherency verb (C2). A non-contiguous online set falls back to
 * "w>r1,r2,..." and "c0,c1,...".
 * After the last pass, TOTAL lines aggregate every row per pattern and a
 * VERDICT line states PASS or FAIL over the P rows (S1 is never judged).
 */
#define _GNU_SOURCE
#include <errno.h>
#include <pthread.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/auxv.h>
#include <time.h>
#include <unistd.h>

#include "litmus_patterns.h"

#ifndef HWCAP_ATOMICS
#define HWCAP_ATOMICS (1 << 8)
#endif

#define LIT_VERSION "1193-c1-v1"

static struct lit_shared g_shared __attribute__((aligned(LIT_LINE)));

struct role_arg {
	int cpu;
	lit_u32 role;
	int pin_ok;
	struct lit_result res;
	pthread_t tid;
};

struct pattern_info {
	enum lit_pattern id;
	const char *name;
	lit_u64 iters; /* spec minimum per pair / per writer / per CPU */
	int kind;      /* 0 = ordered pair, 1 = P4 rotation, 2 = all CPUs */
	int judged;    /* 0 for S1 */
};

static const struct pattern_info patterns[] = {
	{ LIT_P1, "P1", 1000000, 0, 1 },
	{ LIT_P2, "P2", 1000000, 0, 1 },
	{ LIT_P3, "P3", 100000, 0, 1 },
	{ LIT_P4, "P4", 1000000, 1, 1 },
	{ LIT_P5, "P5", 100000, 0, 1 },
	{ LIT_P6, "P6", 1000000, 2, 1 },
	{ LIT_P6L, "P6L", 1000000, 2, 1 },
	{ LIT_S1, "S1", 1000000, 0, 0 },
};
#define NPATTERNS (sizeof(patterns) / sizeof(patterns[0]))

struct total {
	unsigned long long runs, iters, forbidden, timeouts, pin_fail;
	double ms;
};

static struct total totals[NPATTERNS];

static int cpus[LIT_MAX_THREADS];
static int ncpus;
static int contiguous; /* online set is lo..hi with no gaps */

static double now_s(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

static void *role_main(void *p)
{
	struct role_arg *a = p;
	cpu_set_t set;

	CPU_ZERO(&set);
	CPU_SET(a->cpu, &set);
	a->pin_ok = sched_setaffinity(0, sizeof(set), &set) == 0 &&
		    sched_getcpu() == a->cpu;
	/*
	 * Even a role that failed to pin runs, so the barrier count stays
	 * complete; the row reports the failure.
	 */
	lit_run_role(&g_shared, a->role, &a->res);
	return NULL;
}

/* Runs one instance; the role order in cpu_list gives role 0, 1, ... */
static void run_one(int pass, const struct pattern_info *pi, lit_u64 iters,
		    const int *cpu_list, int n, struct total *t)
{
	struct role_arg args[LIT_MAX_THREADS];
	char set_str[128];
	size_t off = 0;
	lit_u64 forbidden = 0, timeouts = 0;
	int pin_fail = 0;
	double ms;

	memset(args, 0, sizeof(args));
	lit_init(&g_shared, pi->id, iters, (lit_u32)n);
	for (int i = 0; i < n; i++) {
		args[i].cpu = cpu_list[i];
		args[i].role = (lit_u32)i;
		if (pthread_create(&args[i].tid, NULL, role_main, &args[i]) != 0) {
			fprintf(stderr, "pthread_create failed: %s\n", strerror(errno));
			exit(2);
		}
	}
	for (int i = 0; i < n; i++) {
		pthread_join(args[i].tid, NULL);
		forbidden += args[i].res.forbidden;
		timeouts += args[i].res.timeouts;
		pin_fail += !args[i].pin_ok;
	}
	ms = (double)args[0].res.ticks * 1000.0 / (double)lit_cntfrq();

	if (pi->kind == 0) {
		snprintf(set_str, sizeof(set_str), "%d>%d", cpu_list[0], cpu_list[1]);
	} else if (contiguous) {
		/* Shared with C2: P4 "w<W>/<lo>-<hi>", P6 "<lo>-<hi>". */
		if (pi->kind == 1) {
			off = (size_t)snprintf(set_str, sizeof(set_str), "w%d/", cpu_list[0]);
		}
		snprintf(set_str + off, sizeof(set_str) - off, "%d-%d", cpus[0],
			 cpus[ncpus - 1]);
	} else {
		for (int i = 0; i < n && off < sizeof(set_str) - 8; i++) {
			const char *sep = i == 0 ? "" : (pi->kind == 1 && i == 1 ? ">" : ",");

			off += (size_t)snprintf(set_str + off, sizeof(set_str) - off,
						"%s%d", sep, cpu_list[i]);
		}
	}
	printf("ROW pass=%d pattern=%s cpus=%s iters=%llu forbidden=%llu timeouts=%llu ms=%.1f%s\n",
	       pass, pi->name, set_str, (unsigned long long)iters,
	       (unsigned long long)forbidden, (unsigned long long)timeouts, ms,
	       pin_fail ? " PIN_FAILED" : "");
	t->runs++;
	t->iters += iters;
	t->forbidden += forbidden;
	t->timeouts += timeouts;
	t->pin_fail += (unsigned long long)pin_fail;
	t->ms += ms;
}

static void run_pass(int pass, lit_u64 divisor, int have_lse)
{
	for (size_t k = 0; k < NPATTERNS; k++) {
		const struct pattern_info *pi = &patterns[k];
		lit_u64 iters = pi->iters / divisor;
		int list[LIT_MAX_THREADS];

		if (iters == 0) {
			iters = 1;
		}
		if (pi->id == LIT_P6L && !have_lse) {
			printf("SKIP pass=%d pattern=P6L reason=no-lse\n", pass);
			continue;
		}
		if (pi->kind == 0) {
			for (int a = 0; a < ncpus; a++) {
				for (int b = 0; b < ncpus; b++) {
					if (a == b) {
						continue;
					}
					list[0] = cpus[a];
					list[1] = cpus[b];
					run_one(pass, pi, iters, list, 2, &totals[k]);
				}
			}
		} else if (pi->kind == 1) {
			for (int w = 0; w < ncpus; w++) {
				int n = 0;

				list[n++] = cpus[w];
				for (int r = 0; r < ncpus; r++) {
					if (r != w) {
						list[n++] = cpus[r];
					}
				}
				run_one(pass, pi, iters, list, n, &totals[k]);
			}
		} else {
			run_one(pass, pi, iters, cpus, ncpus, &totals[k]);
		}
		fflush(stdout);
	}
}

static void print_online(void)
{
	char buf[64] = "?";
	FILE *f = fopen("/sys/devices/system/cpu/online", "r");

	if (f) {
		if (fgets(buf, sizeof(buf), f)) {
			buf[strcspn(buf, "\n")] = 0;
		}
		fclose(f);
	}
	printf("INFO sysfs_online=%s\n", buf);
}

int main(int argc, char **argv)
{
	double minutes = 10.0;
	lit_u64 divisor = 1;
	int one_pass = 0;
	int opt, pass = 0, fail = 0;
	cpu_set_t set;
	double t_start, t_end;
	int have_lse = (getauxval(AT_HWCAP) & HWCAP_ATOMICS) != 0;

	while ((opt = getopt(argc, argv, "m:d:1")) != -1) {
		switch (opt) {
		case 'm':
			minutes = atof(optarg);
			break;
		case 'd':
			divisor = strtoull(optarg, NULL, 0);
			if (divisor == 0) {
				divisor = 1;
			}
			break;
		case '1':
			one_pass = 1;
			break;
		default:
			fprintf(stderr, "usage: %s [-m minutes] [-d divisor] [-1]\n", argv[0]);
			return 2;
		}
	}
	setvbuf(stdout, NULL, _IOLBF, 0);

	if (sched_getaffinity(0, sizeof(set), &set) != 0) {
		perror("sched_getaffinity");
		return 2;
	}
	for (int c = 0; c < CPU_SETSIZE && ncpus < LIT_MAX_THREADS; c++) {
		if (CPU_ISSET(c, &set)) {
			cpus[ncpus++] = c;
		}
	}
	contiguous = ncpus > 0 && cpus[ncpus - 1] - cpus[0] == ncpus - 1;
	printf("LITMUS_BEGIN version=%s minutes=%.1f divisor=%llu spec=%s\n",
	       LIT_VERSION, minutes, (unsigned long long)divisor,
	       divisor == 1 ? "yes" : "no");
	printf("INFO cpus=%d list=", ncpus);
	for (int i = 0; i < ncpus; i++) {
		printf("%s%d", i ? "," : "", cpus[i]);
	}
	printf(" cntfrq=%llu lse=%d line=%d\n", (unsigned long long)lit_cntfrq(),
	       have_lse, LIT_LINE);
	print_online();
	if (ncpus < 2) {
		printf("LITMUS_ERROR need at least 2 CPUs\n");
		return 2;
	}

	t_start = now_s();
	do {
		double t0 = now_s();

		pass++;
		printf("PASS_BEGIN pass=%d t=%.1f\n", pass, t0 - t_start);
		run_pass(pass, divisor, have_lse);
		printf("PASS_END pass=%d s=%.1f\n", pass, now_s() - t0);
	} while (!one_pass && now_s() - t_start < minutes * 60.0);
	t_end = now_s();

	for (size_t k = 0; k < NPATTERNS; k++) {
		const struct total *t = &totals[k];
		int bad = patterns[k].judged &&
			  (t->forbidden || t->timeouts || t->pin_fail);

		if (t->runs == 0) {
			continue;
		}
		printf("TOTAL pattern=%s runs=%llu iters=%llu forbidden=%llu timeouts=%llu pin_fail=%llu ms=%.0f judged=%s result=%s\n",
		       patterns[k].name, t->runs, t->iters, t->forbidden, t->timeouts,
		       t->pin_fail, t->ms, patterns[k].judged ? "yes" : "no",
		       patterns[k].judged ? (bad ? "FAIL" : "PASS") : "REPORTED");
		fail |= bad;
	}
	printf("VERDICT %s passes=%d seconds=%.1f\n", fail ? "FAIL" : "PASS", pass,
	       t_end - t_start);
	printf("LITMUS_DONE\n");
	return fail ? 1 : 0;
}
