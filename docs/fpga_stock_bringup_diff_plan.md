# FPGA config-entry: rigorous stock-vs-ours bring-up diff — PLAN & STATE

**Created:** 2026-06-13
**Status:** Phase 1 multi-agent workflow LAUNCHED (see "Current State" below)
**Owner doc** — this is the pick-up-where-we-left-off roadmap. Findings land in
`reverse_engineering/analysis_v120/stock_vs_ours_bringup_diff.md` (written by the workflow).

## Why this exists (the methodology shift)

For months the config-entry investigation has been **empirical trial-and-error**:
"maybe pin X does Y," test on the bench, observe, move on. That found real things
(DMM pipeline, button map, the 0x4AD19 bitstream-offset bug) but never cracked the
wall. On 2026-06-13 a direct measurement settled the central question (see
[[fpga-config-entry-wall]] memory + `sibling_loader_config_diff.md`): sending
`0x11` IDCODE to the FPGA returns garbage, not `0x0120681B` — **the FPGA never
enters SSPI config-receive mode; it ignores every config opcode because its NV
meter design is running and owns the SSPI pins.**

The logical core (David, 2026-06-13): **stock firmware, on this exact hardware,
DOES configure the FPGA into scope mode.** So whatever stock does that we don't —
*if it is an MCU action at all* — is necessarily present in the stock binary and is
findable. We have the decompile, the device, the working sibling (`rosenrot00`),
the canonical loader (`openFPGALoader`), the Apicula netlist, and emulators.

**The damning gap (confirmed by recon 2026-06-13):** the MCU-side critical path is
already ~90–98% decoded to the register level — BUT a **register-by-register diff of
our firmware's bring-up against stock's exact instruction sequence has NEVER been
done.** Our firmware is a clean-room rewrite that was never verified identical to
stock. Recon already surfaced un-eliminated divergences (e.g. stock drives the
frontend relay bank + PB11 HIGH *before* the 0x3B upload; our firmware leaves those
floating — `stock_pre_fpga_gpio_state.md`). We poked these once and never reconciled.

## The goal (the disciplined version)

> Produce stock's complete, ordered, register-level action list from **reset vector →
> first scope sample**. Force our firmware to match it **exactly** — every peripheral
> write, value, ordering, and delay — eliminating **all** divergences, not
> cherry-picking suspects. Leave nothing on that path unexplained or unmatched.
> If the wall survives a *provably-identical* boot, that is rigorous **proof** it is
> hardware (→ JTAG), not a guess.

Scope boundary: peripherals/pins that touch the FPGA or could affect its pre-config
state — GPIO (all ports), CRM/clocks, SPI3, USART2, DMA, timers, AFIO/IOMUX. Exclude
LCD/UI/font/USB-MSC/JPEG/touch unless they incidentally drive an FPGA-relevant pin.

## Method — multi-agent workflow `stock-bringup-diff`

1. **Spine** — extract the ordered critical-path units (functions/config blocks) from
   `FPGA_BOOT_SEQUENCE.md` + `master_init_phase[1-4].c` + `reset_and_clock_init.md`.
2. **Stock decode** (fan-out per unit) — register-level action list for each stock unit.
3. **Our decode** (fan-out) — the equivalent in our firmware (HAL calls translated to
   register effects), or "absent."
4. **Diff** (fan-out) — per-unit divergence records, each rated config-relevance H/M/L.
5. **Command trace** — exact ordered FPGA command bytes (USART2 boot frames + SPI3
   prelude/upload/close/post-writes), stock vs ours.
6. **NV-release angle** — does the running NV design expose a command/pin to release
   SSPI or re-enter config (Gowin `0x3C` RELOAD, an Apicula-netlist release path)?
7. **Synthesis** — ranked divergence doc + concrete bench-test for each + next-session
   actions. Writes `analysis_v120/stock_vs_ours_bringup_diff.md`.

## Known leads to verify (pre-workflow — eliminate ALL, don't cherry-pick)

- **Frontend relay bank pre-upload:** stock drives PC12, PE4/5/6, PA15, PA10, PB10 to
  range-5 defaults via `gpio_mux_portc_porte(5)`+`gpio_mux_porta_portb(5)` (stock
  ~`0x0802C546`) BEFORE 0x3B. Ours leaves them floating. (Probably not config-entry —
  these are analog routing — but it's an un-eliminated divergence on the exact path.)
- **PB11 polarity/timing:** stock HIGH before upload; verify ours matches exactly.
- **NV-release command:** does stock send the running FPGA a command to relinquish
  SSPI / reconfigure? (Decompile command trace + Apicula netlist.) — strongest novel angle.

## The honest ceiling

The FPGA auto-loads its NV design in ~90µs, before the MCU runs. The differentiator
may be that stock does *something* to re-enter config we haven't found (→ the decode
finds it), OR it bottoms out at hardware (→ the decode *proves* it, forcing JTAG).
Either outcome is a win: the diff converts opinion into proof.

## Verification layer (deferred, ~1 week build)

Stand up the Unicorn/Renode harness to **execute** stock and log every peripheral
write on the bring-up path — ground-truth, zero interpretation. Catches anything the
static read misses. (Harness exists in `emulator/` but doesn't yet reach the SPI3 path.)

## How to bench-test a divergence (fast loop, no reflash)

The `fpga reinit` debug-shell command sweeps the config handshake live
(`scripts/serial_cmd.py "fpga reinit ..."`, device `/dev/cu.usbmodem0B3DC4EC4D621`).
Knobs: clock dividers, prelude framing, gaps, reset-pulse, strap-hold, trailing
clocks, and it now reports the Gowin `0x41` STATUS + `0x11` IDCODE discriminator. For
divergences not covered by a knob, add one (cheap) rather than reflashing per-test.

## Current State (UPDATE EACH SESSION)

- **2026-06-13 (plan created + workflow run COMPLETE):** Workflow `stock-bringup-diff`
  (run wf_14db1b9a-ecd, 70 agents) diffed **22 critical-path units — ALL 22 diverge
  from stock** (confirms our clean-room rewrite was never faithful). **8 HIGH-relevance
  divergences.** Full ranked report: `analysis_v120/stock_vs_ours_bringup_diff.md`.
  - ✅ **Gate cleared:** `fpga_cal_table.h` payload verified = 115638 B, sha `5a0e7338…`,
    Gowin sync + IDCODE present. The bitstream is correct; sweeps are meaningful.
  - 🔆 **HEADLINE LEAD (reopens the "JTAG-only" pessimism):** stock holds **USART2 UE
    CLEAR through the whole scope boot** (enables it only post-upload, only on a
    meter/no-save boot) and sends the `01/02/06/07/08` bytes as **SPI3 register WRITES
    AFTER** the upload. WE enable USART2 unconditionally in `fpga_init` (fpga.c:1772),
    always create/run `dvom_TX`/`dvom_RX`+`meter_poll`, and send those as **USART
    frames BEFORE** the upload (Step 3b) + extra meter frames (Step 8). Live USART2 on
    PA2/PA3 during the config window is the leading suspect for why the running NV
    design keeps owning the SSPI pins → our prelude MISO `0x80` (user mode) vs stock
    `0xFF` (config-wait).
  - ⚠️ **Today's "wall proven" IDCODE test was CONFOUNDED:** it was a *warm* `fpga
    reinit` (full boot, meter running, USART tasks active) = exactly the perturbed
    state the diff flags. A clean **USART-silent scope-boot build** is the real test —
    today's result does NOT cleanly prove the wall.
  - Other HIGH divergences: SPI3 clock **/2 (60MHz) vs stock /4 (30MHz)** [RE-ambiguous,
    confirm on scope]; our **0x41 diagnostic block injects CS-HIGH clocking** (added
    today — ironic SSPI-desync risk, gate it off for clean tests); the `0x00` flush +
    extra pre-`0x15` gap; PB11 LOW→HIGH **arming edge** timing; strap/reset sweeps (low
    expectation, all prior pins negative).
  - **PICK UP HERE → ranked next actions (from synthesis):**
    1. ✅ DONE — bitstream payload verified.
    2. **Build a USART-silent scope-boot:** hold USART2 UE clear, don't create/wake
       dvom tasks, drop Step 3b + Step 8 USART bursts. `fpga reinit`; watch first `0x05`
       prelude MISO `0x80`→`0xFF`. *(Note: Experiment 7 removed only the pre-upload
       burst and got 0xFF but still failed — the NEW variable is killing the ONGOING
       dvom/meter_poll task traffic + UE entirely, not just the burst.)*
    3. Clean the wire: disable the 0x41 diagnostic + 0x00 flush, no CS-HIGH clocking,
       collapse the pre-`0x15` gap. `fpga reinit`.
    4. SPI3 clock sweep: `fpga reinit upload_br=1 cmd_br=1` (/4=stock), then `cmd_br=3/5`;
       after each: `spi3 xfer 11 00 00 00 00` → want `cfg_status_reg` = `01 20 68 1B`.
    5. PB11 arming edge: drive PB11 LOW from boot, `fpga reinit arm_pb11=1`, scope-confirm.
    6. Strap/reset sweep (low expectation): strap_pd2/pd1213, trailing_clocks, reset pins.
    7. Cheap `0x3C` RELOAD probe: `spi3 xfer 3c 00` + `02 00` + read `0x03`.
    8. DECISIVE hardware: FT232H JTAG `--detect` → SRAM-load (`-m`, NEVER `-f`).
    9. Self-capture our wire (solder PB3/4/5/6+PC6+PB11) → diff vs `captures/SPI3_2C53T.sal`.
    10. True byte-parity (if 2–9 fail): implement frontend-relay-range5 + settings-load-cal
        so UE/PC11 become mode-dispatched like stock (resolve the objdump source conflicts first).
  - **Open coverage gaps to resolve for true byte-parity** (see report §gaps): SPI3
    prescaler /2-vs-/4 RE ambiguity; frontend range-5 PE4/5/6+PA15/PB10 source conflict
    (objdump 0x080088A4/0x08008A58 case-5); `settings-load-cal` (config_load never called,
    `flash_fs_load_factory_cal` is a stub) — this cascade is the *structural* reason our
    UE/PC11 are unconditional instead of mode-dispatched; FLASH_PSR wait-state write
    not found; first-acq CH2 read source (objdump 0x080375F8–40).
  - Diagnostic firmware (0x41 read + `tc` knob) + plan/diff docs still UNCOMMITTED.

- **2026-06-13 (bench, Unit 2, USART-silent build) — USART hypothesis NEGATIVE, but
  the wall is now PRECISELY located.** Built + flashed `guest-usart-silent`
  (`FPGA_USART_SILENT_SCOPE`: UEN never set, RX bytes=0 confirmed, no dvom/meter/acq
  tasks). Result: **silencing USART2 entirely changed NOTHING** — `fpga reinit` close
  byte + the free-running `C8 10 00 01` MISO stream are unchanged, `0x11` IDCODE still
  garbage at /2. **The "live USART keeps the NV design owning the pins" hypothesis is
  dead.** It also means today's earlier "wall proven" IDCODE result holds up under
  unconfounded conditions.
  - **🎯 THE PRECISE DIAGNOSIS (via `spi3 gowin`, slow /256 clock where reads are
    valid):** the SSPI read path is clock-limited — at /2 everything reads garbage
    (the `0x8090340D` IDCODE etc. were a CLOCK ARTIFACT, not config-state), but at /256:
    - IDCODE `0x0120681B` ✓ — **config controller is alive and responding.**
    - STATUS (0x41) = **`0x00039020`** = `MEMORY_ERASE | GOWIN_VLD | READY | POR | FLASH_LOCK`.
    - **NO `DONE_FINAL` (bit13), NO `CRC_ERROR` (bit0), NO `ID_VERIFY_FAILED` (bit2),
      NO `SYSTEM_EDIT_MODE` (bit7).** Status is identical before & after `fpga reinit`
      (our config sequence has ZERO effect), and identical at /4 and /64 handshake clock.
  - **What this means:** our bitstream is NOT rejected on content (no CRC/ID error); the
    bus works (IDCODE reads). The failure is that **`CONFIG_ENABLE` (0x15) never engages
    `SYSTEM_EDIT_MODE`** — the exact bit openFPGALoader's `enableCfg()` polls after
    CONFIG_ENABLE to confirm the device entered config-receive. Without edit mode, the
    0x3B payload is ignored (never CRC-checked → no error; never completes → no DONE).
    The wall is now a NAMED register signature, not inference: **the running-NV-design
    GW1N won't enter SSPI edit/config mode from MCU CONFIG_ENABLE.** (`FLASH_LOCK` set +
    `GWVLD` set are the likely reason — a valid, locked NV design refusing SSPI reconfig.)
  - **NEXT (surgical, cheap, mostly firmware):**
    1. Instrument the config sequence to **read STATUS at slow clock immediately after
       `0x15`** (and after `0x05` ERASE) — confirm SYSTEM_EDIT_MODE (bit7) does/doesn't
       set. This is the openFPGALoader-validated checkpoint we've never measured mid-sequence.
    2. **Test `0x3C` RELOAD at slow clock** (software reconfig trigger): send 0x3C, re-read
       STATUS — does it clear GWVLD/FLASH_LOCK and engage edit mode?
    3. If neither engages edit mode → the part genuinely won't SSPI-reconfig a running
       locked NV design from the MCU → **JTAG (RECONFIG-equivalent) is required**, now
       PROVEN via the SYSTEM_EDIT_MODE signature, not guessed.
  - USART-silent experiment (`FPGA_USART_SILENT_SCOPE` + Makefile `guest-usart-silent`)
    is UNCOMMITTED. Negative result — keep the flag for future clean-wire tests.

- **2026-06-13 (bench, probe build) — WALL PROVEN AT THE REGISTER LEVEL. CONFIG_ENABLE
  cannot engage edit mode.** Added `pe` (read STATUS@/256 right after `0x15`) and `rl`
  (send `0x3C` RELOAD before prelude) knobs to `fpga reinit`. Across all four conditions
  (`pe`, `pe k5`, `pe rl`, `pe rl k5`): **`post-0x15 STATUS = 0x00039020`,
  `SYSTEM_EDIT_MODE (bit7) = NEVER set`**, stable = `GWVLD | READY | FLASH_LOCK`.
  - **Definitive conclusion:** the config controller is alive (IDCODE reads at /256),
    the bitstream is fine (no CRC/ID error), but **`CONFIG_ENABLE` (0x15) provably cannot
    put this part into config-receive** — confirmed across clock (/2, /64), framing,
    USART-silence, AND the `0x3C` RELOAD software-reconfig trigger. The part is a valid,
    **FLASH_LOCK**'d, running NV design that refuses SSPI reconfig from the MCU. **This is
    the register-level PROOF (not inference) that JTAG / a hardware RECONFIG is required.**
  - **🔑 NEW concrete signature for the collaboration:** `FLASH_LOCK` (bit17) is set +
    `SYSTEM_EDIT_MODE` never engages. The killer maksidze/Apicula ask: **read STATUS (0x41
    @ /256) on a STOCK boot around its config** — does stock's part have FLASH_LOCK set,
    and does SYSTEM_EDIT_MODE engage on stock's CONFIG_ENABLE? If stock engages edit mode
    *with* FLASH_LOCK set → there's a software unlock/sequence we're missing. If stock's
    part is NOT locked at config time → something about its FPGA pre-config state differs
    (the recurring "stock awakens the NV design in the first 1.4s" thread).
  - **⛔ DO NOT** try to clear FLASH_LOCK from firmware — that risks writing/erasing the NV
    flash (the only copy of the meter design). SRAM-load over JTAG only.
  - Remaining firmware-reachable (speculative, low-priority): reproduce whatever stock does
    in the first ~1.4s that brings the FPGA NV design to its "announce-frame alive" state,
    THEN config. All other MCU-side SSPI levers are now exhausted with proof.
  - Probe knobs (`pe`/`rl` + `edit_mode_status`) UNCOMMITTED — good permanent diagnostics.

## Key references

- Decompile: `reverse_engineering/decompiled_2C53T_v2.c`, `analysis_v120/master_init_phase[1-4].c`,
  `analysis_v120/FPGA_BOOT_SEQUENCE.md`, `analysis_v120/reset_and_clock_init.md`
- Diffs/state: `analysis_v120/stock_pre_fpga_gpio_state.md`, `analysis_v120/sibling_loader_config_diff.md`
- Capture truth: `reverse_engineering/captures/SPI3_STOCK_BOOT_CAPTURE_ANALYSIS.md`
- Our firmware: `firmware/src/drivers/fpga.c`, `firmware/src/main.c`
- References: `/Users/david/Projects/RESEARCH/OpenScope-2C23T` (working sibling),
  `/Users/david/Projects/RESEARCH/openFPGALoader` (canonical Gowin loader),
  `/Users/david/Projects/RESEARCH/gw1n2-apicula` (netlist analysis)
- Memory: [[fpga-config-entry-wall]]
