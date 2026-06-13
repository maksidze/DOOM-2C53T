# USART2 Boot Command Frames — Exact Reconstruction (V1.2.0)

**Date:** 2026-06-12
**Binary:** `archive/2C53T Firmware V1.2.0/APP_2C53T_V1.2.0_251015.bin` (raw image, link base 0x08007000)
**Method:** arm-none-eabi-objdump hand-verified disassembly + Keil `__decompress1` .data-image
reconstruction. Every claim below is anchored to a file offset; nothing is taken from the
prior Ghidra paraphrase.

---

## TL;DR — the headline result

**The stock firmware transmits ZERO USART2 frames during boot.** There is no "USART boot
command sequence" in master init at all. The famous "commands 0x01, 0x02, 0x06, 0x07, 0x08"
are real and in exactly that order — but they are **1-byte FreeRTOS queue items posted to the
FPGA task's SPI3 trigger queue (handle at 0x20002D78)**, not USART frames. They are consumed
by `fpga_task` only after the scheduler starts and become **SPI3 transactions** — they are the
five SPI3 config writes (`01 08`, `02 03`, `06 00`, `07 00`, `08 AD`) seen ~600 ms after the
bitstream upload in the issue-#18 Saleae capture.

This is why the capture shows **no `AA 55` echo frames** before (or during) the upload: the
FPGA echoes every USART command it receives, and stock never sends one. The `5A A5` (12-byte)
and `5A 69` (11-byte) frames at t=2.77–3.59 s are **unsolicited FPGA output** — and stock
cannot even receive them, because **USART2's UE bit is held clear through all of master init**
(see §5). Stock neither waits for, nor reads, nor reacts to any USART RX before the SPI3
upload.

Our firmware's replay of "0x01/0x02/0x06/0x07/0x08 as USART frames via the runtime frame
builder" was replaying a misreading: those bytes were never meant for the USART. (They *do*
elicit `AA 55` echoes from the FPGA because the FPGA echoes any USART command frame — that's
the behavioral proof that stock's wire silence means stock sent nothing.)

---

## 0. Why the target region was wrong: the address-convention bug (again)

The task region "flash 0x08025D96–0x08026540 = file 0x1ED96–0x1F540" contains scope
roll-buffer and FreeRTOS stream code — no USART access. Cause: **the same 0x7000 confusion
that broke the bitstream extraction, in the other direction.** The Ghidra database for V1.2.0
was loaded at base **0x08000000**, so every "address" in `master_init_phase*.c`,
`FPGA_BOOT_SEQUENCE.md`, `function_names.md`, `decompiled_2C53T_v2.c` etc. is really a **file
offset + 0x08000000**. True flash = file offset + 0x08007000.

Proof: `usart2_isr_real @ 0x080277b4` (Ghidra) disassembles correctly at **file offset
0x277b4** (the `cmp #0x5A` / 0x20004E10 buffer code is there), not at file 0x207b4.
Cross-check: the task entry pointers in `FPGA_BOOT_SEQUENCE.md` step 12 (read from *runtime
pointer constants*, hence true flash) line up exactly: `dvom_TX @ 0x0803E3F5` = file 0x373F4
+ 0x8007000 + thumb bit; `fpga @ 0x0803E455` = file 0x37454. Same function, two conventions.

| Item | Ghidra-land / docs | file offset | TRUE flash |
|---|---|---|---|
| master init `FUN_08023A50` | 0x08023A50 | 0x23A50 | 0x0802AA50 |
| USART2 peripheral config | — | 0x258D2 | 0x0802C8D2 |
| Queue/semaphore creation | — | 0x25BFA | 0x0802CBFA |
| Flash-settings → meter_state | "0x08025D90" | 0x25D92 | 0x0802CD92 |
| SPI3 init | "0x08026540" | 0x2650A | 0x0802D50A |
| SPI3 prelude `05` | — | 0x267D2 | 0x0802D7D2 |
| SPI3 prelude `12` | — | 0x2690A | 0x0802D90A |
| SPI3 prelude `15` | — | 0x26A42 | 0x0802DA42 |
| `0x3B` open | — | 0x26B06 | 0x0802DB06 |
| Bitstream upload loop head | "0x08026B28" | 0x26B28 | 0x0802DB28 |
| `0x3A` close | — | 0x26C96 | 0x0802DC96 |
| **Five queue posts (the "boot cmds")** | "0x08025D96 USART cmds" | **0x26DC8–0x26E2D** | **0x0802DDC8** |
| Mode dispatch on saved mode | — | 0x26F50 | 0x0802DF50 |
| USART2 **UE set** (meter path) | — | 0x26F8E | 0x0802DF8E |
| USART2 **UE cleared** (non-meter) | — | 0x2700A | 0x0802E00A |
| `dvom_tx_task` | 0x080373F4 | 0x373F4 | 0x0803E3F4 |
| `fpga_task` | 0x08037454 | 0x37454 | 0x0803E454 |
| `usart2_isr_real` | 0x080277B4 | 0x277B4 | 0x0802E7B4 |

So `master_init_phase2.c` lines 403–426 ("USART TX command sequence 0x08025D96–0x08026540,
10-byte frames, TXE polling") describes code that **does not exist**. File 0x25D96–0x26506 is
the settings-load + default-cal-table populate; 0x2650A starts SPI3 init directly. There is no
USART2_STS poll / USART2_DT write loop anywhere in master init.

---

## 1. The five "boot commands": exact instructions

At file 0x266DE (during the SPI3 phase): `movw r9,#0x2D78; movt r9,#0x2000` — r9 =
**&queue handle 0x20002D78** and is never reassigned until after the posts (verified by
register-write scan 0x266F0–0x26DC8).

Immediately after the `0x3A` close and final CS-framed `00` byte (file 0x26DC8):

```
26dc8: ldr.w  r0, [r9]          ; queue handle from 0x20002D78
26dce: movs   r5, #1
26dd6: strb.w r5, [sp,#0x60]    ; item byte = 0x01
26dda: bl     0x3acf0           ; xQueueSend(q, &item, portMAX_DELAY)
26dde: movs   r1, #2  ... bl 0x3acf0   ; item = 0x02
26df2: movs   r1, #6  ... bl 0x3acf0   ; item = 0x06
26e06: movs   r1, #7  ... bl 0x3acf0   ; item = 0x07
26e1a: movs   r1, #8  ... bl 0x3acf0   ; item = 0x08
```

- Queue 0x20002D78 is created at file 0x25C46 with `xQueueCreate(15, 1)` — **15 items × 1
  byte**. (Creation order/params, file 0x25BFA–0x25C84: 0x2D6C=20×1, 0x2D70=15×1,
  0x2D74=10×2, 0x2D78=15×1, then three binary semaphores 0x2D7C/0x2D80/0x2D84.)
- **`fpga_task` (file 0x37454) is the reader of 0x2D78** (`movw r4,#0x2d78` in its prologue),
  NOT `dvom_tx_task` (which reads 0x2D74). So these bytes are the fpga_task **trigger codes**
  (`trigger_byte`, dispatched as `switch(trigger_byte - 1)` — see
  `fpga_task_annotated.c` cases 0–8).
- **No delays between the five posts** — back-to-back `xQueueSend`. The scheduler isn't
  running yet; the items just sit in the queue until `vTaskStartScheduler`. The ~600 ms gap
  to the capture's five SPI3 config writes is scheduler start + fpga_task wakeup, not a coded
  delay.
- **No conditionals** — all five are posted on every boot, regardless of saved mode. (The
  mode conditional comes *after*, §5.)

### Wire correlation (issue-#18 capture)

| queued code | fpga_task case | SPI3 bytes in capture | param source |
|---|---|---|---|
| 0x01 | case 0 (fast timebase cfg) | `01 08` | ms[0x2D] timebase idx (8 on captured unit; .data default 0x0D) |
| 0x02 | case 1 (roll mode cfg) | `02 03` | roll-mode param |
| 0x06 | case 5 (meter ADC) | `06 00` | ms[0x16] — .data default **0x00** ✓ |
| 0x07 | case 6 (siggen feedback) | `07 00` | ms[0x18] — .data default **0x00** ✓ |
| 0x08 | case 7 (status readback) | `08 AD` then status read `03` → `00 01 42 2E 2E` | — |

The second bytes of codes 6/7 matching the decompressed .data defaults for ms[0x16]/ms[0x18]
independently corroborates the mapping.

---

## 2. The real (runtime) USART2 TX frame — exact bytes

USART2 TX happens in exactly one place in the entire firmware: `dvom_tx_task` (file 0x373F4)
+ `usart2_isr_real` (file 0x277B4). Hand-decoded `dvom_tx_task`:

```
373f6: movw r5,#0x440c / movt #0x4000   ; r5 = USART2_CR1 (0x4000440C)
373fa: movw r6,#0x2d74 / movt #0x2000   ; r6 = &usart_tx queue handle (10 items × 2 bytes)
373fe: movw r7,#5      / movt #0x2000   ; r7 = 0x20000005 = tx_buffer base
37402: movw r8,#15     / movt #0x2000   ; r8 = 0x2000000F = usart2_tx_byte_index
37420: xQueueReceive(q2D74, sp+6, portMAX_DELAY)      ; 2-byte item
37430: ldrh r0,[sp,#6]                  ; r0 = item (lo = byte0, hi = byte1)
37434: strb r9(=0),[r8]                 ; tx index = 0
37438: lsrs r1,r0,#8
3743a: strb r0,[r7,#3]                  ; tx_buffer[3] = item.lo  = cmd  (0x20000008)
3743c: add  r0,r0,r0 lsr #8             ; r0 = item + (item>>8)
37440: strb r1,[r7,#2]                  ; tx_buffer[2] = item.hi  = param (0x20000007)
37442: strb r0,[r7,#9]                  ; tx_buffer[9] = (lo+hi) & 0xFF = CHECKSUM
37444: CR1 |= 0x80                      ; TXEIE → ISR clocks out 10 bytes from 0x20000005
3744c: vTaskDelay(10)
```

Bytes [0], [1], [4..8] are **never written at runtime**. They come from the Keil scatter-load
.data image (entry at file 0xB7314: src flash 0x080BE480, dst 0x20000000, len 0x1240, handler
`__decompress1` at flash 0x080071C0). Decompressing it gives RAM 0x20000000..0x1F:

```
aa aa aa aa | 64 | AA 55 05 00 00 00 00 00 00 00 | 0a 40 ...
              ^    \________ tx_buffer[0..9] ________/
            0x04   0x05                          0x0E
```

**Therefore every stock USART2 TX frame is:**

```
[0]=0xAA  [1]=0x55  [2]=param(hi)  [3]=cmd(lo)  [4..8]=0x00  [9]=(cmd+param)&0xFF
```

- **Header: `AA 55`** — fixed, baked into .data, never touched. (Resolves the "[0],[1]
  unknown — may be 0x00 from BSS" guess in `FPGA_BOOT_SEQUENCE.md`; it is NOT BSS, it's
  initialized .data.)
- **Checksum: plain 8-bit sum of bytes [2] and [3] only** (`(cmd + param) & 0xFF`). The
  header and zero bytes are not included. Matches the FPGA's echo-frame integrity scheme
  (echo byte[3] = cmd, byte[7] = 0xAA).
- [2] has .data initial value 0x05 but is overwritten on the first send, so it never appears
  on the wire as 0x05 unless a (0x05, x) command is sent.
- The ISR transmits all 10 bytes from 0x20000005 on TXE, then clears TXEIE at index 10.

### How our firmware differs (firmware/src/drivers/fpga.c)

| | stock | ours (`usart2_send_cmd` / `fpga_usart_tx_task`) |
|---|---|---|
| byte[0..1] | **0xAA 0x55** | **0x00 0x00** (`frame[10]={0}` / `memset`) |
| byte[2],[3] | param, cmd | same ✓ |
| byte[4..8] | 0x00 | 0x00 ✓ |
| byte[9] | (cmd+param)&0xFF | same ✓ |

The fpga.c comment "stock frame builder does NOT set bytes[4-8] — 0 from BSS" is half right:
[4..8] are indeed 0, but **[0..1] are AA 55 from .data, and our frames are missing them.**
The FPGA evidently tolerates this (meter pipeline works, echoes arrive), but it is a real
wire-format divergence to fix if we ever chase protocol edge cases.

The bigger fix: `fpga_send_boot_commands()` (fpga.c:1931–1939) sends 0x01/02/06/07/08 as
USART frames. **Stock never does this.** Stock's equivalents are fpga_task SPI3 operations
(post codes 1,2,6,7,8 to the SPI3-trigger path), i.e. the five SPI3 config writes already
specified in `SPI3_STOCK_BOOT_CAPTURE_ANALYSIS.md` / fpga.c Step 7c.

---

## 3. Inter-frame delays

- **USART frames:** the only pacing is `vTaskDelay(10)` (10 ms) in `dvom_tx_task` after
  arming TXEIE — i.e. ≥10 ms between any two USART frames, queue-driven. At boot: zero
  frames, so no delays to speak of.
- **The five queue posts:** back-to-back, no delays (§1).
- **SPI3 phase (for completeness, verified at instruction level here):**
  - one-shot SysTick delay LOAD=[0x20002B1C]×10 (file 0x26644) after SPI3 enable;
  - CS↑ + `00`, then **100 ms** (chunked SysTick loop, 100 units of [0x20002B20] ticks,
    ≤50 per chunk, file 0x266EE);
  - CS↓ `05 00` CS↑ `00`, **100 ms** (file 0x26874);
  - CS↓ `12 00` CS↑ `00`, **100 ms** (file 0x269AC);
  - CS↓ `15 00` CS↑ `00`;
  - CS↓ `3B` + 115,638 bytes from flash 0x08051D19 (loop file 0x26B28, count 0x1C3B6,
    embedded task-name strings at file 0x26B3C–0x26B73 jumped by `b.n 0x26b7c`);
  - CS↑ `00`, CS↓ `3A 00`, CS↑ `00`, CS↓ `00`, CS↑, `00`.
  This matches fpga.c's current Step 7 sequence byte-for-byte.

---

## 4. Conditionals — the only mode-dependent boot behavior (file 0x26F50)

After the five posts + GPIO/TMR3 setup, master init dispatches on **state[0xF64]** (saved
last-active mode, loaded from SPI flash at 0x25E48):

- **mode 0 (no valid save) or 1 (meter):**
  - file 0x26F8E: **USART2_CR1 |= 0x2000 (UE) — USART2 is enabled here and only here**;
  - `vTaskResume` of task handles at 0x20002DA0 (dvom_TX) and 0x20002DA4 (dvom_RX);
  - GPIOC BSRR = 0x800 → **PC11 HIGH** (meter MUX on);
  - meter vars seeded (0x7FC00000 = 4-byte float NaN-ish sentinels at state[0xF48..0xF50],
    state[0xF35..]=0x0101, state[0xF38]=0xFF).
- **mode 2 (scope):** sets 0x20002D50 halfword (0 or 0x3C00 from state[0x354]) and falls
  into the disable path.
- **mode 3 (siggen):** clamps state[0xF6A] from state[0xE59] and falls into the disable path.
- **disable path** (file 0x2700A): **USART2_CR1 &= ~0x2000 (UE off)**; `vTaskSuspend` of
  dvom_TX/dvom_RX; GPIOC BRR = 0x800 → PC11 LOW; `xSemaphoreGive(0x2D7C)`;
  queue-reset/unblock call on usart_tx queue 0x2D74.

So **even the USART2 peripheral itself is only enabled at boot if the device last ran in
meter mode** (or has no saved settings). In scope/siggen boots, USART2 stays dead until a
mode switch enables UE (the runtime UE set/clear sites are the meter mode-switch functions
at file 0x4DB6/0x7366 (set) and 0x6874/0x741A/0x752A (clear) — all verified to be bare
UE bit flips, no TX).

---

## 5. RX waits: none

- USART2 is configured at file 0x258D2–0x2593A: BRR computed from PCLK1 for **9600 baud**,
  CR1 M=0 (8 data bits), CR2 stop=00 (1 stop), **TE | RE | RXNEIE set, UE explicitly cleared**
  (bic 0x2000 at 0x25932). NVIC IRQ38 (USART2) is enabled early (ISER1 bit6, file 0x25890).
- Because **UE = 0 from reset until file 0x26F8E/0x2700A (after the bitstream upload)**, the
  MCU physically cannot receive the FPGA's `5A A5` / `5A 69` frames at t=2.77–3.59 s. They
  are unsolicited NV-design output into a disabled peripheral. Master init contains **no
  USART2_SR/DR reads, no RX polling, no semaphore waits on RX** anywhere between reset and
  the SPI3 upload. Stock does NOT gate the upload on any USART response.
- Note the stock RX ISR has no `5A 69` state at all (valid header combos are only
  `5A A5` and `AA 55` — `usart2_isr_state_machine.md`); even at runtime, `5A 69` frames are
  silently dropped. They are most plausibly an FPGA boot banner / secondary status stream
  from the NV design.

---

## 6. What this corrects in earlier docs

1. **`master_init_phase2.c` 403–426** — "USART TX command sequence (0x08025D96–0x08026540),
   10-byte frames byte-by-byte with TXE polling" is **fiction**. That region is settings
   populate + defaults; the real artifact is five 1-byte queue posts at file 0x26DC8 to the
   **fpga_task SPI3 queue**, after the upload, not before.
2. **`FPGA_BOOT_SEQUENCE.md` Phase 4** — same correction; also its "TX buffer [0],[1]
   unknown, maybe 0x00 BSS" → resolved: **0xAA 0x55 from .data**, and the phase ordering is
   wrong (queue posts come *after* Phase 5's SPI3 work, and they aren't USART).
3. **fpga.c Step 7d / `fpga_send_boot_commands`** — sending 0x01/02/06/07/08 over USART has
   no stock counterpart; stock realizes those codes as SPI3 transactions via fpga_task. The
   capture's "five SPI3 config writes" and the "boot command sequence" folklore are the same
   five bytes — there is only one sequence, and it's SPI3.
4. **fpga.c frame builder** — missing the `AA 55` header bytes ([0..1]=0). Cosmetically
   tolerated by the FPGA so far, but stock always sends them.
5. All "flash addresses" in the v120 analysis corpus derived from the Ghidra DB are file
   offsets; add 0x7000 for true flash. (Second instance of this bug class after the
   bitstream-extraction fix of 2026-06-12.)

---

## Appendix: evidence trail

- ISR base-register: file 0x277B8 `movw r4,#0x440C / movt #0x4000`; DR accessed as
  `[r4,#-8]` (0x40004404), SR as `[r4,#-12]` (0x40004400). Explains why no
  `0x40004400/4404` movw/movt or literal exists in the image — all USART2 register access
  goes through CR1-anchored negative offsets (sites: files 0x4DB6, 0x6874, 0x7366, 0x741A,
  0x752A, 0x2566E→0x26F8E/0x2700A, 0x277B8, 0x373F6 — exhaustive movw#0x440C scan).
- Settings magic check at file 0x25D92: first flash byte must be 0x55 or 0xAA
  (0xAA additionally forces state[0xF68]=8).
- Queue item ordering proof: `dvom_tx_task` `strb r0,[r7,#3]` (lo→cmd@0x20000008) /
  `strb r1,[r7,#2]` (hi→param@0x20000007) / `strb (lo+hi),[r7,#9]` (ck@0x2000000E).
- .data image: scatter entry @file 0xB7314 `{0x080BE480, 0x20000000, 0x1240, 0x080071C0}`;
  handler disassembles as Keil `__decompress1` (LZ + zero-run); decompressed bytes
  0x05..0x0E = `AA 55 05 00 00 00 00 00 00 00`.
- meter_state .data defaults: ms[0x14]=0x03, ms[0x16]=0x00, ms[0x17]=0x01, ms[0x18]=0x00,
  ms[0x2D]=0x0D (RAM 0x2000010C/0x10E/0x10F/0x110/0x125).
- fpga_task reads queue 0x2D78: file 0x3745A `movw r4,#0x2d78`.
