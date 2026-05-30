# SPI3 / USART logic-analyzer + continuity capture plan (2026-05-30)

Goal: physically localize why SPI3 MISO (PB4) is inert. Software has exhausted
every lead (ripcord command-first ✗, PB11 gating ✗, USART works fine, SPI3
master config + clocking healthy). The remaining hypotheses are physical and
need probing: **(1) MISO not wired MCU↔FPGA, (2) the FPGA never drives MISO, or
(3) our SCK/MOSI/CS don't reach the FPGA.**

Tools (per CLAUDE.md): HiLetgo 24 MHz 8-ch USB analyzer (`fx2lafw`), driven by
`sigrok-cli` (`brew install sigrok-cli`). Plus a bench DMM for continuity.

---

## ⚠️ Read first — the 60 MHz problem

**Our SPI3 SCK runs at 60 MHz** (CTRL1=`0x0347`, BR=000 → PCLK1 120 MHz ÷2).
A 24 MHz analyzer **cannot sample a 60 MHz clock** (Nyquist needs ≥2×, and clean
decode needs ~8–10×). So:

- **SPI3 cannot be byte-decoded at 60 MHz on this LA.** Two mitigations:
  1. **Continuity check needs no LA at all** — do it first (Step 0).
  2. **Temporarily slow SPI3 to ≤1 MHz in our firmware** (prereq below), then the
     24 MHz LA decodes it with huge margin. SPI slaves are clock-speed-agnostic
     up to their max, so a real FPGA slave still responds at slow SCK.
- **USART2 (9600 baud) is trivially capturable as-is** — no firmware change, and
  this is where the highest-value comparison lives (Capture 3/4a).
- On **stock** firmware (60 MHz SCK, can't slow it) the LA still answers the
  binary question "does MISO ever toggle?" — aliased edges still register as
  transitions even when bytes are undecodable (Capture 4b).

---

## Step 0 — Continuity check (cheapest, do FIRST, no LA, power OFF)

Single highest-value test. With the board powered off, DMM in continuity/beep:

| MCU pin | Signal | Check continuity to… |
|---|---|---|
| PB3 | SPI3 SCK | FPGA clock-in pin |
| **PB4** | **SPI3 MISO** | **FPGA data-out pin** ← the critical one |
| PB5 | SPI3 MOSI | FPGA data-in pin |
| PB6 | SPI3 CS | FPGA CS-in pin |
| PC6 | SPI enable | FPGA enable pin |
| PB11 | active mode | FPGA gate pin |
| PC0 | data-ready | FPGA data-ready-out pin |
| PA2 | USART2 TX | FPGA RX pin |
| PA3 | USART2 RX | FPGA TX pin |

You'll need the AT32F403A LQFP pinout and the GW1N-UV2 package map; trace from
each MCU pin to the FPGA. **If PB4↔FPGA-MISO is open, that is the answer** — the
board (rev V1.4) routes it differently than the RE'd pinout assumes, or it's a
cold joint. This costs minutes and could end the investigation.

Also sanity-check each line for a short to GND/VCC (a MISO shorted to VCC would
read permanently HIGH = our exact symptom).

---

## LA hardware setup

Two probe sessions (8 channels can't cover SPI + both USART lines at once).

**Session A — SPI3 focus:**

| Ch | Pin | Signal |
|----|-----|--------|
| D0 | PB3 | SCK |
| D1 | PB4 | MISO ← watch this |
| D2 | PB5 | MOSI |
| D3 | PB6 | CS |
| D4 | PC6 | SPI enable |
| D5 | PB11 | active |
| D6 | PC0 | data-ready |
| D7 | PA2 | USART2 TX (cadence ref) |

**Session B — USART focus:** D0=PA2 (TX), D1=PA3 (RX), D2=PB6 (CS, as a
SPI-activity marker), D3=PC0. (9600 baud → low sample rate.)

**GND:** tie LA ground to board GND (SWD header GND or FPGA-programming-header
GND is a convenient reference).

**Probe access:** PB3=JTDO / PB4=NJTRST sit near the SWD/JTAG area; PA13/PA14
SWD header is by the USB-C port (good GND + nearby pins). If no test points,
probe MCU LQFP pins or vias directly. **Best practice: probe at the *FPGA* side
of each net** (or both ends) — that shows what actually *arrives* at the FPGA and
localizes a broken trace.

**fx2lafw has no hardware trigger** in sigrok — capture a fixed window and search
offline. Match sample rate to the signal (don't run 24 MHz/8ch — it drops
samples over USB2): use ~1 MHz for USART, ~6–12 MHz for slowed SPI.

---

## Prereq firmware: slow-clock capture mode

Add a debug command so SPI3 can be slowed for capture (spec; implement before
captures 1–2):

```
spi3 speed <0-7>   # rewrite CTRL1 BR[5:3]: 0=/2(60MHz default) … 7=/256(~470kHz)
```
Sketch (`usb_debug.c`, near the other `spi3` handlers):
```c
// disable SPE, set BR bits, re-enable
uint32_t c = *(volatile uint32_t*)0x40003C00;
c &= ~(1u<<6);                  *(volatile uint32_t*)0x40003C00 = c;  // SPE=0
c = (c & ~(7u<<3)) | ((br&7)<<3);
c |=  (1u<<6);                  *(volatile uint32_t*)0x40003C00 = c;  // SPE=1
```
At BR=7 (~470 kHz), 24 MHz LA → ~50× oversample. `status` already prints CTRL1 so
you can confirm the BR change. Restore with `spi3 speed 0` or reboot.

---

## Capture 1 — OUR fw, short SPI3 transfer, slowed (decodable)

1. `spi3 speed 7`  (confirm CTRL1 via `status`)
2. Start LA capture (Session A), then over serial: `spi3 xfer 05 12 15 3B 00 00`
3. Decode SPI (cpol=1, cpha=1, CS active-low).

**Questions:** SCK toggling cleanly on PB3? MOSI = `05 12 15 3B 00 00` exactly?
CS (PB6) low for the whole frame? **Does MISO (PB4) ever leave the idle high?**
PC6=high, PB11=high throughout?

## Capture 2 — OUR fw, scope acquisition, slowed

1. Scope mode, `spi3 speed 7`.
2. Start capture, then `fpga acq 3` (numeric NORMAL trigger).
3. **Q:** during the 512-pair read does MISO show *any* activity? Does PC0 ever
   transition? (Baseline expectation: MISO flat, PC0 stuck high.)

## Capture 3 — OUR fw, USART2 both directions (no slowdown, fully decodable)

1. Put device in **multimeter** mode (the meter poll task drives commands).
2. Session B, capture ~3 s, decode UART 9600 8N1 on PA2 and PA3.
3. **Q:** exact command bytes/cadence we send on PA2; the `5A A5` data frames on
   PA3; and **does the FPGA ever send an echo frame `AA 55`?** (Our counter says
   never — confirm on the wire.) This documents the working USART link precisely.

## Capture 4 — STOCK firmware reference (the gold standard)

Flash stock per `docs/stock_restore_test_plan.md` (`dfu-util` ROM DFU; do NOT
touch option bytes). Then:

- **4a — USART (decodable):** Session B, capture stock in meter AND scope mode.
  Decode UART. **Diff stock's command stream vs ours** (Capture 3). Does stock
  receive **echo frames `AA 55`**? Does stock send a scope-enable command we
  don't? This may explain the echo quirk and reveal a missing command.
- **4b — SPI3 at 60 MHz (activity, not bytes):** Session A, stock in scope mode
  with a signal on the probe. Can't decode, but watch **MISO (PB4) for edges**.
  - Stock MISO **toggles** during acquisition, ours flat → the FPGA *can* drive
    MISO; our firmware isn't eliciting it (commands/sequence) or our SCK/CS isn't
    reaching the FPGA. Focus there.
  - Stock MISO **also flat** at this probe point → we're on the wrong net / MISO
    isn't where we think → revisit pinout (board rev), back to Step 0.

Reflash our firmware afterward: `make flash-all`.

---

## Decision matrix

| Step 0 PB4↔FPGA | Cap 1 MOSI/SCK | Cap 1/2/4b MISO | Conclusion |
|---|---|---|---|
| **open** | — | — | **MISO not wired** (board rev / cold joint) — root cause found |
| ok | wrong/absent | — | our SCK/MOSI/CS not reaching FPGA — routing/GMUX at the probe point |
| ok | correct | ours flat, **stock toggles** | FPGA drives MISO; we fail to trigger streaming — chase the command/enable sequence (use Cap 4a diff) |
| ok | correct | flat on **both** stock & ours | probing wrong net, or this unit's FPGA SPI data-out is dead |

---

## sigrok-cli reference

```bash
sigrok-cli --scan                                   # confirm fx2lafw seen
# Session A capture (slowed SPI), ~2s at 8 MHz:
sigrok-cli -d fx2lafw --config samplerate=8m \
  -C D0,D1,D2,D3,D4,D5,D6,D7 --time 2000 -o capA.sr
# Decode SPI (D0 clk, D2 mosi, D1 miso, D3 cs):
sigrok-cli -i capA.sr -P spi:clk=D0:mosi=D2:miso=D1:cs=D3:cpol=1:cpha=1 -A spi
# Session B capture (USART), ~3s at 1 MHz:
sigrok-cli -d fx2lafw --config samplerate=1m -C D0,D1,D2,D3 --time 3000 -o capB.sr
sigrok-cli -i capB.sr -P uart:tx=D0:rx=D1:baudrate=9600:format=hex -A uart
```
(`--time` is ms. Lower the samplerate if you see "device only sent N samples" /
dropped-sample warnings — these low rates stream reliably over USB2.)

---

## Order of operations (fastest path to an answer)

1. **Step 0 continuity** — minutes, may solve it outright.
2. **Capture 3** (our USART) + **Capture 4a** (stock USART) — fully decodable,
   no slowdown; nails the echo-frame question and diffs command streams.
3. Implement `spi3 speed`, flash, run **Capture 1/2**.
4. **Capture 4b** (stock SPI3 MISO-activity) — the definitive "can the FPGA
   drive MISO at all" test.
