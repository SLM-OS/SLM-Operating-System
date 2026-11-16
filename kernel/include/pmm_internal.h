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
#include <stddef.h>
#include <stdbool.h>

/*
 * Capacity of the user-reserve table (pmm_user_reserve_add). Sized for the
 * GA10B weights-pool extents list; see the comment above the table in
 * pmm.c before changing it. Here so tests can size reserve lists from it.
 */
#define PMM_MAX_USER_RESERVES 2048

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

/*
 * Page-aligned range holding a DTB blob of blob_size bytes at blob_addr
 * (#1244). Pure. False for a zero-size range, or one whose page-rounded
 * end would wrap past the top of the address space.
 */
bool pmm_dtb_reserve_range(uint64_t blob_addr, uint64_t blob_size,
                           dtb_memreserve_t *out);

/*
 * Number of free blocks overlapping [start, end). The unlocked form is for
 * pmm_init; tests use the _locked form.
 */
size_t pmm_free_overlap_count(uintptr_t start, uintptr_t end,
                              uintptr_t *first_addr, unsigned int *first_order);
size_t pmm_free_overlap_count_locked(uintptr_t start, uintptr_t end);

/*
 * The list pmm_add_region_split carves out of every region: firmware
 * /memreserve/ entries, user reserves, and the DTB blob's own range.
 */
int pmm_collect_reserves(dtb_memreserve_t *out, int max);


/*
 * Test hooks for kernel/tests (#1207). The test sources are part of every
 * image (CMake TEST_SOURCES), so these are too.
 *
 * pmm_test_add_region() adds [start, end) to the allocator the way
 * pmm_init adds a carved subrange. The caller must own every page the
 * range touches (allocated from the PMM, not freed); ownership of the
 * pages the allocator lists passes to the allocator.
 *
 * pmm_free_block_order() returns the order of the listed free block that
 * starts at addr, or -1 when no listed block starts there. Walks the
 * lists: tests only.
 */
void pmm_test_add_region(uintptr_t start, uintptr_t end);
int pmm_free_block_order(uintptr_t addr);

/*
 * Test hook for the free-list verifier: under pmm_lock, clear the next
 * link of the head of the lowest order that has two listed blocks, verify
 * twice, put the link back and verify again. Returns that order (-1: no
 * such order) and the three results (orders with an inconsistency). The
 * lock is held throughout, so no allocation ever sees the cleared link.
 */
int pmm_test_verify_cleared_link(unsigned int *first, unsigned int *second,
                                 unsigned int *restored);

/*
 * Test hook: whether the verifier's dump would read the 64 bytes at addr
 * (only a free page inside the heap is read; the heap range has unmapped
 * holes on Jetson).
 */
bool pmm_test_dump_readable(uintptr_t addr);

#endif /* PMM_INTERNAL_H */
