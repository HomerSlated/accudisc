#!/usr/bin/env python3
"""Compare one ctdb_ab arm against cdda2img's pinned reference JSON.

Exits 0 on exact parity, 1 on any difference. Usage:
    ctdb_ab_compare.py <ours.json> <reference.json> <label>

TWO NORMALISATIONS, both applied to the REFERENCE side only, both measured
rather than assumed (cdda2img §143.2, §143.3):

  * cdda2img's decoder counts ZERO-MAGNITUDE corrections -- erasure positions
    that were flagged by C2 but turned out undamaged. Its `corrected_errors`
    is the errata-locator degree `e + t`, which counts every erasure by
    construction, and its emission loop writes `new = old ^ 0`. Ours drops
    them (src/repair/rs16.c:283-288) because the return value should count
    real corrections. cdda2img has accepted ours as correct and theirs as a
    reporting defect, so this normalises THEIR side and never ours.

  * `affected_sectors` is derived from ALL corrections including the no-ops,
    so it inherits the same inflation and needs the same treatment. On the
    misaligned control 6 of their 52 sectors contain nothing but no-ops.

Nothing else is normalised. In particular the correction list is compared
element by element and not by length: a position-basis defect produces the
right COUNT at the wrong bytes, and a wrong grid window reproduces
`dirty_columns` exactly while changing the corrections completely. Both were
injected and both are caught only by the element-wise diff.
"""
import json
import sys


def main():
    if len(sys.argv) != 4:
        sys.exit(f"usage: {sys.argv[0]} <ours.json> <reference.json> <label>")
    ours_path, ref_path, label = sys.argv[1:4]
    ours = json.load(open(ours_path))
    ref = json.load(open(ref_path))

    def by_byte(corrections, drop_noop):
        out = {}
        for c in corrections:
            if drop_noop and c["old"] == c["new"]:
                continue
            out[c["byte"]] = (c["old"], c["new"])
        return out

    R = by_byte(ref["corrections"], True)
    O = by_byte(ours["corrections"], False)
    noop = sum(1 for c in ref["corrections"] if c["old"] == c["new"])

    only_ours = sorted(set(O) - set(R))
    only_ref = sorted(set(R) - set(O))
    differ = sorted(b for b in set(O) & set(R) if O[b] != R[b])

    print(f"=== {label}")
    print(f"  corrections: ours {len(O)} | ref {len(ref['corrections'])}"
          f" ({noop} zero-magnitude -> {len(R)} normalised)")
    print(f"  only-ours {len(only_ours)}  only-ref {len(only_ref)}"
          f"  value-disagreements {len(differ)}")
    for b in only_ours[:5]:
        print(f"    only ours: byte {b} {O[b]}")
    for b in only_ref[:5]:
        print(f"    only ref : byte {b} {R[b]}")
    for b in differ[:5]:
        print(f"    differ   : byte {b} ours={O[b]} ref={R[b]}")

    bad = bool(only_ours or only_ref or differ)

    for key in ("offset", "dirty_columns", "erasure_columns",
                "image_first_frame", "image_frames"):
        if key in ours and key in ref:
            same = ours[key] == ref[key]
            bad |= not same
            print(f"  {key:20s} ours={ours[key]!r:>10} ref={ref[key]!r:>10}"
                  f"  {'==' if same else '!! MISMATCH'}")

    refsect = ref.get("affected_sectors")
    if isinstance(refsect, list):
        nz = {c["byte"] // 2352 for c in ref["corrections"]
              if c["old"] != c["new"]}
        same = ours["affected_sector_count"] == len(nz)
        bad |= not same
        extra = (f"  (ref raw {len(refsect)},"
                 f" {len(refsect) - len(nz)} no-op-only sectors)"
                 if len(nz) != len(refsect) else "")
        print(f"  {'affected_sectors':20s} ours={ours['affected_sector_count']:>10}"
              f" ref={len(nz):>10}  {'==' if same else '!! MISMATCH'}{extra}")

    print(f"  ==> {'DIFFERS' if bad else 'EXACT PARITY'}\n")
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
