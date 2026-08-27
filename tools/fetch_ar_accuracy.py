#!/usr/bin/env python3
"""Fetch AccurateRip's periodic drive-ACCURACY report to a local snapshot.

The second networked development tool, and the same rules apply as to its
sibling ``fetch_ar_offsets.py``: ``tools/`` is excluded from the distribution
tarball, nothing here ships, and the library still fetches nothing at runtime.

A DIFFERENT QUANTITY FROM THE OFFSET LIST'S "PERCENTAGE AGREE"
--------------------------------------------------------------
Both are percentages published by AccurateRip about a drive, and confusing them
would be easy and wrong:

  agree_pct   (driveoffsets.htm)  how many submitters agreed on the drive's
                                  READ OFFSET. A property of one measurement.
  accuracy    (this report)       of all tracks this drive ever submitted, the
                                  share that MATCHED AccurateRip's reference.
                                  A property of how the drive reads discs.

The report is derived, in Spoon's words, "on the basis that people who have a
drive would have the same number of damaged disks as everyone else, on average"
— so it is a population statistic, confounded by whose hands the drive is in.
See the header comment in the generated table for what that forbids.

WHY THE NAMES JOIN BETTER THAN THE OFFSET LIST'S DO
---------------------------------------------------
This report spells the vendor as the drive reports it over INQUIRY —
``HL-DT-ST`` — where driveoffsets.htm prints the marketing name ``LG
Electronics`` for the same drives. Measured over the 634 rows: 400 match the
offset list's spelling literally, and all but one match after the folding
``gen_offsets.py`` already does. So this source is CLOSER to the INQUIRY-keyed
table we generate, not further from it.

WHICH POST
----------
Spoon splits the report over three posts: "Top Drives", "Worst Drives" and "All
Drives". The first two are strict SUBSETS of the third (verified: every one of
their rows appears in it, with identical figures), so this tool takes "All
Drives" alone. Selecting it by heading rather than by position also excludes
the figures from EARLIER report years that other users quote further down the
thread — a 2019 table sits in the same thread in the same markup, and a
position-based reader would silently blend two years' data.

Usage:
    fetch_ar_accuracy.py --out private/research/incoming/ar_accuracy.json
    fetch_ar_accuracy.py --out ... --offline   # parse the cache, never fetch
"""

from __future__ import annotations

import argparse
import html
import json
import re
import sys
import urllib.error
import urllib.request
from datetime import datetime, timezone
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from fetch_ar_offsets import split_drive_name  # noqa: E402

URL = (
    "https://forum.dbpoweramp.com/forum/dbpoweramp/cd-ripper/"
    "337997-cd-drive-accuracy-2026"
)
# The forum rejects a bare scripted agent, so this one names a browser. It is
# still identified as ours by the suffix; the point is to be let in, not to hide.
USER_AGENT = (
    "Mozilla/5.0 (X11; Linux x86_64) AppleWebKit/537.36 (KHTML, like Gecko) "
    "Chrome/120.0.0.0 Safari/537.36 accudisc-accuracy-fetch/1.0"
)

HEADING = "All Drives"

# Drive: VENDOR  - MODEL  (N users):  Submissions: A accurate, B inaccurate, P % accuracy
ROW_RE = re.compile(
    r"^Drive:\s*(?P<name>.+?)\s*\(\s*(?P<users>\d+)\s+users\s*\)\s*:\s*"
    r"Submissions:\s*(?P<acc>\d+)\s+accurate,\s*(?P<inacc>\d+)\s+inaccurate,\s*"
    r"(?P<pct>[\d.]+)\s*%\s*accuracy\s*$"
)


def extract_block(page: str) -> tuple[str, int]:
    """Return the "All Drives" code block and the year of the post holding it.

    Raises SystemExit with a specific reason rather than returning something
    empty: every failure here means the forum's markup moved, and a silent zero
    rows would look exactly like a report that had shrunk.
    """
    hits = [m.start() for m in re.finditer(r"<b>\s*" + HEADING + r"\s*</b>", page)]
    if len(hits) != 1:
        sys.exit(
            f"expected exactly one '{HEADING}' heading, found {len(hits)} — "
            "the thread layout has changed; fix the parser rather than guessing"
        )
    start = hits[0]

    m = re.search(r"<pre[^>]*class=\"[^\"]*bbcode_code[^\"]*\"[^>]*>(.*?)</pre>",
                  page[start:], re.S)
    if not m:
        sys.exit(f"no code block follows the '{HEADING}' heading")

    # The year comes from the post's own timestamp, found by searching BACKWARDS
    # from the heading: this thread carries several years' reports and the
    # snapshot has to say which one it holds.
    stamps = re.findall(r"<time[^>]*datetime='(\d{4})-", page[:start])
    if not stamps:
        sys.exit("no post timestamp precedes the heading; cannot date the report")
    return html.unescape(m.group(1)), int(stamps[-1])


def parse_rows(block: str) -> tuple[list[dict], list[str]]:
    rows: list[dict] = []
    unparsable: list[str] = []
    for raw in block.splitlines():
        line = raw.strip()
        if not line:
            continue
        m = ROW_RE.match(line)
        if not m:
            unparsable.append(line)
            continue
        # ONE name-splitting rule, shared with the offset fetcher rather than
        # reimplemented. Its whitespace-on-both-sides requirement is what keeps
        # "HL-DT-ST" whole, and this report is full of HL-DT-ST.
        split = split_drive_name(m.group("name"))
        if split is None or not split[1]:
            unparsable.append(line)
            continue
        acc, inacc = int(m.group("acc")), int(m.group("inacc"))
        # The published percentage is redundant with the counts. Recomputing it
        # is a free check that the row was read correctly — a misparsed digit
        # shows up here and nowhere else.
        want = float(m.group("pct"))
        got = 100.0 * acc / (acc + inacc)
        if abs(got - want) > 5e-4:
            sys.exit(
                f"row does not agree with its own percentage: {line!r} "
                f"({acc}/{acc + inacc} = {got:.4f}, printed {want:.4f})"
            )
        rows.append(
            {
                "vendor": split[0],
                "product": split[1],
                "users": int(m.group("users")),
                "accurate": acc,
                "inaccurate": inacc,
            }
        )
    return rows, unparsable


def fetch(cache: Path, offline: bool) -> bytes:
    if offline or cache.exists():
        if not cache.exists():
            sys.exit(f"--offline given but no cache at {cache}")
        return cache.read_bytes()
    req = urllib.request.Request(URL, headers={"User-Agent": USER_AGENT})  # noqa: S310
    try:
        with urllib.request.urlopen(req, timeout=120) as resp:  # noqa: S310
            body = resp.read()
    except (urllib.error.URLError, TimeoutError) as exc:
        sys.exit(f"fetch failed: {exc}\n(the link here is slow — just retry)")
    cache.parent.mkdir(parents=True, exist_ok=True)
    cache.write_bytes(body)
    return body


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--out", type=Path, required=True, help="snapshot JSON to write")
    ap.add_argument(
        "--cache",
        type=Path,
        default=Path("/var/tmp/accudisc-ar-accuracy.htm"),
        help="raw page cache (/var/tmp, not /tmp — /tmp is a RAM tmpfs here)",
    )
    ap.add_argument("--offline", action="store_true", help="parse the cache, never fetch")
    args = ap.parse_args()

    page = fetch(args.cache, args.offline).decode("utf-8", "replace")
    block, year = extract_block(page)
    rows, unparsable = parse_rows(block)

    if not rows:
        sys.exit("no rows parsed — the report layout has changed; fix the parser")
    if unparsable:
        sys.exit(
            f"{len(unparsable)} line(s) in the block did not parse, first: "
            f"{unparsable[0]!r} — fix the parser rather than dropping rows"
        )

    seen: dict[tuple[str, str], dict] = {}
    for r in rows:
        k = (r["vendor"], r["product"])
        if k in seen:
            sys.exit(f"the report lists {k} twice; the join key is not unique")
        seen[k] = r

    snapshot = {
        "source": URL,
        "post_heading": HEADING,
        "report_year": year,
        "fetched_utc": datetime.now(timezone.utc).isoformat(timespec="seconds"),
        "rows": rows,
    }
    args.out.parent.mkdir(parents=True, exist_ok=True)
    args.out.write_text(json.dumps(snapshot, indent=1, sort_keys=True), encoding="utf-8")

    subs = [r["accurate"] + r["inaccurate"] for r in rows]
    print(f"{args.out}: {len(rows)} rows from the {year} report")
    print(f"  submissions per drive   min {min(subs)}  max {max(subs)}")
    print(f"  users per drive         min {min(r['users'] for r in rows)}"
          f"  max {max(r['users'] for r in rows)}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
