# Lifecycle run 4, C3: Jetson integration suite (#1199, #1224)

Branch `c3/jetson-suite`, PR head 8a3406ea (no PR opened: the kexec suite cell is 9/10, run06 halted in test_pmm, #1207).

- Step-1 repro on main 6c1f4c9c: `repro-main-6c1f4c9c-{kexec,uefi}-run0{1,2,3}.txt`.
- Old-code failures of the new checks: `test-1199-fails-on-old-code-qemu*.txt`, `build-guard-1199.txt`; the layout check run on main's image is in the session only.
- Final head suites: `final-8a3406ea-{kexec,uefi}-suite-run01..10.txt`; the earlier kexec set at d512f40b is `final-d512f40b-kexec-suite-run*.txt`; superseded heads are `interim-*.txt`.
- Production boots: `prod-8a3406ea-{kexec,uefi}-boot*.txt`; Pi 5: `pi5-boot-test-8a3406ea.txt`.
- /ci: `ci-8a3406ea/` (PR head, full logs and TABLE.txt); `ci-d512f40b.txt` and `ci/` are the earlier run.
- ESP: `esp-asfound.txt`, `esp-restore.txt`; lab release: `release-second-hold.txt`.

## Reproducing the UEFI-direct failing set

The UEFI-direct test-kernel failure list (#1231, and the oplib "unstaged" tests in particular) depends on the image carrying the operator-library shaders. `make kernel-test PLATFORM=JETSON_ORIN_NANO` runs `scripts/cuda/ensure-oplib-shaders.sh` (Makefile line ~398); if that step finds no shaders, the GPU paths are CPU-only and the UEFI failing set differs. Run it (or check its output in the build log) before comparing a new UEFI run with these files.
