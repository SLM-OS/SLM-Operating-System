/*
 * nvidia_gsp_platform_stub.c — ARM64 stub for VBIOS platform access.
 *
 * Jetson's GPU registers (0x17000000) are behind the CBB firewall and
 * NVIDIA_GSP_FIRMWARE is not bundled on this platform, so the GSP
 * bringup path in kernel/gpu/nvidia/bringup.c is never invoked at boot.
 * It is still linked (sharing source with the x86-64 build), so we
 * provide a non-functional stub for `nvidia_vbios_platform_load` to
 * satisfy the linker. Returns -1 so any accidental caller treats the
 * VBIOS as unavailable.
 */
#include "nvidia/nvidia_vbios.h"

int nvidia_vbios_platform_load(const uint8_t **out_data, size_t *out_size)
{
    (void)out_data;
    (void)out_size;
    return -1;
}
