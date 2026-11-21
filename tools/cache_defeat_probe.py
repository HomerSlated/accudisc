#!/usr/bin/env python3
"""Does 0.49.0's cache_defeat make the next read of a span fresh on THIS drive?

Directed by Keith 2026-10-22 for the PX-716A. The rule below was fixed BEFORE
any read (TODO.md, the queue entry), and this script only applies it.

Design
------
Clean media gives identical bytes whether a read is cached or fresh, so the
discriminator is TIMING: the elapsed time of the one READ CD that re-reads the
span, from the per-command trace (host CLOCK_MONOTONIC around the ioctl). A
cached answer costs only the bus transfer; a fresh one costs a seek back and
rotational latency.

Every read happens in ONE process on ONE open handle, so nothing the CLI does at
open (TOC, mode pages) can land between the two reads. ``speed_x=0`` throughout:
with a speed given, the engine sends SET CD SPEED on every read, which could
recalibrate or flush the cache itself. This therefore tests the MECHANISM, not
cdda2img's ladder, which sets a speed on every attempt.

An instance is two reads of the same 24-sector span S, one READ CD each. Not
32 (ADSC_CHUNK_MAX): the CDEmu dry run showed the transport caps a transfer at
64 KiB, so 32 sectors went out as 27 + 5. The validity check below requires
exactly one transfer per read, so a drive that splits even 24 is caught.

  arm A (control): read S, read S                     — nothing between
  arm B (defeat):  read S, read S with cache_defeat   — one far read between,
                                                        at S - 5000

Instances alternate A, B, A, B ... so any drift across the disc hits both arms
alike. Each has its own span, 10000 sectors past the previous one: well beyond
the far target (5000) plus a full 8 MB buffer of read-ahead (~3600 sectors), so
every instance's FIRST read is cold.

Reading rule (fixed before any read)
------------------------------------
t1 = the first read's span transfer, t2 = the re-read's span transfer.

Validity, per instance, from the trace:
  A: no media command (READ CD / READ(10) / READ(12) / SEEK) between the two
     span transfers.
  B: exactly one, a 1-sector READ CD at S - 5000, completed GOOD, and
     read_stats.cache_defeat == DONE.
  Any invalid instance -> UNRESOLVED.

Control: every A instance must have t2 <= t1 / 4 (clearly cached). Otherwise
  MOOT: back-to-back re-reads of a 24-sector span are not cached on this drive,
  so there is nothing for the defeat to do in this pattern. Stop.

Let c = the largest A t2. Each B instance is
  FRESH  if t2 >= 4c,  CACHED if t2 <= 2c,  otherwise AMBIGUOUS.

Verdict: WORKS if every B is FRESH; DOES NOT WORK if every B is CACHED;
anything else UNRESOLVED. No further reads follow any outcome without Keith.
"""

from __future__ import annotations

import argparse
import hashlib
import re
import sys
import time

import accudisc

SPAN = 24  # one READ CD: 56448 bytes, under the 64 KiB transfer cap
FAR = 5000  # ADSC_FLUSH_DISTANCE
STEP = 10000  # between instances
MEDIA = {0xBE: "READ CD", 0x28: "READ(10)", 0xA8: "READ(12)", 0x2B: "SEEK",
         0xB9: "READ CD MSF"}

ISSUE = re.compile(r"trace #(\d+) \+[0-9.]+ > ((?:[0-9a-f]{2} ){5,}[0-9a-f]{2})")
RESULT = re.compile(r"trace #(\d+) < (.*?)\s+([0-9.]+) ms")


def parse(lines):
    """Pair issue and result lines by sequence number: (seq, opcode, lba,
    nsec, status, ms). LBA and length are decoded for READ CD only."""
    cmds = {}
    for ln in lines:
        m = ISSUE.search(ln)
        if m:
            cdb = bytes.fromhex(m.group(2).replace(" ", ""))
            lba = nsec = None
            if cdb[0] == 0xBE and len(cdb) >= 9:
                lba = int.from_bytes(cdb[2:6], "big")
                nsec = int.from_bytes(cdb[6:9], "big")
            cmds[int(m.group(1))] = [cdb[0], lba, nsec, None, None]
            continue
        m = RESULT.search(ln)
        if m and int(m.group(1)) in cmds:
            cmds[int(m.group(1))][3] = m.group(2).strip()
            cmds[int(m.group(1))][4] = float(m.group(3))
    return [(s, *v) for s, v in sorted(cmds.items())]


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--device", required=True)
    ap.add_argument("--pairs", type=int, required=True,
                    help="instances PER ARM (total reads = pairs*5)")
    ap.add_argument("--start", type=int, default=None,
                    help="first span LBA (default: 30%% into the audio)")
    ap.add_argument("--log", required=True, help="raw trace file (/var/tmp)")
    a = ap.parse_args()

    lines: list[str] = []
    dev = accudisc.Device(a.device, trace=2)
    dev.set_log(lines.append)

    toc = dev.read_toc()
    audio = [t for t in toc.tracks if t.is_audio and t.session in (0, 1)]
    lo = min(t.full_extent[0] for t in audio)
    hi = max(t.lba + t.sectors for t in audio)
    n = 2 * a.pairs
    first = a.start if a.start is not None else lo + (hi - lo) * 3 // 10
    spans = [first + i * STEP for i in range(n)]
    if spans[0] - FAR < lo or spans[-1] + SPAN > hi:
        print(f"spans {spans[0]}..{spans[-1]} do not fit in audio {lo}..{hi}",
              file=sys.stderr)
        return 2

    rows = []
    for i, s in enumerate(spans):
        arm = "AB"[i % 2]
        mark = len(lines)
        b1, b2 = bytearray(), bytearray()
        dev.read(s, SPAN, sink=lambda ch: b1.extend(ch.data))
        st = dev.read(s, SPAN, sink=lambda ch: b2.extend(ch.data),
                      cache_defeat=(arm == "B")).stats
        rows.append((arm, s, parse(lines[mark:]),
                     int(st.cache_defeat),
                     hashlib.sha256(b1).hexdigest() ==
                     hashlib.sha256(b2).hexdigest()))
    dev.close()

    with open(a.log, "w") as f:
        f.write(time.strftime("# %Y-%m-%dT%H:%M:%S%z\n"))
        f.write("\n".join(lines) + "\n")

    # ---- apply the rule ----------------------------------------------------
    res = []
    ok = True
    for arm, s, cmds, cd, same in rows:
        span = [c for c in cmds if c[1] == 0xBE and c[2] == s and c[3] == SPAN]
        if len(span) != 2:
            print(f"{arm} {s}: {len(span)} span transfers, want 2 -> invalid")
            ok = False
            continue
        between = [c for c in cmds if span[0][0] < c[0] < span[1][0]
                   and c[1] in MEDIA]
        if arm == "A":
            valid = not between
        else:
            valid = (len(between) == 1 and between[0][1] == 0xBE and
                     between[0][2] == s - FAR and between[0][3] == 1 and
                     (between[0][4] or "").startswith("GOOD") and
                     cd == int(accudisc.CacheDefeat.DONE))
        t1, t2 = span[0][5], span[1][5]
        far = between[0][5] if between else None
        print(f"{arm} lba={s:6d} t1={t1:8.3f} ms  t2={t2:8.3f} ms  "
              f"far={'-' if far is None else f'{far:.3f} ms':>11}  "
              f"between={[MEDIA[c[1]] for c in between]}  "
              f"same_bytes={same}  valid={valid}")
        ok &= valid
        res.append((arm, t1, t2))

    if not ok:
        print("VERDICT: UNRESOLVED (an instance failed validity)")
        return 1
    a_rows = [r for r in res if r[0] == "A"]
    if any(t2 > t1 / 4 for _, t1, t2 in a_rows):
        print("VERDICT: MOOT (control re-reads not clearly cached)")
        return 0
    c = max(t2 for _, _, t2 in a_rows)
    cls = ["FRESH" if t2 >= 4 * c else "CACHED" if t2 <= 2 * c else
           "AMBIGUOUS" for arm, _, t2 in res if arm == "B"]
    print(f"c = {c:.3f} ms; B: {cls}")
    if all(x == "FRESH" for x in cls):
        print("VERDICT: WORKS")
    elif all(x == "CACHED" for x in cls):
        print("VERDICT: DOES NOT WORK")
    else:
        print("VERDICT: UNRESOLVED")
    return 0


if __name__ == "__main__":
    sys.exit(main())
