# Troubleshooting Guide

Common issues and solutions for SLM-OS development and hardware bring-up.

---

## Build Issues

### Permission denied during linking

**Symptom:**
```
ld: cannot open output file slmos.elf: Permission denied
```

**Causes:**
1. QEMU process holding lock on the ELF file
2. CLion or IDE indexing the build directory
3. Google Drive sync locking files

**Solutions:**
1. Kill any running QEMU processes:
   ```bash
   # Windows (from Cygwin/Git Bash)
   tasklist.exe | grep -i qemu
   taskkill.exe /F /PID <pid>

   # Or use Task Manager
   ```

2. Build to a local directory (not on Google Drive):
   ```bash
   make BUILD_DIR=/c/temp/slmos-build kernel
   ```

3. If using CLion, disable auto-reload in Settings > Build > CMake

### Rust compilation fails

**Symptom:**
```
error: could not find `aarch64-unknown-none` in target list
```

**Solution:**
Add the bare-metal target:
```bash
rustup target add aarch64-unknown-none
```

### "make: command not found"

**Solution:**
Use the full path to Cygwin make:
```bash
"C:/cygwin64/bin/make.exe" kernel
```

---

## QEMU Issues

### Kernel hangs silently (no output)

**Possible causes:**
1. Boot code crashes before UART init
2. Exception during early boot
3. MMU misconfiguration

**Debugging steps:**
1. Run with GDB:
   ```bash
   make debug    # In one terminal
   make gdb      # In another terminal
   ```

2. Set breakpoint at entry:
   ```gdb
   b _start
   c
   ```

3. Single-step through boot code:
   ```gdb
   si
   ```

### Exception at address 0x0

**Symptom:**
```
[EXCEPTION] Instruction Abort
  ELR: 0x0000000000000000
```

**Cause:** Null pointer dereference, likely due to:
- Uninitialized function pointer
- Stack corruption
- Context switch bug

**Solution:**
Check recent changes to task creation or context switch code. Verify `struct task` field offsets match assembly constants in `context.S`.

### Tests timeout

**Symptom:**
```
make: *** [test] Error 124
```

**Causes:**
1. Infinite loop in test code
2. Deadlock in scheduler
3. Test waiting for event that never occurs

**Solutions:**
1. Increase timeout:
   ```bash
   make TEST_TIMEOUT=120 test
   ```

2. Run interactively to see output:
   ```bash
   make run
   ```

3. Check for spinlock deadlocks (wrong lock ordering)

---

## Hardware Issues (Jetson)

### No serial output

**Symptoms:**
- Terminal shows nothing after power-on
- Or shows garbage characters

**Checklist:**
1. **Verify wiring:**
   | Jetson J14 | USB-UART |
   |------------|----------|
   | Pin 6 (GND) | GND |
   | Pin 8 (TX) | RX |
   | Pin 10 (RX) | TX |

2. **Check baud rate:** Must be 115200, 8N1

3. **Verify USB-UART adapter:**
   - Must be 3.3V (not 5V!)
   - Try different USB port
   - Check device appears (`ls /dev/ttyUSB*` on Linux)

4. **Try the combined UART:** Jetson Orin uses TCU (Tegra Combined UART), not traditional UART. The debug port on J14 should work.

### Kernel doesn't boot

**Symptoms:**
- UEFI menu appears but SLM-OS hangs
- Or immediate reboot after selecting SLM-OS

**Checklist:**
1. **Verify extlinux.conf:**
   ```
   LABEL slmos
       LINUX /boot/slmos.elf
       FDT /boot/tegra234-p3768-0000+p3767-0000.dtb
   ```

2. **Check file permissions:**
   ```bash
   ls -la /boot/slmos.elf
   # Should be readable
   ```

3. **Verify DTB path:** The exact DTB filename may vary by Jetson variant

4. **Try kernel console:** Add to APPEND line:
   ```
   APPEND console=ttyTCU0,115200 earlyprintk
   ```

### Exception during hardware init

**Symptom:**
```
[EXCEPTION] Data Abort at 0x...
  FAR: 0x03880000  (some hardware address)
```

**Cause:** Accessing hardware register that:
- Requires different clock/power configuration
- Has different address on Jetson vs QEMU
- Needs privilege level configuration

**Solution:**
1. Check if address matches expected peripheral (compare to Tegra TRM)
2. Verify platform detection is working
3. Ensure clocks/power domains are enabled before access

### Memory issues on Jetson

**Symptom:**
- Works on QEMU but crashes on Jetson
- Or only some memory is accessible

**Causes:**
1. Different RAM base address
2. Memory carve-outs for GPU/firmware
3. DRAM configuration differences

**Solutions:**
1. Check device tree for memory node:
   ```bash
   # On running Linux Jetson:
   cat /proc/device-tree/memory@80000000/reg | xxd
   ```

2. Verify `RAM_BASE` and `RAM_SIZE` in `platform.h` match actual hardware

3. Leave space for firmware/GPU carve-outs (typically first ~128MB reserved)

---

## Scheduler Issues

### Tasks never run

**Symptom:**
- Task created but entry function never executes
- `produced=0, consumed=0` in multi-task tests

**Causes:**
1. Task stuck in BLOCKED state
2. Scheduler not running on secondary CPUs
3. Task pinned to CPU that's not processing run queue

**Solutions:**
1. Check task state after creation
2. Verify secondary CPUs are running (`smp_boot()` succeeded)
3. Use `CPU_AFFINITY_ANY` instead of pinning

### Priority inversion symptoms

**Symptom:**
- High-priority task waits unexpectedly
- Low-priority task runs while high-priority blocked

**Solution:**
Use `pi_mutex_t` instead of `spinlock_t` for mutexes held across blocking operations.

### Context switch corruption

**Symptom:**
- Random crashes after task switch
- Register values wrong after yield()

**Cause:** `struct task` layout changed without updating assembly offsets.

**Solution:**
Verify `TASK_CONTEXT_OFFSET` in `context.S` matches actual offset of `context` field in `struct task`.

---

## Memory Issues

### Page fault during model allocation

**Symptom:**
```
[EXCEPTION] Data Abort - Translation fault
```

**Causes:**
1. Model memory pools not initialized
2. Allocation request exceeds pool size
3. MMU mappings incomplete

**Solutions:**
1. Verify `rust_model_mem_init(MODEL_MEM_WEIGHT_MB, MODEL_MEM_WORKSPACE_MB)` is called during boot (call site in `kernel/src/main.c`)
2. Check the per-platform pool sizes in `kernel/include/config.h` — Jetson Orin Nano needs 2048/256 for SLM workloads; QEMU defaults of 256/128 will under-fit a 1.5 B parameter Q4_K_M model
3. Verify physical memory is mapped in page tables

### Memory leak detected

**Symptom:**
- Free page count decreases over time
- `pmm_get_free_pages()` shows fewer pages than expected

**Debugging:**
1. Use IPC memory leak test pattern:
   ```c
   size_t before = pmm_get_free_pages();
   // ... operations ...
   size_t after = pmm_get_free_pages();
   ASSERT(before == after);
   ```

2. Check for missing `free()` calls after allocation

3. Verify reference counting in shared resources

---

## IPC Issues

### Message queue timeout

**Symptom:**
```
msg_recv() returned IPC_ERR_TIMEOUT
```

**Causes:**
1. Producer not sending messages
2. Queue capacity too small
3. Timeout value too short

**Solutions:**
1. Verify producer task is running
2. Increase queue capacity in `msg_queue_create()`
3. Use `TIMEOUT_INFINITE` for blocking operations

### Shared memory access fault

**Symptom:**
```
[EXCEPTION] Data Abort accessing shared buffer
```

**Causes:**
1. Buffer not mapped in task's address space
2. Wrong permissions (writing to read-only)
3. Buffer freed while still mapped

**Solutions:**
1. Call `shm_map()` before accessing buffer
2. Check `SHM_PROT_*` flags match access pattern
3. Verify buffer lifetime extends beyond all mappings

---

## Debugging Tips

### Finding crash location

```bash
# Get address from exception output, then:
aarch64-none-elf-addr2line -e build/kernel/slmos.elf 0x40001234
```

### Dumping task state

Add to shell or debug code:
```c
extern void task_dump_all(void);  // Prints all task states
```

### Checking scheduler state

```c
extern void sched_dump_queues(void);  // Prints per-CPU run queues
```

### Verifying memory pools

```c
PoolStats stats = weight_pool_stats();
uart_printf("Weight pool: %zu/%zu blocks free\n",
    stats.free_blocks, stats.total_blocks);
```

---

## Getting Help

1. Check existing documentation in `docs/`
2. Review test cases in `kernel/tests/` for usage examples
3. Search for similar issues in git history
4. Check `FUTURE.md` for known limitations

---

## Phase 5: Model Loading and Inference Issues

### Stack Overflow During Model Loading or Inference

**Symptom:** Kernel panic (data abort, instruction abort) during `model load` or `model infer`.

**Cause:** The `ParsedOnnx` struct (~6KB) and `OperatorGraph` (~4KB) are stack-allocated. In debug builds, Rust creates larger stack frames. Combined with the inference engine call chain, total stack usage can exceed the limit.

**Fix:**
- Use Release builds (`BUILD_TYPE=Release` in Makefile) for smaller stack frames
- Stack size is configured in `kernel/include/config.h` as `STACK_SIZE` (currently 64KB)
- Reduce parser constants in `runtime/src/loader/onnx_parser.rs` (`MAX_NODES`, `MAX_INITIALIZERS`, `NAME_LEN`) if models exceed limits

### Model Memory Pool Exhaustion

**Symptom:** `model load` fails after loading/unloading several models, or tests hang during model loading.

**Cause:** The model memory pools (weight pool: 256MB, workspace pool: 128MB) allocate in 2MB blocks. Each model load consumes at least 1 weight block + 1 workspace block (4MB minimum). Repeated load/unload in test suites can exhaust available blocks.

**Fix:**
- Check pool status with `model pools`
- Ensure models are unloaded after use (`model unload <name>`)
- The registry `init()` function only initializes once — it does NOT free existing allocations

### Inference Engine Deadlock

**Symptom:** `model infer` or `rust_infer_classify` hangs indefinitely.

**Cause (fixed):** The `run_inference()` function acquired a spinlock but early-return on init failure (via `?` operator) skipped the unlock. All subsequent inference calls would spin forever.

**Fix:** This was fixed by replacing the `?` early return with an explicit `match` that ensures `engine_unlock()` is always called. If the issue recurs, check that all paths through `run_inference()` in `runtime/src/inference/engine.rs` release the `ENGINE_LOCK`.

### QEMU MMU Hang with User-Accessible Pages

**Symptom:** System hangs at "Enabling MMU..." when `VMM_FLAG_USER` is set on RAM page table entries.

**Cause:** Setting `PTE_AP_RW_ALL` (AP[1]=1) in L2 block descriptors causes QEMU to hang during MMU enable. Tested with `-cpu cortex-a53`, `-cpu cortex-a76`, and `-cpu max` — all hang. PAN (Privileged Access Never) and SPAN were investigated and ruled out. The PTE format is valid per the ARM Architecture Reference Manual. Root cause is believed to be a QEMU-specific emulation limitation.

**Status:** EL0 data access is deferred to hardware testing (Pi 5, Jetson). The syscall infrastructure works at EL1. See `docs/component-isolation.md` for details.

### ONNX Model Name Truncation

**Symptom:** Tensor names not found during inference (WeightNotFound error).

**Cause:** The ONNX parser uses fixed-size name buffers (`NAME_LEN=40` in parser, `TENSOR_NAME_LEN=40` in graph). ONNX models with tensor names longer than 39 characters will have their names truncated, causing name-mismatch errors during graph construction or inference.

**Fix:** Increase `NAME_LEN` and `TENSOR_NAME_LEN` if the target model has longer names. Be aware this increases struct sizes and stack usage.

---

*Last updated: April 2026*
