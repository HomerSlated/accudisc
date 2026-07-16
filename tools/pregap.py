#!/usr/bin/env python3
"""Per-boundary pregap census. Given a .sub region and its START_LBA, find every
index-0 (pregap) span and report extent, damage, and whether the recovery-critical
anchors (pregap-start frame, index-1 boundary frame) survived.

Usage: pregap.py FILE.sub START_LBA EXPECT_TRACK EXPECT_INDEX1_LBA
"""
import sys

def q_extract(sec96):
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
    return crc16_ccitt(q[0:10]) == ((~((q[10] << 8) | q[11])) & 0xFFFF)

def bcd(x):
    return (x >> 4) * 10 + (x & 0x0f)

def main():
    path, start = sys.argv[1], int(sys.argv[2])
    exp_t, exp_i1 = int(sys.argv[3]), int(sys.argv[4])
    data = open(path, 'rb').read()
    n = len(data) // 96
    frames = {}   # lba -> (tno, idx, rel_frames)  (ADR=1, CRC-ok only)
    bad = set()
    total = ok = 0
    for i in range(n):
        lba = start + i
        q = q_extract(data[i*96:(i+1)*96])
        total += 1
        if not crc_ok(q):
            bad.add(lba); continue
        ok += 1
        if (q[0] & 0x0f) != 1:            # position frames only
            continue
        tno, idx = bcd(q[1]), bcd(q[2])
        rel = bcd(q[3]) * 60 * 75 + bcd(q[4]) * 75 + bcd(q[5])
        frames[lba] = (tno, idx, rel)

    # index-0 frames belonging to the expected track = its pregap
    pg = sorted(l for l, (t, i, r) in frames.items() if t == exp_t and i == 0)
    # observed index-1 start of the expected track
    i1 = sorted(l for l, (t, i, r) in frames.items() if t == exp_t and i == 1)
    obs_i1 = i1[0] if i1 else None

    rate = 100.0 * ok / total if total else 0.0
    if not pg:
        # no clean index-0 frame seen — is the boundary region damaged?
        near_bad = sum(1 for l in bad if exp_i1 - 300 <= l < exp_i1)
        print(f"t{exp_t:02d} i1@{exp_i1}: PREGAP NOT SEEN  "
              f"(obs_i1={obs_i1}, {near_bad} bad frames in 300 before i1, "
              f"region {rate:.1f}% Q-ok)")
        return
    lo, hi = pg[0], pg[-1]
    # pregap length via the countdown anchor: rel at its lowest index-0 lba
    rel_at_lo = frames[lo][2]
    length_by_rel = rel_at_lo + (obs_i1 - lo if obs_i1 else 0) if obs_i1 else rel_at_lo + 1
    # simplest robust length: index1_start - first_index0_lba
    length_span = (obs_i1 - lo) if obs_i1 else (hi - lo + 1)
    dead_in_pg = sum(1 for l in bad if lo <= l < (obs_i1 or hi + 1))
    start_ok = "start-ok" if frames.get(lo, (0,0,0))[1] == 0 else "?"
    i1_ok = "i1-ok" if obs_i1 is not None else "i1-BAD"
    print(f"t{exp_t:02d} i1@{exp_i1}: pregap {length_span:3d}f "
          f"[{lo}..{(obs_i1 or hi+1)-1}] relmax={frames[lo][2]} "
          f"cleanpg={len(pg)} dead_in_pg={dead_in_pg} {start_ok} {i1_ok} "
          f"| region {rate:.1f}% Q-ok ({total-ok}/{total} bad)")

if __name__ == '__main__':
    main()
