#!/usr/bin/env python3
"""
make_fs.py — convert the raw Gowin config binary to a `.fs` ASCII bitfile.

WHY THIS EXISTS
---------------
openFPGALoader auto-detects file type by extension and natively parses Gowin
`.fs` files (ASCII '0'/'1' bitstreams). Our scope bitstream was extracted from
the stock firmware as a *raw binary* config stream (`scope_bitstream_2c53t_v120.bin`,
byte-verified against a Saleae MOSI capture of a stock boot). This script wraps
that binary into a `.fs` so openFPGALoader can load it without `--file-type bin`.

PRIMARY PLAN IS STILL THE RAW BIN. At the bench, prefer:
    openFPGALoader -c ft232 --detect                                   # prove the JTAG chain first
    openFPGALoader -c ft232 -m --file-type bin scope_bitstream_2c53t_v120.bin   # load raw binary to SRAM

The raw `.bin` is the exact on-wire byte stream and needs no format assumption.
This `.fs` is a FALLBACK for if `--file-type bin` is rejected by your
openFPGALoader build.

⚠️  UNVERIFIED UNTIL BENCH: this `.fs` is hand-built. The bit ORDER assumption is
that each config byte shifts MSB-first (matching how the SPI3/JTAG bitstream is
clocked and how our `.bin` was verified). If openFPGALoader loads this `.fs` and
`--read-register STATUS` shows an IDCODE-mismatch / CRC error that the raw `.bin`
does NOT show, the bit order is wrong — flip MSB/LSB below and regenerate. A real
GowinEDA-produced `.fs` is the gold standard if this fails.

USAGE
-----
    python3 make_fs.py scope_bitstream_2c53t_v120.bin scope_bitstream_2c53t_v120.fs
"""
import sys

LINE_BITS = 64  # openFPGALoader ignores newlines; width is cosmetic.


def main():
    if len(sys.argv) != 3:
        sys.exit(f"usage: {sys.argv[0]} <in.bin> <out.fs>")
    src, dst = sys.argv[1], sys.argv[2]
    with open(src, "rb") as f:
        data = f.read()

    # Sanity: confirm the Gowin sync word + IDCODE are where we expect (preamble
    # is FF padding then A5 C3 ...; IDCODE 0x0120681B for the GW1N-2 family).
    idcode_off = data.find(bytes.fromhex("0120681b"))
    note = (f"IDCODE 0x0120681B found at byte {idcode_off}"
            if idcode_off >= 0 else "WARNING: IDCODE 0x0120681B NOT FOUND")

    bits = []
    for byte in data:
        bits.append(format(byte, "08b"))  # MSB-first
    bitstream = "".join(bits)

    with open(dst, "w") as f:
        f.write("//Gowin .fs bitfile (hand-built from raw binary by make_fs.py)\n")
        f.write("//Part: GW1N-UV2 (GW1N-2 family)  IDCODE 0x0120681B\n")
        f.write(f"//Source: {src}  ({len(data)} bytes)\n")
        f.write(f"//Bit order: MSB-first per byte.  {note}\n")
        f.write("//UNVERIFIED until confirmed on the bench — see make_fs.py header.\n")
        for i in range(0, len(bitstream), LINE_BITS):
            f.write(bitstream[i:i + LINE_BITS] + "\n")

    print(f"wrote {dst}: {len(data)} bytes -> {len(bitstream)} bits "
          f"({len(bitstream)//LINE_BITS + 1} lines).  {note}")


if __name__ == "__main__":
    main()
