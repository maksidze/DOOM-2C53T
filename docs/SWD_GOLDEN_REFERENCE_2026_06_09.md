# SWD Golden Reference — Session Handoff (2026-06-09)

**One-line:** Defeated the SWD-killing watchdog on unit #2, turning the working
stock firmware into a live-debuggable golden reference. Used it to pin the
scope bug precisely to the **FPGA PC0 data-ready arm** and rule out every
config/pin/GMUX theory. SWD can't capture the dynamic arm (read protection +
low duty cycle) — next phase is guest-shell experiments.

---

## What we achieved today

1. **Watchdog defeated → stable SWD.** Stock kills the SWD link ~1-2s after
   attach because of the IWDG, *not* anti-debug (no DHCSR checks exist —
   verified in the decompile). NOP the IWDG start write and the link is rock
   stable, indefinitely, at 1 MHz.
2. **Caseless flash + recover loop re-proven** (with a new gotcha: unplug the
   ST-Link during IAP flashing — see below).
3. **Resolved the "HardFault" ghost** — it's a read-protection artifact, not a
   real fault. The device was healthy the whole time.
4. **Localized the scope bug** to the PC0 data-ready gate, with config / pins /
   GMUX all eliminated.

## The watchdog patch (how to rebuild the no-wdg image)

- Stock V1.2.0 APP: `archive/2C53T Firmware V1.2.0/APP_2C53T_V1.2.0_251015.bin`
  (sha256 `a17c5c35...`, 751232 bytes).
- IWDG start write `IWDG_KR = 0xCCCC` is at **file offset `0x275CC`**:
  `08 60` (`str r0,[r1]`) → `00 bf` (NOP). Only that one store is changed;
  unlock / prescaler / reload / feed all still run, so the counter is simply
  never started.
- Build script: `/tmp/patch_nowdg.py` (asserts source sha, asserts the 2 target
  bytes, asserts exactly a 2-byte diff, size unchanged).
- Output: `/tmp/APP_2C53T_V1.2.0_nowdg.bin` (sha256 `7a6dacfc...`), staged on
  mars at `~/APP_2C53T_V1.2.0_nowdg.bin`.

## Flashing (caseless IAP, host = `david@mars.local`)

1. **Unplug the ST-Link first.** In upgrade mode the device runs on USB power
   only; the ST-Link leads brown it out → the IAP volume enumerates then drops
   after ~0.66 s. (Our first no-wdg flash, done with the ST-Link interfering,
   produced a genuinely bad write that boot-faulted; the clean reflash booted
   perfectly.)
2. On device: hold **MENU + tap pinhole reset** → upgrade mode (works even from
   a frozen app; the bootloader checks MENU).
3. On mars: find the IAP volume (`/dev/sdc`, label `IAP`, marker `Ready.TXT`),
   then `sudo mcopy -i /dev/sdc ~/<image>.bin ::/<image>.bin`; `sync;
   sudo blockdev --flushbufs /dev/sdc; sync`. Marker flips `Ready.TXT` →
   `Success.TXT`, device reboots.
4. Replug the ST-Link only after it's running (normal mode is battery-backed and
   tolerates the ST-Link fine).

## SWD / OpenOCD (on mars)

- Config `~/at32_attach.cfg`:
  ```
  source [find interface/stlink.cfg]
  transport select hla_swd
  adapter speed 1000
  set CHIPNAME at32f403a
  set CPUTAPID 0x2ba01477          ;# Cortex-M4 DAP (NOT M3's 0x1ba01477)
  source [find target/stm32f1x.cfg]
  reset_config none separate
  ```
- Drive over SSH: `sudo timeout N openocd -f ~/at32_attach.cfg -c "init" -c ...`
- Wiring: SWDIO / SWCLK / GND only — **no 3V3** (clone supply fights the board reg).
  Pad order on V1.4 silkscreen: 3V3, SWDIO, GND, SWCLK.

## READ-PROTECTION DEBUG ARTIFACT — don't be fooled

Unit #2 has flash read protection ON. Over SWD this means:
- PC and **flash reads return the lockup sentinel** (PC=`0xfffffffe`,
  flash=`0xFF`), so OpenOCD **misreports "Handler HardFault"** even when the
  device is running perfectly. **Ignore the fake HardFault.**
- **RAM reads (`0x2000xxxx`) and peripheral reads (`0x4000xxxx`) WORK and are
  live** — confirmed because SysTick CVR (`0xE000E018`) changes every read.
- Code single-stepping / breakpoints are effectively blocked.
- **Wedging:** continuous `read_memory` loops on the running target freeze the
  core (dead display/buttons). Use discrete halt→read→resume only. Recover any
  wedge with a pinhole reset (boots back into working no-wdg stock).

## Config diff — stock(working) vs OpenScope: static setup is NOT the gap

| | Stock (working) | OpenScope |
|---|---|---|
| SPI3 CR1 | `0x0347` | `0x0347` (identical) |
| SPI3 CR2 | `0x0003` (RX+TX DMA) | polled (secondary lead) |
| AFIO MAPR | `0x02000000` (JTAG off) | same |
| Pin routing | legacy AFIO remap **alone** (GD32 binary, never touches GMUX) | sets legacy + GMUX |
| PC6 / PB11 / PB6(CS) / PC0 | 1 / 1 / 1 / 1 | same |

GPIOB CRL `0x81989408`: PB3/PB5 AF-PP, PB4 input, PB6 GPIO-CS — textbook SPI3
master. **GMUX is not the blocker on this unit** (matches guest: OpenScope's
MISO responds `0x00`, not `0xFF`).

## THE GAP (precise problem statement)

Stock's acquisition loop (`reverse_engineering/analysis_v120/fpga_task_annotated.c:1303-1323`)
is gated on **PC0 = FPGA data-ready, active-LOW** (`GPIOC_IDR` bit0, `0x40011008`):
- PC0 LOW → advance acquisition counter → trigger double-buffered SPI3 reads
  (two queue items) → scope data flows.
- PC0 HIGH → reset counter to 0.

Observed on both units: **PC0 stays HIGH → the FPGA sampling engine never
produces buffers → no scope.** Everything upstream (USART comms, SPI3 config,
pins, an extensive correct-looking command sequence in `fpga.c`) works. So the
single open question is:

> **What makes the FPGA begin sampling and pull PC0 low — and what is OpenScope
> doing differently such that it never does?**

The arm lives inside the FPGA's response to an earlier command; it's invisible
to RDP-limited SWD. We confirmed the acquisition burst is ~0.3% duty cycle
(buffer ~every 50 ms; SPI3 readout ~140 µs), so 10 random halt-snapshots all
caught PC0 HIGH / SPI3 idle. SWD halt-sampling fundamentally can't catch it.

## NEXT SESSION — guest CDC-shell experiment (no RDP limit, can't wedge)

1. Reflash the guest debug build (`/tmp/openscope_guest_0x7000.bin` on mars, or
   `make guest`) via IAP (unplug ST-Link first). Replug USB → `/dev/ttyACM0`
   (may need a cable replug for first CDC enumeration). Drive via
   `sudo python3 ~/sercmd.py <cmd>` on mars.
2. Instrument OpenScope's acquisition to **poll PC0 (`GPIOC_IDR` bit0) in a tight
   loop and report over CDC whether it EVER goes low.** This is the litmus test.
3. Iterate the arm to make PC0 drop: command order/timing, control-pin
   sequencing (PB11 active / PC6 enable), with/without the H2 cal upload, and
   the queue-driven double-buffer trigger described in
   `analysis_v120/FPGA_TASK_ANALYSIS.md`. Try DMA-paced SPI3 (stock CR2=`0x03`)
   vs polled.

## Unit #2 current state

Running the **no-wdg stock** image (functionally identical to factory, but
debuggable). Leave it as-is — it's the golden reference for future SWD snapshots.
Restore bone-stock any time via IAP with `APP_2C53T_V1.2.0_251015.bin`
(sha `a17c5c35...`). Recover from any hang with pinhole reset.

**Safety constraints (persist):** never disable read protection on unit #2
(mass-erase trap); never write its W25Q128 external SPI flash (factory cal);
never connect ST-Link 3V3.

## Key files / addresses

- Memory: `memory/project_swd_golden_reference.md` (auto-loaded next session)
- `/tmp/patch_nowdg.py`, `/tmp/APP_2C53T_V1.2.0_nowdg.bin`
- mars: `~/at32_attach.cfg`, `~/cap_pc0.cfg`, `~/sercmd.py`, `~/APP_2C53T_V1.2.0_nowdg.bin`
- PC0 data-ready: `GPIOC_IDR` bit0 = `0x40011008`
- IWDG patch: file offset `0x275CC` (`08 60` → `00 bf`)
- SPI3 base `0x40003C00`, USART2 base `0x40004400`
