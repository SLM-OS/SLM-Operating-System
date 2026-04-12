# Session E — Drivers

**Source:** `docs/code-review-2026-04-12.md` §4
**Scope:** 9 issues (2 CRITICAL, 3 HIGH, 2 MEDIUM, 2 LOW)
**Files touched:** `kernel/drivers/fb_console.c`, `kernel/drivers/uart_tegra.c`, `kernel/drivers/uart_x86.c`, `kernel/drivers/uart_rp1.c`, `kernel/drivers/virtio_net.c`, `kernel/drivers/gic.c`, `kernel/arch/x86_64/lapic.c`, `kernel/src/kprintf.c`

## Mission

Fix MMIO ordering bugs, interrupt-controller EOI races, and missing cache
maintenance around DMA. Driver files are largely independent of each other
and of other sessions' work, so this session parallelizes cleanly.

## Working rules

- Read `CLAUDE.md` and `kernel/CLAUDE.md`. Jetson UART notes in the Jetson
  section of the root CLAUDE.md are load-bearing.
- MMIO changes are platform-specific — test on the affected target. Jetson
  UART fixes need lab hardware.
- The recent static-buffer race fix (commit 3c2d9a1) is in the Rust
  runtime, not the kernel UART path. `kprintf` is already locked with
  UART_LOCK. Don't duplicate.
- Work in a worktree: `git worktree add ../slm-os-session-e -b fix/session-e-drivers`.
- Ask John before committing.

## Issues

### CRITICAL

#### DRV-C1 — Framebuffer scroll bounds
- **File:** `kernel/drivers/fb_console.c:238-243`
- **Problem:** `fb_scroll` copies `fb_info.pitch` bytes per row without
  reclamping against `fb_info.height` on the last-line clear.
- **Fix:**
  ```c
  int last_line_y = fb_info.height - char_height;
  if (last_line_y < 0) return;  /* too small to scroll */
  /* copy upward... */
  /* Clear last line — clamp to remaining rows */
  for (int y = last_line_y; y < fb_info.height; y++) {
      memset(fb + y * fb_info.pitch, 0, fb_info.pitch);
  }
  ```
  Add assertions that `pitch * height <= fb_info.size`.
- **Verification:** Exercise scroll by writing many lines in QEMU x86-64 with
  framebuffer enabled.

#### DRV-C2 — LAPIC EOI missing ordering
- **File:** `kernel/arch/x86_64/lapic.c:123-125`
- **Problem:** Out-of-order speculation may re-enter an ISR before EOI is
  retired.
- **Fix:**
  ```c
  void lapic_eoi(void) {
      lapic_write(LAPIC_EOI, 0);
      /* Serialize: ensure EOI is visible before returning to handler epilogue */
      __asm__ volatile("lock; addl $0, (%%rsp)" ::: "memory", "cc");
  }
  ```
  `lock` prefix provides a full fence on x86. Alternative: `mfence`.
- **Verification:** `make test PLATFORM=X86_64` stress with timer interrupts;
  look for duplicate handler invocations.

### HIGH

#### DRV-H1 — `uart_tegra` missing DSB on non-TCU RX fallback
- **File:** `kernel/drivers/uart_tegra.c:286`
- **Problem:** TCU path has `dsb sy` before LSR read; non-TCU fallback does
  not. Speculative MMIO returns stale data after kexec.
- **Fix:**
  ```c
  #else
      for (;;) {
          __asm__ volatile("dsb sy" ::: "memory");
          if ((UART_REG(NS16550_LSR) & LSR_DR) != 0) break;
      }
  #endif
  ```
- **Verification:** Jetson boot via kexec — confirm RX path works reliably.
  `labctl boot_test` on jetson-nano-1 or nano-2.

#### DRV-H2 — `virtio_net` missing cache flush on descriptor ring updates
- **File:** `kernel/drivers/virtio_net.c:189-191, 217-220`
- **Problem:** `virtio_mb()` is not sufficient on Pi 5 / Jetson (no SMPEN).
  Device DMA reads through PoC miss dirty L2 lines.
- **Fix:** After updating `avail->idx` (and when adding to descriptor ring):
  ```c
  virtio_mb();
  #ifdef PLATFORM_HAS_NC_MEMORY
      cache_clean_range(&vq->avail->idx, sizeof(vq->avail->idx));
      /* And the descriptor table entry just written */
      cache_clean_range(&vq->desc[head], sizeof(vq->desc[head]));
  #endif
  ```
  Check `kernel/include/cache.h` for the correct helper name.
- **Verification:** Pi 5 networking smoke test (ping over virtio_net or
  similar — see `docs/networking.md`).

#### DRV-H3 — `uart_x86` init missing explicit barriers
- **File:** `kernel/drivers/uart_x86.c:42-52`
- **Fix:** Add `__asm__ volatile("" ::: "memory")` between configuration
  writes. On x86 `outb` is already serialized; the explicit barrier is for
  compiler ordering and future maintainer clarity.

### MEDIUM

#### DRV-M1 — Framebuffer cursor not atomic
- **File:** `kernel/drivers/fb_console.c:316-342`
- **Fix:** If framebuffer output can reach from multiple CPUs, wrap in
  UART_LOCK or a dedicated fb lock. Otherwise document single-CPU invariant
  and add a `DEBUG_ASSERT(current_cpu() == 0)`.

#### DRV-M2 — GIC enable/disable atomic-semantics comment
- **File:** `kernel/drivers/gic.c:565-600`
- **Fix:** Add a comment above `GICD_ISENABLER` / `GICD_ICENABLER` writes
  noting that these are write-1-to-set registers (no RMW required). Prevents
  future refactors from breaking atomicity.

### LOW

#### DRV-L1 — `uart_rp1` FUNCSEL sequencing volatile delay
- **File:** `kernel/drivers/uart_rp1.c:175-179`
- **Fix:** Expand the comment with the empirical origin; consider
  `timer_busy_wait_us(1)` if the timer is initialized at this point.

#### DRV-L2 — Stale SWPALB comment in kprintf
- **File:** `kernel/src/kprintf.c:43-53`
- **Fix:** Update the comment block to describe current IRQ-disable-only
  locking. Remove references to SWPALB alternatives that are no longer used.

## Suggested work order

1. **DRV-C2** (5 min) — LAPIC EOI fence
2. **DRV-C1** (30 min) — fb_scroll bounds
3. **DRV-H3** (10 min) — uart_x86 compiler barriers
4. **DRV-L2** (5 min) — kprintf comment cleanup
5. **DRV-M2** (5 min) — GIC atomic-semantics comment
6. **DRV-H1** (20 min) — Tegra UART DSB
7. **DRV-H2** (30 min) — virtio_net cache_clean_range
8. **DRV-M1** (15 min) — fb cursor doc/assert
9. **DRV-L1** (10 min) — rp1 delay comment

## Testing requirements

- **Every fix:** `make test` (QEMU ARM64). `make test PLATFORM=X86_64`.
- **DRV-C1, M1:** x86-64 QEMU with framebuffer — exercise scrolling.
- **DRV-C2:** x86-64 timer-interrupt stress.
- **DRV-H1:** Jetson — kexec boot via `slmos-kexec`, verify RX works after
  transfer.
- **DRV-H2:** Pi 5 networking smoke test. If lwIP isn't wired up on Pi 5
  today, at minimum confirm `make test` net tests pass on QEMU.
- **DRV-H3:** x86-64 early-boot UART output intact.

## Deliverable — PR template

```
## Summary
Session E fixes from code-review-2026-04-12: driver MMIO / DMA / ordering.

## Issues fixed
- DRV-C1 (fb bounds), DRV-C2 (LAPIC EOI fence)
- DRV-H1 (Tegra DSB), DRV-H2 (virtio cache maint), DRV-H3 (x86 UART barriers)
- DRV-M1, M2 (fb cursor, GIC docs)
- DRV-L1, L2 (comments)

## Test plan
- [ ] `make test` (QEMU ARM64) passes
- [ ] `make test PLATFORM=X86_64` passes
- [ ] QEMU x86-64 framebuffer scroll stress passes
- [ ] Jetson kexec boot — RX after kexec works
- [ ] Pi 5 networking smoke test passes
- [ ] x86-64 early UART output intact
```
