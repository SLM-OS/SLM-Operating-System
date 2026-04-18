/*
 * pcie_stub.c — No-op PCIe backend for platforms without a PCIe
 * host-controller driver (Jetson today; any future ARM64 platform
 * not explicitly supported).
 *
 * Keeps `pcie_init()` callable and returns PCIE_ERR_UNSUPPORTED so
 * callers that ask for PCIe get a clean negative answer rather than
 * a link error. No host_ops is installed — pcie_core sees that and
 * short-circuits the enumeration.
 */

#include "platform.h"

#if defined(PLATFORM_JETSON_ORIN_NANO)

#include "pcie.h"

int pcie_backend_register(void)
{
    /* Deliberately do not install host_ops. pcie_init() will log
     * "no backend available" and return this error. */
    return PCIE_ERR_UNSUPPORTED;
}

#endif /* PLATFORM_JETSON_ORIN_NANO */
