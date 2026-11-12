# Lifecycle run 4 close-out: #1237 (c1/1213-fix 33f05c5a) on pi-5-1, 2026-11-12

Each `*-bootNN.txt` is one labctl `boot_test` output_dir file, copied byte for byte: the full serial log from power-on to the match. Nothing is added to or cut from them.

- Production: `make kernel PLATFORM=RASPI5` at 33f05c5a, slmos.bin sha256 8b827008c260d983, banner `build 20261112180020 (33f05c5a)`. boot_test runs=5, expect `slmos>`, timeout 120 s, output_dir /var/lib/labctl/output/l4co-pi5-prod-33f05c5a. **5/5**, 13.0-13.1 s each.
- Integration suite: `make kernel-test PLATFORM=RASPI5` at 33f05c5a, slmos.bin sha256 437c7fe5d3ac8ac9, banner `build 20261112180146 (33f05c5a)`. boot_test runs=3 (output_dir ...-a) then runs=2 (...-b), expect `All test suites passed|total failure\(s\)`, timeout 400 s. **5/5** reached the summary, 168.8-177.7 s.
- Deployed to kernel_2712.img only (config.txt has kernel=kernel_2712.img); original kept as .prev, restored after (6935256 bytes, mtime 2026-10-29); kernel8.img untouched. Board powered on and released about 18:23 UTC.

| File | Result | Lines | Samples collected | slot_reuse_stress | Summary | KERNEL PANIC / PAGE FAULT / Synchronous |
|---|---|---|---|---|---|---|
| 1237-33f05c5a-pi5-prod-boot01.txt | PASS, slmos> reached | 220 | - | - | - | 0 |
| 1237-33f05c5a-pi5-prod-boot02.txt | PASS, slmos> reached | 220 | - | - | - | 0 |
| 1237-33f05c5a-pi5-prod-boot03.txt | PASS, slmos> reached | 220 | - | - | - | 0 |
| 1237-33f05c5a-pi5-prod-boot04.txt | PASS, slmos> reached | 220 | - | - | - | 0 |
| 1237-33f05c5a-pi5-prod-boot05.txt | PASS, slmos> reached | 220 | - | - | - | 0 |
| 1237-33f05c5a-pi5-suite-boot01.txt | PASS, suite summary reached | 9979 | isolated=10/10, normal=10/10 | bad=0 | 18 total failure(s) | 0 |
| 1237-33f05c5a-pi5-suite-boot02.txt | PASS, suite summary reached | 9996 | isolated=10/10, normal=10/10 | bad=0 | 18 total failure(s) | 0 |
| 1237-33f05c5a-pi5-suite-boot03.txt | PASS, suite summary reached | 10014 | isolated=10/10, normal=10/10 | bad=0 | 18 total failure(s) | 0 |
| 1237-33f05c5a-pi5-suite-boot04.txt | PASS, suite summary reached | 10039 | isolated=10/10, normal=10/10 | bad=0 | 18 total failure(s) | 0 |
| 1237-33f05c5a-pi5-suite-boot05.txt | PASS, suite summary reached | 10099 | isolated=10/10, normal=10/10 | bad=0 | 18 total failure(s) | 0 |

No CPU 1 panic in any integration boot. The failing set (`1237-33f05c5a-pi5-suite-failset.txt`, 18 failures, the #1109 list) is identical in all 5 boots and to run 4's `run4-hw/c1/pi5/pi5-fix-5e5548a1-integration-run01.txt` (sorted [FAIL] lines md5 15789a1b).
