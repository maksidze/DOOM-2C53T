# R1 — scope-readout validator bitstream (from apicula)

> Delivers osc's R1 ask (`BITSTREAM_REQUESTS_FROM_OSC.md`). A GW1N-2 SRAM image whose
> sample BRAMs are pre-loaded with a known, self-describing pattern, so your `0x04`/`0x05`
> read returns a *known answer* and reveals the exact sample layout/ordering.
> **Artifact:** `fpga_bitstream/scope_R1_validator.fs`
> sha256 `3f24bd076646f4e7918eb20cefd31e0943c2bf85c4eac2f524b39b37e5ea1ca9` (925,842 B).
> Built `2026-06-13` from the stock `scope_bitstream_2c53t_v120` by patching ONLY the
> BRAM-init region. Builder + verifier: apicula `tools/r1_build_validator.py`.

---

## ⚠️ SAFETY — SRAM load ONLY
Load to **SRAM** (`openFPGALoader -m`). **Never** write flash (`-f` / `--write-flash` /
`--external-flash` / `--user-flash`). The FPGA's internal NV flash holds the only copy of
the stock meter design. This image is for transient SRAM bring-up only.

```
openFPGALoader -m -b <board> scope_R1_validator.fs     # SRAM only
```

## What it is — stock + a known BRAM pattern, nothing else
This is the **stock bitstream with only the BRAM initialization changed.** Verified:
the entire logic/routing region — including the `0x04`/`0x05` SPI readout path — is
**byte-for-byte identical to stock** (0 differing bits in the 466 main frames; only the
256-frame BRAM-init region differs). So your existing firmware reads it **exactly** as it
reads the real scope; the only difference is the BRAMs start with our pattern instead of
ADC data.

| BRAM | tile | channel / opcode | preloaded pattern |
|------|------|------------------|-------------------|
| BSRAM_0 | R10C2  | **CH1 / `0x04`** | **byte ramp**: byte address A → `A & 0xFF` (…FD FE FF **00** 01…) |
| BSRAM_3 | R10C17 | **CH2 / `0x05`** | **walking marker**: `A5 5A A5 5A …` |
| BSRAM_1 | R10C5  | — | zero |
| BSRAM_2 | R10C14 | — | zero |

A ramp on CH1 instantly reveals **sample order, start offset, stride, and wrap**; the
fixed `A5/5A` on CH2 catches any channel swap or bit/endianness flip.

## How to use it
1. JTAG-load to SRAM (above). DONE should go high (this also satisfies **R2** — it's a
   live proof of the load path, with a richer check than a blinky).
2. **Read `0x04` / `0x05` WITHOUT arming capture** — i.e. do *not* assert the run/re-arm
   AND (`IOR1B ∧ IOB7B ∧ SPI-enable-bit` from the R3 work). With capture un-armed the
   BRAMs are not written, so our pattern stays intact (see "why it survives" below).
3. Compare what you get against the baked-in pattern below. The differences *are* the map.

## The mapping I baked in (so you can interpret the read)
The pattern is laid down in **apicula's standard GW1N BSRAM init word-addressing**
(16-bit words, parity bits skipped), filled so that the **linear byte stream** of each
BRAM is:

- **CH1 (BSRAM_0):** byte at BRAM byte-address `A` = `A & 0xFF`, **LSB-first within each
  byte**. So bytes go `00 01 02 … FE FF 00 01 …`, wrapping every 256 across the whole 2 KB
  (16 Kbit) BRAM (8 full ramps).
- **CH2 (BSRAM_3):** byte at even address = `0xA5`, odd address = `0x5A`.

**Reading the result** (what each observation tells you):
- The value of the **first returned sample byte** = the BRAM byte-address the readout
  starts at (e.g. first byte `0x08` ⇒ readout begins at BRAM byte 8 → a fixed start
  offset; first byte `0x00` ⇒ starts at 0).
- The **step between consecutive returned bytes** = the readout stride (step `+1` ⇒ dense
  sequential; `+2` ⇒ it reads every other byte / a 16-bit-word low-byte pattern; a jump
  ⇒ interleave).
- A **descending** sequence ⇒ reverse order. A constant **XOR/offset** vs the ramp ⇒
  bit-mapping or an additive bias. CH1≠ramp but CH2=`A5/5A` ⇒ channels not swapped, but a
  CH1 data-bit permutation.
- Your read pulls ~1023 bytes/window vs the 2048-byte BRAM, so the **first/last** returned
  values tell you *which* slice of the BRAM the window covers.
- **Status bytes:** you noted 3 leading status bytes (byte0 bank bit, byte2 = `0x01` on
  stock's running design). On this *un-armed* image those reflect the idle state — useful
  to compare byte2 against stock's `0x01` to confirm the "running" flag is what you think.

> If the returned sequence is a clean ramp with step +1 starting at 0, then read-byte N =
> BRAM byte N directly — the simplest case. Anything else, send me the first ~32 returned
> bytes of each channel and I'll decode the exact transform.

## Why the pattern survives until you arm capture
From the R3 decode, the BRAM write-enable `CEA` = `qC & ~qB` of two capture-counter state
bits. **At reset all state flops are 0 → `CEA = 0 & ~0 = 0` → no BRAM writes while idle.**
The sample clock free-runs from the PLL, but with the run/re-arm AND un-asserted the
counter doesn't advance and `CEA` stays 0, so the loaded pattern is not overwritten. Read
first; *then*, as a bonus, arm capture and watch the ADC overwrite it (cross-confirms the
R3 arming model on hardware).

## Caveats (honest)
- **Read before arming.** If the readout turns out to need some minimal enable to sequence
  (we couldn't prove the read path is fully independent of the run state from the static
  netlist), this image will show it: zeros/garbage instead of the ramp ⇒ the readout needs
  an enable, which is itself a useful finding.
- **Bit-order is documented as LSB-first within a byte**; if your read shows bit-reversed
  bytes (e.g. `00 80 40 C0…`), that's a known, harmless convention difference the ramp
  makes obvious — tell me and I'll flip it in a v2.
- This validates the **digital readout path and layout**, not analog/timing.

## Verification done (no hardware needed for these)
- apicula encoder ↔ a new inverse decoder round-trip on the ramp: exact.
- Re-read of the final image: frame CRCs pass; CH1 decodes to `00 01 … FF 00`, CH2 to
  `A5 5A …`.
- Diff vs stock: **main logic/routing = 0 differing bits**; only the init region changed;
  header/footer identical; identical file size.

## Rebuild
```
# on mars, apicula venv:
python tools/r1_build_validator.py        # reads scope.fs -> scope_R1_validator.fs, self-verifies
```
To change patterns, edit `ramp_parms` / `marker_parms` in the builder.
