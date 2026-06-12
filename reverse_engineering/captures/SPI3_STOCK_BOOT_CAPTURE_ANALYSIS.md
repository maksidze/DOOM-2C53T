# Stock Boot SPI3 Capture — Decoded (2026-06-12)

**Source:** issue #18, maksidze — Saleae Logic capture of a **stock V1.2.0 boot**
on a 2C53T-V1.4, with the firmware patched to a /64 SPI prescaler
(`8A 78` → `05 22` at file offset `0x368F0`) so a 24MHz-class analyzer can
decode the normally-60MHz bus. Device remains fully functional with the patch.

**Files:** `SPI3_2C53T.sal` (original), `export/digital.csv` (8-channel
transition export via Logic 2 automation), `export/win*_{mosi,miso}.bin`
(per-CS-window byte dumps), `export/analysis_full.txt` (full window list).
Decoder: `analyze_capture.py`, UART: `decode_uart.py`, export: `export_sal.py`.

**Channels:** 0=PB5 MOSI, 1=PB6 CS, 2=PB4 MISO, 3=PB3 SCK, 4=PA3 USART2_RX,
5="PA4" (mislabel — dead line; stock TX is PA2, not captured), 6=PC6, 7=PB11.

## Headline: the FPGA mystery is solved

**Our `fpga_cal_table.h` payload was wrong.** The captured upload body
(115,638 bytes inside the 0x3B frame) matches
`APP_2C53T_V1.2.0_251015.bin` at **file offset 0x4AD19** byte-for-byte
(115,638/115,638). The stock APP is **linked at 0x08007000**, so flash
address `0x08051D19` (the pointer in the disassembly) = file offset
`0x51D19 − 0x7000 = 0x4AD19`. The April extraction used the flash address
as a file offset and pulled 115,638 bytes of unrelated data from 0x7000
too far into the image. Every replay experiment since then sent the FPGA a
garbage config — which it correctly rejected. This also explains why "our"
blob lacked the Gowin `.fs` preamble: the REAL stream begins
`FF×22 A5 C3 06 00 00 00 01 20 68 1B …` — preamble + **IDCODE 0x0120681B**
(GW1N-2 family), exactly like rosenrot00's 2C23T loader.

Corrected stream sha256:
`5a0e73384e496bdb3b3d591b852bec2e806e70cbc71439c9829695324efd5c3b`

## Myths killed by the capture

1. **"FPGA must drive MISO during the upload"** — NO. Stock's bulk window
   shows MISO = 0xFF for all 115,639 clocks. Our `spi3 h2verify`
   "0/115,638 non-FF" was never a failure signal.
2. **"A reset/config-mode pulse precedes the upload"** — NO such pulse on
   any captured line. PC6 is HIGH from before capture start; PB11 rises
   1.0ms before the handshake; nothing else moves.
3. **"Scope data is one interleaved CH1/CH2 stream"** — at least in this
   mode, NO: per-channel reads with opcodes 0x04 (CH1) and 0x05 (CH2),
   1026 bytes per window.

## Full boot timeline (t = capture seconds)

| t | Event |
|---|---|
| 0.000 | capture start: MOSI/CS/MISO/SCK idle HIGH, PC6 HIGH, PB11 LOW, UART lines low |
| 1.383 | USART2 lines come alive (FPGA NV image servicing meter path) |
| 2.772–3.593 | 6 USART RX frames (5A A5 / 5A 69 headers) — meter init traffic |
| 3.6072 | **PB11 → HIGH** (1.0ms before SPI activity; stays HIGH) |
| 3.6082 | bare CS pulse (no clocks) |
| 3.709 | CS↓ `05 00` CS↑ (MISO FF FF) |
| 3.809 | CS↓ `12 00` CS↑ (+100ms) |
| 3.909 | CS↓ `15 00` CS↑ (+100ms), then **8µs** later: |
| 3.9094–4.4483 | CS↓ `3B` + 115,638-byte bitstream CS↑ (MISO all FF) |
| 4.4484 | CS↓ `3A 00` CS↑ — **MISO returns FF F8** (0xF8 = accept status) |
| 4.4484 | CS↓ `00` CS↑ (single-byte flush frame) |
| 5.0553 | **+607ms**, five 2-byte config writes back-to-back (~3µs gaps, no dummy bytes): `01 08`, `02 03`, `06 00`, `07 00`, `08 AD` |
| 5.0554 | CS↓ `03 FF FF FF FF` CS↑ — **MISO `00 01 42 2E 2E`** (status read) |
| 5.0557→end | acquisition loop: alternating `04`+1025×FF / `05`+1025×FF windows (1026 B each, ~29ms cadence, 174 pairs in 5.4s) |
| 10.510 | PB11 1µs glitch (capture artifact?), capture ends |

## Acquisition read format (opcodes 0x04/0x05)

MOSI: opcode + 1025 × 0xFF. MISO: 2–3 status-ish bytes (`00 00 01`,
`80 00 01`, `00 00 00` observed — byte0 bit7 may be a flag), then ~1023
unsigned samples. CH1 (0x04) hovered ~0x4D, CH2 (0x05) drifted
0x70→0xA0 across reads. USART2 is **silent** during scope acquisition.

## Firmware changes made from this capture (2026-06-12)

1. `fpga_cal_table.h` regenerated from file offset 0x4AD19 (byte-exact vs
   capture; header comment carries the sha256).
2. PB11 arm moved to ~1ms **before** the SPI3 handshake.
3. Post-0x3A MISO status byte captured (`fpga.h2_close_status`, expect 0xF8).
4. New Step 7c: 600ms wait, the five config writes, the 0x03 status read
   (`fpga.scope_status[]`, expect `00 01 42 2E`).
5. `status` shell command prints both new diagnostics.

**Bench litmus:** after flashing, `status` should show close=F8 and scope
status `00 01 42 2E`-ish, and PC0 (`gpio read C 0`) should finally go LOW.
Next change if PC0 arms: rewrite `fpga_acquisition_task` to the real
0x04/0x05 read format.
