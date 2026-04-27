/*
 * pmm_internal.h - PMM internals exposed for unit testing.
 *
 * Not part of the public PMM API. Only consumers should be:
 *   - kernel/mm/pmm.c itself (definitions)
 *   - kernel/tests/test_pmm.c (regression coverage)
 *
 * Anything else should use kernel/include/pmm.h.
 */

#ifndef PMM_INTERNAL_H
#define PMM_INTERNAL_H

#include "dtb.h"
#include <stdint.h>

/*
 * Pure carve helper: given a region [start, end) and a list of
 * memory reservations, write the carved subranges (regions minus the
 * reserved sub-intervals) into out_starts/out_ends.
 *
 * @param start, end       Half-open input region.
 * @param rsv, n_rsv       Memory reservations to subtract. Reservations
 *                         that don't overlap [start, end) are ignored.
 *                         Reservations are NOT assumed sorted.
 * @param out_starts, out_ends, max_out
 *                         Output arrays for subrange bounds. At most
 *                         max_out entries are written.
 *
 * @return  Number of subranges written. 0 if start >= end. n_rsv + 1
 *          worst case when every reservation produces a split. Truncates
 *          silently at max_out.
 *
 * Pure: no allocation, no side effects, no PMM state mutation. Safe to
 * call from anywhere.
 */
int pmm_carve_reserves(uintptr_t start, uintptr_t end,
                       const dtb_memreserve_t *rsv, int n_rsv,
                       uintptr_t *out_starts, uintptr_t *out_ends,
                       int max_out);

#endif /* PMM_INTERNAL_H */
