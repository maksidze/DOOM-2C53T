#!/usr/bin/env python3
"""Analyze maksidze's issue-#18 Saleae capture of a stock 2C53T boot (SPI3 /64 patch).

Streams the exported digital.csv (transition format) and produces:
  - control-line timeline (CS / PC6 / PB11 edges, UART first-activity)
  - per-CS-window SPI decode (mode 3, MSB first): byte counts + MOSI/MISO content
  - MISO payload statistics for the bulk window (does the FPGA drive MISO?)
  - raw MOSI/MISO byte dumps per window for offline diffing
"""
import csv
import sys
from collections import Counter

CSV = "export/digital.csv"
OUT_DIR = "export"

# column indexes (after time)
MOSI, CS, MISO, SCK, RX, TX, PC6, PB11 = range(8)
NAMES = ["MOSI", "CS", "MISO", "SCK", "RX", "TX", "PC6", "PB11"]


def main():
    f = open(CSV)
    rdr = csv.reader(f)
    next(rdr)  # header

    prev = None
    t = 0.0

    timeline = []          # (t, "desc") control events
    windows = []           # per CS-low window dicts
    cur = None             # active window state

    bit_mosi = 0
    bit_miso = 0
    nbits = 0

    uart_first = {RX: None, TX: None}
    uart_edges = {RX: 0, TX: 0}

    for row in rdr:
        t = float(row[0])
        vals = [int(x) for x in row[1:]]
        if prev is None:
            prev = vals
            timeline.append((t, "START state " + ",".join(
                f"{NAMES[i]}={vals[i]}" for i in range(8))))
            continue

        for ch in (RX, TX):
            if vals[ch] != prev[ch]:
                uart_edges[ch] += 1
                if uart_first[ch] is None:
                    uart_first[ch] = t

        if vals[PC6] != prev[PC6]:
            timeline.append((t, f"PC6 -> {vals[PC6]}"))
        if vals[PB11] != prev[PB11]:
            timeline.append((t, f"PB11 -> {vals[PB11]}"))

        if vals[CS] != prev[CS]:
            if vals[CS] == 0:
                cur = {"t0": t, "mosi": bytearray(), "miso": bytearray(),
                       "clocks": 0}
                nbits = bit_mosi = bit_miso = 0
            else:
                if cur is not None:
                    cur["t1"] = t
                    if nbits:
                        cur["partial_bits"] = nbits
                    windows.append(cur)
                    cur = None

        # SPI mode 3: sample on rising SCK edge while CS low
        if cur is not None and vals[SCK] == 1 and prev[SCK] == 0:
            cur["clocks"] += 1
            bit_mosi = (bit_mosi << 1) | vals[MOSI]
            bit_miso = (bit_miso << 1) | vals[MISO]
            nbits += 1
            if nbits == 8:
                cur["mosi"].append(bit_mosi)
                cur["miso"].append(bit_miso)
                nbits = bit_mosi = bit_miso = 0

        prev = vals

    if cur is not None:  # capture ended mid-window
        cur["t1"] = t
        cur["open_at_end"] = True
        windows.append(cur)

    print("=== CONTROL TIMELINE ===")
    for tt, d in timeline:
        print(f"{tt:12.6f}  {d}")
    for ch in (RX, TX):
        print(f"UART {NAMES[ch]}: first edge {uart_first[ch]}, total edges {uart_edges[ch]}")

    print(f"\n=== SPI WINDOWS (CS low) : {len(windows)} ===")
    for i, w in enumerate(windows):
        n = len(w["mosi"])
        mosi_head = w["mosi"][:8].hex(" ")
        miso_head = w["miso"][:8].hex(" ")
        extra = ""
        if n > 8:
            extra = f" ... mosi_tail={w['mosi'][-4:].hex(' ')} miso_tail={w['miso'][-4:].hex(' ')}"
        flags = []
        if w.get("partial_bits"):
            flags.append(f"partial={w['partial_bits']}bits")
        if w.get("open_at_end"):
            flags.append("OPEN_AT_END")
        print(f"[{i:3d}] t={w['t0']:.6f}..{w['t1']:.6f} dur={w['t1']-w['t0']*1.0:.6f}s "
              f"bytes={n} clocks={w['clocks']} {' '.join(flags)}")
        print(f"      MOSI: {mosi_head}{extra and ''}")
        print(f"      MISO: {miso_head}{extra}")

        if n > 1000:  # bulk window: MISO stats + dumps
            mosi = bytes(w["mosi"])
            miso = bytes(w["miso"])
            with open(f"{OUT_DIR}/win{i}_mosi.bin", "wb") as fo:
                fo.write(mosi)
            with open(f"{OUT_DIR}/win{i}_miso.bin", "wb") as fo:
                fo.write(miso)
            c = Counter(miso)
            top = c.most_common(6)
            non_ff = n - c.get(0xFF, 0)
            non_00 = n - c.get(0x00, 0)
            print(f"      BULK: MISO non-FF={non_ff}/{n}  non-00={non_00}/{n}  top={[(hex(b), k) for b, k in top]}")
            print(f"      BULK: dumped to win{i}_mosi.bin / win{i}_miso.bin")


if __name__ == "__main__":
    main()
