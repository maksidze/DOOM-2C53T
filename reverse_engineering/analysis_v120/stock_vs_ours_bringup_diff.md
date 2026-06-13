# Stock vs Ours: FPGA Bring-Up Diff (Synthesis)

**Date:** 2026-06-13
**Goal:** Make our firmware **byte-identical to stock on the FPGA bring-up path** (power-on → clock → GPIO → SPI3/USART2 config → 0x3B/0x3A bitstream upload → post-config scope writes → first 0x04/0x05 acquisition read), so the Gowin GW1N-UV2 enters SSPI config-receive and accepts the bitstream we replay.

**Success signal:** `fpga` SSPI register read `0x11 IDCODE → 01 20 68 1B` (FPGA in config-receive). Secondary: `0x3A` close returns **0xF8** (stock accept) and `0x03` status returns **00 01 42 2E (2E)**. Today the FPGA drives MISO LOW (0x00) during 0x3B and 0x80 during the 0x05 prelude — the running NV (meter) design owns the SSPI balls and never re-enters config.

**Bench tooling:** all sweeps run live from the USB-debug shell via `fpga reinit <knobs>`, which calls `fpga_spi3_config_sequence(const fpga_cfg_seq_opts_t *opt)` (decl: `firmware/src/drivers/fpga.h:228-277`, impl: `firmware/src/drivers/fpga.c:1460`). Knobs available **without reflashing**: `upload_br`, `cmd_br`, `prelude_gap_ms`, `pre_upload_gap_ms`, `post_close_ms`, `arm_pb11`, `reset_port`/`reset_pin`/`reset_low_ms`, `prelude_frame_mode`, `strap_pd2`, `strap_pd1213`, `trailing_clocks`. Raw byte poke: `spi3 xfer <bytes>`. Readbacks land in `fpga.cfg_status_reg[]` (0x41), `fpga.h2_close_status` (0x3A), `fpga.scope_status[]` (0x03).

---

## TL;DR — what actually matters

The NV-release assessment (below) concludes there is **no MCU-commanded path** to make the running NV meter design relinquish the SSPI port. Stock re-enters config by something **invisible to all 8 captured channels** (most consistent with the GW1N's internal power-on config-controller state). **The single decisive next step is external JTAG SRAM-load of the apicula R1 validator** (M0–M3/VPP header), which decouples "is the part reconfigurable at all" from "can the MCU trigger it via SSPI."

That said, several HIGH-relevance MCU-side divergences are cheap to eliminate first and must be cleared before declaring the wall purely internal. They cluster into three families:

1. **USART2 must be SILENT during the whole bring-up** (stock holds UE clear; we enable it early and inject frames). This is the strongest "we are perturbing the NV design" lever.
2. **SPI3 clock rate / SSPI read reliability** (stock = /4 = 30 MHz per objdump; we run /2 = 60 MHz; the SSPI **read** path is clock-limited — IDCODE is garbage at /2, clean at /256).
3. **Extra/missing wire events around the 0x3B/0x3A window** (our 0x41 status read + 0x00 flush clocked with CS HIGH; the prelude framing/gap differences; trailing-clock count).

---

## Ranked divergence table (config_relevance HIGH → LOW)

> Sorted by `config_relevance`. "Bench test" = exact `fpga reinit` knob or a named one-line firmware change. Watch `fpga.cfg_status_reg` for `01 20 68 1B` on a `0x11` read.

### HIGH relevance

| # | Unit | Target | Stock | Ours | Bench test |
|---|------|--------|-------|------|------------|
| 1 | usart2-config / post-init / settings-load | **USART2 CR1 UE** | UE held **CLEAR** through all of master init; set only post-upload AND only if last-mode==meter/no-save. USART2 **dead** on a scope boot. | `fpga.c:1772` sets UE **unconditionally** in `fpga_init`, before the SPI3 upload; dvom_TX/dvom_RX always created, never suspended; meter_poll runs. | **New firmware gate**: skip the `UE |= (1<<13)` (and RDBFIEN, and dvom task create) on the scope-boot path; re-run `fpga reinit`. Watch whether the 0x05 prelude MISO changes from 0x80→0xFF (NV design stops owning pins). This is the **#1 lever**. |
| 2 | usart2-config / spi3-bitstream / command-trace | **USART boot commands 0x01/0x02/0x06/0x07/0x08** | Stock sends **ZERO USART frames at boot.** These codes are 1-byte **SPI3-queue** items posted AFTER the upload → realized as the five SPI3 *register writes* (01 08 / 02 03 / 06 00 / 07 00 / 08 AD). | `fpga.c:1813-1822` sends them as **USART2 frames** (Step 3b) BEFORE the SPI3 phase; plus extra meter bring-up frames (0x0508/0509/0507/0514) in Step 8. | **New firmware gate**: compile out Step 3b + Step 8 USART traffic. Re-`fpga reinit`. Commanding the NV design over USART plausibly flips its user SPI slave onto the SSPI pins (our 0x80 vs stock 0xFF). |
| 3 | spi3-periph-init | **SPI3 CR1 BR prescaler** | Objdump struct field `0x100` → **BR=001 = /4 = 30 MHz** SCK. (CLAUDE.md folklore "/2" is contradicted by the struct.) | `fpga.c:1907-1916` writes CR1 `0x0307` → **BR=000 = /2 = 60 MHz**. We clock 2× faster than stock. | `fpga reinit upload_br=1 cmd_br=1` (/4). Then sweep `cmd_br=3` (/16) and `cmd_br=5` (/64) — the SSPI read path is clock-limited. Watch `0x11 IDCODE` read for `01 20 68 1B`. |
| 4 | spi3-config-writes-queue / spi3-bitstream | **0x41 STATUS read injected** | Stock never issues 0x41 anywhere; only a 0x03 read after the 5 config writes. | `fpga.c:1620-1633` injects 0x41 + pads + readback, INCLUDING a **bare 0x00 clocked with CS HIGH** (8 stray SCK edges, CS deasserted) right after 0x3A close. | **Firmware**: disable the 0x41 diagnostic block; re-`fpga reinit`. CS-HIGH clocking is exactly the SSPI-desync mechanism the wire-exact rewrite tried to remove. |
| 5 | spi3-bitstream / spi3-config-writes | **Inter-frame idle bytes (CS HIGH clocking) — polarity mismatch** | Stock clocks a single **0x00 idle byte with CS HELD HIGH** after several frames (open/05/12/3B-close/3A/final). | We clock **nothing** with CS HIGH (deliberately removed at `fpga.c:1527-1532`) — but then our 0x41 block + 0x00 flush DO clock with CS HIGH (contradiction). | **Firmware A/B**: (a) re-add the stock CS-HIGH idle bytes at each frame boundary; (b) ensure NO other CS-HIGH clocking exists. Diff our wire vs stock #18 capture. Open question whether the SSPI FSM is CS-gated. |
| 6 | spi3-bitstream | **Extra 100ms gap before 0x15 CONFIG_ENABLE** | Stock has gaps after 05 and after 12 only — **no** gap before 0x15. | `fpga.c:1578` inserts a 3rd `prelude_gap_ms` before the 15-frame. | `fpga reinit prelude_gap_ms=0` to collapse, or set the pre-15 gap to 0 in firmware. Also sweep `pre_upload_gap_ms`. |
| 7 | spi3-config-writes / spi3-bitstream | **0x00 flush frame after 0x3A** | Not in the modeled stock action list for this unit. | `fpga.c:1615-1618` sends a standalone CS-LOW single-0x00 frame right after 0x3A. | **Firmware**: remove the flush frame; re-`fpga reinit`. Any byte into the SSPI FSM at config-close can desync the post-config parser. |
| 8 | post-init / tasks-created | **PB11 (FPGA active strap) rise timing** | LOW from t=0, raised HIGH **~1ms before** the handshake (a true LOW→HIGH arming edge), held. | Historically floated→pull-up HIGH (no edge). Now `arm_pb11` raises it; `FPGA_WARM_HANDOFF_TEST` raises it at top of main (seconds early). | `fpga reinit arm_pb11=1` with PB11 driven LOW from boot first (so a real edge exists). Confirm on scope: PB11 LOW→HIGH exactly 1ms pre-prelude. |
| 9 | scheduler-start / spi3-bitstream | **Config writes pre-scheduler vs post-scheduler** | The 5 config writes + 0x3B upload run **inside fpga_task AFTER vTaskStartScheduler** (RTOS-tick-quantized gaps). | `fpga_spi3_config_sequence` runs **synchronously in `fpga_init` before the scheduler** (busy-wait gaps). | Lower priority than 1–8; first eliminate the wire-level diffs. If wire matches and config still fails, defer the upload into the task post-scheduler and retest. |
| 10 | spi3-bitstream-3b-3a | **0x3B payload (historical bug, now fixed — VERIFY)** | 115,638-byte Gowin `.fs` at **file offset 0x4AD19**, IDCODE 0x0120681B, sha256 `5a0e7338…`. | Was built from wrong offset 0x51D19 (garbage) for months; `fpga_cal_table.h` regenerated from 0x4AD19. | **Verify before any sweep**: `shasum` the embedded table / check `FPGA_H2_CAL_TABLE_SIZE == 115638` and the leading bytes carry the `.fs` preamble. A wrong payload makes every other sweep meaningless. |
| 11 | scheduler-start | **PD2 / PD12-13 strap + reset pulse** | Stock pulses **no** RECONFIG pin; PD2/PD12-13 are our hypotheses, not stock behavior. | `fpga.c:1476-1513` optionally holds PD2/PD12/PD13 or pulses a reset pin. | `fpga reinit strap_pd2=1` (hold HIGH) / `=2` (hold LOW); `strap_pd1213=1/2`; `reset_port=4 reset_pin=2 reset_low_ms=10` etc. Sweep candidate config straps. (All prior pin candidates PC4/PD3/PD2/PB9/PA6/PD6 bench-NEGATIVE — low expectation.) |
| 12 | spi3-bitstream-3b-3a | **Trailing clocks after last byte, before 0x3A** | Capture shows **0** trailing clocks (stock-faithful). | `trailing_clocks` knob (rosenrot00's 2C23T loader clocks ~200). | `fpga reinit trailing_clocks=200` then `=512`. Gowin runs CRC/DONE/wakeup on CCLK after the final config byte. |

### MEDIUM relevance

| # | Unit | Target | Stock | Ours | Bench test |
|---|------|--------|-------|------|------------|
| 13 | spi3-bitstream-3b-3a | 0x3B pump cadence | per-byte loop (small inter-byte stretch) | `spi3_pump` gap-free continuous clock | Bench note (fpga.c:262) says /2 vs /16 made no difference; low expectation. Sweep `upload_br` anyway. |
| 14 | post-init / settings-load | PC11 (meter MUX) polarity vs boot mode | HIGH on meter boot, LOW on scope boot (mode-dispatched) | LOW unconditionally (scope posture) | Correct for scope boot. Only matters if we ever boot meter posture. |
| 15 | spi3-prelude | 100ms gap mechanism (yield vs busy-wait) | chunked SysTick busy-wait (no CPU yield) | `fpga_scope_delay_ms` may `vTaskDelay`-yield → other tasks may touch the bus | Ensure acq_task is suspended (line 1468) AND USART tasks gated (#1) so nothing else drives the bus during gaps. |
| 16 | spi3-periph-init | CR2 bits 0,1 semantics | labeled RXNEIE/TXEIE (likely RXDMAEN/TXDMAEN) | single write 0x03; we run polled | Register content identical; verify against spi_init helper disasm. |
| 17 | usart2-config | TX frame header [0][1] | `0xAA 0x55` baked in .data | `0x00 0x00` (our frame[10]={0}) | If we keep ANY USART traffic, fix header to AA 55 + field order ([2]=param,[3]=cmd). |
| 18 | first-acquisition-read | frame status/sample split | 1+priming+1024 = 1026 | 1+2 status+1023 = 1026 | Total 1026 matches; one-byte alignment risk. Re-decode after config entry succeeds. |
| 19 | exmc-lcd-systick | PB9 (undoc frontend) HIGH vs stock LOW | LOW (output-reg default) | HIGH (`fpga.c:2052-2054`) | PB9 shares GPIOB with SPI3/PB11; drive LOW and retest as a strap. |
| 20 | post-init | osc/fpga task split | separate `osc`(p2) + `fpga`(p3) tasks | single combined task | Scheduling cadence diff; revisit only if config entry solved. |
| 21 | tasks-created | Timer1 (10ms autoreload) missing | present | nearest is 500ms health | Unknown if it paces FPGA housekeeping. |
| 22 | scheduler-start | merged bitstream+config block timing | split master-init vs scheduler phases | one synchronous block | Same as #9. |

### LOW relevance (CRT / clock / non-FPGA pins — listed for completeness)

`reset-vector` & `clock-init`: CPSIE I deferral, CRM_CTRL/MISC mask differences, auto_step ramp, VTOR ordering, scatterload vs GCC crt, FLASH_PSR wait-state (unconfirmed in both) — all converge to 240 MHz with no FPGA-pin effect. `powerhold-gpio-clocks` & `static-gpio-config`: PC9 power-hold glitch, PB8 backlight drive/timing, button-matrix dynamic-scan model, PD pin EXMC fidelity — none touch FPGA pins. `frontend-coupling-relays`: PD12/PD13 coupling and PC1/PC2 attenuation never driven (UI-only) — note PD12=EXMC A17 conflict (LCD) on our side. `queues-semaphores` & `semaphore-init-state`: missing fpga_sem1/sem2 (we use queue+meter_sem); pure software. `afio-jtag-remap` & `spi3-gpio-pc6`: AT32 GMUX writes (SPI3_GMUX_0010) — **required silicon difference**, bench-confirmed necessary; not a defect.

---

## Command-trace comparison (stock wire vs ours)

**Stock USART2 (ground truth, usart_boot_frames_exact.md + issue #18):**
- **ZERO USART2 frames during boot.** UE held clear; the MCU cannot even receive. The `0x01/0x02/0x06/0x07/0x08` bytes are FreeRTOS SPI3-queue items (handle 0x20002D78) posted AFTER the upload, realized as SPI3 **register writes** by fpga_task.
- 5A A5 (12B data) / 5A 69 (11B status) frames at t=2.77–3.59s are **unsolicited NV-design output** into the disabled USART2 — ignored.
- Runtime TX (meter mode only, after UE): 10 bytes `[AA 55 param cmd 00×5 checksum]`.

**Stock SPI3 sequence (issue #18 capture):**
1. `[t=3.6072]` PB11 → HIGH, 1.0ms before SPI. PC6 already HIGH.
2. Bare CS pulse (CS↑→CS↓ zero clocks→CS↑) = SSPI frame-sync.
3. wait ~100ms → CS↓ `05 00` CS↑ (MISO FF).
4. wait ~100ms → CS↓ `12 00` CS↑.
5. wait ~100ms → CS↓ `15 00` CS↑. (05/12/15 = CONFIG_ENABLE prelude.)
6. CS↓ `0x3B` + full **115,638-byte** bitstream (file 0x4AD19, IDCODE 0x0120681B) at /2... (MISO all 0xFF = config-receive/high-Z) CS↑.
7. CS↓ `0x3A 00` → MISO **0xF8** (accept) CS↑.
8. CS↓ `0x00` single flush CS↑.
9. wait ~607ms (scheduler+wakeup, not a coded delay).
10. Five 2-byte writes, each own CS frame: `01 08 / 02 03 / 06 00 / 07 00 / 08 AD`.
11. Status read CS↓ `0x03` + 4×FF → MISO `00 01 42 2E (2E)` CS↑.
12. Acquisition loop: per-channel `0x04`(CH1)/`0x05`(CH2), 1026-byte reads, ~29ms cadence, **NOT interleaved**, USART silent.

**Where ours differs (worst → least):**
- **USART:** we send 0x01–0x08 as **USART frames** pre-SPI3 (stock: never on USART). UE enabled early (stock: clear). Header 00 00 (stock: AA 55).
- **Bench observation (the wall):** during 0x3B our MISO = **0x00** (FPGA NOT receiving); stock = 0xFF (high-Z, receiving). On the very first `0x05` prelude our MISO = **0x80** (NV user design owns pins/running) vs stock 0xFF. Our NV-booted FPGA never re-enters config-wait → 0x3B ignored, 0x3A returns 00/FC not 0xF8.
- **Clock:** /2 (60 MHz) vs stock /4 (30 MHz). SSPI read path clock-limited (IDCODE garbage at /2).
- **Extra wire events:** 0x41 status read + CS-HIGH 0x00 clock; extra 0x00 flush; 3rd prelude gap; trailing_clocks=0.
- **PB11 edge:** stock has true LOW→HIGH 1ms pre-handshake; ours historically floated (no edge).
- **Payload (fixed, verify):** was file 0x51D19 garbage; now 0x4AD19. Confirm sha `5a0e7338…`.

---

## NV-release assessment

**Question:** can the running NV (meter) design be COMMANDED to release the SSPI port / re-enter config from the MCU, without a RECONFIG_N pulse (which maksidze measured never fires)?

**Verdict: NO viable MCU-commanded path exists.**

- **(a) User fabric exposes no config/reconfig command or strap.** Apicula unpack (R3/R4): the user design = 2 scope channels + DMM + sig-gen + a 383-DFF capture control cone; **zero config-strap logic** (that lives in the GW1N config controller, not the unpacked fabric). The SSPI balls are repurposed as ordinary user I/O (SCLK=PB3, SO=PB4, SI=PB5, CS_N=PB6) while the design runs — which is exactly why 0x05/0x12/0x15 lands on deaf ears. The runtime SPI register bits (01 08 / 02 03 / …) gate **capture run/arm** (IOR1B ∧ IOB7B ∧ SPI-bit AND), not configuration. Relinquishing the port requires the config controller to seize it back = a power-on/RECONFIG event, not a user-fabric action.
- **(b) Gowin SSPI 0x3C RELOAD is NOT a missing lever.** Per openFPGALoader `gowin.cpp`, `Gowin::reset()=RELOAD+NOOP`, issued only AFTER writing **internal NV flash**, to re-read the freshly-written flash image. RELOAD makes the FPGA re-read its **own NV flash (the meter image)** then go to user mode — it does **not** accept an MCU-streamed SRAM bitstream. Stock's actual boot uses 05/12/15→3B→3A→03 with **no 0x3C** anywhere.
- **(c) Config-entry is gated by the GW1N's internal config-controller power-on state, not a pin we can find.** In the entire SPI3-init→0x3B window stock's **only** GPIO write is PC6 HIGH. No RECONFIG/PROGRAMN toggle exists (matches maksidze's "RECONFIG_N never pulses"). Exhaustive MCU-side campaign (Experiments 1–14) eliminated bytes, SPI mode, clock /2../64, reset pins PC4/PD3/PD2 + PB9/PA6/PD6 (all NEGATIVE), USART silence, CS framing, range-5 frontend. A control build that never touches SPI3 still saw zero UART announce frames — yet the same NV design wakes fully at runtime (PB11 HIGH + PC11 HIGH + AA55 USART cmd → continuous 5A A5). So the user design is alive and reachable; its **config controller is not re-enterable from any observable MCU action**.

**Mechanism candidates & how to test:**
1. *0x3C RELOAD only reloads the meter image* — RULED OUT by gowin.cpp semantics. Formally close cheaply: `spi3 xfer 3c 00` then `02 00` (NOOP), read `0x03`. Prediction: ignored or reboots into meter (5A A5 resumes); acq stays zero.
2. *User design can't release SSPI by command* — re-confirm via apicula netlist grep for any user-SPI-bit → CONFIG/PROGRAMN/RECONFIG path (expect none). Bench: sweep every byte of the post-config writes while reading 0x3A; prediction = close status never flips to 0xF8.
3. **DECISIVE — external JTAG SRAM-load** of the apicula R1 validator (`scope_R1_validator.fs`, **SRAM-only, never `-f`**) via M0–M3/VPP header: `openFPGALoader -c ft232 --detect` (expect IDCODE 0x0120681B), then `-m` load. If DONE goes HIGH and 0x04/0x05 returns the baked ramp / A5 5A marker → the part **is** reconfigurable and the problem is definitively "SSPI cannot re-enter config on a running NV design from the MCU." Single cleanest proof/disproof.
4. *RECONFIG_N on an untraced gold pad, controller-driven* (competes with 3) — maksidze recapture with a probe on each of the 5 gold pads / QN48 RECONFIG candidate during a stock cold boot, time-aligned to 0x3B. If a pad pulses LOW coincident with config-entry and NOT on the 8 MCU lines → that's the lever (controller-driven). If none → mechanism 3 stands.
5. *Disambiguate stock wire == ours* — capture OUR firmware's SPI3 (PB3/4/5/6 + PC6 + PB11) at ≤1MHz with the HiLetgo analyzer, diff edge-for-edge vs `reverse_engineering/captures/SPI3_2C53T.sal`. The cold-boot campaign matched every *derived* variable but never measured our actual wire.

---

## Coverage gaps / unexplained

**Units where our path is entirely absent (`ours_present=false`) — implement to reach byte-parity:**
- **`frontend-relay-range5`** — no boot-time range-indexed frontend selection. Stock loads range index 5 from meter_state and calls `gpio_mux_portc_porte(5)`/`gpio_mux_porta_portb(5)`, which **drives PB11 HIGH as a side effect** plus the full relay/gain posture, before the 0x3B handshake. We hardcode a fixed meter-DCV pattern that never runs the range-5 path. **CONFLICT in sources** on range-5 levels for PE4/PE5/PE6 and PA15/PB10 (objdump doc vs decompiled case-5 body) — needs a fresh objdump of `0x080088A4`/`0x08008A58` case-5 paths to settle; load-bearing for the byte diff.
- **`frontend-coupling-relays`** — CH1/CH2 coupling (PD12/PD13) + probe attenuation (PC1/PC2) are UI-only enums with no GPIO path. PD12 conflicts with EXMC A17 (LCD) on our side. Physical relay mapping inferred, not schematic-confirmed.
- **`settings-load-cal`** — no SPI-flash saved-config load at boot. `config_load()` never called; `flash_fs_load_factory_cal()` is a no-op stub. **Cascades** into every downstream meter_state-driven divergence, including `ms[0xF64]` SAVED MODE which gates the only boot UE flip and the PC11 decision. This absence is the structural reason UE and PC11 are unconditional (see HIGH #1, #14).

**Units with `stock_unresolved` (RE still owes ground truth):**
- `reset-vector`/`clock-init`: **FLASH_PSR (0x40022000) wait-state write not found** in either — should be 4 for 240 MHz; mechanism unconfirmed (silicon auto-config?). `__scatterload` not disassembled; `__libc_init_array` ctor contents not enumerated; exact clock_init call site not pinned.
- `static-gpio-config`: button-input pull directions (PB7/PC8/PC13/matrix) **conflict between two sources** (April decompile vs objdump doc) — objdump treated as ground truth but per-call struct bytes not re-derived.
- `frontend-relay-range5`: range-5 relay-level conflict (above); `0x40001c34` = TMR2_CCRx vs comparator latch unpinned.
- `spi3-periph-init`: **prescaler ambiguity** (struct 0x100 → /4=30MHz vs CLAUDE.md "/2=60MHz") — confirm APB1CLK and BR bits on a scope; `spi_init` helper not read line-by-line; CR2 bits 0/1 semantics (DMA vs interrupt) unverified.
- `spi3-gpio-pc6`: APB1EN base offset (0x1018 vs 0x101C), PC6/PB6 BSR address aliasing, gpio_init CTLR nibble values not expanded.
- `spi3-prelude`/`systick-pre-prelude`: SysTick reload bases at 0x20002B1C/0x20002B20 are runtime-computed (only addresses known); SPI3 STS bit positions for AT32 not register-doc-verified; opcodes 0x05/0x12/0x15 Gowin semantics inferred.
- `spi3-bitstream-3b-3a`: instruction at file 0x26D9E loads `r8=0x40011014` before queue posts — unresolved (candidate frontend GPIO BRR); status bytes discarded in this block.
- `post-init`: TMR3/TMR6/TMR7 PSC/ARR not byte-resolved; TMR5 freq-measure regs unresolved; state[0xF64] 0-vs-1 handling not separated; PD2/PD3/PC4/PC0 not touched in this block.
- `first-acquisition-read`: Ghidra folds the MISO sample reads (shows 0xFF stores); CH2 wire source (separate 0x05 vs interleave) ambiguous between decompile and capture; needs objdump of 0x080375F8-0x08037640.

---

## Next-session actions (ordered, concrete)

1. **VERIFY the 0x3B payload first** (HIGH #10). `shasum`/inspect `firmware/src/drivers/fpga_cal_table.h`: size == 115,638, leading bytes carry the Gowin `.fs` preamble, sha `5a0e7338…`. If wrong, every sweep below is meaningless.
2. **Eliminate USART perturbation** (HIGH #1, #2). Build a scope-boot variant that holds USART2 UE CLEAR, does not create/wake dvom_TX/dvom_RX, and removes the Step 3b + Step 8 USART command bursts. Re-`fpga reinit`. Watch the first `0x05` prelude MISO: 0x80→0xFF would mean the NV design stopped owning the pins. This is the highest-yield MCU-side test.
3. **Clean the wire around the handshake** (HIGH #4, #5, #7, #6). Disable the 0x41 diagnostic block and the 0x00 flush; ensure NO CS-HIGH clocking; collapse the extra pre-0x15 gap (`prelude_gap_ms`/`pre_upload_gap_ms`). Re-`fpga reinit`.
4. **Sweep SPI3 clock** (HIGH #3). `fpga reinit upload_br=1 cmd_br=1` (/4=stock), then `cmd_br=3`,`cmd_br=5` for the read path. After each, do `spi3 xfer 11 00 00 00 00` and read `fpga.cfg_status_reg` for **`01 20 68 1B`**.
5. **PB11 arming edge** (HIGH #8). Drive PB11 LOW from boot, then `fpga reinit arm_pb11=1`; scope-confirm LOW→HIGH 1ms before the prelude.
6. **Strap/reset sweep** (HIGH #11, #12, MED #19). `strap_pd2=1/2`, `strap_pd1213=1/2`, `trailing_clocks=200/512`, then the `reset_port`/`reset_pin`/`reset_low_ms` candidates. Low expectation (all prior pins NEGATIVE) but cheap.
7. **Close 0x3C cheaply** (NV mech 1). `spi3 xfer 3c 00` + `02 00` + read `0x03`. Expect meter reload / no acq.
8. **DECISIVE hardware test** (NV mech 3). External JTAG via M0–M3/VPP: `openFPGALoader -c ft232 --detect` (expect IDCODE 0x0120681B), then SRAM-load `scope_R1_validator.fs` (`-m`, **never `-f`**). DONE HIGH + baked ramp on 0x04/0x05 ⇒ part is reconfigurable; problem isolated to SSPI config-entry from a running NV design.
9. **Capture OUR wire** (NV mech 5). Solder PB3/4/5/6 + PC6 + PB11, `fpga reinit upload_br=... `, capture at the HiLetgo, diff edge-for-edge vs `SPI3_2C53T.sal`. A prelude/CONFIG_ENABLE timing mismatch would be the first un-eliminated MCU finding; a clean match confirms the blocker is purely the FPGA's internal config-controller state → JTAG is the only route.
10. **Implement the missing units** for true byte-parity if 2–9 don't crack it: `frontend-relay-range5` (resolve the PE4/5/6 + PA15/PB10 source conflict via fresh objdump first), then the `settings-load-cal` meter_state restore so UE/PC11 become mode-dispatched like stock.

**Watch for `0x11 IDCODE → 01 20 68 1B` after every step — that is the moment the FPGA entered config-receive.**
