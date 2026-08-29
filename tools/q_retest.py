#!/usr/bin/env python3
"""Q re-read retest: does re-reading recover static Q damage, or only speed?

WHY THIS EXISTS
---------------
`docs/reference/RECOVERY.md:1387-1392` records that "no re-read combo improved"
residual Q, and treats the residual as static physical damage. cdda2img's §181
showed that conclusion is arithmetically inconsistent with their own measured
per-capture failure probability: at q=0.9849 on this disc, independent draws
predict ~20 frames rescued per additional capture out of 1314. A null result
therefore needs an explanation, and only three are available:

  1. the draws were NOT independent (same speed, correlated failure),
  2. the re-read path did not actually re-read,
  3. q does not transfer from whole-disc passes to targeted re-reads.

Our own tree already argued for (1) and nobody connected it: RECOVERY.md:888
lists **R6 "same-speed consensus"** as a REJECTED approach, on the grounds that
"persistent same-speed miscorrections converge on the wrong answer; consensus
must be speed-diverse". Meanwhile R1-R3 — the rungs the null result mostly
rests on — vary no speed at all.

Keith lifted the standing rule against damage/recovery tests for this question
specifically: "If we were able to recover Q data all along, but wrongly
concluded it was impossible, that's a serious flaw that needs to be corrected."

WHAT IT MEASURES
----------------
Per-frame Q-CRC outcomes, captured PER PASS, so the analysis is over the SET of
frames that failed and not over aggregate counts. Aggregate `subq_bad` cannot
answer this: 3270 bad on two passes is consistent both with the same 3270
frames failing twice (correlated, no recovery possible) and with 6540 distinct
frames each failing once (independent, all recoverable). Only the intersection
separates those, and it needs per-frame data.

  q_within  — computed across passes AT ONE SPEED
  q_across  — computed across passes at DIFFERENT speeds

  q_within ~ 1.0  and q_across < 1.0  -> speed is the mechanism; fixed-speed
                                          re-reads cannot work; the old null
                                          measured the wrong factor; R6 was
                                          right and RECOVERY.md's conclusion
                                          is correct but for the wrong reason.
  q_within ~ q_across < 1.0           -> fixed-speed re-reads SHOULD work, and
                                          the old null is a defect somewhere.

THE GATE (cdda2img §181 §5.3, adopted)
--------------------------------------
Record `speed_requested_x` and `speed_honoured_x` per pass, and DROP any rung
whose honoured speed duplicates another's. Without that a "different speeds"
comparison can quietly become a comparison of a thing with itself. This is not
hypothetical here: the PX-716A snaps a request of 16x down to 8x, so a naive
{16, 8} pair measures one speed twice.

Speeds are NEVER inferred from the request. Every pass records what the drive
said it adopted, and `analyse` refuses to compare two rungs whose honoured
speeds are equal.
"""

from __future__ import annotations

import argparse
import json
import pathlib
import sys
import time
from collections import Counter

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1]
                       / "bindings" / "python"))

import accudisc as ad  # noqa: E402

# The Q sub-frame occupies bytes 12..23 of each 96-byte raw P-W frame after
# deinterleaving; accudisc_sub_extract_q does that, and q_parse CRC-checks it.
FRAME = 96


def capture(dev_path: str, speed_x: int, out: pathlib.Path,
            lba: int, count: int) -> dict:
    """One whole-disc pass. Returns the per-pass record, bad frames included.

    The bad-frame SET is the deliverable, not the count — see the module
    docstring on why counts cannot answer the question.
    """
    bad: list[int] = []
    seen = 0

    def sink(chunk):
        nonlocal seen
        # chunk.data is audio+subchannel interleaved per sector; the subchannel
        # lane is the last 96 bytes of each sector.
        slen = chunk.sector_len
        data = chunk.data
        for i in range(chunk.nsec):
            raw = data[i * slen + slen - FRAME: (i + 1) * slen]
            q = ad.extract_q(raw)
            # parse_q RAISES CrcError on a failed checksum rather than
            # returning crc_ok=False — and a failed checksum is the entire
            # measurement here, so the exception IS the datum. Catching it is
            # not defensive coding; treating it as an error would discard
            # every observation this tool exists to make.
            try:
                ad.parse_q(q)
            except ad.CrcError:
                bad.append(chunk.lba + i)
            seen += 1

    t0 = time.monotonic()
    with ad.Device(dev_path) as dev:
        result = dev.read(lba, count, sink=sink, sub=ad.Sub.RAW,
                          speed_x=speed_x, copy=True)
    elapsed = time.monotonic() - t0
    st = result.stats

    rec = {
        "speed_requested_x": st.speed_requested_x,
        "speed_honoured_x": st.speed_honoured_x,
        "elapsed_s": round(elapsed, 1),
        "subq_total": st.subq_total,
        "subq_ok": st.subq_ok,
        "subq_bad": st.subq_total - st.subq_ok,
        "frames_seen": seen,
        "bad": bad,
    }
    # Cross-check our own decode against the library's counter. They are
    # independent paths to the same fact; a disagreement means one of them is
    # wrong and the analysis below would be built on it.
    if len(bad) != rec["subq_bad"]:
        rec["DECODE_MISMATCH"] = (
            f"sink counted {len(bad)} bad frames, library counted "
            f"{rec['subq_bad']} — do not trust this pass")
    out.write_text(json.dumps(rec))
    return rec


def q_from_pair(a: set, b: set) -> float | None:
    """Per-capture failure probability from two passes' bad-frame sets.

    For a frame that fails independently with probability q each pass,
    P(fails both) / P(fails one) = q. Estimated as |A n B| / |A u B| gives the
    Jaccard index J = q/(2-q), so q = 2J/(1+J).

    Returns None when the union is empty: no evidence, which is NOT q=0.
    """
    union = a | b
    if not union:
        return None
    j = len(a & b) / len(union)
    return 2 * j / (1 + j)


def analyse(records: list[dict]) -> None:
    by_speed: dict[int, list[dict]] = {}
    for r in records:
        by_speed.setdefault(r["speed_honoured_x"], []).append(r)

    print("PASSES")
    print(f"  {'req':>4} {'honoured':>8} {'bad':>7} {'total':>7} {'secs':>6}")
    for r in records:
        flag = "  <-- DECODE MISMATCH" if "DECODE_MISMATCH" in r else ""
        print(f"  {r['speed_requested_x']:>4} {r['speed_honoured_x']:>8} "
              f"{r['subq_bad']:>7} {r['subq_total']:>7} "
              f"{r['elapsed_s']:>6.0f}{flag}")

    # THE GATE. A requested speed is not a speed.
    dupes = {s: [r["speed_requested_x"] for r in rs]
             for s, rs in by_speed.items()
             if len({r["speed_requested_x"] for r in rs}) > 1}
    if dupes:
        print("\n  NOTE: distinct REQUESTS collapsed onto one honoured speed:")
        for s, reqs in dupes.items():
            print(f"    honoured {s}x <- requested {sorted(set(reqs))}")
        print("    They are the same rung. Treated as within-speed.")

    print("\nq_WITHIN  (same honoured speed — tests the R6 hypothesis)")
    within = []
    for s, rs in sorted(by_speed.items()):
        if len(rs) < 2:
            print(f"  {s:>3}x: only {len(rs)} pass, no pair")
            continue
        for i in range(len(rs)):
            for j in range(i + 1, len(rs)):
                q = q_from_pair(set(rs[i]["bad"]), set(rs[j]["bad"]))
                if q is not None:
                    within.append(q)
                    print(f"  {s:>3}x: pass{i} vs pass{j}  "
                          f"|A|={len(rs[i]['bad'])} |B|={len(rs[j]['bad'])} "
                          f"|A&B|={len(set(rs[i]['bad']) & set(rs[j]['bad']))}"
                          f"  q={q:.4f}")

    print("\nq_ACROSS  (different honoured speeds)")
    across = []
    speeds = sorted(by_speed)
    for i in range(len(speeds)):
        for j in range(i + 1, len(speeds)):
            a, b = by_speed[speeds[i]][0], by_speed[speeds[j]][0]
            q = q_from_pair(set(a["bad"]), set(b["bad"]))
            if q is not None:
                across.append(q)
                print(f"  {speeds[i]:>3}x vs {speeds[j]:>3}x  "
                      f"|A|={len(a['bad'])} |B|={len(b['bad'])} "
                      f"|A&B|={len(set(a['bad']) & set(b['bad']))}  q={q:.4f}")

    if within and across:
        mw, ma = sum(within) / len(within), sum(across) / len(across)
        print(f"\n  mean q_within = {mw:.4f}   mean q_across = {ma:.4f}")
        print("\nVERDICT")
        if mw > 0.995 and ma < 0.99:
            print("  q_within ~ 1.0, q_across < 1.0 — SPEED IS THE MECHANISM.")
            print("  Fixed-speed re-reads cannot recover Q. RECOVERY.md's")
            print("  conclusion stands; its stated REASON (static damage) is")
            print("  wrong, and R6 predicted this.")
        elif abs(mw - ma) < 0.01:
            print("  q_within ~ q_across — fixed-speed re-reads SHOULD work.")
            print("  Expected recovery over n re-reads: 1 - q^n.")
            for n in (3, 10, 20):
                print(f"    n={n:>2}: {100 * (1 - mw ** n):.1f}%")
            print("  The old null result is then a DEFECT, not a fact.")
        else:
            print("  Neither pattern cleanly. Report both numbers; do not")
            print("  round one toward a story.")

    # The frames that failed in EVERY pass are the irreducible floor.
    if len(records) >= 2:
        always = set(records[0]["bad"])
        for r in records[1:]:
            always &= set(r["bad"])
        ever: Counter = Counter()
        for r in records:
            ever.update(r["bad"])
        print(f"\n  frames bad in EVERY pass: {len(always)}")
        print(f"  frames bad in ANY pass  : {len(ever)}")
        if ever:
            print(f"  recoverable by re-read  : {len(ever) - len(always)} "
                  f"({100 * (len(ever) - len(always)) / len(ever):.1f}% of "
                  f"the damaged population)")


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--device", default="/dev/sr0")
    ap.add_argument("--dir", default="/var/tmp/qretest")
    ap.add_argument("--speeds", default="8,8,8,4,24",
                    help="requested speeds, one pass each, in order")
    ap.add_argument("--count", type=int, default=0,
                    help="sectors (0 = whole disc)")
    ap.add_argument("--analyse-only", action="store_true")
    args = ap.parse_args()

    d = pathlib.Path(args.dir)
    d.mkdir(parents=True, exist_ok=True)

    if args.analyse_only:
        recs = [json.loads(p.read_text())
                for p in sorted(d.glob("pass*.json"))]
        if not recs:
            sys.exit(f"no pass*.json in {d}")
        analyse(recs)
        return

    with ad.Device(args.device) as dev:
        toc = dev.read_toc()
    plan = ad.plan_read_range(toc)
    if not plan.ok:
        sys.exit(f"cannot resolve a read range: {plan.reason.token}")
    lba, count = plan.lba, args.count or plan.count
    print(f"disc: lba {lba}, {count} sectors, leadout {toc.leadout_lba}")

    speeds = [int(x) for x in args.speeds.split(",")]
    recs = []
    for n, s in enumerate(speeds):
        out = d / f"pass{n}_req{s}.json"
        print(f"\n[{n + 1}/{len(speeds)}] pass at {s}x -> {out.name}")
        r = capture(args.device, s, out, lba, count)
        print(f"    honoured {r['speed_honoured_x']}x, "
              f"{r['subq_bad']} bad of {r['subq_total']}, "
              f"{r['elapsed_s']:.0f}s")
        if "DECODE_MISMATCH" in r:
            print(f"    {r['DECODE_MISMATCH']}")
        recs.append(r)

    print()
    analyse(recs)


if __name__ == "__main__":
    main()
