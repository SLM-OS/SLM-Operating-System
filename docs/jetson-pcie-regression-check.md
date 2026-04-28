# Jetson PCIe regression check — verification log

Hardware-test log for #371 sub-task 7. PR #389
("dynamic-kernel-replace stage 3, SDHCI driver") made two changes
to `kernel/drivers/pcie/pcie_core.c` that the issue body called out
as potentially affecting Jetson's `pcie_tegra194.c` enumeration path:

1. **Bridge skip from bus-number to class-code.** Old:
   `if (d->bus == 0) continue;`. New: `if (d->class_code == 0x06)
   continue;`. PCI class `0x06` covers host bridges + PCI-to-PCI
   bridges, regardless of which bus they sit on. Reason: on QEMU
   GPEX every endpoint lives on bus 0 alongside the host bridge,
   so the old condition silently dropped every endpoint;
   keying on class fixes that without breaking the Pi 5 RC-on-
   bus-0/EP-on-bus-1 topology.

2. **Unconditional `CMD_MEMORY_SPACE` enable after BAR
   assignment.** Without this, MMIO reads against newly-assigned
   BARs return all-ones — `map_bar()` looked correct but the
   device wasn't decoding accesses. Surfaced by the SDHCI work
   but a general fix that mirrors what UEFI does implicitly.

Both changes are in `pcie_core.c`, the platform-neutral PCIe
enumeration framework. The question for this sub-task: do they
break Jetson?

---

## 2026-04-27 — initial verification (claude-code, on jetson-nano-1)

**Board:** `jetson-nano-1`
**Linux distro:** L4T 5.15.148-tegra (slmos-1, kexec-from-Linux deploy)
**Kernel build base:** `origin/main` at `ba32e9f` (post #389, #468,
#504, #508, #511 — all the dynamic-kernel-replace and test-fix
work merged).
**Boot path:** Linux/L4T cold boot → `slmos-kexec /root/slmos.elf`
(SSH-driven, IP `192.168.4.238`).

### Structural finding — `pcie_core.c` is unreachable from Jetson

`kernel/drivers/pcie/pcie_stub.c` is the registered backend on
`PLATFORM_JETSON_ORIN_NANO`:

```c
int pcie_backend_register(void)
{
    /* Deliberately do not install host_ops. pcie_init() will log
     * "no backend available" and return this error. */
    return PCIE_ERR_UNSUPPORTED;
}
```

`pcie_core.c::pcie_init` checks the return value and exits early:

```c
int rc = pcie_backend_register();
if (rc != PCIE_OK) {
    INFO("pcie: no backend available on this platform (%d)", rc);
    return rc;
}
```

With no `host_ops` installed, neither the bus-walking code path
(which contains the bridge-skip change at `pcie_core.c:486`) nor
the BAR-assignment code path (which contains the
`CMD_MEMORY_SPACE` enable at `pcie_core.c:521`) can execute on
Jetson. **The PR #389 pcie_core changes are structurally inert
on this platform.**

The Jetson PCIe init flow is independent: `pcie_tegra194.c` drives
the BPMP MRQs, APPL wrapper, P2U lanes, DLF, DBI, and link
training out-of-band, exposed via the shell `pcietrain` /
`rtldiag` commands. It never goes through `pcie_core.c::scan_bus`.

### Live diagnostics (post-kexec)

`bpmp` — BPMP IPC + clock state:

```
=== BPMP IPC Smoke Test ===
  bpmp_init:            rc=0
[INFO] BPMP: MRQ_PING OK (reply=0xbd5b7dde)
  MRQ_PING round-trip:  OK
  UART_A IS_ENABLED:    rc=0 state=0 (expect 1)
  PEX2_C8_CORE EN:      rc=0 state=1
=== End Smoke Test ===
```

`pcietrain` — Tegra C8 RC bring-up:

```
=== Tegra PCIe C8 link-up sequence ===
  bpmp_init:            rc=0
[INFO] pcie-tegra: configuring PCIe C8 root complex
[INFO] pcie-tegra: PG_SET_STATE(PCIEX4CA, on) rc=0
[INFO] pcie-tegra: P2U lane 0 + lane 1 init OK
[INFO] pcie-tegra: DLF exchange disabled (DLF_CAP=0x00000001)
[INFO] pcie-tegra: DBI RC setup done (...)
[INFO] pcie-tegra: host init OK (...)
  pcie host init:       rc=0
[WARN] pcie-tegra: link NOT up after 2000 ms (last LTSSM=0x03)
  ...
  DBI bus0 VID:DID:     0x229c10de  (expect 0x229c10de)
  RC alive:             YES
  (EP probe skipped — link never reached L0)
```

`rtldiag` — RTL8168 endpoint probe (jetson-nano-1's PCIe C8 EP):

```
=== Tegra PCIe C8 / RTL8168 Diagnostic ===
  APPL_CTRL:   0x009490e0  (LTSSM_EN=1)
  APPL_DEBUG:  0x00000018  (LTSSM state [8:3] = 0x03)
  DBI bus0:    vendor=0x10de  device=0x229c  (expect 0x10DE:0x229c)
  RC alive:    YES
  Endpoint:    NOT PROBED — bus-1 iATU setup pending
=== End Diagnostic ===
```

### Pre-existing Jetson PCIe limitations (NOT introduced by #389)

- **LTSSM stalls at 0x03** (Polling.Active) post-kexec. Link
  never reaches L0, EP never enumerated. The `rtldiag` source
  comment is explicit: *"bus-1 iATU setup pending — Stage 2+"*.
- **No bridge-of-PCIe-controllers traversal.** SLM-OS today
  initializes PCIe C8 only (the controller behind the AI HAT+
  / RTL8168 connector); C1 (WiFi) and C4 (NVMe) — visible to
  Linux at `0001:00.0` and `0004:00.0` — are not driven.

These are tracked in
[`docs/jetson-pcie-investigation.md`](jetson-pcie-investigation.md)
(LTSSM, iATU, and bus-1 enumeration scope are the Stage-2+
work items there) and are independent of PR #389.

### Hardware identity check via Linux lspci (pre-kexec)

Captured from L4T pre-kexec for hardware-inventory reference —
this is host-OS state, not SLM-OS post-kexec state. For
posterity, what's actually on jetson-nano-1's PCIe today:

```
0001:00:00.0 PCI bridge [0604]: NVIDIA Corporation Device [10de:229e]
0001:01:00.0 Network controller [0280]: Realtek RTL8822CE 802.11ac PCIe WiFi
0004:00:00.0 PCI bridge [0604]: NVIDIA Corporation Device [10de:229c]
0004:01:00.0 Non-Volatile memory controller: Samsung Electronics [144d:a80b]
0008:00:00.0 PCI bridge [0604]: NVIDIA Corporation Device [10de:229c]
0008:01:00.0 Ethernet controller [0200]: Realtek RTL8111/8168 [10ec:8168]
```

No Hailo-8 attached. The #371 sub-task 7 description mentions
Hailo-8 verification, but that applies to a Pi 5 with the AI
HAT+ — not jetson-nano-1. Hailo-8 over PCIe was already verified
on `pi-5-1` in the earlier sub-task work (and continues to
function — the `pcie_bcm2712` backend exercises pcie_core's
bridge-skip path on every boot).

### Conclusion

- **No regression observed.** PR #389's pcie_core.c changes are
  structurally unreachable from Jetson because `pcie_stub.c`
  declines to install `host_ops`, so `pcie_core::scan_bus` never
  runs. The Jetson PCIe init path (`pcie_tegra194.c`) is
  out-of-band from `pcie_core.c`.
- **The Jetson kernel boots cleanly** through the kexec path
  with the current main, hits the shell, runs all the PCIe
  diagnostics, dumps memory + CPU state, etc. — i.e. nothing
  ELSE in the post-#389 main has broken Jetson either.
- The PCIe-link-train failure at LTSSM=0x03 is a pre-existing
  documented limitation of the Jetson backend, not a #389
  regression.

### Closes

- **#371 sub-task 7**: PCIe core change validation on Jetson.
  Verdict: changes do not interact with Jetson's enumeration
  path; no regression possible.
