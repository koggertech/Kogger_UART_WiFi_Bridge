#!/usr/bin/env python3
"""
Talk to a running gps_sniffer over X1: optionally send one command line, then stream its output.

    python console.py --port COM3                               # just watch
    python console.py --port COM3 --send "#LISTEN 5 115200" --seconds 25   # listen to the reverse line
    python console.py --port COM3 --send "#SNIFF"               # back to the auto sweep

Commands (one per line, see gps_sniffer.c): "#LISTEN <pin> <baud>" (pin 4 = X2.2, 5 = X2.1; baud 0 =
sweep), "#SNIFF", "#KOGG-OTA ..." (use ota_push.py for updates). Control lines are those starting with
'#'; by default relayed NMEA ($G...) is summarised, not printed. This tool sends no bytes except --send.
"""
import argparse
import sys
import time

import serial


def main():
    ap = argparse.ArgumentParser(description="watch / command a running gps_sniffer")
    ap.add_argument("--port", required=True)
    ap.add_argument("--baud", type=int, default=115200)
    ap.add_argument("--send", default=None, help="one command line to send first")
    ap.add_argument("--seconds", type=float, default=20.0)
    ap.add_argument("--nmea", action="store_true", help="print relayed $G... lines too")
    a = ap.parse_args()

    s = serial.Serial()
    s.port, s.baudrate, s.timeout = a.port, a.baud, 0.1
    s.dtr = s.rts = False   # set before open: no control line is toggled
    s.open()
    s.reset_input_buffer()
    if a.send:
        s.write(("\r\n" + a.send + "\r\n").encode())
        print(f">> {a.send}")

    end = time.monotonic() + a.seconds
    buf = bytearray()
    nmea = 0
    while time.monotonic() < end:
        buf += s.read(4096)
        while b"\n" in buf:
            raw, _, buf = buf.partition(b"\n")
            line = raw.decode("ascii", "replace").strip()
            if not line:
                continue
            if line.startswith("$G") and not a.nmea:
                nmea += 1
                continue
            print(line)
    s.close()
    if nmea and not a.nmea:
        print(f"({nmea} NMEA lines relayed meanwhile)")


if __name__ == "__main__":
    main()
