# Suggestion Findings — Full Codebase Review

39 suggestion findings across 8 subsystems. Generated 2026-04-26 from parallel multi-agent review of `kernel/` and `runtime/` (excluding `kernel/lib/` vendored code and `kernel/tests/`).

---

## kernel/src + kernel/include

### kernel/src/vfs.c
- **lines 519-563** — `vfs_get_path` is fragile (manual reverse-build with off-by-one risk on the `pos < 1` check; `pos--` then `temp[pos] = '/'` writes at `pos` which can reach 0). The function is only used for debug output; consider using `uart_snprintf` and rebuilding forward.

### kernel/include/spinlock.h
- **lines 277-280** — `spin_is_locked` performs a non-atomic, non-`READ_ONCE`-style read on a `volatile` field. Comment correctly says "racy, debugging only" — but since the field is `volatile uint32_t`, the read is fine; the comment is still warranted because of TOCTOU.

---

## kernel/arch (arm64 + x86_64)

### kernel/arch/arm64/boot.S
- **lines 386-390** — `mov w14, #0xFFFFFFFF` then `str w14, [x12, x13, lsl #2]` would be faster as a `mvn w14, wzr` once before the loop and a single `str` inside; minor.
- **lines 471-486** — MIP0 mask register layout uses bare hex offsets. Naming a few of them (`MIP_MASKL_HOST`, etc.) would catch future register-offset typos at compile time.

### kernel/arch/arm64/context.S
- **lines 180-182** — Comment correctly explains that x3 is scratch, but a stray reader might think the post-DAIF-restore window is fully safe. Worth noting that any IRQ taken between `msr daif` and `ret` will use the *new* task's stack, which is exactly what we want.

### kernel/arch/arm64/mmu.S
- **line 158** — `mov x1, #0x1005` for "M | C | I" works by coincidence (bits 0|2|12). Spell it as `mov x1, #(1<<0); orr x1, x1, #(1<<2); orr x1, x1, #(1<<12)` or `MOV w1, #4101`.

### kernel/arch/arm64/efi_stub.c
- **lines 215-222** — Inline asm for cache clean-by-set/way clobbers x1, x2, x4, x5, x6, x7, x9, x10, x11. Should also list x3 (the LoC) — though it happens to remain consistent with C-side use, a future caller passing x3 in would be surprised.

### kernel/arch/x86_64/idt.S
- **lines 23-65** — `isr_common` doesn't preserve `xmm`/`ymm` state. On x86-64 SysV ABI, xmm0-15 are caller-saved, so the C exception handler is allowed to use them. If `exception_handler` ever uses SSE (or compiler vectorizes a memcpy/memset inside), the interrupted task's xmm state is corrupted. For LAPIC timer + reschedule IPI on IST1 this is fine since they call `schedule()` → context-switch path which `fxsave`s. For other IRQs that resume the interrupted task without a switch, this is a real corruption window. Either save xmm0-15 in `isr_common` or compile the kernel with `-mgeneral-regs-only`.

### kernel/arch/x86_64/lapic.c
- **lines 268-280** — Comment correctly explains the EOI fence; verify the `lock; addl $0, (%rsp)` doesn't cross a stack-page boundary that's been popped — it shouldn't because we're inside the IRQ handler with valid stack, but worth a brief note.

### kernel/arch/x86_64/trampoline32.S
- **lines 251-253** — `gdt64_ptr` is in `.rodata.gdt`. The 4-byte `.long gdt64` (32-bit base) limits future relocation above 4 GB. CLAUDE.md / docs note this is intentional; it's fine but should be paired with a `_Static_assert` that `gdt64 < 0x100000000`.

---

## kernel/sched

### kernel/sched/sched.c
- **lines 1688-1896** — `schedule()` is 200+ lines, well past the 80-line guideline. Pull out the zombie cleanup (1741-1753), the current-task-rewind path (1759-1783), and the next-task-removal (1786-1793) into helpers with clear pre/post-condition comments.

### kernel/sched/ai/ai_state.c
- **line 188** — `total_ready += sched_cpu_rq(c)->ready_count` is a racy unsynchronized read across all CPUs. Acceptable for this metric (consumed by AI), but document it.

---

## kernel/mm + kernel/ipc + kernel/fs + kernel/net + kernel/inference + kernel/usb

### kernel/mm/pmm.c
- **lines 450-452** — File-scope `pmm_split_*` scratch arrays are documented as safe because `pmm_init` is single-threaded, but no `_Static_assert` or runtime guard enforces single entry. Add `static bool in_split = false; ASSERT(!in_split); in_split = true; ... in_split = false;` for cheap defense.

### kernel/mm/vmm.c
- **lines 1109-1281** — `vmm_setup_platform` for Pi 5 is ~170 lines with many magic-numbered MMIO bases. Extract the platform table into `kernel/include/platform_mmio.h` so adding a new device doesn't require editing this function.

### kernel/ipc/ipc.c
- **lines 619-668** — `shared_buffer_create` is 75 lines with a GPU-vs-PMM allocator branch. Extract `alloc_backing(size, flags, gpu_backed_out)` helper.

### kernel/fs/littlefs_slm.c
- **lines 503-510** — The `VALIDATE_FILE` macro hides early returns; consider an inline function instead so failure paths are easier to step through under GDB.

### kernel/net/sys_arch.c
- **lines 24-34** — `sys_now` divides by `(freq / 1000)` which loses precision when freq isn't a multiple of 1000. On QEMU `cntfrq` is often 62.5 MHz; result is fine, but on x86-64 RDTSC frequencies vary. Add a comment noting precision bound, or use 64-bit `(count * 1000) / freq` with a freq-bound check (`count < UINT64_MAX/1000`).

### kernel/net/lwip_slm.c
- **lines 549-639** — `net_init` is 90 lines spanning driver init + lwIP init + DHCP setup. Extract `setup_netif()`, `setup_dhcp_at_boot()` helpers.

### kernel/inference/inference_device.c
- **lines 53-63** — Hand-rolled string compare instead of using a kernel `strcmp`. If `kernel/lib/string.c` exists, prefer that for consistency.

### kernel/inference/inference_device_hailo.c
- **lines 694-1480** — `context_switch_load` is ~800 lines. Extract per-step helpers (`alloc_ccw_buffers`, `alloc_boundary_buffers`, `program_descriptor_lists`, `ship_context_sequence`).

### kernel/usb/core/usb_core.c
- **lines 833-1137** — `usb_enumerate_one` is 305 lines with deeply nested platform-specific child paths. Split into `enumerate_get_device_descriptor`, `enumerate_set_address`, `enumerate_set_configuration` helpers.

### kernel/usb/class/cdc_ecm.c
- **lines 220-266** — `cdc_ecm_dma_init` allocates from NC memory but never frees — relying on bump allocator. Document explicitly that probe failure leaks NC pages (acceptable for boot-once design).

---

## kernel/drivers

### kernel/drivers/bpmp/ivc.c
- **lines 224-228** — Byte-wise volatile copy is correct but slow. The hot path is short (~20 B), acceptable; comment already addresses tradeoff.

### kernel/drivers/pcie/pcie_core.c
- **lines 421-548** — `pcie_init` is 127 lines; consider extracting the BAR allocation pass.

### kernel/drivers/pcie/pcie_tegra194.c
- **line 249** — `pcie_tegra_host_init` is 285 lines.

### kernel/drivers/pcie/pcie_bcm2712.c
- File is 1300 lines; `bcm2712_train_link` is well factored into helpers. Several "magic numbers" remain (e.g. `0x0B2D0000`, `0x0ABA0000` UBUS timeouts at lines 754-755) — names would help.

---

## kernel/ai_accel/hailo + kernel/gpu/nvidia

### kernel/ai_accel/hailo/hailo_control.c
- **lines 447-449** — Two ~1.5 KB BSS scratch buffers (`control_req_wire`, `control_resp_wire`) for the transport, plus six more wire-format static buffers for individual opcodes (read/write_memory, config_stream, set_ngh, set_ctx_info, change_status, hw_consts, etc.). Total BSS footprint is ~10 KB; fine but worth a comment summarizing the budget.

### kernel/ai_accel/hailo/hailo_cs_translator.c
- **lines 1542-1604** — `hailo_cs_translate_contexts` is fine in length, but the per-stage stage-tracker noise (`TRANSLATE_STAGE(410)` ... `(417)` plus the embedded `500000+`, `600000+`, `700000+` scheme on the dynamic stage) is investigation-era diagnostic that should be guarded behind `HAILO_WIRE_DEBUG` like the other probe code, or at least named via macro constants instead of magic numbers.

### kernel/ai_accel/hailo/hailo_vdma.c
- **lines 561-628** — `hailo_vdma_submit_and_wait` is ~140 lines including the `HAILO_WIRE_DEBUG` block. Extract the diagnostic loop into a helper.

### kernel/gpu/nvidia/falcon.c
- **lines 60-89** — `falcon_probe` rejects `cpuctl == 0xFFFFFFFFu` but does not check for `0xbadfXXXX` PRI poison (line 122 in `falcon_wait_halted` does check). Add the same check to `probe`.

---

## runtime/src/mm

### runtime/src/mm/model_mem.rs
- **lines 559-606** — `model_mem_init` is 47 lines but mixes PMM allocation, lock acquisition, pool init, tracker init, and feature gates. Extract the per-pool init into a helper.
- **line 78** — `is_null` checks both `block_index == 0xFFFF && pool_id == 0xFF`. A handle with one of those values but not both is treated as live — consider documenting whether that is intentional.

### runtime/src/mm/eviction/store.rs
- Add a public `set_eviction_policy_for_pool` test that exercises the rollback chain (stage → activate → activate again → rollback → rollback should produce `NoRollbackBlob`).

### runtime/src/mm/eviction/registry.rs
- Counters use `Ordering::Relaxed` (correctly observational) but `LATENCY_TOTAL_NS / LATENCY_SAMPLES` are not snapshot-atomic — `policy_counters()` can read a `total_ns` updated after the matching `samples`. For a per-policy averaged latency this is acceptable; document.

### runtime/src/mm/eviction/cacheus.rs
- **lines 219-263** — `rebuild_for_runtime_config` clones the expert list each rebuild; on a config-blob hot-reload this happens on the alloc hot-path. Pre-build the alternative pools and swap.

### runtime/src/mm/model_loader.rs
- **lines 277-290** — Only one test; `load_from_buffer` and `Drop` are uncovered. Add a test using a fake allocator harness or a `cfg(test)` mock.

---

## runtime/src (sched, component, loader, inference, top-level)

### runtime/src/lib.rs
- 6362 lines. Most of the AI-eviction FFI plumbing belongs in `mm/eviction/ffi.rs` or similar. As-is the file is the largest in the runtime by 5×.

### runtime/src/kernel_ffi.rs
- **lines 661-681** — The static asserts validate sizes but not field offsets. For `GpuInfoFfi`, add `assert!(offset_of!(GpuInfoFfi, capabilities) == ...)` checks (via `core::mem::offset_of!`) so a future struct edit can't drift from the C layout silently.

### runtime/src/loader/protobuf.rs
- **lines 78, 86** — Magic numbers `64` and `9` for varint width. Define `MAX_VARINT_BYTES: usize = 10` and `MAX_VARINT_BITS: u32 = 64`.

### runtime/src/sched/heterogeneous.rs
- **lines 540-545** — `rust_select_inference_core` returns `0xFF` for "no recommendation" but `u8` is also a valid core ID. Use `i16` or wrap in a struct.
