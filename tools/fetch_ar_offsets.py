#!/usr/bin/env python3
"""Fetch AccurateRip's drive read-offset table to a local snapshot.

This is the ONLY networked tool in AccuDisc, and it is a DEVELOPMENT tool:
`tools/` is excluded from the distribution tarball (see tools/mkdist.sh), so
nothing here ships, and the no-lookups rule — which is about runtime — is not
touched. The library never fetches anything.

It lives here rather than in a front end deliberately. AccuDisc owns the offset
data, and a consumer's scraper as a build input would make that consumer a
locked-in build dependency of the library it consumes.

WHAT THIS PRESERVES THAT OTHER IMPORTERS DISCARD
------------------------------------------------
The page spells a drive as ``"VENDOR  - MODEL"``, and that separator is the same
vendor/product split REDUMP keeps and SCSI INQUIRY reports in two fixed fields.
Importers that normalise the name to one string throw the boundary away and
cannot get it back — nothing in ``"PLEXTOR DVDR PX-716A"`` marks where the
vendor ended. We keep the tuple, because the table we generate is INQUIRY-keyed.

Distinguishing the separator from an intra-name hyphen (``HL-DT-ST``,
``DVD-RW``) is what the whitespace-on-both-sides rule below is for.

FOUR COLUMNS, NOT THREE
-----------------------
The table is CD Drive | Correction Offset | Submitted By | Percentage Agree.
That last column is AccurateRip's own agreement rate for the drive, and it is a
better confidence signal than the submission count: it is measured WITHIN one
source, so it does not depend on whether two catalogues are independent — a
question our sibling project measured and could not settle.

Usage:
    fetch_ar_offsets.py --out private/research/incoming/ar_offsets.json
    fetch_ar_offsets.py --cache /var/tmp/ar.htm --offline   # parse, no network
"""

from __future__ import annotations

import argparse
import json
import re
import sys
import urllib.error
import urllib.request
from datetime import datetime, timezone
from html.parser import HTMLParser
from pathlib import Path

URL = "http://www.accuraterip.com/driveoffsets.htm"
USER_AGENT = "accudisc-offset-fetch/1.0 (+https://github.com/HomerSlated/accudisc)"

# The two alternating row backgrounds mark data cells; header cells are black.
_DATA_BG = {"#f4f4f4", "#fcfcfc"}


def split_drive_name(raw: str) -> tuple[str, str] | None:
    """``"PLEXTOR  - DVDR PX-716A"`` -> ``("PLEXTOR", "DVDR PX-716A")``.

    A leading ``"- MODEL"`` is a no-vendor entry and yields ``("", model)``,
    which matches REDUMP's own empty-vendor rows. Returns None if there is no
    model left after splitting.

    The separator must have whitespace on BOTH sides. Without that rule
    ``HL-DT-ST`` splits into ``("HL", "DT-ST ...")`` — a vendor that does not
    exist, keyed against nothing, silently absent from every lookup.
    """
    s = raw.strip()
    m = re.match(r"^-\s+(.*)$", s)
    if m:
        model = " ".join(m.group(1).split())
        return ("", model) if model else None
    m = re.match(r"^(.*?)\s+-\s+(.*)$", s)
    if m:
        vendor = " ".join(m.group(1).split())
        model = " ".join(m.group(2).split())
        return (vendor, model) if model else None
    # No separator at all: treat the whole string as a model, vendor unknown.
    model = " ".join(s.split())
    return ("", model) if model else None


class _OffsetTableParser(HTMLParser):
    """Pull data rows out of driveoffsets.htm.

    Row shape: name | offset | submissions | percentage-agree. Rows whose
    offset reads "purged" are AccurateRip retiring a value; they carry no
    number and are skipped rather than counted as zero.
    """

    def __init__(self) -> None:
        super().__init__()
        self.rows: list[dict] = []
        self.skipped_purged = 0
        self.skipped_unparsable = 0
        self._cells: list[str] = []
        self._in_data_td = False
        self._buf: list[str] = []

    def handle_starttag(self, tag: str, attrs: list[tuple[str, str | None]]) -> None:
        if tag == "tr":
            self._cells = []
        elif tag == "td":
            bg = (dict(attrs).get("bgcolor") or "").lower()
            self._in_data_td = bg in _DATA_BG
            self._buf = []

    def handle_endtag(self, tag: str) -> None:
        if tag == "td" and self._in_data_td:
            self._cells.append("".join(self._buf).strip())
            self._in_data_td = False
            self._buf = []
        elif tag == "tr" and len(self._cells) >= 3:
            self._emit()

    def handle_data(self, data: str) -> None:
        if self._in_data_td:
            self._buf.append(data)

    def _emit(self) -> None:
        name, offset_s, subs_s = self._cells[0], self._cells[1], self._cells[2]
        pct_s = self._cells[3] if len(self._cells) > 3 else ""
        if "purged" in offset_s.lower():
            self.skipped_purged += 1
            return
        try:
            offset = int(offset_s.replace("+", ""))
            subs = int(subs_s)
        except ValueError:
            self.skipped_unparsable += 1
            return
        split = split_drive_name(name)
        if split is None:
            self.skipped_unparsable += 1
            return
        pct: int | None = None
        m = re.match(r"^\s*(\d+)\s*%", pct_s)
        if m:
            pct = int(m.group(1))
        self.rows.append(
            {
                "vendor": split[0],
                "product": split[1],
                "offset": offset,
                "submissions": subs,
                "agree_pct": pct,
            }
        )


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
        default=Path("/var/tmp/accudisc-ar-driveoffsets.htm"),
        help="raw page cache (/var/tmp, not /tmp — /tmp is a RAM tmpfs here)",
    )
    ap.add_argument("--offline", action="store_true", help="parse the cache, never fetch")
    args = ap.parse_args()

    body = fetch(args.cache, args.offline)
    parser = _OffsetTableParser()
    parser.feed(body.decode("latin-1"))

    if not parser.rows:
        sys.exit("no rows parsed — the page layout has changed; fix the parser")

    snapshot = {
        "source": URL,
        "fetched_utc": datetime.now(timezone.utc).isoformat(timespec="seconds"),
        "rows": parser.rows,
    }
    args.out.parent.mkdir(parents=True, exist_ok=True)
    args.out.write_text(json.dumps(snapshot, indent=1, sort_keys=True), encoding="utf-8")

    with_vendor = sum(1 for r in parser.rows if r["vendor"])
    with_pct = sum(1 for r in parser.rows if r["agree_pct"] is not None)
    print(f"{args.out}: {len(parser.rows)} rows")
    print(f"  with a vendor/model split   {with_vendor}")
    print(f"  with an agreement percentage {with_pct}")
    print(f"  skipped: purged {parser.skipped_purged}, unparsable {parser.skipped_unparsable}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
