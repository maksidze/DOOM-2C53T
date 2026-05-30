# Firmware → ripcord: hardware findings + investigation asks (2026-05-30)

**Reciprocal handoff.** ripcord's `HARDWARE_HANDOFF.md` gave us emulation-verified
predictions for the MCU↔FPGA SPI3 protocol. We implemented the USB-shell tooling
to test them on the **real AT32F403A driving the real Gowin FPGA** (FNIRSI 2C53T,
board rev V1.4, bench unit #1). This is what silicon actually did — plus the
specific things only ripcord's static + emulation pipeline can now resolve.

## Confidence tags
| tag | meaning |
|-----|---------|
| `silicon` | directly observed on hardware via the USB CDC debug shell (firmware self-report; trustworthy for state changes, see caveat) |
| `inferred` | reasoned from the observations |
| `open` | unresolved; an ask for ripcord |

**Observability caveat:** all readings are the firmware's own report over USB CDC
(MISO bytes, GPIO levels, frame counters, register reads). Conclusive for "did X
change," but cannot distinguish "FPGA not driving MISO" from "MISO not wired to
PB4" — that needs a logic analyzer / continuity probe (planned separately).

---

## 1. Headline

Your structural contracts hold; your **per-command acquisition model does not
reproduce on silicon**, and the "FPGA ignores USART" assumption (ours, not yours)
was wrong. Net: the SPI3 **data interface never activates** (MISO inert), while
SPI3 master + USART are both provably healthy. The open question is purely *what
turns the FPGA's SPI3 data output on* — which your binary analysis can answer.

---

## 2. Reconciliation vs. your predictions (report-back format)

```
SPI3 config (Mode3/master//2, PB6 CS, @0x40003C00):  CONFIRMED (silicon)
  CTRL1=0x0347 (CPOL=1/CPHA=1/MSTEN/BR=000//2/SPE/SWCS), STS=0x0002,
  SPI3 timeouts=0, transfers complete, SCK toggles. Master is healthy.

Boot blob (115638B, 0x3B..0x3A framed):             EMITTED (silicon), no FPGA response
  Uploaded 115638/115638. spi3 h2verify re-stream → 0/115638 non-FF on MISO,
  PC0 unchanged — even with PB11 HIGH during the upload.

Acq dispatcher "command-first" (<cmd> then payload):  DIVERGED (silicon)
  spi3 xfer 05 / 12 / 15 / 04 FF.. / 01 05 / 03 FF.. → MISO all 0xFF, PC0 1→1.
  spi3 seq 09 FF FF | 0A FF FF (mid-seq CS pulse) and spi3 xfer 09 FF FF 0A FF FF
  → all 0xFF. Sending the command byte first does NOT wake the slave.

PB11 gating-order:                                   DIVERGED (silicon)
  Re-uploading the blob with PB11 already HIGH still yields 0/115638 non-FF.

MISO (PB4):                                          INERT (silicon)
  Never leaves 0xFF / never driven low — across boot handshake (05/12/15),
  the 115638B blob (boot + re-upload), and every command byte. Idle reads HIGH.

PC0 (data-ready):                                    STUCK HIGH (silicon)
  =1 in every read, every mode. Never observed to drop.

USART2 — "FPGA ignores TX":                          REFUTED (silicon)
  TX hardware good (STS TDC+TDBE, BAUD=0x30D4=9600 exact, CTRL1=0x202C).
  FPGA RESPONDS, command-driven ~1:1 with 5A A5 DATA frames (burst TX 38 → DF 32).
  Example frame: 5A A5 E4 2E 63 25 07 00 00 00 01 12.
  But NEVER sends echo frames (AA 55): EF=0 across every command ever sent.
  Our 10-byte TX frame is byte-identical to stock ([2]=cmd_hi [3]=cmd_lo
  [9]=csum, rest 0). It is request-response, NOT autonomous streaming.
```

---

## 3. Investigation asks (what only ripcord can resolve)

**A. Reconcile your FPGA model.** Command-first dispatch produced nothing on
silicon. Your Renode oracle's FPGA model "responds" because it's written to;
the real part doesn't. Please mark command-first (and the cmd-09 mid-seq CS
pulse) as **unverified-on-silicon** in `fpga_protocol.py` and the contract ledger
(contract #19). The 01–09 opcodes may simply not be how *this* FPGA's SPI3 data
path is driven.

**B. What ENABLES SPI3 streaming? (the central question.)** From the V1.2.0
binary, trace the **first SPI3 read in stock that returns non-0xFF data**, and
identify the exact MCU action(s) immediately preceding it:
- Does stock send a specific **USART command** to start scope acquisition before
  it reads SPI3? (We only put `0x80|range` on SPI3 MOSI and a generic boot
  USART set 1,2,6,7,8 — we never send a distinct USART "scope-start" the way the
  meter path sends 0x09.)
- Is there a **post-blob activate/commit** step? We stream the 115638B table
  (0x3B begin … 0x3A end) and stop. Does stock do anything after 0x3A — another
  opcode, a CS sequence, a GPIO — to make the FPGA *act on* the table and bring
  its SPI data interface live? Does the FPGA acknowledge the blob at all?

**C. PC0 (data-ready) semantics.** Our PC0 is stuck HIGH and never drops; we read
SPI3 unconditionally. From the binary: does stock **poll PC0 (or its equivalent
data-ready input) before each SPI3 read**, and what FPGA-side condition / MCU
action makes it transition? If stock gates reads on PC0 LOW and PC0 never drops
for us, that is the gate. (Identify the GPIO and polarity precisely.)

**D. Echo-frame dependency.** Our FPGA streams `5A A5` data frames but never
`AA 55` echo frames. Does stock's scope/acquisition FSM **block on receiving an
echo ack** (byte[3]==sent cmd, byte[7]==0xAA) before advancing — e.g. before
entering SPI3 acquisition? If the scope path waits on an echo our unit never
sends, that would explain the stall. (We know the RX ISR *validates* echoes; the
question is whether any forward path *requires* one.)

**E. Mode/state precondition for `acq_engine_task` (@0x0803B454).** Under what
state is it actually entered? Your handoff notes a `mode_state` machine in the TX
task (1=enable → UEN + resume tasks + PB11 HIGH; 3=TX frame). Does scope
acquisition require a specific mode transition / sequence we're skipping?

---

## 4. Artifacts we can provide back

- **Decoded stock USART command stream** (PA2/PA3 @ 9600, fully capturable) from
  a planned stock-firmware flash + logic-analyzer session — the ground truth to
  validate/correct your model and answer B/D directly. We will also report
  whether **stock receives `AA 55` echo frames** on this physical unit.
- Live register dumps / MISO captures from our `spi3 xfer`, `spi3 seq`,
  `spi3 h2verify`, `status` shell commands on demand.
- Result of flashing **stock V1.2.0** and testing whether the scope physically
  works — if it does, the FPGA SPI3 data path is proven functional and the bug
  is entirely in our firmware (narrows B/C/E enormously).

## 5. Pointers (our repo)
- `reverse_engineering/analysis_v120/ripcord_crossref_and_usb_test_plan_2026_05_30.md` — cross-ref + BENCH RESULTS + USART-IGNORE INVESTIGATION
- `reverse_engineering/analysis_v120/spi3_usart_logic_analyzer_plan_2026_05_30.md` — physical capture plan
- `docs/stock_restore_test_plan.md` — stock flash/restore procedure
- `firmware/src/drivers/fpga.c`, `firmware/src/drivers/usb_debug.c` — SPI3/USART driver + debug shell
