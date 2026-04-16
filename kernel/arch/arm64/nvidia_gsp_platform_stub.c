/*
 * nvidia_gsp_platform_stub.c — ARM64 linker stub for VBIOS access.
 *
 * Satisfies the `nvidia_vbios_platform_load` symbol so the shared
 * GSP code links on ARM64. Returns -1 (VBIOS unavailable). Replace
 * with a full `struct gsp_platform_ops` implementation when bringing
 * up GSP on Jetson — see docs/nvidia-gsp.md §"Platform Shim Contract".
 */
#include "nvidia/nvidia_vbios.h"

int nvidia_vbios_platform_load(const uint8_t **out_data, size_t *out_size)
{
    (void)out_data;
    (void)out_size;
    return -1;
}
