> **CORRECTION 2026-06-12:** every "our blob differs / has no preamble" observation
> below was an artifact of an extraction bug — the blob was read from FILE offset
> 0x51D19, but the APP is linked at 0x08007000, so flash 0x08051D19 = file offset
> **0x4AD19**. The real stream HAS the Gowin `.fs` preamble + IDCODE 0x0120681B and
> was verified byte-exact against a Saleae capture of a stock boot (issue #18).
> See `reverse_engineering/captures/SPI3_STOCK_BOOT_CAPTURE_ANALYSIS.md`.

# H2 blob reinterpretation: it's the FPGA bitstream (evidence from OpenScope-2C23T)

**Date:** 2026-06-10
**Source:** `rosenrot00/OpenScope-2C23T` (independent custom firmware for the FNIRSI 2C23T sibling device), shallow-cloned to `/tmp/openscope-2c23t` and compared against our extracted `h2_cal_table.bin`.

## Conclusion

The 115,638-byte 0x3B/0x3A SPI3 bulk table at `0x08051D19` is **not a register-init/cal table — it is the Gowin FPGA bitstream**, uploaded to the FPGA's SRAM by the stock firmware **at every boot**. This overturns the "register-init table, not an FPGA bitstream" conclusion in `FINDINGS.md` / `CLAUDE.md`, and most likely explains the dead scope capture path: our firmware never loads the bitstream, so the FPGA runs only its NV-flash image (sufficient for USART meter traffic, evidently not for scope capture / PC0 data-ready arming).

## Evidence

The 2C23T HW4.0 hardware revision shares the same FPGA design generation as our 2C53T. rosenrot00's firmware (which has a **fully working scope**) does the following in `src/fpga.c`:

1. **Same opcodes, same bracket:** `fpga_init_once()` (HW4 path) sends opcode **0x3B**, streams `fpga_bitstream[]`, reads a status register, then sends command **0x3A**00 — exactly our mystery open/close pair.
2. **Same size ±1 byte:** their `fpga_bitstream_hw4.c` array is **115,639 bytes**; our blob is **115,638**.
3. **Identical frame structure:** `FF×6` sentinel spacing histogram is **160 bytes × 542 frames** in theirs vs **160 bytes × 541** in ours (our prior analysis: 544 sync-framed 160-byte blocks in Region A). The 16-bit "tag" we found at bytes 28–29 of each block is consistent with a **Gowin per-frame CRC16**.
4. **Gowin preamble:** their array begins `FF×22, A5 C3, …` — a Gowin `.fs` binary preamble. Ours lacks the preamble head (starts `00 00 04 00 …`), suggesting our extraction began after the header bytes, or the stock code emits the header separately. Worth re-checking the bytes immediately before `0x08051D19`.
5. **Content differs, format matches:** aligned byte match is only ~55% (driven by shared 00/FF padding; high-match windows 0x0C000–0x14000 are padding in both). Different logic design for a different board — same FPGA part (frame size × frame count fixed per die).
6. **Pre-HW4 corroboration:** the older 2C23T hardware loads a **46,808-byte** bitstream raw over SPI3 + 200 dummy bytes, then checks an FPGA "done" pin (PC8). Loading the bitstream at boot is FNIRSI's standard architecture across device generations.

## Their capture flow (working reference, pre-HW4 SPI variant)

`scope.c` + `fpga.c`: `fpga_capture_latch()` (PC0 HIGH + PC2 strobe pulse, PC0 LOW) → poll `fpga_capture_ready()` (**PC3 input** = FPGA capture-done output) → `fpga_capture_read()` (per-byte CS framing on PA15, TXE/BSY/RXNE with timeouts; on timeout, re-latch and retry). On HW4: ready pin is PB4, data over an 8-bit parallel bus on GPIOC[7:0]. Pin roles differ per board — the transferable pattern is *bitstream upload at boot is mandatory; ready pin is an FPGA output that arms only after configuration*.

## Implications for the replay experiment

- The graduated "Region A only → full table" plan should be **revised: a partial bitstream load would fail configuration (per-frame CRC + final status check)**. Replay must be the full stream, bracketed 0x3B … 0x3A, matching stock SPI mode/speed, followed by the stock status read.
- Check stock disassembly for a preamble/header emitted before the table bytes (see point 4).
- After a successful load, expect PC0 data-ready behavior to change — retest scope capture immediately.
- The "missing factory calibration" framing for low-Ω/DCV>10V may also resolve: the meter IC's auto-range stability may depend on FPGA-side logic present only in the uploaded image.
