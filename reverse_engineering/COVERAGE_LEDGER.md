# Rigorous Coverage Ledger — stock firmware → understood + runnable + verified

**Created:** 2026-06-13
**Why:** `COVERAGE.md` reports "~99% understanding," but that counts functions *named*.
The 2026-06-13 stock-bringup-diff workflow proved all 22 critical-path units **diverge**
from our firmware — so "named" overstates "understood and runnable." This ledger measures
the real thing on a hard, non-gameable rubric, function-by-function, and produces one
headline % we drive toward 100%.

## The rubric (per function, 309 total)

Three dimensions, scored independently:

- **D — Decode** (do we understand exactly what it does?)
  - 0 none · 1 named/role only · 2 partial register-level · **3 full register-level
    reconstruction, nothing unexplained**
- **R — Reimplement** (is it in our runnable firmware?)
  - 0 absent · 1 partial · **2 complete in our firmware** · **NA = justified not-needed**
    (libc we replace, stock-only assets, dead code) with a one-line reason
- **V — Verify** (is our version provably equivalent to stock?)
  - 0 unverified · 1 static equivalence-reviewed · **2 bench- or emulation-confirmed
    equivalent** · NA (when R=NA)

**A function is RESOLVED when:** `D=3` AND ( (`R=2` AND `V≥1`) OR `R=NA` ).
i.e. we either faithfully reimplemented and verified it, or justified skipping it.

**Headline metric = RESOLVED / (meaningful functions).**

### Denominator (meaningful functions)

Of 309 real functions, some are legitimately **SKIP-justified** (`R=NA`) and count as
resolved once justified — we don't need to reimplement printf:

| Class | ~count | Disposition |
|---|---|---|
| C runtime (printf/math/memset/memcpy/strcmp) | 56 | NA — we use our own libc |
| JPEG/Huffman decoders (0x08030524, 0x08031f20) | 2 | NA — boot-logo assets |
| Font/display-render engine | ~9 | NA — our own font system |
| UI image/asset rendering | TBD | NA — our own UI |
| **Meaningful (hardware/protocol/logic)** | **~235** | must reach D=3 + R + V |

The ledger lists every function with its class so the denominator is explicit, not fudged.

## Baseline scoreboard (2026-06-13, pre-campaign)

| Metric | Soft (old) | **Rigorous (this ledger)** |
|---|---|---|
| Named / role known | ~97% | (D≥1) ~97% |
| Full register-level decode (D=3) | — | est. **~10–15%** (critical path + a few subsystems) |
| Reimplemented (R≥2) | — | est. **~20–30%** (our firmware's working features) |
| **Verified equivalent to stock (V≥2)** | — | **~0%** (22/22 critical units *diverged*) |
| **RESOLVED (headline)** | "~99%" | **est. low single digits** |

The gap between "~99% understood" and "~0% verified-equivalent" *is the project.* The
ledger makes every point of it a concrete, checkable line item.

## Relationship to the FPGA secret (honest)

The 2026-06-13 register-level proof (`CONFIG_ENABLE` can't engage `SYSTEM_EDIT_MODE`;
`FLASH_LOCK` set) strongly suggests the FPGA secret is **NOT a decodable MCU instruction
on the SSPI path** — we've exhausted that with proof. So driving this ledger to 100% is
primarily about a **complete, faithful, runnable custom firmware** (worth it on its own).

BUT one FPGA hypothesis remains firmware-reachable and is *directly targeted* by full
coverage: **stock brings its FPGA NV design to an "announce-frames-alive" state in the
first ~1.4s that ours never reaches.** If that's caused by an MCU action, a 100% faithful
decode of the early-boot path finds it. So the campaign serves both goals; we just don't
over-promise the secret falls out of it.

## Process (the steady push)

1. **Populate** — a workflow scores all 309 functions on D/R/V (batched), writing
   `coverage_ledger.csv` + per-function notes. Produces the real baseline headline %.
2. **Prioritize** — sort unresolved by (relevance × leverage). FPGA/clock/early-boot
   first (also chases the remaining hypothesis); assets/libc get fast NA justifications.
3. **Resolve in batches** — for each function: full register decode (D→3), reimplement or
   justify NA (R), then verify (V) via static equivalence or — best — an **emulation
   trace** of stock execution (the ground-truth layer; see plan doc). Update the ledger.
4. **Re-measure** — headline % ticks up each session. Stop at 100% OR when a decoded
   early-boot divergence yields a testable FPGA hypothesis.

## Files

- `coverage_ledger.csv` — the per-function ledger (created by the population workflow).
- Source inventory: `analysis_v120/function_map_complete.txt` (309 addr/size/callers),
  `analysis_v120/function_names.md` (names + confidence), `COVERAGE.md` (soft tracker).
- Verification engine (deferred build): Unicorn/Renode stock-execution trace, see
  `docs/fpga_stock_bringup_diff_plan.md`.
