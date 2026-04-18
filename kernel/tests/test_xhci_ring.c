/*
 * test_xhci_ring.c - Phase 3A ring primitive unit tests (#266)
 *
 * The XHCI ring primitives (cycle-bit, Link TRB wrap, event-ring
 * dequeue) are pure logic over a memory buffer — unit-testable on
 * every platform the harness supports. The ncmem-backed wrappers
 * are Jetson-only; these tests drive the init-with-buffer variants
 * directly with stack-allocated TRB arrays.
 *
 * Phase 3A is mothballed (see docs/jetson-usb-networking-plan.md §8),
 * but the ring code is likely to survive any future revival path,
 * so regression coverage is worth keeping.
 */

#include "unity.h"
#include "test_harness.h"
#include "../drivers/usb/xhci/xhci_ring.h"
#include "../drivers/usb/xhci/xhci_trb.h"

#include <stdint.h>
#include <stddef.h>
#include <string.h>

/* -------------------------------------------------------------------------- */
/* init / alloc                                                                */
/* -------------------------------------------------------------------------- */

static void test_ring_init_rejects_null(void)
{
    struct xhci_trb trbs[8];
    struct xhci_ring r;
    TEST_ASSERT_NOT_EQUAL(0, xhci_ring_init(NULL, trbs, (uintptr_t)trbs, 8));
    TEST_ASSERT_NOT_EQUAL(0, xhci_ring_init(&r, NULL, 0, 8));
}

static void test_ring_init_rejects_tiny(void)
{
    struct xhci_trb trbs[4];
    struct xhci_ring r;
    /* num_trbs < 4 rejected (need at least Link TRB + some producer slots). */
    TEST_ASSERT_NOT_EQUAL(0, xhci_ring_init(&r, trbs, (uintptr_t)trbs, 2));
    TEST_ASSERT_NOT_EQUAL(0, xhci_ring_init(&r, trbs, (uintptr_t)trbs, 3));
    TEST_ASSERT_EQUAL_INT(0, xhci_ring_init(&r, trbs, (uintptr_t)trbs, 4));
}

static void test_ring_init_zeroes_and_writes_link(void)
{
    /* Pre-fill the buffer with non-zero garbage to prove init zeroes. */
    struct xhci_trb trbs[8];
    memset(trbs, 0xA5, sizeof(trbs));

    struct xhci_ring r;
    TEST_ASSERT_EQUAL_INT(0, xhci_ring_init(&r, trbs, (uintptr_t)trbs, 8));

    /* Slots 0..6 must be zeroed — the HC uses the cycle bit (0) to
     * recognise "not-yet-produced" TRBs. */
    for (unsigned i = 0; i < 7; i++) {
        TEST_ASSERT_EQUAL_UINT32(0, trbs[i].param_lo);
        TEST_ASSERT_EQUAL_UINT32(0, trbs[i].param_hi);
        TEST_ASSERT_EQUAL_UINT32(0, trbs[i].status);
        TEST_ASSERT_EQUAL_UINT32(0, trbs[i].control);
    }
    /* Slot 7 is the Link TRB: type=Link, TC=1, cycle=0, param = base. */
    struct xhci_trb *link = &trbs[7];
    TEST_ASSERT_EQUAL_UINT32((uint32_t)((uintptr_t)trbs & 0xFFFFFFFFu),
                             link->param_lo);
    TEST_ASSERT_EQUAL_UINT32(XHCI_TRB_TYPE_GET(link->control), XHCI_TRB_LINK);
    TEST_ASSERT_TRUE(link->control & XHCI_TRB_TC);
    TEST_ASSERT_FALSE(link->control & XHCI_TRB_CYCLE);

    TEST_ASSERT_EQUAL_UINT32(8, r.num_trbs);
    TEST_ASSERT_EQUAL_UINT32(0, r.enqueue);
    TEST_ASSERT_EQUAL_UINT8(1, r.cycle_state);
}

/* -------------------------------------------------------------------------- */
/* enqueue                                                                     */
/* -------------------------------------------------------------------------- */

static void test_enqueue_rejects_null(void)
{
    struct xhci_trb trbs[8];
    struct xhci_ring r;
    struct xhci_trb t = {0};
    TEST_ASSERT_EQUAL_INT(0, xhci_ring_init(&r, trbs, (uintptr_t)trbs, 8));
    TEST_ASSERT_NULL(xhci_ring_enqueue(NULL, &t));
    TEST_ASSERT_NULL(xhci_ring_enqueue(&r, NULL));
}

static void test_enqueue_sets_cycle_to_pcs(void)
{
    /*
     * Ring starts with PCS=1. Every enqueue must set the TRB's cycle
     * bit to PCS regardless of what the caller-supplied TRB had —
     * the caller doesn't own the cycle bit, the ring does.
     */
    struct xhci_trb trbs[8];
    struct xhci_ring r;
    TEST_ASSERT_EQUAL_INT(0, xhci_ring_init(&r, trbs, (uintptr_t)trbs, 8));

    struct xhci_trb cmd = {0};
    cmd.control = XHCI_TRB_TYPE(XHCI_TRB_CMD_NOOP);   /* cycle bit deliberately 0 */

    struct xhci_trb *slot = xhci_ring_enqueue(&r, &cmd);
    TEST_ASSERT_EQUAL_PTR(&trbs[0], slot);
    TEST_ASSERT_EQUAL_UINT32(XHCI_TRB_CMD_NOOP, XHCI_TRB_TYPE_GET(slot->control));
    TEST_ASSERT_TRUE(slot->control & XHCI_TRB_CYCLE);   /* PCS=1 */
    TEST_ASSERT_EQUAL_UINT32(1, r.enqueue);
}

static void test_enqueue_preserves_non_cycle_bits(void)
{
    /*
     * Caller may pass other flags (IOC, chain, IDT). The ring must
     * clear only the cycle bit and leave other bits untouched.
     */
    struct xhci_trb trbs[8];
    struct xhci_ring r;
    TEST_ASSERT_EQUAL_INT(0, xhci_ring_init(&r, trbs, (uintptr_t)trbs, 8));

    struct xhci_trb cmd = {0};
    cmd.control = XHCI_TRB_TYPE(XHCI_TRB_CMD_NOOP)
                  | XHCI_TRB_IOC | XHCI_TRB_IDT
                  | XHCI_TRB_CYCLE;   /* caller's stale cycle bit; should be overwritten */

    struct xhci_trb *slot = xhci_ring_enqueue(&r, &cmd);
    TEST_ASSERT_NOT_NULL(slot);
    TEST_ASSERT_TRUE(slot->control & XHCI_TRB_IOC);
    TEST_ASSERT_TRUE(slot->control & XHCI_TRB_IDT);
    TEST_ASSERT_TRUE(slot->control & XHCI_TRB_CYCLE);   /* PCS=1 anyway */
}

static void test_enqueue_copies_payload(void)
{
    struct xhci_trb trbs[8];
    struct xhci_ring r;
    TEST_ASSERT_EQUAL_INT(0, xhci_ring_init(&r, trbs, (uintptr_t)trbs, 8));

    struct xhci_trb cmd = {
        .param_lo = 0xCAFEBABEu,
        .param_hi = 0xDEADBEEFu,
        .status   = 0x12345678u,
        .control  = XHCI_TRB_TYPE(XHCI_TRB_CMD_NOOP),
    };
    struct xhci_trb *slot = xhci_ring_enqueue(&r, &cmd);
    TEST_ASSERT_EQUAL_UINT32(0xCAFEBABEu, slot->param_lo);
    TEST_ASSERT_EQUAL_UINT32(0xDEADBEEFu, slot->param_hi);
    TEST_ASSERT_EQUAL_UINT32(0x12345678u, slot->status);
}

static void test_enqueue_wraps_at_link_trb(void)
{
    /*
     * 8-TRB ring means 7 usable slots (slot 7 is the Link). Enqueue
     * 7 items, then enqueue an 8th: it must wrap to slot 0, toggle
     * PCS to 0, and flip the Link TRB's cycle bit so the HC follows
     * the link.
     */
    struct xhci_trb trbs[8];
    struct xhci_ring r;
    TEST_ASSERT_EQUAL_INT(0, xhci_ring_init(&r, trbs, (uintptr_t)trbs, 8));

    struct xhci_trb cmd = {0};
    cmd.control = XHCI_TRB_TYPE(XHCI_TRB_CMD_NOOP);

    /* Fill slots 0..6 with PCS=1. */
    for (unsigned i = 0; i < 7; i++) {
        struct xhci_trb *slot = xhci_ring_enqueue(&r, &cmd);
        TEST_ASSERT_EQUAL_PTR(&trbs[i], slot);
        TEST_ASSERT_TRUE(slot->control & XHCI_TRB_CYCLE);
    }
    TEST_ASSERT_EQUAL_UINT32(7, r.enqueue);
    TEST_ASSERT_EQUAL_UINT8(1, r.cycle_state);

    /* Next enqueue: wraps, toggles PCS, sets Link cycle. */
    struct xhci_trb *wrap_slot = xhci_ring_enqueue(&r, &cmd);
    TEST_ASSERT_EQUAL_PTR(&trbs[0], wrap_slot);
    TEST_ASSERT_EQUAL_UINT8(0, r.cycle_state);

    /* The wrapped TRB was written with the NEW PCS (0), so its cycle
     * bit is clear. The Link TRB (slot 7) had its cycle bit SET to
     * the PREVIOUS PCS (1) so the HC follows it. */
    TEST_ASSERT_FALSE(trbs[0].control & XHCI_TRB_CYCLE);
    TEST_ASSERT_TRUE(trbs[7].control & XHCI_TRB_CYCLE);
    TEST_ASSERT_EQUAL_UINT32(1, r.enqueue);
}

static void test_enqueue_double_wrap_toggles_pcs_back(void)
{
    /*
     * Two full wraparounds — PCS should come back to 1, Link cycle
     * should come back to 0 (for the next iteration).
     */
    struct xhci_trb trbs[8];
    struct xhci_ring r;
    TEST_ASSERT_EQUAL_INT(0, xhci_ring_init(&r, trbs, (uintptr_t)trbs, 8));

    struct xhci_trb cmd = {0};
    cmd.control = XHCI_TRB_TYPE(XHCI_TRB_CMD_NOOP);

    /* 7 + 1 wrap + 7 + 1 wrap = 16 enqueues total = two laps. */
    for (unsigned i = 0; i < 16; i++)
        TEST_ASSERT_NOT_NULL(xhci_ring_enqueue(&r, &cmd));
    TEST_ASSERT_EQUAL_UINT32(2, r.enqueue);   /* 16 mod 7 ... no, via two wraps */
    TEST_ASSERT_EQUAL_UINT8(1, r.cycle_state);   /* back to starting PCS */
}

/* -------------------------------------------------------------------------- */
/* event ring peek                                                             */
/* -------------------------------------------------------------------------- */

static void test_event_ring_init(void)
{
    struct xhci_trb trbs[8];
    memset(trbs, 0xFF, sizeof(trbs));
    struct xhci_event_ring r;
    TEST_ASSERT_EQUAL_INT(0, xhci_event_ring_init(&r, trbs, (uintptr_t)trbs, 8));

    /* All slots zeroed. */
    for (unsigned i = 0; i < 8; i++)
        TEST_ASSERT_EQUAL_UINT32(0, trbs[i].control);
    TEST_ASSERT_EQUAL_UINT32(8, r.num_trbs);
    TEST_ASSERT_EQUAL_UINT32(0, r.dequeue);
    TEST_ASSERT_EQUAL_UINT8(1, r.cycle_state);
}

static void test_event_ring_peek_empty(void)
{
    /*
     * Fresh event ring: all slots have cycle=0, ECS=1, so peek
     * should report no event available.
     */
    struct xhci_trb trbs[8];
    struct xhci_event_ring r;
    TEST_ASSERT_EQUAL_INT(0, xhci_event_ring_init(&r, trbs, (uintptr_t)trbs, 8));

    struct xhci_trb out;
    TEST_ASSERT_FALSE(xhci_event_ring_peek(&r, &out));
    TEST_ASSERT_EQUAL_UINT32(0, r.dequeue);     /* didn't advance */
}

static void test_event_ring_peek_consumes_matching_cycle(void)
{
    /*
     * Simulate the HC writing an event: set cycle=1 on slot 0
     * matching ECS. Peek should return it, advance dequeue, and the
     * next peek should see cycle mismatch again.
     */
    struct xhci_trb trbs[8];
    struct xhci_event_ring r;
    TEST_ASSERT_EQUAL_INT(0, xhci_event_ring_init(&r, trbs, (uintptr_t)trbs, 8));

    trbs[0].param_lo = 0xDEADBEEFu;
    trbs[0].status   = (uint32_t)(XHCI_CC_SUCCESS << XHCI_CC_SHIFT);
    trbs[0].control  = XHCI_TRB_TYPE(XHCI_TRB_EVT_CMD_COMPLETION) | XHCI_TRB_CYCLE;

    struct xhci_trb out;
    TEST_ASSERT_TRUE(xhci_event_ring_peek(&r, &out));
    TEST_ASSERT_EQUAL_UINT32(0xDEADBEEFu, out.param_lo);
    TEST_ASSERT_EQUAL_UINT32(XHCI_TRB_EVT_CMD_COMPLETION,
                             XHCI_TRB_TYPE_GET(out.control));
    TEST_ASSERT_EQUAL_UINT32(XHCI_CC_SUCCESS, XHCI_CC_GET(out.status));
    TEST_ASSERT_EQUAL_UINT32(1, r.dequeue);

    /* Second peek: slot 1 still has cycle=0, peek returns false. */
    TEST_ASSERT_FALSE(xhci_event_ring_peek(&r, &out));
    TEST_ASSERT_EQUAL_UINT32(1, r.dequeue);
}

static void test_event_ring_peek_wraps_and_toggles_ecs(void)
{
    /*
     * 4-TRB event ring. Fill with cycle=1 in every slot to simulate
     * the HC producing 4 events. Consume all 4 with peek(). On the
     * 4th consume, dequeue wraps to 0 and ECS toggles 1→0. A 5th
     * peek against cycle=1 slots should now return false (mismatch).
     */
    struct xhci_trb trbs[4];
    struct xhci_event_ring r;
    TEST_ASSERT_EQUAL_INT(0, xhci_event_ring_init(&r, trbs, (uintptr_t)trbs, 4));

    for (unsigned i = 0; i < 4; i++) {
        trbs[i].param_lo = 0x10000u + i;
        trbs[i].control  = XHCI_TRB_TYPE(XHCI_TRB_EVT_CMD_COMPLETION)
                           | XHCI_TRB_CYCLE;
    }

    struct xhci_trb out;
    for (unsigned i = 0; i < 4; i++) {
        TEST_ASSERT_TRUE(xhci_event_ring_peek(&r, &out));
        TEST_ASSERT_EQUAL_UINT32(0x10000u + i, out.param_lo);
    }
    TEST_ASSERT_EQUAL_UINT32(0, r.dequeue);     /* wrapped */
    TEST_ASSERT_EQUAL_UINT8(0, r.cycle_state);  /* ECS toggled */

    /* Slots still show cycle=1, but ECS is now 0 — mismatch. */
    TEST_ASSERT_FALSE(xhci_event_ring_peek(&r, &out));
}

static void test_event_ring_dequeue_phys(void)
{
    struct xhci_trb trbs[8];
    struct xhci_event_ring r;
    uintptr_t base = 0x80000000UL;  /* pretend physical address */
    TEST_ASSERT_EQUAL_INT(0, xhci_event_ring_init(&r, trbs, base, 8));

    /* dequeue=0 → phys = base */
    TEST_ASSERT_EQUAL_UINT64(base, xhci_event_ring_dequeue_phys(&r));

    /* Advance dequeue to 3 by consuming 3 slots. */
    for (unsigned i = 0; i < 3; i++)
        trbs[i].control = XHCI_TRB_CYCLE;   /* match ECS=1 */
    struct xhci_trb out;
    for (unsigned i = 0; i < 3; i++)
        TEST_ASSERT_TRUE(xhci_event_ring_peek(&r, &out));

    /* dequeue=3 → phys = base + 3*sizeof(TRB) = base + 48. */
    TEST_ASSERT_EQUAL_UINT64(base + 3 * 16,
                             xhci_event_ring_dequeue_phys(&r));
}

static void test_event_ring_peek_null_args(void)
{
    struct xhci_trb trbs[4];
    struct xhci_event_ring r;
    struct xhci_trb out;
    TEST_ASSERT_EQUAL_INT(0, xhci_event_ring_init(&r, trbs, 0, 4));
    TEST_ASSERT_FALSE(xhci_event_ring_peek(NULL, &out));
    TEST_ASSERT_FALSE(xhci_event_ring_peek(&r, NULL));
}

static void test_event_ring_peek_then_dequeue_phys_tracks_advance(void)
{
    /*
     * Regression coverage for the xhci_send_noop ERDP-update invariant:
     * after every peek that returns true, xhci_event_ring_dequeue_phys
     * must report the address of the NEXT-to-consume slot. The NO_OP
     * poll loop writes dequeue_phys to ERDP on every consumed event
     * (match or skip); if peek ever advanced dequeue without the phys
     * tracking, the HC would see a stale ERDP and eventually stop
     * delivering events once the ring filled.
     */
    struct xhci_trb trbs[8];
    struct xhci_event_ring r;
    uintptr_t base = 0x80000000UL;
    TEST_ASSERT_EQUAL_INT(0, xhci_event_ring_init(&r, trbs, base, 8));

    /* Seed 4 consecutive events with matching ECS. */
    for (unsigned i = 0; i < 4; i++) {
        trbs[i].param_lo = 0xA000u + i;
        trbs[i].control  = XHCI_TRB_TYPE(XHCI_TRB_EVT_CMD_COMPLETION)
                           | XHCI_TRB_CYCLE;
    }

    struct xhci_trb out;
    for (unsigned i = 0; i < 4; i++) {
        /* Before peek: dequeue_phys points at slot i. */
        TEST_ASSERT_EQUAL_UINT64(base + (uint64_t)i * 16,
                                 xhci_event_ring_dequeue_phys(&r));
        TEST_ASSERT_TRUE(xhci_event_ring_peek(&r, &out));
        TEST_ASSERT_EQUAL_UINT32(0xA000u + i, out.param_lo);
        /* After peek: dequeue_phys points at slot i+1 (HC's next-to-
         * produce), which is exactly what must go into ERDP. */
        TEST_ASSERT_EQUAL_UINT64(base + (uint64_t)(i + 1) * 16,
                                 xhci_event_ring_dequeue_phys(&r));
    }
}

static void test_event_ring_peek_skip_then_match_advances_through_all(void)
{
    /*
     * Models xhci_send_noop's inner loop: the first CMD_COMPLETION
     * event doesn't correlate with our submitted command (stale
     * Linux-era residue or an unrelated event), so the driver skips
     * it. The second event matches and ends the poll. Each peek must
     * consume a distinct slot AND the physical dequeue must advance
     * in lock-step so the ERDP write covers both slots.
     */
    struct xhci_trb trbs[8];
    struct xhci_event_ring r;
    uintptr_t base = 0xD000u;
    TEST_ASSERT_EQUAL_INT(0, xhci_event_ring_init(&r, trbs, base, 8));

    /* Slot 0: unrelated CMD_COMPLETION (simulated stale pointer). */
    trbs[0].param_lo = 0xCAFECAFEu;
    trbs[0].param_hi = 0;
    trbs[0].status   = (uint32_t)(XHCI_CC_SUCCESS << XHCI_CC_SHIFT);
    trbs[0].control  = XHCI_TRB_TYPE(XHCI_TRB_EVT_CMD_COMPLETION)
                       | XHCI_TRB_CYCLE;
    /* Slot 1: the completion we actually want. */
    trbs[1].param_lo = 0xBEEFu;
    trbs[1].param_hi = 0;
    trbs[1].status   = (uint32_t)(XHCI_CC_SUCCESS << XHCI_CC_SHIFT);
    trbs[1].control  = XHCI_TRB_TYPE(XHCI_TRB_EVT_CMD_COMPLETION)
                       | XHCI_TRB_CYCLE;

    struct xhci_trb out;

    /* First peek: stale event. Must return true and advance. */
    TEST_ASSERT_TRUE(xhci_event_ring_peek(&r, &out));
    TEST_ASSERT_EQUAL_UINT32(0xCAFECAFEu, out.param_lo);
    TEST_ASSERT_EQUAL_UINT64(base + 16, xhci_event_ring_dequeue_phys(&r));

    /* Second peek: matching event. */
    TEST_ASSERT_TRUE(xhci_event_ring_peek(&r, &out));
    TEST_ASSERT_EQUAL_UINT32(0xBEEFu, out.param_lo);
    TEST_ASSERT_EQUAL_UINT64(base + 32, xhci_event_ring_dequeue_phys(&r));

    /* Third peek: no more events. */
    TEST_ASSERT_FALSE(xhci_event_ring_peek(&r, &out));
    /* Dequeue_phys unchanged — failed peek does not advance. */
    TEST_ASSERT_EQUAL_UINT64(base + 32, xhci_event_ring_dequeue_phys(&r));
}

static void test_event_ring_dequeue_phys_after_wrap(void)
{
    /*
     * After a full lap, dequeue wraps to 0 and ECS toggles. The ERDP
     * value we'd write at that point must point at the ring base
     * (not past the end). Explicit coverage because the wrap path is
     * exactly where subtle ERDP arithmetic bugs hide.
     */
    struct xhci_trb trbs[4];
    struct xhci_event_ring r;
    uintptr_t base = 0x40000000UL;
    TEST_ASSERT_EQUAL_INT(0, xhci_event_ring_init(&r, trbs, base, 4));

    for (unsigned i = 0; i < 4; i++) {
        trbs[i].control = XHCI_TRB_TYPE(XHCI_TRB_EVT_CMD_COMPLETION)
                          | XHCI_TRB_CYCLE;
    }

    struct xhci_trb out;
    /* Consume 3 — dequeue_phys tracks slot 3. */
    for (unsigned i = 0; i < 3; i++)
        TEST_ASSERT_TRUE(xhci_event_ring_peek(&r, &out));
    TEST_ASSERT_EQUAL_UINT64(base + 3 * 16, xhci_event_ring_dequeue_phys(&r));

    /* 4th consume wraps. dequeue goes back to 0, ECS toggles 1→0. */
    TEST_ASSERT_TRUE(xhci_event_ring_peek(&r, &out));
    TEST_ASSERT_EQUAL_UINT8(0, r.cycle_state);
    TEST_ASSERT_EQUAL_UINT64(base, xhci_event_ring_dequeue_phys(&r));
}

static void test_erst_entry_layout(void)
{
    /*
     * The driver's xhci_erst_entry isn't exported via the ring header,
     * but any replacement must satisfy xHCI 1.2 §6.5: 16 bytes total,
     * base_lo at +0, base_hi at +4, size at +8, reserved at +12. A
     * regression that adds padding or re-orders fields would surface
     * here as soon as a matching struct is re-declared.
     */
    struct erst_entry_test {
        uint32_t base_lo;
        uint32_t base_hi;
        uint32_t size;
        uint32_t reserved;
    };
    TEST_ASSERT_EQUAL_UINT32(16, sizeof(struct erst_entry_test));
    TEST_ASSERT_EQUAL_UINT32(0,  offsetof(struct erst_entry_test, base_lo));
    TEST_ASSERT_EQUAL_UINT32(4,  offsetof(struct erst_entry_test, base_hi));
    TEST_ASSERT_EQUAL_UINT32(8,  offsetof(struct erst_entry_test, size));
    TEST_ASSERT_EQUAL_UINT32(12, offsetof(struct erst_entry_test, reserved));
}

/* -------------------------------------------------------------------------- */
/* Suite entry                                                                 */
/* -------------------------------------------------------------------------- */

int test_suite_xhci_ring(void);

int test_suite_xhci_ring(void)
{
    UnityBegin("test_xhci_ring.c");
    RUN_TEST(test_ring_init_rejects_null);
    RUN_TEST(test_ring_init_rejects_tiny);
    RUN_TEST(test_ring_init_zeroes_and_writes_link);
    RUN_TEST(test_enqueue_rejects_null);
    RUN_TEST(test_enqueue_sets_cycle_to_pcs);
    RUN_TEST(test_enqueue_preserves_non_cycle_bits);
    RUN_TEST(test_enqueue_copies_payload);
    RUN_TEST(test_enqueue_wraps_at_link_trb);
    RUN_TEST(test_enqueue_double_wrap_toggles_pcs_back);
    RUN_TEST(test_event_ring_init);
    RUN_TEST(test_event_ring_peek_empty);
    RUN_TEST(test_event_ring_peek_consumes_matching_cycle);
    RUN_TEST(test_event_ring_peek_wraps_and_toggles_ecs);
    RUN_TEST(test_event_ring_dequeue_phys);
    RUN_TEST(test_event_ring_peek_null_args);
    RUN_TEST(test_event_ring_peek_then_dequeue_phys_tracks_advance);
    RUN_TEST(test_event_ring_peek_skip_then_match_advances_through_all);
    RUN_TEST(test_event_ring_dequeue_phys_after_wrap);
    RUN_TEST(test_erst_entry_layout);
    return UnityEnd();
}
