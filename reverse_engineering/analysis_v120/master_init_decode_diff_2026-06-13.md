# master_init decode + diff vs our firmware — 2026-06-13

**Method:** 14-region parallel decode-and-diff workflow (`master-init-decode-diff`, 15 agents, ~979K tokens). Each agent decoded one functional region of stock `master_init` (FUN_08023A50, 15.4KB) to register level and diffed it against our clean-room firmware, flagging early-boot FPGA actions we omit.

## Verdict

- **Decode:** D2  |  **Reimplement:** R0

- Decode: 13/14 regions are decode_level 3; the lone exception is DMA (region 'dma', level 2 — the USART2-RX DMA channel index is still unresolved to a concrete 0x40020000+offset and the 0x40005C00=USB vs SPI3 mislabel only partially closed). Honest min => master_init is NOT fully D3; it is D2 pending that one re-decode. Reimpl: R2 only if ALL meaningful regions are R2 — they are not. R2 regions: adc1, usart2_fpga, iwdg. But flash_cfg_cal is R0 (no saved-config read, no magic validation, no 40-entry scope/meter cal-table, no factory-cal defaults — only a single hardcoded per-device factor), and the SAME meter_state cal/mode state feeds the analog_frontend (R1, invented/inverted relay bits + entire DAC1 trigger-comparator path absent) and the spi3_fpga_handshake (R1, non-stock USART burst before upload in the default build) and freertos_objects (R1, missing fpga_sem1/sem2 handshake + merged osc/fpga task). The weakest load-bearing regions — flash_cfg_cal (R0), analog_frontend (R1), spi3_fpga_handshake (R1) — gate scope acquisition and FPGA bring-up, so the overall reimpl floor is R0.


> Honest correction: the baseline ledger over-credited master_init at D3/R1. The faithful figure is **D2/R0** — the *decode* is nearly complete, but our *reimplementation* floor is R0 because the saved-config + calibration-table restore path is entirely absent.


## Per-region scorecard

| Region | D | R | FPGA flags |
|---|---|---|---|
| clock_power_hold | D3 | R1 | 1 |
| gpio_banks | D3 | R1 | 4 |
| exmc_lcd | D3 | R1 | 2 |
| adc1 | D3 | R2 | 0 |
| usart2_fpga | D3 | R2 | 2 |
| spi3_fpga_handshake | D3 | R1 | 3 |
| timers_nvic | D3 | R1 | 3 |
| dma | D2 | R1 | 3 |
| freertos_objects | D3 | R1 | 3 |
| flash_cfg_cal — stock SPI-flash saved-config read + meter_state unpack + scope/meter cal-table load/defaults | D3 | R0 | 1 |
| button_matrix | D3 | NA | 0 |
| analog_frontend | D3 | R1 | 2 |
| i2c_touch | D3 | R1 | 0 |
| iwdg | D3 | R2 | 0 |

## Headline divergences (stock vs ours)

- **[HIGH] spi3_fpga_handshake** — Default build sends USART boot commands (0x01/02/06/07/08) BEFORE the 0x3B bitstream upload; stock sends ZERO USART before the upload. Commanding the NV user design plausibly switches the FPGA's user-mode SPI slave onto the shared SSPI pins (prelude MISO 0x80 user-mode vs stock 0xFF config-wait float), so the 0x3B CONFIG_ENABLE is ignored — the config-entry wall.
- **[HIGH] flash_cfg_cal** — Entire stock saved-config read + magic-byte validation + meter_state unpack + 40-entry scope/meter gain-offset cal table + ~800B factory-cal defaults is ABSENT. Our meter cal is one hardcoded per-device factor (0.0304f/0.001f) and config.c is an unrelated UI store. This is the R0 floor of the whole init.
- **[HIGH] analog_frontend** — Cal-table-derived DAC1 scope-trigger-comparator value (0x40007408 + enable 0x40007404|=1, plus second target 0x40001c34) is never computed or written. Relay bit pattern is invented and partly INVERTED vs the decoded FUN_080018A4 switch (ours PC12=H/PE4=H/PE5=L/PE6=H vs stock case-2 PC12=H/PE5=H/PE6=L, PE4 untouched). No saved-config-driven mode/range.
- **[HIGH] clock_power_hold** — PC9 power-hold POLARITY inverted: stock writes GPIOC->clr=0x200 (PC9 LOW) via the clear register; we write GPIOC->scr (PC9 HIGH). Same pin/cfg, opposite level AND opposite register. If PC9 gates an FPGA power/config-domain strap, our inverted level could change the Gowin's config-acceptance state.
- **[HIGH] gpio_banks / analog_frontend** — Stock drives the full analog-frontend relay/gain bank to range-5 levels (PC12/PE4/PE5/PE6/PA15/PA10/PB10/PB11 + PB9/PA6) DURING master_init, before the 0x3B open; our firmware leaves all of these floating through the bitstream-upload window (relay config deferred to meter-mode entry).
- **[MED] spi3_fpga_handshake** — USART2 fully enabled (RE/TE/RDBFIEN/UEN + NVIC + dvom/meter tasks) during scope boot in our default build; combined with the pre-upload command burst this keeps the user design active on the SSPI pins. (Note: the USART-silent bench build alone did NOT move the wall, so this is contributory, not sole cause.)
- **[MED] freertos_objects** — FPGA acquisition semaphore handshake (fpga_sem1 GIVEN, fpga_sem2 TAKEN) is entirely absent — replaced by a 1-byte trigger queue — and the stock 'osc' acquisition task is merged into our 'fpga' task. The two sems serialize config-vs-acquire; our merged single-task may interleave 0x3B/0x3A config and 0x04/0x05 reads with different ordering than stock.
- **[MED] dma** — Architecture mismatch on the FPGA DATA path: stock reads bulk scope samples over FSMC (0x60020000) via DMA1; we read scope data over SPI3 polling (returns constant 0xFF). If stock never carries scope samples on SPI3 at all, our SPI3-centric acquisition model targets the wrong bus.
- **[LOW] exmc_lcd** — AT32 IOMUX REMAP3 |= (1<<27) during EXMC bring-up is never written by us; also PD6 LCD hardware-reset pulse absent (live main.c path uses SWRESET cmd instead) and full ST7789 gamma/power init is skipped in the live path (the register-faithful lcd.c is dead code).
- **[MED] clock_power_hold (doc defect)** — Annotation address base is off by 0x7000 (master_init labeled 0x08023A50 but真 region is 0x0802AA50) — same file-vs-link confusion as the bitstream bug. Any doc keyed to 0x08023A5x master_init addresses is mis-addressed.

## Ranked FPGA config-entry hypotheses

Deduped + ranked across all regions by how plausibly each explains why our firmware can't get the GW1N-UV2 into SSPI config-receive (register proof: CONFIG_ENABLE never engages SYSTEM_EDIT_MODE; FLASH_LOCK set; bitstream byte-correct).


### #1 — Default build sends USART boot commands (0x01/02/06/07/08) to the FPGA BEFORE the 0x3B bitstream upload; stock sends ZERO USART pre-upload and boots straight into the SPI3 config handshake.

- **Region:** spi3_fpga_handshake
- **Why it might matter:** Most plausible direct cause of the config-entry wall: commanding the already-running NV user design switches its user-mode SPI slave onto the shared SSPI pins, so the GW1N is in user-mode (MISO driven 0x80) rather than config-wait (MISO floats 0xFF). With the SSPI port owned by the user design, CONFIG_ENABLE/0x3B is ignored — exactly matching 'CONFIG_ENABLE never engages SYSTEM_EDIT_MODE'. Exp 7 already showed silencing flips prelude 0x80->0xFF. The default build still violates this. Directly touches SPI3/USART ordering and FPGA pre-config state.
- **Test:** TRUE COLD power-cycle boot (not warm `fpga reinit`) with FPGA_USART_SILENT_SCOPE=1 vs =0; read fpga.init_hs prelude MISO via `status`. PASS = prelude MISO 0xFF + 0x11 IDCODE 01 20 68 1B + 0x3A close 0xF8 + scope status-read 03 -> 00 01 42 2E. Make FPGA_USART_SILENT_SCOPE=1 the default if confirmed.

### #2 — Stock drives the full analog-frontend relay/gain bank to range-5 levels (PC12=L, PE4=H, PE5=H, PE6=L, PA15=H, PA10=H, PB10=H, PB11=H, plus PB9=L, PA6=L) DURING master_init, BEFORE the 0x3B open; our firmware leaves all of these floating through the upload window.

- **Region:** gpio_banks
- **Why it might matter:** If the GW1N-UV2 samples any of these as configuration straps or as the analog environment it validates at config time, our floating pins are a plausible reason the bitstream is rejected / config never accepted. Changes FPGA pre-config pin state, directly in scope of the favored categories (PB11/relay pin states + sequencing relative to the handshake).
- **Test:** Insert a stock-faithful range-5 relay/gain drive (gpio_mux_portc_porte(5)+gpio_mux_porta_portb(5) equivalents + PB9=L/PA6=L) immediately after PC6 HIGH and before the 0x3B prelude in fpga.c; re-run the SPI3 handshake on a Saleae and watch for 0x3A close returning 0xF8 (vs current FF) and PC0 ready edge.

### #3 — PB11 ('FPGA active mode') timing: stock raises PB11 HIGH early (from the relay range-5 write, well before the handshake — capture shows 1.0ms before the bare CS pulse); our firmware holds PB11 LOW until an arm edge / only 1ms before the handshake.

- **Region:** gpio_banks / spi3_fpga_handshake
- **Why it might matter:** PB11 may be a mode/strap input the FPGA samples during config. An inverted/late level at config time could block config-entry. High-value because it is a PB11 pin-state-and-sequencing item, exactly the favored category, and is cheap to test alongside rank 2.
- **Test:** Drive PB11 HIGH before the 0x3B open (not just 1ms before the handshake); logic-analyze our SPI3 wire (PB3/4/5/6 + PC6 + PB11) on a cold boot and diff against the stock issue-#18 timeline; confirm PB11->HIGH lands 1ms before CS with no stray SCK edges while CS HIGH.

### #4 — PC9 power-hold polarity inverted: stock writes GPIOC->clr (PC9 LOW) at 0x0802AAB6; we write GPIOC->scr (PC9 HIGH).

- **Region:** clock_power_hold
- **Why it might matter:** If PC9 LOW is stock's intended steady state and it also straps/enables the FPGA's NV/config domain, driving it the opposite way could leave the Gowin in a mode that ignores config opcodes — matching the 'never enters SSPI config-receive' wall. Touches FPGA pre-config state. Ranked below the SSPI-pin items because PC9 is primarily a power-hold rail and the device stays powered either way, making an FPGA-strap role speculative.
- **Test:** STATIC: enumerate every GPIOC scr/clr bit9 write across the stock APP (7 scr, 25 clr movw 0x1010/0x1014 sites) to find PC9's settled level before scheduler start. BENCH: logic-analyze PC9 on a cold stock boot through ~2s. If stock settles PC9 LOW, flip ours to clr and re-run the SSPI config-entry litmus (PC0 arming).

### #5 — FPGA acquisition semaphore handshake (fpga_sem1 GIVEN, fpga_sem2 TAKEN) absent — replaced by a 1-byte trigger queue; stock 'osc' task (prio2) merged into our 'fpga' task (prio3).

- **Region:** freertos_objects
- **Why it might matter:** The two sems serialize config-vs-acquire. Our merged single-task may issue a 0x04/0x05 data read before the post-0x3A config-write sequence (01 08/02 03/06 00/07 00/08 AD + status 03) completes, touching the SSPI pins mid-config. FreeRTOS task/semaphore sequencing around the handshake is a favored category, but this is ordering/timing rather than a wrong register, so mid-rank.
- **Test:** STATIC: trace fpga_sem1/sem2 give/take in fpga task entry 0x0803E455 and osc entry 0x0804009D to recover the config->acquire ordering. BENCH: logic-analyze inter-phase timing (0x3A close -> ~600ms -> 5 config writes -> first 0x04 read); verify we never issue 0x04/0x05 before the post-0x3A config burst completes.

### #6 — PB9 and PA6 (undocumented analog-frontend control lines) configured OUTPUT-PP and sitting LOW in stock master_init; our firmware leaves them floating until meter mode.

- **Region:** gpio_banks
- **Why it might matter:** Either could gate an FPGA analog reference or a strap; a floating level during upload could differ from what the bitstream expects. Lower-ranked than the explicit relay bank because they are undocumented and lower-probability, but still a pre-config pin-state divergence.
- **Test:** Drive PB9=LOW, PA6=LOW pre-upload (folded into the rank-2 range-5 drive) and re-attempt the handshake; compare PC0 ready / 0x3A status.

### #7 — FPGA scope DATA path: stock reads bulk samples over FSMC (0x60020000) via DMA1; we read over SPI3 polling (returns constant 0xFF).

- **Region:** dma
- **Why it might matter:** Architecture-level: if stock never carries scope samples on SPI3, our SPI3 scope-read chases a non-existent path and the FPGA's user-mode FSMC-side handshake we omit could be tied to its willingness to leave/accept config. Ranked here because it is a DATA-path (post-config) concern, less likely to gate config-ENTRY than the SSPI-pin/strap items, but it could reframe the whole acquisition model.
- **Test:** Saleae-capture the FSMC data lines (A17=1, 0x60020000) during a stock boot+scope-arm and compare to SPI3; confirm whether stock ever clocks scope samples on SPI3 at all. Run BEFORE any further SPI3 bitstream-replay attempts.

### #8 — Pre-upload absolute timing: stock starts the 0x3B upload ~3.9s after power-on (NV design takes ~1.4s to boot); relative ordering of PB11/relay/USART vs the upload differs from ours.

- **Region:** spi3_fpga_handshake
- **Why it might matter:** If the FPGA config-wait window is timing-gated to its own power-on sequence, starting the upload too early/late could miss it. Our 2000ms wait covers NV boot, so this is lower-probability, but the relative ordering vs USART/relay writes is unverified.
- **Test:** Logic-analyze our cold-boot SPI3 wire timeline (PB3/4/5/6 + PC6 + PB11 + PA2/PA3) and diff against the stock issue-#18 timeline (1.383s USART live / 2.77-3.59s meter frames / 3.91s upload).

### #9 — DAC1 scope-trigger-comparator threshold (0x40007408 + enable 0x40007404|=1, plus 0x40001c34) never computed or written by us.

- **Region:** analog_frontend / i2c_touch
- **Why it might matter:** Internal MCU DAC/comparator, NOT a direct FPGA config write — unlikely to gate SSPI config-receive. But if the FPGA scope datapath waits on a valid trigger-DAC level before streaming, a zero threshold could mimic 'FPGA not working' even after config succeeds. Low-probability for the config-entry wall specifically.
- **Test:** Read 0x40007408/0x40007404 via usb_debug after our boot (expect 0/disabled); port FUN_080018A4's VFP calc, write a mid-scale enabled threshold, and re-check whether 0x04/0x05 reads return non-FF scope data.

### #10 — AT32 IOMUX REMAP3 |= (1<<27) at 0x40010030 during EXMC bring-up, omitted by us.

- **Region:** exmc_lcd
- **Why it might matter:** On AT32 the extended GMUX/REMAP registers override legacy remap and select EXMC vs alt-function muxing for high pins. Low probability it touches SPI3 (PB3/4/5 configured separately and our LCD works), but it is an unreplicated AT32-specific remap executed before fpga_init.
- **Test:** STATIC: confirm REMAP3 bit27 does not touch PB3/4/5/PB6/PC6. BENCH: SWD-read 0x40010030 on stock vs ours boot and diff; only escalate if the bit gates an SPI3-adjacent mux.

### #11 — USART2 RTOR=0xF000 receiver-timeout and EXTI3 rising-edge RX path (IRQ9) present in stock, omitted by us; TMR5 FPGA freq/period input-capture timer absent.

- **Region:** usart2_fpga / timers_nvic
- **Why it might matter:** Pure USART meter-path / measurement-channel details that do not touch SSPI config or the 0x3B path. Cannot plausibly affect whether the Gowin enters config-receive. Listed for completeness, lowest priority. (The UE-timing sub-hypothesis here is already DISPROVEN — stock UE is live pre-upload and the USART-silent build changed nothing on its own.)
- **Test:** Already largely negative; optionally confirm via Saleae that the FPGA emits no USART before the 0x3B window on stock (consistent with 'stock sends no USART at boot'). No further test warranted for the config wall.

## Recommended next experiment

Run a SINGLE bench experiment that stacks the two highest-ranked pre-config divergences and captures the result on a Saleae, on a TRUE COLD power-cycle boot (not warm `fpga reinit` — the FPGA is stateful): (1) build with FPGA_USART_SILENT_SCOPE=1 so NO USART command is sent before the 0x3B upload and USART2 UEN/tasks stay quiet; AND (2) immediately after PC6 HIGH and before the 0x3B prelude, drive the stock range-5 frontend posture (PC12=L, PE4=H, PE5=H, PE6=L, PA15=H, PA10=H, PB10=H, PB11=H, PB9=L, PA6=L) with PB11 raised >=1ms before the bare CS sync pulse. Capture PB3/PB4(MISO)/PB5/PB6/PC6/PB11 + read fpga.init_hs via the `status` shell command. PASS = prelude MISO 0xFF (config-wait float), 0x11 IDCODE returns 01 20 68 1B, 0x3A close returns 0xF8, and the post-upload status read 03 -> 00 01 42 2E 2E with PC0 going active. If it passes, bisect (USART-silent alone vs relays alone) to attribute the cause; if it still returns 0xFF/no-DONE, escalate to the FSMC-bus capture (rank 7) and an external Gowin-programmer load to isolate config-entry from the rest of the pipeline.


## Flagged for verification

- **0x7000 address-base defect:** two agents (clock_power_hold, gpio_banks) claim the true location of the master_init power-hold sequence is **0x0802AA50**, not the 0x08023A50 used throughout project docs — the same file-offset-vs-0x08007000-link-base confusion that bit the bitstream extraction. The *decode content* (registers/values) stands regardless; only the addresses would be off. VERIFY against the binary before propagating to canonical docs.


## Full region reports

```json
[
 {
  "region": "clock_power_hold (stock master_init SECTION 1)",
  "stock_summary": "REAL stock address is 0x0802AA50-0x0802AAB6 (the annotated file master_init_phase1.c labels it 0x08023A50, but Capstone ground-truth on APP_2C53T_V1.2.0_251015.bin, link base 0x08007000, shows 0x08023A50 is unrelated RAM-counter loop code; the GPIOC-clock+PC9 sequence the annotation describes byte-for-byte actually lives at 0x0802AA50). Decode of the real region: (1) 0x0802AA5C movw/movt sb=0x40021018 (CRM_APB2EN); 0x0802AA64 ldr; 0x0802AA6C orr #0x10; 0x0802AA70 str -> GPIOC clock enable (APB2EN bit4). (2) cfg struct built on stack: pins(+0)=0x200 (PC9), CNF/dir(+4)=0, mode/pull(+5)=0x18, gpio_mode(+6)=0x10 (nonzero=>output path), drive(+7)=1 (strong). r5=0x40011000 (GPIOC base). 0x0802AAB2 bl 0x80372FC = gpio_init -> programs GPIOC->cfghr nibble for PC9 = output push-pull 50MHz, high-drive (verified: gpio_init output path at 0x0803731C touches ONLY CRL/CRH mode reg, never BSRR/BRR/ODR). (3) 0x0802AAB6 str r4(0x200),[r7] where r7=0x40011014 = GPIOC offset 0x14 = clr register (AT32 GPIO +0x10=scr/set, +0x14=clr/clear, confirmed in at32f403a_407_gpio.h:535) -> GPIOC->clr=(1<<9) => PC9 driven LOW. SECTION 1 then falls through (0x0802AAB8+) into APB2EN |= 0x04/0x08/0x10/0x40 (GPIOA/B/C/E clocks). PLL/SYSCLK is NOT in this region: it is a separate clock-init fn (~0x080335E8) doing pllrcs=1(HEXT), pllhextdiv=1(HEXT/2), pllmult_l=11, pllmult_h=3 => mult=11+16*3+1=60, so 8MHz/2*60=240MHz; AHB/1, APB2DIV=/2(0b100@bits13:11), APB1DIV=/2(@bits10:8) -- register-identical to ours.",
  "decode_level": 3,
  "our_impl": "firmware/src/main.c:357-360 (GPIOC clock + PC9 cfghr + scr=HIGH); firmware/src/at32f403a_407_clock.c:46-97 (system_clock_config = PLL HEXT/2*60 -> 240MHz). main.c:634 system_clock_config is only the EMULATOR_BUILD stub.",
  "reimpl_level": "R1",
  "reimpl_reason": "Clock/PLL half is R2 (register- and value-identical: HEXT/2*60=240MHz, AHB/1, APB1/2 /2). But the PC9 power-hold half diverges in POLARITY: stock drives GPIOC->clr=0x200 (PC9 LOW), we drive GPIOC->scr=(1<<9) (PC9 HIGH). Same pin, same cfg (PP 50MHz strong), opposite output level and opposite register. Net region = partial/divergent = R1.",
  "divergences": [
   {
    "what": "PC9 output level at this point in init. Stock writes GPIOC->clr=0x200 (drives PC9 LOW) at 0x0802AAB6; gpio_init itself never sets the level. Our main.c:360 writes GPIOC->scr=(1<<9) (drives PC9 HIGH).",
    "stock": "GPIOC->clr = (1<<9)  // PC9 LOW (0x40011014 = 0x200)",
    "ours": "GPIOC->scr = (1<<9)  // PC9 HIGH (main.c:360)",
    "severity": "high"
   },
   {
    "what": "Ordering of PC9 relative to peripheral-clock enables. Stock enables GPIOC clock, configs+drives PC9, THEN enables GPIOA/B/C/E in one contiguous block before any clock_config. Ours: GPIOC+PC9 first, then SCB->VTOR/dfu_check/wdt, THEN system_clock_config(), THEN GPIOA/B/D/E/IOMUX/XMC. Stock does the GPIO-clock block BEFORE the PLL switch; we interleave VTOR/DFU/watchdog and run PLL between. PC9 itself is first in both.",
    "stock": "GPIOC-clk, PC9 cfg+drive, GPIOA/B/C/E-clk (all pre-PLL, contiguous)",
    "ours": "GPIOC-clk, PC9 cfg+drive, VTOR, dfu_check_magic, wdt_reload, system_clock_config(PLL), then GPIOA/B/D/E/IOMUX/XMC",
    "severity": "low"
   },
   {
    "what": "GPIOD clock. Stock SECTION 1 enables A/B/C/E only (D deferred to EXMC setup later, per annotation note). Ours enables GPIOD here too (main.c:396).",
    "stock": "APB2EN bits 2,3,4,6 (A,B,C,E); GPIOD deferred",
    "ours": "also CRM_GPIOD_PERIPH_CLOCK (main.c:396)",
    "severity": "low"
   },
   {
    "what": "Annotated source address base is wrong. master_init_phase1.c labels this region 0x08023A50; true location in APP_2C53T_V1.2.0_251015.bin is 0x0802AA50 (delta 0x7000 -- same 0x7000 file-vs-link confusion noted in CLAUDE.md for the bitstream). Any downstream doc keyed to 0x08023A5x addresses for master_init is mis-addressed.",
    "stock": "true addr 0x0802AA50-0x0802AAB6",
    "ours": "n/a (documentation defect)",
    "severity": "med"
   }
  ],
  "fpga_early_boot_actions": [
   {
    "action": "None in this region touch SPI3/USART/FPGA-config pins (PB3/4/5/6, PC6, PB11, PA2/3). SECTION 1 is purely CRM clock-enable + PC9. The only FPGA-adjacent fact is the PC9 polarity divergence (stock LOW vs our HIGH) and timing: stock holds all GPIO config in a tight pre-PLL block, we run the 240MHz PLL switch in the middle of GPIO bring-up. If PC9 (or whatever rail/strap it gates) influences FPGA power/POR sequencing, our inverted level + later timing could change the FPGA's power-on-reset window relative to when SPI3 config is later attempted.",
    "why_might_matter": "If PC9 LOW is stock's intended steady state and our HIGH is merely tolerated by a bootloader-latched power hold, then PC9 may also be a strap/enable into the FPGA's NV/config domain (e.g., holding it in a state that lets SSPI config be accepted). Driving it the opposite way could leave the Gowin in a mode that ignores config opcodes -- which matches the observed 'FPGA never enters SSPI config-receive' wall.",
    "testable": "BENCH: scope PC9 on a stock boot vs our boot from power-on through ~2s; confirm whether stock genuinely parks PC9 LOW (and whether the rail it gates stays up via another pin) or re-sets it HIGH shortly after 0x0802AAB6. STATIC: trace every GPIOC scr/clr write to bit9 across the whole stock APP (movw 0x1010/0x1014 sites enumerated: 7 scr, 25 clr) to find if/where PC9 returns HIGH. If stock truly runs PC9 LOW, flip ours to clr and re-run the SPI3 config-entry litmus (PC0 arming)."
   }
  ],
  "verify_path": "1) STATIC-EQUIV for clock: already V-grade -- stock 0x080335E8 PLL fields (pllrcs=1,pllhextdiv=1,mult_l=11,mult_h=3 =>60) decode-match at32f403a_407_clock.c CRM_PLL_MULT_60 + CRM_HEXT_DIV_2 + AHB/1 + APB1/2 /2; SYSCLK=240MHz both. 2) RESOLVE PC9 polarity to V: (a) static -- disassemble stock from 0x0802AAB6 forward and grep all bit9 scr/clr writes to determine PC9's final settled level before scheduler start; (b) bench -- logic-analyzer PC9 from cold power-on on a STOCK unit through full boot; compare to our boot trace. Decision rule: if stock settles PC9 LOW and device stays powered, our main.c:360 scr write is a genuine bug to flip; if stock re-sets PC9 HIGH later, document the transient-LOW-then-HIGH as benign and mark region R2. 3) Then re-run the FPGA SSPI config-entry litmus under both PC9 polarities to test the hypothesis that PC9 gates FPGA config acceptance."
 },
 {
  "region": "gpio_banks (stock master_init FUN_08023A50, doc addr 0x08023AB6-0x08024250 = objdump 0x0802AAB6-0x0802B250)",
  "stock_summary": "Region performs GPIO clock-enables + static pin-mode config for the button matrix, EXMC/LCD bus, and a few \"extra\" pins, all via the SPL gpio_init helper (doc 0x080302FC / objdump 0x080372FC; 8-byte cfg: +0 pins, +4 out_type, +5 pull[4=none/0x18=up/0x28=down], +6 mode[0=in/0x10=out/8=AF/3=analog], +7 drive).\nCLOCKS (CRM->APB2EN @0x40021018): |=0x10 (GPIOC, first, for power hold) @0x08023A6C; then |=0x04,0x08,0x10,0x40 (GPIOA,B,C,E) @0x08023AB8-AE8; later |=0x20,0x40,0x01 (GPIOD,E,AFIO) @0x08023B7A-B98. CRM->AHBEN @0x40021014 |=0x100 (EXMC) @0x08023BA8.\nPIN MODE CONFIG (ground-truth pull from objdump per stock_pre_fpga_gpio_state.md, which corrects the phase1.c annotation): PC9 OUTPUT-PP 50MHz strong (power hold) @0x08023AB2, then GPIOC->BRR=0x200 momentary @0x08023AB6 (mode re-init only; reset stub holds PC9 high). PB7 INPUT PULL-DOWN @0x08023B04 (PRM idle-low). PC8+PC13 INPUT PULL-UP @0x08023B18 (POWER+UP passive). PA7+PA8 input @0x08023B2E. PB0 @0x08023B3C, PC5+PC10 @0x08023B4A, PE2+PE3 @0x08023B5E (matrix). Then PB7+PB8 re-driven OUTPUT-PP moderate (PB8=backlight) and PB0/PC5/PC10/PE2/PE3 set OUTPUT-PP moderate for the matrix.\nEXMC pins: AFIO JTAG remap IOMUX REMAP3 (0x40010030) |=(1<<27) @0x08023BB6; PD0,1,8,9,10,14,15 AF-PP-50MHz-strong @0x08023BC2; PE7-15 AF-PP @0x08023BD2; PD6 OUTPUT-PP-strong @0x08023BEC; PD4,5,7,11 OUTPUT-PP-strong @0x08023BFE. EXMC regs: SNCTL0=0x5010 then |=1; SNTCFG0=0x02020424; SNWTCFG0=0x00000202; AT32 ext reg 0xA0000220 bits[15:8] cleared then bits[7:0]=8. GPIOD->BSRR=(1<<6) (PD6 high) + PD6 reset-pulse toggle in SysTick delays.\nRegion tail (Section 10, doc 0x0802412C): GPIOC PC14 configured INPUT. (Relay/gain bank PC12/PE4-6/PA15/PA10/PB10/PB11/PB9/PA6 driven to range-5 levels is in Group 5 at objdump 0x0802C53C = doc ~0x080255xx, PAST this region's 0x08024250 boundary.)",
  "decode_level": 3,
  "our_impl": "Split across files: main.c:357-360 (PC9 power hold), main.c:394-399 (GPIOA/B/D/E/IOMUX/XMC clock enables), main.c:417-418 (PB8 backlight), main.c:435-458 (EXMC PD/PE pins via _GPIO_CFG macro + EXMC regs); button matrix PB7/PC8/PC13/PA7/PB0/PC5/PE2/PE3/PA8/PC10/PE3 in drivers/button_scan.c:69-148,234-268 (button_scan_init + per-scan reconfig). PC14: ABSENT. Relay/gain bank: drivers/fpga.c:2080-2127 (deferred to meter-entry, not boot/upload path).",
  "reimpl_level": "R1",
  "reimpl_reason": "All functionally-required pins (power-hold, button matrix with correct pulls, EXMC bus, backlight) are reimplemented and bench-confirmed; but it is NOT register-faithful in order/locus (clocks+configs split across main.c and button_scan.c at different times vs stock's single contiguous block), PC14 is entirely omitted, and the analog-frontend relay/gain bank that stock drives during master_init is deferred to meter mode \u2014 leaving those pins floating through the FPGA bitstream-upload window.",
  "divergences": [
   {
    "what": "Button pin pull-config: phase1.c annotation claims PB7=pull-up(0x28), PC8/PC13=pull-down(0x18). objdump ground truth (stock_pre_fpga_gpio_state.md) says PB7=PULL-DOWN, PC8/PC13=PULL-UP, which is what OUR firmware does (button_scan.c:252,251,253). So ours matches hardware; the phase1.c source annotation is the thing that's wrong.",
    "stock": "PB7 pull-down, PC8/PC13 pull-up (objdump-confirmed)",
    "ours": "PB7 pull-down, PC8/PC13 pull-up (button_scan.c:251-253) \u2014 bench-confirmed 2026-04-06",
    "severity": "low"
   },
   {
    "what": "PC14 configured as INPUT by stock at doc 0x0802412C; our firmware never configures PC14 anywhere.",
    "stock": "GPIOC PC14 = INPUT (mode from struct)",
    "ours": "ABSENT \u2014 PC14 left at reset default",
    "severity": "low"
   },
   {
    "what": "GPIO clock enables + button-matrix pin config are a single contiguous block in stock master_init; ours are split between main.c (clocks, PC9, EXMC, PB8) and button_scan.c (matrix pins, configured later when the input task starts).",
    "stock": "one contiguous init block, fixed order",
    "ours": "split across main.c boot + button_scan_init (different time/order)",
    "severity": "low"
   },
   {
    "what": "Stock re-drives PB7 and PB0/PC5/PC10/PE2/PE3 as OUTPUT-PP after the input config (matrix idle posture). Our matrix uses dynamic per-scan reconfig (pin_input/pin_output toggling) rather than a fixed boot posture.",
    "stock": "static OUTPUT posture set once at init",
    "ours": "dynamic reconfigure each scan phase (button_scan.c:111-148)",
    "severity": "low"
   },
   {
    "what": "EXMC config value SNCTL0: stock writes 0x5010 then |=1 (=0x5011); ours writes 0x00005010 then |=0x0001 \u2014 identical result. SNTCFG0/SNWTCFG0 identical. AT32 ext-reg 0xA0000220 sequence (clear[15:8], set[7:0]=8) is present in stock but NOT in our EXMC init.",
    "stock": "writes 0xA0000220: &~0xFF00 then |8",
    "ours": "0xA0000220 not touched (main.c:455-458 only writes SNCTL0/SNTCFG0/SNWTCFG0)",
    "severity": "low"
   },
   {
    "what": "PD6 reset-pulse toggle (BSRR set, delay, BRR clear, delay, BSRR set) that stock does as part of the LCD/EXMC bring-up \u2014 our LCD init uses a software-reset command path (lcd_write_cmd 0x01) instead of the PD6 toggle.",
    "stock": "PD6 hardware toggle pulse in SysTick-timed sequence",
    "ours": "no PD6 toggle; uses ST7789 SWRESET cmd 0x01 (main.c:462)",
    "severity": "low"
   }
  ],
  "fpga_early_boot_actions": [
   {
    "action": "Stock, within master_init (Group 5, objdump 0x0802C53C \u2014 just PAST this region's 0x08024250 tail), drives the entire analog-frontend relay/gain bank to range-index-5 levels BEFORE the SPI3 0x3B bitstream open: PC12=LOW, PE4=HIGH, PE5=HIGH, PE6=LOW, PA15=HIGH, PA10=HIGH, PB10=HIGH, PB11=HIGH. Our firmware leaves ALL of these floating during the upload (fpga.c relay config at L2080-2127 runs only on meter entry, not the boot/upload path).",
    "why_might_matter": "If the Gowin GW1N-UV2 samples any of these as configuration straps or as the analog environment it validates at config time, our floating pins are a plausible reason the bitstream is rejected and the FPGA never enters/accepts SSPI config \u2014 matching the live 'config-entry wall' mystery.",
    "testable": "Bench: replicate the range-5 levels (call equivalent of gpio_mux_portc_porte(5)+gpio_mux_porta_portb(5)) right after PC6 HIGH and before the 0x3B prelude; re-run the SPI3 handshake and watch for the 0x3A close returning status 0xF8 (vs current failure) on a Saleae capture / PC0 ready edge."
   },
   {
    "action": "Stock holds PB11 HIGH from the relay range-5 write (well before the SPI3 handshake); our firmware holds PB11 LOW until an arm edge (main.c GPIO, fpga.c L1730/L2043).",
    "why_might_matter": "PB11 ('FPGA active mode') may be a mode/strap input sampled by the FPGA during config. An inverted level at config time could block config-entry.",
    "testable": "Bench: drive PB11 HIGH before the 0x3B open (not just 1ms before the handshake) and capture whether config-entry behavior changes; cross-check the #18 Saleae capture for whether stock's PB11 is HIGH-throughout vs a late pulse."
   },
   {
    "action": "Stock configures PB9 and PA6 as OUTPUT-PP in master_init (Group 2/5) and they sit LOW on the boot range; our firmware leaves PB9/PA6 floating until meter mode (fpga.c L2101-2107).",
    "why_might_matter": "Both are undocumented analog-frontend control lines; if either gates the FPGA's analog reference or a strap, a floating level during upload could differ from what the bitstream expects.",
    "testable": "Bench: drive PB9=LOW, PA6=LOW pre-upload and re-attempt the handshake; compare PC0 ready / 0x3A status."
   },
   {
    "action": "Stock's EXMC AT32-extension register write at 0xA0000220 (clear bits[15:8], set bits[7:0]=8) is omitted in our EXMC init.",
    "why_might_matter": "Not FPGA-directly, but it's an unmodeled AT32 EXMC timing-extension write in the same init region; worth noting it's absent in case any shared-bus timing affects downstream peripheral bring-up ordering.",
    "testable": "Static: confirm LCD/EXMC works without it (it does); low priority for the FPGA path."
   }
  ],
  "verify_path": "Static-equiv for the in-region button/EXMC/PC14 parts: (1) Add PC14 INPUT config to match stock (trivial, low-value). (2) Accept the split-locus button config as functionally equivalent \u2014 already bench-confirmed 15/15 buttons + working LCD, so the divergences are cosmetic/ordering and can be marked V by static review. For the FPGA hypothesis (the real prize, technically in the adjacent Group-5 block just past 0x08024250): the decisive bench litmus is to insert a stock-faithful range-5 relay/gain drive (PC12=L,PE4=H,PE5=H,PE6=L,PA15=H,PA10=H,PB10=H,PB11=H, plus PB9=L,PA6=L) immediately after PC6 HIGH and before the 0x3B prelude in fpga.c, then capture the SPI3 handshake on a Saleae: success = 0x3A close returns 0xF8 and the post-upload config writes (01 08 / 02 03 / 06 00 / 07 00 / 08 AD) yield status-read 03 \u2192 00 01 42 2E 2E, with PC0 going active. That single change tests the strongest unmodeled-FPGA-action hypothesis surfaced here."
 },
 {
  "region": "exmc_lcd (master_init FUN_08023A50 phase1 \u00a74\u2013\u00a78, stock 0x08023B62\u20130x0802405A)",
  "stock_summary": "Clock/AF enable: CRM->APB2EN |= (1<<5)GPIOD |(1<<6)GPIOE |(1<<0)AFIO; CRM->AHBEN |= (1<<8)EXMC (CRM 0x40021014/0x40021018). AT32 remap: IOMUX REMAP3 (decode reads [0x40010008+0x28]=0x40010030) |= (1<<27) \u2014 AT32-specific EXMC/remap bit, NOT replicated by us. GPIO AF-PP-50MHz init: GPIOD pins 0xC703 (PD0,1,8,9,10,14,15 = EXMC D-bus), GPIOE 0xFF80 (PE7-15 = D4-D12), GPIOD PD6 (0x40) separate, GPIOD 0x8B0 (PD4=NOE,PD5=NWE,PD7=NE1,PD11=A16). Note PD12/A17 not in stock masks here (likely set elsewhere or analysis bit-error). EXMC regs (base 0xA0000000): SNCTL0=0x5010 (bank OFF), SNTCFG0(0xA0000004)=0x02020424, SNWTCFG0(0xA0000104)=0x00000202; then AT32 ext reg 0xA0000220: clear bits[15:8], set bits[7:0]=8; then SNCTL0 |= 1 (enable bank). PD6 reset pulse: BSRR set PD6 HIGH, ~10us, BRR PD6 LOW, ~20us, BSRR PD6 HIGH, ~20us (LCD hardware reset on PD6). ST7789V init over EXMC (cmd@0x6001FFFE,data@0x60020000): 0x11 SLPOUT +120ms; 0x36=0x00; 0x3A=0x55; 0xB2=0C 0C 00 33 33; 0xB7=0x46; 0xBB=0x1B; 0xC0=0x2C; 0xC2=0x01; 0xC3=0x0F; 0xC4=0x20; 0xC6=0x0F; 0xD0=A4 A1; 0xD6=0xA1; 0xE0=14-byte pos gamma; 0xE1=14-byte neg gamma; 0x29 DISPON. Then lcd_params@0x20008340 (w=320,h=240,orient=1,RAMWR=0x2C,CASET=0x2A,RASET=0x2B); 0x36=0xA0 (landscape); CASET/RASET full window.",
  "decode_level": 3,
  "our_impl": "Two divergent impls exist. (1) firmware/src/drivers/lcd.c:174 lcd_gpio_init / :240 lcd_fsmc_init / :284 lcd_init \u2014 register-faithful full ST7789 sequence, but lcd_init() is NEVER called (grep: only definition, no caller outside attic). (2) The LIVE path is an inline block in firmware/src/main.c:432-478 that hand-rolls EXMC + a stripped LCD init. EXMC writes main.c:455-458 match stock (0x5010/0x02020424/0x00000202, then |=1). GPIO main.c:446-452 covers PD0,1,4,5,7,8,9,10,11,12,14,15 + PE7-15.",
  "reimpl_level": "R1",
  "divergences": [
   {
    "what": "LCD reset pulse on PD6 (BSRR/BRR toggle HIGH-LOW-HIGH with us delays, stock 0x08023C5A-0x08023D62)",
    "stock": "PD6 configured as output, pulsed HIGH->LOW->HIGH to hardware-reset the ST7789 panel before init",
    "ours": "ABSENT in both paths. main.c never touches PD6; lcd.c lcd_reset() is a bare delay_ms(120) stub with a TODO ('Identify RST pin from hardware teardown') and is not called. PD6 is even listed as a 'safe non-EXMC' button-test pin in attic/btntest2.c",
    "severity": "med"
   },
   {
    "what": "IOMUX REMAP3 |= (1<<27) (stock 0x08023BB0-0x08023BBA, AT32-specific EXMC-region remap)",
    "stock": "Sets bit 27 of the AT32 extended remap register at 0x40010030 during EXMC bring-up",
    "ours": "ABSENT. Our firmware does only the JTAG-off SWD remap (fpga.c:1716+) for PB3/4/5; this separate EXMC-region REMAP3 bit is never written. Already flagged LOW in compliance_audit_2026_04_06.md item 19",
    "severity": "low"
   },
   {
    "what": "AT32 EXMC extended register 0xA0000220 (clear[15:8], set[7:0]=8) (stock 0x08023C34-0x08023C48)",
    "stock": "Programs an AT32-specific EXMC timing-extension register before enabling the bank",
    "ours": "ABSENT in both main.c and lcd.c. We jump straight from SNWTCFG0 to SNCTL0|=1",
    "severity": "low"
   },
   {
    "what": "Full ST7789 power/porch/gamma init (stock 0x08023DE2-0x08023F1E: 0xB2,0xB7,0xBB,0xC0,0xC2,0xC3,0xC4,0xC6,0xD0,0xD6,0xE0,0xE1)",
    "stock": "~13 panel-tuning commands incl. positive/negative 14-byte gamma, VCOM, power control",
    "ours": "The LIVE main.c path (461-475) sends ONLY 0x01(SWRESET),0x11,0x36=0xA0,0x3A=0x55,0x29 \u2014 skips ALL gamma/power/porch. (lcd.c HAS the full sequence but is dead code.)",
    "severity": "med"
   },
   {
    "what": "MADCTL ordering and value",
    "stock": "Writes 0x36=0x00 first (during config), then 0x36=0xA0 (landscape) after DISPON",
    "ours": "main.c writes 0x36=0xA0 once, before 0x3A and DISPON (different order, single write)",
    "severity": "low"
   },
   {
    "what": "SLPOUT post-delay and reset timing",
    "stock": "0x11 then mandatory 120ms; reset uses precise SysTick us-delays",
    "ours": "main.c: 0x01 +200ms, 0x11 +200ms (delay present but coarse delay_ms, no SysTick); acceptable functionally",
    "severity": "low"
   },
   {
    "what": "lcd.c full driver is unreachable",
    "stock": "n/a",
    "ours": "lcd_init() in lcd.c is register-faithful (R2-quality) but has NO caller \u2014 the project ships the stripped main.c inline init instead. Dead-code divergence: header comments still claim GD32F307@120MHz",
    "severity": "low"
   }
  ],
  "fpga_early_boot_actions": [
   {
    "action": "IOMUX REMAP3 |= (1<<27) at 0x40010030 during EXMC bring-up (stock 0x08023BB0), which our firmware omits",
    "why_might_matter": "On AT32F403A the extended IOMUX/GMUX remap registers (REMAP3/4/5/6/7) override the legacy STM32 remap and select EXMC vs GPIO/alt-function muxing for high pins. If bit27 gates an EXMC or alternate-function routing that shares a port bank used by the SPI3/FPGA path, an unset bit could leave a pin in the wrong mux state at the moment the FPGA SSPI handshake runs. Low probability (SPI3 is PB3/4/5, configured separately, and our LCD works) but it is an unreplicated AT32-specific remap write executed before fpga_init, so ordering-wise it precedes FPGA config.",
    "testable": "Static: pull the AT32F403A reference manual REMAP3 bit27 definition and confirm it does NOT touch PB3/4/5/PB6/PC6 (SPI3 + CS + SPI-enable). Bench: read 0x40010030 on a stock boot via SWD and after our boot; diff. If different, set the bit in our init before fpga_init() and re-run the PC0-arming/0x3A-status litmus."
   },
   {
    "action": "PD6 LCD hardware-reset pulse (stock 0x08023C5A-0x08023D62) \u2014 establishes that stock DOES drive a dedicated panel-reset GPIO with timed pulses",
    "why_might_matter": "Not directly FPGA, but it proves the stock boot performs a real reset-pin toggle sequence with SysTick-precise timing in this region. The FPGA bring-up mystery has repeatedly chased a 'missing reset pin'; this region documents the stock idiom (BSRR/BRR + us delays). If the FPGA config-entry also relies on a comparably-timed pin our firmware leaves static, the same omission pattern (we stub reset pins as no-ops) would apply. Worth confirming no shared GPIO/timing dependency between this PD6 sequence and FPGA mode pins.",
    "testable": "Static: confirm PD6 is solely the ST7789 RST net and not co-routed to any FPGA M0-M3/RECONFIG net on the 2C53T-V1.4 schematic/teardown. Bench: scope PD6 vs FPGA mode pins on stock boot to confirm independence."
   }
  ],
  "verify_path": "1) Static-equiv: this region is NOT FPGA-critical on current evidence \u2014 push to V by (a) confirming AT32 REMAP3 bit27 does not touch any SPI3/FPGA pin (datasheet lookup), and (b) confirming PD6 is solely ST7789-RST (schematic). If both hold, the EXMC-register subset (SNCTL0/SNTCFG0/SNWTCFG0/enable) is already byte-exact in main.c:455-458 = V for the memory-controller part. 2) For honest reimpl parity: either wire main.c to call lcd.c:lcd_init() (which is register-faithful) and add the PD6 reset pulse + 0xA0000220 write + REMAP3 bit, OR document that the stripped init is intentional. 3) Bench litmus for the FPGA hypothesis: SWD-read 0x40010030 and 0xA0000220 on a stock boot vs ours and diff; only escalate REMAP3 to a real FPGA suspect if the bit gates an SPI3-adjacent mux."
 },
 {
  "region": "adc1 (stock 0x08025300-0x08025470, master_init_phase2.c PHASE 2A == master_init_phase4.c SECTION C; both annotate the same ADC1 block)",
  "stock_summary": "ADC1 @ 0x40012400, battery-sense (PB1 = ADC1 channel 9). Register sequence (phase2a/phase4-C agree):\n- VFP cal-math preamble (0x08025300-0x080253A0): computes 6 per-range gain/offset cal coefficients from meter_state_base offsets +0x2D8/+0x2EC/+0x300/+0x314/+0x328/+0x33C, VSTR to a cal output buffer. This is the SCOPE/meter voltage-scaling LUT prep, not an ADC register op.\n- RCU_APB2EN (0x40021018): |=0x08 (GPIOB clk), |=0x200 (ADC1 clk).\n- RCU_CFG0 (0x40021004): [15:14]=0b10 (ADCPSC[1:0]=PCLK2/6), &=~0x10000000 (clear bit28 ADCPSC[2]). Net ADC clock = PCLK2/6.\n- PB1 gpio_init -> analog input (annotation's inline 'PA pin 2' comment is a decompile artifact; channel is ch9=PB1 per summary).\n- ADC1_CTRL1 (0x40012404): &=~0xF0000 (clear AWDCH), |=0x100 (bit8 SCAN=1).\n- ADC1_CTRL2 (0x40012408): |=0x02 (bit1 CONT=1), &=~0x800 (bit11 ALIGN=0 right-aligned).\n- ADC1_OSQ3 (0x4001242C): &=~0xF00000 (clear L[23:20] => 1 conversion).\n- ADC1_SPT2 (0x40012410): |=0x38000000 (bits29:27 SMP9=0b111 => 239.5 cycles).\n- ADC1_OSQ2 (0x40012434): SQ1[4:0]=9 (channel 9 first in regular sequence). (Annotation labels it OSQ2/SQ7 in phase2 but the offset 0x34 = OSQ1/SQR1 SQ1; phase4 correctly calls it SQR1 SQ1[4:0]=9.)\n- ADC1_CTRL2: &=~0x2000000, |=0xE0000 (ETESEL=0b111 SWSTART), |=0x100000 (ETERC ext-trig-enable for regular), |=0x100, |=0x01 (ADON on), |=0x08 (RSTCAL), poll until bit3 clears, |=0x04 (CAL start), poll until bit2 clears.\nNO DMA (polled). NO injected channels. Software-triggered single battery conversion.",
  "decode_level": 3,
  "our_impl": "/Users/david/Projects/RESEARCH/osc/firmware/src/drivers/battery.c:42 battery_adc_init() (lines 42-73). HAL maps verified in firmware/at32f403a_gcc/libraries/drivers/src/at32f403a_407_adc.c: adc_calibration_init=CTRL2 bit3 RSTCAL (line 204 adcalinit), adc_calibration_start=CTRL2 bit2 CAL (line 235 adcal).",
  "reimpl_level": "R2",
  "reimpl_reason": "Register-faithful for the load-bearing ADC config: same ch9, same /6 prescaler (CRM_ADC_DIV_6), 239.5-cyc sample (SPT2 SMP9=0b111), right-aligned, 1-conversion regular seq, software trigger, and the exact RSTCAL-poll-then-CAL-poll sequence via AT32 HAL (which writes the identical CTRL2 bits 3 then 2). The two divergences (no SCAN, no CONT) are functionally inert for a single-channel software-triggered one-shot read.",
  "divergences": [
   {
    "what": "CTRL1 bit8 SCAN mode",
    "stock": "ADC1_CTRL1 |= 0x100 (SCAN=1)",
    "ours": "adc_cfg.sequence_mode=FALSE (SCAN=0)",
    "severity": "low"
   },
   {
    "what": "CTRL2 bit1 CONT (continuous conversion)",
    "stock": "ADC1_CTRL2 |= 0x02 (CONT=1)",
    "ours": "adc_cfg.repeat_mode=FALSE (CONT=0); each read uses an explicit software trigger in adc_read_mv_once()",
    "severity": "low"
   },
   {
    "what": "GPIO clock + PB1 analog ordering vs ADC clock",
    "stock": "enables GPIOB clk, inits PB1 analog, then enables ADC1 clk, then sets prescaler",
    "ours": "enables ADC1 clk + prescaler FIRST (battery.c:44-45), then PB1 analog (51), then ADC config \u2014 ordering differs but no register hazard (clocks are independent enables)",
    "severity": "low"
   },
   {
    "what": "VFP per-range cal-coefficient math preamble",
    "stock": "computes 6 gain/offset cal pairs from meter_state_base into a cal output buffer before touching ADC regs",
    "ours": "ABSENT in this region \u2014 battery uses a fixed DIVIDER_MV_PER_COUNT=1611 constant; the 6-range cal LUT is a scope/meter voltage-scaling concern handled elsewhere/not at all, not battery ADC",
    "severity": "low"
   }
  ],
  "fpga_early_boot_actions": [],
  "verify_path": "Static-equiv is already essentially complete (R2): HAL bit-mapping confirmed against at32f403a_407_adc.c. To push to V with zero doubt: (1) build our firmware, disassemble battery_adc_init and confirm the emitted CTRL2 writes are |=0x100/|=0x01/|=0x08/poll/|=0x04/poll matching stock 0x08025400-0x08025468; (2) bench litmus \u2014 read ADC1_CTRL1/CTRL2/SPT2/OSQ1 over SWD after battery_adc_init() and diff against a stock boot dump (expect CTRL1 differ in bit8 SCAN, CTRL2 differ in bit1 CONT, all else identical). Note: this region has NO bearing on the FPGA scope-bitstream mystery \u2014 the 'scope ADC' is the FPGA's own ADC delivered over SPI3, not MCU ADC1; MCU ADC1 here is battery-only."
 },
 {
  "region": "usart2_fpga (stock master_init phase3 sections B\u2013E, 0x08025858\u20130x08025AEA; USART2 base 0x40004400)",
  "stock_summary": "Stock USART2 (0x40004400, GD32/AT-style register names CTL0/CTL1/CTL2/BRR/RTOR) brought up inside master_init_phase3:\n- Clock: RCU_APB1EN |= (1<<17) USART2EN (section A, 0x08025818).\n- Pins: gpio_init(PA2=AF push-pull 50MHz TX, 0x08025838) ; gpio_init(PA3=input-floating RX, 0x08025842).\n- NVIC: ISER1=0x40 enables IRQ#38 = USART2 global IRQ (0x08025890); priority encoded from SCB_AIRCR PRIGROUP into NVIC_IPR (0x08025858\u20130x0802588A).\n- CTL0 &= ~(1<<12) clears M \u2192 8-bit word (0x080258EC).\n- CTL1 &= ~(3<<12) clears STOP \u2192 1 stop bit (0x080258FC).\n- CTL0 = (CTL0 & ~0x2000) | 0x2C \u2192 sets TE(bit3)+RE(bit2)+RXNEIE/RDBFIEN(bit5), UE(bit13) momentarily cleared (0x0802590A\u20130x08025932).\n- BRR = computed 9600 divisor: VFP path (CTL0 |=1 sets UE at 0x08025A00). meter[0xE5C]*200 feeds float division \u2192 (uint32)(div-1). At 120MHz APB1 this is ~12500 (0x30D4).\n- Section E: CTL0 |= (1<<28); CTL2 flow-control fields zeroed (CTS off, no DMA/flow); RTOR=0xF000 receiver-timeout.\n- The literal 'USART2_BRR = 0x64' annotation at 0x08025ABC is a DECODE ERROR (the comment self-doubts 'may be timer'); 0x64 at 120MHz \u2260 9600. Real BRR is the computed ~12500; 0x64 is a TIM6 prescaler/ARR write to r4 (timer base), not USART2_BRR.\nNet peripheral end-state: USART2 = 8N1, 9600 baud, TE+RE+RXNEIE all set, UE SET, RTOR=0xF000, NVIC IRQ#38 enabled. Empirical confirmation (issue-#18 Saleae capture): USART2 lines go live at t=1.383s and run 6 meter RX frames at 2.77\u20133.59s \u2014 BEFORE the 3.91s SPI3 bitstream upload \u2014 so UE is genuinely SET pre-upload, matching this decode.",
  "decode_level": 3,
  "our_impl": "/Users/david/Projects/RESEARCH/osc/firmware/src/drivers/fpga.c:1789\u20131825 (fpga_init Step 2); IRQ handler USART2_IRQHandler at fpga.c:1081; baud macro fpga.h:36.",
  "reimpl_level": "R2",
  "reimpl_reason": "Active build (both experiment flags=0) sets the identical end-state: clock on, PA2 AF-PP/PA3 input-float, CTL0 8-bit + TE(bit3)+RE(bit2)+RDBFIEN(bit5)+UEN(bit13), BRR=240MHz/2/9600=12500 matching stock's computed divisor, USART2 NVIC enabled. Same regs, same bit values, same effective order. Minor non-functional divergences only.",
  "divergences": [
   {
    "what": "USART2 RTOR (receiver-timeout) register write =0xF000",
    "stock": "USART2_RTOR = 0x0F000 (section E, 0x08025xxx)",
    "ours": "Never written (RTOR left at reset 0). We don't use receiver-timeout interrupt.",
    "severity": "low"
   },
   {
    "what": "CTL0 bit28 set (stock '|= (1<<28)')",
    "stock": "USART2_CTL0 |= (1<<28) (section E 0x08025A32) \u2014 likely an AT32-specific bit (e.g. ext/oversampling or a reserved feature)",
    "ours": "Not set. Our ctrl1 only sets bits 2,3,5,13.",
    "severity": "low"
   },
   {
    "what": "CTL2 flow-control explicit zeroing",
    "stock": "CTL2 &= ~1 then CTL2=0 (CTS/RTS/DMA off)",
    "ours": "ctrl3/CTL2 left at reset (already 0). Functionally identical but not explicitly written.",
    "severity": "low"
   },
   {
    "what": "NVIC priority value/grouping for USART2",
    "stock": "Priority encoded from SCB_AIRCR PRIGROUP into NVIC_IPR (computed)",
    "ours": "NVIC_SetPriority(USART2_IRQn,5) \u2014 chosen to sit below configMAX_SYSCALL_INTERRUPT_PRIORITY(5) for FreeRTOS safety; numeric value differs from stock but both are valid.",
    "severity": "low"
   },
   {
    "what": "Bit-set sequencing of CTL0",
    "stock": "Single composite write (CTL0 & ~0x2000)|0x2C then later |=1 for UE",
    "ours": "Four separate read-modify-write |= ops (bits 2,3,5,13) after ctrl1=0. Same final value; intermediate states differ but UE asserted last in both.",
    "severity": "low"
   }
  ],
  "fpga_early_boot_actions": [
   {
    "action": "RTOR=0xF000 receiver-timeout (we omit). Stock arms a USART2 receiver-timeout; on the FPGA's USART link this governs how stock detects end-of-frame / idle on the meter channel.",
    "why_might_matter": "Pure USART meter-path detail; does not touch SSPI config or the 0x3B bitstream path. Cannot plausibly affect whether the Gowin enters config-receive. Low likelihood.",
    "testable": "Static: confirm our USART2 RX framing (header-state-machine in USART2_IRQHandler) closes frames without RTOR. Bench: add RTOR=0xF000 and check meter-frame integrity \u2014 irrelevant to scope/SSPI."
   },
   {
    "action": "USART2 UE asserted BEFORE the SPI3 0x3B upload, with 6 live meter RX frames (2.77\u20133.59s) preceding upload (3.91s). Earlier hypothesis was that stock holds UE clear until post-upload and that OUR live USART2 was stealing the SSPI pins.",
    "why_might_matter": "Originally the leading FPGA-config-entry suspect (prelude MISO 0x80 user-mode vs 0xFF config-wait). NOW DISPROVEN BOTH WAYS: (a) the same capture's timeline shows stock UE live pre-upload, so we already match stock; (b) the 2026-06-13 USART-silent bench build (FPGA_USART_SILENT_SCOPE) changed NOTHING \u2014 config-entry wall identical. This region is NOT the FPGA mystery.",
    "testable": "Already tested (negative): guest-usart-silent build, RX bytes=0 confirmed, fpga reinit still returned 0xFF/no DONE. See SPI3_STOCK_BOOT_CAPTURE_ANALYSIS.md lines 144\u2013164. No further test warranted."
   }
  ],
  "verify_path": "Static-equivalence is already achievable to V: (1) Confirm both flags compile out (FPGA_WARM_HANDOFF_TEST=0, FPGA_USART_SILENT_SCOPE=0) so the RE/TE/RDBFIEN/UEN path is live \u2014 done, both =0. (2) Register-end-state table matches stock 1:1 except the three low-severity omissions (RTOR, CTL0 bit28, explicit CTL2=0), none of which alter 8N1/9600/TE/RE/RXNEIE/UE. (3) The UE-timing question \u2014 the only FPGA-relevant item \u2014 is closed by the issue-#18 capture timeline (UE live pre-upload in stock) AND the 2026-06-13 USART-silent bench result (silencing UE changed nothing). Optional belt-and-suspenders bench litmus: scope PA2/PA3 + PB6 CS on a cold boot of OUR firmware and confirm USART2 meter frames appear before the SPI3 0x3B window exactly as stock's 1.383s/2.77s/3.91s timeline \u2014 purely to confirm ordering, not expected to move the config-entry wall."
 },
 {
  "region": "spi3_fpga_handshake (stock master_init FUN_08023A50, 0x0802650A\u20130x08026800; phase3.c sub-sections L/M/N\u2192annotated as M/N/O/P)",
  "stock_summary": "Three sub-regions. (L) GPIO for SPI3, 0x0802650A: RCU_APB1EN|=bit15, RCU_APB2EN|=bit3 (IOPBEN); gpio_init PB3=AF-PP/50MHz(SCK), PB4=input-floating(MISO), PB5=AF-PP/50MHz(MOSI), PB6=output-PP/50MHz(software CS); then GPIOC_BOP=(1<<6) \u2192 PC6 HIGH (FPGA SPI enable). (M) SPI3 peripheral, 0x080265CA: spi_init(SPI3_BASE=0x40003C00) Mode3 (CPOL=1 bit1, CPHA=1 bit0), MSTEN, 8-bit, MSB-first, soft-CS; prescaler is /2=60MHz (CLAUDE.md + issue-#18 capture; the phase3.c `0x100`/'/4' annotation is the packed config word, not the real divider). SPI3_CTL1|=bit1 (RXNEIE), |=bit0 (TXEIE); SPI3_CTL0|=bit6 (SPE). CTL2 also gets DMA-req bits 0x03 (RXDMAEN+TXDMAEN) per stock. (N/O/P) handshake, 0x08026638\u20130x08026800: SysTick reload\u00d710 (~10ms) then \u00d71 (~1ms) busy-wait on COUNTFLAG. Then the FPGA config handshake \u2014 GROUND-TRUTHED by maksidze's issue-#18 Saleae capture (NOT the phase3.c '0x40/0x00/0x05 read FPGA-ID' annotation, which is a misread: the 0x40 is the GPIOC_BOP PC6 write, not an SPI byte). Real wire: PB11\u2192HIGH 1.0ms before SPI; a bare CS-low\u2192high pulse (zero clocks, frame-sync); CS\u2193 05 00 CS\u2191 | +~100ms | CS\u2193 12 00 CS\u2191 | +~100ms | CS\u2193 15 00 CS\u2191 | +8\u00b5s | CS\u2193 3B <115,638-byte Gowin bitstream> CS\u2191 | CS\u2193 3A 00 CS\u2191 (MISO\u21920xF8 accept) | CS\u2193 00 CS\u2191. ~600ms later five scope-config register writes (01 08 / 02 03 / 06 00 / 07 00 / 08 AD) then a 0x03 status read (\u219200 01 42 2E 2E). CS polarity: PB6 LOW=assert, HIGH=deassert. Decisive: in the entire 0x08026638\u20130x08026DA4 window the ONLY GPIO output write is PC6 HIGH \u2014 stock pulses no FPGA reset/reconfig pin, and (capture Exp 7) sends ZERO USART before the 0x3B upload.",
  "decode_level": 3,
  "our_impl": "firmware/src/drivers/fpga.c: GPIO+SPI3 init in fpga_init() (PB3/4/5/6 gpio_init L1887-1912, PC6 HIGH L1917-1923, SPI3 CTRL1 L1956-1965, CTRL2=0x03 L1974, SPE L1977, SysTick delay L1995); full handshake in fpga_spi3_config_sequence() L1473-1703 (PB11 arm L1528-1538, bare CS pulse L1570-1574, 05/12/15 prelude L1592-1612, 0x3B+spi3_pump bitstream L1637-1648, 0x3A close L1653-1656, flush 00 L1659-1661, scope-config writes L1681-1696). JTAG-disable/SPI3 remap done earlier at fpga_init L1764-1787. Bitstream fpga_cal_table.h (115638 bytes, matches capture).",
  "reimpl_level": "R1",
  "reimpl_reason": "SPI3/GPIO/CS-framed handshake is register- and byte-faithful to the issue-#18 capture (R2-grade), BUT the default build inserts a non-stock USART burst BEFORE the upload (Step 3b) \u2014 a real ordering divergence stock does not do \u2014 so the region as built is R1, not R2.",
  "divergences": [
   {
    "what": "USART boot commands (0x01,0x02,0x06,0x07,0x08) sent BEFORE the SPI3 0x3B upload",
    "stock": "Sends ZERO USART before the upload (capture Exp 7: no AA-55 echoes pre-upload; only unsolicited NV-design frames). Boots straight into the SPI3 config handshake, USART silent the whole 10.5s capture.",
    "ours": "fpga.c Step 3b (L1861-1872) usart2_send_cmd(0x01/02/06/07/08) with 50ms gaps fires before fpga_spi3_config_sequence(); active in the default build (FPGA_USART_SILENT_SCOPE=0). The FPGA_USART_SILENT_SCOPE=1 experiment build removes it but is not the default.",
    "severity": "high"
   },
   {
    "what": "SPI3 prescaler historically /4 vs stock /2",
    "stock": "/2 = 60MHz (Mode 3).",
    "ours": "Now /2 (CTRL1 BR=000, L1956-1965) \u2014 matches. (Prior /4 bug fixed 2026-04-06.)",
    "severity": "low"
   },
   {
    "what": "AT32 GMUX SPI3 routing write (SPI3_GMUX_0010) not present in stock",
    "stock": "GD32 binary: legacy AFIO SWJ_CFG=010 only; no GMUX register exists in its world.",
    "ours": "fpga.c L1786-1787 writes SWJTAG_GMUX_010 + SPI3_GMUX_0010 \u2014 required on AT32 (bench-confirmed SCK dead without it). Justified MCU-port difference, not a true divergence.",
    "severity": "low"
   },
   {
    "what": "Optional FPGA reset / PDx strap / reload-0x3C paths in fpga_spi3_config_sequence",
    "stock": "Pulses NO dedicated FPGA reset/reconfig pin; only GPIO write in window is PC6 HIGH.",
    "ours": "All gated off by default (reset_port=0, strap_*=0, reload_3c=0) in the fpga_init call site L2052-2057 \u2014 diagnostic-only, off in normal build. Not a live divergence.",
    "severity": "low"
   },
   {
    "what": "Post-handshake 0x41 STATUS_REGISTER read + 600ms scope-config",
    "stock": "Capture shows ~600ms gap then 5 cfg writes + 0x03 status; no 0x41 read observed on the captured (MOSI) lines but plausibly stock-silent.",
    "ours": "Adds a Gowin 0x41 status read (L1663-1676) stock isn't confirmed to do, plus the 5 scope-config writes (L1681-1696, faithful). The extra 0x41 read is a benign diagnostic inside its own CS frame.",
    "severity": "low"
   }
  ],
  "fpga_early_boot_actions": [
   {
    "action": "Stock sends NO USART command before the 0x3B bitstream upload; our default build (Step 3b, fpga.c L1862-1871) sends 0x01/02/06/07/08 first.",
    "why_might_matter": "Commanding the FPGA's NV (already-running) user design plausibly switches its user-design SPI slave onto the shared SSPI pins, so the GW1N is in user-mode (prelude MISO driven 0x80) instead of config-wait (stock MISO floats 0xFF). If the SSPI port is owned by the user design, the 0x3B CONFIG_ENABLE/bitstream is ignored \u2014 exactly the config-entry wall. This is the single highest-value remaining lead and the default build still violates it.",
    "testable": "Cold power-cycle boot (NOT warm `fpga reinit`) with FPGA_USART_SILENT_SCOPE=1 vs =0; read fpga.init_hs prelude MISO via `status`. Stock-faithful (silent) build should show prelude MISO 0xFF (config-wait float) and 0x3A close 0xF8; the default (USART-before) build shows 0x80/00. Already partially seen (Exp 7: silencing flipped 0x80\u21920xFF). Make FPGA_USART_SILENT_SCOPE the default if confirmed."
   },
   {
    "action": "Stock holds USART2 fully disabled (UEN clear, no NVIC) through the scope boot; our default enables USART2 RE/TE/RDBFIEN/UEN + NVIC (fpga.c L1817-1824) and runs dvom/meter tasks.",
    "why_might_matter": "Even absent explicit TX, an enabled USART2 receiver plus the dvom/meter tasks may answer/clock the NV design's unsolicited frames and keep the user design active on the SSPI pins, reinforcing user-mode ownership of MISO. Same root as above \u2014 keeping the wire quiet is the stock posture.",
    "testable": "FPGA_USART_SILENT_SCOPE=1 leaves UEN clear and creates no auto-tasks; cold-boot and watch first 0x05 prelude MISO 0x80\u21920xFF and the 0x11 IDCODE for 01 20 68 1B. Compare close status 0xF8 vs FF."
   },
   {
    "action": "Exact pre-upload timing: stock raises PB11 1.0ms before the bare CS pulse and starts the upload ~3.9s after power-on; the NV design takes ~1.4s to boot.",
    "why_might_matter": "If the FPGA config-wait window is timing-gated to its own power-on/config sequence, starting the 0x3B upload too early (before NV boot settles) or too late could miss the config-wait window. Our 2000ms pre-upload wait covers NV boot, but the relative ordering vs USART/relay writes differs.",
    "testable": "Logic-analyzer capture of our own SPI3 wire (PB3/4/5/6+PC6+PB11) on a cold boot, diff against the stock issue-#18 capture timeline; confirm PB11\u2192HIGH lands 1ms before CS and no stray SCK edges occur with CS HIGH."
   }
  ],
  "verify_path": "Static-equiv for the SPI3/GPIO/CS-framed bytes is already at V-grade (byte-exact vs issue-#18 capture; size, opcodes, framing, CS polarity, PB11 timing all matched). To push the WHOLE region to V and resolve the R1\u2192R2 gap: (1) Make FPGA_USART_SILENT_SCOPE=1 the default (delete Step 3b USART-before-upload) so the built sequence matches stock's USART-silent boot. (2) Bench litmus \u2014 true COLD power-cycle boot (not warm `fpga reinit`, FPGA is stateful): read fpga.init_hs prelude MISO via the `status` shell command; PASS = prelude MISO 0xFF (config-wait float) AND fpga.h2_close_status 0xF8 AND fpga.scope_status[0]=0x03-read returns 00 01 42 2E. (3) If still 0xFF/00 after silencing USART, escalate to the wire capture in fpga_actions[3] and the external Gowin-programmer bitstream load to isolate config-entry from the rest."
 },
 {
  "region": "timers_nvic (stock master_init FUN_08023A50, phase2h/2c/2j/2l + phase3 A/C \u2014 addrs 0x08025800\u20130x080276F2)",
  "stock_summary": "TMR3 button scan (phase2h, 0x08026E00): r5=0x40000400(TMR3_CTRL1), r7=0x40000414(SWEVG), r6=0xE000E409. Writes: TMR3_PR(0x40000428)=0x13 (period=19); TMR3_DIV(0x4000042C)=prescaler computed as (APB1_tmr_freq>>13)+adj (~14648); TMR3_SWEVG|=0x01 (UG preload); TMR3_CTRL1 &= ~0x70 (CMS=00 edge-aligned, DIR=0 up); TMR3_IDEN(0x4000040C)|=0x01 (UIE); NVIC_IPR[29]=computed-from-PRIGROUP; NVIC_ISER0(0xE000E100)=0x20000000 (enable IRQ29/TIM3); TMR3_CTRL1 &= ~0x01 (CEN=0 \u2014 left STOPPED). Effective ~500\u2013819Hz (decompile's own note). TMR3 is conditionally STARTED later in phase2j (0x08027080): re-writes PR/DIV per-mode, SWEVG|=1, CNT=0, CTRL1|=0x01 (CEN). NVIC priority grouping derived everywhere from SCB_AIRCR[10:8] PRIGROUP (read at 0x08025858/0x080257D4/0x08026Exx). NVIC enables in region: ISER0=0x200 (IRQ9 EXTI3/USART2-RX edge), ISER1=0x40 (IRQ38 USART2), ISER0=0x100000 (IRQ20 SPI3, but polled), ISER0=0x20000000 (IRQ29 TIM3), ISER1=0x800 (IRQ43 TIM7/FatFs). TMR5 (0x40000C00, phase2c/2j): timer_struct_init(FUN_0802A430) then per-mode input-capture config for FPGA frequency-counter/period measurement; RCU_APB1EN|=0x20 (TMR5EN bit5); interrupt NOT enabled here. TMR7/'TMR8'-FatFs (phase2l, 0x080275D0): RCU_APB1EN|=0x40 (TMR6EN bit6); NVIC_IPR[43]; NVIC_ISER1=0x800 (IRQ43); TMRx_CTRL1|=0x01 (CEN); then xTimerCreate/xTimerStart for Timer1/Timer2; tail-call vTaskStartScheduler. (Region note's 'TMR8 ARR=99' is a mislabel \u2014 the FatFs/software-timer IRQ in this binary is TMR7/IRQ43; no TMR8 write appears in this region.)",
  "decode_level": 3,
  "our_impl": "TMR3: firmware/src/drivers/button_scan.c:255-267 (button_scan_init) + TMR3_GLOBAL_IRQHandler at button_scan.c:185. Called from main.c:540. NVIC group: relies on CMSIS/FreeRTOS default (nvic_irq_enable at button_scan.c:265); explicit nvic_priority_group_config only in dead attic/fulltest2.c:486. TMR5/EXTI3/TMR7-FatFs: ABSENT (no match in fpga.c/main.c).",
  "reimpl_level": "R1",
  "reimpl_reason": "TMR3 is register-faithful in regs/values/order (R2-quality on its own), but the broader region is R1: stock's per-mode TMR3 start-deferral, NVIC PRIGROUP-derived priorities, TMR5 (FPGA freq/period), EXTI3 RX edge path, and TMR7/FatFs timer are all unreplicated or simplified.",
  "divergences": [
   {
    "what": "TMR3 effective scan rate. Stock PR=19 + DIV~14648 \u2192 ~500-819Hz. Ours DIV=11999,PR=19 \u2192 with AT32 APB1-prescaler!=1 timer-clock doubling (240MHz) = 240e6/12000/20 = 1000Hz, not the 500Hz the source comment claims (comment assumes undivided 120MHz).",
    "stock": "~500-819Hz",
    "ours": "~1000Hz (comment says 500Hz \u2014 wrong by 2x due to timer-clock-doubling oversight)",
    "severity": "low"
   },
   {
    "what": "TMR3 start timing. Stock leaves CEN=0 in phase2h and starts it conditionally per-mode in phase2j (0x08027080) with a fresh PR/DIV reload, CNT=0, then CEN=1.",
    "stock": "configured stopped, started later by mode FSM with per-mode reload",
    "ours": "started immediately at end of button_scan_init (TMR3->ctrl1|=1 at button_scan.c:267), single fixed rate",
    "severity": "low"
   },
   {
    "what": "Preload mechanism. Stock issues SWEVG|=0x01 (UG) to force PR/DIV load. Ours writes cval=0 and relies on counter rollover/first-update to latch DIV.",
    "stock": "explicit UG event (SWEVG bit0)",
    "ours": "cval=0, no explicit UG (button_scan.c:261)",
    "severity": "low"
   },
   {
    "what": "NVIC priority grouping. Stock reads SCB_AIRCR PRIGROUP[10:8] and computes group/sub split for every IRQ.",
    "stock": "PRIGROUP-derived priorities",
    "ours": "no explicit nvic_priority_group_config in live code; CMSIS default group used via nvic_irq_enable(TMR3,5,0). Pri 5 == configMAX_SYSCALL_INTERRUPT_PRIORITY (5<<4) \u2014 at the FreeRTOS boundary, allowed but tight.",
    "severity": "low"
   },
   {
    "what": "TMR5 (FPGA frequency-counter / period measurement timer).",
    "stock": "configured (input-capture, RCU_APB1EN bit5) for meter freq/period via FPGA",
    "ours": "ABSENT (no TMR5 anywhere)",
    "severity": "med"
   },
   {
    "what": "EXTI3 rising-edge interrupt on PA3 (USART2 RX start-bit), IRQ9, in addition to USART2 RXNE.",
    "stock": "EXTI3 RTSR|=8, IMR|=8, NVIC ISER0=0x200",
    "ours": "ABSENT \u2014 USART2 RX handled without EXTI3 edge path",
    "severity": "low"
   },
   {
    "what": "TMR7/FatFs software-timer interrupt (IRQ43) + TMR6EN clock.",
    "stock": "RCU_APB1EN|=0x40, NVIC ISER1=0x800, CEN=1",
    "ours": "ABSENT (no FatFs / TMR6/7 timer)",
    "severity": "low"
   }
  ],
  "fpga_early_boot_actions": [
   {
    "action": "TMR5 input-capture init for FPGA frequency-counter/period measurement (phase2c/2j, base 0x40000C00, RCU_APB1EN bit5). Stock wires a hardware timer to capture an FPGA-driven signal for freq/period readings.",
    "why_might_matter": "Not the SPI3 config-entry path, but it is an FPGA-data-acquisition channel we never set up. If any FPGA bring-up handshake or the FPGA's own state machine expects this capture timer to be live (e.g. to gate/ack a measurement mode), its absence could leave the FPGA in an unexpected state. Low probability of affecting bitstream acceptance, but it is an unreplicated FPGA-adjacent timer.",
    "testable": "Static: confirm TMR5 isr/capture is referenced by fpga_task in stock (grep full_decompile.c for 0x40000C00 / IRQ50). Bench: scope TMR5 capture pins during stock boot vs ours; check if FPGA emits any signal into the TMR5 channel before/around config entry."
   },
   {
    "action": "EXTI3 rising-edge interrupt armed on PA3 (USART2 RX) in parallel with RXNE (phase2b, EXTI_RTSR|=8 / IMR|=8 / NVIC ISER0=0x200).",
    "why_might_matter": "FPGA USART2 command/echo channel uses PA3. Stock double-arms edge+RXNE; if the FPGA's early USART handshake (the 0x01..0x08 boot command sequence) depends on tight edge timing or wakeup-from-idle behavior that EXTI3 provides, our RXNE-only path could miss or mistime early FPGA replies that precede/accompany config entry.",
    "testable": "Bench: logic-analyze PA3 during stock boot \u2014 verify whether the FPGA sends any USART bytes BEFORE the SPI3 0x3B upload (CLAUDE.md history says 'stock sends no USART at boot', which would make this benign); if confirmed silent, downgrade. Static: check stock dvom_RX/usart2 ISR for EXTI3 vs RXNE entry."
   },
   {
    "action": "USART2 UE (enable) is conditional on last-mode (phase2i): enabled only if meter_state[0xF64]!=0, else left disabled with UE cleared (CTL0 &= ~0x2000).",
    "why_might_matter": "Ordering/timing: in scope mode stock keeps USART2 OFF at boot. If our firmware enables USART2 unconditionally, we could be driving PA2/PA3 (shared FPGA lines) at a time stock keeps them quiet, perturbing the FPGA's SPI3 config-entry window. Conversely if we leave it off when stock turns it on, FPGA boot commands never go out.",
    "testable": "Compare our USART2 init order vs SPI3 upload order; bench-capture PA2/PA3 idle state during the SPI3 0x3B window on stock vs ours."
   }
  ],
  "verify_path": "Static-equiv (cheap, gets TMR3 to V): objdump our button_scan.o and diff the TMR3 register write sequence (PR=19, DIV, IDEN UIE, NVIC IRQ29, CEN) against stock 0x08026E00-0x08026F50 \u2014 already register-matched here; accept the deliberate divergences (immediate-start, 1000Hz). Fix the source comment at button_scan.c:255 (240MHz timer clock \u2192 ~1000Hz, not 500Hz) or change DIV to 23999 if true 500Hz is desired. For the FPGA-adjacent omissions: bench litmus \u2014 run a Saleae capture on PA2/PA3 (USART2) and the TMR5 capture pin during a STOCK scope-mode boot across the SPI3 0x3B/0x3A window; confirm whether (a) FPGA emits USART before config entry and (b) TMR5 sees any FPGA edge before config. If both silent (consistent with prior 'stock sends no USART at boot' finding), mark TMR5/EXTI3 as NA-for-config and this region reaches V at R1-justified. If either is active pre-config, escalate as a real bring-up diff."
 },
 {
  "region": "dma (master_init phase3 section E, stock 0x08025BB4 \u2014 \"DMA1 channel init / SPI3 RX buffer config\")",
  "stock_summary": "Register-level decode of master_init phase3 sections D\u2013E (0x08025A0C\u20130x08025BF0). Three distinct DMA-relevant actions live here; the phase3.c annotation conflates them and mislabels two:\n\n1. DMA CLOCK ENABLE (0x0802594E): RCU_APB1EN(0x40021018-region) |= (1<<29). Note: 0x08025946 also does RCU |= 0x2. Per remaining_unknowns.md the real DMA1 clock-enable bit is DMA0EN=bit0 of RCU_AHBEN at 0x40021014 (set lazily by FUN_0803bee0/dma1_configure). In region E proper the clock-enable is the only structural DMA write.\n\n2. USART2-RX DMA (section D, 0x08025A0C\u20130x08025ADE): DMA_INTC &= ~0x70 (clear ch flags); NVIC_IPR[DMA_IRQ]=prio; DMA_CHx_MADDR = &meter->usart_rx_buf (=0x20000F5A); DMA_CHx_CTRL |= (1<<24) high-priority; DMA_CHx_CTRL |= 1 channel-enable. This DMAs the USART2 meter RX stream into a RAM circular buffer.\n\n3. \"SPI3 RX buffer / acq config\" (0x08025B8A\u20130x08025BF0): acq(0x20002B24)->dma_base=0x40005C00, display_buf=0x20000030/0x20000058, flag=1; then bl 0x08039990; then a DMA block (DMA_CTL clear bit0; DMA_CNT=0; DMA_PADDR=0; DMA_MADDR=0x80; DMA_CTL=0x9E00 mem-to-mem/16-bit/high-prio/increment; DMA_CTL&=~2; DMA_INTF&=~2) feeding spi_flash_read_config(buf,128); SPI3_CTL1|=2; SPI3_CTL0|=2.\n\nCRITICAL MIS-ANNOTATION (resolved via function_names.md L141/L252/L1090 + stock_iap_bootloader.md \u00a79A): 0x40005C00 is the USB_EPnR peripheral base, NOT an SPI3 DMA base. 0x08039990 is usb_endpoint_init_all, NOT init_acq_config; the '0x101..0x707 channel tables' are USB EP num/type pairs. So most of item 3's 'SPI3 RX' framing is actually USB endpoint init; only the mem-to-mem 0x9E00 block is a genuine SPI-flash config read. The real bulk DMA data path (LCD blit and FPGA waveform) is set up elsewhere, not in region E.",
  "decode_level": 2,
  "our_impl": "main.c:355-399 (clock enables \u2014 DMA1 clock NEVER enabled; only DMA2 lazily in drivers/dac_output.c:78). LCD blit: drivers/lcd.c:142-149 (direct memory-mapped *LCD_DATA_ADDR writes, no DMA). SPI3 FPGA RX: drivers/fpga.c:183,236,246 (polled busy-wait on RDBF). SPI flash: drivers/flash_fs.c:303 (polled). USART2 RX: interrupt-driven (no DMA). SPI3 CTL2 DMA bits set but unused: drivers/fpga.c:1974 (FPGA_SPI->ctrl2=0x03).",
  "reimpl_level": "R1",
  "reimpl_reason": "R1: we implement the LCD pixel push (functionally, via direct EXMC writes instead of DMA1) and FPGA/flash I/O (via polling instead of DMA), but we never enable the DMA1 clock, never configure any DMA1 channel, and do not DMA USART2-RX. Region E's structural DMA setup is essentially absent; the equivalent work is done by different mechanisms. Not NA because stock genuinely uses DMA here for USART2-RX and as scaffolding for the later FSMC waveform DMA.",
  "divergences": [
   {
    "what": "LCD framebuffer transfer mechanism",
    "stock": "DMA1 channel (Ch2 per WAVEFORM_DATA_PATH_CORRECTION, or Ch4 per compliance audit #18) mem\u2192EXMC blit via dma1_configure (FUN_0803bee0)",
    "ours": "Direct memory-mapped 16-bit writes to *LCD_DATA_ADDR (0x60020000), no DMA (lcd.c:142-149)",
    "severity": "low"
   },
   {
    "what": "USART2 meter RX path",
    "stock": "DMA channel: DMA_CHx_MADDR=0x20000F5A, CTRL|=(1<<24)|1 (high-prio circular RX into meter rx buf)",
    "ours": "Interrupt-driven USART2 RX, no DMA channel",
    "severity": "low"
   },
   {
    "what": "DMA1 peripheral clock",
    "stock": "RCU enables DMA clock (APB1EN bit29 here; AHBEN bit0 DMA0EN for DMA1 in dma1_configure)",
    "ours": "DMA1 clock never enabled (main.c:355-399 enable only GPIO/IOMUX/XMC; only DMA2 enabled, in dac_output.c:78)",
    "severity": "low"
   },
   {
    "what": "SPI3 RX transport",
    "stock": "SPI3_CTL1|=2 (RXDMAEN) \u2014 DMA-capable RX path scaffolded",
    "ours": "FPGA_SPI->ctrl2=0x03 set but NO DMA channel configured; all SPI3 reads polled (fpga.c:183/236/246). Bits are no-ops",
    "severity": "low"
   },
   {
    "what": "acq_config struct init (0x20002B24)",
    "stock": "sets dma_base, display_buf A/B, flag=1, then usb_endpoint_init_all + spi_flash_read_config(128)",
    "ours": "no equivalent acq_config struct / SPI-flash saved-settings read at this stage",
    "severity": "med"
   }
  ],
  "fpga_early_boot_actions": [
   {
    "action": "Stock reads the FPGA waveform/ADC stream over the FSMC parallel bus at 0x60020000 via DMA1 Ch2 (init_dma_acquisition/FUN_0803fee0: DMA1_C2PADDR=0x60020000, C2CTL=0x4543, peripheral\u2192memory, 16-bit, FTFIE). Our firmware instead reads waveform over SPI3 polling (spi3_xfer(0xFF)), which returns constant 0xFF \u2014 no scope data on that bus.",
    "why_might_matter": "This is a structural architecture mismatch on the FPGA DATA path. If stock never expects bulk scope samples on SPI3 (SPI3 is command/config only) and instead clocks them out the FSMC A17 data register, then our SPI3-centric acquisition model may be fundamentally aimed at the wrong bus. The FPGA's user-mode behavior (and possibly its willingness to leave config mode / accept the bitstream) could be tied to the FSMC-side handshake the stock performs, which we omit entirely. NOTE: this DMA setup is in the acquisition task, not region E itself; region E only enables the DMA clock + USART2-RX DMA. Flagged here per the hypothesis-hunt mandate.",
    "testable": "Saleae/logic-capture the FSMC data lines (A17=1, 0x60020000) during a stock boot+scope-arm and compare to SPI3. Confirm whether stock ever clocks scope samples on SPI3 at all. If not, our SPI3 scope-read is chasing a non-existent path. Statically: confirm FUN_0803fee0 (init_dma_acquisition) is the sole bulk-sample reader and that SPI3 in stock only carries the 0x3B/0x3A config + meter cmds."
   },
   {
    "action": "Stock DMAs USART2 meter RX into 0x20000F5A as a free-running channel (DMA_CHx_MADDR, CTRL high-prio + enable) established at master_init time, before tasks run.",
    "why_might_matter": "Low-probability for the config-entry wall, but the FPGA NV design services USART meter traffic; if the FPGA expects the host to be continuously draining RX via DMA from boot, our interrupt-driven late-start RX could change USART timing/backpressure the FPGA sees during its early-boot autobaud/handshake.",
    "testable": "Compare USART2 RX byte timing on a stock boot vs ours with a logic analyzer in the first 100ms; check whether the FPGA emits any unsolicited frames before the host's first TX that our late-init RX might miss."
   },
   {
    "action": "Stock does spi_flash_read_config(...,128) (the 0x9E00 mem-to-mem DMA block) at this point to load saved device/scope settings from W25Q flash into acq_config.",
    "why_might_matter": "Indirect: the loaded config includes voltage-range/timebase/cal indices that feed the scope trigger DAC and the FPGA command parameters. Wrong/absent config could mean we send the FPGA acquisition params it doesn't expect, but this does not gate config-mode entry.",
    "testable": "Read 128 bytes from the stock flash config region and diff against the defaults our firmware uses; confirm none of those bytes are FPGA-config-mode-relevant (they are scope UI/cal settings, expected to be a dead end for the config wall)."
   }
  ],
  "verify_path": "Static-equiv to V: (1) Re-decode 0x08025A0C\u20130x08025BF0 against full_decompile.c to nail which physical DMA channel/index the USART2-RX block uses (the phase3 generic 'DMA_CHx' must be resolved to a concrete 0x40020000+offset) and to formally confirm 0x40005C00=USB (kills the 'SPI3 RX buffer' label in this region permanently) \u2014 that closes the last decode gap to reach decode_level 3. (2) Document that our LCD-direct-write and SPI3-polled choices are functionally equivalent for their respective jobs (no FPGA-config dependency on DMA itself). Bench litmus for the FPGA hypothesis (separate from region E): logic-capture the FSMC 0x60020000 data lines during a stock scope-arm to prove/disprove that scope samples flow over FSMC+DMA1Ch2 (not SPI3) \u2014 this is the highest-value test surfaced by this region and should be run before any further SPI3 bitstream-replay attempts."
 },
 {
  "region": "freertos_objects (stock 0x08025BFA-0x08025D8E, master_init phase3 F/G/H/I)",
  "stock_summary": "Stock creates RTOS objects in strict order via xQueueGenericCreate (0x0803AB74), xTimerCreate wrapper (0x0803BD88), xTaskCreate wrapper (0x0803B6A0), then xQueueGenericSend (0x0803ACF0) / xSemaphoreTake (0x0803B3A8). No MCU peripheral register writes in this region \u2014 it writes only RTOS handle pointers into RAM. (1) 4 QUEUES: usart_cmd@0x20002D6C [len20,item1,type0]; secondary_cmd/button@0x20002D70 [15,1,0]; usart_tx@0x20002D74 [10,2,0]; spi3_data@0x20002D78 [15,1,0]. (2) 3 BINARY SEMAPHORES (type3,len1,item0): meter_sem@0x20002D7C; fpga_sem1@0x20002D80; fpga_sem2@0x20002D84. (3) 2 TIMERS: Timer1@0x20002D88 [period10tk, autoreload, cb=0x080400B9]; Timer2@0x20002D8C [period1000tk, autoreload, cb=0x080406C9]. (4) 6 TASKS (entry,name,stackWords,prio,handle): display(0x0803DA51,384,prio1,@0x2D90); key(0x08040009,128,prio4,@0x2D94); osc(0x0804009D,256,prio2,@0x2D98); fpga(0x0803E455,128,prio3,@0x2D9C); dvom_TX(0x0803E3F5,64,prio2,@0x2DA0); dvom_RX(0x0803DAC1,128,prio3,@0x2DA4). (5) INITIAL SEM STATE: give fpga_sem1 (AVAILABLE), take meter_sem (EMPTY), take fpga_sem2 (EMPTY). The 8-task count in CLAUDE.md = these 6 + Timer1/Timer2 FreeRTOS daemon-driven timer callbacks.",
  "decode_level": 3,
  "our_impl": "main.c:534-535 (2 queues), 543-544 (display/key tasks), 566-577 (2 timers); drivers/fpga.c:2214-2216 (3 queues/sem), 2232-2235 (dvom_TX/dvom_RX/fpga/meter_poll tasks). No fpga_sem1/fpga_sem2 equivalent; no osc task; no initial give/take sequence.",
  "reimpl_level": "R1",
  "reimpl_reason": "Most objects present with matching names/stacks/prios, but the FPGA acquisition SEMAPHORE HANDSHAKE (fpga_sem1 given, fpga_sem2 taken) is entirely absent \u2014 ours uses a 1-byte trigger queue instead \u2014 and the stock 'osc' acquisition task is folded into our 'fpga' task. These are the gating primitives for scope acquisition start, so faithfulness is partial.",
  "divergences": [
   {
    "what": "FPGA gating semaphores fpga_sem1(0x20002D80)/fpga_sem2(0x20002D84) \u2014 stock creates 2 binary sems and sets initial state (sem1 GIVEN=go-ahead-acquire, sem2 TAKEN=wait-for-trigger)",
    "stock": "xQueueGenericCreate(1,0,3) x2 + xSemaphoreGive(sem1) + xSemaphoreTake(sem2,0)",
    "ours": "ABSENT \u2014 no fpga semaphores at all; acquisition gated by a 1-byte spi3_acq_queue trigger (fpga.c:2215,1310) instead of the sem handshake",
    "severity": "med"
   },
   {
    "what": "meter_semaphore initial TAKE (start empty)",
    "stock": "create meter_sem(type3) then xSemaphoreTake(meter_sem,0) so it starts unavailable, blocking meter until USART RX data arrives",
    "ours": "meter_sem=xSemaphoreCreateBinary() (fpga.c:2216) \u2014 FreeRTOS binary sems already start empty, so the explicit take is a no-op; behaviorally equivalent",
    "severity": "low"
   },
   {
    "what": "'osc' task (entry 0x0804009D, stack256, prio2) \u2014 dedicated oscilloscope acquisition task",
    "stock": "separate xTaskCreate for 'osc' prio2 distinct from 'fpga' prio3",
    "ours": "no 'osc' task; acquisition merged into fpga_acquisition_task named \"fpga\" (fpga.c:2234, stack256 prio3). One task does both roles",
    "severity": "med"
   },
   {
    "what": "usart_cmd_queue (0x20002D6C, len20 item1) and spi3_data_queue (0x20002D78, len15 item1)",
    "stock": "two base queues created",
    "ours": "no usart_cmd_queue equivalent (our RX is processed directly in fpga_usart_rx_task, no command queue); spi3_acq_queue (15,1) matches spi3_data shape but is used as a trigger queue not a data queue",
    "severity": "low"
   },
   {
    "what": "Object creation ORDER and split location",
    "stock": "all 4 queues + 3 sems + 2 timers + 6 tasks created in one contiguous block inside master_init BEFORE scheduler start, deterministic order",
    "ours": "split across main.c (display/key + 2 timers) and fpga.c:fpga_create_tasks (FPGA queues/sem/tasks), and gated on fpga.initialized \u2014 if init fails no FPGA tasks exist at all",
    "severity": "low"
   },
   {
    "what": "display/key stack sizes",
    "stock": "display=384 words, key=128 words",
    "ours": "display=768 words, key=384 words (fpga.c dvom_TX=64/dvom_RX=128/fpga=256 match stock)",
    "severity": "low"
   },
   {
    "what": "Timer1 (period 10tk, cb 0x080400B9) and Timer2 (period 1000tk, cb 0x080406C9)",
    "stock": "two stock timers with specific housekeeping/measurement callbacks",
    "ours": "1sec + health timers (main.c:566,573) \u2014 different periods/callbacks; functionally our own housekeeping, not stock-faithful",
    "severity": "low"
   }
  ],
  "fpga_early_boot_actions": [
   {
    "action": "Stock creates fpga_semaphore1 and GIVES it (xSemaphoreGive at 0x08025D76) before scheduler start, and creates fpga_semaphore2 TAKEN. These gate the fpga/osc acquisition handshake. Our firmware replaces this with a plain 1-byte queue and never models the two-sem handshake.",
    "why_might_matter": "If the stock fpga task blocks on fpga_sem2 until the osc task (or an ISR) gives it post-config, then the SPI3 bitstream-config phase and the acquisition phase are SERIALIZED by these sems. Our merged single-task design may run the 0x3B/0x3A config and the 0x04/0x05 reads with different timing/ordering than stock, which could matter if the FPGA needs config fully settled (status 0xF8 + the five config writes) before any data read touches the SSPI pins.",
    "testable": "Static: trace where stock gives/takes fpga_sem1/sem2 (grep fpga_task_annotated.c + osc task entry 0x0804009D for xSemaphoreGive/Take on 0x20002D80/0x20002D84) to recover the exact config\u2192acquire ordering. Bench: with a logic analyzer, compare stock inter-phase timing (0x3A close \u2192 600ms \u2192 5 config writes \u2192 first 0x04 read) against ours; verify we don't issue a 0x04/0x05 read before the post-0x3A config-write sequence completes."
   },
   {
    "action": "Stock runs a dedicated 'osc' task (prio2, 0x0804009D) separate from the 'fpga' task (prio3). Priority ordering: key(4) > fpga/dvom_RX(3) > osc/dvom_TX(2) > display(1). We collapse osc into fpga at prio3.",
    "why_might_matter": "The prio split means stock's fpga task (config/SPI3) preempts the osc task. If the bitstream upload or post-config handshake relies on the fpga task running to completion at prio3 while osc waits at prio2, our single prio3 task may interleave config and data reads differently \u2014 but this is timing, not a missing register write.",
    "testable": "Static review of osc task body (0x0804009D) to confirm whether it touches SPI3/USART or only post-config data; if it only consumes already-configured data, the merge is benign (downgrade to low)."
   },
   {
    "action": "Stock creates a usart_cmd_queue (0x20002D6C, len20) feeding the FPGA USART command path; we have no command queue (RX handled inline).",
    "why_might_matter": "Low FPGA-config risk \u2014 this is meter/command plumbing, not the SPI3 config path. Unlikely to affect bitstream acceptance.",
    "testable": "Confirm via fpga_task_annotated.c that usart_cmd_queue carries only post-boot meter/scope commands, not any boot-time config opcode."
   }
  ],
  "verify_path": "Static-equivalence: (1) Recover the full fpga_sem1/fpga_sem2 give/take graph by reading fpga task entry 0x0803E455 and osc task entry 0x0804009D in full_decompile.c, mapping every xSemaphoreGive/Take on 0x20002D80/0x20002D84 \u2014 this resolves whether the two-sem handshake serializes config-vs-acquire (the one real open question keeping this from V). (2) Confirm Timer1 cb 0x080400B9 / Timer2 cb 0x080406C9 do not touch SPI3/USART/FPGA pins (grep their bodies for 0x4000xxxx SPI3/USART2 / GPIO BSRR). If both are clean, this region's only material divergence is the sem-handshake/osc-merge, which is a scheduling concern, not a missing FPGA register write \u2014 then push to V by a bench logic-analyzer capture comparing stock vs ours inter-phase SPI3 timing (0x3A\u2192config-writes\u2192first 0x04 read)."
 },
 {
  "region": "flash_cfg_cal \u2014 stock SPI-flash saved-config read + meter_state unpack + scope/meter cal-table load/defaults (phase3 \u00a7F SPI-flash read @0x08025B14/0x08025BB8; \u00a7K unpack @0x08025D92; \u00a7L cal-default check @0x080261A8; phase4 \u00a7H validate/restore @0x08025D92\u20130x08025E50, defaults @0x08026198\u20130x08026506)",
  "stock_summary": "Pure RAM + SPI-flash region; NO peripheral register writes to FPGA/SPI3/USART. Sequence: (1) phase3 \u00a7F @0x08025BB8: DMA SPI-flash read of ~128B saved config into a buffer (DMA_MADDR=0x80, DMA_CTL=0x9E00 mem-to-mem 16-bit hi-prio incr; SPI3_CTL1|=2 RXDMA, SPI3_CTL0 TXDMA) \u2014 preceded by acq_config init @0x20002B24 (dma_base=0x40005C00, display_buf=0x20000030/0x20000058, flag=1) and init_acq_config()@0x08039990. (2) \u00a7K/\u00a7H @0x08025D92: load config word0, signature=word0&0xFF; ==0x55 \u2192 restore; ==0xAA \u2192 ms[0xF68]=8 then restore; else \u2192 ms[0xF68]=8, ms[0xF60]=1, use_defaults. Restore unpacks packed words into meter_state(0x200000F8): ms[0x00..0x02] relay/mode, ms[0x03] range(u32), ms[0x07/0x14/0x16/0x17] scope/probe/timebase, ms[0x18/0x2D/0x34/0x35] coupling/trigger, ms[0x1A](u32), ms[0x3C/0x23A/0x354/0xF39], ms[0x4C](u32), ms[0x232/0x236] scope-cal-scale/offset, ms[0xE58..E61]/ms[0xE5C] timer params, ms[0xF60] mode, ms[0xF64] saved-mode(u16), ms[0x08/0x09], ms[0x0A](u16)/ms[0x48]; then 40 cal words from config[14..] split low\u2192gain table ms[0x260+i*2], high\u2192offset table ms[0x2D8+i*2]; trailing ms[0x350]=config[0x128], ms[0x352]=config[0x12C]. (3) \u00a7L @0x080261A8: read ms[0x34E]; if ==0xFFFF||==0 \u2192 write ~800B hardcoded factory cal block @0x080261BE\u20130x08026506: scope gains ms[0x260..]\u22480x0650-0x0670 (e.g. *(u32*)0x260=0x06650667, 0x264=0x065B065E, 0x268=0x0669065A...), scope offsets ms[0x2D8..]\u22480x0630-0x0660, meter gains ms[0x29C..0x2D6]\u22480x0CB0 (0x29C=0x0CC40CCE...), meter offsets ms[0x310..0x34E]\u22480x0CA0, terminal ms[0x34E]=0x05FA0CA4. Else branch straight to phase5 GPIO/SPI3 init @0x0802650A. No FPGA/SPI3/USART writes anywhere in this region (confirmed by phase4 lines 872-879).",
  "decode_level": 3,
  "our_impl": "ABSENT for the cal-restore path. Closest analogues: firmware/src/drivers/meter_data.c:177-178 (hardcoded METER_CAL_LOW_OHM_FACTOR=0.0304f / METER_CAL_KOHM_FACTOR=0.001f, single per-device factor, bench unit #1); firmware/src/util/config.c + config.h (clean-room \"OSC2\" UI settings store, RAM stub, carries NO scope/meter gain-offset cal tables); firmware/src/drivers/flash_fs.c (SPI-flash mutex/atomic wrapper, driver calls still stubbed \u2014 no read_config). No equivalent of stock saved-config read, magic 0x55/0xAA validation, meter_state(0x200000F8) unpack, 40-entry gain/offset cal table, or factory cal defaults exists.",
  "reimpl_level": "R0",
  "reimpl_reason": "R0: our firmware performs none of the stock saved-config read, magic validation, meter_state unpack, or 40-entry scope/meter cal-table load/default-write; meter cal is a single hardcoded per-device factor and config.c is an unrelated UI store.",
  "divergences": [
   {
    "what": "Saved-config read from SPI flash at boot (stock DMA-reads ~128-300B persistent config into meter_state)",
    "stock": "phase3 \u00a7F @0x08025BB8 spi_flash_read_config + phase4 \u00a7H unpack into 0x200000F8",
    "ours": "Absent \u2014 flash_fs.c driver stubbed, no boot-time config read; UI config.c is a RAM stub",
    "severity": "med"
   },
   {
    "what": "Magic-byte config validation (0x55 valid / 0xAA meter-mode / else defaults)",
    "stock": "@0x08025D9A cmp #0x55 / cmp #0xAA, sets ms[0xF68]/ms[0xF60] accordingly",
    "ours": "Absent \u2014 no signature scheme on any meter/scope state",
    "severity": "low"
   },
   {
    "what": "40-entry per-range scope+meter gain/offset calibration table restore into meter_state",
    "stock": "config[14..] \u2192 ms[0x260..0x34E] gain/offset pairs (scope ~0x0650, meter ~0x0CB0)",
    "ours": "Absent \u2014 meter uses one hardcoded factor 0.0304f/0.001f (meter_data.c:177); scope has no per-range cal-table",
    "severity": "high"
   },
   {
    "what": "Factory cal defaults when cal corrupt (ms[0x34E]==0xFFFF/0)",
    "stock": "~800B hardcoded block @0x080261BE-0x08026506",
    "ours": "Absent \u2014 no fallback cal block; values are compile-time constants tuned for unit #1",
    "severity": "med"
   },
   {
    "what": "Restored mode/range/coupling/trigger device state (ms[0x00..0x4C], ms[0xF60/0xF64/0xF68])",
    "stock": "unpacked from saved config, drives initial screen + relay/gain restore later",
    "ours": "Partial/divergent \u2014 clean-room scope_state + config.c hold our own subset, not stock-faithful offsets/values",
    "severity": "low"
   }
  ],
  "fpga_early_boot_actions": [
   {
    "action": "None directly in this region. Stock's own audit (master_init_phase4.c lines 872-879) states this region makes NO SPI3 writes to the FPGA, NO USART2 commands, and does not touch the FPGA data bus (0x40003C00). The SPI3 GPIO + peripheral init + 0x40/0x00/0x05 handshake live in the NEXT region (phase3 \u00a7M-P @0x0802650A+), not here.",
    "why_might_matter": "Indirect only: the restored ms[0x14] probe_type, ms[0x16/0x17] timebase, ms[0xF39/0xF64] saved mode, and the scope cal table feed the LATER FPGA scope-config command burst (CH offset/gain/trigger sends in fpga.c) and the DAC1 trigger-comparator computation. If our firmware boots with absent/wrong meter_state cal+mode, any subsequent FPGA scope-config commands carry different parameters than stock \u2014 but this does NOT affect whether the GW1N-UV2 accepts the SPI3 bitstream (that is config-mode entry, upstream of any parameter). So for the bitstream-acceptance mystery this region is effectively clean.",
    "testable": "Static: diff the parameter bytes our fpga.c sends in the post-bitstream scope-config burst (0x04/0x05 reads, CH offset/gain cmds) against a stock Saleae boot capture; if they differ, trace back to absent meter_state cal/mode restore. This is a measurement-accuracy test, NOT a config-entry test \u2014 it will not move the bitstream-acceptance needle."
   }
  ],
  "verify_path": "Static-equivalence is sufficient to reach V for the FPGA-mystery purpose: stock's annotated KEY FINDINGS (phase4.c:867-921) plus the full instruction-level decode of \u00a7F/\u00a7K/\u00a7L/\u00a7H confirm zero FPGA/SPI3/USART register access in this region \u2014 re-read those address ranges in full_decompile.c to independently confirm no str to 0x40003Cxx / 0x40004400 appears between 0x08025B14 and 0x0802650A. For functional parity (not FPGA): bench litmus \u2014 boot stock, dump meter_state 0x200000F8+0x260..0x34E over SWD to capture the real per-unit 40-entry cal table, compare to our hardcoded 0.0304f path; and confirm via Saleae that no SPI3 activity occurs during this window (it should be silent until the \u00a7P handshake). Conclusion to record: this region is NOT a candidate for the bitstream-acceptance failure; it is a measurement-accuracy gap (R0) to be closed by reading real cal data, separate from the config-entry wall."
 },
 {
  "region": "button_matrix (stock addr 0x08025468-0x0802553C, phase4 \"Section D\")",
  "stock_summary": "GROUND-TRUTH DISASSEMBLY CONTRADICTS THE ANNOTATION. I disassembled file offset 0x1E468 (= flash 0x08025468, app link base 0x08007000) through 0x1E53C as Thumb-2. The region 0x08025468-0x0802553C contains ZERO GPIO/RCU register writes. It is a tight VFP (FPU) floating-point loop: vadd.f32/vsub.f32/vmul.f32/vstr/vldr operating on a struct with stride-256 rows (store offsets 0x110/0x210/0x310, 0x114/0x214/0x314, 0x118/0x218/0x318, 0x11c/0x21c/0x31c) \u2014 i.e. butterfly/cross-product/matrix math. Loop control at 0x8025518 (adds r1,#16; adds r0,#1; bcc.w 0x80253e8). At 0x8025524/0x802552a it calls 0x802fb80 (vPortFree x2), then 0x802553e onward is a transpose/swap loop (ldr/str pairs with bic #3, cmp #0x1a0=416, vsqrt.f32 + vcmp at 0x8025614 for vector normalization). This is an FFT/DSP or measurement-engine math routine, NOT master_init button GPIO. The actual register writes the prompt expects (RCU_APB2EN |= GPIOA/B/C/D/E, gpio_init for PA7/PB0/PC5/PE2 rows, PA8/PC10/PE3 cols, PC8/PB7/PC13 passive) are described ONLY as commented-out pseudo-calls in the annotation file master_init_phase4.c:292-329; they have no register-level values and do not match the binary at this address. The real button matrix scan/GPIO toggling lives in input_and_housekeeping (~0x08039188), not here.",
  "decode_level": 3,
  "our_impl": "firmware/src/drivers/button_scan.c (matrix_scan at :101-179, button_scan_init at :234-268, pull config at :251-253). This is our complete button driver, but it corresponds to the stock scan in input_and_housekeeping (~0x08039188), NOT to the FPU math actually at 0x08025468-0x0802553C.",
  "reimpl_level": "NA",
  "reimpl_reason": "The literal stock bytes at 0x08025468-0x0802553C are VFP DSP/matrix math with no GPIO content, so there is nothing button-related in THIS region to reimplement; the region label is mis-attributed. (Separately, our button_scan.c is a faithful, bench-confirmed reimplementation of the REAL stock button matrix that lives in input_and_housekeeping \u2014 including the PB7 pull-down fix \u2014 but that is a different address range.)",
  "divergences": [
   {
    "what": "Region address vs content: prompt/annotation claim 0x08025468-0x0802553C = 'Button Matrix GPIO + Timer Setup'; binary at that exact address is VFP float math (vadd/vmul/vsub/vstr loop with stride-256 struct, vPortFree calls, vsqrt normalization).",
    "stock": "FPU butterfly/transpose/normalize loop, no GPIO/RCU writes, no TMR3 config.",
    "ours": "N/A \u2014 our button driver does not (and should not) live at this address; no equivalent FPU routine reviewed here.",
    "severity": "high"
   },
   {
    "what": "Annotation master_init_phase4.c Section D stubs the register writes as comments (gpio_init pseudo-calls, RCU_APB2EN |= 0x10/0x40/0x04/0x08 with duplicated lines) and lists pins (GPIOC pin4, GPIOE 0x10/0x20/0x40, PA15, PB 0x800/0x400, PA 0x400) that partly contradict the documented matrix pinout (e.g. PA15 is a gain-select pin, not a button row).",
    "stock": "No decoded register-level values; the named pins are inconsistent with the confirmed matrix (rows PA7/PB0/PC5/PE2, cols PA8/PC10/PE3).",
    "ours": "button_scan.c uses the hardware-CONFIRMED matrix pinout, so where they differ, ours is the trustworthy source.",
    "severity": "med"
   },
   {
    "what": "TMR3 scan-timer setup: stock configures TMR3 for the 500Hz scan ISR somewhere in the boot path; it is NOT in this region. Our TMR3 config (button_scan.c:255-267: div=11999, pr=19, iden=1, ctrl1 enable) was not diffable against this address.",
    "stock": "Not present at 0x08025468-0x0802553C.",
    "ours": "button_scan.c:255-267 \u2014 plausible but unverified against the true stock TMR3 init location.",
    "severity": "low"
   }
  ],
  "fpga_early_boot_actions": [],
  "verify_path": "1) Static: re-base the phase4 annotation to ground truth \u2014 the FPU math at 0x08025468 belongs to a different function than 'button setup'; locate the TRUE master_init GPIO-matrix block by scanning master_init (0x08023A50+) for writes to 0x40010800 (GPIOA CRL/CRH), 0x40010C00 (GPIOB), 0x40011000 (GPIOC), 0x40011800 (GPIOE) and RCU_APB2EN (0x40021018), then diff THOSE against button_scan_init. 2) Confirm button matrix scan equivalence by diffing our matrix_scan (button_scan.c:101-179) against input_and_housekeeping (~0x08039188) disassembly: same row-drive/col-read directions, same pull config (PB7 pull-DOWN active-HIGH, PC8/PC13 pull-UP active-LOW). 3) Bench litmus (already passed historically, 2026-04-06): all 15 buttons register correctly including PRM/PB7 \u2014 this confirms the REAL reimplementation is correct regardless of the mislabeled address. 4) Fix the annotation file so future agents are not sent to an FPU routine when hunting button GPIO."
 },
 {
  "region": "analog_frontend (master_init phase4 B/E/F: stock 0x08025270-0x08025370 DAC cal, 0x0802553C-0x0802559A meter MUX calls, 0x0802559A-0x08025662 relay restore; core = FUN_080018A4 gpio_mux_portc_porte + FUN_08001A58 gpio_mux_porta_portb)",
  "stock_summary": "Phase4-E (0x0802553C) calls two analog-MUX functions with values from saved meter_state, then Phase4-F (0x0802559A) restores coupling/probe relays. Register-level decode of the two MUX functions (full_decompile.c:2206-2394):\n\nFUN_080018A4 (gpio_mux_portc_porte, arg = ms[0x02] meter mode, 10-way switch 0..9):\n - GPIOC BSRR(set)=0x40011010, GPIOC BRR(clr)=0x40011014; GPIOE BSRR(set)=0x40011810, GPIOE BRR(clr)=0x40011814.\n - Bits used: GPIOC 0x1000=PC12; GPIOE 0x10=PE4, 0x20=PE5, 0x40=PE6.\n - case 2: GPIOC 0x40011010=0x1000 (PC12 SET); GPIOE 0x40011810=0x20 (PE5 SET); 0x40011814=0x40 (PE6 CLR). i.e. PC12=H, PE5=H, PE6=L, PE4 untouched.\n - Other cases set/clear PC12 and various PE4/5/6 combos (uVar4 0xf800/0xf804 select set-vs-clear via the +0xffff0000 trick that swaps BSRR<->BRR offset). Then ALWAYS: computes a VFP value = (gain-offset)/DAT_08001a54 * (ms[0xFC]+100) + offset, gain/offset read from cal tables selected by ms[0x125] (tb range, >=5/==4/else) and ms[0x10C]==3 (100x probe): tables at 200003a8/36c, 3bc/380, or 394/358. Result -> DAC1 D12R at 0x40007408 (low 12 bits), then 0x40007404 |= 1 (DAC1 enable). This is the SCOPE TRIGGER COMPARATOR DAC threshold.\n\nFUN_08001A58 (gpio_mux_porta_portb, arg = ms[0x03] meter range, 10-way switch 0..9):\n - GPIOA BSRR(set)=0x40010810, GPIOA BRR(clr)=0x40010814; GPIOB BSRR(set)=0x40010c10, GPIOB BRR(clr)=0x40010c14.\n - Bits: GPIOA 0x8000=PA15; GPIOB 0x800=PB11, 0x400=PB10. (PA10 NOT touched here despite CLAUDE.md folklore.)\n - Per-case sets/clears PA15, PB11, PB10. Then computes second VFP value = (gain-offset)/DAT_08001c5c*(ms[0xFD]+100)+offset from tables 20000420/3e4, 434/3f8, or 40c/3d0 -> writes 0x40001c34 (a second comparator/CCR target, base 0x40001c00).\n\nPhase4-F relay restore (master_init_phase4.c F): ms[0x00]!=0 -> GPIOD_BOP=0x1000 (PD12 set, CH1 DC couple) else GPIOD_BC; ms[0x01] -> PD13; then ms[0x14] probe switch: case1 PC2+PC1 set, case3 PC2 set/PC1 clr, case2 both clr.\n\nPhase4-B (DAC cal 0x08025270) precomputes the same DAC values into I2C1+8/I2C1+4 region per CH1/CH2 before the MUX calls. Ordering: B (precompute) -> E (mux calls drive GPIOC/E + GPIOA/B + DAC + enable) -> F (PD12/PD13 coupling + PC1/PC2 probe atten).",
  "decode_level": 3,
  "our_impl": "firmware/src/drivers/fpga.c:2081-2127 (hardcoded DCV relay block in fpga_init), :813-844 (fpga_set_meter_frontend_baseline diagnostic), :728-760 (fpga_set_scope_frontend_range \"intentionally simple\" approximation). DAC1 trigger comparator path: ABSENT (only read-only diag in usb_debug.c:1618). meter_data.c does NO frontend GPIO (decoder only).",
  "reimpl_level": "R1",
  "divergences": [
   {
    "what": "Cal-table-derived DAC1 scope trigger comparator value (stock writes 0x40007408 low-12 + enables 0x40007404 |=1 in FUN_080018A4, and a second value to 0x40001c34 in FUN_08001A58, both computed from gain/offset cal tables in meter_state)",
    "stock": "VFP interpolation per range -> DAC1 D12R=0x40007408, DAC1 enable 0x40007404|=1; second target 0x40001c34",
    "ours": "ABSENT. Our DAC1 use (dac_output.c) is siggen PA4 waveform out via TMR6+DMA, unrelated. Trigger comparator threshold never set.",
    "severity": "high"
   },
   {
    "what": "Frontend relay bit pattern is invented, not decoded from the FUN_080018A4 switch",
    "stock": "case 2: PC12=H, PE5=H, PE6=L (PE4 untouched)",
    "ours": "fpga.c:2095-2098 PC12=H, PE4=H, PE5=L, PE6=H \u2014 PE5/PE6 inverted vs stock, PE4 driven where stock leaves it alone",
    "severity": "med"
   },
   {
    "what": "PA10 gain pin",
    "stock": "FUN_08001A58 only touches PA15/PB11/PB10; PA10 is NOT a gain-select in this function",
    "ours": "fpga.c:2112-2115 drives PA15 and PA10 HIGH as 'gain bits' \u2014 PA10 has no stock basis here",
    "severity": "low"
   },
   {
    "what": "Coupling/probe relays driven from saved state",
    "stock": "PD12/PD13 from ms[0x00]/ms[0x01]; PC1/PC2 from ms[0x14] probe type (3-case)",
    "ours": "PD12/PD13 configured elsewhere (fpga.c:1519 lists them) but not driven from a restored config; PC1/PC2 probe-atten switch ABSENT",
    "severity": "med"
   },
   {
    "what": "Values are state-driven (saved_config restore feeds ms[] -> mux arg), ordering B->E->F",
    "stock": "mode/range come from flash saved_config (0x08006000) restore",
    "ours": "fixed DCV constants, no saved-config restore, single ordering",
    "severity": "med"
   }
  ],
  "fpga_early_boot_actions": [
   {
    "action": "Stock computes and loads the DAC1 trigger-comparator threshold (0x40007408 + enable 0x40007404|=1) and the 0x40001c34 value from the per-range cal table during this analog_frontend phase; our firmware never writes either.",
    "why_might_matter": "This is an internal MCU comparator/DAC, NOT a direct FPGA config write, so it is unlikely to gate SSPI config-receive. But the FPGA's scope-capture datapath may sample/compare against this analog threshold; if the FPGA waits on a valid trigger-DAC level before streaming, a missing/zero threshold could look like 'FPGA not accepting/working' even though config succeeded. Lower-probability for the config-entry wall specifically.",
    "testable": "Bench: after our boot, read 0x40007408/0x40007404 (already exposed in usb_debug.c:1618) \u2014 expect 0/disabled. Then port FUN_080018A4's VFP calc, write a mid-scale threshold + enable DAC1, and re-check whether 0x04/0x05 channel reads return non-FF scope data."
   },
   {
    "action": "Relay/MUX writes here are pure GPIO (GPIOC/E/A/B/D + PC1/PC2/PD12/PD13) with NO SPI3 or USART in this phase \u2014 confirmed by the annotated source's KEY FINDINGS 1-2.",
    "why_might_matter": "Confirms (negatively) that the analog_frontend region is NOT the source of the SSPI config-entry failure: stock sends zero SPI3/USART traffic to the Gowin here. This narrows the FPGA-accept mystery to the dedicated SPI3 config phase (Phase 5, 0x08026540+), not this region.",
    "testable": "Static: grep this address range for SPI3 (0x40003c00) / USART2 (0x40004400) writes \u2014 none exist. Bench: a logic-analyzer capture across this phase shows no SCK/MOSI activity on PB3/PB5 or PA2."
   }
  ],
  "reimpl_reason": "R1: relays partially present but with invented (and partly inverted) bit patterns, no saved-config drive, and the entire cal-table-derived DAC1 trigger-comparator path (0x40007408 + 0x40001c34) is absent. Not R0 (some relay GPIO exists), not R2 (registers/values/order differ).",
  "verify_path": "1) Static-equiv: port FUN_080018A4/FUN_08001A58 verbatim as table-driven functions (10-case switch, exact GPIOC/E/A/B BSRR/BRR bit writes + the VFP DAC formula to 0x40007408/0x40001c34), feeding them mode/range from a restored config; diff register trace vs the decompile case-by-case. 2) Bench litmus: scope a stock boot vs ours on PC12/PE4/PE5/PE6/PA15/PB10/PD12/PD13 with the logic analyzer to confirm the real DCV idle pattern (resolves the PE5/PE6 inversion and PA10/PE4 questions empirically). 3) Read-back 0x40007408/0x40007404 on both stock and ours (usb_debug T5 already does this) to confirm stock sets a non-zero enabled trigger DAC and ours does not; then test if writing it changes whether 0x04/0x05 scope reads return data."
 },
 {
  "region": "i2c_touch (stock 0x08025250-0x08025270, master_init_phase4 Section A) \u2014 REIDENTIFIED as DAC channel-1 enable (scope trigger comparator DAC), NOT I2C/touch",
  "stock_summary": "The phase4 annotation labels 0x40007400 as \"I2C1 base / touch GT911\". This is a MISDECODE. On AT32F403A/GD32/STM32F1, I2C1 = 0x40005400; 0x40007400 = DAC peripheral base (DAC_CTRL at +0x00, DAC_DHR12R1/DAC1 12-bit right-aligned data at +0x08). Our own usb_debug.c:1620 already correctly labels 0x40007400 as DAC_CR and 0x40007408 as DAC1 data. Section A (0x08025250-0x08025270) is six read-modify-write ops to DAC_CTRL configuring DAC channel 1: i2c[0]|=0x38 -> TSEL1=0b111 (software trigger); |=0x04 -> TEN1=1 (trigger enable); &=~0xC0 -> WAVE1=00 (no noise/triangle wave gen); &=~0x02 -> BOFF1=0 (output buffer enabled); &=~0x1000 -> DMAEN1=0 (DMA disabled for ch1); |=0x01 -> EN1=1 (channel 1 enabled). Section B immediately follows (0x08025270): computes a 12-bit value via VFP from the scope gain/offset cal tables and writes it to 0x40007400+8 (DAC1 data, masked 0xFFF) then sets bit0 of +4. Per CLAUDE.md + function_names.md (FUN_080018A4 / gpio_mux_portc_porte, siggen_configure @0x08001c60), this DAC1 path at 0x40007408 is the SCOPE TRIGGER-LEVEL COMPARATOR DAC on PA4 \u2014 a static, software-triggered, non-DMA analog reference set once at boot. No I2C peripheral register, no touch controller, and no GT911/GT915 traffic appear anywhere in 0x08025250-0x08025270.",
  "decode_level": 3,
  "our_impl": "firmware/src/drivers/dac_output.c:75 (dac_output_init) + main.c:493 (called at boot). DAC1 also labeled in usb_debug.c:1620. No I2C/touch init: ABSENT by design (touch unused).",
  "reimpl_level": "R1",
  "reimpl_reason": "We DO bring up DAC1, but for a different role and with a register-divergent config: stock configures DAC1 as a static software-triggered, no-DMA, buffer-ENABLED, ENABLED trigger-level comparator; ours configures DAC1 as TMR6-TRGO + DMA-circular siggen output, buffer-DISABLED, started DISABLED. Same peripheral, opposite trigger source / DMA / enable state / output-buffer bit. Not register-faithful; the stock boot-time scope-trigger DAC value (Section B) is never computed or written by us. (The 'i2c_touch' label itself is wrong \u2014 there is no I2C bring-up to reimplement here, so the touch dimension is NA.)",
  "divergences": [
   {
    "what": "Peripheral identity of the region",
    "stock": "0x40007400 = DAC_CTRL; Section A enables DAC channel 1 (scope trigger comparator on PA4)",
    "ours": "We (and the phase4 annotation) must not treat this as I2C1/touch; our dac_output.c uses HAL DAC1 but for siggen",
    "severity": "low"
   },
   {
    "what": "DAC1 trigger source (TSEL1 / TEN1)",
    "stock": "TSEL1=0b111 software trigger, TEN1=1 \u2014 static value, manually loaded",
    "ours": "DAC_TMR6_TRGOUT_EVENT + DMA circular (dac_output.c:96-100) \u2014 continuous waveform",
    "severity": "med"
   },
   {
    "what": "DAC1 output buffer (BOFF1)",
    "stock": "&=~0x02 -> buffer ENABLED",
    "ours": "dac_output_buffer_enable(DAC1,FALSE) -> buffer DISABLED (dac_output.c:93)",
    "severity": "low"
   },
   {
    "what": "DAC1 enable state at boot",
    "stock": "EN1=1 \u2014 channel ON at boot with computed trigger-level value",
    "ours": "dac_enable(DAC1,FALSE) at init (dac_output.c:104); only enabled in siggen mode",
    "severity": "med"
   },
   {
    "what": "Boot-time trigger-comparator value (Section B)",
    "stock": "Computes 12-bit DAC value from scope cal tables via VFP and writes 0x40007408 at boot",
    "ours": "ABSENT \u2014 no scope-trigger DAC value computed or written at boot",
    "severity": "med"
   },
   {
    "what": "DMA on DAC1 (DMAEN1)",
    "stock": "DMAEN1=0 (no DMA for this static reference)",
    "ours": "dac_dma_enable(DAC1,TRUE) (dac_output.c:100)",
    "severity": "low"
   },
   {
    "what": "Touch/I2C bring-up",
    "stock": "None in this region (touch GT911/GT915 not initialized here)",
    "ours": "None (touch unused for input)",
    "severity": "low"
   }
  ],
  "fpga_early_boot_actions": [],
  "verify_path": "Static-equiv is already at V for the FPGA question (this region provably touches no SPI3/USART/FPGA/strap pin \u2014 it is DAC_CTRL + DAC1 data only; confirmed by cross-referencing our usb_debug.c:1618-1620 which decodes the identical addresses as DAC). To close the DAC-role divergence to V: (1) static \u2014 confirm in the binary that the only writes in 0x08025250-0x08025270 target 0x40007400/+4/+8 (DAC), and that no 0x40005400 (real I2C1) write exists anywhere in master_init (grep full_decompile.c for 0x40005400). (2) Bench litmus for the trigger-comparator role: on a stock boot, scope PA4 with a meter \u2014 expect a static DC reference voltage that tracks the trigger-level knob; on our firmware PA4 is high-Z until siggen mode. This divergence affects scope trigger accuracy, NOT FPGA config entry, so it is OUT OF SCOPE for the bitstream-acceptance mystery and can be deprioritized relative to the SPI3/strap regions. Also recommend fixing the phase4 source annotation: relabel Section A 'I2C1/touch' -> 'DAC1 channel-1 enable (scope trigger comparator)'."
 },
 {
  "region": "iwdg (phase2k_iwdg_init, stock 0x080275A0\u20130x080275D0, IWDG @ 0x40003000)",
  "stock_summary": "Stock IWDG init (FUN_08023A50 phase 2K, master_init_phase2.c:723-752). Exact register sequence on IWDG base 0x40003000:\n1. IWDG_KR (0x40003000) = 0x5555 \u2014 unlock write access to PR/RLR.\n2. IWDG_PR (0x40003004) = BFI(...,4,0,3) i.e. PR[2:0]=0x04 \u2014 prescaler /64. IWDG clock = LSI(~40kHz)/64 = ~625Hz.\n3. IWDG_RLR (0x40003008) = 0x04E1 = 1249 \u2014 reload. Timeout = 1249/625 \u2248 2.0s.\n4. IWDG_KR = 0xAAAA \u2014 reload counter (feed).\n5. IWDG_KR = 0xCCCC \u2014 start watchdog.\nNothing else in this region. Fed thereafter every 11 calls to input_and_housekeeping (~50ms cadence). Init occurs at the tail of master_init immediately before the scheduler launch (phase 2L). Fully decoded \u2014 the only non-literal write is the BFI on PR which resolves to PR[2:0]=4 (=DIV_64); all 5 writes and their order accounted for.",
  "decode_level": 3,
  "our_impl": "firmware/src/drivers/watchdog.c:36 watchdog_init(); called from firmware/src/main.c:590 (just before vTaskStartScheduler at main.c:593). Feed path: watchdog.c:56 watchdog_feed() called by health_check() (watchdog.c:99-122). HAL: at32f403a_lib/.../at32f403a_407_wdt.c (WDT base 0x40003000, register-identical to stock IWDG).",
  "reimpl_level": "R2",
  "reimpl_reason": "R2: identical register set, identical write values for the control sequence, identical ordering \u2014 via the AT32 HAL which expands to the same raw writes. The only numeric difference is the reload value (functional/timeout tuning, not a register-faithfulness defect).",
  "divergences": [
   {
    "what": "IWDG reload value (IWDG_RLR / WDT->rld)",
    "stock": "0x04E1 = 1249 \u2192 ~2.0s timeout at 625Hz",
    "ours": "1875 \u2192 ~3.0s timeout (watchdog.c:50, wdt_reload_value_set(1875))",
    "severity": "low"
   },
   {
    "what": "Feed cadence / mechanism",
    "stock": "unconditional feed every 11 input_and_housekeeping calls (~50ms), no health gating",
    "ours": "health-gated cooperative feed: health_check() runs ~500ms, feeds only if all registered tasks checked in within deadline (watchdog.c:99-122)",
    "severity": "low"
   },
   {
    "what": "Prescaler write style",
    "stock": "read-modify-write BFI into PR[2:0]=4",
    "ours": "wdt_divider_set(WDT_CLK_DIV_64) writes div bitfield (WDT->div_bit.div=0x04) \u2014 same effective value/register",
    "severity": "low"
   },
   {
    "what": "Unlock\u2192reload\u2192enable command ordering",
    "stock": "KR 0x5555 / PR / RLR / KR 0xAAAA / KR 0xCCCC",
    "ours": "cmd 0x5555 (write_enable) / div / rld / cmd 0xAAAA (counter_reload) / cmd 0xCCCC (enable) \u2014 same order, same opcodes",
    "severity": "low"
   }
  ],
  "fpga_early_boot_actions": [],
  "verify_path": "Static-equivalence is already sufficient to reach V for this region: (1) IWDG/WDT base is 0x40003000 on both; the AT32 HAL functions used (at32f403a_407_wdt.c) expand to WDT->cmd=0x5555, WDT->div=0x04, WDT->rld=N, WDT->cmd=0xAAAA, WDT->cmd=0xCCCC \u2014 byte-for-byte the stock sequence; (2) confirmed call-site ordering matches (init last, just before scheduler). Optional bench litmus: read RLR/PR via SWD after watchdog_init() to confirm PR[2:0]=4 and RLR=1875 (or change to 1249 if exact-timeout fidelity is desired). No FPGA-relevant assertion needed \u2014 this region has zero FPGA/SPI3/USART/timing interaction with the Gowin config path."
 }
]
```