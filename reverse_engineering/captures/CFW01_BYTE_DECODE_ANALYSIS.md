# CFW Boot Capture — BYTE-LEVEL decode (2026-06-13)

**Source:** `SPI3_2C53T_CFW_01.zip` (maksidze, issue #18) — Saleae capture of
**our latest `experiment/fpga-config-capture` firmware** booting, with
maksidze's low-frequency `fpga.c` (all SPI slowed for the analyzer). Crucially
this drop includes **`digital.csv`** (62 MB, transition-logged) — the
byte-decodable export the earlier `.sal` could not give us. This supersedes
`CFW_BOOT_CAPTURE_ANALYSIS.md` (which had byte values unrecoverable).

**Channels:** `PB5 MOSI, PB6 CS, PB4 MISO, PB3 SCK, PA3 USART2_RX, PA2 USART2_TX,
PC6 FPGA-SPI-enable, PB11 FPGA-active-mode`. SPI Mode 3, MSB-first, 8-bit.
Decoder: `/tmp/mak/decode.py` (CS-framed, sample MOSI/MISO on SCK rising edge).
12.459 s, 2,153,955 transitions, 34 CS-framed SPI frames.

---

## The decisive result: config-entry DEFINITIVELY fails

Every byte we couldn't read before is now readable. The bitstream MOSI is
byte-correct, and **the FPGA never drives MISO at a single protocol checkpoint**:

| Frame | t (+ms) | MOSI | MISO (ours) | MISO (stock #18) | Verdict |
|---|---:|---|---|---|---|
| 0 | 472 | (bare CS pulse) | — | — | prelude |
| 1 | 4474 | (bare CS pulse) | — | — | prelude |
| 2 | 4574 | `05 00` | `FF FF` | (config-wait) | — |
| 3 | 4674 | `12 00` | `FF FF` | — | — |
| 4 | 4774 | `15 00 3B` + **115,638 B bitstream** | all `FF` (0/115641 non-FF) | all `FF` | upload OK on the wire |
| **5** | **5268** | **`3A 00 00`** (close) | **`FF FF FF`** | **`F8`** | **★ CONFIG FAILED** |
| 6 | 5868 | `01 08` | `FF FF` | — | scope cfg write |
| 7 | 5868 | `02 03 06 00 07 00 08 AD` | all `FF` | — | scope cfg writes |
| **8** | **5868** | **`03` status read** | **`FF FF FF FF FF`** | **`00 01 42 2E 2E`** | **★ FPGA mute** |
| 10+ | 9832→ | `88` + 1025/2049 B reads | all `FF` | (0x04/0x05 1026 B) | scope data (empty) |

**The payload was never the problem.** Bitstream bytes match stock; the FPGA
simply isn't listening. `0x3A` close returns `0xFF` not `0xF8`; the `0x03`
status read returns all-`FF` instead of stock's `00 01 42 2E 2E`. MISO floats
high (FPGA high-Z) through the entire exchange.

---

## Our boot-silence fixes are CONFIRMED on the wire ✓

The deviations flagged in the older-capture analysis are **gone** in this build:

* **USART2 TX (PA2): ZERO edges before/during the upload.** First TX edge at
  **+5918 ms** — *after* the whole config attempt. Matches stock's "no USART at
  boot" exactly. The pre-upload-USART deviation is fixed.
* **PB11 (active mode): HIGH throughout the upload window** (steady `=1` across
  frames 2–5). Armed before the handshake, as stock does.

So the two firmware fixes we made (delete pre-upload USART, arm PB11 early) are
real and verified. **Config still fails anyway** — proving those deviations were
not the root cause.

---

## The smoking gun: the FPGA is in USER MODE, not config-wait

Two independent signals show the FPGA is **running its NV meter design**, deaf
to SSPI, the entire time:

1. **USART2 RX (PA3→MCU): only 2 edges in 12.5 s — essentially dead.**
   Stock's FPGA UART comes alive at ~1.38 s and emits `5A A5`/announce frames.
   Ours emits nothing. The FPGA never reaches stock's responsive state.

2. **PC6 carries a 142.8 Hz heartbeat (464 µs low / 7.00 ms period), present
   from +0.09 ms — before any SPI — and rock-regular across all 12.5 s.**
   It dips LOW **83 times *during* the 493 ms bitstream upload** while our
   firmware holds it push-pull HIGH.

   **No MCU code accounts for this.** A tree-wide sweep of every `GPIOC` write
   (and all four IRQ handlers: USART2, SPI3, USB, TMR3) shows the only code that
   clears PC6 is `fpga_scanner.c` — a manual diagnostic that is **not** invoked
   at boot. PC6 is configured as a plain push-pull output (no timer AF). A pin
   we drive HIGH that still gets pulled LOW at a fixed 142.8 Hz ⇒ **the FPGA is
   driving the PC6 net.** Stock's PC6 is steady HIGH (FPGA not driving it).

   142.8 Hz is consistent with an FPGA-side heartbeat from its running NV
   configuration (or a config-state output) — i.e. the FPGA booted its internal
   flash image and is *running it*, not sitting in SSPI config-wait.

This is the cleanest confirmation yet of the long-standing hypothesis
(commit 36653d1, "FPGA already in user mode, not config-wait"): **by the time we
send `0x3B`, the GW1N has already auto-loaded its NV bitstream and is executing
it.** Its SSPI configuration port is closed, so it ignores our upload and
returns high-Z (`0xFF`) at every checkpoint.

---

## What this means for the path forward

The config-entry trigger is **not on any of the 8 captured lines** (SPI, USART,
PC6, PB11 all match stock or are now stock-faithful, and config still fails).
Stock must flip the FPGA into config-wait via a signal we don't capture and may
not even control from the MCU:

* **RECONFIG_N / MODE pins (M0–M3) / the 5 unlabeled gold pads** — a hardware
  config-mode strap or reconfig pulse. If it's on the gold pads (not an MCU
  GPIO), firmware *cannot* reproduce it; only JTAG or a hardware mod can.
* **A power-up timing race** — the GW1N may sit briefly in config-wait before
  auto-loading NV; stock (Keil, faster boot) might catch that window. But this
  capture shows the 142.8 Hz heartbeat already active at +0.09 ms, so we've
  missed any such window before capture start. Racing the FPGA's autoload from
  our slower FreeRTOS boot looks impractical.

**Both point at the FT232H + openFPGALoader JTAG path** (the hardware on order)
as the way in: JTAG SRAM-load bypasses the SSPI/MCU handshake entirely. The 5
gold pads (continuity-trace to FPGA pins 8/9/10/11 = TMS/TCK/TDI/TDO per the
QN48 pinout) are the prime target.

### Secondary observations (for later, post-config)
* Bulk frame merges `15 00` into the same CS-low transaction as `3B`+bitstream
  (intended sequence had `15 00` as its own CS frame). maksidze's low-freq build
  may have merged them; harmless to config (FPGA mute regardless), but worth
  re-checking framing parity once config works.
* Our scope data-read opcode is `0x88` (`0x80 | range`) with 1025/2049-byte
  bulk reads; stock uses per-channel `0x04`(CH1)/`0x05`(CH2) with 1026-byte
  reads. A real deviation in the acquisition path — irrelevant until config
  succeeds, then must be aligned to stock.

---

## Decoder

`scripts/decode_spi_csv.py` reproduces the full frame table and pin stats from
a Logic 2 CSV export (`File -> Export Data -> CSV`) with the 8-channel layout
noted above:

```
python3 scripts/decode_spi_csv.py digital.csv              # SPI frame table
python3 scripts/decode_spi_csv.py digital.csv --pin-stats  # PC6/PB11/USART
```

The `--pin-stats` run on this capture prints `PC6: ~142.8 Hz`, `PB11: steady`,
`USART_TX: first +5918ms`, `USART_RX: 2 edges` — the user-mode/dead-UART
signature described above.
