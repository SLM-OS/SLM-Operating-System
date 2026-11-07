# ADR 0002: Deliver models through UEFI file protocols at boot

## Context

`slm load <vfs-path>` of a 1.1 GB GGUF must work after a cold UEFI-direct
boot on Jetson Orin Nano with no host involved (#475). Jetson has no block
driver at runtime: no NVMe, PCIe or Jetson SDMMC, and `boot_media_create`
returns NULL. `/mnt/files` is a 64 MB ramdisk, and `slm xload` needs a host
and about 10 minutes at 2 MB/s. While Boot Services are alive, the firmware
can read the disks.

## Decision

The EFI stub reads `\slmos\models\*.gguf` from any SimpleFileSystem volume
before ExitBootServices, NVMe first, and a later volume is the fallback for
the same file name. Files go in physically contiguous LoaderData pages,
2 MiB aligned, in `0x100000000..0x240000000`: inside the RAM identity map and
clear of the link range, load image and DTB. Each file is hashed and cleaned
by VA (`DC CIVAC`), then recorded in `.data`. Between `vmm_init` and
`pmm_init` the kernel re-checks magic and hash, then reserves the pages with
`pmm_user_reserve_add`; a record that fails is dropped and its pages stay
free. Passing files appear read-only at `/boot/models`. `slm load` borrows
those pages through `load_slm_borrow_pages`, so there is no copy, and unload
leaves them reserved. Every failure in the stub is non-fatal.

## Alternatives considered

- **NVMe or SDMMC driver in SLM-OS.** The lasting answer, but out of scope
  for now. PCIe has no Jetson backend on either boot path.
- **Model on the ESP.** The 512 MiB partition is too small for the model.
- **Repartition the NVMe.** Not needed if the firmware reads the existing
  ext4 APP partition, and it would disturb the spare L4T rootfs.
- **Embed the model in `slmos.bin`.** Rebuilds and redeploys 1.1 GB per
  kernel change, and the relocation copy runs uncached.
- **`slm xload` from a host.** Fails the no-host requirement.

## Consequences

- Every boot with a model on disk spends the read and hash time before the
  kernel starts. The stub stops reading after 180 s, and the firmware's
  5-minute watchdog remains the backstop for a Read that never returns.
- Reserved pages leave the PMM for the whole boot, even after `slm unload`.
- Models change only by writing the disk from SD Linux and rebooting.
- `VFS_MAX_NAME` grew from 32 to 64 to hold real model file names.
- The kexec path is unchanged: its table is empty, so `/boot/models` is
  never mounted.

## Evidence

Host: `make test` (`test_boot_models.c`, fake firmware), `make
test-qemu-uefi` (real AAVMF firmware; read error by blkdebug), `make
runtime-test`.

Hardware, jetson-nano-2: pending. Volume visibility, SD and NVMe read speed,
and the choice of NVMe first are not yet measured.
