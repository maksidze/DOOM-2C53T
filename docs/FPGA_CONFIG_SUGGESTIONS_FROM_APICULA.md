# FPGA config — suggestions & cross-findings from the Apicula side

> **Source:** the sister project `gw1n2-apicula` (adding GW1N-2 support to Project
> Apicula, the open-source Gowin bitstream toolchain). That work reached the point of
> **unpacking the scope's stock bitstream** and **authoring + round-tripping our own
> GW1N-2 bitstreams**, so it can speak authoritatively about the *bitstream/Gowin-config*
> side. This doc is a handoff: what we can confirm, what's new, and ranked hypotheses
> for the SSPI config-entry blocker.
>
> **Honesty notes for the reader (osc agent):**
> - I read this repo up to commit `407652e` (2026-06-13). Some of my first ideas were
>   *already implemented* — I call those out so you don't redo them.
> - Anything I'm not certain of is labelled **HYPOTHESIS** — verify on the bench.
> - Date: 2026-06-13.

---

## 1. What we can CONFIRM (independent corroboration)

### 1a. The stock bitstream blob is definitively good — "GowinEDA gold standard" achieved
`make_fs.py` notes its `.fs` is "UNVERIFIED until bench; a real GowinEDA `.fs` is the
gold standard." We effectively provide that gold standard **without a bench**:
- We built GW1N-2 support in Apicula and ran `gowin_unpack` on
  `scope_bitstream_2c53t_v120.bin` (sha `5a0e7338…`): **all 722 config-frame CRC-16s
  validate**, and it decodes into a sensible fabric netlist. The blob is a complete,
  intact, correctly-ordered GW1N-2 bitstream.
- `make_fs.py`'s **MSB-first byte order is correct** — it matches our independently
  CRC-validated converter. The `.fs` fallback is sound. (If openFPGALoader ever rejects
  the `.fs` but accepts the `.bin`, that's file-type detection, not bit order.)

**Takeaway:** stop suspecting the payload. If SSPI/JTAG config is rejected, it is *not*
the data.

### 1b. The captured SPI3 opcodes ARE standard Gowin SSPI config (from openFPGALoader's `gowin.cpp`)
| captured | Gowin opcode | meaning |
|---|---|---|
| `05 00` | `0x05 ERASE_SRAM` | erase config SRAM |
| `12 00` | `0x12 INIT_ADDR` | reset config address pointer |
| `15 00` | `0x15 CONFIG_ENABLE` | **enter configuration mode** |
| `3B` + 115638 B | config data write | the bitstream |
| `3A 00` | `0x3A CONFIG_DISABLE` | exit config (→ `0xF8` = accept) |
| `03 FF…` | `0x03 READ_SRAM` | status/readback |

So the stock sequence is the bona-fide Gowin SSPI SRAM-config flow — **not** a custom
user-design protocol. (You already encode this correctly in `fpga.c`; this just
confirms the opcode meanings from the vendor-tool source.) Also: **openFPGALoader's
JTAG path uses the *same* commands** (CONFIG_ENABLE → INIT_ADDR → write → CONFIG_DISABLE),
so the JTAG bench plan is well-aligned.

### 1c. `MISO = 0xFF` throughout ⇒ config never *engaged* (not a data reject)
Stock returns `0xF8` after `0x3A` (accept); your failing captures return `0xFF`
(high-Z). Combined with 1a, this means the FPGA isn't rejecting the bitstream — it
**never enters config mode at all**. The problem is *config-entry*, full stop. This
is the right thing to be hunting.

---

## 2. Already implemented — do NOT re-derive
From reading `fpga.c::fpga_spi3_config_sequence` (current):
- `0x15 CONFIG_ENABLE` is already sent as its **own CS-framed** transaction
  (`prelude_frame_mode == 0`, default). ✓
- The stray "flush `0x00` with CS HIGH" clocks were already removed. ✓
- PB11 armed ~1ms early; pre-upload USART silenced; bare CS frame-sync pulse present. ✓
- A reset-pin sweep (`reset_port`/`reset_pin`) is already wired into `fpga reinit`. ✓

⚠️ The `CFW01_BYTE_DECODE_ANALYSIS.md` "config still fails" capture is of an **older
build** (pre no-USART / pre PB11-arming). **Confirm whether the *current* split-framed
build has actually been bench-captured.** If not, that's the cheapest next step — the
earlier "fails" result may not apply to the current code.

---

## 3. FPGA-internal findings (NEW — useful once config works)
These come from unpacking the stock image; they're not visible from MCU-side RE and
corroborate your acquisition findings:

- **The image is 4 independent blocks** (zero dataflow crossover): **2 scope channels +
  a DMM + a signal generator.** Matches your meter/scope/sig-gen model exactly.
- **Scope channels map to BRAMs:** left-edge ADC (IDDRC) → **BSRAM_0**, right-edge ADC →
  **BSRAM_3**, fully independent. This is the FPGA side of your per-channel
  `0x04`(CH1)/`0x05`(CH2) reads — **the bulk read is literally the contents of those two
  sample BRAMs** (so expect raw sample buffers, not an interleaved stream — consistent
  with your "not interleaved" capture note). Each capture window ≈ one BRAM's worth.
- **Signal generator = the ODDRC (DDR output / DAC) block on the left edge.** When you
  get to the "high-risk sig-gen FPGA encoding" session, that ODDRC block + its control
  inputs are the thing to trace; the apicula netlist (`scope_unpacked.v`) localizes it.
- **Headroom:** ~55% of logic and ~49% of FFs are unused; BRAM (4/4) and the PLL (1/1)
  are full. So a custom trigger/feature has plenty of *logic* room but no spare capture
  buffer or new clock. (See `gw1n2-apicula/docs/07-architecture-headroom.md`.)

---

## 4. Config-entry blocker — ranked HYPOTHESES

Given config never engages even with correct framing/data, the trigger is something
*other* than the SPI command bytes. Ranked by "cheap to test" × "fits the evidence":

**H1 — Bench-test the current build first (cheapest).** Prior "fails" was an old build.
Capture the *current* split-framed sequence; check MISO after `0x3A` for `0xF8`. If it's
now `0xF8`, you're done. *(If already done and still `0xFF`, proceed.)*

**H2 — The FPGA never reaches its stock "alive" state, and config-entry depends on it.**
Stock's FPGA emits `5A A5` announce frames on USART RX (~1.4–3.6 s) *before* the upload;
your custom captures show the FPGA UART essentially dead and a 142.8 Hz PC6 heartbeat
(NV design idling). **HYPOTHESIS:** the GW1N's NV design must finish its own init/handshake
before its SSPI config port will honor CONFIG_ENABLE — and on custom it never gets there.
*Test:* figure out what makes the FPGA announce (`5A A5`) on stock — likely an early
clock/power/reset/strap the NV design needs — and reproduce *that* before sending `0x15`.
This reframes the hunt from "what triggers config" to "why won't the FPGA's own NV image
fully wake up under our boot."

**H3 — SSPI re-entry from user mode needs `RECONFIG_N` (pin 48), not just CONFIG_ENABLE.**
Per Gowin UG290, after the device auto-loads NV flash it's in user mode; a `RECONFIG_N`
low pulse is the documented way to re-trigger configuration. Your capture shows no such
pulse on the 8 MCU lines, and the MCU-GPIO RECONFIG hunt came up empty (PC4/PD3/PD2) —
so **if** RECONFIG_N is required, it's on an untraced pin or one of the 5 gold pads
(QN48 pin 48). *Test:* continuity-trace pin 48; scope it during a *stock* mode-entry.
*Caveat:* this competes with the fact that stock's captured SPI-only sequence apparently
succeeds without a visible pulse — so either it's not required, or the pulse is on a line
you didn't capture.

**H4 — SPI-peripheral behavioral mismatch (AT32 vs stock MCU) at the SSPI layer.**
CS-to-first-clock setup, clock idle polarity dwell, or inter-byte CS timing can differ
between the AT32F403A SPI3 and whatever the stock firmware runs, even at "Mode 3, /2."
The GW1N SSPI command FSM is timing-sensitive (you already found IDCODE reads need a
slow clock). *Test:* on the bench, slow the *command phase* clock (not just the bulk
upload) and widen CS setup/hold; compare MISO response to `0x15`.

---

## 5. The JTAG path is the right unblock — and a clean diagnostic
`gw1n2-apicula` confirms openFPGALoader speaks fluent GW1N-2 (we generated a
nextpnr-himbaechel chipdb and authored bitstreams for this exact part). The
`docs/gowin_jtag_programmer_guide.md` plan is sound. Two adds:
- **It's also the decisive diagnostic.** If `openFPGALoader -c ft232 --detect` shows
  IDCODE `0x0120681B` and `-m` SRAM-load brings up the scope, that *proves* the
  bitstream + FPGA are fine and **isolates the failure to SSPI config-entry** (→ H2/H3).
- **`--read-register` / status read after a (failed) SSPI attempt** will tell you whether
  config engaged-and-rejected (CRC/IDCODE error bits) vs never-engaged (your `0xFF`
  evidence says the latter — but confirm via JTAG).
- Keep the safety rule absolute: **SRAM only (`-m`), never `-f`/flash** — the NV flash
  holds the only copy of the meter design.

---

## 6. Where to find the Apicula side
- Repo: `gw1n2-apicula` (sister project). Bitstream unpack + our converter:
  `tools/bin2fs.py` (frame-structured, CRC-validated); analysis: `docs/07-architecture-headroom.md`.
- The unpacked stock netlist (`scope_unpacked.v`) and the channel/interface maps are
  available if you want to chase the scope data layout or sig-gen encoding post-config.
- If you need a *fresh* known-good GW1N-2 bitstream (e.g. a minimal blinky to prove the
  JTAG load path independent of the scope image), we can generate one on demand.

---

## 7. The unique lever: we can MANUFACTURE GW1N-2 bitstreams (use it)

The Apicula side can synthesize arbitrary GW1N-2 images from RTL
(yosys → nextpnr-himbaechel → gowin_pack), validated end-to-end. The osc project only
has the extracted stock blob — so test/diagnostic bitstreams are something only this
sister project can produce. Highest-value uses, in order:

### 7a. "Hello world" SRAM image — to validate the JTAG load path (NOT SSPI)
**Why JTAG only:** the SSPI failure is config-*entry* (FPGA returns `0xFF`, never opens
its config port). A minimal stream hits the same closed port → same failure. Swapping
the payload does **not** help SSPI. It *does* help the **JTAG** path, by decoupling
"did the SRAM load work" from "is the 115 KB scope image doing anything visible."

- ⚠️ **SRAM only (`openFPGALoader -m`). NEVER `-f`/flash** — flash holds the only meter image.
- For a first pass you don't even need an observable output: **openFPGALoader's success
  report + DONE→HIGH** confirms the load. A runtime-observable image (drives an
  LED/BNC/test-point) is a stronger "it's alive" check but needs the **FPGA-side board
  pin map** (which FPGA ball → observable net) — that's osc's board knowledge, not in
  the MCU pinout. Tell Apicula a safe, observable FPGA pin and we'll target it.
- We already have a CRC-clean minimal image to start from:
  `gw1n2-apicula/tools/roundtrip/gw1n2-example/` (counter→pin), regenerable on demand.

### 7b. ⭐ Scope-readout validator — the high-value one
An image that **pre-loads a BRAM with a known ramp/counter pattern**. Then the existing
`0x04`(CH1)/`0x05`(CH2) bulk read returns a **known answer** instead of live ADC noise —
validating the read protocol *and* data layout in one shot. From the FPGA side the bulk
read is "the raw contents of BSRAM_0 / BSRAM_3," so a known BRAM init = a known readout.
This is the fastest path to closing the scope-acquisition gap once *any* config route works.
(Apicula can author this; requires config-load working first via JTAG or fixed SSPI.)

### 7c. Sig-gen probe
For the flagged high-risk sig-gen RE: author a known waveform on the ODDRC/DAC block, or
trace its control encoding from the unpacked netlist (`scope_unpacked.v`). The sig-gen is
the left-edge ODDRC block (§3).

### Decisive experiment that ties it together
`openFPGALoader -c ft232 --detect` (expect IDCODE `0x0120681B`) → `-m` SRAM-load the
hello-world. If it loads + DONE goes HIGH, the bitstream and FPGA are proven good and the
*only* remaining problem is SSPI config-entry (→ §4 H2/H3). That's the cleanest split of
the problem space available.
