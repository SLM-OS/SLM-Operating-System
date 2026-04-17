Run the SLM-OS CI pipeline locally: build all targets and run the QEMU test suite.

## Instructions

Run the following steps in order. Stop and report on first failure.

### 1. Build — ARM64 QEMU target
```
make clean && make PLATFORM=qemu
```
Report: pass/fail, any warnings.

### 2. Build — x86-64 QEMU target
```
make clean && make PLATFORM=x86_64
```
Report: pass/fail, any warnings.

### 3. Run test suite (ARM64 QEMU)
```
make test
```
Parse the output for `[PASS]`, `[FAIL]`, `PAGE FAULT`, and `KERNEL PANIC`.
Report: total tests, passed, failed, any crashes.

### 4. Diff check
Run `git diff main...HEAD --stat` and confirm no untracked build artifacts
are being committed (`.o`, `.bin`, `.elf`, `.iso` files).

## Output

Summarize results as a table:

| Step | Status | Notes |
|------|--------|-------|
| ARM64 build | ✅/❌ | warnings if any |
| x86-64 build | ✅/❌ | warnings if any |
| Test suite | ✅/❌ | N/M passed |
| Artifact check | ✅/❌ | stray files if any |

If everything passes, say "Ready for PR."
If anything fails, list what needs fixing before a PR can be opened.
