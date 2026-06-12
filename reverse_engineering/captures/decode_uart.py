#!/usr/bin/env python3
"""Bit-bang decode the USART2 channels (9600 8N1) from the transition CSV."""
import csv

CSV = "export/digital.csv"
BIT = 1.0 / 9600.0

# build edge lists for ch RX (col 5) and TX (col 6); cols offset by 1 for time
for col, name in ((5, "PA3_RX"), (6, "PA4_TX")):
    edges = []  # (t, val)
    with open(CSV) as f:
        rdr = csv.reader(f)
        next(rdr)
        prev = None
        for row in rdr:
            v = int(row[col])
            if prev is None or v != prev:
                edges.append((float(row[0]), v))
                prev = v

    # reconstruct level at arbitrary time
    def level_at(t, edges=edges):
        lo, hi = 0, len(edges) - 1
        if t < edges[0][0]:
            return edges[0][1]
        while lo < hi:
            mid = (lo + hi + 1) // 2
            if edges[mid][0] <= t:
                lo = mid
            else:
                hi = mid - 1
        return edges[lo][1]

    # find start bits: falling edges when line was idle-high for >= half bit
    out = []
    i = 0
    t_busy_until = 0.0
    for j in range(1, len(edges)):
        t, v = edges[j]
        if v == 0 and t >= t_busy_until and level_at(t - 1e-7) == 1:
            # candidate start bit
            byte = 0
            for b in range(8):
                ts = t + BIT * (1.5 + b)
                byte |= level_at(ts) << b
            stop = level_at(t + BIT * 9.5)
            out.append((t, byte, stop))
            t_busy_until = t + BIT * 10
    print(f"=== {name}: {len(out)} bytes ===")
    run = []
    last_t = None
    for t, b, stop in out:
        if last_t is not None and t - last_t > 0.005:  # frame gap
            print(f"  {run_t0:.6f}: " + " ".join(f"{x:02x}" for x in run))
            run = []
        if not run:
            run_t0 = t
        run.append(b)
        last_t = t
    if run:
        print(f"  {run_t0:.6f}: " + " ".join(f"{x:02x}" for x in run))
