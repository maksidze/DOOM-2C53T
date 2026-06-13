# Stock Pre-FPGA GPIO / Peripheral State (V1.2.0)

**Date:** 2026-06-12
**Binary:** `archive/2C53T Firmware V1.2.0/APP_2C53T_V1.2.0_251015.bin` (raw, link base **0x08007000**)
**Method:** `arm-none-eabi-objdump -D -b binary -marm -Mforce-thumb --adjust-vma=0x8007000`,
hand-decoded against the AT32 SPL `gpio_init()` and a from-scratch reimplementation of the Keil
`__decompress1` to recover the `.data` (RAM 0x20000000) boot image. Every line below is anchored
to a TRUE-flash objdump address.

**Address convention (per `usart_boot_frames_exact.md`):** doc/Ghidra addresses are `file_offset +
0x08000000`; TRUE flash = `file_offset + 0x08007000`. Master init `FUN_08023A50` (doc) = file
0x23A50 = **objdump 0x0802AA50**. SPI3 FPGA handshake begins at file 0x2650A = **objdump
0x0802D50A**. The window analyzed is **0x0802AA50 → 0x0802D63C** (entry until the first SPI3 byte
of the bitstream handshake).

---

## TL;DR — the headline result

Stock, before it ever clocks an SPI3 byte to the FPGA, **fully establishes the analog-frontend
relay/gain posture** by calling two range-select helpers with the boot-default range index **5**
(decompressed from `.data`, `meter_state[0x200000FA]=0x05` and `[0x200000FB]=0x05`). That drives
**PC12, PE4, PE5, PE6** (input-routing/attenuation relays) and **PA15, PA10, PB10, PB11**
(gain/active selects) to defined levels. It also raises **PC6 HIGH** (FPGA SPI/enable strap) at the
top of the SPI3 init.

**Our replacement firmware leaves the entire frontend relay/gain bank (PC12, PE4/5/6, PA15, PA10,
PB10, PB9, PA6) FLOATING during the bitstream-upload window** — those pins are only driven later,
inside `fpga_set_meter_frontend_baseline()`, which runs in meter mode, not in the boot/upload path.
If the Gowin samples any of these as straps/environment at config time, that is the most likely
unmodeled difference. **PB11 is the sharpest single discrepancy:** stock drives it **HIGH** (as part
of relay range-5, ~before the handshake), our firmware drives it **LOW** until the arm edge.

---

## (a) Ordered table — every externally-visible pin write, in execution order

Struct decode key (AT32 SPL `gpio_init_type`, confirmed against `at32f403a_407_gpio.c`):
`+0` pins · `+4` out_type (0=PP,4=OD) · `+5` pull (4=none,0x18=up,0x28=down) · `+6` mode
(0=INPUT,0x10=OUTPUT,8=MUX/AF,3=ANALOG) · `+7` drive (0/1/2). Helper = `gpio_init @ 0x080372FC`.

### Group 1 — RCC/CRM clock enables (no pins yet, but gate everything)

| objdump | reg | write | meaning |
|---|---|---|---|
| 0x0802AA6C | CRM APB2EN 0x40021018 | `\|=0x10` | IOPCEN (GPIOC) |
| 0x0802AABE..AAE4 | 0x40021018 | `\|=0x04,0x08,0x10,0x40` | IOPAEN, IOPBEN, IOPCEN, IOPEEN |
| 0x0802AB7A..AB98 | 0x40021018 | `\|=0x20,0x40,0x01` | IOPDEN, IOPEEN, **AFIOEN** |

→ All of GPIOA–E + AFIO clocked before any pin config. (More peripheral enables follow inline.)

### Group 2 — static GPIO mode config (11 `gpio_init` calls, 0x0802AAB2–0x0802ABFE)

| objdump | port | pins | mode / config | notes |
|---|---|---|---|---|
| 0x0802AAB2 | GPIOC | **PC9** | OUTPUT PP, strong | power hold |
| 0x0802AAB6 | GPIOC_BRR 0x40011014 | `=0x200` | **PC9 LOW** (momentary; early reset handler had already set it — see note) |
| 0x0802AB04 | GPIOB | **PB7** | INPUT **PULL-DOWN** | PRM button idle-low |
| 0x0802AB18 | GPIOC | **PC8, PC13** | INPUT **PULL-UP** | POWER + UP passive buttons |
| 0x0802AB2E | GPIOB | **PB7, PB8** | OUTPUT PP, moderate | PB8 = LCD backlight; PB7 re-driven as output here (overrides the pull-down config above) |
| 0x0802AB3C | GPIOB | **PB0** | OUTPUT PP, moderate | button matrix row |
| 0x0802AB4A | GPIOC | **PC5, PC10** | OUTPUT PP, moderate | button matrix rows/cols |
| 0x0802AB5E | GPIOE | **PE2, PE3** | OUTPUT PP, moderate | button matrix row/col |
| 0x0802ABC2 | GPIOD | **PD0,1,8,9,10,14,15** | **MUX/AF** PP, strong | EXMC data bus |
| 0x0802ABD2 | GPIOE | **PE7–PE15** | **MUX/AF** PP, strong | EXMC data/addr bus |
| 0x0802ABEC | GPIOD | **PD6** | OUTPUT PP, strong | EXMC NWAIT/ctrl |
| 0x0802ABFE | GPIOD | **PD4,5,7,11** | OUTPUT PP, strong | EXMC NOE/NWE/NE/A16 ctrl |

*Note (PC9):* the BRR write at 0x0802AAB6 momentarily clears PC9, but power-hold is asserted by the
reset/clock-init stub at 0x08000000 **before** master init runs (see `reset_and_clock_init.md`); the
device does not drop. This is just the SPL re-init of the pin's mode.

### Group 3 — EXMC (LCD) + SysTick delay machinery (0x0802AC02–0x0802C020)

EXMC base 0xA0000000: `SNCTL=0x5010/0x5011`, `SNTCFG=0x02020424`, `SNWTCFG=0x00000202`. SysTick
(0xE000E010) one-shots used for the inter-phase delays. No FPGA-relevant external pins. (Big region;
includes scope buffer init, FatFs/SPI-flash, DAC/ADC, USART2 config — summarized in Groups 4–7.)

### Group 4 — Signal-generator DAC + its analog pins (0x0802C1FC–0x0802C260)

| objdump | port/peri | write | meaning |
|---|---|---|---|
| 0x0802C238 | GPIOA | PA4 | mode from struct (ANALOG) — DAC1 out |
| 0x0802C23C | DAC 0x40007400 | `CR \|= 0x38, \|= 0x04` | DAC channel enable/trigger config |

PA4/PA5 become DAC outputs (siggen). Not FPGA straps.

### Group 5 — **Analog-frontend relay/gain bank** (0x0802C53C–0x0802C658) ← KEY

At 0x0802C53C, `r4 = 0x200000F8` (meter_state). Two helper calls drive the relays from RAM fields:

| objdump | call | arg source | RAM boot default |
|---|---|---|---|
| 0x0802C546 | `gpio_mux_portc_porte(r0)` (0x080088A4) | `r0 = [0x200000FA]` (meter_state[2]) | **0x05** |
| 0x0802C54C | `gpio_mux_porta_portb(r0)` (0x08008A58) | `r0 = [0x200000FB]` (meter_state[3]) | **0x05** |

**`gpio_mux_portc_porte(5)` (case-5 path 0x080088F2→0x0800896C) drives:**
| target reg | write | pin/level |
|---|---|---|
| GPIOC_BRR 0x40011014 | `=0x1000` | **PC12 LOW** (probe-route relay) |
| GPIOE_BSR 0x40011810 | `=0x10` | **PE4 HIGH** |
| GPIOE_BSR 0x40011810 | `=0x20` | **PE5 HIGH** |
| GPIOE_BRR 0x40011814 | `=0x40` | **PE6 LOW** |

**`gpio_mux_porta_portb(5)` (case-5 path 0x08008AD2→0x08008B7A) drives:**
| target reg | write | pin/level |
|---|---|---|
| GPIOA_BSR 0x40010810 | `=0x8000` | **PA15 HIGH** |
| GPIOB_BSR 0x40010C10 | `=0x800` | **PB11 HIGH** ← FPGA "active mode" |
| GPIOB_BSR 0x40010C10 | `=0x400` | **PB10 HIGH** |
| GPIOA_BSR 0x40010810 | `=0x400` | **PA10 HIGH** |

Both helpers then (tail) compute a per-range DAC1 comparator value into `0x40007404` — that path is
gated by `meter_state[0x2D]` (boot default 0x0D ≥ 5), so the DAC write executes, but it is internal
(no external pin). The **relay GPIO writes above always execute regardless** of the cal-table branch.

Also in this block (0x0802C58E, 0x0802C604): two conditional relay nudges keyed on
`meter_state[0]` / `meter_state[1]` (both boot-default 0x00) and `meter_state[0x14]` (boot-default
0x03 → no extra relay write on the captured default). With defaults, no additional pins move here.

### Group 6 — USART2 + its pins (0x0802C66E–0x0802C6F0, config only)

`fp = USART2_CR1 0x4000440C`. PA2/PA3 configured (PA2 MUX/AF-PP for TX, PA3 input for RX), BRR set
for 9600 baud, `TE\|RE\|RXNEIE` set, **UE explicitly CLEARED**. Per `usart_boot_frames_exact.md`,
**USART2 stays disabled (UE=0) through the entire pre-FPGA window** — the MCU sends/receives nothing
on USART2 before (or during) the bitstream upload. NVIC IRQ38 enabled, but peripheral is dark.

### Group 7 — **AFIO JTAG-disable remap** (0x0802C764–0x0802C77A) ← frees SPI3 pins

| objdump | reg | write | meaning |
|---|---|---|---|
| 0x0802C764 | AFIO+0x08 (0x40010008) | `&= ~0xF000` | clear SWJTAG mux field |
| 0x0802C776 | 0x40010008 | `\|= 0x2000` | set SW-DP-only (disable JTAG-DP) → **frees PB3/PB4/PB5 for SPI3** |

Followed (0x0802C77C–0x0802C7AC) by EXTI/EXTICR config clears and an `EXTICR` field set — the
external-interrupt routing for the FPGA-ready / button lines. (Bit-level EXTI line not fully decoded;
not an output-pin change.)

### Group 8 — SPI3 init + FPGA control pins (0x0802D50A–0x0802D63C) ← end of window

| objdump | target | write | pin/level |
|---|---|---|---|
| 0x0802D5BA | GPIOB_BSR 0x40010C10 | `=0x40` | **PB6 HIGH** (SPI3 software CS idle-high) |
| 0x0802D580/594/5A6/5B6 | GPIOE | PE3/PE4/PE5/PE6-area struct cfg | SPI3-adjacent pin modes (PB3/4/5 set MUX/AF for SPI3 elsewhere in this block) |
| 0x0802D5E8 | SPI3_CR1 (0x40003C0C, via `[r6,#-4]`) | `\|=0x02, \|=0x01` | CPOL=1 / master config |
| 0x0802D604 | SPI3_CR1 (0x40003C00+0xC00) | `\|=0x40` | **SPE — SPI3 ENABLE** |
| 0x0802D634 | GPIOC | PC6 cfg OUTPUT PP strong | (re-asserts) |
| 0x0802D63C | GPIOC_BSR 0x40011010 | `=0x40` | **PC6 HIGH** (FPGA SPI/enable strap) |

Immediately after 0x0802D63C the SPI3 handshake prelude begins (CS↓ `00`, the 100 ms SysTick
waits, then the `05/12/15` config bytes and the `0x3B` bitstream open — per `usart_boot_frames_exact.md`
§3).

---

## (b) Replication checklist — final pre-FPGA state of every externally-visible pin

State of each FPGA-relevant / analog pin at the instant the first SPI3 bitstream byte goes out:

| Pin | Stock state before SPI3 | Set by |
|---|---|---|
| **PC9** | OUTPUT PP, **HIGH** (power hold; asserted pre-main) | reset stub + 0x0802AAB2 |
| **PC6** | OUTPUT PP, **HIGH** (FPGA SPI/enable strap) | 0x0802D63C |
| **PB6** | OUTPUT PP, **HIGH** (SPI3 CS idle) | 0x0802D5BA |
| **PB3/PB4/PB5** | **MUX/AF** (SPI3 SCK/MISO/MOSI), enabled via JTAG-disable remap | AFIO 0x0802C776 + SPI3 cfg |
| **PB11** | OUTPUT PP, **HIGH** (FPGA active mode) | relay range-5, 0x08008AE8 |
| **PC11** | OUTPUT PP — **NOT driven HIGH pre-FPGA** (meter-MUX HIGH only happens *after* the upload, in the mode==meter branch at file 0x26F8E). Effective level: whatever the OUTPUT register holds = **LOW** (BSS/0). | config only |
| **PC12** | OUTPUT PP, **LOW** (probe route) | relay range-5, 0x0800896C |
| **PE4** | OUTPUT PP, **HIGH** | relay range-5 |
| **PE5** | OUTPUT PP, **HIGH** | relay range-5 |
| **PE6** | OUTPUT PP, **LOW** | relay range-5 |
| **PA15** | OUTPUT PP, **HIGH** (gain sel) | relay range-5 |
| **PA10** | OUTPUT PP, **HIGH** (gain sel) | relay range-5 |
| **PB10** | OUTPUT PP, **HIGH** (gain sel) | relay range-5 |
| **PB9** | OUTPUT PP (configured as output in the GPIOB cluster; level = OUTPUT-reg default LOW unless a relay case sets it) | config; not moved by range-5 |
| **PA6** | OUTPUT PP (configured output; level LOW on default range) | config; not moved by range-5 |
| **PA2/PA3** | PA2 = AF-PP (USART2 TX), PA3 = input (USART2 RX). **USART2 UE = 0 (peripheral disabled).** | 0x0802C66E |
| **PC0** | (FPGA→MCU ready) input; EXTI line config in Group 7. Not an output. | EXTICR |
| **PC1/PC2** | not explicitly driven in this window (probe-atten lines; left at config default) | — |
| **PD12/PD13** | not written in this window (PD4/5/6/7/11 are EXMC; PD12/13 unconfigured here) | — |
| **PE2/PE3, PA7, PB0, PC5, PC8, PC10, PC13, PB7** | button matrix (Group 2): rows/cols OUTPUT, PC8/PC13 input-pull-up, PB7 re-driven OUTPUT | Group 2 |
| **PB8** | OUTPUT PP, HIGH (LCD backlight) — set later in display init, but configured here | 0x0802AB2E |

**Relay/gain summary for boot range index 5 (the one the FPGA sees at config time):**
`PC12=L  PE4=H  PE5=H  PE6=L  |  PA15=H  PA10=H  PB10=H  PB11=H`.

---

## (c) Diff vs our replacement firmware

Sources read: `firmware/src/main.c` (boot section ~L357–422), `firmware/src/drivers/fpga.c`
(`fpga_init` pre-upload ~L1690–1730, `fpga_set_meter_frontend_baseline` L784–802),
`firmware/src/drivers/button_scan.c` (init).

| Pin | Stock pre-FPGA | Ours pre-FPGA (boot + fpga_init upload path) | Verdict |
|---|---|---|---|
| PC9 | OUTPUT HIGH | OUTPUT HIGH (main.c L359–360) | ✅ match |
| PC6 | OUTPUT HIGH | OUTPUT HIGH (main.c L368, fpga.c L1709) | ✅ match |
| PB6 | OUTPUT HIGH (CS idle) | driven by SPI3 framing | ✅ effectively match |
| PB3/4/5 | MUX/AF SPI3 (JTAG remap) | same (SPI3 init) | ✅ match |
| **PB11** | **HIGH** (relay range-5) | **LOW** (main.c L377, fpga.c L1730) until arm edge | ❌ **opposite level** |
| PC11 | LOW (output, undriven-high path) | LOW (main.c L379, fpga.c L1714) | ✅ match |
| **PC12** | **LOW** (driven) | **FLOATING** (never configured pre-upload) | ❌ **stock drives, we don't** |
| **PE4** | **HIGH** (driven) | **FLOATING** | ❌ stock drives, we don't |
| **PE5** | **HIGH** (driven) | **FLOATING** | ❌ stock drives, we don't |
| **PE6** | **LOW** (driven) | **FLOATING** | ❌ stock drives, we don't |
| **PA15** | **HIGH** (driven) | **FLOATING** | ❌ stock drives, we don't |
| **PA10** | **HIGH** (driven) | **FLOATING** | ❌ stock drives, we don't |
| **PB10** | **HIGH** (driven) | **FLOATING** | ❌ stock drives, we don't |
| **PB9** | OUTPUT (LOW on range-5) | FLOATING pre-upload | ⚠️ stock configures as output, we leave floating |
| **PA6** | OUTPUT (LOW on range-5) | FLOATING pre-upload | ⚠️ stock configures as output, we leave floating |
| PA2/PA3 | AF-TX / input, **UE=0** | configured; UE per our usart init | ✅ (UART irrelevant to upload) |

Notes:
- Our `fpga_set_meter_frontend_baseline()` (fpga.c L784) *does* set PC12/PE4/PE5/PE6/PA15/PA10/PB10
  (plus PB9/PA6) to stock-like levels — but it is invoked on **meter entry**, NOT in the boot
  bitstream-upload path. During `fpga_init`'s 0x3B upload, none of those relay pins are configured;
  GPIOA/B/E for those bits are at reset (floating input) unless another init touched them.
- Our PE4/5/6 baseline matches stock's range-5 (E4=H, E5=L? — *note*: fpga.c L794 sets PE5 **LOW**,
  but stock range-5 sets PE5 **HIGH**). Even the meter baseline disagrees with stock on PE5.

### Highest-value differences (act on these first)

1. **The entire analog-frontend relay/gain bank is floating during the bitstream upload.** Stock has
   PC12/PE4/PE5/PE6/PA15/PA10/PB10 all actively driven (range index 5) **before** the 0x3B open. If
   the Gowin samples any of these as configuration straps or as the analog environment it validates
   against, our floating pins are a plausible reason the bitstream is rejected and the NV meter
   design never announces. **Fix:** replicate the range-5 levels (table above) right after PC6 HIGH
   and before the prelude, by calling the equivalent of `gpio_mux_portc_porte(5)` +
   `gpio_mux_porta_portb(5)`.

2. **PB11 polarity is inverted vs stock at config time.** Stock holds PB11 **HIGH** from the relay
   range-5 write (well before the handshake); we hold it **LOW** until an arm edge. If PB11 is an
   FPGA mode/strap input sampled during config, this is a direct conflict. (The #18 capture's "PB11
   rises 1 ms before handshake" may be a *second* toggle, but the relay write puts it HIGH much
   earlier — re-examine whether stock's PB11 is HIGH-throughout vs pulsed.)

3. **PB9 and PA6** are configured as **outputs** by stock in this window (CLAUDE.md already flags them
   as "undocumented analog-frontend control"); we leave them floating until meter mode. Drive them to
   the range-5 level (LOW) pre-upload.

4. **PE5 level mismatch even in our meter baseline** (we set LOW, stock range-5 sets HIGH) — worth
   correcting in `fpga_set_meter_frontend_baseline()` regardless.

---

## Appendix — `.data` boot defaults (decompressed Keil `__decompress1`)

Scatter entry `{src=0x080BE480, dst=0x20000000, len=0x1240, handler=__decompress1@0x080071C0}`.
Reimplemented decompressor reproduces the doc anchor exactly (`...aa aa | 64 | aa 55 05 00.. | 0a 40`)
and yields len=0x1240. `meter_state` (base 0x200000F8) defaults:

```
ms[0x00]=00  ms[0x01]=00  ms[0x02]=05  ms[0x03]=05  ms[0x04]=32
ms[0x14]=03  ms[0x16]=00  ms[0x17]=01  ms[0x18]=00  ms[0x2D]=0D
state[0x20000F64] (saved last-mode) = 00   →  master-init mode dispatch takes the
   "mode 0 / no valid save" branch = meter path (UE set, PC11 HIGH) AFTER the upload.
```

- `ms[0x02]=0x05` and `ms[0x03]=0x05` are the range arguments to the two relay helpers → range
  index **5** is the boot-default frontend posture documented above.
- `ms[0x16]=00 / ms[0x17]=01 / ms[0x18]=00 / ms[0x2D]=0D` match `usart_boot_frames_exact.md` §Appendix,
  independently validating the decompressor.
- These defaults apply **only if SPI-flash settings are absent/invalid** (magic-byte check at file
  0x25D92 — first flash byte must be 0x55 or 0xAA). On a factory/erased unit the `.data` defaults
  stand; on a configured unit the saved range may differ, but the relay helpers still run with
  whatever range the flash provides — they are never skipped.
