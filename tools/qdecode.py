#!/usr/bin/env python3
"""Decode raw P-W subchannel (.subf) per sector: extract Q, check CRC-16/CCITT,
print track/index/rel-MSF/abs-MSF for good frames and flag bad ones.

Usage: qdecode.py FILE.sub START_LBA [--only-bad] [--boundaries]
"""
import sys

def q_extract(sec96):
    """raw 96 bytes P-W -> 12-byte Q (bit 6 of each byte, MSB-first)."""
    q = bytearray(12)
    for j in range(12):
        b = 0
        for k in range(8):
            b = (b << 1) | ((sec96[j*8 + k] >> 6) & 1)
        q[j] = b
    return q

def crc16_ccitt(data):
    crc = 0
    for byte in data:
        crc ^= byte << 8
        for _ in range(8):
            crc = ((crc << 1) ^ 0x1021) & 0xFFFF if (crc & 0x8000) else (crc << 1) & 0xFFFF
    return crc

def crc_ok(q):
    want = (~((q[10] << 8) | q[11])) & 0xFFFF
    return crc16_ccitt(q[0:10]) == want

def bcd(x):
    return (x >> 4) * 10 + (x & 0x0f)

def main():
    path = sys.argv[1]
    start = int(sys.argv[2])
    only_bad = '--only-bad' in sys.argv
    boundaries = '--boundaries' in sys.argv
    data = open(path, 'rb').read()
    n = len(data) // 96
    total = ok = 0
    prev = None  # (tno, idx)
    bad_lbas = []
    for i in range(n):
        lba = start + i
        q = q_extract(data[i*96:(i+1)*96])
        good = crc_ok(q)
        total += 1
        if good:
            ok += 1
        adr = q[0] & 0x0f
        ctrl = (q[0] >> 4) & 0x0f
        tno = bcd(q[1])
        idx = bcd(q[2])
        rel = (bcd(q[3]), bcd(q[4]), bcd(q[5]))
        ab = (bcd(q[7]), bcd(q[8]), bcd(q[9]))
        if not good:
            bad_lbas.append(lba)
        # Only ADR=1 frames carry position (track/index/MSF). ADR=2 is MCN,
        # ADR=3 is ISRC — they must not be read as position or they masquerade
        # as index-0 boundaries.
        is_pos = good and adr == 1
        cur = (tno, idx) if is_pos else prev
        is_boundary = is_pos and prev is not None and cur != prev
        if boundaries:
            if is_boundary or not good:
                tag = "BAD-CRC" if not good else f"--> t{tno} i{idx}"
                print(f"lba {lba}  {tag}"
                      + ("" if not good else
                         f"  ctrl{ctrl:x} rel{rel[0]:02d}:{rel[1]:02d}:{rel[2]:02d} abs{ab[0]:02d}:{ab[1]:02d}:{ab[2]:02d}"))
            if is_pos:
                prev = cur
        elif not only_bad or not good:
            if not good:
                tag = "BAD "
            elif adr == 2:
                tag = "MCN "
            elif adr == 3:
                tag = "ISRC"
            else:
                tag = "ok  "
            print(f"lba {lba} {tag} adr{adr} ctrl{ctrl:x} t{tno:02d} i{idx:02d} "
                  f"rel{rel[0]:02d}:{rel[1]:02d}:{rel[2]:02d} abs{ab[0]:02d}:{ab[1]:02d}:{ab[2]:02d}")
            if is_pos:
                prev = cur
    print(f"# {ok}/{total} CRC-ok ({100.0*ok/total:.2f}%), {total-ok} bad")
    if bad_lbas:
        print(f"# bad LBAs: {bad_lbas}")

if __name__ == '__main__':
    main()
