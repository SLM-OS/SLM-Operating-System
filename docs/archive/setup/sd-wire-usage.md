# SD-Wire Usage Guide

The SD-Wire device allows an SD card to be accessed or re-programmed without removing it from the device under test.

## Basic Usage

1. Disconnect power supply from the DUT (Device Under Test).

2. Connect micro SD card to the TS (Test Server) using card reader:
   ```
   sudo sd-mux-ctrl --device-serial=sd-wire_1 --ts
   ```

3. Flash the SD card using bmaptool or access it like a normal mass storage device

4. Connect SD card to the DUT:
   ```
   sudo sd-mux-ctrl --device-serial=sd-wire_1 --dut
   ```

5. Connect power supply to the DUT.

6. Boot DUT from new image.

## Platform Compatibility

| Platform | SD-Wire Compatible | Notes |
|----------|-------------------|-------|
| Jetson Orin Nano | Yes | Works reliably |
| Raspberry Pi 5 | **No** | See below |
| Other boards | Varies | Test before relying on it |

### Raspberry Pi 5 Incompatibility

**The SD-Wire is NOT compatible with Raspberry Pi 5.**

Testing (January 2026) confirmed that Pi 5 fails to boot when the SD card is routed through the SD-Wire, even with:
- Slow Class 4 SD cards
- Various SD card brands
- Different SD-Wire connection modes

The Pi 5's faster SD interface (SDR104 mode) appears to be incompatible with the SD-Wire's signal integrity. The Pi 5 shows no video output and fails to reach the bootloader when SD-Wire is in the path.

**Workaround:** Manually swap SD cards for Pi 5 development, or use a newer SD-Wire Pro device (untested).

---

*Last updated: January 2026*
