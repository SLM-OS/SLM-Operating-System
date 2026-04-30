# Memory Management — Fact Sheet

Physical memory manager, virtual memory, caches, cross-CPU shared memory.

## Matrix

| Sub-capability | QEMU (ARM64) | Pi 5 | Jetson | x86-64 |
|---|---|---|---|---|
| PMM algorithm | Buddy allocator, orders 0-18 | same | same | same |
| PMM max block | 1 GB (order 18) | 1 GB | 1 GB | 1 GB |
| Total RAM available | 1 GB (configurable) | 4 GB (model-dep, 8 GB models not tested) | ~6.7 GB across 3 regions (8 GB physical) | 256 MB (QEMU) / board-dep (HW) |
| Usable RAM regions | 1 contiguous | 1 contiguous | 3 (skips OP-TEE 0xBE-0xC2) | 1 contiguous (QEMU) |
| PMM region API | `pmm_init` | `pmm_init` | `pmm_add_region` × 3 | `pmm_init` from MB2 mmap |
| VMM granule | 4 KB | 4 KB | 4 KB | 4 KB |
| Page table levels | 3 (L1/L2/L3) | 3 | 3 + 1GB blocks for high RAM | 4 (PML4/PDP/PD/PT) |
| TTBR model | `TTBR0_EL1` | `TTBR0_EL1` | `TTBR0_EL1` (aliased to EL2 via VHE) | CR3 |
| Kernel VA = PA? | Identity | Identity | Identity | Higher-half (not yet) |
| Cache coherency (hardware) | ✅ | ❌ SMPEN unset | ❌ SMPEN unset | ✅ MESI |
| Manual cache maintenance | — | DC CVAC/CIVAC + DSB SY | Same + DC CVAC before secondary boot | — |
| Non-cacheable shared region | — | `0xFFE00000`, 2 MB, MAIR idx 2 | `0xBDE00000`, 2 MB, MAIR idx 2 | — (coherent BSS) |
| NC allocator | — | `ncmem_alloc` (bump) | Same | — |
| NC uses | — | runqueues, task table, current-task ptrs, steal deques | Same | — |
| Steal deque lock location | In-struct | External cacheable (`steal_deque_lock[MAX_CPUS]`) | External cacheable | In-struct |
| Spinlock hardware enable | N/A | Runtime flag `spinlock_hw_enabled` set by `vmm_init` | Same pattern | N/A |
| Max tasks supported | 256 | 256 | 256 | 256 |
| Kernel heap | buddy + slab-ish (GSP DMA uses page-aligned alloc) | same | same | same |
| Model_mem weight pool (default) | 256 MB / 128 blocks | 256 MB | 256 MB | **64 MB / 32 blocks** (256 MB QEMU RAM cap) |
| Model_mem workspace pool (default) | 128 MB / 64 blocks | 128 MB | 128 MB | **32 MB / 16 blocks** |

## Skipped / Blocked

- **Jetson OP-TEE carveout reclamation** — 64 MB at `0xBE000000–0xC2000000` is CBB-protected. Writes trigger RAS error and power off the CPU core. PMM skips the range.
- **Jetson high-memory regions (>4 GB)** — mapped via 1-GB block descriptors L1 indices 4-8 (`0x100000000–0x23FFFFFFF`). Works; no issues noted.
- **Pi 5 SMPEN** — TF-A firmware doesn't set CPUECTLR_EL1.SMPEN on secondaries. Workarounds: NC memory + manual DC CVAC/CIVAC + spinlock DRAM flush. Root fix blocked on firmware change (not feasible).
- **Larger Pi 5 RAM variants (8 GB)** — not regression-tested. PMM should Just Work; unverified.
- **Higher-half kernel** — not implemented on any platform. All kernels use identity-mapped low memory.
- **User-mode page tables (TTBR0/TTBR1 split)** — not implemented. EL0 components use `kernel/src/component_runtime.c` which stays in EL1 mapping today (#component-isolation.md).

## See also

- `docs/mmu.md` (narrative)
- `docs/memory-map.md` (per-platform maps)
- `kernel/CLAUDE.md` §"Non-Cacheable Shared Memory"
- `kernel/CLAUDE.md` §"Buddy Allocator (PMM)"

*Last updated: 18 April 2026*
