#!/usr/bin/env python3
"""
Put a new gps_sniffer image into the board over its X1 serial port, without the BOOT button.

The running sniffer writes the image into its other OTA slot, checks it (CRC-32 of every chunk and of
the whole image, then the IDF image check) and reboots into it; a new image that resets within 3 s is
rolled back by the bootloader. Wire format: gps_sniffer.c, ota_session().

    python ota_push.py --port <serial port> <build dir>/gps_sniffer.bin

Exit code 0: "#OTA DONE", then the new image's banner and "#BOOT", then "#OTA image ... confirmed"
(it kept running 3 s, so the bootloader will not roll it back). 2: booted but not confirmed. 1: failed.
"""
import argparse
import struct
import sys
import time
import zlib

import serial

SYNC = b"\xA5\x5A"
CHUNK = 1024
MAX_RETRIES = 20


def read_line(s, deadline):
    buf = bytearray()
    while time.monotonic() < deadline:
        b = s.read(1)
        if not b:
            continue
        if b == b"\n":
            return buf.decode("ascii", "replace").strip()
        buf += b
    return None


def wait_for(s, tokens, timeout):
    """Return the first line containing one of tokens, from the token on; None on timeout.
    Relayed NMEA lines in between are skipped."""
    deadline = time.monotonic() + timeout
    while True:
        line = read_line(s, deadline)
        if line is None:
            return None
        for t in tokens:
            i = line.find(t)
            if i >= 0:
                return line[i:]


def main():
    ap = argparse.ArgumentParser(description="gps_sniffer update over X1, no BOOT button")
    ap.add_argument("--port", required=True)
    ap.add_argument("--baud", type=int, default=115200)
    ap.add_argument("image")
    a = ap.parse_args()

    img = open(a.image, "rb").read()
    if len(img) < 32 or img[0] != 0xE9:
        sys.exit("not an ESP app image (no 0xE9 header): pass gps_sniffer.bin, not a merged image")
    crc = zlib.crc32(img) & 0xFFFFFFFF

    s = serial.Serial()
    s.port, s.baudrate, s.timeout = a.port, a.baud, 0.05
    s.dtr = s.rts = False  # set before open: no control line is toggled
    s.open()
    s.reset_input_buffer()

    print(f"image {len(img)} bytes, crc32 {crc:08x}")
    s.write(f"\r\n#KOGG-OTA {len(img)} {crc:08x}\r\n".encode())
    r = wait_for(s, ("#OTA READY", "#OTA FAIL"), 15)
    if not r or not r.startswith("#OTA READY"):
        sys.exit(f"no READY from the board: {r}")
    print(r)

    off, tries, t0 = 0, 0, time.monotonic()
    while off < len(img):
        chunk = img[off:off + CHUNK]
        s.write(SYNC + struct.pack("<HI", len(chunk), off) + chunk
                + struct.pack("<I", zlib.crc32(chunk) & 0xFFFFFFFF))
        r = wait_for(s, ("#OTA ACK", "#OTA NAK", "#OTA FAIL"), 3)
        if r and r.startswith("#OTA FAIL"):
            sys.exit(f"\n{r}")
        if r:
            held = int(r.split()[2])
            tries = tries + 1 if (r.startswith("#OTA NAK") or held <= off) else 0
            off = held
        else:
            tries += 1
        if tries > MAX_RETRIES:
            sys.exit(f"\nstuck at {off} of {len(img)}")
        print(f"\r{off * 100 // len(img):3d}%  {off}/{len(img)}", end="", flush=True)
    print()

    r = wait_for(s, ("#OTA DONE", "#OTA FAIL"), 30)
    if not r or not r.startswith("#OTA DONE"):
        sys.exit(f"no DONE from the board: {r}")
    print(f"{r}  ({time.monotonic() - t0:.1f} s)")

    # After the reboot: show what the new image prints (bootloader, banner, #BOOT, sweep table, #LOCK,
    # #OTA), count the relayed NMEA instead of printing it, and stop once the image has confirmed itself.
    deadline = time.monotonic() + 20
    booted = confirmed = False
    nmea = 0
    while time.monotonic() < deadline and not confirmed:
        line = read_line(s, deadline)
        if line is None:
            break
        if line.startswith("$G"):
            nmea += 1
            continue
        if line:
            print(line)
        booted = booted or "#BOOT" in line
        confirmed = "#OTA image" in line and "confirmed" in line
    print(f"({nmea} NMEA lines relayed meanwhile)")
    if not booted:
        sys.exit("no #BOOT line after the reboot: check the board (LED, power)")
    if not confirmed:
        print("new image did not confirm itself: it will be rolled back at the next reset")
        sys.exit(2)
    sys.exit(0)


if __name__ == "__main__":
    main()
