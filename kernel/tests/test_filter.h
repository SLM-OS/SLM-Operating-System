/*
 * Boot-test filter and repeat count.
 *
 * A test kernel normally runs every suite once. With a filter it runs
 * only the named tests (exact RUN_TEST names, comma-separated), N times,
 * printing one line per iteration, then a per-test summary and the usual
 * "Test Suite Summary" block. See docs/testing.md "Running selected
 * tests" for the sources (kernel command line, build time, shell) and
 * how each hardware boot path passes them.
 *
 * The parser functions are pure (no globals) so the test_filter suite
 * can exercise them; the active filter is a single global that
 * UnityRunTest() and the harness consult.
 */

#ifndef TEST_FILTER_H
#define TEST_FILTER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define TEST_FILTER_MAX_NAMES   16
#define TEST_FILTER_NAME_MAX    96      /* including the NUL */
#define TEST_FILTER_REPEAT_MAX  1000000u

/* Kernel command line keys (whitespace-separated key=value tokens). */
#define TEST_FILTER_KEY_NAMES   "slmos.test"
#define TEST_FILTER_KEY_REPEAT  "slmos.test_repeat"

/* Parse results. */
#define TEST_FILTER_OK            0
#define TEST_FILTER_ERR_EMPTY    -1     /* no name, or an empty element */
#define TEST_FILTER_ERR_TOO_MANY -2     /* more than TEST_FILTER_MAX_NAMES */
#define TEST_FILTER_ERR_TOO_LONG -3     /* a name of TEST_FILTER_NAME_MAX or more */
#define TEST_FILTER_ERR_CHAR     -4     /* a character outside [A-Za-z0-9_] */
#define TEST_FILTER_ERR_REPEAT   -5     /* repeat not a decimal in 1..MAX */

typedef struct {
    uint32_t runs;
    uint32_t passed;
    uint32_t failed;
    uint32_t ignored;
    uint64_t total_ticks;
    uint64_t min_ticks;
    uint64_t max_ticks;
} test_filter_stats_t;

typedef struct {
    bool     active;
    uint32_t repeat;                    /* passes over the named tests */
    uint32_t count;                     /* number of names */
    char     names[TEST_FILTER_MAX_NAMES][TEST_FILTER_NAME_MAX];
    const char *source;                 /* "cmdline", "build", "shell" */
} test_filter_t;

/* Parse "a,b,c" (len bytes, not necessarily NUL-terminated) into f->names.
 * On success f->count is set; f->active and f->repeat are untouched. */
int test_filter_parse_names(test_filter_t *f, const char *csv, size_t len);

/* Parse a decimal repeat count in 1..TEST_FILTER_REPEAT_MAX. */
int test_filter_parse_repeat(const char *s, size_t len, uint32_t *out);

/* Find `key=value` in a whitespace-separated command line. Returns the
 * value and its length, or NULL when the key is absent. */
const char *test_filter_find_kv(const char *cmdline, const char *key,
                                size_t *out_len);

/* Fill f from a command line. Returns 1 when slmos.test= was found and
 * parsed (f->active set, repeat from slmos.test_repeat= or 1), 0 when
 * absent (f untouched), or a negative TEST_FILTER_ERR_* code. */
int test_filter_from_cmdline(test_filter_t *f, const char *cmdline);

/* Index of name in f->names, or -1. Exact, case-sensitive match. */
int test_filter_index_of(const test_filter_t *f, const char *name);

/* ---- Active filter (one per kernel) ------------------------------------ */

extern test_filter_t       g_test_filter;
extern test_filter_stats_t g_test_filter_stats[TEST_FILTER_MAX_NAMES];
extern uint32_t            g_test_filter_pass;   /* 1-based current pass */

static inline bool test_filter_active(void)
{
    return g_test_filter.active;
}

/* ---- Test-name to suite index (generated at build time) --------------- */

/* Name of the first suite function (e.g. "test_suite_scheduler") whose
 * RUN_TEST list contains test_name, starting the search at *cursor and
 * advancing it; NULL when there are no more. Start with *cursor = 0.
 * Built by scripts/gen-test-index.py into the boot-test image; other
 * images get an empty index. */
const char *test_index_next_suite(const char *test_name, uint32_t *cursor);

/* Number of entries in the generated index (0 outside boot-test images). */
uint32_t test_index_size(void);

#endif /* TEST_FILTER_H */
