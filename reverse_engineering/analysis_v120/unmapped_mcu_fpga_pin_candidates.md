# Unmapped MCU↔FPGA Pin Candidates — the "hidden connection" short-list

**Date:** 2026-06-13
**Why this exists:** maksidze (GitHub #18, 2026-06-13T17:50) — *"There may be some
other connections between the MCU and the FPGA, some signals are missing... some of
the tracks run under large chips."* This is direct board-side corroboration of our
config-entry conclusion: when the SPI3 wire is byte-identical to stock yet config
still fails, the lever must be a connection we haven't mapped. Apicula's R3 says the
scope's run/re-arm is an FPGA **input** net (`IOR1B`) that we never drive — a prime
suspect for one of maksidze's "missing signals."

This doc gives maksidze a **short candidate list** to continuity-check (instead of
tracing everything under the big ICs), and feeds the Apicula `IOR1B` role-match.

> **UPDATE 2026-06-13 (Apicula `R3_FOLLOWUP2`):** the *run/re-arm* question is now
> answered — continuous capture = a **3-input level AND** of `IOR1B`(≈**PB11**) ∧
> `IOB7B`(≈**PC6/PC11**) ∧ an **SPI-control bit** (latched from **PB5**/MOSI). **All
> three are pins we already control**, so the warm-handoff one-buffer halt is most
> likely a *firmware* fix (assert all three coincidentally), **not** a hidden wire.
> That **removes re-arm from the hidden-line hunt.** What remains hidden-line-relevant
> is **only the config-entry wall** — and Apicula explicitly *can't* see that (it lives
> in the GW1N config controller, not the user fabric Apicula unpacked). So §2 below is
> now scoped to **config-entry**, and the newly-urgent item is the **JTAG-pin
> disambiguation in §3a** (two sources now disagree on where the TAP is).

---

## 0. The reframe — the hidden line may not be MCU-driven at all

Our full decompile sweep of stock master-init (`stock_pre_fpga_gpio_state.md`,
RECONFIG hunt in `captures/SPI3_STOCK_BOOT_CAPTURE_ANALYSIS.md`) established a hard
fact: **in the entire SPI3-init → 0x3B-upload window the ONLY GPIO output write is
PC6 HIGH.** Stock pulses no dedicated FPGA reset/reconfig pin. So if a config-entry
or run/re-arm line exists, it is one of:

1. an MCU pin driven **outside** the upload window (e.g. PC4 is driven on a mode
   flag *after* the upload), or
2. **not MCU-driven at all** — a hardwired config-mode strap (MODE0/JTAGSEL_N tied
   to a rail/resistor), a power-sequencing signal, or an FPGA clock input.

So the trace maksidze is proposing is exactly right and broader than "MCU↔FPGA":
**every FPGA pin's destination matters**, especially the config straps and any FPGA
pin that lands on a resistor/rail rather than the MCU.

---

## 1. Known MCU↔FPGA connections (the 8 lines + data-ready) — for reference / cross-off

| MCU pin | role | dir | FPGA-side (Apicula, where known) |
|---|---|---|---|
| PB3 | SPI3 SCK | →FPGA | IOB5A (proxy pin 16) |
| PB4 | SPI3 MISO | ←FPGA | IOB5B (17) — the 0x04/0x05 readback IOBUF |
| PB5 | SPI3 MOSI | →FPGA | IOB18B (24) |
| PB6 | SPI3 CS (GPIO) | →FPGA | IOB18A (23) |
| PC6 | "SPI enable" strap, HIGH | →FPGA | **`IOB7B`** (capture co-enable, GCLKC_4) — or PC11 |
| PB11 | "active mode", HIGH in measure | →FPGA | **`IOR1B`** (master run/re-arm) — Apicula §4 |
| PC11 | "meter MUX enable", HIGH in meter | →FPGA | `IOB7B` candidate (vs PC6) |
| PC0 | "data-ready", active-low | ←FPGA | **`IOR13A`** (proxy pin 32; registered capture-status OBUF) |
| PA2/PA3 | USART2 TX/RX, 9600 | bidir | meter cmd/data path |

These 9 are **proven** and every one is now stock-faithful on the wire, yet config
still fails — hence the hunt for a 10th line.

## 2. MCU-pin candidates for a hidden FPGA connection (ranked)

Ports A–E enumerated; obvious non-FPGA pins crossed off (USB PA11/PA12, SWD
PA13/PA14, BOOT1 PB2, OSC PC14/PC15, **SPI2/W25Q128 flash = PB12/PB13/PB14/PB15**,
LCD EXMC bus on PD0/1/4-11/14/15 + PE7-15, button matrix, DAC PA4/PA5).

### Tier A — driven by stock but UNEXPLAINED (we know a wire exists; purpose unknown)
| MCU pin | what stock does | already tested? |
|---|---|---|
| **PC4** | driven HIGH when FPGA mode flag `DAT_2000010f`==2, else LOW — set **after** upload | RECONFIG-pulse test ❌ negative; **NOT** tested as a held run/re-arm level |
| **PD3** | static HIGH during AFIO/SPI3 bring-up, never cleared | RECONFIG-pulse ❌ negative |
| **PD2** | static HIGH in probe/coupling region | RECONFIG-pulse ❌ negative |

> These three are the **highest-value continuity targets**: stock provably drives
> them, we can't explain them, and the RECONFIG test only ruled out a *low pulse* —
> it did **not** rule out PC4/PD2/PD3 being a held run-enable like Apicula's `IOR1B`.
> If any of them traces to an FPGA pin, that's very likely the missing lever.

### Tier B — used but role-unconfirmed (could double as FPGA strap/sense)
| MCU pin | current attribution | note |
|---|---|---|
| PC1, PC2 | "probe-atten lines, left at config default" | never driven pre-upload; could be FPGA-facing |
| PA6, PB9 | "undocumented analog-frontend control" (outputs) | left FLOATING during our upload; stock drives them |

### Tier C — genuinely unmapped (no known function in the decompile)
`PA0, PA1, PA9, PC3, PC7, PD12, PD13, PE0, PE1`
> Lower prior (stock doesn't obviously configure them), but if a trace lands on the
> FPGA they immediately become Tier A.

## 3. FPGA-side targets — what to look for at the FPGA end (config-entry only)

`IOR1B`/`IOB7B`/`IOR13A` are now matched to **known** MCU pins (§1, Apicula §4), so
they are **not** continuity unknowns. The config-entry straps remain the targets:

| FPGA pin / net | QN48 pin (UG171E) | why it matters | expected destination |
|---|---|---|---|
| MODE0 | 13 | config-mode strap | **rail/resistor**, not MCU (UG171E: MODE1/2 internally GND) |
| JTAGSEL_N | 3 | JTAG-reconfig select | rail/resistor |
| RECONFIG_N | 48 | config re-trigger | confirmed by maksidze: **always HIGH, no pulse** |
| TMS/TCK/TDI/TDO | 8/9/10/11 *(UG171E)* — **see §3a, disputed** | JTAG | the 5 gold pads |

**The decisive continuity / DC questions for config-entry:**
1. Where does **MODE0 (pin 13)** go — a pull resistor to 3V3 or GND? (sets config-mode group)
2. Is **JTAGSEL_N (pin 3)** strapped, and to what?
3. Does any FPGA *input* pin trace to **PC4, PD2, or PD3** (our only unexplained MCU
   outputs)? Lower prior now that re-arm is solved, but still the only MCU-side
   candidates for a config-entry lever.

## 3a. ⚠️ JTAG-pin disambiguation — settle BEFORE wiring the FT232H

Two independent sources now **disagree** on where the JTAG TAP is:
- **UG171E (GW1N-2 QN48 datasheet):** TMS/TCK/TDI/TDO = pins **8/9/10/11**.
- **Apicula die data (`QFN48XF` package):** TAP on the **top edge** = pins
  **44/45/47/48** (IOT9B/IOT9A/IOT7B/IOT7A).

### RESOLVED 2026-06-13 — UG171E QN48 table pulled, offset computed, conflict dissolves

Fetched UG171E (v1.8.1E) and read the QN48 column of the pin-list table directly
(`pdftotext -layout`, pages 7–8). Cross-tab vs Apicula's `QFN48XF` proxy numbers:

| function | die cell (**both sources agree**) | UG171E **QN48** pin | Apicula QFN48XF pin |
|---|---|---|---|
| SCLK | IOB5A | **29** | 16 |
| SO | IOB5B | **28** | 17 |
| SI | IOB18B | **35** | 24 |
| CS_N | IOB18A | **34** | 23 |
| TMS | IOT9B | **8** | 44 |
| TCK | IOT9A | **9** | 45 |
| TDI | IOT7B | **10** | 47 |
| TDO | IOT7A | **11** | 48 |

**Finding 1 — Apicula's die-level IOB↔function map is confirmed.** Every IOB name
matches UG171E exactly (IOB5A/5B/18A/18B, IOT7/9 A/B), SSPI pins included. Their
netlist analysis is trustworthy.

**Finding 2 — the offset is NOT consistent (SCLK +13, SO/SI/CS +11, JTAG −36/−37;
A/B order even flips on IOB5) ⇒ the two package numberings are genuinely different
origins, no single offset.** BUT: **both sources place JTAG on the identical die cells
(IOT9B/9A/7B/7A, top edge).** Apicula's "top edge 44–48" and UG171E's "8–11" are the
*same silicon location*, numbered per different packages. **The scope is a GW1N-2 in
QN48 ⇒ UG171E is authoritative ⇒ TMS=8, TCK=9, TDI=10, TDO=11. The gold-pad target of
8/9/10/11 stands.** The only open check: confirm the chip's **package marking = QN48**
(if it's actually QFN48XF, the numbers shift to 44–48 — but UG171E being the GW1N-2
QN48 datasheet and the FNIRSI BOM both point to QN48).

**Net:** the §5 "JTAG conflict" was a numbering artifact, not a location conflict.
Apicula numbers must not be applied to the scope; UG171E 8–11 are correct. No need to
hold up maksidze's FT232H wiring on this — just verify the package marking.

**QN48 physical layout** (from UG103E Fig 3-17, top view — rendered to
`gw1n2_qn48_pins_topview.png` in this dir). Pins run counter-clockwise: **left edge
top→bottom = 1–12**, bottom = 13–24, right = 25–36, top = 48→37 (pin 1 = top-left).
So the JTAG target **TMS=8 / TCK=9 / TDI=10 / TDO=11 are four consecutive pins low on
the LEFT edge**, just above the bottom-left corner (pin 12 = VCC). Orientation
neighbors: JTAGSEL_N=3 (upper-left edge), RECONFIG_N=48 (top-left corner), MODE0=13
(bottom-left corner). GND reference: VSS = pins **2 & 26** + the center **EPAD** (tie
to GND). So the 5 gold pads = the 8/9/10/11 cluster + a nearby GND (EPAD or pin 2).

## 4. What this unblocks
- **maksidze (board, no chip removal needed for most of it):** (1) **§3a JTAG
  disambiguation** — now the highest-value board task, gates safe FT232H wiring;
  (2) two strap reads (MODE0 pin 13, JTAGSEL_N pin 3); (3) optional 3-pin trace
  (PC4/PD2/PD3 ↔ FPGA) for the config-entry lever.
- **us (firmware):** the warm-handoff one-buffer halt is now a concrete recipe —
  assert **PB11(run) ∧ PC6/PC11(enable) ∧ the SPI-control bit** *coincidentally*
  (Apicula §4.1). Testable post-JTAG with the R1 ramp validator, which will also
  reveal which `01 08`/`02 03`/… bit sets capture-enable.
- **Apicula:** §4 + §5 delivered; R1 ramp validator queued for the JTAG bench.

## 4a. Whole-binary GPIO audit (2026-06-13) — closes the input/EXTI/out-of-window blind spots

The prior sweeps were **outputs-only, master-init-only, RECONFIG-framing-only**. This
pass classified *every* GPIO access across all three decompiles (`decompiled_2C53T.c`,
`_v2.c`, `analysis_v120/full_decompile.c`) — direct register pokes (`DAT_4001xxxx`) AND
HAL-mediated (`gpio_read_pin(base,mask)` / `gpio_configure_pins`). Findings:

**Inputs (the real blind spot) — only TWO pins are read, both already known-ish:**
- **PC0** — `_DAT_40011008 & 1`, read in the scope acquisition FSM (the FPGA→MCU
  "data-ready" we already mapped; also the likely EXTI source). No surprise.
- **PC7** — `gpio_read_pin(0x40011000, 0x80)` in `FUN_08036084`, used as a *conditional
  strap* (selects `FUN_0800bcd4(10)` vs `(7)` during a framebuffer/display branch).
  **NEW: PC7 is read as a board-variant/strap input — previously unmapped (Tier C).**
  Worth a continuity check; likely a HW-revision strap, not FPGA, but unconfirmed.
- Buttons are read in the TMR3 ISR (separate, fully understood) — not re-listed.

**Outputs surfaced that the pre-FPGA-window sweep missed:**
- ⭐ **PD2** — driven **HIGH** (`_DAT_40011410 = 4`, GPIOD BSRR bit 2) in **three
  scope/FPGA-mode-entry paths** (lines ~6854/7306/7775 of `_v2.c`), each right beside a
  `fpga_queue` send + `device_mode`/`_config_value` change. **This upgrades PD2 from
  "static HIGH, unexplained" to "asserted on FPGA scope-mode entry" — the strongest
  hidden-FPGA-line candidate we have.** It is NOT one of our known FPGA control pins
  (PC6/PB11/PC11/PB6) and NOT EXMC (PD2/3/12/13 are all free GPIO). Our firmware never
  drives it. NB: the RECONFIG hunt pulsed PD2 low→high (negative) — but if PD2 is a
  *held* mode/enable asserted before config, a pulse wouldn't replicate stock.
- **PD12 / PD13** — driven (`_DAT_40011410 = 0x1000 / 0x2000`) in a `meter_state`-indexed
  loop (channel/range-select shape). Previously logged "unconfigured"; **actually driven,
  unmapped** — probably analog frontend, unconfirmed.
- **PC1 / PC2** — confirmed *actively driven* (probe-atten relays), not merely "left at
  default" as the pre-FPGA-window doc implied (they're driven at runtime, not at boot).
- **PC4** — confirmed driven (BRR clear seen), mode-2 conditional (already known).

**EXTI still not bit-decoded here:** the decompile abstracts AFIO/EXINT entirely (zero
`DAT_4001000x`/`DAT_4001040x` refs — even the JTAG-disable remap is HAL'd), so EXTI
line→pin source-select remains objdump-only (`stock_pre_fpga_gpio_state.md`: routes the
"FPGA-ready/button lines"). PC0 is the FPGA-ready source; low residual risk.

**Revised continuity priority for maksidze (supersedes §3 #3):**
1. **PD2 → FPGA?** (now the top MCU-side candidate — asserted on scope-mode entry)
2. **PD12/PD13, PC7** → FPGA or analog? (driven/read, unmapped)
3. PC4/PD3 (lower — PD3 not seen driven in this pass; was CRH-config HIGH per objdump)

If **PD2 traces to an FPGA input**, it's both a config-entry lever candidate *and*
firmware-reachable (we can drive it) — exactly the "hidden connection" profile.

### BENCH TEST — NEGATIVE (2026-06-13, Unit 2)
Added a `fpga reinit` strap-hold knob (`s2`/`sd`, debug-only) and held PD2 (HIGH and
LOW), PD12+PD13 (HIGH), and PD2+PD12/13 together through the entire handshake. **All
five variants → `0x3A close=FF`, `0x03 status=FF FF FF FF`, acqread all-FF — identical
to the no-strap control.** Driving these pins from the MCU has **zero effect on
config-entry.** Conclusion: **PD2 is NOT the config-entry lever** (at least not a held
MCU level we can drive). This is consistent with the standing finding that config-entry
is FPGA *internal state*, not anything on an MCU-drivable line. Caveats (why it's a
demotion, not a full disproof): PD2 may (a) connect to the FPGA but matter only
*post-config* (a run/mode signal — the decompile shows it asserted on scope-mode
*entry*, i.e. after config, fitting this), untestable until a live config exists; (b)
need a timing/edge we didn't replicate; or (c) not reach the FPGA at all. **Demoted from
"top config-entry candidate" to "driven-but-unmapped; trace to learn its role, not as a
config lever." Its post-config/run relevance can only be tested with a JTAG-loaded
config.** Strap knob left uncommitted (no green → below commit bar).

## 5. Sources
- `stock_pre_fpga_gpio_state.md` (every pre-FPGA pin write, objdump-anchored)
- `captures/SPI3_STOCK_BOOT_CAPTURE_ANALYSIS.md` → "RECONFIG-pin hunt — exhausted"
- `docs/R3_CAPTURE_ARMING_FROM_APICULA.md` (IOR1B run/re-arm net)
- `docs/R3_FOLLOWUP2_FROM_APICULA.md` (3-input AND re-arm; IOR13A data-ready; JTAG pkg)
- `docs/FPGA_CONFIG_SUGGESTIONS_FROM_APICULA.md` + UG171E QN48 pinout
- GitHub #18 (maksidze continuity work)
