/*
 * hailo_stub.c — No-op platform shim for non-RASPI5 ARM64 / x86-64.
 *
 * Keeps the main kernel's `hailo_platform_install()` call a valid
 * symbol on every platform. Returns HAILO_ERR_NODEV so callers can
 * tell "no Hailo here" apart from "Hailo present but something
 * failed". The core state machine stays at STATE_UNINIT.
 */

#include "platform.h"

#if !defined(PLATFORM_RASPI5)

#include "hailo.h"

int hailo_platform_install(void)
{
    /* Do not install a vtable — hailo_init() returns
     * HAILO_ERR_INVAL when hailo_platform is NULL, and the probe
     * path short-circuits the same way. */
    return HAILO_ERR_NODEV;
}

/* No-ops so cross-platform call sites under HAILO_WIRE_DEBUG link
 * cleanly on non-Pi5 builds. */
void hailo_platform_log_link_state(const char *label)
{
    (void)label;
}

void hailo_platform_dump_bridge_errors(const char *label)
{
    (void)label;
}

void hailo_platform_clear_bridge_errors(const char *label)
{
    (void)label;
}

#endif /* !PLATFORM_RASPI5 */
