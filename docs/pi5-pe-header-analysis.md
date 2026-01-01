# Pi 5 PE Header Issue: Deep Analysis Report

**Date:** December 31, 2025
**Status:** Root cause identified, solution ready for implementation

## Executive Summary

The SLM-OS kernel fails to boot on Pi 5 because of a fundamental address calculation error when stripping the PE header. The minimal LED test (standalone binary) works, but the full kernel (extracted from PE binary) fails. The root cause is that symbol offset calculations reference `_start`, which no longer exists in the stripped binary.

**Solution:** Implement conditional compilation to skip the PE header entirely for Pi 5, making `_start` = `real_start` at the load address.

---

## Part 1: Binary Comparison Analysis

### 1.1 Test Results Summary

| Test | Binary Size | Result | Notes |
|------|-------------|--------|-------|
| Circle LED test | 65 KB | Works | 10 blinks |
| Minimal SLM-OS LED test | 184 bytes | Works | 5 blinks after EL2→EL1 |
| Infinite loop test | 4 bytes | Works | Pi hangs (code executes) |
| Full SLM-OS (stripped PE) | 879 KB | Fails | No LED activity |

### 1.2 Working vs Non-Working Code Comparison

**Working test (led_test_el1.bin) - First instructions:**
```asm
   0:   mrs     x0, currentel
   4:   cmp     x0, #0x8
   8:   b.ne    0x48
   c:   mov     x0, #0x33ff       ; EL2→EL1 transition
   ...
  48:   dsb     sy                ; LED blink starts here
  4c:   isb
  50:   mov     x0, #0x7c00       ; GPIO2 base address
```

**SLM-OS (slmos-pi5.bin) - First instructions:**
```asm
   0:   mrs     x10, currentel
   4:   cmp     x10, #0x8
   8:   b.ne    0x48
   c:   mov     x10, #0x33ff      ; EL2→EL1 transition
   ...
  48:   dsb     sy                ; LED blink starts here
  4c:   isb
  50:   mov     x10, #0x7c00      ; GPIO2 base address
```

**Observation:** The startup code is nearly identical. Both do EL2→EL1 transition and LED blink. The difference is register usage (x0-x3 vs x10-x13).

### 1.3 Critical Difference: Post-LED Code

**Working test at 0xb0:**
```asm
  b0:   wfe                       ; Infinite loop - test ends
  b4:   b       0xb0
```

**SLM-OS at 0xb0:**
```asm
  b0:   mov     x19, x0           ; Save DTB pointer
  b4:   mrs     x1, mpidr_el1     ; Check CPU ID
  ...
  c4:   ldr     x1, 0x118         ; Load stack offset from literal pool
  c8:   mov     sp, x1            ; Set stack pointer (WRONG VALUE!)
```

### 1.4 The Root Cause

At offset 0x118 in the stripped binary, we find:
```
00000118: 10bf 3900 0000 0000    ; Value: 0x0039bf10
```

This value (`0x0039bf10` = 3,784,464) is the **offset from `_start` to `__stack_top`**.

**The Problem:**

In the original `slmos.bin`:
- `_start` is at file offset 0x00000 (PE header)
- `real_start` is at file offset 0x10000 (actual code)
- Offset from `_start` to `__stack_top` = 0x39bf10

When we strip the PE header (skip first 64KB):
- The new file starts with what was at offset 0x10000
- `_start` is **no longer in the file**
- But the literal pool still contains offset relative to `_start`

**Boot.S code (after stripping):**
```asm
adr     x0, _start              ; Calculates address of _start
                                 ; But _start is 0x10000 BEFORE our binary!
                                 ; If loaded at 0x80000, this returns 0x70000
ldr     x1, .Lstack_offset      ; Loads 0x39bf10
add     x1, x0, x1              ; 0x70000 + 0x39bf10 = 0x43bf10 (WRONG!)
mov     sp, x1                  ; Stack pointer is garbage
```

**Expected stack address:** ~0x80000 + 0x39bf10 = 0xB9BF10
**Actual calculated:** 0x70000 + 0x39bf10 = 0xA9BF10 (0x10000 too low!)

This 64KB offset error causes the stack pointer to be set incorrectly, leading to undefined behavior as soon as any function call or stack operation occurs.

---

## Part 2: Raw Binary Solution Analysis

### 2.1 How Circle Handles This

Circle's approach (which works):
1. **No PE header** - kernel is pure executable code
2. **Simple linker script** - code placed directly at load address
3. **`--section-start=.init=0x80000`** - forces code to correct address
4. **All symbol offsets work** because there's no header gap

**Circle's linker invocation:**
```
ld --section-start=.init=0x80000 -T circle.ld ...
```

**Circle's startup64.S:**
```asm
.section .init

.globl _start
_start:                         ; Directly at 0x80000
    mrs     x0, CurrentEL
    ...
```

### 2.2 Solution Options

#### Option A: Conditional PE Header (Recommended)

Modify `boot.S` to conditionally include PE header:

```asm
#if !defined(PLATFORM_RASPI5)
/* PE/COFF header for UEFI boot (Jetson, etc.) */
_start:
    ccmp    x18, #0, #0xd, pl   ; MZ magic
    b       real_start
    ... 100+ lines of PE header ...
    .space  0x10000 - (. - _start)
#endif

#if defined(PLATFORM_RASPI5)
/* Raw binary for Pi 5 - no PE header needed */
_start:
#endif
real_start:
    /* Actual kernel code starts here */
    ...
```

**Advantages:**
- Minimal code changes
- Same source file, conditional compilation
- Linker script unchanged
- Symbol offsets work correctly

**Disadvantages:**
- Build must be platform-specific (already is)

#### Option B: Separate Boot File

Create `boot-raspi5.S` without PE header.

**Advantages:**
- Clean separation
- No conditionals in main boot.S

**Disadvantages:**
- Code duplication
- Two files to maintain

#### Option C: Fix Literal Pool References

Change literal pool to use `real_start` as base:

```asm
#if defined(PLATFORM_RASPI5)
    adr     x0, real_start
    .Lstack_offset: .quad __stack_top - real_start
#else
    adr     x0, _start
    .Lstack_offset: .quad __stack_top - _start
#endif
```

**Advantages:**
- Keeps PE header for other platforms
- Works with current stripping approach

**Disadvantages:**
- More conditionals throughout code
- Still requires manual stripping

### 2.3 Recommended Solution: Option A

**Rationale:**
1. Simplest implementation
2. Matches Circle's proven approach
3. No manual binary stripping required
4. Clean objcopy produces correct output
5. All symbol calculations work automatically

---

## Part 3: Implementation Plan

### 3.1 Files to Modify

1. **`kernel/arch/arm64/boot.S`**
   - Wrap PE header in `#if !defined(PLATFORM_RASPI5)`
   - For RASPI5, `_start` = `real_start`
   - Remove `real_start` label for RASPI5 (use `_start` directly)

2. **`kernel/kernel-raspi5.ld`** (minor update)
   - Ensure `.text.boot` is first section
   - Entry point remains `_start`

3. **`Makefile`** (optional)
   - Remove manual PE header stripping for RASPI5
   - `slmos.bin` is directly usable as `kernel_2712.img`

### 3.2 Code Changes

**boot.S modification:**

```asm
.section .text.boot
.global _start
.global real_start

#if !defined(PLATFORM_RASPI5)
/*
 * ARM64 Linux Image Header with PE/COFF stub
 * Required for UEFI boot on Jetson and similar platforms.
 */
_start:
    ccmp    x18, #0, #0xd, pl   /* PE/COFF "MZ" magic */
    b       real_start          /* Branch to actual code */
    .quad   0                   /* text_offset */
    .quad   _kernel_file_size   /* image_size */
    ... (rest of PE header)
    .space  0x10000 - (. - _start)

real_start:
#else
/*
 * Pi 5 raw binary - no PE header needed
 * Firmware loads at 0x80000 and jumps directly
 */
_start:
real_start:
#endif
    /* Actual kernel code - identical for all platforms */
#if defined(PLATFORM_QEMU_VIRT)
    ... QEMU early debug ...
#elif defined(PLATFORM_RASPI5)
    ... Pi 5 EL2→EL1 + LED blink ...
#endif
    ... rest of boot code ...
```

### 3.3 Build Process

**Current (broken):**
```bash
make kernel PLATFORM=RASPI5
dd if=build/kernel/slmos.bin of=kernel_2712.img bs=65536 skip=1  # Manual strip
```

**After fix:**
```bash
make kernel PLATFORM=RASPI5
cp build/kernel/slmos.bin kernel_2712.img  # Direct copy, no stripping
```

### 3.4 Verification Plan

1. Build kernel with modified boot.S
2. Verify binary starts with EL check (not PE header)
3. Copy directly to SD card as kernel_2712.img
4. Boot Pi 5, observe 3 LED blinks
5. Verify kernel continues to C code (serial output once PCIe init done)

---

## Part 4: Risk Assessment

### Low Risk
- Solution follows proven Circle approach
- Minimal code changes
- No impact on QEMU or Jetson platforms

### Medium Risk
- Need to ensure PLATFORM_RASPI5 is correctly defined during build
- Literal pool offsets must be verified

### Mitigation
- Test on QEMU first (should still work)
- Verify LED blinks on Pi 5
- Check objdump of generated binary before deployment

---

## Appendix A: Binary Layout Diagrams

### Current (Broken) Approach

```
slmos.bin (original):
┌─────────────────┐ 0x00000
│   PE Header     │
│   (_start)      │
│   64 KB         │
├─────────────────┤ 0x10000
│   real_start    │
│   (actual code) │
│                 │
└─────────────────┘

slmos-pi5.bin (stripped):
┌─────────────────┐ 0x00000 ← Loaded at 0x80000
│   real_start    │
│   (actual code) │
│                 │
│ Literal pool:   │
│ offset = 0x39bf10 (relative to _start which is GONE!)
└─────────────────┘

adr x0, _start → 0x80000 - 0x10000 = 0x70000 (WRONG!)
```

### Fixed Approach

```
slmos.bin (RASPI5, no PE header):
┌─────────────────┐ 0x00000 ← Loaded at 0x80000
│   _start        │ ← _start = real_start
│   (actual code) │
│                 │
│ Literal pool:   │
│ offset = 0x39af10 (relative to _start which IS HERE)
└─────────────────┘

adr x0, _start → 0x80000 (CORRECT!)
add x1, x0, offset → 0x80000 + 0x39af10 = 0xB9AF10 (CORRECT!)
```

---

## Appendix B: Related Files

| File | Purpose |
|------|---------|
| `kernel/arch/arm64/boot.S` | Boot code, PE header, EL transition |
| `kernel/kernel-raspi5.ld` | Pi 5 linker script |
| `docs/pi5-baremetal-status.md` | Overall Pi 5 porting status |
| `/tmp/circle-framework/` | Reference implementation |
| `/tmp/led_test_el1.bin` | Working minimal test |
| `/tmp/slmos-pi5.bin` | Non-working stripped binary |
