# Hailo-8/8L PCIe Driver — Annotated Notes

Annotated reference derived from Hailo's open-source Linux PCIe driver
(<https://github.com/hailo-ai/hailort-drivers>, SPDX: MIT, dual-licensed GPLv2)
and the userspace `hailort` runtime (<https://github.com/hailo-ai/hailort>,
SPDX: MIT / LGPL).

All driver file citations are to the **`hailo8` branch** of `hailort-drivers`
(the `master` branch of that repo drops Hailo-8 support — it is now for
Hailo-10/15/Mars only). Local copies of the fetched sources live alongside
this file as `hailo-*.c`/`hailo-*.h` in `docs/reference/`.

---

## Model freeze

For the Phase 4/5 milestones, SLM-OS will target:

- **Model:** MobileNetV1, INT8 weights/activations, 224x224x3 input, 1000-way
  softmax output (ImageNet). Standard classification baseline.
- **Source:** Hailo Model Zoo (<https://github.com/hailo-ai/hailo_model_zoo>),
  `mobilenet_v1` entry under `docs/public_models/HAILO8_classification.rst`.
- **Compiler:** Hailo Dataflow Compiler v5.3.0 (current stable at time of
  planning; runs on Linux x86-64 only; free for non-commercial use).
- **Artifact:** Pre-compiled `mobilenet_v1.hef` from the dev workstation.
  SLM-OS never invokes the compiler — the `.hef` is shipped as a VFS asset.

The `.hef` file compile is **not** required in this research phase. It is
deferred to the first workstation build in Phase 4.

---

## 1. PCIe identifiers

Source: `docs/reference/hailo-pcie-common.h` lines 38-44 (also
`hailo-pcie.c` line 1451, the `pci_device_id` table).

| Identifier                        | Value   |
|-----------------------------------|---------|
| PCI vendor (Hailo Technologies)   | `0x1E60`|
| Hailo-8 device ID                 | `0x2864`|
| Hailo-8L device ID (AI HAT+ 13T)  | `0x2864`*|
| Hailo-10H device ID               | `0x45C4`|
| Hailo-15L device ID               | `0x43A2`|
| Hailo-Mars device ID              | `0x26A2`|

\* Hailo-8L is a bin-down of Hailo-8 and enumerates with the same Hailo-8
device ID on PCIe; the driver treats both as `HAILO_BOARD_TYPE_HAILO8`.
SKU differentiation happens through firmware config, not PCI IDs.

`hailo-pcie.c` line 1451 shows the full `pci_device_id` table used for
`probe()` dispatch; for the hailo8 branch the only entry is:

```c
{PCI_DEVICE_DATA(HAILO, HAILO8, HAILO_BOARD_TYPE_HAILO8)}
```

---

## 2. BAR layout

Source: `docs/reference/hailo-pcie-common.h` lines 26-28; BAR map is done in
`hailo-pcie.c` `pcie_resources_init()` (lines 1013-1076).

| BAR  | Purpose             | SLM-OS notes                               |
|------|---------------------|--------------------------------------------|
| BAR0 | Config / bridge     | PLDA XpressRICH AXI-to-PCIe bridge regs;   |
|      |                     | ATR tables live at offset `0x700+` (see §4)|
|      |                     | Interrupt status/mask live here            |
| BAR2 | VDMA channel regs   | Per-channel descriptor-ring doorbells      |
| BAR4 | Firmware access     | MMIO window used to push firmware and      |
|      |                     | control messages into SoC SRAM via ATR     |

BAR1, BAR3, BAR5 are unused on Hailo-8. `MAX_BAR` in `hailo-pcie-common.h`
line 124 is 6 (index BAR0..BAR5).

Mapping in the Linux driver (`hailo-pcie.c:1029-1042`):

```c
err = hailo_bar_iomap(pdev, HAILO_PCIE_CONFIG_BAR,    &resources->config);
err = hailo_bar_iomap(pdev, HAILO_PCIE_VDMA_REGS_BAR, &resources->vdma_registers);
err = hailo_bar_iomap(pdev, HAILO_PCIE_FW_ACCESS_BAR, &resources->fw_access);
```

All three BARs are **MMIO**, not port I/O. SLM-OS maps them as
**device memory (Device-nGnRnE)** via `vmm_map_mmio()`. Do **not** mark them
cacheable — write-back caching breaks doorbells and descriptor writes.

---

## 3. Register layout — BAR0 (config)

Source: `docs/reference/hailo-pcie-common.c` lines 18-65 (macros) and
functions below.

| Offset | Reg                              | Width | Purpose                              |
|-------:|----------------------------------|------:|--------------------------------------|
| `0x0098` | `PCIE_CONFIG_VENDOR_OFFSET`    | 16-bit| Returns `0x1E60` when device alive   |
| `0x0188` | `BSC_IMASK_HOST`               | 32-bit| Host interrupt-enable mask           |
| `0x018C` | `BCS_ISTATUS_HOST`             | 32-bit| Host interrupt status (W1C)          |
| `0x0400` | `BCS_SOURCE_INTERRUPT_PER_CHANNEL`      | 32-bit | VDMA H2D channel IRQ bitmap |
| `0x0500` | `BCS_DESTINATION_INTERRUPT_PER_CHANNEL` | 32-bit | VDMA D2H channel IRQ bitmap |
| `0x0640` | `PCIE_REQUEST_SIZE_OFFSET`     | —     | FW response header size (BAR4 space) |
| `0x0700` | `ATR0_PCIE_BRIDGE_OFFSET` (base)| 20 B | ATR[0] (address translation reg)     |
| `0x0720` | `ATR1_PCIE_BRIDGE_OFFSET`      | 20 B  | ATR[1] — used as "FW loaded" flag   |
| `0x0740` | `ATR2_PCIE_BRIDGE_OFFSET`      | 20 B  | ATR[2]                               |
| `0x0760` | `ATR3_PCIE_BRIDGE_OFFSET`      | 20 B  | ATR[3] (max index is 3)              |

### BCS_ISTATUS_HOST bitfields

(`hailo-pcie-common.c` lines 23-30)

- `[7:0]`   VDMA source-channel IRQs (H2D)
- `[15:8]`  VDMA dest-channel IRQs (D2H)
- `[31:24]` Software interrupts. The `hailo_pcie_nnc_sw_interrupt_masks`
  enum (`hailo-pcie-common.h` lines 71-75) decodes these:
  - `0x02` FW notification (async event from SoC)
  - `0x04` FW control response ready
  - `0x08` Driver-down ack

### ATR (Address Translation Register) table

The PLDA bridge uses ATRs to map a PCIe config-space request at BAR4 into a
device-side physical address. Each ATR entry is 20 bytes and 4KB aligned
(`ATR_TABLE_SIZE = 0x1000`, `hailo-pcie-common.c` line 37):

```c
struct hailo_atr_config {
    u32 atr_param;         // = (ATR_PARAM | (index << 12)); ATR_PARAM = 0x17
    u32 atr_src;           // = 0 (host-side source)
    u32 atr_trsl_addr_1;   // device-side target addr, low  32 bits
    u32 atr_trsl_addr_2;   // device-side target addr, high 32 bits
    u32 atr_trsl_param;    // = 6 (AXI memory translation)
};
```

To write into device SRAM at a given device physical address, the driver:

1. Writes the device address into `atr_trsl_addr_{1,2}` of ATR[0].
2. Writes the payload data into BAR4 at offset `(dest_addr & 0xFFF)` — the
   bridge translates the access through ATR[0] into the device's address
   space.
3. Restores the original ATR[0] value if needed (it is shared with firmware
   at runtime, so writes during probe have to save/restore — see
   `hailo-pcie-common.c:575-633`).

ATR[1]'s `trsl_addr_1` is repurposed as a **firmware-loaded flag**: firmware
writes `PCIE_BLOCK_ADDRESS_ATR1 = 0x200000` there once it has finished
booting. `hailo_pcie_is_firmware_loaded()` (line 845) simply polls that
register.

---

## 4. Firmware upload protocol (Hailo-8 path)

### 4.1. Firmware image layout

Source: `docs/reference/hailo-fw-validation.h` (full file — 60 lines).

A Hailo-8 firmware image is a flat blob containing:

1. **Firmware header** (`firmware_header_t`, 24 B, little-endian):
   ```c
   struct firmware_header_t {
       u32 magic;            // 0x1DD89DE0 for Hailo-8
       u32 header_version;   // FIRMWARE_HEADER_VERSION_INITIAL (0)
       u32 firmware_major;
       u32 firmware_minor;
       u32 firmware_revision;
       u32 code_size;        // size of the fw_code that follows
   };
   ```
2. **Firmware code blob** (`code_size` bytes, 4-byte aligned per
   `FW_CODE_SECTION_ALIGNMENT`).
3. **Secure-boot certificate header** (`secure_boot_certificate_header_t`,
   8 B):
   ```c
   struct secure_boot_certificate_header_t {
       u32 key_size;
       u32 content_size;
   };
   ```
4. **Cert key blob** (`key_size` bytes).
5. **Cert content blob** (`content_size` bytes).

Size limits: `MAXIMUM_APP_FIRMWARE_CODE_SIZE = 0x40000` (256 KB),
`MAXIMUM_FIRMWARE_CERT_KEY_SIZE = 0x1000`, `MAXIMUM_FIRMWARE_CERT_CONTENT_SIZE
= 0x1000`. Validation happens in `FW_VALIDATION__validate_fw_headers()`
(`hailo-pcie-common.c:700+`, implementation in `hailo-fw-validation.c`).

Hailo-8 firmware filenames (`hailo-pcie-common.c:223-252`):

| File                              | Target address | Max size | Mandatory |
|-----------------------------------|---------------:|---------:|-----------|
| `hailo/hailo8_fw.bin`             | `0x00020000`   | `0x50000`| yes       |
| `hailo/hailo8_board_cfg.bin`      | `0x60001000`   | `0x500`  | no        |
| `hailo/hailo8_fw_cfg.bin`         | `0x60001500`   | `0x500`  | no        |

Only `hailo8_fw.bin` has the `has_header = true / has_core = true` flags
set — it contains both the app FW and core FW concatenated (Hailo-8 is a
HAILO_DEV_TYPE_INTEGRATED device, which is why
`FW_VALIDATION__validate_fw_headers()` validates both headers back-to-back).

### 4.2. Upload flow (Hailo-8 boot sequence)

Source: `hailo-pcie-common.c` lines 297-316 (compat table),
`hailo_pcie_write_firmware_batch()` line 808.

Per-board firmware addresses for Hailo-8
(`hailo-pcie-common.c:297-307`):

```c
[HAILO_BOARD_TYPE_HAILO8] = {
    .boot_fw_header       = 0xE0030,
    .boot_key_cert        = 0xE0048,
    .boot_cont_cert       = 0xE0390,
    .app_fw_code_ram_base = 0x60000,
    .core_code_ram_base   = 0xC0000,
    .core_fw_header       = 0xA0000,
    .raise_ready_offset   = 0x1684,  // "doorbell" to fw
    .boot_status          = 0xE0000,
}
```

The upload uses the ATR[0] window (not VDMA — see §5 below for VDMA
context). Each write happens through `write_memory()`
(`hailo-pcie-common.c:577`): set ATR[0] → write into BAR4 window → restore
ATR[0]. For the Hailo-8 `write_single_file()` path
(`hailo-pcie-common.c:769`):

1. `hailo_write_app_firmware()` — writes FW header, code, key cert,
   content cert.
2. If `has_core` is set, `hailo_write_core_firmware()` writes the core FW
   code and header.
3. `hailo_trigger_firmware_boot()` writes `1` to
   `stage->trigger_address` (Hailo-8: `0xE0980`, a doorbell that the boot
   ROM polls).

### 4.3. Boot ack and timeout

Source: `hailo-pcie-common.c:845-877`.

After the boot trigger, the host polls `ATR[1].trsl_addr_1` at
`hailo-pcie-common.c:847`; firmware writes `PCIE_BLOCK_ADDRESS_ATR1 =
0x200000` into it when boot completes.

- **Retry count:** `FIRMWARE_LOAD_WAIT_MAX_RETRIES = 100`.
- **Sleep per retry:** `FIRMWARE_LOAD_SLEEP_MS = 50` — so a **5 s total
  timeout**. Matches `FIRMWARE_WAIT_TIMEOUT_MS = 5000`.
- **Boot status** (independent): `fw_addresses->boot_status = 0xE0000` (on
  Hailo-8) returns `0x1 = BOOT_STATUS_UNINITIALIZED` until bootloader
  enters, then advances. `hailo_pcie_wait_for_boot()` polls this with a
  1 ms period and `COUNT_UNTIL_REACH_BOOTLOADER = 10` retries (**10 ms
  total**).

Two timeouts total:

1. Reach bootloader: 10 ms (via `boot_status`).
2. FW fully loaded: 5 s (via ATR[1] flag).

### 4.4. Control-channel message format (post-boot)

Source: `hailo-pcie-common.c:462-508`.

Once firmware is loaded, the "control" channel lives in BAR4. A command
is written at BAR4 offset 0 with layout:

```
| md5[16] | buffer_len (u32) | buffer[buffer_len] |
```

(size is rounded up to `FW_CODE_SECTION_ALIGNMENT = 4`.) Then the host
writes `FW_ACCESS_APP_CPU_CONTROL_MASK` (or `..._CORE_CPU_...` for CPU1) to
`fw_access` at `fw_addresses->raise_ready_offset` (`0x1684` on Hailo-8).
That write is a **doorbell** — it triggers an interrupt on the target
CPU of the SoC.

The response arrives in BAR4 at `PCIE_REQUEST_SIZE_OFFSET = 0x640`, same
format. A `HAILO_PCIE_NNC_FW_CONTROL_IRQ (0x04)` interrupt fires when it is
ready. `hailo-pcie-common.c:490-508` reads the response header + payload.

### 4.5. Control-channel wire-format gotchas (SLM-OS bring-up, 2026-04-18)

SLM-OS hit four separate, independently-debuggable bugs during the Phase 5.2
IDENTIFY bring-up on pi-5-1. Each produced a "response looks wrong" symptom
that was easy to confuse with earlier layers (MD5, ATR, timing). Recording
here so the next opcode port doesn't rediscover them.

1. **Header scalars are big-endian on the wire.**
   `common_header.{version, flags, sequence, opcode}` plus
   `parameter_count` go through `BYTE_ORDER__htonl` on the host and
   `ntohl` on parse (see `control_protocol__pack_request_header` in
   `hailort-control_protocol.cpp:199`). `firmware_version.{major, minor,
   revision}` is the exception — HailoRT memcpys it raw, so it stays
   native LE. Same rule for response `status.{major_status,
   minor_status}`: big-endian on the wire, unswap before checking.

2. **ISTATUS only latches bits after IMASK is set.**
   `BSC_IMASK_HOST` (BAR0 `0x0188`) must be OR'd with
   `BSC_ISTATUS_HOST_MASK` (`0xFF00_FFFF`) at least once, and
   `BCS_ISTATUS_HOST` should be cleared via a `0xFFFFFFFF`
   write-1-to-clear. Without it, `BCS_ISTATUS_HOST` reads zero forever
   even though firmware IS processing requests — looks like firmware is
   ignoring the doorbell. See `hailo_pcie_enable_interrupts` in
   `hailo-pcie-common.c:867`.

3. **Wait for the specific FW-control bit, not any non-zero.**
   `BCS_ISTATUS_HOST` multiplexes VDMA channel interrupts (bits `0-15`),
   notification IRQ (`0x02<<24`), FW-control IRQ (`0x04<<24`), and
   driver-down IRQ (`0x08<<24`). Polling for "non-zero" races ahead of
   the actual response on a quiet device and returns stale bytes. Mask
   to `0x04 << 24` specifically. See `hailo_pcie_nnc_sw_interrupt_masks`
   in `hailo-pcie-common.h:71`.

4. **Response has a `parameter_count` gap between status and body.**
   Wire layout is `[common_header(16)][status(8)][parameter_count(4)][body]`.
   HailoRT's parse walks past a `CONTROL_PROTOCOL__payload_t` (whose first
   field is `parameter_count`) before pointing its response body at
   `payload->parameters`. Miss the 4-byte gap and every scalar in the
   body shifts — on IDENTIFY, `fw_version.major` picks up
   `fw_version_length` (`0x0C00_0000`). Also: mark the body struct
   `__attribute__((packed))`. `product_number[42]` is not 4-aligned, so
   without packing the compiler tacks on 2 bytes of trailing padding,
   `sizeof(struct hailo_control_identify_response)` grows from the 162
   bytes firmware sends to 164, and the response-length check rejects a
   perfectly good response as truncated.

All four are implemented in `kernel/ai_accel/hailo/hailo_control.{c,h}`
and covered by the `test_control_identify_*` suite in
`kernel/tests/test_hailo.c`.

---

## 5. VDMA control channel

Source: `docs/reference/hailo-vdma-common.{c,h}`. The VDMA is a PLDA PCIe
"descriptor-DMA" engine (mentioned explicitly in
`hailo-vdma-common.c:942-943`: *"from PLDA PCIe reference manual, 9.2.5
Starting a Channel and Transferring Data"*).

### 5.1. Per-channel register layout

Base offset per channel: `CHANNEL_BASE_OFFSET(i) = i << 5`
(`hailo-vdma-common.h:19`), i.e. **32 bytes per channel**. Within each
channel's 32-byte register block:

| Offset | Name                     | Width | Purpose                        |
|-------:|--------------------------|------:|--------------------------------|
| `0x00` | `CHANNEL_CONTROL_OFFSET` | 8-bit | start/pause/abort bits         |
| `0x01` | `CHANNEL_DEPTH_ID_OFFSET`| 8-bit | desc-list depth + data-id      |
| `0x02` | `CHANNEL_NUM_AVAIL_OFFSET`| 16-bit| host: "N descriptors ready"   |
| `0x04` | `CHANNEL_NUM_PROC_OFFSET`| 16-bit| device: "N descriptors done"  |
| `0x08` | `CHANNEL_ERROR_OFFSET`   | 8-bit | error flags                    |
| `0x10` | `CHANNEL_DEST_REGS_OFFSET`| — | mirror of the above for D2H dir |

`MAX_VDMA_CHANNELS_PER_ENGINE` is 16 (from `hailo_ioctl_common.h`
`HAILO_MAX_VDMA_CHANNELS_COUNT`), and there is exactly **one VDMA engine**
on PCIe Hailo-8 (`HAILO_PCIE_DMA_ENGINES_COUNT = 1`,
`hailo-pcie-common.h:30`). All channels live in BAR2.

### 5.2. Control register bits

(`hailo-vdma-common.c:20-30`)

- `VDMA_CHANNEL_CONTROL_START = 0x01`
- `VDMA_CHANNEL_CONTROL_ABORT = 0b00`
- `VDMA_CHANNEL_CONTROL_ABORT_PAUSE = 0b10`
- `VDMA_CHANNEL_CONTROL_START_RESUME = 0b01`
- `VDMA_CHANNEL_CONTROL_START_PAUSE = 0b11`
- `VDMA_CHANNEL_CONTROL_START_ABORT_PAUSE_RESUME_BITMASK = 0x03`

Channel start (`hailo_vdma_start_channel()`, `hailo-vdma-common.c:859`):

1. Stop the channel (pause → wait-idle → abort).
2. Write descriptor list DMA address (low 16-47 to `ADDRESS_L`, high
   32 bits to `ADDRESS_H`).
3. Write `(depth << DESC_DEPTH_SHIFT) | (data_id << DATA_ID_SHIFT)` to
   `CHANNEL_CONTROL_OFFSET`.
4. Write `VDMA_CHANNEL_CONTROL_START` to control.

**Descriptor list DMA address must be 64 KB aligned** (`hailo-vdma-common.h:16`:
`VDMA_DESCRIPTOR_LIST_ALIGN = (1 << 16)`; also the check at
`hailo-vdma-common.c:867`: `(desc_dma_address & 0xFFFF) != 0`).

### 5.3. Descriptor format

(`hailo-vdma-common.h:35-40`) — 16 bytes each, packed:

```c
struct hailo_vdma_descriptor {
    u32 PageSize_DescControl;
    u32 AddrL_rsvd_DataID;        // bottom 8 bits = data_id
    u32 AddrH;
    u32 RemainingPageSize_Status;
};
```

A descriptor list is power-of-2 in length (up to `MAX_SG_DESCS_COUNT =
65536`), circular, with a modulo-mask at `desc_count - 1`
(`hailo-vdma-common.h:50`). The driver programs descriptors with
`hailo_vdma_program_descriptors_list()` (vdma_common.c:338).

### 5.4. Doorbell (submit) and completion

**Submit** = update `num_avail`: `hailo_vdma_set_num_avail()`
(`hailo-vdma-common.c:426`) writes the new available descriptor count into
the channel's `CHANNEL_NUM_AVAIL_OFFSET` via a read-modify-write. The
device's DMA engine picks up and consumes descriptors until `num_proc`
catches up with `num_avail`.

**Completion** = MSI on the channel bit in
`BCS_SOURCE_INTERRUPT_PER_CHANNEL` or
`BCS_DESTINATION_INTERRUPT_PER_CHANNEL`. `hailo_pcie_read_interrupt()`
(`hailo-pcie-common.c:440`) does the full ISR path:

1. Read-and-clear `BCS_ISTATUS_HOST`.
2. If bit in `VDMA_SRC_IRQ_MASK` (0x000000FF) set → read
   `BCS_SOURCE_INTERRUPT_PER_CHANNEL`; OR into `vdma_channels_bitmap`.
3. If bit in `VDMA_DEST_IRQ_MASK` (0x0000FF00) set → read
   `BCS_DESTINATION_INTERRUPT_PER_CHANNEL`; OR into bitmap.
4. High byte (`SW_IRQ_MASK = 0xFF000000`) decodes to FW-control /
   FW-notification / driver-down events.

Both `BCS_*_INTERRUPT_PER_CHANNEL` registers are **write-1-to-clear**, and
the main `BCS_ISTATUS_HOST` is **read-and-write-same** to clear
(`hailo-pcie-common.c:433-437` `read_and_clear_reg()`).

---

## 6. Inference submit/complete

Source: split between driver (`hailort-drivers`, sets up VDMA rings) and
userspace runtime (`hailort/libhailort`, produces the per-inference
descriptor batch). The driver itself does not know about "inference" — it
knows only about **transfers** on VDMA channels.

From the kernel-driver point of view:

1. Userspace (or, in SLM-OS, the Hailo kernel module) allocates input and
   output buffers and maps them via
   `HAILO_VDMA_BUFFER_MAP` (ioctl enum in `hailo-ioctl-common.h`).
2. Each buffer is bound to a VDMA channel via
   `hailo_vdma_program_descriptors_list()`: writes N descriptors into the
   ring, each pointing at a scatter-gather chunk of the buffer.
3. The last descriptor of each transfer gets the
   `INTERRUPTS_DOMAIN_HOST` bit set
   (`hailo-vdma-common.c:203-223` `get_interrupts_bitmask()`,
   `hailo-vdma-common.h:139-144`), so completion writes a bit into the
   per-channel IRQ register.
4. `hailo_vdma_launch_transfer()` (`hailo-vdma-common.c:438`) bumps
   `num_avail` — **that's the submit doorbell**.
5. Device DMA engine advances `num_proc` as it consumes descriptors;
   last descriptor raises the IRQ.
6. Host ISR (§5.4) matches the channel bitmap and wakes the waiter.

A single "inference" at the userspace layer corresponds to ~1 transfer on
each input channel and ~1 transfer on each output channel, plus per-layer
context-switch descriptors embedded in the `.hef` that get pushed on a
third (control) channel. Batch inference is implemented by submitting
multiple transfers back-to-back with
`hailo_vdma_program_descriptors_list_batch()` (`hailo-vdma-common.c:356`).

**SLM-OS simplification for Phase 5:** a synchronous `hailo infer`
shell command only needs to:

- Submit one H2D transfer on input channel X with the input tensor.
- Submit one D2H transfer on output channel Y with the output buffer.
- Wait on a single completion IRQ (or poll `CHANNEL_NUM_PROC_OFFSET` if
  MSI is deferred).

That matches the "simple inference" path and is what `hailortcli run`
exercises end-to-end.

---

## 7. `.hef` (Hailo Executable Format)

Source: `docs/reference/hailo-hef-internal.hpp` (structure definitions,
639 lines), `hailo-hef-parser-head.cpp` (partial fetch of `hef.cpp`,
1229 lines; the full file is 224796 bytes and was not cached).

### 7.1. Outer header

(`hailo-hef-internal.hpp:116-158`, packed, **big-endian** on disk)

```c
#pragma pack(push, 1)
typedef struct {
    uint32_t magic;                   // 0x01484546 ("HEF\x01" in LE)
    uint32_t version;                 // 0..3
    uint32_t hef_proto_size;          // size of the protobuf body
    hef__header_distinct_t distinct;  // version-dependent tail
} hef__header_t;
```

`magic = 0x01484546` (`HEADER_MAGIC` in `hef_internal.hpp:180`). File
bytes are in network byte order for the common portion —
`parse_hef_header_before_distinct()` at `hef.cpp:479` does
`htonl()` on `magic`, `version`, `hef_proto_size`.

Version-distinct tail (`hef_internal.hpp:119-144`):

| Ver | Fields                                                                          |
|-----|---------------------------------------------------------------------------------|
| 0   | `u32 reserved; u8[16] expected_md5`                                             |
| 1   | `u32 crc; u64 ccws_size; u32 reserved`                                          |
| 2   | `u64 xxh3_64bits; u64 ccws_size; u64 reserved1; u64 reserved2`                  |
| 3   | `u64 xxh3_64bits; u64 ccws_size_with_padding; u32 hef_padding_size; u64 additional_info_size; u64 proto_xxh3_64bits; u64 reserved1` |

Current Hailo Model Zoo models compiled with Dataflow Compiler v5.x emit
**v3** by default. SLM-OS must handle at least v3.

### 7.2. Body

After the header is a **protobuf** blob of length `hef_proto_size`,
parsed via `google::protobuf::io::ZeroCopyInputStream`
(`hef.cpp:55` includes `<google/protobuf/io/zero_copy_stream_impl.h>`).
The top-level message is `ProtoHEFHef` (from the `.proto` files under
`hailort/hrpc/proto/`, not fetched here).

Relevant protobuf sub-messages referenced in
`hef_internal.hpp`:

- `ProtoHEFNetworkGroupMetadata` — top-level "network group" descriptor
- `ProtoHEFPreliminaryConfig` — initial register/context programming
- `ProtoHEFContext` — per-context switch config (a model may have many)
- `ProtoHEFEdgeLayer` — input/output stream edges; direction
  (H2D / D2H), connection type (BOUNDARY / INTERMEDIATE), layer type
  (INFO / MUX / PLANES)
- `ProtoHEFFusedLayersMetadata` — optional post-processing (NMS, argmax,
  softmax) fused into the model

### 7.3. CCWS block (Compressed Config Words)

v1+ `.hef` files have a `ccws_size` (or `ccws_size_with_padding` in v3)
region **after** the proto. CCWS contains the actual **weights** and
per-layer configuration words. Default is 4 KB aligned (`hef_padding_size`
absorbs the alignment slack).

The driver forwards CCWS to firmware during model-load — firmware unpacks
and DMAs it into the on-chip SRAM. The host never interprets CCWS; it's
opaque to everything except the Hailo compiler + firmware.

### 7.4. SLM-OS parser strategy

Writing a protobuf parser in bare-metal C is **not** the right path.
Three options, in rough order of preference for the Phase 4/5 capstone
target:

1. **Workstation pre-parse + flat binary.** Run the Hailo compiler output
   through a small Python helper on the dev workstation that extracts
   exactly the fields SLM-OS needs (boundary layer directions, tensor
   shapes, channel indices, CCWS offset/size). Emit a SLM-OS-specific
   flat C struct alongside the `.hef`. SLM-OS reads both at load time.
   Keeps the kernel simple; downside is a per-model offline step.
2. **Embed nanopb.** ~1500 lines of portable C; could be added under
   `kernel/lib/nanopb/`. Would let SLM-OS parse raw `.hef` directly.
   Heavier, and the proto schema evolves (`hef_internal.hpp:202-238` shows
   30+ extensions across compiler releases).
3. **Treat `.hef` as fully opaque** and forward to firmware via the
   FW control channel. The firmware would need a `LOAD_HEF` control
   message and enough SRAM to hold the whole `.hef`. Not supported by
   current firmware.

**Recommendation for Phase 0 → 5:** option 1. Build the Python helper in
Phase 4 alongside the first `.hef` compile.

---

## 8. Register-map cheat-sheet (one page)

```
BAR0 (config)
  +0x0098  VENDOR_ID              r/o  expect 0x1E60
  +0x0188  BSC_IMASK_HOST         r/w  interrupt enable
  +0x018C  BCS_ISTATUS_HOST       r/wc [31:24] sw, [15:8] D2H, [7:0] H2D
  +0x0400  SOURCE_INT_PER_CH      r/wc per-channel H2D completion bits
  +0x0500  DEST_INT_PER_CH        r/wc per-channel D2H completion bits
  +0x0640  FW response area       (in BAR4 actually; see below)
  +0x0700  ATR[0]                 20 B, param/src/trsl_lo/trsl_hi/trsl_param
  +0x0720  ATR[1]                 ATR[1].trsl_lo == 0x200000 ⇔ FW loaded
  +0x0740  ATR[2], +0x0760 ATR[3]

BAR2 (vdma)
  per channel i, base = (i << 5):
  +0x00 CTRL    [1:0] start/abort/pause/resume
  +0x01 DEPTH_ID [7:5] desc depth, [4:0] data_id
  +0x02 NUM_AVAIL (u16, doorbell — write advances)
  +0x04 NUM_PROC  (u16, device-side completion count)
  +0x08 ERROR

BAR4 (fw_access, SoC SRAM window through ATR[0])
  +0x0000 control request buffer  (md5 | len | payload)
  +0x0640 control response        (md5 | len | payload)
  +0x1684 FW doorbell (Hailo-8 raise_ready_offset)
  FW addresses accessed via write_memory() (ATR[0] retargets the BAR4
  window to arbitrary device phys addresses).
```

---

## 9. Gotchas

### 9.1. Hailo-8 vs master-branch driver divergence

As of 2026-04 the `master` branch of `hailort-drivers` has **removed Hailo-8
support** — only Hailo-10H/15L/Mars remain. The AI HAT+ and AI HAT+ 26 TOPS
both use Hailo-8/8L, so the **`hailo8` branch** is the source of truth. All
file references in this doc are to that branch. When upstreaming from a
fresh clone, explicitly check out `hailo8` or the last release tag with
Hailo-8 support (v5.3.0 was the last release to include it in `master`;
post-v5.3 releases dropped it).

### 9.2. Firmware version coupling

Hailo-8 firmware version is a three-tuple `(major, minor, revision)` in
the firmware header (`fw-validation.h:26-30`). The `.hef` compiler emits
models against a **specific firmware ABI**. Mismatched firmware and `.hef`
typically manifests as a boot succeeding but inference returning garbage
or a control-channel timeout. The Dataflow Compiler release notes tie each
compiler version to a minimum firmware version. SLM-OS should ship both
the firmware binary and the `.hef` as a **matched pair** from the same
Hailo SDK release.

The SDK-shipped firmware is `hailo8_fw.bin` — SLM-OS can embed it via
`.incbin` at build time, exactly the same pattern as the scheduler weights
today.

### 9.3. Endianness

- **BAR MMIO access:** little-endian on both ARM64 and x86-64 (PCIe is
  LE; the bridge passes through). All `iowrite32()` calls in the driver
  assume LE.
- **.hef file format:** **big-endian (network byte order)** for the
  outer header. `hef.cpp:485-487, 497-523` all do `htonl()`/`htonll()`.
  The protobuf body is self-describing and endian-agnostic.
- **Firmware header on disk:** LE (raw `u32`s read directly,
  `hailo-fw-validation.c:validate_fw_header`).

### 9.4. Alignment requirements

| Object                     | Alignment      | Source                                |
|----------------------------|----------------|---------------------------------------|
| VDMA descriptor list       | 64 KB          | `VDMA_DESCRIPTOR_LIST_ALIGN` (`vdma_common.h:16`) |
| FW code section            | 4 bytes        | `FW_CODE_SECTION_ALIGNMENT`           |
| ATR table entry            | 32 B stride    | `ATR_PCIE_BRIDGE_OFFSET(i)`           |
| ATR-window write target    | 4 KB           | `ATR_TABLE_SIZE = 0x1000`             |
| `.hef` v3 CCWS start       | 4 KB           | `hef_padding_size` absorbs slack      |
| PCIe config BAR access     | naturally aligned (u8/u16/u32) | PCIe spec    |

### 9.5. ARM64 cache coherency

The Hailo-8 does **not** participate in ARM64 cache coherency — it does
PCIe-side DMA into host RAM with its own coherency domain. Any descriptor
ring, input tensor, or output tensor that lives in cacheable host RAM
must be flushed (for H2D) or invalidated (for D2H) across the DMA
boundary.

On the Pi 5 (BCM2712), SLM-OS already has the `cache_clean_range()` /
`cache_invalidate_range()` helpers used by the Jetson GPU path. The same
pattern applies:

- **Before submit (H2D):** `cache_clean_range(buffer, size)` — flush host
  writes out to RAM.
- **Before read (D2H):** `cache_invalidate_range(buffer, size)` — drop
  stale cache lines so the next read fetches from RAM.

Alternative: allocate all Hailo DMA buffers as **non-cacheable** via PMM's
NC region (Jetson uses one at `0xBDE00000`; Pi 5 will need an equivalent
carveout). That removes the per-submit cache ops but costs CPU-side
throughput when building tensors.

### 9.6. MSI vs MSI-X

The Hailo driver uses plain **MSI** (not MSI-X):
`pci_enable_msi()` at `hailo-pcie.c:951`. A single MSI vector routes all
interrupt sources (VDMA channel completions + SW events) — the ISR reads
`BCS_ISTATUS_HOST` to demultiplex. This simplifies SLM-OS's MSI allocator
path (Phase 1) — only a single MSI vector needs to be wired.

### 9.7. ATR[0] is shared with firmware

Post-boot, firmware uses ATR[0] for its own control traffic. Any
`write_memory()` call that retargets ATR[0] (to do a one-off poke at
device SRAM) must save and restore the ATR[0] value. The driver's
`write_memory()` (`hailo-pcie-common.c:577-602`) does this; SLM-OS must
replicate the save/restore. **This is easy to miss** — corrupting ATR[0]
without restore kills the control channel silently.

### 9.8. `hailo_get_desc_page_size()` / PCIe payload negotiation

`hailo-pcie.c:1212` calls `hailo_get_desc_page_size(pDev, &desc_max_page_size)`
to query the negotiated MaxPayloadSize from the PCIe root complex. This
determines the VDMA descriptor page size. On Pi 5 pcie1, the negotiated
MPS is set by VideoCore firmware during link training (typically 128 B
or 256 B). SLM-OS must read the device's PCIe capability structure and
honor this — otherwise DMA transactions will be malformed on the wire.

### 9.9. Boot requires `dtparam=pciex1` in Pi 5 firmware config

The Pi 5 downstream kernel tree has `pcie1` **disabled by default**
(`rpi-linux-bcm2712-rpi-5-b.dts:173-175` does not set `status=okay` on
pcie1). VideoCore firmware only trains the external link when `config.txt`
has `dtparam=pciex1`. SLM-OS inherits this state — if `pciex1` wasn't
set, the RC registers will be present but the link will be down and
endpoint enumeration will fail. This must be validated in Phase 0
hardware smoke-testing before writing any SLM-OS driver code.

### 9.10. Firmware distribution / licensing

The Hailo firmware binary is **not** GPL'd — it's distributed under a
proprietary license with redistribution rights for use with Hailo
hardware. The Linux driver pulls it via `request_firmware_direct()`
(`hailo-pcie-common.c:393`), which reads from `/lib/firmware/hailo/`. For
SLM-OS the firmware is embedded at build time via `.incbin` with an
accompanying `LICENSE.hailo_firmware` in the repo that quotes Hailo's
redistribution terms (sourced from the Hailo SDK installer).

---

## 10. File inventory (saved under `docs/reference/`)

Hailo driver (branch: `hailo8` of `hailort-drivers`):

- `hailo-pcie.c` — `linux/pcie/src/pcie.c` — Linux entry point, probe,
  IRQ wiring, char-dev fops
- `hailo-pcie.h` — `linux/pcie/src/pcie.h` — `struct hailo_pcie_board` etc.
- `hailo-pcie-common.c` — `common/pcie_common.c` — **primary source**: board
  compat table, ATR, FW load, interrupt demux
- `hailo-pcie-common.h` — `common/pcie_common.h` — BAR indices, device
  IDs, IRQ bitmasks
- `hailo-vdma-common.c` / `hailo-vdma-common.h` — `common/vdma_common.*` —
  VDMA channel/descriptor programming
- `hailo-fw-validation.c` / `hailo-fw-validation.h` — `common/fw_validation.*`
  — firmware header struct + validator
- `hailo-fw-operation.c` / `hailo-fw-operation.h` — `common/fw_operation.*`
  — FW control message framing helpers
- `hailo-nnc.c` / `hailo-nnc.h` — `linux/pcie/src/nnc.{c,h}` — NNC
  accelerator-mode hooks (the relevant codepath for AI HAT+; not SoC mode)
- `hailo-soc.c` — `linux/pcie/src/soc.c` — SoC mode (Hailo-10/15L; not
  used on AI HAT+, kept for reference)
- `hailo-soc-structs.h` — `common/soc_structs.h`
- `hailo-ioctl-common.h` — `common/hailo_ioctl_common.h` — board-type
  enums, struct definitions shared with userspace

Userspace runtime (`hailort`, master branch):

- `hailo-hef-internal.hpp` — `hailort/libhailort/src/hef/hef_internal.hpp`
  — header structs (§7.1) and extension bitmasks
- `hailo-hef-layer-info.hpp` — `hailort/libhailort/src/hef/layer_info.hpp`
- `hailo-hef-parser-head.cpp` — first 60 KB of
  `hailort/libhailort/src/hef/hef.cpp` (full file is 224 KB; rest omitted
  to keep cache small — fetch again for full parse work)
