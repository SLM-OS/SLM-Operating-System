# Captured ctxsmoke wire bytes (SLM-OS, HAILO_WIRE_DEBUG=ON)

Source: `hailo ctxsmoke full` on pi-5-1, fw v4.23.0, 2026-04-24.
All four SET_CONTEXT_INFO bodies + GET_HW_CONSTS response captured
with full byte dumps. ALL eight CS RPCs returned `rc=0` — fw
accepted every byte.

Used by host-tools/hailo-ushim's planned `--full-handshake` extension
to replay SLM-OS's exact CS handshake against hailo_pci, with the
IOVA fields patched at runtime to match buffers allocated through
HAILO_DESC_LIST_CREATE.

## ctxsmoke IOVAs (the values embedded below)

| Tag | IOVA |
|---|---|
| ccw | `0x1000ab0000` |
| boundary_input | `0x1000ae0000` |
| boundary_output | `0x1000af0000` |

## SET_NETWORK_GROUP_HEADER

Result: rc=0 (fw accepted). Body bytes not dumped — the request
is built by `hailo_control_set_network_group_header` and is
HEF-derived; for ctxsmoke it's a synthetic 1-network-group header.
Probe would build this from the ctxsmoke-equivalent struct.

## SET_CONTEXT_INFO bodies

### ACTIVATION (63 bytes, rc=0)

```
1e ff ff ff ff 21 ff ff ff ff 10 00 00 00 af 00
10 00 00 00 00 10 40 00 00 00 00 01 00 00 20 ff
ff ff ff 02 00 00 00 ae 00 10 00 00 00 00 10 40
00 00 00 00 01 00 00 01 00 00 01 00 01 00 00
```

Action breakdown:
- `[00..04]` BURST_CREDITS_TASK_RESET (type=0x1e, time_stamp=INIT)
- `[05..29]` OpenBoundaryOutput (type=0x21, time_stamp=INIT, body 20 B)
  - `[10]` packed_vdma_channel_id (0x10 = engine 1 + ch 0)
  - **`[11..18]` dma_address LE u64 = `0x1000af0000` ← PATCH bnd_out IOVA**
  - `[19..20]` desc_page_size LE u16 = 0x1000 (4096)
  - `[21..]` initial_credit_size + remaining fields
- `[30..58]` OpenBoundaryInput (type=0x20, time_stamp=INIT, body 28 B)
  - `[35]` packed_vdma_channel_id (0x02)
  - **`[36..43]` dma_address LE u64 = `0x1000ae0000` ← PATCH bnd_in IOVA**
  - `[44..]` page_size + credit + extra input fields

### BATCH_SWITCHING (16 bytes, rc=0)

```
1f ff ff ff ff 25 ff ff ff ff 02 1d ff ff ff ff
```

No IOVAs — pure action sequence. Patch nothing.
- `[00..04]` action 0x1f
- `[05..09]` action 0x25 (with parameter byte 0x02 at offset 10)
- `[11..15]` action 0x1d

### PRELIMINARY (37 bytes, rc=0)

```
16 ff ff ff ff 01 00 00 00 00 ab 00 10 00 00 00
00 02 02 00 00 00 00 00 00 00 18 ff ff ff ff 01
00 00 02 00 01
```

- `[00..04]` action 0x16 (FETCH something)
- `[05..09]` parameters
- **`[10..17]` IOVA LE u64 = `0x10_00ab_0000` ← PATCH ccw IOVA**
   - bytes: `ab 00 10 00 00 00 00 02` → wait: bytes 10..17 = `ab 00 10 00 00 00 00 02`. LE u64 = 0x02_0000_0010_00AB → that has high byte 0x02 set, doesn't match 0x1000ab0000 cleanly. **Need to verify offset by inspecting SLM-OS code or HailoRT trace.**
   - Likely actually `[09..16]` or `[10..17]`. The pattern `ab 00 10 00 00 00` is at bytes 10..15 — that's the bottom 6 bytes of the IOVA. Bytes 16, 17 = `00 02` would be the page_size (LE u16 = 0x0200 = 512) following the IOVA. So dma_address u64 spans only 6 bytes? That's odd for u64. May be u48 or a different field layout.
   - **Verification needed before patching.**

### DYNAMIC (103 bytes, rc=0)

```
07 ff ff ff ff 10 02 00 00 01 01 00 00 01 01 00
00 00 00 01 00 00 00 00 01 00 00 00 af 00 10 00
00 00 00 10 40 00 00 00 00 01 00 00 06 ff ff ff
ff 02 01 00 01 01 00 00 01 01 00 00 00 00 00 00
00 00 00 01 00 00 00 ae 00 10 00 00 00 00 10 40
00 00 00 00 01 00 00 00 00 01 00 27 ff ff ff ff
02 01 15 ff ff ff ff
```

- **bnd_out IOVA at byte ~28: `00 00 00 af 00 10 00 00` ← PATCH bnd_out**
- **bnd_in IOVA at byte ~71: `00 00 00 ae 00 10 00 00` ← PATCH bnd_in**

## GET_HW_CONSTS response (51 bytes — captured for reference)

```
[000]: 00 00 00 13 08 00 00 00 ff ff ff ff 00 a0 00 00
[010]: 01 00 01 00 00 01 00 00 00 00 00 00 00 00 00 00
[020]: 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
[030]: 00 00 00
```

## CHANGE_CONTEXT_SWITCH_STATUS calls

| Position | State | App | BatchSize | BatchCount | rc |
|---|---|---|---|---|---|
| step 1 | RESET (0) | 0xff | 0 | 0 | 0 |
| step ENABLED | ENABLED (1) | 0 | 0 | 0 | 0 |

## Plan for `--full-handshake`

1. Allocate via hailo_pci: 1 CCW desc list, 1 boundary-input desc list, 1 boundary-output desc list. Get IOVAs.
2. Hard-code ACTIVATION/BATCH/PRELIMINARY/DYNAMIC bytes from above.
3. At runtime: patch the IOVA fields with the hailo_pci-allocated IOVAs (precise byte offsets per the breakdown above — TBD: verify PRELIMINARY's IOVA offset by inspecting SLM-OS struct hailo_cs_action_fetch_ccw_bursts or similar).
4. Send full sequence: RESET → CLEAR → HW_CONSTS → SET_NETWORK_GROUP_HEADER → 4× SET_CONTEXT_INFO → ENABLED.
5. Then LAUNCH_TRANSFER on channel 2.

If LAUNCH_TRANSFER succeeds: SLM-OS's bytes are correct end-to-end,
#253 lives in bare-metal bringup (MMIO/cache/IRQ). If it fails:
SLM-OS bytes have a defect that the official driver path also
rejects.
