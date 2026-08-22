#!/usr/bin/env python3
"""Compile the drive read-offset table from AccurateRip, live and as REDUMP froze it.

    REDUMP  private/code/redumper/offsets.ixx      read offsets, INQUIRY-keyed
    PROV    redumper's driveoffsets.txt (2022)     the same rows, WITH counts
    AR      tools/fetch_ar_offsets.py --out ...    read offsets + confidence

THESE ARE NOT TWO COLLECTIONS. Established 2026-08-22 and not to be re-derived:
REDUMP's offset table is AccurateRip's published list, imported once in 2022 and
frozen. Set-compared row for row — 4595 each way, ZERO rows in either that the
other lacks — the only transformation being AccurateRip's marketing vendor names
rewritten to INQUIRY ones (LG Electronics -> HL-DT-ST, Panasonic -> Matshita,
Lite-On -> JLMS). redumper's own history shows it: `driveoffsets.txt` is in
AccurateRip's four-column schema, carries AccurateRip's [Purged] markers, was
imported in one commit and never grew a row. So where the two disagree it is one
source disagreeing with its own earlier draft — see apply_retractions().

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

    alias OFF   shared 3501   REDUMP-only 1086   AR-only 1298
    alias ON    shared 4564   REDUMP-only   23   AR-only  235

(Recomputed 2026-08-22 against the shipped FIVE-entry alias list. The figures
this docstring carried until then — 4554/33/245 — were measured before JLMS and
CENDYNE_ were added; those two account for exactly the 33 -> 23 shift, 9 rows
and 1. The list before that read 4526/63/276, from a two-alias configuration
that predated FREECOM_. A residual difference of two keys between the 2026-08-16
run and this one is still unexplained and immaterial to every conclusion below,
but it is not zero and is recorded rather than rounded away.

The remaining 23 REDUMP-only keys are not drives AccurateRip lacks: they are the
rows a whole-field rewrite cannot reach, where AccurateRip prints the vendor run
into the product — "SATA LG ELECTRONICSBD-RE B", "PanasonicBD-CMB U". They are
listed by name whenever the generator runs.)

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

# Fraction of REDUMP rows allowed to miss the provenance join before the run is
# refused. See apply_retractions() for why a ceiling exists at all.
UNKNOWN_PROVENANCE_CEILING = 0.05

# EXACT whole-field vendor rewrites, applied to BOTH sources before keying.
# Reviewed by hand, one line per decision, never inferred — see the module
# docstring for the measurement behind each and for why similarity matching is
# refused. Left side and right side are both uppercase, whitespace-collapsed.
VENDOR_ALIAS = {
    "HL-DT-ST": "LG ELECTRONICS",  # Hitachi-LG Data Storage; 649/650 match, 0 differ
    "MATSHITA": "PANASONIC",       # Matsushita; 375/375 match, 0 differ
    "FREECOM_": "FREECOM",         # AccurateRip trailing underscore
    "CENDYNE_": "CENDYNE",         # AccurateRip trailing underscore; 1/1 match, 0 differ
    "JLMS": "LITE-ON",             # Lite-On's INQUIRY string; 9/9 match, 0 differ
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

    Case folding matches the runtime, which folds too (adsc_inquiry_normalize
    in src/drive/offsets.c) — vendors are not consistent about capitalisation
    and "AOpen"/"AOPEN" are one company. Measured before adopting it: of 5888
    emitted rows, ZERO pairs differed only by case, so folding collides nothing.

    ALIASING is the part that stays build-time-only, and the distinction is not
    cosmetic. Case is a spelling of one string; an alias asserts that two
    DIFFERENT strings name one company, which is a human judgement (see
    VENDOR_ALIAS). The runtime must never make it, so this tool emits every
    aliased spelling it saw as its own row and the lookup matches literally.
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

    RESOLUTION IS NOT SELECTION WHEN THE ROWS AGREE. Most duplicates do agree:
    of 75 duplicated names, 66 carry the SAME offset in every row. Keeping only
    the largest of those throws away real measurements — 930 submissions, 4.9%
    of the total across those keys. `DVD RW` is listed at 298 and 267, both +6,
    and shipped as 298 when 565 people measured it. The counts are not rival
    claims there; they are one claim, reported twice.

    So rows are POOLED BY (key, offset) first — submissions summed, agreement
    taken as the submission-weighted mean, which is the only average that keeps
    "percent of submissions agreeing" meaning what it says — and only then does
    the largest pool win the key. Selection between DIFFERENT offsets is
    unchanged and still by count; it just now compares pooled totals rather than
    whichever single row happened to be biggest.

    Why the counts are duplicated at all is unknown. AccurateRip does not
    normalise near-identical names (SIimtype survives beside Slimtype), and the
    pairs are not always lopsided — 298|267, 136|197|12 — so the display name is
    not its key. Firmware revision is the obvious candidate and is NOT
    established. Pooling is right either way: whatever distinguishes the rows,
    it is not something a drive reports in the two fields we can match on.
    """
    rows = json.loads(path.read_text(encoding="utf-8"))["rows"]

    # key -> offset -> [submissions, submissions*pct, first row seen, largest row]
    #
    # The LARGEST single row is carried alongside the sum on purpose: it is what
    # this function used to return, so it is the only honest baseline for saying
    # how much pooling recovers. Measured against the first row instead — which
    # is what a naive counter does — the same change reports 12955 submissions
    # rather than 933, a number that is arithmetically correct and describes a
    # behaviour nothing ever had.
    pools: dict[str, dict[int, list]] = defaultdict(dict)
    for r in rows:
        key = fold(r["vendor"], r["product"])
        pool = pools[key].get(r["offset"])
        if pool is None:
            pools[key][r["offset"]] = [r["submissions"],
                                       r["submissions"] * (r["agree_pct"] or 0),
                                       r, r["submissions"]]
        else:
            pool[0] += r["submissions"]
            pool[1] += r["submissions"] * (r["agree_pct"] or 0)
            pool[3] = max(pool[3], r["submissions"])

    pooled_rows, merged, recovered, contested = [], 0, 0, 0
    for key, by_offset in pools.items():
        for pool in by_offset.values():
            subs, weighted, first = pool[0], pool[1], pool[2]
            # Agreement as the submission-WEIGHTED mean: 193 rows at 100% and 4
            # at 75% pool to 99, not to a meaningless flat-average 87. A zero
            # count carries no weight, so it keeps whatever the row stated.
            pool[1] = round(weighted / subs) if subs else (first["agree_pct"] or 0)

        winner_off, winner = max(by_offset.items(), key=lambda kv: kv[1][0])
        losers = [(o, p) for o, p in by_offset.items() if o != winner_off]
        ties = [o for o, p in losers if p[0] == winner[0]]

        row = dict(winner[2])
        if winner[0] != winner[3]:
            merged += 1
            recovered += winner[0] - winner[3]
        row["submissions"] = winner[0]
        row["agree_pct"] = winner[1]
        pooled_rows.append(row)

        if losers:
            contested += 1
            for o, p in sorted(losers):
                print(f"  AR duplicate {row['vendor']!r} {row['product']!r}: "
                      f"{o:+d}/{p[0]} subs discarded for "
                      f"{winner_off:+d}/{winner[0]} subs")
            if ties:
                print(f"  AR TIE {row['vendor']!r} {row['product']!r}: "
                      f"{winner_off:+d} vs "
                      + ", ".join(f"{o:+d}" for o in sorted(ties))
                      + f", all {winner[0]} subs — keeping the first")

    print(f"  AccurateRip: {len(rows)} rows -> {len(pooled_rows)} keys; "
          f"{merged} key(s) pooled agreeing duplicates, recovering {recovered} "
          f"submission(s) over keeping the largest row alone; "
          f"{contested} key(s) had rival offsets")
    return pooled_rows


def read_provenance(path: Path) -> dict[str, dict[int, tuple[int, int]]]:
    """AccurateRip's list AS REDUMP IMPORTED IT, with the columns that import dropped.

    WHY THIS FILE EXISTS. REDUMP's offset table is not a second measurement of the
    same drives — it is AccurateRip's published list, imported once and frozen.
    Verified by set comparison of redumper's `driveoffsets.txt` (its 2022 import,
    deleted in redumper commit 15f369e) against the `offsets.ixx` generated from
    it: 4595 rows each way, ZERO rows in either that are not in the other, the
    only transformation being marketing vendor names rewritten to INQUIRY ones
    (LG Electronics -> HL-DT-ST, Panasonic -> Matshita, Lite-On -> JLMS).

    The imported file carries FOUR tab-separated columns —

        TEAC - DW-224E-CN\t+120\t2\t50%

    name, offset, submissions, agreement — and redumper's `generate_offsets.cc`
    read only the first two. The submission count and the agreement percentage,
    the two figures that say whether a row is worth anything, were discarded at
    import. Recovering them is the whole point of this input: it is what lets a
    REDUMP row be told apart from a REDUMP row AccurateRip has since withdrawn.

    Keyed with fold(), the same key the merge uses, so the vendor aliases apply
    on both sides — and PER VALUE within that key, not one row per key. The 2022
    list has the same duplicate-name rows the live one does, so a {name: row}
    dict would keep whichever the page printed last and silently mislabel the
    other value as having no provenance. That is the same trap read_ar()
    documents, and it was rebuilt here once before being caught.
    """
    prov: dict[str, dict[int, tuple[int, int]]] = {}
    purged = 0
    for line in path.read_text(encoding="utf-8", errors="replace").splitlines():
        if not line.strip():
            continue
        f = line.split("\t")
        if len(f) < 2:
            continue
        if f[1] == "[Purged]":
            # AccurateRip's own withdrawal marker. redumper's importer skipped
            # these too, so they are already absent from the REDUMP table and
            # are counted here only so the total reconciles.
            purged += 1
            continue
        if len(f) < 4:
            continue
        # "VENDOR - PRODUCT", or "- PRODUCT" where the drive reports no vendor.
        # Split on the FIRST separator only: product names contain " - " too.
        name = f[0]
        if name.startswith("- "):
            vendor, product = "", name[2:]
        elif " - " in name:
            vendor, product = name.split(" - ", 1)
        else:
            vendor, product = "", name
        off, subs, pct = int(f[1]), int(f[2]), int(f[3].rstrip("%"))
        by_value = prov.setdefault(fold(vendor, product), {})
        # Same name AND same offset twice: keep the larger count, matching
        # read_ar(). Different offsets both stay — that is the point of the map.
        if off not in by_value or subs > by_value[off][0]:
            by_value[off] = (subs, pct)
    if not prov:
        sys.exit(f"no provenance rows parsed from {path}")
    values = sum(len(v) for v in prov.values())
    print(f"  provenance: {len(prov)} keys / {values} values from the 2022"
          f" AccurateRip import ({purged} already [Purged] there)")
    return prov


def apply_retractions(redump, prov, ar, stats):
    """Drop REDUMP values AccurateRip has since withdrawn or superseded.

    REDUMP is AccurateRip's list at an earlier date (see read_provenance), so a
    REDUMP value that disagrees with AccurateRip is not a second opinion — it is
    the same source's earlier draft, quoted back at it. Two arms, kept apart
    because they are withdrawn in different ways:

      RETRACTED  the name was in the 2022 import and is absent from the live
                 list. AccurateRip removed the entry.
      SUPERSEDED the name is still live and the offset has changed. Measured on
                 this corpus, all eight are lopsided the same way, live against
                 2022: 1065/3, 652/3, 554/1, 477/7, 263/6, 198/2, 23/2, 7/2.

    NO AGREEMENT THRESHOLD. Absence is the signal; the agreement percentage is
    recorded for the reader and never gates, because it would keep
    `Philips DVD-ROM PCDV632` (+116, one submission, 100% agreement, and gone)
    whose only fault is that AccurateRip withdrew it.

    A row whose name does not join the 2022 import is UNKNOWN PROVENANCE, not
    "keep": it is kept, but counted and listed, because a join that quietly
    resolves everything by letting misses fall through to the safe branch is a
    guard that measures its own scope. The glued-vendor rows are the expected
    residue — AccurateRip prints `SATA LG ELECTRONICSBD-RE B` with the vendor
    run into the product, so no whole-field alias can reach it.
    """
    live = {fold(a["vendor"], a["product"]): a for a in ar}
    kept, dropped, unjoined = [], [], []

    for vendor, product, off in redump:
        key = fold(vendor, product)
        # The provenance must describe THIS value, not merely this drive: a key
        # the 2022 list held twice describes each of its values separately.
        snap = prov.get(key, {}).get(off)
        if snap is None:
            unjoined.append((vendor, product, off))
            kept.append((vendor, product, off))
            continue
        snap_subs, snap_pct = snap
        a = live.get(key)
        if a is None:
            dropped.append(("RETRACTED", vendor, product, off, snap_subs, snap_pct, None))
        elif a["offset"] != off:
            dropped.append(("SUPERSEDED", vendor, product, off, snap_subs, snap_pct, a))
        else:
            kept.append((vendor, product, off))

    stats["redump_retracted"] = sum(1 for d in dropped if d[0] == "RETRACTED")
    stats["redump_superseded"] = sum(1 for d in dropped if d[0] == "SUPERSEDED")
    stats["redump_unknown_provenance"] = len(unjoined)

    # THE FLOOR. Making --redump-provenance required stops the flag being
    # forgotten and nothing else: a TRUNCATED or wrong-format file parses
    # perfectly, yields a small prov map, sends every unmatched row down the
    # "unknown provenance, keep it" branch, and emits the pre-rule table with a
    # zero exit status. Measured: 100 lines of the real file produce 4531
    # unknowns, 0 retractions, and a table byte-identical to the unfiltered one.
    #
    # So the rule has to assert its own reach. 15 of 4595 rows (0.33%) fail to
    # join against the correct input, all of them the glued-vendor residue; the
    # ceiling here is fifteen times that, which no legitimate corpus drift
    # approaches and which truncation exceeds by orders of magnitude.
    rate = len(unjoined) / max(len(redump), 1)
    print(f"  provenance reach: {len(redump) - len(unjoined)}/{len(redump)} rows"
          f" joined ({rate * 100:.2f}% unknown)")
    if rate > UNKNOWN_PROVENANCE_CEILING:
        sys.exit(
            f"provenance covers too little of the REDUMP table: {len(unjoined)} of "
            f"{len(redump)} rows ({rate * 100:.1f}%) do not join, ceiling is "
            f"{UNKNOWN_PROVENANCE_CEILING * 100:.0f}%.\n"
            "The retraction rule cannot run on a partial import — refusing to emit "
            "a table that would look correct and carry withdrawn rows.\n"
            "Recover the full file with: "
            "git -C <redumper> show 15f369e^:driveoffsets.txt"
        )

    for reason, vendor, product, off, subs, pct, a in sorted(dropped, key=lambda d: (d[0], d[1], d[2])):
        detail = (f"AccurateRip now holds {a['offset']:+d} on {a['submissions']} subs"
                  if a else "absent from the live AccurateRip list")
        print(f"  {reason} {vendor!r} {product!r}: {off:+d} "
              f"(2022: {subs} subs {pct}% agree) — {detail}")
    if unjoined:
        print(f"  {len(unjoined)} REDUMP row(s) of UNKNOWN PROVENANCE — kept, rule not applied:")
        for vendor, product, off in sorted(unjoined):
            print(f"    ? {vendor!r} {product!r} {off:+d}")
    return kept, dropped


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

    # EVERY distinct spelling of a key is kept, not just the first seen. Case is
    # NOT a distinction here — both this key and the runtime fold it — but the
    # vendor/product SPLIT and the alias are, so ("", "DVDROM GO-D1600B") and
    # ("DVDROM", "GO-D1600B") remain two rows: a drive reporting either form has
    # to match. Aliasing makes this load-bearing rather than incidental —
    # HL-DT-ST and LG ELECTRONICS are one key here and two distinct INQUIRY
    # strings in the field, and BOTH must be present for either drive to match.
    #
    # Stored upper-cased so the emitted table is already in the runtime's
    # comparison form; the set then collapses spellings that differed only by
    # case, of which this corpus currently has none.
    spellings: dict[str, set[tuple[str, str]]] = defaultdict(set)

    for vendor, product, off in redump:
        key = fold(vendor, product)
        claims[key][off] |= SRC_REDUMP
        spellings[key].add((norm(vendor).upper(), norm(product).upper()))

    for a in ar:
        key = fold(a["vendor"], a["product"])
        claims[key][a["offset"]] |= SRC_AR
        spellings[key].add((norm(a["vendor"]).upper(), norm(a["product"]).upper()))
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


def emit(rows: list[Row], out: Path, stats: dict, dropped: list) -> None:
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

    # THE WITHDRAWN ROWS, NAMED. They are absent from the table below, and this
    # is the only place that says so — generator output scrolls away, this file
    # is committed and shows up in every diff. REDUMP is AccurateRip's list at an
    # earlier date, so a REDUMP value AccurateRip has since changed or removed is
    # that source's own withdrawn draft, not a second opinion to report as a
    # conflict. See apply_retractions() in tools/gen_offsets.py.
    if dropped:
        block = [
            "/* WITHDRAWN BY THE SOURCE — dropped from the table above, listed so the",
            " * deletion is reviewable rather than invisible.",
            " *",
            " *   RETRACTED  in REDUMP's 2022 AccurateRip import, absent from the live",
            " *              AccurateRip list: AccurateRip removed the entry.",
            " *   SUPERSEDED still listed, different offset now: AccurateRip corrected it.",
            " *",
            " * The 2022 figures are the submission count and agreement percentage that",
            " * REDUMP's importer discarded; recovered from redumper's git history.",
            " */",
        ]
        for reason, vendor, product, off, subs, pct, a in sorted(
            dropped, key=lambda d: (d[0], d[1], d[2])
        ):
            now = (f"AccurateRip now {a['offset']:+d} / {a['submissions']} subs"
                   if a else "gone from AccurateRip")
            block.append(
                f"/* {reason:10s} {vendor} {product}: {off:+d} "
                f"(2022: {subs} subs {pct}% agree) -> {now} */"
            )
        lines.extend(block + [""])
    for r in rows:
        lines.append(
            f"    {{ {c_str(r.vendor)}, {c_str(r.product)}, {r.read:+d}, "
            f"{r.subs}, {r.pct}, {r.sources}, {r.flags} }},"
        )
    out.write_text("\n".join(lines) + "\n", encoding="utf-8")

    print(f"\n{out}: {len(rows)} entries")
    # Zero counters are printed, not omitted: a missing line reads as "this
    # generator does not count that", which is exactly the wrong conclusion to
    # invite about conflicts once the retraction rule has removed them all.
    for k in ("conflicting_keys", "adjudicated", "unresolved_conflicts",
              "redump_retracted", "redump_superseded",
              "redump_unknown_provenance"):
        print(f"  {k:26s} {stats.get(k, 0)}")
    for k in sorted(set(stats) - {"conflicting_keys", "adjudicated",
                                  "unresolved_conflicts", "redump_retracted",
                                  "redump_superseded", "redump_unknown_provenance"}):
        print(f"  {k:26s} {stats[k]}")
    for label, mask in (("REDUMP", SRC_REDUMP), ("AccurateRip", SRC_AR)):
        print(f"  rows held by {label:12s} {sum(1 for r in rows if r.sources & mask)}")
    print(f"  rows held by BOTH          {sum(1 for r in rows if r.sources == (SRC_REDUMP | SRC_AR))}")
    print(f"  rows flagged CONFLICT      {sum(1 for r in rows if r.flags & F_CONFLICT)}")


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--redump", type=Path, required=True)
    ap.add_argument("--ar", type=Path, required=True)
    ap.add_argument(
        "--redump-provenance",
        type=Path,
        required=True,
        help="redumper's driveoffsets.txt, its 2022 AccurateRip import, with the "
             "submission and agreement columns its own importer discarded. "
             "Recover it with: git -C <redumper> show 15f369e^:driveoffsets.txt. "
             "REQUIRED, not optional: without it the retraction rule would not "
             "run and the table would look identical, which is the silent kind "
             "of wrong this generator exists to avoid.",
    )
    ap.add_argument("--out", type=Path, required=True)
    args = ap.parse_args()

    print("merging:")
    redump = read_redump(args.redump)
    prov = read_provenance(args.redump_provenance)
    ar = read_ar(args.ar)

    retractions: dict = {}
    kept, dropped = apply_retractions(redump, prov, ar, retractions)

    rows, stats = merge(kept, ar)
    stats.update(retractions)
    emit(rows, args.out, stats, dropped)
    return 0


if __name__ == "__main__":
    sys.exit(main())
