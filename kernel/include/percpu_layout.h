/*
 * percpu_layout.h - Where the padded per-CPU arrays really are (test kernels)
 *
 * Each file that keeps a padded per-CPU array static exports a table of
 * { name, &array[0], &array[1], element size, element count } under
 * ENABLE_BOOT_TESTS. kernel/tests/test_percpu_layout.c walks the tables
 * and fails if any element is not on its own cache line (#1263). Global
 * arrays are checked by the test directly.
 */

#ifndef PERCPU_LAYOUT_H
#define PERCPU_LAYOUT_H

#include <stddef.h>

struct percpu_layout_ent {
    const char *name;
    const volatile void *e0;    /* &array[0] */
    const volatile void *e1;    /* &array[1] */
    size_t elem_size;           /* sizeof(array[0]) */
    size_t count;               /* number of elements */
};

#define PERCPU_LAYOUT_ENT(arr) \
    { #arr, &(arr)[0], &(arr)[1], sizeof((arr)[0]), sizeof(arr) / sizeof((arr)[0]) }

#if defined(ENABLE_BOOT_TESTS)
/* Each returns the number of entries and points *tab at them. */
size_t sched_percpu_layout(const struct percpu_layout_ent **tab);      /* sched.c */
#if !defined(PLATFORM_X86_64)
size_t pmu_percpu_layout(const struct percpu_layout_ent **tab);        /* arch/arm64/pmu.c */
#endif
#endif

#endif /* PERCPU_LAYOUT_H */
