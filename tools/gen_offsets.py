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
(accudisc_write_offset_signal / _locate) rather than a table.

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
F_GENERIC = 8

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

# EXACT whole-key REBADGE mappings: a rebadged drive to the OEM drive it IS.
# Both sides in fold() form — aliased, upper-cased, whitespace- and
# underscore-collapsed. One line per HUMAN decision, with the source that
# justifies it, and never a similarity match: "these two strings look alike" is
# not evidence, and name-shape reasoning has already been wrong three times on
# this very corpus (see docs/reference/TODO.md).
#
# WHAT IT IS FOR. A rebadged drive reports the REBADGE string over INQUIRY, so a
# row dropped under that string strands its owner even when we hold the right
# offset under the OEM name. This table is consulted ONLY where a row is about to
# be dropped as RETRACTED, and only to KEEP it.
#
# THE GUARD, and it is the whole of the rule: RESCUE ONLY, NEVER SYNTHESISE. An
# entry may keep a row that EXISTS in the corpus. It may never bring a row into
# being for a rebadge nobody submitted — the same source names `Memorex MD6032`
# and `DVD-632` as SD-M1212 rebadges and neither is in either corpus, so emitting
# them would publish an offset nobody measured under a name nobody reported. That
# is not left to the reviewer to remember: assert_every_name_survives() refuses
# any row that corresponds to no input spelling, so a synthesising entry aborts
# the run.
#
# It is also applied ONLY WHEN THE TWO ROWS AGREE on the offset. A disagreement
# means the mapping or the data is wrong; it is reported and NOT applied.
# SUPERSEDED rows are never rescued — there AccurateRip corrected the value
# rather than removing the name, and republishing it would restore a number the
# publisher has replaced.
#
# NOT a rescue by "some live row shares this offset" — measured and useless:
# +116 has 43 live rows, +6 has 1888 rows across 206302 submissions. The link is
# the rebadge, which is human knowledge and is not in the numbers.
REBADGE = {
    # "Philips PCDV632 (6X/32X) (Known firmware : 1P16) Toshiba SD-M1212 OEM
    # drive RPC-1" — archive.rpc1.org/farzeno/club-internet/dvd/dvdfi.htm.
    # Corroborated by the sibling the relation predicts and that needs no rescue:
    # PHILIPS PCA532 (+116, 1 sub) is a Toshiba SD-M1202 rebadge, SD-M1202 is
    # +116 on 19 subs, and both are live and already ship.
    "PHILIPS DVD-ROM PCDV632": "TOSHIBA DVD-ROM SD-M1212",
}

# PRODUCT STRINGS TOO GENERIC TO IDENTIFY A DRIVE ON THEIR OWN.
#
# Since 0.15.0 the lookup keys on the product and the vendor only narrows, which
# is what lets a drive whose vendor string nobody submitted still be found. The
# cost is that a product string naming a CATEGORY rather than a model answers for
# any vendor at all. Most such strings protect themselves by colliding — "CD-ROM"
# is held at four different offsets and comes back ERR_AMBIGUOUS — but a generic
# name that only ONE submitter ever sent has nothing to collide with, and hands
# back that one drive's offset with the confidence of an exact match.
#
# Six bare CATEGORY WORDS are the whole of that set in the current corpus, found
# by matching them against the emitted product column; every one is a single row,
# and no other bare-generic name is present:
#
#     DVD            +48    FUJITSU     COMBO          +6     E-ELEI
#     DVDRW          +6     DEXPRESO    DVD+RW         +1292  ATAPI
#     OPTICAL DRIVE  +6     BUFFALO     CD-ROM DRIVE   +12    900 40X
#
# A SECOND CAUSE, THE SAME REMEDY. A product string can fail to identify a model
# without naming a category at all. The INQUIRY vendor field is EIGHT BYTES, and
# a drive whose name is longer simply continues into the product field, so what
# lands there is a FRAGMENT of a name rather than a name:
#
#     "DVDROM 8" + "X"  = DVDROM 8X        "DVDROM 1" + "0X" = DVDROM 10X
#
# `X` alone — one character — was answering for any vendor at all. Confirmed
# against AccurateRip's own file, which publishes the two fields separately and
# prints these as `DVDROM 8 - X` and `DVDROM 1 - 0X`, so the split is what the
# firmware reported rather than anything this tool inferred.
#
# There is no model to recover by rejoining them: the whole reported identity is
# a category plus a speed, unbranded. The rows nonetheless stay, because +564 is
# what that generation measures — 14 other rows hold it, among them HITACHI
# DVD-ROM GD-2500 on 48 submissions and SAMSUNG CD-ROM SN-124 on 48. The string
# names a class rather than a drive, and two drives reporting `DVDROM 8X` are
# indistinguishable to us in any case, so it remains the best available answer
# when the caller supplies the whole of it.
#
# THE BLOCK IS ON THE PRODUCT-ONLY PATH, NOT ON THE ROW, and the distinction is
# the whole design. `BUFFALO OPTICAL DRIVE` rests on 85 submissions — dropping it
# would throw away a real measurement to fix a matching rule, which is the
# opposite of "the tool should be genuinely useful, not ignore data without good
# justification". So the row ships, and answers whenever the caller's vendor
# narrows to it; what it may no longer do is answer for a vendor it has never
# been seen with.
#
# NOT EXTENDED TO EVERY FRAGMENT EITHER, and this is the harder of the two
# boundaries. Five more products are spill fragments that answer for any vendor:
#
#     -ROM             +12    "ATAPI CD" + "-ROM"            = ATAPI CD-ROM
#     -952E-AKV        +691   "E-IDE CD" + "-952E-AKV"       = E-IDE CD-952E-AKV
#     -956E-AKV        +691   "E-IDE CD" + "-956E-AKV"       = E-IDE CD-956E-AKV
#     -ROM JOYBEE610   +691   "BENQ DVD" + "-ROM JOYBEE610"  = BENQ DVD-ROM JOYBEE610
#     ROM DRIVE 50MAX  +12    "ATAPI CD" + "ROM DRIVE 50MAX" = ATAPI CDROM DRIVE 50MAX
#
# They stay reachable because each is still DISTINCTIVE: `-952E-AKV` carries a
# model number, and nothing but the drive it was cut from is going to report it,
# so answering on the product alone is answering for the right drive. `X` and
# `0X` are the opposite — one and two characters, matching by accident is the
# expected case, not the unlikely one. The line is drawn at "could another drive
# plausibly report this string?", not at "is this a fragment?", because being a
# fragment is a fact about the firmware and not by itself a hazard. A stated
# choice, not an oversight; if a real drive is ever found reporting one of the
# five, it moves.
#
# NOT extended to the generic names that DO collide. Those already refuse to pick
# — ERR_AMBIGUOUS with every candidate listed — and that is a different and
# safer failure than a confident wrong number, so blocking them would remove
# information without removing a hazard. A stated choice, not an oversight.
#
# EXACT whole-field, upper-cased, whitespace-collapsed, one line per human
# decision — the same contract as VENDOR_ALIAS and REBADGE. Never a pattern: a
# regex over product names is exactly the "nnXnnX rule" that was measured
# overzealous, because some drives really are called `16XDVD-ROM-AMH`.
GENERIC_PRODUCTS = frozenset({
    "DVD",
    "DVDRW",
    "DVD+RW",
    "COMBO",
    "OPTICAL DRIVE",
    "CD-ROM DRIVE",
    # Fragments left by the eight-byte vendor field overflowing, not categories.
    "X",
    "0X",
})


def src_names(mask: int) -> str:
    return "+".join(
        n for n, m in (("REDUMP", SRC_REDUMP), ("AR", SRC_AR)) if mask & m
    ) or "none"


# ONE DRIVE, SEVERAL NAMES THE FOLD CANNOT REACH.
#
# VENDOR_ALIAS rewrites a whole vendor field; the underscore fold reaches a
# punctuation difference. Neither reaches a drive sold under different BADGES
# with the product spelled differently under each — Lenovo's Ultraslim DVD is
# listed by AccurateRip four ways:
#
#     Lenovo   - Ultraslim DVD        +6   44 subs
#     lenovo   - UltraslimDVD         +6   21 subs
#     think    - plusUltraslimDVD     +6   20 subs   (ThinkPlus, Lenovo's brand)
#     ThinkPad - Ultraslim DVD        +6  339 subs   (pooled from 2 + 337)
#
# Four keys, one drive, 424 submissions between them — and a caller querying the
# second was told 21. The offset is not in doubt here, so what the alias buys is
# an honest confidence figure; where a family DISAGREES it buys more than that,
# because read_ar()'s rival-offset resolution then lets the better-evidenced
# value win instead of a one-submission row answering on its own authority.
#
# EXACT WHOLE-KEY, POST-FOLD, one line per human decision — the same contract as
# VENDOR_ALIAS, REBADGE and GENERIC_PRODUCTS, and for the same reason. A rule
# was measured and REJECTED: squashing spacing and punctuation collapses 146
# groups in this corpus, of which 132 agree on the offset and 14 DO NOT. Some of
# the 14 are two genuinely different drives ("MAD DOG 56X CDROM" +691 beside a
# bare "56X CD-ROM" +12), so a sweep would assert identities the data refuses.
# Each entry here is a claim that two strings name ONE drive, and has to be
# defended as one.
#
# The keys are fold() OUTPUT — upper-cased, whitespace-collapsed, vendor-aliased,
# underscores already spaces. The value is the canonical key, chosen as the
# best-evidenced spelling so the merge reads naturally; it is a join key only and
# never reaches the table, which still emits EVERY spelling as its own row.
KEY_ALIAS = {
    # ONE DRIVE, FOUR NAMES — evidence split, offset never in doubt (all +6).
    "LENOVO ULTRASLIM DVD":   "THINKPAD ULTRASLIM DVD",
    "LENOVO ULTRASLIMDVD":    "THINKPAD ULTRASLIM DVD",
    "THINK PLUSULTRASLIMDVD": "THINKPAD ULTRASLIM DVD",

    # ONE SUBMISSION CONTRADICTING HUNDREDS. Same vendor, same model number, one
    # punctuation mark apart, and the minority row ANSWERS — nothing collides, so
    # a drive reporting the losing spelling was handed a wrong offset at exit 0.
    # Merging lets read_ar()'s rival-offset resolution pick the evidenced value.
    #
    # The two GH24NS95/GSA-E60L lines point OPPOSITE WAYS — hyphenated is the
    # minority in one and the majority in the other — which is the concrete
    # reason this is a reviewed list and not a "strip the hyphen" rule.
    "LG ELECTRONICS DVD-RAM GH24NS95": "LG ELECTRONICS DVDRAM GH24NS95",
    "LG ELECTRONICS DVDRAM- GP65NB60": "LG ELECTRONICS DVDRAM GP65NB60",
    "LG ELECTRONICS DVDRAM-GP65NB60":  "LG ELECTRONICS DVDRAM GP65NB60",
    "LG ELECTRONICS DVDRAM GSA-E60L":  "LG ELECTRONICS DVD-RAM GSA-E60L",
    "TSSTCORP CDDVDW SE -218GN":       "TSSTCORP CDDVDW SE-218GN",

    # AGREEING SPELLINGS, so nothing changes but the reported evidence. Both HP
    # spellings of DT30N are +103; the +102 in that family is HL-DT-ST's, a real
    # vendor difference the narrowing already handles and NOT aliased here. Both
    # "ATAPI CD" spellings are +12 and are one drive, ATAPI CD-ROM, cut across
    # the eight-byte vendor field; "16X DVD-" + "ROM" is a different drive and
    # deliberately stays its own key.
    "HP DVDROM DT30N":  "HP DVD-ROM DT30N",
    "ATAPI CD -ROM":    "ATAPI CD ROM",
}


def repair_ar_trailing_separator(rows: list[dict]) -> int:
    """"LG Electronics -" is a VENDOR with an empty product, not a product.

    AccurateRip publishes such rows — that one is on the live page, confirmed
    2026-08-25. fetch_ar_offsets.py now splits them correctly, but the split is
    baked into ar_offsets.json AT FETCH TIME, so the fixed parser changes nothing
    until someone re-fetches over the network. This repairs what is on disk.

    NOT the two-parsers trap documented in read_provenance(). That trap is two
    implementations of one rule drifting apart; this is ONE rule, called from
    both places that need it — read_ar(), and assert_every_name_survives(), whose
    reference must describe the same input read_ar() consumed or it would report
    the repair as a lost name and an invented one. Self-limiting: after a
    re-fetch nothing matches and the reported count goes to zero.
    """
    repaired = 0
    for r in rows:
        if not r["vendor"] and r["product"].endswith(" -"):
            r["vendor"], r["product"] = r["product"][:-2].strip(), ""
            repaired += 1
    return repaired


def norm(s: str) -> str:
    """Collapse whitespace runs and trim — the INQUIRY rule, minus case folding.

    Mirrors adsc_inquiry_normalize() in src/drive/offsets.c. Drives pad the
    fixed INQUIRY fields ("DVDR   PX-716A"), so the table and the lookup must
    agree on this or nothing matches.
    """
    return " ".join(s.split())


def fold(vendor: str, product: str, alias: bool = True) -> str:
    """The build-time join key: aliased, case- and underscore-folded, collapsed.

    Case folding matches the runtime, which folds too (adsc_inquiry_normalize
    in src/drive/offsets.c) — vendors are not consistent about capitalisation
    and "AOpen"/"AOPEN" are one company. Measured before adopting it: of 5888
    emitted rows, ZERO pairs differed only by case, so folding collides nothing.

    UNDERSCORE FOLDING is the same kind of claim as case, not the same kind as
    aliasing. AccurateRip spells one drive both ways — `DVDRAM_GHA2N` beside
    `DVDRAM GHA2N`, `BD-RE_BT20N` beside `BD-RE BT20N` — and a separator is a
    spelling of a string, not a different drive. Measured over the union corpus
    before adopting it: of 4822 distinct keys, folding underscore to space merges
    26 groups / 52 keys, ALL 26 agreeing on the offset and ZERO disagreeing.
    Deleting the underscore instead merges only 2 groups, so substitution is the
    rule that matches how the source actually spells these names.

    It runs AFTER the alias lookup, which is what the measurement above assumed,
    and it subsumes the trailing-underscore vendors VENDOR_ALIAS had to name one
    at a time: `Generic_` joins its provenance row without an alias line, and
    FREECOM_/CENDYNE_ are now covered twice over.

    ALIASING is the part that stays build-time-only, and the distinction is not
    cosmetic. Case and separators are spellings of one string; an alias asserts
    that two DIFFERENT strings name one company, which is a human judgement (see
    VENDOR_ALIAS). The runtime must never make it, so this tool emits every
    aliased spelling it saw as its own row and the lookup matches literally.

    THE RUNTIME DOES NOT FOLD UNDERSCORES — adsc_inquiry_normalize touches case
    and whitespace only, so `DVDRAM_GHA2N` matches a literal `DVDRAM_GHA2N` row
    and nothing else. Every spelling this key pools MUST therefore still be
    emitted; merge() asserts exactly that before the table is written.
    """
    v = norm(vendor).upper()
    v = VENDOR_ALIAS.get(v, v)
    key = f"{v} {norm(product).upper()}".strip()
    key = " ".join(key.replace("_", " ").split())
    # LAST, so an alias is written in the form every other key already has —
    # upper-cased, collapsed, vendor-aliased, underscores folded. Writing one in
    # raw INQUIRY form would simply never match, and silently.
    #
    # alias=False returns the key BEFORE this step, which apply_retractions
    # needs: the retraction rule compares a name against AccurateRip's listing of
    # THAT NAME, and after aliasing it would be comparing against another drive's
    # merged value. See the ALIASED arm there.
    return KEY_ALIAS.get(key, key) if alias else key


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

    repaired = repair_ar_trailing_separator(rows)
    if repaired:
        print(f"  AR trailing-separator repaired in {repaired} row(s) — the "
              "json predates the fetcher fix; a re-fetch makes this 0")
    stranded = sorted(f"{r['vendor']!r} {r['product']!r}"
                      for r in rows if r["product"].endswith(" -"))
    if stranded:
        sys.exit(
            f"{path}: {len(stranded)} row(s) still carry a trailing separator "
            f"in the product — {stranded[:5]}. A vendor with no product is not "
            "a product whose name ends in a hyphen."
        )

    # key -> offset -> [submissions, submissions*pct, first row seen, largest row]
    #
    # The LARGEST single row is carried alongside the sum on purpose: it is what
    # this function used to return, so it is the only honest baseline for saying
    # how much pooling recovers. Measured against the first row instead — which
    # is what a naive counter does — the same change reports 12955 submissions
    # rather than 933, a number that is arithmetically correct and describes a
    # behaviour nothing ever had.
    pools: dict[str, dict[int, list]] = defaultdict(dict)

    # EVERY spelling that fed this key, across ALL of its offsets, carried
    # forward for merge() to emit. Pooling returns ONE row per key, so without
    # this the other spellings vanish between here and the table — and the
    # runtime matches literally, so a vanished spelling is a drive that gets
    # ERR_NOTFOUND. Not hypothetical: before the underscore fold existed this
    # already cost ("TSSTCORP", "CDDVDW"), which pooled into ("", "TSSTCORP
    # CDDVDW") and was absent from the shipped table. Collected over all offsets
    # rather than the winning one because a spelling is a name a drive reports,
    # not evidence for a value — it is emitted at whichever offset the key wins.
    forms: dict[str, set[tuple[str, str]]] = defaultdict(set)

    for r in rows:
        key = fold(r["vendor"], r["product"])
        forms[key].add((norm(r["vendor"]).upper(), norm(r["product"]).upper()))
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
        row["spellings"] = forms[key]
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
        elif name.endswith(" -"):
            # TRAILING SEPARATOR: a vendor with an EMPTY product, "DVDROM -".
            # Without this arm the name falls through to the no-separator branch
            # and keys as the PRODUCT "DVDROM -", which joins nothing — REDUMP
            # holds the same drive as ("DVDROM", ""). The provenance lookup then
            # misses, apply_retractions takes its unjoined branch and continues,
            # so the live-AR check NEVER RUNS and a row AccurateRip has since
            # withdrawn survives as "unknown provenance, rule not applied".
            #
            # Measured: exactly that happened to "DVDROM -" (+564, 2022: 1 sub,
            # absent from the live list). It shipped through 0.16.0 and was
            # harmless only because accudisc_offset_for_inquiry refuses an empty
            # product — luck, not design. A guard that cannot be reached by the
            # rows it is meant to catch is not a guard.
            #
            # Ordered AFTER the interior-separator arm deliberately: a name with
            # both ("A - B -") is a vendor plus a product whose name ends in a
            # hyphen, not an empty product.
            vendor, product = name[:-2], ""
        else:
            vendor, product = "", name
        off, subs, pct = int(f[1]), int(f[2]), int(f[3].rstrip("%"))
        by_value = prov.setdefault(fold(vendor, product), {})
        # Same name AND same offset twice: POOL them, matching read_ar(). This
        # used to keep the larger count, which matched read_ar() as it was
        # BEFORE 0.12.1 pooled instead — the two arms had quietly diverged, and
        # a provenance figure printed beside a withdrawn row is a claim about
        # how much evidence the 2022 list held, so understating it misleads
        # exactly the reader the note is written for.
        #
        # 53 (key, offset) pairs collide here. Only 24 come from the underscore
        # fold; the other 29 are names the 2022 file simply lists twice
        # ("ASUS - DRW-24B1ST" at +6 on 2 submissions AND on 686), so this is
        # not a hazard the fold introduced. No CURRENTLY withdrawn row is among
        # the 53, which is luck rather than design and is the reason to fix it
        # now rather than when a future corpus makes one of them wrong.
        prev = by_value.get(off)
        if prev is None:
            by_value[off] = (subs, pct)
        else:
            total = prev[0] + subs
            by_value[off] = (
                total,
                round((prev[0] * prev[1] + subs * pct) / total) if total else pct,
            )
    if not prov:
        sys.exit(f"no provenance rows parsed from {path}")
    # THE TRAILING-SEPARATOR GUARD. A key ending in " -" means the arm above was
    # removed or reordered and a "VENDOR -" row keyed as the PRODUCT "VENDOR -"
    # instead. That does not fail loudly on its own: it makes the key join
    # nothing, apply_retractions takes its unjoined branch, and a withdrawn row
    # is KEPT and merely counted as unknown provenance. Fatal rather than
    # reported, because the symptom is a row that should not be in the table.
    stranded = sorted(k for k in prov if k.endswith(" -"))
    if stranded:
        sys.exit(
            f"{path}: {len(stranded)} name(s) parsed with the separator left in "
            f"the key — {stranded[:5]}. A trailing ' -' is a vendor with an "
            "EMPTY product, not a product whose name ends in a hyphen."
        )
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
    kept, dropped, unjoined, rescued, aliased = [], [], [], [], []
    rebadge_used: set[str] = set()

    for vendor, product, off in redump:
        key = fold(vendor, product)
        raw = fold(vendor, product, alias=False)
        if raw != key:
            # ALIASED, so THE RETRACTION RULE DOES NOT APPLY — and this arm is
            # here because omitting it silently deleted three real spellings.
            #
            # The rule asks "does AccurateRip still list THIS NAME at THIS
            # value?". Once KEY_ALIAS has asserted that this name and another are
            # one drive, live[key] is the DRIVE's merged value rather than this
            # name's listing, so an offset mismatch is not AccurateRip
            # withdrawing anything — it is the alias doing what it was added to
            # do. Treating it as SUPERSEDED dropped the row, and with it the only
            # copy of a spelling AccurateRip files under a different vendor:
            # HL-DT-ST DVD-RAM GH24NS95, DVDRAM GSA-E60L and DVDRAM- GP65NB60 all
            # vanished, measured — and assert_every_name_survives could NOT see
            # it, because its reference is `kept` and a dropped row is not there.
            #
            # Keeping the row emits the SPELLING; the merged key supplies the
            # VALUE, so the drive answers with the well-evidenced offset under
            # every name it is known by. Listed rather than silent: an arm that
            # skips a guard must be visible in the run that skips it.
            aliased.append((vendor, product, off, key))
            kept.append((vendor, product, off))
            continue
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
            # THE REBADGE ARM. A retracted name that a reviewed entry says is a
            # rebadge of a live OEM drive is kept IF the two agree on the offset.
            # Every other outcome drops the row and says why, because an entry
            # that silently does nothing is an entry nobody has tested.
            oem_key = REBADGE.get(key)
            oem = live.get(oem_key) if oem_key else None
            if oem_key is not None:
                rebadge_used.add(key)
            if oem_key is None:
                dropped.append(("RETRACTED", vendor, product, off, snap_subs, snap_pct, None))
            elif oem is None:
                print(f"  REBADGE TARGET NOT LIVE {vendor!r} {product!r} -> "
                      f"{oem_key!r}: no such row in AccurateRip, mapping is stale "
                      "— NOT applied, row stays dropped")
                dropped.append(("RETRACTED", vendor, product, off, snap_subs, snap_pct, None))
            elif oem["offset"] != off:
                print(f"  REBADGE DISAGREES {vendor!r} {product!r}: {off:+d} against "
                      f"{oem_key!r} at {oem['offset']:+d} / {oem['submissions']} subs "
                      "— the mapping or the data is wrong, NOT applied, row stays dropped")
                dropped.append(("RETRACTED", vendor, product, off, snap_subs, snap_pct, None))
            else:
                kept.append((vendor, product, off))
                rescued.append((vendor, product, off, snap_subs, snap_pct, oem_key, oem))
                print(f"  RESCUED {vendor!r} {product!r}: {off:+d} — retracted by "
                      f"AccurateRip, kept because {oem_key!r} is the OEM drive it "
                      f"rebadges and carries {oem['offset']:+d} on "
                      f"{oem['submissions']} submissions")
        elif a["offset"] != off:
            dropped.append(("SUPERSEDED", vendor, product, off, snap_subs, snap_pct, a))
        else:
            kept.append((vendor, product, off))

    # NO ALIASED NAME MAY BE DROPPED. The arm above returns before the retract
    # and supersede arms, so this cannot fire while that arm exists — which is
    # the point: it fires the moment someone removes or reorders it.
    #
    # Written because nothing else could see the failure. Without the arm, three
    # REDUMP rows were dropped as SUPERSEDED and the ONLY copies of
    # HL-DT-ST DVD-RAM GH24NS95, DVDRAM GSA-E60L and DVDRAM- GP65NB60 left the
    # table — AccurateRip files those under "LG Electronics", so no AR spelling
    # replaced them. assert_every_name_survives() could not catch it: its
    # reference is `kept` plus the AR spellings, and a dropped row is in neither,
    # so the requirement shrank exactly as far as the table did. That is the
    # vacuous-guard shape this file has now hit three times.
    mis_dropped = sorted(
        (v, pr, o) for _, v, pr, o, *_ in dropped
        if fold(v, pr, alias=False) != fold(v, pr)
    )
    if mis_dropped:
        sys.exit(
            f"{len(mis_dropped)} KEY_ALIAS'd row(s) were dropped by the "
            f"retraction rule — {mis_dropped[:3]}. An aliased name is a SPELLING "
            "of a merged drive, not a value AccurateRip withdrew; dropping it "
            "deletes the only copy of a name no AccurateRip row carries."
        )

    # CONSERVATION. Every input row leaves here exactly once, kept or dropped.
    # This is not belt-and-braces: assert_every_name_survives() re-reads the AR
    # file for its reference but takes the REDUMP half from `kept`, which is THIS
    # function's own output — so a row lost here would shrink the requirement and
    # the table together and the coverage guard would stay silent. That is the
    # same shape as the vacuous first version of that guard, one function
    # upstream, and this closes it.
    if len(kept) + len(dropped) != len(redump):
        sys.exit(
            f"apply_retractions lost rows: {len(redump)} in, "
            f"{len(kept)} kept + {len(dropped)} dropped = {len(kept) + len(dropped)}"
        )

    # An entry that never fired is an entry nobody has tested. Reported rather
    # than fatal: AccurateRip reinstating a row would legitimately make one inert.
    for k in REBADGE:
        if k not in rebadge_used:
            print(f"  REBADGE ENTRY UNUSED {k!r} -> {REBADGE[k]!r}: nothing was "
                  "dropped under that name, so this line changed nothing")
    stats["redump_rescued_rebadge"] = len(rescued)

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
    if aliased:
        print(f"  {len(aliased)} REDUMP row(s) KEPT under a KEY_ALIAS — the "
              "retraction rule compares a name against its own listing and "
              "these names were merged into another:")
        for vendor, product, off, key in sorted(aliased):
            print(f"    ~ {vendor!r} {product!r} {off:+d} -> {key!r}")
    if unjoined:
        print(f"  {len(unjoined)} REDUMP row(s) of UNKNOWN PROVENANCE — kept, rule not applied:")
        for vendor, product, off in sorted(unjoined):
            print(f"    ? {vendor!r} {product!r} {off:+d}")
    return kept, dropped, rescued


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
    generic_hit: set[str] = set()

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
        # AN ALIASED ROW CONTRIBUTES ITS SPELLING, NOT A VALUE.
        #
        # REDUMP's table IS AccurateRip's list frozen in 2022, so a REDUMP row
        # under a name KEY_ALIAS has merged carries the SAME datum read_ar()
        # already weighed on the AccurateRip side — and, where they differ,
        # already DISCARDED: "DVD-RAM GH24NS95" +667 is the one submission that
        # lost to +6 on 1315. Letting it claim an offset here re-enters that
        # discarded row by the other door, and merge() then reads two identical
        # data as two sources disagreeing. Measured: 3 keys became
        # conflicting_keys and shipped ACCUDISC_OFFSET_F_ADJUDICATED, which tells
        # a caller "the sources disagreed and this is what most agreed on" about
        # a disagreement that only the alias created.
        #
        # The spelling still goes in, which is the whole point of keeping the row
        # in apply_retractions: the drive must answer under every name it is
        # known by. Its VALUE comes from the merged key.
        if fold(vendor, product, alias=False) == key:
            claims[key][off] |= SRC_REDUMP
        spellings[key].add((norm(vendor).upper(), norm(product).upper()))

    for a in ar:
        key = fold(a["vendor"], a["product"])
        claims[key][a["offset"]] |= SRC_AR
        # read_ar() returns one pooled row per key; a["spellings"] is every
        # spelling that fed it, which is more than the surviving row's own.
        spellings[key] |= a["spellings"]
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
                if form_product in GENERIC_PRODUCTS:
                    r.flags |= F_GENERIC
                    generic_hit.add(form_product)
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

    # An entry matching no row is an entry nobody has tested — and worse here
    # than in REBADGE, because this list exists to REMOVE reach: a name that has
    # left the corpus makes the list look like it is doing more than it is.
    # Reported rather than fatal, since a corpus refresh may legitimately drop
    # one of these drives.
    for g in sorted(GENERIC_PRODUCTS - generic_hit):
        print(f"  GENERIC ENTRY UNUSED {g!r}: no row carries that product, so "
              "this line changed nothing")
    stats["products_generic"] = len(generic_hit)

    rows.sort(key=Row.sort_key)
    return rows, stats


def assert_every_name_survives(rows: list[Row], kept: list, ar_path: Path) -> None:
    """No fold may collapse a NAME. Checked against a fresh read of the input.

    The runtime matches a literal string — adsc_inquiry_normalize folds case and
    whitespace and nothing else — so an input spelling this table does not carry
    is a drive we answer ERR_NOTFOUND for. Case, alias and underscore folding all
    pool EVIDENCE under one key; every spelling that fed that key still has to be
    emitted. This is the only place that claim is checked rather than asserted in
    a docstring, and it is not decorative: pooling silently cost
    ("TSSTCORP", "CDDVDW") before the underscore fold was ever proposed.

    IT RE-READS THE AR FILE ON PURPOSE. An earlier version of this check built
    its requirement from read_ar()'s own output, and was measured to be VACUOUS:
    breaking the spelling carry-through shrank the requirement and the table
    together, so the check passed on a table missing 25 names. A guard whose
    reference is derived from the thing it guards cannot distinguish the failure
    it exists to catch. The reference has to come from outside the pipeline.

    SCOPE is `kept` — the POST-retraction REDUMP list — plus every raw
    AccurateRip spelling. Using the raw REDUMP table instead would fire on the 16
    rows the retraction rule is meant to remove, and the natural response to a
    guard that cries wolf is to weaken it.

    IT CHECKS BOTH DIRECTIONS. Names may not be lost, and names may not be
    INVENTED — see the second block below for why the latter is load-bearing
    rather than tidy.
    """
    required = {(norm(v).upper(), norm(p).upper()) for v, p, _ in kept}
    ar_rows = json.loads(ar_path.read_text(encoding="utf-8"))["rows"]
    # The SAME repair read_ar() applied, from the same function. The reference
    # has to describe the input the pipeline consumed: a row this tool corrects
    # before reading is not a spelling the table owes an answer for, and leaving
    # it here would report the correction as BOTH a lost name and an invented
    # one. What the guard is testing is spelling carry-through through POOLING,
    # which this does not touch; the repair has its own fatal guard in read_ar().
    repair_ar_trailing_separator(ar_rows)
    for r in ar_rows:
        required.add((norm(r["vendor"]).upper(), norm(r["product"]).upper()))

    emitted = {(r.vendor, r.product) for r in rows}

    # THE OTHER DIRECTION, and it is not symmetry for its own sake. A table may
    # not carry a name no source reported: that would publish an offset nobody
    # measured under a name nobody submitted, which is the "researched data must
    # not share a field with reported data" rule. Today it holds because nothing
    # invents rows — but the reviewed REBADGE TABLE still to be built is exactly
    # a mechanism that COULD, and its stated condition is rescue-only. Asserting
    # it here makes that condition structural, so reviewing a rebadge line is
    # "is this mapping true?" and never "did this mapping synthesise anything?".
    extra = sorted(emitted - required)
    if extra:
        sys.exit(
            f"{len(extra)} row(s) in the table correspond to NO input spelling — "
            "something synthesised a name instead of rescuing one:\n"
            + "\n".join(f"    {v!r} {p!r}" for v, p in extra)
        )

    missing = sorted(required - emitted)
    if not missing:
        print(f"  name coverage: {len(required)} input spelling(s), "
              "all emitted, none invented")
    else:
        sys.exit(
            f"{len(missing)} input spelling(s) reached no row in the table — a "
            "fold collapsed a NAME instead of pooling evidence:\n"
            + "\n".join(f"    {v!r} {p!r}" for v, p in missing)
        )


def assert_rescues_are_corroborated(rows: list[Row], rescued: list) -> None:
    """A rescued row's OEM twin must be IN THIS TABLE, not merely live upstream.

    apply_retractions() checks the OEM row against AccurateRip's pooled map,
    which is the right test for whether the two AGREE. It is not the right test
    for the claim the shipped file then makes, which is that this offset is
    corroborated by a row of THIS table. Those come apart the moment anything
    downstream drops the OEM side — and the rescued row would survive as an
    orphan pointing at a corroboration that is no longer there, with nothing
    saying so.
    """
    by_key: dict[str, set[int]] = defaultdict(set)
    for r in rows:
        by_key[fold(r.vendor, r.product)].add(r.read)

    for vendor, product, off, _subs, _pct, oem_key, _oem in rescued:
        if off not in by_key.get(oem_key, set()):
            sys.exit(
                f"rescued {vendor!r} {product!r} at {off:+d} cites {oem_key!r} as "
                "its corroboration, but no row of the generated table carries that "
                f"name at that offset (table has {sorted(by_key.get(oem_key, set()))})"
            )
    if rescued:
        print(f"  rescue corroboration: {len(rescued)} rescued row(s), each "
              "matched by its OEM twin IN the generated table")


def emit(rows: list[Row], out: Path, stats: dict, dropped: list,
         rescued: list) -> None:
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

    # THE RESCUED ROWS, NAMED — and for a stronger reason than the withdrawn
    # ones. A rescued row emits as REDUMP-only with zero AccurateRip figures,
    # because attaching the OEM's submission count to it would claim that many
    # people measured a drive under a name AccurateRip does not list. So in the
    # table itself it is indistinguishable from any ordinary uncorroborated
    # REDUMP row, and the evidence it actually rests on lives ONLY here.
    if rescued:
        block = [
            "/* RESCUED BY REBADGE — present in the table above, though the source",
            " * withdrew the name. A rebadged drive reports the REBADGE string over",
            " * INQUIRY, so dropping its row strands its owner while we hold the same",
            " * measurement under the OEM name.",
            " *",
            " * Kept on a REVIEWED, CITED, EXACT mapping (REBADGE in",
            " * tools/gen_offsets.py) and only where the two rows AGREE on the offset.",
            " * The row itself ships as REDUMP-only with no AccurateRip figures — the",
            " * submissions below were made against the OEM name, not this one.",
            " *",
            " * STATE THE TRADE: this republishes a row AccurateRip took down, on a",
            " * human judgement. What it buys is a plausible offset for a rare drive,",
            " * corroborated by its OEM twin; what it costs is that the publisher's",
            " * withdrawal is overridden here. That is why each one is named.",
            " */",
        ]
        for vendor, product, off, subs, pct, oem_key, oem in sorted(rescued):
            block.append(
                f"/* RESCUED    {vendor} {product}: {off:+d} "
                f"(2022: {subs} subs {pct}% agree) -> rebadge of {oem_key}, "
                f"{oem['offset']:+d} on {oem['submissions']} subs */"
            )
        lines.extend(block + [""])

    # AMBIGUOUS UNDER THE SHIPPED LOOKUP, NAMED. Since 0.15.0 the runtime keys
    # on the PRODUCT and lets the vendor narrow, so ambiguity is a property of a
    # product rather than of a (vendor, product) row — and nothing in the row
    # format can express it. Listed here because it is the one place a reader can
    # see which products come back ERR_AMBIGUOUS without running the lookup.
    #
    # A product listed here is NOT a defect. Every one of these is resolved by a
    # vendor the table already holds; what the block records is which products
    # cannot answer on their own.
    prod: dict[str, dict[int, set[str]]] = defaultdict(lambda: defaultdict(set))
    for r in rows:
        prod[r.product][r.read].add(r.vendor)
    contested = {p: v for p, v in prod.items() if len(v) > 1}
    if contested:
        block = [
            "/* AMBIGUOUS ON THE PRODUCT ALONE — these return ERR_AMBIGUOUS to a",
            " * caller whose INQUIRY vendor matches none of the rows below. The",
            " * vendor resolves every one of them; the list is here so that which",
            " * products cannot stand alone is reviewable in a diff.",
            " *",
            " * NOT ALL OF THESE ARE PRODUCTS. The INQUIRY vendor field is",
            " * eight bytes and a longer name continues into the product field,",
            " * so what is printed here as a product may be a fragment. 'ROM'",
            " * is the tail of ATAPI CD-ROM and of 16X DVD-ROM — two different",
            " * drives, not one product held at two offsets. '16X' is a related",
            " * but distinct fault: there both fields are uninformative, a",
            " * category as the vendor and a speed rating as the product.",
            " *",
            " * The firmware is wrong in both cases and the corpus records it",
            " * faithfully. Each line below is still exact as a MATCHING rule,",
            " * which is all this block describes — it is the reading of them",
            " * as model names that does not hold.",
            " */",
        ]
        for prod_name in sorted(contested):
            for off in sorted(contested[prod_name]):
                vendors = ", ".join(
                    repr(v) if v else "(no vendor)"
                    for v in sorted(contested[prod_name][off])
                )
                block.append(f"/* AMBIGUOUS  {prod_name}: {off:+d} <- {vendors} */")
        lines.extend(block + [""])
        stats["products_ambiguous"] = len(contested)

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
              "redump_unknown_provenance", "redump_rescued_rebadge",
              "products_ambiguous", "products_generic"):
        print(f"  {k:26s} {stats.get(k, 0)}")
    for k in sorted(set(stats) - {"conflicting_keys", "adjudicated",
                                  "unresolved_conflicts", "redump_retracted",
                                  "redump_superseded", "redump_unknown_provenance",
                                  "redump_rescued_rebadge", "products_ambiguous",
                                  "products_generic"}):
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
    kept, dropped, rescued = apply_retractions(redump, prov, ar, retractions)

    rows, stats = merge(kept, ar)
    assert_every_name_survives(rows, kept, args.ar)
    assert_rescues_are_corroborated(rows, rescued)
    stats.update(retractions)
    emit(rows, args.out, stats, dropped, rescued)
    return 0


if __name__ == "__main__":
    sys.exit(main())
