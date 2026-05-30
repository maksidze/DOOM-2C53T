# ripcord ↔ fpga.c cross-reference + software-only USB test plan (2026-05-30)

Companion to `ripcord_hardware_handoff_2026_05_30.md`. Produced by a cross-reference
workflow over the ripcord predictions vs. our `firmware/src/drivers/fpga.c` and
`usb_debug.c`, with an adversarial review pass. **No logic analyzer required** —
every test below is observable through the USB CDC debug shell.

---

## TL;DR — the headline finding

Ripcord verified the **MCU emission side** of a *software* FPGA model in Renode.
Our firmware is a clean-room reimplementation. The two **agree on the static
structural contracts** (SPI3 @0x40003C00, Mode 3, /2; PB6 active-LOW CS; the
115638-byte boot blob framed `0x3B`/`0x3A`; PC6/PB11 as enable/gate) but
**diverge fundamentally on the per-command acquisition protocol**:

> **Ripcord predicts an SPI3 dispatcher that writes a 1-byte command (`01`–`09`)
> FIRST, then streams the payload — so MOSI reads `<cmd> <payload…>`. Our SPI3
> acquisition path NEVER puts `01`–`09` on MOSI at all.** It sends a single
> `0x80|range` arming byte, then clocks `0xFF` filler. All of our `01/02/06/07/08`
> traffic goes out **USART2** — a different wire.

This is the most important lead from the whole exercise. If ripcord's model is
right, the FPGA's SPI3 data interface expects to be *told* (e.g. `04` = burst
half 1) to start streaming, and **we never send that command** — which is a
candidate root cause for our long-standing "SPI3 MISO dead / PC0 stuck HIGH"
symptom. The new `spi3 xfer` shell command (added 2026-05-30) exists precisely
to test this.

Standing bench reality that overrides the emulation model on two points it
cannot see (memory `project_fpga_ignores_tx`, `project_spi3_miso_dead`):
- the FPGA currently **ignores all USART TX** (zero echo frames),
- SPI3 **MISO looks dead** and **PC0 (data-ready) stays HIGH** → acquisition
  engine not running.

⚠️ **Therefore: an all-`0xFF` MISO with PC0 HIGH is our EXPECTED baseline.** By
itself it is *non-diagnostic* — it cannot distinguish "dead slave" from "MISO
not routed" from "FPGA never told to stream." Log such a result as **"no new
information,"** never as a pass or fail. A *change* away from that baseline is
the only positive signal.

---

## Cross-reference table

| Contract | Ripcord prediction | Our implementation | USB-testable | How (shell) |
|---|---|---|---|---|
| **Boot blob** 115638B `0x3B`/`0x3A` | stream from flash `0x08051D19`, FPGA SPI inactive until done | **implemented, divergent framing**: `spi3_pump(fpga_h2_cal_table,NULL,115638)` gap-free (not 3-byte records); `0x3B` is tail of handshake G3 (`fpga.c:1735`), `0x3A` post-upload at `:1781` **CS-HIGH** (our inference) | **yes** | `status` → bytes_sent/115638 + done; `spi3 h2verify` re-streams + samples MISO + PC0 before/after |
| **SPI3 config** Mode3 /2 | @0x40003C00 master, STS@..08, DT@..0C | **match**: CTRL1 CPHA/CPOL=1, MSTEN, BR=000(/2=60MHz), SPE; CTRL2=0x03 (`fpga.c:1632-1653`) | **yes** | `status` (CTRL1/STS); `mem read 0x40003C00 4` (= 4 **words**) |
| **CS = PB6** active-LOW | assert per-cmd, deassert at loop bottom | **match, framing inferred**: assert once around whole handshake+upload (`:1700`); 3-phase post-upload dance (`:1777-1791`); left LOW | **yes** | `gpio read B6`, `gpio scan`, `status` (PB6 level) |
| **PC6/PB11** enable+gate | both HIGH; gating reality unconfirmed | **PC6 HIGH at boot** (`:1594-1599`); **PB11 left FLOATING through the entire blob upload**, raised HIGH only at `:1939-1944` (after upload) | **yes** ⭐ | force both HIGH then re-probe (see **T3**) |
| **Acq dispatcher** cmd-first on SPI3 | `<cmd> <payload>`; cmd byte written first | **DIVERGENT (fundamental)**: only `0x80|range` + `0xFF` filler on MOSI; `01`–`09` never on SPI3 (`fpga.c:1244+`) | **yes (needs `spi3 xfer`)** ⭐ | **T7** — `spi3 xfer 04` etc. |
| **State struct** @0x200000F8 | offsets +0x14/16/18/1C/2D/46/356/483/5B0/9B0/DB6/DB8 | **n/a** — our RAM layout is compiler-assigned, shares nothing with stock | **no** | only meaningful against STOCK firmware |
| **cmd 01** auto-range | `01 <idx>` hold/advance, `01 12` clamp; LUT @0x0804D833 | not on SPI3; range via `0x80\|range` arming byte; USART `0x01` pairs only | needs `spi3 xfer` | `spi3 xfer 01 05` (T7) |
| **cmd 02** set mode | `02 <mode>` → +0x14 | not on SPI3 (USART `0x02` pair, FPGA ignores) | needs `spi3 xfer` | `spi3 xfer 02 <mode>` |
| **cmd 03** roll | `03` + readback → rings | partial: roll case reads 4× `0xFF`, **discarded** (TODO); no `03` byte | **yes** | roll timebase → `fpga acq <roll#>` |
| **cmd 04** burst half1 | `04` + 512 pairs → +0x5B0 | 512 pairs read, but `0x80\|range`+`0xFF` echo, **no `04`** | **yes / `spi3 xfer`** ⭐ | `fpga acq <normal#>` then `spi3 read`; **T7** `spi3 xfer 04` |
| **cmd 05** burst half2 | `05` + 512 pairs → +0x9B0 | no distinct half2 command; dual reads contiguous | yes / `spi3 xfer` | dual mode; `spi3 xfer 05` |
| **cmd 06** set channel | `06 <mode>` → +0x16 | not on SPI3 (USART) | needs `spi3 xfer` | `spi3 xfer 06 <mode>` |
| **cmd 07** set trigger | `07 <mode>` → +0x18 | `0x07` is a USART channel/trigger **prefix**, not SPI3 | needs `spi3 xfer` | `spi3 xfer 07 <mode>` |
| **cmd 08** write trim | `08 <computed +0x1C>` | not on SPI3 (USART boot / timebase only) | needs `spi3 xfer` | `spi3 xfer 08 <v>` |
| **cmd 09** ADC-ref read | `09 FF FF 0A FF FF` + mid-seq PB6 pulse → +0x46 | **missing on SPI3**; our `0x09` = USART meter-start (different subsystem) | needs `spi3 seq` ⭐ | **T8** — `spi3 seq 09 FF FF \| 0A FF FF` |
| **Handshake** `05`/`12`/`15` | static-only, unreached in emulation, meaning unknown | **implemented**: sent in boot G1/G2/G3; MISO captured to `init_hs[0..11]` | **yes** ⭐ | `status` dumps `init_hs[]` = real FPGA replies ripcord never had |

⭐ = highest-value rows.

---

## Firmware gaps (status)

| Gap | What | Status |
|---|---|---|
| **GAP-1** generic `spi3 xfer <hex…>` — arbitrary MOSI, MISO dump, CS always released | the enabler for replaying ripcord's command-first frames | **IMPLEMENTED** 2026-05-30 (`usb_debug.c`), builds clean |
| **GAP-2** manual CS | already covered by `gpio set B6 0/1` | n/a |
| **GAP-3** `spi3 seq … \| …` — mid-sequence CS pulse for cmd 09 | inline us-scale CS pulse at `\|` | **IMPLEMENTED** 2026-05-30 |

To use GAP-1/GAP-3 (and Wave 2 below), reflash via the normal HID bootloader:
**Settings → Firmware Update** on the device, then `cd firmware && make flash`.
**No DFU.** Wave 1 needs no reflash.

---

## The test plan

Connect: power on normally (USB-C), `screen /dev/tty.usbmodem* 115200`, type
`help` to confirm. Run cheapest/safest first. After each wire-touching command,
note the `Delta:` line and `status` counters.

### Wave 1 — runs on CURRENT firmware (no reflash)

**T0 — Baseline snapshot.** `status`, then `gpio read C0`.
Record: `initialized`, `spi3_active`, all counters, **IOMUX remap init vs LIVE**,
SPI3 CTRL1/STS, PB4/PC6/PB6 levels, the 12 `init_hs[]` handshake bytes, H2 done
flag, PC0. → This is the reference everything else is compared against. *Any
non-`0xFF`/non-zero `init_hs[]` byte is already a live FPGA reply.*

**T1 — SPI3 routing + config.** `mem read 0x40003C00 4` (CTRL1/CTRL2/STS/DT —
note count is **words**, so this returns 16 bytes). Confirm Mode3/master/SPE.
Judge **pin routing from `status`'s IOMUX remap5 LIVE field, NOT from CTRLL
nibbles** — on AT32 SPI3 routing is the GMUX fabric, not legacy AF mode bits.
*Expected GREEN:* the GMUX writes are already in `fpga.c:1487-1488`. If remap5
LIVE is wrong, the bug is "GMUX didn't take" (clock/order), **not** "never
written."

**T2 — Blob acceptance probe.** `gpio read C0` → `spi3 h2verify` → reads back the
115638-byte stream's MISO at sampled indices + non-FF count + PC0 before/after.
*Positive signal:* PC0 drops, or any sampled MISO ≠ `0xFF`. *Baseline (no info):*
all `0xFF`, PC0 stays HIGH. **Do not** hand-poke CTRL1 with `mem write` to "fix"
anything — use `fpga scope reinit` or reboot to restore config.

**T3 — PC6/PB11 gating hypothesis** ⭐ (most promising new lead). We upload the
blob while **PB11 is floating**; if the FPGA needs PB11 HIGH to clock the blob
in, our late-raise means the blob landed while gated off. Steps:
1. `gpio scan` / `status` — record PC6, PB11, PC0.
2. `gpio set C6 1` (PC6 is already output → drives).
3. Confirm PB11 is **output HIGH**: at runtime `fpga.c:1944` configures it as
   output, so `status`/`gpio read B11` should already read 1. ⚠️ `gpio set` only
   writes the output latch, **not** pin direction — if PB11 reads as input/0,
   it was never raised; rely on the runtime path rather than forcing the latch.
4. With PB11 confirmed HIGH: `spi3 h2verify` (re-upload with gate open), then
   `gpio read C0`.
*Decision:* if MISO/PC0 come alive only with PB11 HIGH during the re-upload →
**we found the gating bug** → fix `fpga_init` to raise PB11 *before* the blob.

**T4 — Acquisition read.** Put the device in scope mode. `fpga acq <N>` where N is
the **numeric** trigger byte (⚠️ `fpga acq normal`/`roll`/`dual` words are
silently ignored — they all send the same default; confirm the byte from
`fpga.h`: `ROLL+1=2`, `NORMAL+1=3`, `DUAL+1=4`). Then `spi3 read 32` (a buffer
dump of the last capture — **no wire activity**) and `status` (`spi3_ok_count`,
`diag_ch1_raw`, `diag_data_varies`). *Positive:* `varies=1` and non-constant
bytes. *Baseline:* all-`0xFF`/constant, `varies=0` → no info.

**T5 — Handshake reply consistency.** Power-cycle 2–3×, compare `init_hs[0..11]`
via `status` each boot. Stable non-`0xFF` replies = genuine FPGA handshake
responses to `05`/`12`/`15` — new data ripcord never captured.

**T6 — Existing raw tests for contrast.** `spi3 acqtest` and `spi3 probe` —
already in firmware; useful as known-pattern controls alongside the above.

### Wave 2 — needs the reflash (GAP-1/GAP-3 commands)

**T7 — Command-first acquisition replay** ⭐⭐ (tests the headline divergence).
After flashing: `spi3 xfer 04` (ripcord's burst half-1 command byte that our
firmware never sends). Then `spi3 xfer 04 FF FF FF FF FF FF FF` to clock a few
payload bytes after the command. Compare MISO to the all-`0xFF` baseline.
*If MISO changes only when `04` precedes the filler* → ripcord's command-first
model is correct and **our acquisition is mis-architected** (we must send the
command byte on SPI3, not just `0x80|range`). Repeat with `01 05`, `03`, `05`.

**T8 — ADC-ref read with mid-sequence CS pulse.**
`spi3 seq 09 FF FF | 0A FF FF` — reproduces ripcord's cmd-09 pattern with the
PB6 pulse at `|`. Capture the two MISO bytes around `0A`; ripcord says they
assemble a 16-bit ADC-reference value.

**T9 — ID query.** `spi3 xfer 05` (ripcord's "ID query"). Compare its MISO to the
`init_hs[1]` reply already captured at boot (⚠️ the boot handshake sends `05` as
`g1={0x00,0x05,0x00,0x00}`, so the boot reply is at `init_hs[1]`, not `[0]` —
align indices before declaring agree/disagree).

---

## What today's first three tests tell us

- **T0/T1 GREEN, T2/T3 still all-`0xFF`+PC0 HIGH:** routing/config are fine; the
  FPGA SPI slave is not streaming. → Wave 2 (T7) becomes the priority: maybe we
  must *command* it. Also re-examine PB11 ordering.
- **T3 wakes MISO/PC0 when PB11 is HIGH during upload:** gating-order bug found —
  smallest, highest-confidence firmware fix. Raise PB11 before the blob in
  `fpga_init`, rebuild, retest.
- **T1 RED (remap5 LIVE wrong):** GMUX isn't taking effect — investigate clock
  enable ordering around `gpio_pin_remap_config(SPI3_GMUX_0010,...)`.
- **T7 (Wave 2) MISO responds to a leading `04`:** the big one — our SPI3
  acquisition needs to emit the ripcord command byte first. Rework
  `fpga_acquisition_task` to match the `<cmd> <payload>` model.

All SPI3 transfers in both files are timeout-guarded (100000-spin → `0xFF`), and
the new `spi3 xfer`/`spi3 seq` always deassert CS on exit, so no test can hang or
brick the device.
