# FPGA Bitstream Replay Experiment Plan

**Date:** 2026-06-10
**Goal:** Replay the stock firmware's boot-time FPGA configuration upload (0x3B + 115,638 bytes + 0x3A over SPI3) from our firmware, and verify that the scope capture path comes alive (PC0 data-ready arms, SPI3 ADC frames flow).

**Background:** The 115,638-byte blob at `0x08051D19` in stock V1.2.0 — long suspected to be a cal/register table — is the **Gowin FPGA bitstream**, uploaded to the FPGA SRAM at every boot. Evidence: `reverse_engineering/analysis_v120/h2_extracted/h2_is_gowin_bitstream_2c23t_evidence.md`. Our firmware skips this step entirely; the FPGA's non-volatile fallback image handles USART meter traffic but apparently not scope capture. This experiment replaces the obsolete "graduated Region-A-only" plan — **a partial bitstream load fails configuration (per-frame CRC16 + final status check), so replay must be the full stream.**

## Verified facts (2026-06-10 desk check)

- `h2_cal_table.bin` is a byte-exact extraction of the stock binary at file offset `0x51D19` (verified against `archive/2C53T Firmware V1.2.0/APP_2C53T_V1.2.0_251015.bin`).
- No Gowin `.fs` preamble precedes the table in the stock binary — the 2C53T stream genuinely starts at `0x51D19`. FNIRSI's 0x3B channel is not a verbatim `.fs` file (the 2C23T HW4 stream carries the `FF×22 A5 C3` preamble + IDCODE `0x0120681B` = GW1N-2 family; ours omits the header but the frame body format is identical). **Replay exactly what stock sends — do not prepend a `.fs` header.**
- Transfer at 60MHz ≈ 15.4ms raw + polling overhead → expect ~20–50ms total. Negligible boot cost.

## Phase 0 — RESULTS (2026-06-10, desk work complete)

Phase 0 changed the experiment's shape. Findings:

1. **Our firmware already replayed the table at every boot** — `fpga_init()` in `fpga.c` has streamed `fpga_h2_cal_table` (115,638 B, byte-identical to `h2_cal_table.bin`, verified) since ~April. The scope stayed dead anyway — because the framing was wrong.
2. **`SPI3_HANDSHAKE_BYTE_ACCURATE.md` is debunked.** objdump of the raw stock binary proves: the upload loop lives at `0x08026B28` inside master init `FUN_08023A50` (single occurrence in the image — the doc's `FUN_08027a50` attribution is wrong), and the CS polarity is inverted (`[r4,lr]` = `0x40010C10` = GPIOB_BSRR = PB6 HIGH = CS **deassert**; the doc claimed BRR/assert).
3. **Definitive stock sequence** (from `init_function_decompile.txt` 4180–5160, ground-truthed against objdump): every byte full-duplex polled (TXE → write → RXNE → read); each command in its own CS-LOW frame with a CS-HIGH dummy byte between frames:
   `CS↑ 00 | CS↓ 05 00 CS↑ 00 | ~100ms | CS↓ 12 00 CS↑ 00 | ~100ms | CS↓ 15 00 CS↑ 00 | CS↓ 3B <115,638 bytes> CS↑ 00 | CS↓ 3A 00 CS↑ 00 | CS↓ 00 CS↑ 00`
   No warmup bursts; no delay between the 0x15 frame and the 0x3B frame.
4. The old code held CS LOW from before warmup through the bulk upload and sent 0x3A with CS HIGH — to a CS-edge-framed loader (cf. rosenrot00's 2C23T code) that reads as one giant malformed command. Prime suspect for every failed scope-arm attempt to date.
5. **Fix implemented in `fpga.c`** (stock-faithful framed sequence, per-frame RX captured in `fpga.init_hs[0..11]`). Builds clean: 480,216 bytes. NOT gated behind a flag — the upload already ran unconditionally, so the framing fix replaces it in place; old behavior is one `git revert` away.
6. **USART boot-command ordering kept as-is** (after the upload) even though validated stock order is before (Phase 4 at `0x08025D96` < SPI3 phase at `0x08026540`): meter works with the current order, and keeping it makes the framing fix a single-variable experiment. Moving USART cmds first is the next knob if PC0 still doesn't arm.

## Phase 1–2 — BENCH RESULTS (2026-06-10, two rounds — both negative on scope arm)

**Round 1 (CS-framing fix only, USART cmds after upload):**
- Upload runs (115,638/115,638), boot clean, **meter works** (17 data frames in meter mode) — no regression.
- `init_hs[0..11]` all `0xFF` — FPGA silent during 0x05/0x12/0x15 frames.
- PC0 stuck HIGH through scope wake/acqmode/heartbeats/triggered acquisitions, live 60Hz signal on CH1 probe.
- Raw `spi3 read` returns a uniform `0xE3` wall (vs the chronic `0xFF`) — unconfirmed whether new (no recorded baseline; NOTE: `spi3 read` dumps the CH1 RAM buffer filled by acq transactions, while `h2verify` samples the wire directly — the E3 may be an acq-path artifact, not FPGA data).
- Zero echo frames for 2000+ TX commands all session (stock acks every TX with `AA 55`).

**Round 2 (+ USART cmds moved BEFORE SPI3 phase, stock Phase 4 order):**
- No change: same E3 wall, PC0 HIGH, zero echoes. Manual meter polls (0x0009) in scope mode returned no data frames (meter-mode regression check still owed).
- **`spi3 h2verify` (wire-level sampling during a full re-upload): FPGA drives NOTHING — 0/115,638 non-FF responses, FF at start opcode, all block boundaries, Region B start, end opcode, and 16 post-upload reads. MISO idles high. PC0 unchanged before/during/after.**

**Interpretation:** the FPGA's SPI3 slave never responds to anything we send on the config channel, under either ordering. Leading hypotheses for what's still missing:
1. **FPGA reset/config-mode entry** — rosenrot00's working 2C23T loader pulses FPGA RESET before 0x3B (his PC8) and reads ID registers first; the Gowin part may only accept SSPI config in its post-reset window. We never reset the FPGA; its NV design may own the SPI pins and ignore 0x3B. Stock may pulse a reset earlier in master init (Phases 1–2 GPIO writes — recheck `master_init_phase1.c`).
2. Stock's exchange may ALSO read all-FF (TX-only port) and the load takes silently — in which case the scope-arm gap is elsewhere entirely.
3. Electrical/pinmux difference on MISO during config (e.g., FPGA drives a different pin until configured).

**Next session: the fallback is now the main line — SWD-instrument a STOCK boot** (golden-reference workflow, watchdog NOP defeat per commit c68d70e): capture PC0 timeline, whether stock's upload gets non-FF responses, and any FPGA-reset GPIO pulse before the SPI3 phase. Also worth: grep master_init phase 1/2 for GPIO pulses on FPGA-adjacent pins, and ask rosenrot00 how the 2C23T signals config-mode entry.

## Phase 0 — original prep list (superseded by results above)

1. **Re-read the stock transfer code** (`analysis_v120/fpga_h2_spi3_bulk.md`, `spi3_bulk_cal_resolved.md`, `master_init_phase[1-4].c`, `FPGA_BOOT_SEQUENCE.md`) and pin down, exactly:
   - CS (PB6) behavior: held low across the whole stream vs toggled per byte
   - Whether 0x3B/0x3A are sent inside or outside the CS frame
   - Whether stock reads/discards RX during the stream (full-duplex exchange vs TX-only)
   - Any status read or delay after 0x3A (cf. rosenrot00 HW4: status reg read + 0x3A + 100ms settle)
   - Position in the 53-step boot sequence relative to the USART boot commands (0x01–0x08), PC6/PB11 pin states, and SysTick delays
2. **Payload pipeline:** `scripts/extract_fpga_bitstream.py` — carve offset `0x51D19`, length 115,638 from the archived V1.2.0 binary, emit `firmware/build/fpga_bitstream_2c53t.bin`; link into `.rodata` via objcopy (or generated C array à la rosenrot00). +113KB flash; app budget (1008KB) is fine.
3. **Implement `fpga_load_bitstream()`** in `firmware/src/drivers/fpga.c`, replicating the stock sequence byte-for-byte: SPI3 mode 3, /2 (60MHz), software CS PB6, PC6 HIGH. Feed IWDG before and after the stream (transfer ≪ watchdog timeout, but don't start it half-fed).
4. **Gate it** behind `FPGA_BITSTREAM_REPLAY=1` (Makefile flag) or a Settings menu action for the first runs, with an on-screen pass/fail result.

## Phase 1 — Bench, instrumented

- SWD attached (golden-reference workflow, watchdog NOP defeat per commit `c68d70e`); logic analyzer on CS (PB6) + PC0. NOTE: the HiLetgo analyzer (24MHz) **cannot decode 60MHz SPI data** — use it for CS envelope/PC0 timing only. If data-level capture is ever needed, drop our replay clock to /16 (≈7.5MHz) for one run; Gowin config logic tolerates slow clocks, and a slower first run is an acceptable variation if the 60MHz replay fails.
- Boot with replay enabled. Verify: CS envelope duration matches ~115,640 bytes; no watchdog reset; boot completes.
- **Primary observable: PC0.** Compare data-ready behavior before/after the load (SWD watch or analyzer). Stock-side reference: scope bug was localized to the PC0 arm.
- **Regression check:** meter must still work (USART echo frames, live readings). If the load resets FPGA command state, re-run the USART boot sequence (0x01–0x08) after it, per stock ordering.

## Phase 2 — Scope capture attempt

- Run the normal scope start sequence (PB11 HIGH, USART trigger/timebase commands), wait on PC0, read a 1024-byte SPI3 frame.
- Sanity-check data: interleaved CH1/CH2 unsigned 8-bit, baseline ≈ 128−28. Feed the signal generator output into CH1 for a known waveform.

## Phase 3 — Cal side-effects (if Phase 2 succeeds or even if not)

- Retest meter low-Ω (the per-device 0.0304 factor) and DCV >10V. If the autorange logic/cal lives in the uploaded image, the frame[6] rotation instability may resolve — which would also retire the hardcoded band overrides in `meter_data.c`.

## Fallback — if replay doesn't arm PC0

1. Re-verify the stream against a live stock boot: flash stock V1.2.0 via `scripts/iap_flash.py`, attach SWD, breakpoint/instrument the bulk-transfer loop (analyzer is too slow for the data; SWD is the data-accurate path).
2. Check for a code-emitted header: instrument what stock writes to SPI3_DT immediately before the table bytes (possibly opcode framing or per-block CS toggles our decompile reading missed).
3. Compare notes with rosenrot00 (cross-link issue) and Lanchon (#11 — already engaged on the SPI3 capture path).

## Risks

- **NV flash corruption: negligible.** Stock performs this exchange on every boot; it is an SRAM configuration load, not an NV programming sequence (NV writes would wear out the part at one write per boot).
- **Wedged FPGA after a bad load:** power-cycle recovers — the non-volatile image auto-loads on power-up.
- **Meter regression:** mitigated by re-running USART boot commands post-load; worst case, build without `FPGA_BITSTREAM_REPLAY`.
- **Wrong framing (CS/byte order):** Phase 0 step 1 exists precisely to eliminate guessing; if status still fails, retry at slower SPI clock before questioning the payload.

## Success criteria

1. Replay completes without watchdog reset or boot regression; meter unaffected.
2. PC0 data-ready behavior changes post-load (arms during scope acquisition).
3. A 1024-byte SPI3 capture frame contains plausible interleaved ADC data (stretch: clean siggen waveform on CH1).
4. (Bonus) DCV >10V and low-Ω autorange stabilize.

---

## RESOLUTION (2026-06-12) — root cause found via issue #18

maksidze captured a **full stock boot** on a Saleae (stock V1.2.0 patched to a
/64 SPI prescaler at file 0x368F0). Decode:
`reverse_engineering/captures/SPI3_STOCK_BOOT_CAPTURE_ANALYSIS.md`.

**The payload was wrong, not the framing.** The bitstream pointer `0x08051D19`
is a flash address; the APP is linked at 0x08007000, so the file offset is
**0x4AD19**. Our April extraction used the flash address as a file offset —
every replay (Phases 1–2) sent 115,638 bytes of garbage, which the FPGA
correctly rejected. The real stream has the Gowin `.fs` preamble + IDCODE
0x0120681B and matches the capture byte-exactly.

The capture also retired this plan's open hypotheses:
- **H-reset: DEAD.** No reset pulse exists anywhere in a stock boot.
- **H-allFF: CONFIRMED.** Stock's upload also reads MISO all-FF; `h2verify`
  non-FF counting is not a success metric. The real success signals are the
  **0xF8** byte after `3A 00` and the `0x03` status read (`00 01 42 2E 2E`).
- **Phase 2's read format was wrong:** scope data is per-channel
  `04`/`05` + 1025×FF reads (1026 bytes), not one interleaved stream.

Implemented 2026-06-12: corrected `fpga_cal_table.h` (sha256 `5a0e7338…`),
PB11 armed 1ms pre-handshake, post-0x3A status captured, new Step 7c
(600ms → `01 08`,`02 03`,`06 00`,`07 00`,`08 AD` → read `03`).
**Remaining:** bench litmus (PC0 LOW + status bytes), then rewrite
`fpga_acquisition_task` to the 0x04/0x05 format, then Phase 3 (meter cal
side-effects) as originally written.
