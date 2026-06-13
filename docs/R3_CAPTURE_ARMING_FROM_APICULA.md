# R3 — What ARMS / SUSTAINS scope capture (netlist trace, no hardware)

> Reply to **R3** (the ⭐ priority) in `BITSTREAM_REQUESTS_FROM_OSC.md`.
> Source: static analysis of the unpacked stock netlist `tools/m5/scope_unpacked.v`
> (the real FNIRSI 2C53T design) + the GW1N-1P5C/QFN48XF pinout from our chipdb.
> Tool: `tools/m_arming.py` (self-contained; re-runs the whole trace). Date 2026-06-13.

---

## TL;DR — the answer to "what arms a new capture / why it stops after one buffer"

1. **The four sample BRAMs are *always* write-enabled.** `WREA = VCC`, `OCEA = VCC`,
   `RESETA = VSS` on all four (BSRAM_0/CH1, BSRAM_1, BSRAM_2, BSRAM_3/CH2, all in row 10).
   So there is **no write-enable pin to assert** — capture is **not** gated by WRE.

2. **Capture is gated by the BRAM clock-enable `CEA`,** which is a pure combinational
   function of an **internal free-running address/state counter** (≈383 DFFs), *not* of
   any pin directly. The CE gate LUT for CH1 has `INIT=0x3300` = **`CEA = qC & ~qB`** of two
   counter-state bits → the write happens during a counter-defined window each sweep.

3. **One primary input is the master "run / re-arm":** the pad at apicula location
   **`IOR1B`** (right edge, top; **pin 35** on the QFN48 proxy). It deep-drives
   **70 DFF clock-enables + 8 DFF async-SET (preset) + 156 DFF data** across the capture
   engine. Held in the run state → the counter advances and CE cycles → **continuous
   capture**. Its SET fan-out is the **re-arm / restart** path.

4. **So the "captured one buffer then stopped" symptom is expected:** after your MCU
   reset, the run/re-arm control (and the SPI-loaded control register, see §3) is no
   longer driven to the running state, so the counter completes one window and the
   8 preset/enable nets are never re-pulsed → the engine halts. **To restart continuous
   capture you must drive the run/re-arm control and/or re-issue the SPI control
   sequence** (your `01 08 / 02 03 / 06 00 / 07 00 / 08 AD` writes go in on exactly the
   pins below).

---

## The capture (write) datapath control, in detail

All four channels are structurally identical (strong evidence this is a regular,
replicated capture block):

| BRAM | tile | CLKA (write clk) | CEA (gate) | WREA | OCEA | RESETA |
|------|------|------------------|------------|------|------|--------|
| BSRAM_0 (CH1) | R10C2  | global spine `GB20` | LUT `qC & ~qB` | **VCC** | VCC | VSS |
| BSRAM_1       | R10C5  | global spine `GB40` | LUT of counter | VCC | VCC | VSS |
| BSRAM_2       | R10C14 | global spine `GB40` | LUT of counter | VCC | VCC | VSS |
| BSRAM_3 (CH2) | R10C17 | global spine `GB20` | LUT of counter | VCC | VCC | VSS |

- **Sample/write clock (`CLKA`) is on the global clock spine (`GBxx`)** and traces to
  **no IBUF** — i.e. it comes from the **PLL / global tree, not an MCU pin.** You cannot
  start/stop capture by withholding a clock from a GPIO; the clock free-runs. (Consistent
  with M5: the design uses the rPLL, which we only partially decode.)
- **The write address is a deep free-running counter** (~383 DFFs in the address cone,
  feeding the 14 `ADA` bits). It is what makes the scope "free-run" — it counts on the
  sample clock and wraps, continuously overwriting the ring buffer while running.
- **`CEA` = function of counter-state bits only** (CH1: `INIT=0x3300` ⇒ `qC & ~qB`). This
  defines *which* portion of each sweep is written — i.e. the capture window, not the arm.

## The control inputs that drive the engine (the actual answer to your question)

Backward-tracing `CEA` + the address counter + their controlling registers lands on
**exactly three primary inputs**. Forward-tracing each confirms its role:

| apicula IOB | QFN48 proxy pin | special fn | forward role into the engine | interpretation |
|-------------|-----------------|-----------|------------------------------|----------------|
| **IOR1B**  | **pin 35** | — | **70× DFF.CE, 8× DFF.SET, 156× DFF.D** | **MASTER run-enable + re-arm** |
| **IOB7B**  | **pin 19** | GCLKC_4 | 2× DFF.CE, 3× DFF.D | secondary enable / mode (clock-capable pin) |
| **IOB18B** | **pin 24** | **SI** | 1× DFF.D | a **bit of the SPI control register** (see below) |

`IOB18A` (pin 23, **SSPI_CS_N**) is also wired to a pad but is unused inside the fabric
(0 endpoints) — it gates the SPI shift externally.

### This is your "SPI3" control bus, reusing the SSPI config pins
The runtime control SPI the stock MCU uses after configuration **is the GW1N SSPI
(configuration-SPI) port repurposed as user I/O**. From the same pinout:

| signal | apicula IOB | QFN48 proxy pin | direction |
|--------|-------------|-----------------|-----------|
| **SCLK** | IOB5A | pin 16 | MCU → FPGA |
| **SI** (MOSI) | IOB18B | pin 24 | MCU → FPGA (control reg; one bit reaches CEA path) |
| **SO** (MISO) | IOB5B | pin 17 | FPGA → MCU (**this is the `0x04`/`0x05` readback line** — it's the only IOBUF in the design) |
| **CS_N** | IOB18A | pin 23 | MCU → FPGA |

So your post-config `SPI3` writes (`01 08 / 02 03 / 06 00 / 07 00 / 08 AD`) are shifted in
on **SI (pin 24)**, clocked by **SCLK (pin 16)**, framed by **CS_N (pin 23)**; the readback
(`0x04`/`0x05`, ~1025 bytes) clocks **out on SO (pin 17)**. One bit of that loaded control
register feeds the capture-enable path — i.e. **the SPI register is (part of) the arm/run
control**, alongside the dedicated run line on IOR1B.

**Likely restart recipe to try:** re-drive the run line (IOR1B-equivalent) into the run
state, then re-issue the stock control-register write sequence over SCLK/SI/CS_N exactly
as stock does post-config. If a single bit gates run/continuous, it's in that register
and/or on IOR1B; toggling it through the 8 DFF.SET path is what re-arms the counter.

---

## ⚠️ Honest caveats (read before acting)

- **Static structural analysis, not simulation.** I traced connectivity and LUT INIT
  masks; I did not simulate the FSM. The roles (run/re-arm vs window) are inferred from
  fan-out *kind* (CE/SET/D) and INIT decode. They're consistent across all 4 channels,
  which is strong, but treat the exact "drive this high to run" polarity as a hypothesis
  to confirm on the bench.
- **Pin numbers are from the GW1N-1P5C / QFN48XF *proxy* package, not the scope's chip.**
  The reliable, package-independent facts are the **apicula IOB names** (IOR1B, IOB7B,
  IOB18B=SI, IOB5A=SCLK, IOB5B=SO, IOB18A=CS_N) and their **special functions** — those
  come straight from the scope bitstream + die and won't change. The **QFN48 pin numbers
  may not match the scope's actual package/bonding.** Cross-check against your board trace
  using the special-function names (SI/SCLK/SO/CS_N), which are unambiguous.
- **Many top-edge IOT pads (the IOT2…IOT19 "MCU bus" group) are *not bonded* on the QFN48
  proxy** (shown as "pin ?" in the tool output). They exist in silicon and the design uses
  them, but the proxy package doesn't expose pin numbers for them — another reason to rely
  on IOB locations + your trace for those.

### Bonus cross-check for your JTAG hunt (relevant to R2)
Per this pinout, **JTAG is not at QN48 pins 8–11.** Those pins are left-edge IO
(`IOL11B/IOL12A/IOL12B/IOL17A`). JTAG sits at:
**TMS=IOT9B (pin 44), TCK=IOT9A (pin 45), TDI=IOT7B (pin 47), TDO=IOT7A (pin 48)**, with
DONE=IOT18B (37) / READY=IOT18A (38) nearby. On the scope's package the numbers will
differ, but the JTAG IOB *locations* are fixed — worth re-checking maksidze's gold-pad
assumption (issue #18) against the actual package, since pins 8–11 look like ordinary
left-edge GPIO, not the TAP.

---

## Reproduce
```
python3 tools/m_arming.py        # full trace: BRAM control, CE-gate decode,
                                  # forward roles of the 3 inputs, pin map
```
Pinout JSON (`/tmp/gw1n2_pinout.json`) is exported from the GW1N-2 chipdb on mars:
`db.pinout['GW1N-1P5C']['QFN48XF']`.

---

## Read-side (B-port) trace — how `0x04`/`0x05` returns the buffer (R3 bonus / R1 feed)

Done as a follow-up. The sample BRAMs are **true dual-port**: port A = ADC write,
port B = MCU read. Per channel (BSRAM_0 shown; all four identical):

| side | port | net group | meaning |
|------|------|-----------|---------|
| **write (A)** | CLKA | `CLK0` (global/PLL spine) | sample clock |
| | CEA | `CE1` (the capture gate, §above) | write window |
| | WREA | `LSR1` = VCC | always write |
| | DIA0–9 | `D0–D7, B6, B7` | **the ADC sample bus in** (~10 bits) |
| | ADA6–11 | `C0–C5` | write address (high bits; low bits width-absorbed) |
| **read (B)** | DOB0–5 | `Q0–Q5` | **read data out** (→ fabric → SO) |
| | ADB6–11 | `A0–A5` | read address |
| | CLKB/CEB | (unconnected placeholders) | see caveat |

Forward-tracing the read data:

- **All four BRAMs' read data converges on one pad — `SO` (IOB5B, proxy pin 17)** — the
  only `IOBUF` in the design. `SO.I` = readback-data-out, `SO.OEN` = a control-logic
  output-enable (gates *when* SO drives, i.e. the read window), `SO.O` = the input
  direction. So the channel readout is a **4-to-1 mux onto SO, selected by the opcode**
  (`0x04`=CH1 / `0x05`=CH2) that you shift in on **SI**. This is the full
  `0x04`/`0x05` → BSRAM_0/BSRAM_3 path your bench already confirmed, now traced in the fabric.
- The read **address (ADB) and the readout sequencing are produced by the same ~383-DFF
  control/counter cone** as the write side (driven by IOR1B + the SPI register), **not** by
  a simple "opcode resets a per-byte counter."

### What this means for R1 (the address↔byte mapping you asked about)
Because the read pointer is generated inside that 383-DFF control cone, the exact
**word → returned-byte mapping (start offset, stride, wrap, any reversal) cannot be read
off the static netlist cheaply** — it needs either gate-level simulation of that cone or,
far simpler, the **R1 ramp validator**: pre-load BSRAM_0 with `00 01 02 … FF …` and
BSRAM_3 with a walking marker, read back over `0x04`/`0x05`, and the mapping falls out in
one capture. So R1 isn't just nice-to-have — it's the *correct* tool for the byte mapping,
and this trace tells you it will be unambiguous (single mux, single SO line, per-opcode).

### Read-side caveats
- `CLKB`/`CEB` resolve to unconnected unpacker placeholders (the read clock/enable weren't
  recovered as routed nets). The read may be the unregistered/bypass path, or the clock
  rides a net the static tracer can't follow. Treat the read as "data appears on DOB,
  muxed to SO" without assuming a specific read-clock edge until the bench (R1) shows timing.
- Data width: 6 DOB bits + 6 DOA bits are connected (12 total); the 8-bit sample occupies a
  subset. The ramp validator will also pin down exactly which bits carry the sample.

---

## Status of the other requests
- **R3 (this doc): answered**, including the read-side bonus.
- **R1:** *recommended next* once the JTAG bench lands — it's the clean way to get the
  word↔byte mapping this trace shows is gated behind the control cone. Hardware-gated.
- **R2:** deferred (hardware-gated); ≈ our `tools/roundtrip/gw1n2-example/` counter→pin.
- **R4:** nothing config-strap-relevant surfaced in the user fabric (as expected; that lives
  in the GW1N config controller, not the unpacked design).
