/*
 * ce_stubs.c — link-time stubs for `test_ga10b_ce`.
 *
 * The encoding tests only exercise `ga10b_build_ce_memcpy_pushbuffer`
 * (pure logic). The runtime entry `ga10b_ce_memcpy` is compiled in to
 * keep the test linking against the real `ga10b_ce.o`, but never
 * called — these stubs satisfy its references to bringup-state
 * helpers so the host link succeeds without dragging in
 * `ga10b_bringup.c` (which needs MMIO, PMM, UART, ...).
 */

#include <stddef.h>
#include <stdint.h>

#include "ga10b_bringup.h"
#include "ga10b_channel_handoff.h"

const struct ga10b_channel_handoff *ga10b_bringup_handoff(void)
{
    return NULL;
}

int ga10b_submit_and_poll(struct ga10b_bringup *b,
                          const uint32_t *pb_buf, uint32_t pb_dwords,
                          uint64_t poll_phys, uint32_t expected_payload,
                          int error_phase, const char *tag)
{
    (void)b; (void)pb_buf; (void)pb_dwords; (void)poll_phys;
    (void)expected_payload; (void)error_phase; (void)tag;
    return -1;
}
