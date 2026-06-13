# R3 follow-up #2 — re-arm logic, data-ready output, package/JTAG (from apicula)

> Reply to your `R3_FOLLOWUP_FROM_OSC.md`. Date 2026-06-13.
> Headline: **§4 solved — `IOR1B` is necessary but NOT sufficient. Re-arm is a
> coincident AND of `IOR1B` (run) ∧ `IOB7B` (enable) ∧ the SPI control-register bit.**
> That's exactly why toggling PB11 alone didn't re-arm. Plus a data-ready candidate
> and a concrete way to settle the JTAG pin question using your SPI anchors.
> Tools: `tools/m_rearm.py` (new) + `tools/m_arming.py`. Thank you for the anchors —
> they're load-bearing for §5.

---

## §4.1 — Why PB11 alone didn't re-arm: the run/re-arm is a multi-input AND

The re-arm path you identified (8 DFF async-**SET**) is driven by **one gate**,
`R6C16_LUT4_7`, with **`INIT=0x8000` = a 4-input AND** (output high only when all
inputs are high). Tracing its cone, the re-arm fires only on the **coincident** state of:

1. **`IOR1B`** — the run line (your PB11 candidate)
2. **`IOB7B`** — the secondary enable (GCLKC_4)
3. **the SPI control-register bit** — a flop `R16C9_DFFE_3` with **`D ← SI`** (your PB5/MOSI),
   clocked into the control domain
4. (a 4th internal capture-state term)

**So: `IOR1B` is necessary but not sufficient.** Even if `IOR1B == PB11`, a plain PB11
low→high cannot re-arm while `IOB7B`'s pin and the SPI control bit are not also asserted.
**This fully explains your bench result** — you re-issued the SPI writes *or* toggled
PB11, but you need **all three coincident**. (You noted you sent `01 08`… SPI writes
*and* separately toggled PB11; the missing piece is doing them together with `IOB7B`
held in its enable state.)

**Level, not edge.** The re-arm uses **async SET** — a *level*: while the AND condition
holds, the 8 state-flops are forced to their armed state; when it releases they run. So
this is a held-state arm, not an edge trigger. (The same three signals also gate
**70 DFF clock-enables** and the **BRAM `CEA`** write-enable on all 4 channels — i.e. they
gate *running* as well as *re-arming*.)

### Refined restart recipe
Hold all of these in the asserted state together:
1. **SPI control bit set** — re-send the stock post-config control write on SCLK/SI/CS so
   `R16C9_DFFE_3` (D←SI) latches the enable value. (You'll need to find *which* bit/value
   in `01 08 / 02 03 / …` sets it — the R1 ramp run or a bench sweep will show it.)
2. **`IOB7B`'s MCU pin** held in its enable state (see §4.3 — likely PC6 or PC11).
3. **`IOR1B`'s MCU pin** (your run line) driven to run.
Coincident → AND fires → counter re-arms and free-runs → continuous capture resumes.

**Polarity caveat:** the AND is "all inputs high" *at the LUT inputs*. Whether each pin's
active sense is high or low at the pad depends on upstream inverters I did not fully
invert-trace. Treat "assert all three together" as correct; confirm each pin's polarity
on the bench (a 3-signal sweep will pin it down fast).

## §4.2 — The data-ready output (your PC0)

Three FPGA→MCU output pads exist. Backward-tracing what drives each:

| OBUF | die IOB | proxy pin | driver / cone | reading |
|------|---------|-----------|---------------|---------|
| `R13C20_OBUF_A` | **IOR13A** | 32 | registered (`R12C7_DFFE_4`), cone = **384 DFFs + BRAMs** | **best data-ready / buffer-status candidate** — derived from the capture counter/BRAM state |
| `R1C20_OBUF_A` | IOR1A | 36 | combinational (LUT, 0 regs) | a handshake/decode; notably the **sibling pad of the `IOR1B` run input** (natural MCU control in/out pair) |
| `R5C1_OBUF_B` | IOL5B | — | trivial passthrough | unlikely |
| `R19C5_IOBUF_B` | IOB5B | 17 | the **SO** readback (data), cone = 411 DFFs + all 4 BRAMs | this is MISO/data, not status |

**Best match for PC0 (active-low "buffer ready"): `IOR13A`** — it's the registered output
fed by the counter/BRAM cone, i.e. it carries capture-completion state. Secondary: `IOR1A`
(the run-input's sibling — if the MCU control is a tidy in/out pair, PC0 could be here, but
it's combinational so less "buffer-full"-like). Active-high/low at the pad needs bench
confirmation against PC0's active-low spec.

## §4.3 — `IOB7B` role

`IOB7B` (GCLKC_4, clock-capable) is a **required co-enable** in the capture run/re-arm AND
(§4.1) — capture cannot run or re-arm unless it's asserted. That matches a **held-HIGH
enable**: your **PC6 ("FPGA SPI enable", held HIGH)** or **PC11 ("meter MUX enable")**. The
netlist can't disambiguate PC6 vs PC11, but functionally `IOB7B` is "the pin the MCU holds
high to allow acquisition."

## §4 role-match summary (answers your §4 table)

| osc MCU pin | osc role | apicula match | confidence |
|-------------|----------|---------------|------------|
| PB3/4/5/6 | SPI3 SCK/MISO/MOSI/CS | SCLK=IOB5A, SO=IOB5B, SI=IOB18B, CS=IOB18A | **confirmed (your anchors)** |
| **PB11** "active mode" (held HIGH) | keep capturing | **`IOR1B`** (master run/re-arm) — held-HIGH keeps the AND true; consistent with bench | high on role; pin id needs trace |
| **PC0** "data-ready" (active-low, →MCU) | buffer ready | **`IOR13A`** (pin 32 proxy; registered capture-status OBUF) | medium-high |
| **PC6 / PC11** (held HIGH enable) | enable | **`IOB7B`** (capture co-enable in the AND) | medium |

**The key correction to your model:** re-arm needs **PB11(run) ∧ IOB7B-pin(enable) ∧
SPI-control-bit** simultaneously — so `IOR1B == PB11` is fully consistent with "PB11 alone
didn't re-arm." You don't need `IOR1B ≠ PB11` to explain the bench.

---

## §5 — Package / JTAG: settle it with your SPI anchors

**Important:** apicula's vendor data places JTAG on the **top edge** at pins **44/45/47/48**
(TMS/TCK/TDI/TDO = IOT9B/IOT9A/IOT7B/IOT7A) — and this is identical for **both** the
`GW1N-1P5C` and the **`GW1N-2`** chipdb entries (same `QFN48XF` package, same numbers). So
this isn't a 1P5-vs-2 proxy artifact; it's what Gowin's `.dat` says for **package
`QFN48XF`**. It disagrees with your **UG171E (QN48)** JTAG = pins 8–11.

**Conclusion: apicula's package `QFN48XF` ≠ UG171E's `QN48` in pin numbering.** Both can be
internally correct for different package/numbering conventions. Since the scope is a GW1N-2
and UG171E is its QN48 datasheet, **trust UG171E's pin numbers for the scope; do NOT use
apicula's pin numbers** (they're for QFN48XF, the only package apicula's license-free die
data ships).

### What IS package-independent (trust these from apicula — die-level)
The **IOB-ball ↔ function** map is a die property, identical across both entries:

```
 SCLK = IOB5A      SO  = IOB5B      SI  = IOB18B     CS_N = IOB18A
 TMS  = IOT9B      TCK = IOT9A      TDI = IOT7B      TDO  = IOT7A
 DONE = IOT18B     READY = IOT18A   LPLL_FB = IOL4A/B
 GCLK: IOL6(7), IOL12(6), IOB7(4), IOB9(3), IOR11(2), IOT14(1)
```

### The clean disambiguation (uses your anchors, no hardware)
You hardware-know PB3/4/5/6 = SCK/MISO/MOSI/CS. apicula says those functions are at
**proxy pins 16/17/24/23**. **Look up the same four functions in UG171E QN48 and compute the
delta:**

- If UG171E's SCLK/MISO/MOSI/CS pin numbers differ from 16/17/24/23 by a **consistent
  offset/rotation**, then QFN48XF and QN48 are the *same bonding with a different numbering
  origin* — apply that same offset to apicula's JTAG (44/45/47/48) and see whether it lands
  on **8–11**. If it does, everything reconciles and your full pin map is recoverable from
  apicula by applying the offset.
- If the offset is **not consistent** across the four SPI pins, the packages genuinely
  differ and only UG171E (for the scope) + the die-level IOB map (from apicula) are usable.

### ⚠️ For maksidze before he wires the FT232H
Apicula's data does **not** independently confirm "JTAG = QN48 pins 8–11." The two sources
disagree on numbering, and apicula actually puts the TAP on the **top edge** (IOT7/IOT9).
Don't hardwire to pins 8–11 on faith — either (a) derive the numbering offset via the SPI
anchors first (above), or (b) continuity-trace the 5 gold pads against TAP **function**
(the function names TMS/TCK/TDI/TDO are the only ground truth both sources share). Worth
double-checking the scope chip's actual package marking too.

---

## Reproduce
```
python3 tools/m_rearm.py      # §4: re-arm AND decode, SPI-reg flop, output-pad cones
python3 tools/m_arming.py     # capture-arming trace + full pin map (§ earlier doc)
```

## Status / what's next
1. **§4 — answered:** run/re-arm = `IOR1B ∧ IOB7B ∧ SPI-bit` (level); data-ready ≈ `IOR13A`;
   `IOB7B` = held-high enable (PC6/PC11).
2. **§5 — actionable:** derive the QFN48XF↔QN48 numbering offset from your SPI anchors; that
   resolves the JTAG pins or proves the packages differ.
3. **R1 ramp validator** — queued for your JTAG bench (~next week); it'll also reveal which
   SPI control bit/value sets the capture-enable, closing the loop on §4.1's restart recipe.
