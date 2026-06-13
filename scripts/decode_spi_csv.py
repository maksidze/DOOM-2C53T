#!/usr/bin/env python3
"""Decode a Saleae/sigrok transition-logged digital CSV of the 2C53T's
FPGA boot — SPI3 byte stream + USART/control-pin timeline.

This is the decoder that cracked the FPGA config-entry investigation from
maksidze's issue-#18 `digital.csv` capture (see
reverse_engineering/captures/CFW01_BYTE_DECODE_ANALYSIS.md). It expects an
8-channel transition CSV exported from Logic 2 (File -> Export Data -> CSV),
with this column order (header row required):

    Time [s], PB5 MOSI, PB6 CS, PB4 MISO, PB3 SCK, PA3 USART2_RX,
              PA2 USART2_TX, PC6 FPGA-SPI-enable, PB11 FPGA-active-mode

SPI is decoded as Mode 3 (CPOL=1, CPHA=1): sample MOSI/MISO on the SCK
RISING edge, MSB-first, 8 bits/byte, framed by CS (active LOW). The 0x3B
bitstream upload, the 0x3A close status, and the 0x03 scope-status read are
the bytes that matter — a successful config returns 0xF8 on the 0x3A close;
a wedged FPGA floats MISO high (0xFF) at every checkpoint.

Usage:
    python3 scripts/decode_spi_csv.py digital.csv
    python3 scripts/decode_spi_csv.py digital.csv --pin-stats
"""
import csv
import sys

# Column indices in the expected CSV layout.
T, MOSI, CS, MISO, SCK, RX, TX, PC6, PB11 = range(9)


def load(path):
    rows = []
    with open(path) as f:
        r = csv.reader(f)
        next(r)  # header
        for row in r:
            rows.append(tuple(
                float(row[0]) if i == 0 else int(row[i]) for i in range(9)
            ))
    return rows


def decode_spi(rows):
    """Yield CS-framed SPI transactions as dicts {t, mosi:[...], miso:[...]}."""
    frames = []
    cur = None
    prev_sck = 1
    prev_cs = 1
    nb = mbyte = sbyte = 0
    for r in rows:
        cs, sck = r[CS], r[SCK]
        if cs == 0 and prev_cs == 1:           # CS assert -> new frame
            cur = {'t': r[T], 'mo': [], 'mi': []}
            nb = mbyte = sbyte = 0
        if cs == 1 and prev_cs == 0 and cur is not None:  # CS deassert
            frames.append(cur)
            cur = None
        if cur is not None and cs == 0 and sck == 1 and prev_sck == 0:
            mbyte = ((mbyte << 1) | r[MOSI]) & 0xFF
            sbyte = ((sbyte << 1) | r[MISO]) & 0xFF
            nb += 1
            if nb == 8:
                cur['mo'].append(mbyte)
                cur['mi'].append(sbyte)
                nb = mbyte = sbyte = 0
        prev_sck, prev_cs = sck, cs
    return frames


def hexs(b, n=24):
    s = ' '.join('%02X' % x for x in b[:n])
    return s + (' ...' if len(b) > n else '')


def report(rows):
    t0 = rows[0][T]
    print(f"transitions: {len(rows)}   duration: {rows[-1][T]-t0:.3f}s")
    frames = decode_spi(rows)
    print(f"CS-framed SPI frames: {len(frames)}\n")
    for i, fr in enumerate(frames):
        mo, mi = fr['mo'], fr['mi']
        rt = (fr['t'] - t0) * 1000
        if len(mo) <= 24:
            print(f"[frame {i}] +{rt:.2f}ms  bytes={len(mo)}")
            print(f"  MOSI: {hexs(mo)}")
            print(f"  MISO: {hexs(mi)}")
        else:
            nonff = sum(1 for x in mi if x != 0xFF)
            print(f"[frame {i}] +{rt:.2f}ms  bytes={len(mo)} (BULK)")
            print(f"  MOSI head: {hexs(mo,12)}  tail: " +
                  ' '.join('%02X' % x for x in mo[-6:]))
            print(f"  MISO non-FF: {nonff}/{len(mi)}")
        print()


def pin_stats(rows):
    """Characterize PC6 toggling + USART activity (the config-state tells)."""
    t0 = rows[0][T]
    for idx, name in [(PC6, 'PC6'), (PB11, 'PB11')]:
        falls = [rows[i][T] for i in range(1, len(rows))
                 if rows[i][idx] == 0 and rows[i-1][idx] == 1]
        if len(falls) > 1:
            periods = [(falls[j+1]-falls[j])*1000 for j in range(len(falls)-1)]
            periods.sort()
            med = periods[len(periods)//2]
            print(f"{name}: {len(falls)} low-edges  "
                  f"median period {med:.2f}ms (~{1000/med:.1f} Hz)" if med else
                  f"{name}: {len(falls)} low-edges")
        else:
            print(f"{name}: {len(falls)} low-edges (steady)")
    for idx, name in [(TX, 'USART_TX'), (RX, 'USART_RX')]:
        edges = sum(1 for i in range(1, len(rows)) if rows[i][idx] != rows[i-1][idx])
        first = next(((rows[i][T]-t0)*1000 for i in range(1, len(rows))
                      if rows[i][idx] != rows[i-1][idx]), None)
        ft = f"  first +{first:.1f}ms" if first is not None else ""
        print(f"{name}: {edges} edges{ft}")


if __name__ == '__main__':
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    data = load(sys.argv[1])
    if '--pin-stats' in sys.argv:
        pin_stats(data)
    else:
        report(data)
