# CFW Boot SPI3 Capture — Analysis (2026-06-12)

**Source:** `SPI3_2C53T_CFW.sal` — a Saleae Logic 2 capture of **our OpenScope
custom firmware booting** (the OLDER committed build, before the
"no-USART-before-upload" and PB11-arming fixes). 24 MHz sample rate, 8 digital
channels, ~12.5 s long. User marker "Reset" at 0.529–0.739 s.

**Channels (meta.json):** 0=PB5 MOSI, 1=PB6 CS, 2=PB4 MISO, 3=PB3 SCK,
4=PA3 USART2_RX (FPGA→MCU), 5=PA2 USART2_TX (MCU→FPGA; the export still
mislabels it "PA4"), 6=PC6 FPGA-enable, 7=PB11 active-mode. SPI Mode 3,
MSB-first, 8-bit; UART 9600 8N1 LSB-first. Compared throughout against the
stock decode in `SPI3_STOCK_BOOT_CAPTURE_ANALYSIS.md`.

---

## ⚠️ Format status: PARTIALLY cracked — byte VALUES are NOT recoverable

The per-channel `digital-N.bin` files are **Logic 2's internal v3 format**
(magic `<SALEAE>`, version field = 3), **not** the documented 44-byte
fixed-header export. The real layout was reverse-engineered as far as the
**activity/burst structure**, but the dense per-edge stream uses a nested
run-length sub-block encoding that could not be flattened reliably.

### What WAS decoded (validated)
* **Header:** 43 bytes, identical across channels —
  `'<SALEAE>'(8) · u32 version=3 · u32 0x64 · u32 init=1 · window descriptors`.
  (The `init=1` is a copied constant, **not** a trustworthy per-channel idle
  level.)
* **Data section** (from offset 43): `u32 n_spans · u64 0 · u32 0`, then a list
  of **span records**. Each span is anchored by the 2-byte marker `01 00`
  immediately preceded by a **u64 first-edge sample index** (÷24 MHz = seconds).
  These anchors are monotonic and decode cleanly → a reliable **burst-level
  timeline** for every channel (table below).
* **Prelude SCK run** decoded directly: the first clean run on PB3 is
  ~957 fast edges (half-period 8 samples ⇒ **~1.5 MHz**, consistent with the
  firmware's /64 upload prescaler quantized by a 24 MHz analyzer) spanning
  0.5010→0.5063 s, with **33 inter-byte gaps of ~520 samples (~21.7 µs)** —
  i.e. byte-by-byte CS-framed transactions, ≈59 bytes. This is the **prelude**,
  not the gapless bulk.

### What could NOT be decoded
* **The bulk-upload SCK stream and ALL MOSI/MISO/UART byte values.** After the
  first ~1 kB the dense stream switches to a nested run-length sub-block
  encoding (recurring `0x0605 / 0x0506`-style sub-headers mid-stream) that
  desyncs a flat u16-delta walk. Every attempt (zero-skip separators, skip
  sweeps, resync-on-anchor) reproduced the same desync.
* Therefore the report's **most critical questions cannot be answered from these
  bins**:
  * the `05` prelude MISO byte (0x80 user-mode vs 0xFF config-wait),
  * the `0x3A` close status (0xF8 vs 0xFF),
  * MISO during the bulk upload (0xFF vs 0x00),
  * the decoded USART TX command bytes (header/cmd/params/checksum),
  * the `5A A5`/`5A 69` FPGA announce-frame contents,
  * the post-upload `01 08 / 02 03 / 06 00 / 07 00 / 08 AD` writes and the
    `03` status read payload.

**Recommendation: re-export from Logic 2 as the documented binary/CSV** (the
exact path used for stock in `SPI3_STOCK_BOOT_CAPTURE_ANALYSIS.md`: Logic 2
automation → `export/digital.csv` transitions + per-CS-window
`win*_{mosi,miso}.bin`, decoded with `analyze_capture.py` / `decode_uart.py`).
That export *is* byte-decodable; these internal `digital-*.bin` files are not.

---

## Burst-level boot timeline (reliable; t = capture seconds)

| t (s) | Channel(s) | Event (interpretation) |
|------:|-----------|------------------------|
| 0.0276 | PC6 | first PC6 toggle (PC6 is **toggling**, 39 bursts, not a static strap) |
| **0.4511** | **PA2 MCU-TX** | **first USART TX burst — BEFORE any SPI activity** |
| **0.4907** | **PA2 MCU-TX** | second USART TX burst (still pre-upload) |
| 0.5010 | PB6 CS + PB3 SCK + PB5 MOSI | **SPI3 begins** — CS frames open, prelude + upload |
| 0.5010–0.5063 | PB3 SCK | prelude: ~59 bytes, /64-class clock, ~520-sample inter-byte gaps |
| **0.5288** | **PB11** | **active-mode → HIGH (AFTER SPI already started)** |
| 0.5506 | PA3 FPGA-TX | FPGA emits first frame |
| 0.84–3.61 | PC6 / PA3 | PC6 keeps toggling; FPGA-TX bursts at 1.9, 2.1, 2.2, 2.7, 3.2, 3.6 s |
| 3.3203 / 3.3808 | PA2 MCU-TX | **more MCU USART TX mid-sequence** |
| **3.7124** | **PB11** | active-mode → back LOW |
| 3.91–4.50 | PB5 MOSI + PB3 SCK | **second dense SPI block** (≈30 MOSI sub-bursts @ ~21 ms cadence + SCK clusters) — config writes / status reads / re-upload region |
| 4.06–5.39 | PA3 FPGA-TX | FPGA-TX bursts continue |
| 5.1072 | PB6 CS + PB5 MOSI | a later CS-framed SPI burst |
| 5.18–6.12 | PA2 MCU-TX | sustained MCU USART TX (≈18 bursts) |
| 6.1–12.5 | PC6 / PA2 | periodic PC6 toggles + MCU-TX (~0.5 s cadence) to end of capture |
| 12.5013 | all | capture end |

PB11 exact edges (sparse channel, decoded byte-exact): **0.5288 s ↑,
3.6126 s ↓, 3.7124 s ↑.**

---

## STOCK vs OURS comparison (what the timeline alone establishes)

| Signal | STOCK (#18 capture) | OURS (this capture) | Deviation |
|---|---|---|---|
| USART **MCU-TX (PA2)** pre-upload | **ZERO bytes** all boot | **TX bursts at 0.4511 & 0.4907 s, BEFORE the 0.5010 s upload** (+more at 3.32/3.38 s and 5.18–6.12 s) | **★ MAJOR** — we command the NV design over USART before/around config; stock never touches USART |
| **PB11** vs handshake | rises **1.0 ms BEFORE** SPI | rises at **0.5288 s, ~28 ms AFTER** SPI already started (0.5010 s) | Ordering wrong: PB11 not armed before the prelude |
| **PC6** enable | HIGH & static from before capture | **toggling (39 bursts)** across the whole boot | Not a clean static strap in this build |
| SPI upload clock | /64 ≈1.875 MHz (patched) | /64-class (~1.5 MHz quantized) prelude confirmed | Match (this build pins /64) |
| Prelude framing | `05 00`/`12 00`/`15 00`, CS-framed, ~100 ms apart | CS-framed byte exchange with ~520-sample inter-byte gaps confirmed (~59 bytes) — **byte values not recoverable** | Structure matches; contents unverifiable here |
| `05` prelude MISO | 0xFF (config-wait) | **unknown** (dense stream undecodable) | Cannot test the 0x80-vs-0xFF question from these bins |
| Bulk upload MISO | 0xFF (FPGA high-Z) | **unknown** | Cannot test |
| `0x3A` close status | 0xF8 | **unknown** | Cannot test |
| Post-upload config writes / `03` read | `01 08 / 02 03 / 06 00 / 07 00 / 08 AD` then `03`→`00 01 42 2E` | a second dense SPI block exists at 3.9–4.5 s (right shape) but **byte values not recoverable** | Cannot verify contents |
| FPGA-TX (PA3) announce | 6 unsolicited `5A A5`/`5A 69` frames at 2.8–3.6 s | **FPGA-TX bursts present** from 0.55 s onward (≈6+ bursts) — **contents not recoverable** | FPGA does talk; can't read the frames |

---

## Single biggest deviation (evidence-first)

**Our firmware transmits USART2 bytes on PA2 at 0.4511 s and 0.4907 s — before
the SPI3 bitstream upload begins at 0.5010 s — whereas stock transmits ZERO
USART for its entire boot.** This is exactly the "Step 3b pre-upload USART
commands" that the stock capture proved absent, and matches the leading
config-entry hypothesis recorded in `SPI3_STOCK_BOOT_CAPTURE_ANALYSIS.md`
(Experiment 7): commanding the NV meter design over USART before the upload
plausibly flips its user-design SPI slave onto the SSPI pins, so the FPGA is in
**user-mode (MISO-driven)** rather than **config-wait (MISO-float)** when `0x3B`
arrives — and it then ignores the bitstream. The capture is **this older build
that still sends those pre-upload commands**, so it is consistent with the
config-entry failure. The fix already applied to `fpga.c` (delete pre-upload
USART, move boot commands to after the SPI3 sequence) directly targets this
deviation. Secondary deviations: **PB11 arms ~28 ms too late** (after the
prelude, not 1 ms before) and **PC6 is not a clean static enable** in this
build — both also addressed in the later committed fixes.

> The decisive byte-level confirmation (prelude `05` MISO = 0x80 vs 0xFF, close
> status, etc.) requires a **re-export** of this `.sal` in the byte-decodable
> format, or a fresh capture of the current (post-fix) firmware.

---

## Decoder artifacts

* `/tmp/cfw_cap/decode3.py` — span-anchor + prelude-run decoder (the reliable
  parts). Reproduces the timeline table above and the SCK ~1.5 MHz / ~59-byte
  prelude measurement.
* The dense run-length sub-block flattening is unsolved; see "What could not be
  decoded" above.
