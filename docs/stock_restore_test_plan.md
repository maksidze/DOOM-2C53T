# Stock Firmware Restore — Test Plan

**Goal:** Flash the original FNIRSI 2C53T V1.2.0 firmware back onto the dev unit. Verify all stock functions work. This unblocks issue #9 (4pda testers want a safety net) AND tells us whether the hardware is intact.

## Why this matters

We've never confirmed the oscilloscope, meter, or signal generator actually work on this physical unit — we flashed our firmware before testing stock. If stock works fully, every remaining bug is provably a software/protocol-decoding problem (the answer is in the binary we have). If it doesn't, we learn whether we damaged the hardware during experimentation.

## Procedure

1. **Open case, attach BOOT0 jumper to 3V3**
2. **Hold POWER button** (don't release until step 6 — PC9 power hold is not asserted in ROM DFU)
3. **Pinhole reset** while holding POWER
4. **Verify ROM DFU enumeration:**
   ```
   dfu-util -l
   ```
   Expect: `Found DFU: [2e3c:df11] ... alt 0, name "@Internal Flash ..."`
5. **Flash stock firmware:**
   ```
   dfu-util -a 0 -d 2e3c:df11 -s 0x08000000:leave \
     -D "archive/2C53T Firmware V1.2.0/APP_2C53T_V1.2.0_251015.bin"
   ```
6. **Release POWER, remove BOOT0 jumper, close case** (or leave open for follow-on testing)
7. Power on normally — should boot stock UI

**Do not change option bytes.** Stock firmware also requires EOPB0=0xFE (its SP=0x20036F90 is in the 224KB SRAM range). Our existing setting is compatible.

## Verification checklist

Walk through each function with bench equipment:

- [ ] Boots to stock splash + scope screen
- [ ] All 15 buttons respond (cycle through modes, navigate menus)
- [ ] **Scope mode** — connect probe to signal generator output, see waveform; verify timebase + V/div controls work
- [ ] **Meter mode (DCV)** — measure AAA battery, expect ~1.5V; measure 9V battery, expect ~9V
- [ ] **Meter mode (resistance)** — measure known resistor (e.g., 10kΩ)
- [ ] **Meter mode (continuity)** — short probes, expect beep
- [ ] **Signal generator** — output sine wave, scope on stock unit OR external scope
- [ ] **USB Mass Storage** — plug into computer, screenshot/file browser appears

## Outcome decision tree

**A. Everything works** → 🎉 Hardware intact. FPGA bitstream intact. SPI flash cal intact. **The MCU↔FPGA protocol is 100% in the binary; we just haven't decoded it yet.** No need for second unit. Continue decompilation work with confidence. Document the restore procedure → reply to issue #9.

**B. Partial — scope works, meter doesn't (or vice versa)** → That subsystem is hardware-damaged. Focus efforts on the working path; consider second unit only for the broken path. Still document restore procedure for issue #9.

**C. Nothing works / device behaves erratically** → Likely damage from our GPIO experimentation, OR corrupted SPI flash calibration. Diagnostic options:
  1. Check SPI flash contents (W25Q128) against any backup
  2. Inspect board for visible damage (relay coils, gain resistor traces)
  3. Buy second unit for clean baseline
  Even in this case, our custom firmware still flashes fine afterward — no work is lost.

## Risk assessment

**Risk: low/none.**
- Stock binary cleanly overwrites our bootloader + app (751KB stock spans 0x08000000–0x080B7600)
- Gowin FPGA is non-volatile and only reprogrammed via M0-M3 pins — nothing we did via SPI3 touches the bitstream
- We can always reflash our firmware via `make flash-all` after the test

## Follow-up after test

1. Take screenshots / video of stock UI working (or not)
2. Write `docs/restore_stock_firmware.md` with the procedure + photos
3. Reply to issue #9 with the doc link — gives 4pda testers a safety net
4. If stock works fully: update `community.md` and pulse log noting the milestone
5. Reflash our firmware: `make flash-all`
6. Continue meter calibration / decompilation work with confirmed hardware baseline
