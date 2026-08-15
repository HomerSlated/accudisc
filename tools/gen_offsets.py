#!/usr/bin/env python3
"""Compile the drive read-offset table from the two live primary sources.

    REDUMP  private/code/redumper/offsets.ixx      read offsets, INQUIRY-keyed
    AR      tools/fetch_ar_offsets.py --out ...    read offsets + confidence

EAC's OffsetBase was a third candidate and is DROPPED (Keith, 2026-08-15). It is
not an independent source: the only copy of it is a 2004 Wayback snapshot of a
page that no longer exists, and EAC-the-application reads its offsets from
AccurateRip over the network — so agreement between the two measures a corpus
against its own archived ancestor, not two collections agreeing. Its one unique
contribution was ~112 WRITE offsets, and write offsets are now a measurement
(accudisc_measure_write_offset) rather than a table.

Output is src/drive/offsets_db.inc, compiled into libaccudisc. Nothing at
runtime looks anything up over a network; this tool runs on the development
cycle and its output is committed.

KEYING, AND THE VENDOR ALIAS LIST
---------------------------------
The table is keyed on SCSI INQUIRY vendor + product, because that is what a
drive reports and therefore the only key a runtime lookup can use.

REDUMP keys on the INQUIRY vendor; AccurateRip sometimes keys on the marketing
name for the same company. Unaliased, that splits one drive into two rows and
makes the catalogues look far more divergent than they are. Measured here:

    alias OFF   shared 3503   REDUMP-only 1086   AR-only 1299
    alias ON    shared 4526   REDUMP-only   63   AR-only  276

HL-DT-ST is Hitachi-LG; MATSHITA is Panasonic. Of 650 REDUMP HL-DT-ST rows, 649
match an AccurateRip "LG Electronics" row and all 649 AGREE on the offset, with
none disagreeing — which is what makes the rewrite safe to assert.

This is an EXACT rewrite of a whole vendor field from an explicit reviewed list,
never a substring or similarity match. That distinction is the whole point: a
substring matcher maps "Plextor PX-116A" onto "PX-116A2", a different drive, and
a wrong offset applied silently is worse than a missing one. Nothing is inferred
here; a new alias is a human decision recorded below.

CONFLICTS
---------
REDUMP carries eight keys twice with disagreeing values, and a first-match
lookup silently returned whichever row upstream emitted first — a well-formed
int32 that could be wrong by up to 667 samples. Two outcomes here:

  * where two or more sources agree on one value and the rest do not, that
    value is adjudicated and flagged ADJUDICATED, so the resolution stays
    visible rather than becoming invisible truth;
  * otherwise every value is kept, each flagged CONFLICT, and the runtime
    reports them all and refuses to pick.

Adjudication is corroboration by an independent collection, NOT a preference
for a position in the file. Note the caveat honestly: whether REDUMP and
AccurateRip are truly independent is unsettled — a sibling project measured
agreement as flat across a 100x range of submission support, which is equally
explained by shared provenance and by read offset being a near-deterministic
per-model constant. Corroboration is still strictly better evidence than file
order, which is what it replaces.

Usage:
    gen_offsets.py --redump private/code/redumper/offsets.ixx \
                   --ar     private/research/incoming/ar_offsets.json \
                   --out    src/drive/offsets_db.inc
"""

from __future__ import annotations

import argparse
import json
import re
import sys
from collections import defaultdict
from pathlib import Path

# Keep in step with the ACCUDISC_OFFSET_SRC_* / ACCUDISC_OFFSET_F_* macros in
# include/accudisc/accudisc.h. tests/test_offsets.c asserts the generated table
# agrees with the header, so a drift here fails the suite rather than the field.
SRC_REDUMP, SRC_AR = 1, 2
F_CONFLICT, F_ADJUDICATED = 1, 4

# EXACT whole-field vendor rewrites, applied to BOTH sources before keying.
# Reviewed by hand, one line per decision, never inferred — see the module
# docstring for the measurement behind each and for why similarity matching is
# refused. Left side and right side are both uppercase, whitespace-collapsed.
VENDOR_ALIAS = {
    "HL-DT-ST": "LG ELECTRONICS",  # Hitachi-LG Data Storage; 649/650 match, 0 differ
    "MATSHITA": "PANASONIC",       # Matsushita; 375/375 match, 0 differ
    "FREECOM_": "FREECOM",         # AccurateRip trailing underscore
}


def src_names(mask: int) -> str:
    return "+".join(
        n for n, m in (("REDUMP", SRC_REDUMP), ("AR", SRC_AR)) if mask & m
    ) or "none"


def norm(s: str) -> str:
    """Collapse whitespace runs and trim — the INQUIRY rule, minus case folding.

    Mirrors adsc_inquiry_normalize() in src/drive/offsets.c. Drives pad the
    fixed INQUIRY fields ("DVDR   PX-716A"), so the table and the lookup must
    agree on this or nothing matches.
    """
    return " ".join(s.split())


def fold(vendor: str, product: str) -> str:
    """The build-time join key: aliased, case-folded, whitespace-collapsed.

    Case folding and aliasing are right for pooling evidence across sources at
    build time and WRONG for the runtime lookup, which compares against bytes a
    drive actually reported. The runtime stays case-sensitive and alias-free;
    only this tool folds, and it emits every spelling it saw so either
    convention still matches at runtime.
    """
    v = norm(vendor).upper()
    v = VENDOR_ALIAS.get(v, v)
    return f"{v} {norm(product).upper()}".strip()


# --------------------------------------------------------------------------
# Sources
# --------------------------------------------------------------------------

def read_redump(path: Path) -> list[tuple[str, str, int]]:
    entry = re.compile(
        r'\{\s*"(?P<vendor>[^"]*)"\s*,\s*"(?P<product>[^"]*)"\s*,\s*(?P<offset>[+-]?\d+)\s*\}'
    )
    rows = []
    for line in path.read_text(encoding="utf-8").splitlines():
        m = entry.search(line)
        if m:
            rows.append((m["vendor"], m["product"], int(m["offset"])))
    if not rows:
        sys.exit(f"no entries parsed from {path}")
    return rows


def read_ar(path: Path) -> list[dict]:
    """AccurateRip rows, with same-drive duplicates resolved by submission count.

    THE PAGE LISTS SOME DRIVES TWICE WITH DIFFERENT OFFSETS, and the two rows are
    not comparable evidence:

        PIONEER BD-RW BDR-206    +667 / 1065 submissions
                                   +0 /    4 submissions
        PLEXTOR DVDR PX-740A     +618 /  554 submissions
                                   +6 /    1 submission

    That asymmetry is the whole signal. A parser that stores one row per key —
    a dict build, or a last-write-wins insert — silently keeps whichever row the
    page printed last, which in every case measured here is the SMALL one. Doing
    that turns AccurateRip's own 1065-to-4 verdict into its opposite while
    looking perfectly well-formed, and it is the reason the eight REDUMP
    "self-contested" keys were read as REDUMP duplicating itself: both
    catalogues carry both values, and only the counts distinguish them.

    So the duplicates are resolved HERE, within the source, by AccurateRip's own
    count, before any cross-source comparison sees them. Ties keep the first row
    and are reported, because a tie is not a resolution.
    """
    rows = json.loads(path.read_text(encoding="utf-8"))["rows"]
    best: dict[str, dict] = {}
    collapsed = 0
    for r in rows:
        key = fold(r["vendor"], r["product"])
        prev = best.get(key)
        if prev is None:
            best[key] = r
            continue
        collapsed += 1
        if r["submissions"] > prev["submissions"]:
            if prev["offset"] != r["offset"]:
                print(
                    f"  AR duplicate {r['vendor']!r} {r['product']!r}: "
                    f"{prev['offset']:+d}/{prev['submissions']} subs superseded by "
                    f"{r['offset']:+d}/{r['submissions']} subs"
                )
            best[key] = r
        elif r["submissions"] == prev["submissions"] and r["offset"] != prev["offset"]:
            print(
                f"  AR TIE {r['vendor']!r} {r['product']!r}: "
                f"{prev['offset']:+d} vs {r['offset']:+d}, both "
                f"{r['submissions']} subs — keeping the first"
            )
        elif prev["offset"] != r["offset"]:
            print(
                f"  AR duplicate {r['vendor']!r} {r['product']!r}: "
                f"{r['offset']:+d}/{r['submissions']} subs discarded for "
                f"{prev['offset']:+d}/{prev['submissions']} subs"
            )
    if collapsed:
        print(f"  ({collapsed} AccurateRip duplicate row(s) resolved by submission count)")
    return list(best.values())


# --------------------------------------------------------------------------
# Merge
# --------------------------------------------------------------------------

class Row:
    __slots__ = ("vendor", "product", "read", "subs", "pct", "sources", "flags")

    def __init__(self, vendor: str, product: str, read: int) -> None:
        self.vendor = norm(vendor)
        self.product = norm(product)
        self.read = read
        self.subs = 0
        self.pct = 0
        self.sources = 0
        self.flags = 0

    def sort_key(self) -> tuple:
        return (self.vendor.upper(), self.product.upper(), self.read)


def merge(redump, ar) -> tuple[list[Row], dict]:
    """Pool every source's claims per key, then adjudicate once, symmetrically.

    No source is applied on top of another. An earlier draft walked them in
    sequence — REDUMP first, AR attached to what REDUMP left — and that shape
    set the AR source flag even where AccurateRip DISAGREED with the value being
    kept, asserting a corroboration that had not happened. Pooling first and
    deciding once makes that unrepresentable.
    """
    stats = defaultdict(int)

    claims: dict[str, dict[int, int]] = defaultdict(lambda: defaultdict(int))
    ar_meta: dict[str, dict] = {}

    # EVERY distinct exact spelling of a key is kept, not just the first seen.
    # The folded key is right for pooling evidence and wrong for emission: the
    # runtime compares against the bytes a drive reported, case and all, so
    # collapsing ("", "DVDROM GO-D1600B") and ("DVDROM", "GO-D1600B") into one
    # row would leave a drive reporting the other form unmatched. Aliasing makes
    # this load-bearing rather than incidental — HL-DT-ST and LG Electronics are
    # one key here and two distinct INQUIRY strings in the field, and BOTH must
    # be present for either drive to match.
    spellings: dict[str, set[tuple[str, str]]] = defaultdict(set)

    for vendor, product, off in redump:
        key = fold(vendor, product)
        claims[key][off] |= SRC_REDUMP
        spellings[key].add((norm(vendor), norm(product)))

    for a in ar:
        key = fold(a["vendor"], a["product"])
        claims[key][a["offset"]] |= SRC_AR
        spellings[key].add((norm(a["vendor"]), norm(a["product"])))
        ar_meta[key] = a

    rows: list[Row] = []
    for key, values in claims.items():
        forms = sorted(spellings[key])
        vendor, product = forms[0]
        meta = ar_meta.get(key)

        def build(off: int, mask: int, flags: int) -> None:
            """Append one row per exact spelling of this key."""
            for form_vendor, form_product in forms:
                r = Row(form_vendor, form_product, off)
                r.sources = mask
                r.flags = flags
                # AR's figures describe the value AR actually holds, nothing else.
                if meta is not None and mask & SRC_AR:
                    r.subs = min(meta["submissions"], 0xFFFF)
                    r.pct = meta["agree_pct"] or 0
                rows.append(r)

        if len(values) == 1:
            off, mask = next(iter(values.items()))
            build(off, mask, 0)
            continue

        stats["conflicting_keys"] += 1
        best_n = max(bin(m).count("1") for m in values.values())
        winners = [off for off, m in values.items() if bin(m).count("1") == best_n]

        if best_n >= 2 and len(winners) == 1:
            off = winners[0]
            build(off, values[off], F_ADJUDICATED)
            stats["adjudicated"] += 1
            print(
                f"  adjudicated {vendor!r} {product!r}: {sorted(values)} -> {off:+d} "
                f"({src_names(values[off])}"
                + (
                    f", AR {meta['submissions']} subs {meta['agree_pct']}% agree"
                    if meta and values[off] & SRC_AR
                    else ""
                )
                + ")"
            )
        else:
            for off, mask in sorted(values.items()):
                build(off, mask, F_CONFLICT)
            stats["unresolved_conflicts"] += 1
            print(
                f"  UNRESOLVED {vendor!r} {product!r}: "
                + ", ".join(f"{o:+d} ({src_names(m)})" for o, m in sorted(values.items()))
            )

    rows.sort(key=Row.sort_key)
    return rows, stats


def emit(rows: list[Row], out: Path, stats: dict) -> None:
    def c_str(s: str) -> str:
        return '"' + s.replace("\\", "\\\\").replace('"', '\\"') + '"'

    lines = [
        "/* Drive read-offset table — generated by tools/gen_offsets.py; do not edit.",
        " *",
        " * Sources, both factual user-submitted measurement data:",
        " *   REDUMP Disc Preservation Project (https://redump.org), via redumper",
        " *   AccurateRip drive offset list (http://www.accuraterip.com/driveoffsets.htm)",
        " *",
        " * Columns: vendor, product, read_offset, ar_submissions, ar_agree_pct,",
        " * sources bitmask, flags. Offsets are in SAMPLES, where one sample is one",
        " * stereo frame of 4 bytes (588 per sector) — REDUMP's unit and AccurateRip's.",
        " *",
        f" * Entries: {len(rows)}",
        " */",
        "",
    ]
    for r in rows:
        lines.append(
            f"    {{ {c_str(r.vendor)}, {c_str(r.product)}, {r.read:+d}, "
            f"{r.subs}, {r.pct}, {r.sources}, {r.flags} }},"
        )
    out.write_text("\n".join(lines) + "\n", encoding="utf-8")

    print(f"\n{out}: {len(rows)} entries")
    for k in sorted(stats):
        print(f"  {k:26s} {stats[k]}")
    for label, mask in (("REDUMP", SRC_REDUMP), ("AccurateRip", SRC_AR)):
        print(f"  rows held by {label:12s} {sum(1 for r in rows if r.sources & mask)}")
    print(f"  rows held by BOTH          {sum(1 for r in rows if r.sources == (SRC_REDUMP | SRC_AR))}")
    print(f"  rows flagged CONFLICT      {sum(1 for r in rows if r.flags & F_CONFLICT)}")


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--redump", type=Path, required=True)
    ap.add_argument("--ar", type=Path, required=True)
    ap.add_argument("--out", type=Path, required=True)
    args = ap.parse_args()

    print("merging:")
    rows, stats = merge(read_redump(args.redump), read_ar(args.ar))
    emit(rows, args.out, stats)
    return 0


if __name__ == "__main__":
    sys.exit(main())
