# C3: boot-sequence coherency audit (#1193)

Builder C3, QEMU-and-code stage. No hardware was touched. The reference is
C0 section (b), "What a correct secondary bring-up sequence must contain"
(`git show be435c5b:docs/archive/coherency-gauntlet/c0-facts.md`), plus C0
Q5 (caches-off accesses), Q6 (PSCI entry state) and Q7 (set/way). Line
numbers are for the tree at `c3-base-1193` (9a13b9ee) unless a later commit
is named.

Terms: "caches-off" means SCTLR_ELx.C=0 (with M=0, data accesses are
Device-nGnRnE because HCR_EL2.DC=0 on every path, see below). "Clean" is DC
CVAC, "invalidate" is DC IVAC, "clean+invalidate" is DC CIVAC, all by VA to
the PoC unless set/way is named. On Cortex-A76 and Cortex-A78AE, DC IVAC is
executed as DC CIVAC and DC ISW as clean+invalidate (A76 TRM §7.2.4, A78AE
TRM §2.6.2.4, C0 Q7).

## 1. Sequences per CPU and boot path

### 1.1 Pi 5 primary (firmware load at 0x80000, BL31 enters BL33 at EL2)

| Step | Where | What |
|---|---|---|
| 1 | firmware | VPU loads `kernel_2712.img`; BL31 enters at EL2 with SCTLR_EL2.{M,C,I}=0. A76 caches were invalidated at cold reset (A76 TRM §7.2). |
| 2 | `boot.S:420-455` | (PI5_IRQ_DIAG only: caches-off stores into the NC block at 0xFFE0FF00, `boot.S:435-443`, `:689-696`.) CurrentEL check. |
| 3 | `boot.S:472-636` | HSTR_EL2=0, CNTHCTL, CNTVOFF, GIC-400 and PCIe/MIP MMIO programming (Device). |
| 4 | `boot.S:664-666` | HCR_EL2 := E2H \| RW \| TGE (overwrite; DC=0, VM=0). |
| 5 | `boot.S:705-727` | CNTHCTL, CPACR, SCTLR_EL2 := 0x30d00800 (EL1 layout under E2H: M=C=I=0, A=0, SA=0, WXN=0). |
| 6 | `boot.S:917-970` | DAIF masked, SP = `__stack_top`, CPACR, BSS cleared with caches off (`:950-960`), VBAR. |
| 7 | `main.c:225-352` | `kernel_main` runs C with caches off: UART, DTB parse (globals written caches off), prints. |
| 8 | `vmm.c:2025-2033` | Page tables built with caches off (`vmm_setup_platform`). |
| 9 | `vmm.c:2034-2064` | DC CVAC over `l1_table` only on Pi 5, DSB SY. |
| 10 | `mmu.S:44-147` | TLBI VMALLE1, DSB NSH, IC IALLU, MAIR, TCR (walks IRGN/ORGN WB-WA, SH Inner, `vmm.h:202-213`), TTBR0=TTBR1, TLBI, SCTLR \|= M\|C\|I, ISB, TLBI, then `daifclr #0xf`. |
| 11 | `vmm.c:2097-2100` | `secondary_mmu_{ttbr,mair,tcr}` written with caches on, then DMB ISH only. |
| 12 | `vmm.c:2106-2108` | `spinlock_hw_enabled = 1`. |

Caches-off accesses on the primary and how the other side maintains them:
every caches-off store (BSS, early stack frames, DTB globals, page tables)
goes straight to DRAM and is later read through the WB Inner Shareable
mapping. Nothing invalidates those ranges by VA after the MMU comes on. This
is safe only while no cache holds a line for those addresses when the MMU is
enabled. On Pi 5 that holds because CPU 0's caches were invalidated at reset
and the firmware never runs through them in NS memory (D6).

### 1.2 Pi 5 secondaries (PSCI CPU_ON releases the TF-A holding pen)

| Step | Where | What |
|---|---|---|
| 1 | TF-A | Entry at EL2, SCTLR.{M,C,I}=0, caches clean and coherent from the caller's view (PSCI §6.4.3.4). |
| 2 | `smp_boot.S:21-104` | DAIF masked, HSTR, CNTHCTL, CNTVOFF, HCR_EL2 := E2H\|RW\|TGE, CNTHCTL, CPACR, SCTLR := 0x30d00800. |
| 3 | `smp_boot.S:161-178` | Caches-off loads of `secondary_mmu_ttbr`, `_mair`, `_tcr`, each preceded by DC IVAC + DSB SY. Literal-pool loads from `.text`. |
| 4 | `smp_boot.S:181-199` | MAIR, TCR, TTBR0=TTBR1, ISB, TLBI VMALLE1, DSB SY, ISB, SCTLR \|= M\|C\|I, ISB. |
| 5 | `smp_boot.S:201-265` | DC ISW over every set and way of L1D and L2 (not L3), DSB SY, ISB. |
| 6 | `smp_boot.S:278-302` | SP = `cpu_stacks[id+1]`, CPACR, VBAR, `secondary_init`. |

The secondary makes no caches-off store. Its only caches-off loads are the
three MMU words (written by CPU 0 with caches on) and the `.text` literal
pool (never written since load).

### 1.3 Jetson primary, kexec path (Linux VHE at EL2, `slmos-kexec`)

| Step | Where | What |
|---|---|---|
| 1 | Linux 5.15 | `cpu_soft_restart` writes INIT_SCTLR_EL1_MMU_OFF (C=M=I=0) at EL2 with E2H=1 and jumps to 0x80000000. CPU 0 is not reset; other CPUs went through PSCI CPU_OFF. Linux cleaned+invalidated the kexec segments. |
| 2 | `boot.S:223` | x1 == 0, not EFI. |
| 3 | `boot.S:813-821` | HCR_EL2 \|= E2H\|RW\|TGE (read-modify-write: every other bit Linux left stays). |
| 4 | `boot.S:832-848` | Timers off, "EL2" on UARTC. |
| 5 | `boot.S:865-869` | SCTLR \|= SPAN, PSTATE.PAN=0. No other SCTLR bit is written (D3). |
| 6 | as 1.1 steps 6-12 | Jetson `vmm.c` cleans `l1_table`, `l2_kernel`, `l2_ram_c0` by VA (not `l2_mmio`, `l2_pcie_bar`, `l2_sysram`). `pmm_init` runs after the MMU is on (`main.c:358-406`, #608). |

CPU 0's caches still hold whatever Linux left (C0 Q3). The kexec segment
range was cleaned+invalidated by Linux, so the image, BSS and boot stack
start clean; any line re-fetched speculatively by Linux after that flush and
before SCTLR.C=0 is a stale clean line that SLM-OS would hit after its MMU
comes on (D6). The NC block is not part of the segments (D5).

### 1.4 Jetson primary, UEFI-direct path

| Step | Where | What |
|---|---|---|
| 1 | UEFI | Entry at EL2, HCR_EL2.E2H=0 (HCR_EL2=0x88000000), MMU and caches on. |
| 2 | `efi_stub.c:625-666` | ExitBootServices, then DC CIVAC by VA of the loaded image, the link-address destination and the DTB (`efi_dcache_clean_inval`, `:350-361`). Model files were cleaned when read. |
| 3 | `boot.S:286-326` | DAIF.{A,I,F} masked, SCTLR_EL2 &= ~(M\|C\|I), TLBI ALLE2, IC IALLU. No stack access from here. |
| 4 | `boot.S:347-363` | Relocation copy with caches off (Device stores to the destination), IC IALLU. |
| 5 | `boot.S:813-821` | HCR_EL2 \|= E2H\|RW\|TGE with the MMU off; SCTLR_EL2 from UEFI is now read in the E2H layout. |
| 6 | as 1.3 steps 4-6 | No set/way anywhere on this path. |

The copy destination was cleaned+invalidated before the MMU went off, so the
relocated image is coherent unless a prefetch re-allocated a destination
line in the short window between `efi_dcache_clean_inval` and SCTLR.C=0
(D6). The NC block is not cleaned (D5).

### 1.5 Jetson secondaries (both paths; PSCI CPU_ON goes to the MCE, core reset)

| Step | Where | What |
|---|---|---|
| 1 | NVIDIA TF-A | Entry at EL2 after a core reset (C0 Q3, inference), SCTLR.{M,C,I}=0. |
| 2 | `smp_boot.S:40-53` | HCR_EL2 := E2H\|RW\|TGE (overwrite), timers off. SCTLR is not written; TF-A's SCTLR_EL2 is reinterpreted in the E2H layout (D3). |
| 3 | `smp_boot.S:136-302` | Same as 1.2 steps 3-6, including the set/way loop. |

### 1.6 HCR_EL2.DC and SCTLR before MMU enable

| CPU / path | HCR_EL2.DC | SCTLR.{M,C,I} before enable | Other SCTLR bits |
|---|---|---|---|
| Pi 5 primary | 0 (overwrite) | 0,0,0 | 0x30d00800 written |
| Pi 5 secondary | 0 (overwrite) | 0,0,0 | 0x30d00800 written |
| Jetson primary, kexec | as left by Linux (0 for the VHE host flags), RMW | 0,0,0 | Linux INIT_SCTLR_EL1_MMU_OFF + SPAN |
| Jetson primary, UEFI | as left by UEFI (0), RMW | 0,0,0 (cleared at `boot.S:306-311`) | UEFI's SCTLR_EL2, read in the E2H layout, + SPAN |
| Jetson secondary | 0 (overwrite) | 0,0,0 | TF-A reset value, read in the E2H layout |

## 2. The set/way loop (`smp_boot.S:201-265`)

**What it does.** After setting SCTLR.{M,C,I} on a secondary, it selects
CSSELR=0 (L1D) and CSSELR=2 (L2), decodes CCSIDR_EL1 (non-CCIDX format,
correct for A76 and A78AE, which do not implement FEAT_CCIDX), and issues DC
ISW for every set and way, then DSB SY and ISB. The decode is correct: set
shift = LineSize+4, way shift = clz(assoc-1) on 32 bits, level field 2 for
L2, both loops include index 0. L3 (DSU, shared) is not touched.

**Is it needed?** No. Its comment says the three caches-off loads "entered
L1/L2 as Non-Shareable lines". With SCTLR.C=0, loads and stores do not
access the L1, L2 or L3 arrays at all (A76 TRM §7.2.3, A78AE TRM §2.6.2.3),
so nothing was allocated. The caches were invalidated at the core's last
reset, and PSCI guarantees them clean and coherent at entry (C0 Q6). Linux
does no set/way on either board (C0 Q6). The primary, which runs far more
code caches-off (all of `kernel_main` up to `vmm_init`), has no equivalent
and works; if the loop's rationale were right, the primary would need it
more than any secondary.

**Is it safe?** On these cores, yes for data integrity: DC ISW is executed
as a local clean+invalidate, the loop makes no data access between the
SCTLR write and the loop, and the core holds no dirty line of its own. It is
not portable (architecturally DC ISW is an invalidate that may discard dirty
data) and it gives no cross-CPU guarantee (Arm ARM D7.5.9.12, D7.5.9.16.1).
It cannot have been the mechanism behind a cross-CPU visibility fix.

**Primary equivalent.** None, and none is wanted: the builder rules forbid
set/way on the Jetson UEFI-direct MMU-off path, and by the argument above
the primary does not need it. The secondary loop runs with the MMU on, so it
is outside that rule, but under Arm's "strongly discourages ... on coherent
systems" guidance.

**Stage decision.** The loop is kept in this stage. `SETWAY_INVAL=OFF`
(Part 2) removes it at build time so its necessity can be measured on its
own.

## 3. Locations accessed with two different attributes

| Location | Attributes seen | Maintenance | Verdict |
|---|---|---|---|
| `secondary_mmu_{ttbr,mair,tcr}` | WB IS store on CPU 0; Device load on the secondary | Reader DC IVAC only; the writer does not clean | D1 |
| BSS, boot stack, DTB globals, static page tables | Device store on CPU 0 before the MMU; WB IS loads afterwards (CPU 0 and walkers) | Page tables: DC CVAC (a no-op for caches-off data); the rest: none | D2, D6 |
| UEFI relocation destination | WB in UEFI, Device store by the copy, WB IS afterwards | CIVAC before the MMU goes off | correct; D6 window |
| NC block (Pi 5 0xFFE00000, Jetson 0xBDE00000) | Device (caches-off diagnostics), Normal NC (all SLM-OS users), Normal WB in the previous owner on Jetson (Linux linear map or UEFI) | None on the block | D5 (Jetson only) |
| NC block vs PMM | Reserved from PMM (`pmm.c:646`, `:718`), never handed out as WB | n/a | correct |
| NC block placement on Pi 5 | `vmm.c:1851-1880` splits the last GB of `RAM_SIZE`; `NC_MEM_BASE` is fixed at 0xFFE00000 | n/a | D11 (latent) |
| MAIR / TCR | Same constants on every CPU (`MAIR_EL1_VALUE`, `TCR_EL1_VALUE`), published through `secondary_mmu_*` | n/a | no index mismatch |
| Shareability | Normal WB and NC always Inner Shareable (`vmm.c:141-165`, `:217-245`); the only Non-shareable mapping is C2's temporary negative-control alias (`VMM_FLAG_NONSHARE`) | n/a | deliberate |
| Per-task user L1 | Copies the kernel L1 entries (`vmm.c:680`) | n/a | same attributes |

## 4. Defect list

| Id | Severity | Location | Defect | Action |
|---|---|---|---|---|
| D1 | Medium | `vmm.c:2097-2100`, `smp.c:775-789`, `smp_boot.S:148-178` | `secondary_mmu_{ttbr,mair,tcr}` are written with caches on and never cleaned to the PoC before CPU_ON (C0 (b) step 1). The secondary reads them caches-off after DC IVAC, which works only because A76/A78AE execute DC IVAC as clean+invalidate, which cleans CPU 0's dirty line. Architecturally an invalidate may discard it; the secondary would then read 0 and take `cbz x0, .Lsec_skip_mmu` (`smp_boot.S:165`), running C with the MMU off. | Fixed: `smp_init` cleans the three words before the CPU_ON loop. |
| D2 | Low | `vmm.c:2034-2064` | Page tables written caches-off are "cleaned" (DC CVAC). The data is already in DRAM, so the clean is a no-op; a stale clean line would survive and be hit by the cacheable walker, and only some tables are covered. The comment's reason (secondary walkers read DRAM, not CPU 0's cache) is wrong: walks are WB Inner Shareable and coherent. Linux invalidates its caches-off-written tables (`head.S:96-105`). | Fixed: DC IVAC over every static table before `mmu_enable`. |
| D3 | Low | `boot.S:865-869`, `smp_boot.S:40-53`, `:194-198` | Jetson never writes a known SCTLR baseline: the primary inherits Linux's or UEFI's SCTLR (the UEFI value is reinterpreted after the E2H flip) and secondaries inherit TF-A's. Pi 5 writes 0x30d00800 on every CPU. | Filed (needs Jetson hardware). |
| D4 | Low | `boot.S:817-821` | Jetson primary HCR_EL2 is read-modify-write, so DC, VM, TVM or trap bits left by the previous owner persist. HCR_EL2.DC=1 would make every caches-off access cacheable and void the assumptions above. Neither Linux's VHE flags nor UEFI set it today. | Filed with D3. |
| D5 | Medium (latent, NC axis) | `ncmem.c:21-40`, `efi_stub.c:659-666`, `vmm.c:1538` | On Jetson, CPU 0 is not reset across kexec or the UEFI hand-off, and its caches may hold dirty WB lines for the NC block from the previous owner. SLM-OS maps the block Normal NC and never cleans it by VA. An eviction of such a line later writes stale data over NC data (run queues, task table, DMA rings): Arm ARM B2.11 "values reverted". | Filed (needs hardware). |
| D6 | Low | `boot.S:950-960`, `boot.S:347-363`, `main.c:225-352` | Caches-off stores by the primary (BSS, boot stack, DTB globals, UEFI relocation) are never invalidated by VA before they are read through the cacheable mapping. Safe only while no cache holds those lines: true on Pi 5 (cold reset); on Jetson it rests on Linux's segment flush and the EFI stub's CIVAC, each with a speculative re-fetch window. #608 was this class of bug for PMM free lists. | Filed. |
| D7 | Info | `smp_boot.S:201-265` | Set/way loop: unnecessary, rationale wrong, safe on these cores, not portable (section 2). | Kept; `SETWAY_INVAL=OFF` added. |
| D8 | Low | `smp_boot.S:165` | If `secondary_mmu_ttbr` reads 0 the secondary continues into C with the MMU and caches off, instead of parking. Silent incoherent execution. | Documented (reachable only through D1, now fixed). |
| D9 | Low | `cache.h:28-43`, `smp.c:402-407`, `smp.c:745-750`, `ncmem.h:4-6` | "SMPEN" check reads CPUECTLR_EL1 bit 6, RES0 on A76 and PF_MODE[0] on A78AE (C0 Q1). The log lines ("SMPEN NOT set, using DC CVAC/CIVAC workaround") and the `ncmem.h` claim "incoherent despite SMPEN being set" (H4) are contradicted by the TRMs. | Fixed: messages and comments say what the bit is. |
| D10 | Low (unrelated) | `mmu.S:147` | `mmu_enable` ends with `msr daifclr, #0xf`, unmasking IRQ, FIQ, SError and Debug on the primary before `gic_init`, instead of restoring the caller's DAIF. | Filed. |
| D11 | Low (latent) | `vmm.c:1851-1880`, `ncmem.h:30` | Pi 5 NC block is mapped NC only because `RAM_SIZE` is 4 GB: the code splits the last GB of RAM, while `NC_MEM_BASE` is a constant. An 8 GB `RAM_SIZE` would map 0xFFE00000 WB while every user treats it as NC. | Fixed: compile-time assert. |
| D12 | Medium (for NC_AXIS=OFF) | `sched.c:609`, `sched.c:2617`, `task.c:155`, `preempt.c:57-59`, `cdc_ecm.c:82-123`, `usb_core.c:54-80`, `shell_sys.c:1813-1990`, `shell_sys.c:2217-2253`, `diag_pi5.h:21` | `PLATFORM_HAS_NC_MEMORY` was also used as "this is Pi 5 or Jetson" (idle-loop WFI/WFE choice, IRQ unmask rules), as "this platform has the DMA-safe NC region" (USB/CDC-ECM buffers) and to gate shell benches. Their `#else` branches are QEMU-only shortcuts. `preempt.c` allocates its per-CPU state from NC with no gate at all. | Fixed in Part 2 (see section 5). |

Also observed, not a defect: `smp.c:555-565` counts a secondary whose boot
flag never becomes visible as online after a WARN ("boot flag not visible
(cache incoherency) - assuming online"). On coherent hardware this WARN
must never print; the hardware stage should treat it as a failure signal in
NC_AXIS=OFF runs.

## 5. Part 2 summary: NC users and their cacheable branches

`PLATFORM_HAS_NC_MEMORY` now means only "CPU-to-CPU shared state lives in
the NC block". The block itself, `ncmem_alloc` and the fixed-address
diagnostic slots are `PLATFORM_HAS_NC_REGION`, which stays defined on Pi 5
and Jetson under `NC_AXIS=OFF` because device DMA (xHCI, MACB, CDC-ECM, USB
descriptor bounce, camera RCE) and the Jetson SMMU identity map depend on
it. Board behavior (WFI or WFE in idle, which CPUs unmask IRQs) is keyed on
the platform macros directly.

| File | Use | `#else` branch on ARM64 hardware |
|---|---|---|
| `kernel/sched/sched.c` | run queues, steal deques, `sched_diag_*`, init flag, NC traces | correct (BSS + `cache.h` maintenance); idle loop and `scheduler_start` unmask were QEMU-only, fixed |
| `kernel/sched/task.c` | task table, `current_task`, task lock CIVAC, debug print gate | correct; `task_entry_unmasks_irq` was QEMU-only, fixed |
| `kernel/sched/smp.c` | NC logical map, CurrentEL slots, timeout marker | correct |
| `kernel/sched/preempt.c` (not gated before) | `reschedule_pending`, `orig_elr`, `orig_spsr` | had none (NC only); BSS branch added |
| `kernel/sched/steal_deque.c`, `kernel/include/steal_deque.h` | comments | n/a |
| `kernel/include/ncmem.h` | definition | split into REGION and MEMORY |
| `kernel/include/nc_trace.h` | debug traces | correct (no-op) |
| `kernel/include/diag_pi5.h` | PI5_IRQ_DIAG layout | `#error` now keys on the region |
| `kernel/include/platform.h` | comment | updated |
| `kernel/src/kprintf.c` | UART lock | correct: real IRQ-saving spinlock, so secondaries may print |
| `kernel/src/main.c` | NC init flag | correct (flag not used) |
| `kernel/src/shell_sys.c` | `cpu`, `bench smp`, `bench deadline`, `bench isolate`, S3, thermal | `bench smp`/`deadline`/`isolate` printed "NC memory required"; cacheable slots added |
| `kernel/src/admin_telemetry.c`, `kernel/src/lua_slm.c` | `sched_diag_*` externs | correct (array form) |
| `kernel/arch/arm64/exceptions.c` | NC traces | correct (no-op) |
| `kernel/ipc/pi_mutex.c` | `cache_clean` on task fields | correct |
| `kernel/usb/core/usb_core.c`, `kernel/usb/class/cdc_ecm.c` | DMA buffers | cacheable `#else` is not DMA-safe on Pi 5/Jetson; now keyed on the region |
| `kernel/tests/test_pmm.c` | ncmem allocator tests | now keyed on the region |
| `kernel/tests/test_integration.c`, `kernel/tests/test_scheduler.c` | cross-CPU sync slots | correct (BSS + maintenance) |
| `runtime/src/msg_router.rs` | lock-order comment | updated; the Rust runtime is one library for every ARM64 platform and has no platform cfg |
| `CMakeLists.txt`, `kernel/CLAUDE.md` | comments | updated |

Under `NC_AXIS=OFF` the `#else` branches still call the `cache.h` by-VA
helpers (DC CVAC/CIVAC + DSB SY) on Pi 5 and Jetson, because the same
helpers serve device DMA. On coherent hardware they are redundant but
correct. A build that removes CPU-to-CPU maintenance as well would need
separate helpers for CPU sharing and for DMA; that is not part of this
stage.
