#!/usr/bin/env python3
"""Full backup of the external W25Q128 SPI flash via the USB CDC debug shell.

The firmware's `flash dump <addr> <len>` command streams RAW BINARY after a
"FLASHDUMP <len>\\r\\n" header line, then returns to the "> " prompt. We loop
4096-byte chunks across the whole 16 MB chip and write one binary file.

Usage: uv run python scripts/flash_backup.py <out.bin> [total_bytes]
"""
import glob
import sys
import time

import serial

CHUNK = 4096
DEFAULT_TOTAL = 16 * 1024 * 1024  # W25Q128 = 16 MB


def find_port():
    for g in ("/dev/cu.usbmodem*", "/dev/tty.usbmodem*"):
        hits = sorted(glob.glob(g))
        if hits:
            return hits[0]
    raise SystemExit("No usbmodem serial port found")


def read_exact(ser, n, deadline):
    buf = bytearray()
    while len(buf) < n:
        if time.time() > deadline:
            raise TimeoutError(f"timeout: got {len(buf)}/{n}")
        chunk = ser.read(n - len(buf))
        if chunk:
            buf += chunk
    return bytes(buf)


def read_until(ser, marker, deadline):
    buf = bytearray()
    while marker not in buf:
        if time.time() > deadline:
            raise TimeoutError(f"timeout waiting for {marker!r}; got tail {bytes(buf[-40:])!r}")
        b = ser.read(1)
        if b:
            buf += b
    return bytes(buf)


def main():
    if len(sys.argv) < 2:
        raise SystemExit("usage: flash_backup.py <out.bin> [total_bytes]")
    out_path = sys.argv[1]
    total = int(sys.argv[2]) if len(sys.argv) > 2 else DEFAULT_TOTAL

    port = find_port()
    ser = serial.Serial(port, 115200, timeout=0.2)
    print(f"# port {port}  ->  {out_path}  ({total} bytes)", flush=True)
    time.sleep(0.3)
    ser.reset_input_buffer()

    t0 = time.time()
    with open(out_path, "wb") as f:
        for addr in range(0, total, CHUNK):
            n = min(CHUNK, total - addr)
            ser.reset_input_buffer()
            ser.write(f"flash dump 0x{addr:06X} {n}\r\n".encode())
            ser.flush()
            deadline = time.time() + 6.0
            # Wait for the "FLASHDUMP <n>\r\n" header, then read exactly n raw bytes.
            hdr = read_until(ser, b"FLASHDUMP", deadline)
            line = hdr + read_until(ser, b"\n", deadline)
            data = read_exact(ser, n, deadline)
            f.write(data)
            if (addr // CHUNK) % 256 == 0:
                pct = 100.0 * (addr + n) / total
                rate = (addr + n) / max(1e-3, time.time() - t0) / 1024
                print(f"  {pct:5.1f}%  {addr+n}/{total}  {rate:.0f} KiB/s", flush=True)
    ser.close()
    print(f"# done: {out_path}  in {time.time()-t0:.0f}s", flush=True)


if __name__ == "__main__":
    main()
