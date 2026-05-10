# Memory Management — Fact Sheet

Physical memory manager, virtual memory, caches, cross-CPU shared memory.

## Matrix

| Sub-capability | QEMU (ARM64) | Pi 5 | Jetson | x86-64 |
|---|---|---|---|---|
| PMM algorithm | Buddy allocator, orders 0-19 (#608) | same | same | same |
| PMM max block | 2 GB (order 19) | 2 GB | 2 GB | 2 GB |
| Total RAM available | 1 GB (configurable) | 4 GB (model-dep, 8 GB models not tested) | ~6.7 GB across 3 regions (8 GB physical) | 256 MB (QEMU) / board-dep (HW) |
| Usable RAM regions | 1 contiguous | 1 contiguous | 3 (skips OP-TEE 0xBE-0xC2) | 1 contiguous (QEMU) |
| PMM region API | `pmm_init` | `pmm_init` | `pmm_add_region` × 3 | `pmm_init` from MB2 mmap |
| Per-page PMM ownership tracking | `PMM_OWNED` bit | Same | Same | Same |
| VMM granule | 4 KB | 4 KB | 4 KB | 4 KB |
| Page table levels | 3 (L1/L2/L3) | 3 | 3 + 1GB blocks for high RAM | 4 (PML4/PDP/PD/PT) |
| TTBR model (kernel) | `TTBR1_EL1` (high half) | `TTBR1_EL1` (aliased to EL2 via VHE) | `TTBR1_EL1` (aliased to EL2 via VHE) | CR3 |
| TTBR model (per-task user) | `TTBR0_EL1` per task (#728) | Same | Same | Per-task CR3 (planned) |
| Per-task ASID tagging | ✅ TLBI on recycle, no flush on swap (#738) | Same | Same | PCID (planned) |
| User memory mapping (EL0) | ✅ `SYS_MMAP` / `SYS_MUNMAP` (#731) | ✅ | ✅ | ❌ EL0 unwired |
| EL0 ELF loader | ✅ `task_create_user_elf` (#734) | ✅ | ✅ | ❌ |
| Stack size (per task) | 256 KB (`STACK_SIZE`, raised from 64 KB in #643 for SLM forward) | Same | Same | Same |
| Stack overflow detection | Bottom-of-stack canary (64 B) + `schedule()`-time SP-range assert (#644) | Same | Same | Same (RSP-side) |
| Kernel VA = PA? | Identity | Identity | Identity | Higher-half (not yet) |
| Cache coherency (hardware) | ✅ | ❌ SMPEN unset | ❌ SMPEN unset | ✅ MESI |
| Manual cache maintenance | — | DC CVAC/CIVAC + DSB SY | Same + DC CVAC before secondary boot | — |
| Non-cacheable shared region | — | `0xFFE00000`, 2 MB, MAIR idx 2 | `0xBDE00000`, 2 MB, MAIR idx 2 | — (coherent BSS) |
| NC allocator | — | `ncmem_alloc` (bump) | Same | — |
| NC uses | — | runqueues, task table, current-task ptrs, steal deques | Same | — |
| Steal deque lock location | In-struct | External cacheable (`steal_deque_lock[MAX_CPUS]`) | External cacheable | In-struct |
| Spinlock hardware enable | N/A | Runtime flag `spinlock_hw_enabled` set by `vmm_init` | Same pattern | N/A |
| Max tasks supported | 64 | 64 | 64 | 64 |
| Kernel heap | buddy + slab-ish (GSP DMA uses page-aligned alloc) | same | same | same |
| Rust runtime heap | 128 MB (`RUST_HEAP_MB`, default) | 128 MB | **256 MB** (raised in #634 to fit Qwen2.5-1.5B + KV cache) | 128 MB |
| Model_mem weight pool (default) | 256 MB / 128 blocks | **512 MB / 256 blocks** | **1 GB / 512 blocks** (capped by PMM order 19) | **64 MB / 32 blocks** (256 MB QEMU RAM cap) |
| Model_mem workspace pool (default) | 128 MB / 64 blocks | 128 MB | **256 MB** | **32 MB / 16 blocks** |

## Skipped / Blocked

- **Jetson OP-TEE carveout reclamation** — 64 MB at `0xBE000000–0xC2000000` is CBB-protected. Writes trigger RAS error and power off the CPU core. PMM skips the range.
- **Jetson high-memory regions (>4 GB)** — mapped via 1-GB block descriptors L1 indices 4-8 (`0x100000000–0x23FFFFFFF`). Works; no issues noted.
- **Pi 5 SMPEN** — TF-A firmware doesn't set CPUECTLR_EL1.SMPEN on secondaries. Workarounds: NC memory + manual DC CVAC/CIVAC + spinlock DRAM flush. Root fix blocked on firmware change (not feasible).
- **Larger Pi 5 RAM variants (8 GB)** — not regression-tested. PMM should Just Work; unverified.
- **Higher-half kernel** — not implemented on any platform. All kernels use identity-mapped low memory.
- **x86-64 EL0-equivalent (CPL=3) user mode** — syscall path present in tree but never wired into component or task execution. ARM64 has the full per-task TTBR0 + ASID + EL0 path; x86-64 lags.

## See also

- `docs/mmu.md` (narrative)
- `docs/memory-map.md` (per-platform maps)
- `kernel/CLAUDE.md` §"Non-Cacheable Shared Memory"
- `kernel/CLAUDE.md` §"Buddy Allocator (PMM)"

*Last updated: 8 May 2026*
