#!/usr/bin/env python3
"""Capture raw bytes from the firmware debug shell `flash dump` command.

The shell prints `FLASHDUMP <len>\r\n` then streams exactly <len> raw bytes
(usb_send_bytes). This reads them cleanly (binary, no text mangling) and
writes a contiguous region to a file by issuing 4096-byte dumps.

Usage: python3 scripts/flash_capture.py <start_hex_or_dec> <total_len> <outfile>
"""
import glob
import sys
import time

import serial

PORT_GLOBS = ["/dev/cu.usbmodem*", "/dev/tty.usbmodem*"]


def find_port():
    for g in PORT_GLOBS:
        m = sorted(glob.glob(g))
        if m:
            return m[0]
    raise SystemExit("No usbmodem serial port found")


def parse_int(s):
    return int(s, 16) if s.lower().startswith("0x") else int(s)


def read_until(ser, marker, timeout=3.0):
    """Read bytes until `marker` (bytes) is seen; return everything up to+including it."""
    buf = b""
    deadline = time.time() + timeout
    while time.time() < deadline:
        b = ser.read(1)
        if b:
            buf += b
            if buf.endswith(marker):
                return buf
    raise SystemExit(f"timeout waiting for {marker!r}; got tail {buf[-40:]!r}")


def dump_chunk(ser, addr, length):
    ser.reset_input_buffer()
    ser.write(f"flash dump {addr} {length}\r\n".encode())
    ser.flush()
    # The echoed command + the FLASHDUMP header line precede the raw bytes.
    read_until(ser, b"FLASHDUMP ")
    # read the rest of the header line through the terminating \n (consume \r\n)
    line = read_until(ser, b"\n")
    declared = int(line.decode("latin1").strip())
    # now read exactly `declared` raw bytes
    data = b""
    deadline = time.time() + 10.0
    while len(data) < declared and time.time() < deadline:
        chunk = ser.read(declared - len(data))
        if chunk:
            data += chunk
            deadline = time.time() + 10.0
    if len(data) != declared:
        raise SystemExit(f"short read at {addr}: {len(data)}/{declared}")
    return data


def main():
    if len(sys.argv) != 4:
        raise SystemExit(__doc__)
    start = parse_int(sys.argv[1])
    total = parse_int(sys.argv[2])
    outfile = sys.argv[3]

    port = find_port()
    ser = serial.Serial(port, 115200, timeout=0.2)
    time.sleep(0.3)
    ser.reset_input_buffer()
    print(f"# connected: {port}  capturing {total} B @ 0x{start:X} -> {outfile}")

    out = bytearray()
    addr = start
    remaining = total
    while remaining > 0:
        n = min(4096, remaining)
        data = dump_chunk(ser, addr, n)
        out += data
        addr += n
        remaining -= n
        sys.stdout.write(f"\r  {len(out)}/{total} bytes")
        sys.stdout.flush()
    print()
    with open(outfile, "wb") as f:
        f.write(out)
    print(f"# wrote {len(out)} bytes to {outfile}")


if __name__ == "__main__":
    main()
