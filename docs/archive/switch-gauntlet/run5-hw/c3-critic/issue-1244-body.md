## Summary

On a Jetson kexec boot the DTB that `kexec --dtb` passes sits one page after the end of the kernel's last PT_LOAD segment, and `__kernel_end` (the PMM `heap_start`) is only 0x5000 to 0x7000 bytes above it (`. = ALIGN(0x10000)` in `kernel/kernel-jetson.ld`). The blob is a copy of `/sys/firmware/fdt` (the UEFI-direct stub reports `fdt_size = 0xF39DC` on the same board), so everything past its first 20 to 28 KB lies inside the buddy heap. Nothing reserves it: `pmm_add_region_split` carves only the DTB's own `/memreserve/` entries and the user reserves. `dtb.c` keeps using the blob in place (`g_dtb_blob`).

Found by the C3 critic of switch gauntlet run 5 while reviewing #1207. Not fixed. Whether it contributes to #1207/#1228 is a hypothesis, untested (see the last section).

## Evidence

| Capture (branch) | Kernel | `DTB parsed successfully at` | `Heap range` start |
|---|---|---|---|
| `run4-hw/c3/repro-main-6c1f4c9c-kexec-run01.txt` (gauntlet-4-hw-jetson) | main 6c1f4c9c test | 0x82cb9000 | 0x82cc0000 |
| `run4-hw/closeout/c3-1b85b315-jetson-kexec-suite-boot08.txt` (gauntlet-4-hw-jetson) | 1b85b315 test | 0x82d8b000 | 0x82d90000 |
| `run5-hw/c3/suite-kexec-60c548f3-boot01.txt` (gauntlet-5-hw-jetson) | 5069e658 test, PMM_DEBUG=ON | 0x835bb000 | 0x835c0000 |
| `run5-hw/c3/filt-uefi-1fefbfdd-boot01.txt` (gauntlet-5-hw-jetson) | same, UEFI-direct | 0x2679df000 (above the heap) | 0x835c0000 |

- In the 1b85b315 capture the layout print gives `.bss` end 0x82d866f0 and `End: 0x82d90000`; the boot stack is 0x4000, so the segment ends at 0x82d8a6f0 and the next page is 0x82d8b000, the DTB address. A rebuild of main 6c1f4c9c (`make kernel-test PLATFORM=JETSON_ORIN_NANO`) has `__stack_top` 0x82cb86f0 rounding to 0x82cb9000 and `__kernel_end` 0x82cc0000, matching the first row.
- Every kexec capture above prints, from the xHCI handoff code that runs after `pmm_init`: `xhci: dtb path probe rc: /=0 /cpus=-4 /aliases=-4` and `xhci: dtb root dump aborted (bad token=0x0)` (`bad token=0xfcff` on the c3/jetson-suite kernels). Those lines are printed only when `/chosen` cannot be found, yet `dtb_init`, which runs before `pmm_init`, found `/chosen` (`kaslr-seed=8 B`) in the same boot. 0xfcff is what a big-endian token read returns for the little-endian pointer 0xfffc0000, a block boundary of PMM region 2 (hypothesis: a free-list link written into the blob). The UEFI-direct capture has no such line.

Not verified: the size of the kexec blob (no capture prints `totalsize` on the kexec path), and that its `/memreserve/` map does not cover the blob itself.

## Consequences

- Any DTB lookup after `pmm_init` on a kexec boot reads memory the allocator owns (xHCI handoff properties, `macb`, the `dtb-dump` shell command).
- The first free blocks of region 1 (at `heap_start`) carry their free-list headers inside the blob, and allocations from them overwrite it.

## Suggested next step

Print `[dtb, dtb + totalsize)` next to the heap range at boot, reserve it with `pmm_user_reserve_add` before `pmm_init` (or copy the blob into kernel memory), and add a test that a DTB lookup still works after the PMM suite. Then rerun the #1207 kexec suite loop to see whether the halt rate changes.

## Relation to #1207 / #1228 (hypothesis, untested)

No in-place writer of the blob was found in the kernel, so this alone does not explain a zeroed or overwritten free-list link. It is one more region on kexec boots, and only on kexec boots, where PMM state and foreign data share pages, and #1207/#1228 have only been seen on kexec.
