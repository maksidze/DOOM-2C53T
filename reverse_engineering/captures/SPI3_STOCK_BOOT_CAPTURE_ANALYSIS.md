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

---

## Bench results with corrected bitstream (2026-06-12, Unit 2)

**Progress — FPGA slave is now ALIVE.** Before the fix MISO floated at 0xFF
(slave inert, `h2verify` 0/115,638 non-FF). After: the FPGA drives MISO to
0x00 during the upload (`h2verify` **115,638/115,638 non-FF**), the `0x3B`
opcode returns `0x80`, the bank bit toggles between `0x04`/`0x05` reads, and
**PC0 arms (=0)** — none of which ever happened on garbage uploads.

**But not yet at stock's configured state.** Two gaps remain:

| Signal | Stock | Ours | Meaning |
|---|---|---|---|
| `0x03` status read | `00 01 42 2E` | `80 00 00 00` | config-state regs (`01 42 2E`) not set |
| acq read byte[2] | `01` (buffer valid) | `00` (empty) | no samples captured |
| `0x3A` close status | `F8` (every boot) | `FC` / `00` (varies) | config result inconsistent |
| samples | live ADC values | all `0x00` | sampling engine not running |

**Leading hypothesis: the 115KB upload is marginal, not the framing.** The
slave activates but the config doesn't reliably reach the "ready" state — the
boot-to-boot `0x3A` variance (F8 vs FC vs 00) is the tell. Our `spi3_pump`
clocks the bitstream gaplessly at /2 (60MHz); a signal-integrity or timing
issue over 115K gapless bytes would corrupt enough frames to fail the Gowin
per-frame CRC while still activating the slave.

**Next experiments (priority order):**
1. **Slow the upload clock** (/8 or /16 just for the `0x3B` bulk phase) and
   check whether `0x3A` close reliably returns `F8`. If a slower clock fixes
   it → timing/SI on the gapless pump. Cheap, high-value, do first.
2. If close hits F8 but status still `00 00`: the `01 42 2E` state needs a
   command we haven't replayed — instrument what sets it (more SPI3 writes,
   or the USART trigger/run sequence the capture couldn't see on PA2).
3. Only once acq byte[2]=`01`: feed siggen→CH1 and confirm `span>0`.

Tool: `spi3 acqread` (per-channel 0x04/0x05 read + sample stats) and
`spi3 xfer 03 ff ff ff ff` (status reg) added to the debug shell for this.

### Experiment 1 result: upload clock is NOT the bottleneck (NEGATIVE)

Slowed the 0x3B bulk phase to /16 (~7.5MHz, vs /2=60MHz). Result on Unit 2:
**identical** — `0x3A` close still `00`, `0x03` status still `80 00 00`, acq
buffers still all-zero. So our gapless pump was never the marginal link.
Reverted to /2.

**Revised hypothesis:** the FPGA's SPI slave activates (drives MISO, bank bit
toggles, PC0 arms) but the config never reaches "user mode / ready" — close
returns `00` not `F8`. Since the upload DATA is byte-exact and clock rate
doesn't matter, the gap is most likely in the **config-enter handshake**: the
`05 00` / `12 00` / `15 00` prelude (rosenrot00's 2C23T loader reads regs
0x11/0x13/0x41 and writes 0x1200/0x1500 to put the FPGA into SRAM-reconfig
mode) or a trailing requirement we're missing. Our NV-booted FPGA may already
be running its NV config and ignoring/partially-applying the 0x3B SRAM stream
unless correctly told to enter reconfiguration.

**Better tooling needed first:** each idea currently costs a full reflash +
IAP cycle. Next session — add a `fpga reinit [br]` shell command that re-runs
the prelude + 0x3B upload + 0x3A close + config writes on demand and reports
close/status, so the handshake can be swept in seconds instead of minutes.
Then sweep: prelude variants, pre-upload register reads (0x11/0x13/0x41),
trailing dummy clocks, and CTRL2 DMA-bit on/off.

### Experiment 2+3 results: reset pulse & register reads (NEGATIVE / informative)

Built `fpga reinit [br] [gap] [close] [pin]` for live sweeping. Findings:

- **Reset-pulse sweep** (rosenrot00's 2C23T pulses an FPGA RESET before config):
  pulsed PB9, PA6, PD6 LOW 10ms → HIGH before the handshake. **No change** —
  close still 00, buffers empty. (Other pins untested; the 2C53T reset line,
  if any, is unknown.)
- **Gowin SSPI register reads** (`spi3 xfer 11/13/41 00 00 00 00 00 00 00`,
  rosenrot's read framing): all return `80 00 00 00 00 00 00 00` — **no IDCODE
  on 0x11, no status on 0x41**. The 2C53T stock never issues these reads
  (capture shows only 05/12/15), so the FPGA may simply not expose them; but it
  also means we have no positive confirmation the SSPI read path works.
- **Control pins all correct**: PC6=1 (SPI en), PB11=1 (active), PB6=1 (CS idle
  high), PC0=0 (armed). Static config matches stock.

**Sharpest clue — MISO during the upload differs from stock:**

| Phase | Stock MISO | Our MISO |
|---|---|---|
| 0x3B bitstream upload | **0xFF** (FPGA high-Z, receiving) | **0x00** (FPGA driving low) |
| idle / 0x03 status read byte0 | 0x00 | **0x80** (stuck bank bit) |

During config-data load a Gowin device should NOT drive MISO — stock's 0xFF
confirms it's in receive mode. **Our FPGA drives MISO LOW through the whole
upload**, i.e. it never entered config-receive mode, so our byte-exact 0x3B +
bitstream is being interpreted as something else and the 0x3A close returns 00.
The stuck 0x80 bank bit on idle reads (stock toggles 00/80) points the same way:
the FPGA is in a fixed non-config state.

**Conclusion:** pins ✓, bytes ✓, timing ✓ — the gap is the **config-mode
entry**. The 05/12/15 prelude is supposed to put the GW1N into SSPI
SRAM-program mode; on our device it doesn't take. Next leads:
1. Decode the 05/12/15 prelude MISO responses from the issue-#18 capture at the
   bit level (the /64 capture has them) — compare to ours byte-for-byte.
2. Look up the GW1N-2 SSPI config command sequence (Gowin UG290 / SUG100):
   confirm whether 0x05/0x12/0x15 is the full "enable + erase + program" enter
   sequence or whether a command (e.g. config-enable 0x15 BEFORE erase, or a
   specific order/dummy-clock count) is missing/misordered on our side.
3. Ask rosenrot00 how the GW1N enters SRAM-program mode without a reset on a
   NV-booted design (their 2C23T pulses reset; the 2C53T apparently doesn't).
