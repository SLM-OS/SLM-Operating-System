# Lifecycle run 4 close-out: #1237 (c1/1213-fix 33f05c5a) production boots on jetson-nano-2, 2026-11-12

Each `1237-33f05c5a-jetson-{kexec,uefi}-bootNN.txt` is one labctl `boot_test` output_dir file, copied byte for byte (cmp-checked): the full serial log from power-on (MB1 at ~0.06 s) through UEFI, SD Linux, the handoff and the SLM-OS banner, to the `slmos>` prompt. Nothing is added or cut. The files contain a few non-UTF-8 bootloader bytes; use `grep -a`.

- Image: `make kernel PLATFORM=JETSON_ORIN_NANO` (defaults) at 33f05c5a. slmos.elf sha256 b95c0d4774f69e5d (staged as /root/slmos-l4co-prod-33f05c5a.elf, sha checked on the board), slmos.bin sha256 276ffa90ce2c135c (written once to the NVMe ESP as EFI/SLMOS/SLMOS.efi: `jetson-esp-write-33f05c5a-prod.txt`, fsck clean). Banner in every file: `SLM-OS v0.4.0 build 20261112180315 (33f05c5a)`.
- Method: boot_test runs=5 twice per path, expect `slmos>`, timeout 420 s, cycle delay 15 s. boot_test cannot kexec or set BootNext, so a host-side loop (`trigger.sh.txt`, session-local, nothing installed on the board) watched for each new Linux boot_id over ssh, waited for `systemctl is-system-running`, then ran the documented trigger: `setsid nohup slmos-kexec <elf>` (kexec), or `efibootmgr --bootnext 000E` and reboot (UEFI-direct, as scripts/jetson-uefi-deploy.sh does). Trigger logs: `*-triggers-*.log` (one trigger per boot, 23-26 s after Linux start).
- Result: **kexec 10/10** (154.5-158.2 s power-on to prompt), **UEFI-direct 10/10** (127.2-151.4 s). In every file: one banner, the expected boot source line, 0 lines matching `KERNEL PANIC|PAGE FAULT|Unhandled Exception|System halted|ABORT|Synchronous|EXCEPTION`, 0 `[ERROR]` lines. Kexec files contain one `kexec_core: Starting new kernel`; UEFI files contain one `reboot: Restarting system` and `Boot source: UEFI-direct (EFI stub)`.
- As found (`jetson-asfound.txt`): SD Linux, ESP SLMOS.efi 789cba62, no BootNext. Restored at the end of the session: `jetson-restore.txt`.

| File | Result | Lines | Boot source | Fault lines |
|---|---|---|---|---|
| 1237-33f05c5a-jetson-kexec-boot01.txt | PASS, slmos> | 1901 | direct load | 0 |
| 1237-33f05c5a-jetson-kexec-boot02.txt | PASS, slmos> | 1901 | direct load | 0 |
| 1237-33f05c5a-jetson-kexec-boot03.txt | PASS, slmos> | 1902 | direct load | 0 |
| 1237-33f05c5a-jetson-kexec-boot04.txt | PASS, slmos> | 1901 | direct load | 0 |
| 1237-33f05c5a-jetson-kexec-boot05.txt | PASS, slmos> | 1916 | direct load | 0 |
| 1237-33f05c5a-jetson-kexec-boot06.txt | PASS, slmos> | 1901 | direct load | 0 |
| 1237-33f05c5a-jetson-kexec-boot07.txt | PASS, slmos> | 1901 | direct load | 0 |
| 1237-33f05c5a-jetson-kexec-boot08.txt | PASS, slmos> | 1901 | direct load | 0 |
| 1237-33f05c5a-jetson-kexec-boot09.txt | PASS, slmos> | 1901 | direct load | 0 |
| 1237-33f05c5a-jetson-kexec-boot10.txt | PASS, slmos> | 1901 | direct load | 0 |
| 1237-33f05c5a-jetson-uefi-boot01.txt | PASS, slmos> | 2969 | UEFI-direct (EFI stub) | 0 |
| 1237-33f05c5a-jetson-uefi-boot02.txt | PASS, slmos> | 2968 | UEFI-direct (EFI stub) | 0 |
| 1237-33f05c5a-jetson-uefi-boot03.txt | PASS, slmos> | 2967 | UEFI-direct (EFI stub) | 0 |
| 1237-33f05c5a-jetson-uefi-boot04.txt | PASS, slmos> | 2966 | UEFI-direct (EFI stub) | 0 |
| 1237-33f05c5a-jetson-uefi-boot05.txt | PASS, slmos> | 2968 | UEFI-direct (EFI stub) | 0 |
| 1237-33f05c5a-jetson-uefi-boot06.txt | PASS, slmos> | 2971 | UEFI-direct (EFI stub) | 0 |
| 1237-33f05c5a-jetson-uefi-boot07.txt | PASS, slmos> | 2968 | UEFI-direct (EFI stub) | 0 |
| 1237-33f05c5a-jetson-uefi-boot08.txt | PASS, slmos> | 2967 | UEFI-direct (EFI stub) | 0 |
| 1237-33f05c5a-jetson-uefi-boot09.txt | PASS, slmos> | 2979 | UEFI-direct (EFI stub) | 0 |
| 1237-33f05c5a-jetson-uefi-boot10.txt | PASS, slmos> | 2968 | UEFI-direct (EFI stub) | 0 |
