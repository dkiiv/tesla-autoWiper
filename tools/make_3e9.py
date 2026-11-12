#!/usr/bin/env python3
"""
Bench helper for DAS_bodyControls (0x3E9) frames.

Same packing and checksum as main/protocol.c (opendbc tesla_checksum):
  DAS_wiperSpeed           byte 0, upper nibble (0 = off, 1..14, 15 = INVALID)
  DAS_bodyControlsCounter  byte 6, upper nibble
  DAS_bodyControlsChecksum byte 7 = (0xE9 + 0x03 + sum(bytes 0..6)) & 0xFF

Examples
  # one frame for can-utils cansend, wiper speed 5, counter 3
  ./tools/make_3e9.py --speed 5 --counter 3
      -> 3E9#500000000000306C

  # a stream at 10 Hz with a rolling counter, piped into cansend on can0
  ./tools/make_3e9.py --speed 5 --count 100 --stream | \
      while read f; do cansend can0 "$f"; sleep 0.1; done

  # check frames captured with `candump -L can0` (or paste payload hex)
  candump -L can0,3E9:7FF | ./tools/make_3e9.py --check

Standard library only; no third-party packages.
"""

import argparse
import re
import sys

CAN_ID = 0x3E9


def checksum(data7):
    """Tesla checksum over bytes 0..6 (opendbc tesla_checksum)."""
    return ((CAN_ID & 0xFF) + ((CAN_ID >> 8) & 0xFF) + sum(data7)) & 0xFF


def build(speed, counter, base=None):
    d = list(base) if base else [0] * 8
    if not 0 <= speed <= 15:
        raise ValueError("speed must be 0..15")
    d[0] = (d[0] & 0x0F) | (speed << 4)
    d[6] = (d[6] & 0x0F) | ((counter & 0x0F) << 4)
    d[7] = checksum(d[:7])
    return d


def to_cansend(d):
    return "%03X#%s" % (CAN_ID, "".join("%02X" % b for b in d))


def parse_payload(text):
    """Accept '3E9#AABB..', candump -L lines, or bare hex (spaces allowed)."""
    m = re.search(r"3E9#([0-9A-Fa-f]{16})", text)
    if m:
        hexstr = m.group(1)
    else:
        hexstr = re.sub(r"[^0-9A-Fa-f]", "", text)
        if len(hexstr) != 16:
            return None
    return [int(hexstr[i:i + 2], 16) for i in range(0, 16, 2)]


def check(stream):
    prev_ctr = None
    bad = 0
    for line in stream:
        d = parse_payload(line)
        if d is None:
            continue
        ok = d[7] == checksum(d[:7])
        ctr = d[6] >> 4
        skip = prev_ctr is not None and ctr != ((prev_ctr + 1) & 0x0F)
        prev_ctr = ctr
        bad += 0 if ok else 1
        print("%s  speed=%-2d counter=%-2d checksum=%s%s" % (
            to_cansend(d), d[0] >> 4, ctr, "OK " if ok else "BAD",
            "  (counter skip)" if skip else ""))
    return 1 if bad else 0


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--speed", type=int, default=0, help="DAS_wiperSpeed 0..15")
    ap.add_argument("--counter", type=int, default=0, help="starting counter 0..15")
    ap.add_argument("--base", help="8-byte hex payload to start from (other signals)")
    ap.add_argument("--count", type=int, default=1, help="number of frames")
    ap.add_argument("--stream", action="store_true",
                    help="emit --count frames with a rolling counter")
    ap.add_argument("--check", action="store_true",
                    help="read frames on stdin and verify checksum/counter")
    a = ap.parse_args()

    if a.check:
        return check(sys.stdin)

    base = parse_payload(a.base) if a.base else None
    if a.base and base is None:
        ap.error("--base must be 8 bytes of hex")
    n = a.count if a.stream else 1
    for i in range(n):
        print(to_cansend(build(a.speed, a.counter + i, base)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
