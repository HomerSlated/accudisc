#!/usr/bin/env python3
"""name_reduce.py — reduce drive names to one relevant string per line.

Reads the ``name`` column of an ``offsets_all_raw.tsv`` dump and strips the
boilerplate -- speed specifications, interface words, marketing nouns -- until
only the distinguishing part of each drive name is left, or nothing at all.

Two stages, and only the second one is yours to grow:

* **Extraction** is structural, not textual.  ``offset_dump_all.py`` builds col 4
  as ``f"{vendor} {product}".strip()``, so selecting that column already drops
  the source tag, the signed offset, the AccurateRip submission count and the
  vendor/product-vs-name duplication.  Nothing here is a regex guess.
* **Reduction** applies the term dictionary (``drive_name_terms.tsv``).  Each
  line is a *token list* and each rule matches a whole token, so a rule can
  never disturb a neighbour's boundaries and the order rules are applied in
  does not change the result.

The dictionary is grown by reading ``--report``, not by reading the corpus::

    uv run python tools/name_reduce.py --report        # what is still there
    uv run python tools/name_reduce.py --stats         # what each rule caught
    uv run python tools/name_reduce.py --no-vendor     # which vendors are missing
    uv run python tools/name_reduce.py --diff | less   # before -> after
    uv run python tools/name_reduce.py -o out.txt      # 1:1 with input rows
"""

from __future__ import annotations

import argparse
import csv
import os
import re
import sys
from collections import Counter
from dataclasses import dataclass, field
from pathlib import Path

INPUT = Path("private/research/incoming/offsets_all_raw.tsv")
TERMS = Path(__file__).with_name("drive_name_terms.tsv")

NAME_COLUMN = 3  # 0-based: source, vendor, product, name, offset, submissions
_SPLIT_PASSES = 8  # fixpoint guard for chained split rules

KINDS = ("pre", "token", "regex", "split", "strip", "keep", "keepre", "first")
KEEP_KINDS = ("keep", "keepre")

CASE_NOTE = "cs:"          # note prefix: make this rule case-sensitive
STRUCTURAL_NOTE = "struct:"  # note prefix: keep rule guards structure, not a brand
NOTE_PREFIXES = (CASE_NOTE, STRUCTURAL_NOTE)


@dataclass
class Rule:
    """One dictionary entry.  ``hits`` accumulates across the run.

    kind
        ``keep``   literal token that no other rule may touch, and that
                   ``--report`` hides.  Applied before everything below.
        ``keepre`` the same guard, but the pattern is a regex.  ``keep`` stays
                   literal so hand-written brand names never need escaping.

                   A keep rule whose ``note`` starts with ``struct:`` guards
                   STRUCTURE, not a brand -- a revision letter, a class
                   designator -- and is excluded from the vendor set that
                   ``--no-vendor`` tests against.  See :func:`vendor_keeps`.
        ``token``  literal token, dropped on a whole-token match.
        ``regex``  regex, dropped when it ``fullmatch``es a whole token.
        ``split``  regex; on a ``fullmatch`` the token is replaced by its
                   capture groups, which are then re-examined by every rule.
        ``strip``  regex substituted *within* a token by ``repl``; the token
                   survives unless the substitution empties it.
        ``pre``    regex substituted on the WHOLE name by ``repl`` before it
                   is tokenised.  The only kind where boundaries are not
                   structural -- use it solely for cross-token patterns a
                   token rule cannot see, such as a spaced speed spec.
        ``first``  regex dropped only in LEADING position, repeatedly.  This is
                   the one kind that overrides ``keep``: position is exactly
                   what separates a leading class designator ("DVD A DH16A6S")
                   from a trailing revision suffix ("DRW-24B1ST c"), and the
                   same character plays both roles.
    """

    kind: str
    pattern: str
    repl: str
    note: str
    lineno: int
    rx: re.Pattern[str] = field(init=False)
    hits: int = field(default=0, init=False)

    def __post_init__(self) -> None:
        flags = 0 if self.note.startswith(CASE_NOTE) else re.IGNORECASE
        literal = self.kind in ("token", "keep")
        src = re.escape(self.pattern) if literal else self.pattern
        self.rx = re.compile(src, flags)


def check_note(kind: str, note: str, path: Path, lineno: int) -> None:
    """Reject a note whose magic prefix would be silently ignored.

    Only the LEADING prefix is honoured -- ``__post_init__`` tests
    ``startswith`` and :func:`vendor_keeps` does the same -- so a note carrying
    both would compile, match, and be classified wrongly with nothing to show
    for it.  Same for ``struct:`` on a rule that is not a keep guard, where it
    means nothing at all.  Both are cheap to state and invisible if unstated.
    """
    lead = next((pfx for pfx in NOTE_PREFIXES if note.startswith(pfx)), None)
    if lead is None:
        return
    other = next((pfx for pfx in NOTE_PREFIXES if pfx in note[len(lead) :]), None)
    if other:
        sys.exit(
            f"{path}:{lineno}: note carries both {lead!r} and {other!r};"
            f" only the leading prefix is honoured, so a note may declare one"
        )
    if lead == STRUCTURAL_NOTE and kind not in KEEP_KINDS:
        sys.exit(
            f"{path}:{lineno}: {STRUCTURAL_NOTE!r} means nothing on a"
            f" {kind!r} rule -- it only excludes a keep guard from the vendor set"
        )


def load_rules(path: Path) -> list[Rule]:
    """Parse the term dictionary.  Blank lines and ``#`` comments are ignored."""
    rules: list[Rule] = []
    with path.open(newline="") as f:
        reader = csv.reader(f, delimiter="\t", quoting=csv.QUOTE_NONE)
        for lineno, row in enumerate(reader, start=1):
            if not row or not row[0].strip() or row[0].lstrip().startswith("#"):
                continue
            row = [*row, "", "", ""][:4]
            kind, pattern, repl, note = (c.strip() for c in row)
            if kind == "kind":  # header
                continue
            if kind not in KINDS:
                sys.exit(f"{path}:{lineno}: unknown kind {kind!r} (want {KINDS})")
            check_note(kind, note, path, lineno)
            try:
                rules.append(Rule(kind, pattern, repl, note, lineno))
            except re.error as exc:
                sys.exit(f"{path}:{lineno}: bad regex {pattern!r}: {exc}")
    return rules


SOURCE_COLUMN = 0


def read_rows(path: Path) -> list[tuple[str, str]]:
    """Return ``(source, name)`` for every data row, in file order.

    ``QUOTE_NONE`` is not optional.  A drive model containing a ``"`` would
    otherwise be read as a quoted field, swallowing the following tab and
    shifting every column index after it.  A short or empty row is a corrupt
    dump, not a row to skip -- skipping would lose it silently and break the
    1:1 correspondence with the input that the whole output format relies on.
    """
    rows: list[tuple[str, str]] = []
    with path.open(newline="") as f:
        reader = csv.reader(f, delimiter="\t", quoting=csv.QUOTE_NONE)
        for lineno, row in enumerate(reader, start=1):
            if not row or row[0].startswith("#") or row[0] == "source":
                continue
            if len(row) <= NAME_COLUMN or not row[NAME_COLUMN].strip():
                sys.exit(
                    f"{path}:{lineno}: expected a name in column {NAME_COLUMN + 1}"
                    f" of {len(row)} fields: {row!r}"
                )
            rows.append((
                row[SOURCE_COLUMN],
                re.sub(r"\s+", " ", row[NAME_COLUMN]).strip(),
            ))
    return rows


def read_names(path: Path) -> list[str]:
    """Just the names from :func:`read_rows`, in file order."""
    return [name for _, name in read_rows(path)]


def vendor_keeps(rules: list[Rule]) -> list[Rule]:
    """The keep rules that name a VENDOR, which is not all of them.

    Two of the guards protect structure rather than a brand -- a one-character
    revision or class token, and a ``V\\d+`` revision marker -- so "some keep
    rule fired" is NOT the same predicate as "a vendor was recognised".  It
    over-counts by 567 rows, and the result is a well-formed number that
    nothing downstream can catch.  Hence the marker: a keep rule is structural
    when its note begins ``struct:``, DECLARED in the dictionary rather than
    inferred from ``keep`` vs ``keepre``.  Inferring from the kind happens to
    work today and would rot the first time a brand needs a regex spelling.
    """
    return [
        r
        for r in rules
        if r.kind in KEEP_KINDS and not r.note.startswith(STRUCTURAL_NOTE)
    ]


def has_vendor(tokens: list[str], vendors: list[Rule]) -> bool:
    """True when any token is claimed by a vendor keep rule."""
    return any(r.rx.fullmatch(tok) for tok in tokens for r in vendors)


def is_kept(token: str, rules: list[Rule]) -> bool:
    """True when a ``keep`` rule claims this token, making it untouchable.

    Deliberately free of side effects: this is consulted at both the strip and
    the drop stage, so counting hits here would double every keep rule's total
    and make ``--stats`` useless as a safety check.  Counting happens once, in
    :func:`count_keeps`.
    """
    return any(r.kind in KEEP_KINDS and r.rx.fullmatch(token) for r in rules)


def count_keeps(tokens: list[str], rules: list[Rule]) -> None:
    """Credit one hit per kept token, to the first keep rule that claims it."""
    for tok in tokens:
        for rule in rules:
            if rule.kind in KEEP_KINDS and rule.rx.fullmatch(tok):
                rule.hits += 1
                break


def _split_once(tokens: list[str], rules: list[Rule]) -> tuple[list[str], bool]:
    """One split pass.  Returns the new token list and whether anything moved."""
    out: list[str] = []
    changed = False
    for tok in tokens:
        for rule in rules:
            if rule.kind != "split":
                continue
            m = rule.rx.fullmatch(tok)
            if not m:
                continue
            parts = [g for g in m.groups() if g]
            if len(parts) > 1 or parts != [tok]:
                out.extend(parts)
                rule.hits += 1
                changed = True
                break
        else:
            out.append(tok)
    return out, changed


def _apply_splits(tokens: list[str], rules: list[Rule]) -> list[str]:
    """Run split rules to a fixpoint: a split can expose a further split."""
    for _ in range(_SPLIT_PASSES):
        tokens, changed = _split_once(tokens, rules)
        if not changed:
            break
    return tokens


def _apply_strips(tokens: list[str], rules: list[Rule]) -> list[str]:
    """Rewrite within tokens; a token emptied this way is gone.

    A kept token is immune here as well as at the drop stage, or a strip rule
    would erode a vendor name one edit at a time.
    """
    out: list[str] = []
    for tok in tokens:
        if is_kept(tok, rules):
            out.append(tok)
            continue
        for rule in rules:
            if rule.kind != "strip":
                continue
            new, n = rule.rx.subn(rule.repl, tok)
            if n:
                rule.hits += n
                tok = new
        if tok:
            out.append(tok)
    return out


def _apply_drops(tokens: list[str], rules: list[Rule]) -> list[str]:
    """Remove whole tokens claimed by a ``token`` or ``regex`` rule."""
    out: list[str] = []
    for tok in tokens:
        if is_kept(tok, rules):
            out.append(tok)
            continue
        for rule in rules:
            if rule.kind in ("token", "regex") and rule.rx.fullmatch(tok):
                rule.hits += 1
                break
        else:
            out.append(tok)
    return out


def _apply_first(tokens: list[str], rules: list[Rule]) -> list[str]:
    """Drop leading tokens claimed by a ``first`` rule, repeatedly.

    Looping matters: "A D DH16A6S" exposes a second leading single letter once
    the first is gone, and a name can reduce to nothing but designators.
    Deliberately not subject to the keep guard -- see :class:`Rule`.
    """
    while tokens:
        for rule in rules:
            if rule.kind == "first" and rule.rx.fullmatch(tokens[0]):
                rule.hits += 1
                tokens = tokens[1:]
                break
        else:
            break
    return tokens


def _dedup(tokens: list[str]) -> list[str]:
    """All but one copy of any repeated token; the first occurrence wins."""
    seen: set[str] = set()
    out: list[str] = []
    for tok in tokens:
        key = tok.upper()
        if key not in seen:
            seen.add(key)
            out.append(tok)
    return out


def reduce_name(
    name: str,
    rules: list[Rule],
    dedup: bool = True,
    drop_vendor_only: bool = True,
) -> str:
    """Apply the dictionary to one name and return what survives.

    Second pass: a line whose every surviving token is ``keep``-guarded carries
    no model number at all -- just a vendor, or a bare revision letter -- so it
    identifies nothing and is blanked.  ``drop_vendor_only=False`` keeps it.
    """
    return finish_tokens(
        prepared_tokens(name, rules),
        rules,
        dedup=dedup,
        drop_vendor_only=drop_vendor_only,
    )


def prepared_tokens(name: str, rules: list[Rule]) -> list[str]:
    """``pre`` substitutions, then ``split`` to a fixpoint.

    This is the token list the keep guard sees -- everything structural has
    happened and nothing has been dropped yet, so a vendor glued into
    ``16X DVD-ROM`` is visible here and would not be in ``name.split()``.
    Factored out so :func:`reduce_name` and the vendor test can never disagree
    about which stages run first.

    Both halves bump ``rule.hits``, so call this ONCE per name: a second walk
    of the corpus would double every ``pre`` and ``split`` tally and quietly
    turn ``--stats`` into fiction.
    """
    for rule in rules:
        if rule.kind == "pre":
            name, n = rule.rx.subn(rule.repl, name)
            rule.hits += n
    return _apply_splits(name.split(), rules)


def finish_tokens(
    tokens: list[str],
    rules: list[Rule],
    dedup: bool = True,
    drop_vendor_only: bool = True,
) -> str:
    """Everything after :func:`prepared_tokens`: drop, strip, dedup, blank."""
    count_keeps(tokens, rules)
    tokens = _apply_first(_apply_drops(_apply_strips(tokens, rules), rules), rules)
    if dedup:
        tokens = _dedup(tokens)
    if drop_vendor_only and tokens and all(is_kept(t, rules) for t in tokens):
        return ""
    return " ".join(tokens)


def report(
    names: list[str],
    reduced: list[str],
    rules: list[Rule],
    limit: int,
    examples: int,
    show_kept: bool = False,
) -> None:
    """Rank the tokens that survived, so the next term picks itself.

    ``keep`` tokens are hidden by default.  They are decided policy, so leaving
    them in the ranking would bury the tokens still awaiting a decision.
    """
    keeps = [r for r in rules if r.kind in KEEP_KINDS]
    hidden: set[str] = set()

    freq: Counter[str] = Counter()
    where: dict[str, list[str]] = {}
    for src, red in zip(names, reduced):
        for tok in {t.upper() for t in red.split()}:
            if not show_kept and any(r.rx.fullmatch(tok) for r in keeps):
                hidden.add(tok)
                continue
            freq[tok] += 1
            where.setdefault(tok, [])
            if len(where[tok]) < examples:
                where[tok].append(src)

    blank = sum(1 for r in reduced if not r)
    print(
        f"# {len(names)} rows, {blank} fully reduced, {len(freq)} distinct tokens left"
    )
    if hidden:
        print(
            f"# {len(hidden)} kept tokens hidden ({len(keeps)} keep rules); --all shows them"
        )
    print(f"# top {min(limit, len(freq))} surviving tokens by line count\n")
    for tok, n in freq.most_common(limit):
        print(f"{n:6d}  {tok}")
        for ex in where[tok]:
            print(f"          {ex}")


def print_no_vendor(
    rows: list[tuple[str, str]],
    rules: list[Rule],
    limit: int,
    examples: int,
) -> None:
    """Rows that no vendor keep rule claims, grouped by their leading token.

    The audit question is "which vendors has the dictionary not been taught
    yet?", and a flat list is the wrong shape for it -- the few leading tokens
    that are real brands are buried under repeats of the same junk prefix.

    Grouping by the LEADING token is structural, not a guess: ``offset_dump_
    all.py`` builds column 4 as ``f"{vendor} {product}"``, so token 0 is the
    vendor POSITION whatever it happens to hold.  A group headed by a brand is
    a missing keep rule; one headed by ``ATAPI`` or ``16X`` is a drive that
    reported no vendor at all.

    Counts are reported in two units and each is labelled, because they answer
    different questions and are not interchangeable: rows measure how much of
    the corpus is affected, distinct names measure how much there is to read.
    ``--limit 0`` and ``--examples 0`` lift the caps, printing every distinct
    name -- not every row, since a name repeated across the two corpora is one
    line with its count beside it.
    """
    vendors = vendor_keeps(rules)
    structural = [
        r for r in rules if r.kind in KEEP_KINDS and r.note.startswith(STRUCTURAL_NOTE)
    ]

    groups: dict[str, dict] = {}
    names: set[str] = set()
    modelled: set[str] = set()
    hits = 0
    for src, name in rows:
        tokens = prepared_tokens(name, rules)
        reduced = finish_tokens(tokens, rules)
        if has_vendor(tokens, vendors):
            continue
        hits += 1
        names.add(name)
        if reduced:
            modelled.add(name)
        head = tokens[0].upper() if tokens else ""
        g = groups.setdefault(head, {"rows": 0, "names": Counter(), "srcs": Counter()})
        g["rows"] += 1
        g["names"][name] += 1
        g["srcs"][src] += 1

    print(
        f"# {hits} of {len(rows)} rows carry no recognised vendor"
        f" ({len(vendors)} vendor keep rules;"
        f" {len(structural)} structural rule(s) excluded)"
    )
    print(
        f"# {len(names)} distinct names, of which {len(modelled)} still reduce"
        f" to a model string and {len(names) - len(modelled)} reduce to nothing"
    )
    print("# grouped by LEADING token -- column 4 is \"vendor product\", so it is")
    print("# the vendor position: a real brand here is a keep rule we are missing")
    ranked = sorted(groups.items(), key=lambda kv: (-kv[1]["rows"], kv[0]))
    print(f"# {min(limit, len(ranked)) if limit else len(ranked)} of {len(ranked)} groups\n")

    for head, g in ranked[: limit or None]:
        mix = " ".join(f"{s}:{n}" for s, n in sorted(g["srcs"].items()))
        print(f"{g['rows']:6d}  {head or '(none)':<14s}  {mix}")
        for name, n in g["names"].most_common(examples or None):
            print(f"          {n:3d}  {name}")


def vendor_candidates(path: Path) -> list[tuple[str, int]]:
    """Distinct Redump vendor strings (col 2) with row counts, commonest first.

    This is evidence, not a curated brand list: the column holds whatever the
    drive reported, so device-class words (``DVDRW``, ``SATA``, ``CD-RW``) and
    mis-split fragments (``16X DVD-``) appear alongside real vendors.  Read the
    output before pasting it into the dictionary.
    """
    counts: Counter[str] = Counter()
    with path.open(newline="") as f:
        reader = csv.reader(f, delimiter="\t", quoting=csv.QUOTE_NONE)
        for row in reader:
            if not row or row[0].startswith("#") or row[0] == "source":
                continue
            if len(row) > 1 and row[1].strip():
                counts[re.sub(r"\s+", " ", row[1]).strip()] += 1
    return counts.most_common()


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("-i", "--input", type=Path, default=INPUT)
    ap.add_argument("-t", "--terms", type=Path, default=TERMS)
    ap.add_argument("-o", "--output", type=Path, help="default stdout")
    ap.add_argument("--no-dedup", action="store_true", help="keep repeated tokens")
    ap.add_argument(
        "--keep-vendor-only",
        action="store_true",
        help="do not blank lines that reduce to a vendor or a bare revision",
    )
    ap.add_argument("--report", action="store_true", help="rank surviving tokens")
    ap.add_argument("--all", action="store_true", help="report: include kept tokens")
    ap.add_argument("--vendors", action="store_true", help="keep candidates from col 2")
    ap.add_argument("--limit", type=int, default=40, help="report rows (default 40)")
    ap.add_argument("--examples", type=int, default=2, help="example lines per token")
    ap.add_argument("--stats", action="store_true", help="per-rule hit counts")
    ap.add_argument("--blanked", action="store_true", help="lines reduced to nothing")
    ap.add_argument(
        "--no-vendor",
        action="store_true",
        help="rows no vendor keep rule claims, grouped by leading token"
        " (--limit 0 --examples 0 lifts the caps: every distinct name)",
    )
    ap.add_argument("--diff", action="store_true", help="show only changed lines")
    ap.add_argument(
        "--unique",
        action="store_true",
        help="distinct non-empty strings with their source mix",
    )
    args = ap.parse_args()

    if not args.input.exists():
        sys.exit(f"no input at {args.input} (run from the repo root?)")

    if args.vendors:
        print_vendors(args.input)
        return

    rules = load_rules(args.terms)
    rows = read_rows(args.input)

    if args.no_vendor:
        # Before the reduction below: that walk and this one both bump
        # rule.hits, and doing them one after the other would double-count.
        print_no_vendor(rows, rules, args.limit, args.examples)
        return

    sources = [s for s, _ in rows]
    names = [n for _, n in rows]
    reduced = [
        reduce_name(
            n,
            rules,
            dedup=not args.no_dedup,
            drop_vendor_only=not args.keep_vendor_only,
        )
        for n in names
    ]

    if args.report:
        report(names, reduced, rules, args.limit, args.examples, show_kept=args.all)
    elif args.stats:
        print_stats(rules, len(names))
    elif args.blanked:
        gone = [n for n, r in zip(names, reduced) if not r]
        print(f"# {len(gone)} of {len(names)} rows reduced to nothing")
        print("\n".join(gone))
    elif args.unique:
        print_unique(sources, names, reduced)
    elif args.diff:
        for n, r in zip(names, reduced):
            if n != r:
                print(f"{n}\t->\t{r}")
    else:
        write_output(reduced, args.output)


def print_vendors(path: Path) -> None:
    """Emit ready-to-paste ``keep`` lines derived from the dump's vendor column."""
    cands = vendor_candidates(path)
    print(f"# {len(cands)} distinct vendor strings in column 2, commonest first")
    print("# paste the real brands below as keep rules; the column also holds")
    print("# device-class words and mis-split fragments -- read before pasting")
    for vendor, n in cands:
        print(f"keep\t{vendor}\t\t{n} rows")


def print_unique(sources: list[str], names: list[str], reduced: list[str]) -> None:
    """Distinct non-empty reduced strings, with the source mix behind each.

    A bare ``sort -u`` on the output loses which corpus a line came from, which
    is the only thing that makes a distinct string interesting: "both" means the
    two corpora agree on this drive, "Redump" or "AR" means one holds it alone.
    ``names`` shows the raw spellings that reduced to it -- more than one is a
    naming variant the reduction reconciled.
    """
    by_key: dict[str, dict[str, object]] = {}
    for src, name, red in zip(sources, names, reduced):
        if not red:
            continue
        e = by_key.setdefault(red, {"srcs": set(), "rows": 0, "names": set()})
        e["srcs"].add(src)
        e["rows"] += 1
        e["names"].add(name)

    w = csv.writer(sys.stdout, delimiter="\t", lineterminator="\n")
    w.writerow(["reduced", "sources", "rows", "raw_names"])
    for red in sorted(by_key, key=str.upper):
        e = by_key[red]
        srcs = e["srcs"]
        mix = "both" if len(srcs) > 1 else next(iter(srcs))
        w.writerow([
            red,
            mix,
            e["rows"],
            " | ".join(sorted(e["names"])),
        ])


def print_stats(rules: list[Rule], rows: int) -> None:
    """Per-rule hit counts.  A rule at 0 is dead; a rule that leaps is suspect."""
    print(f"# {len(rules)} rules, {rows} rows")
    print(f"{'hits':>8}  {'kind':<6} pattern")
    for rule in sorted(rules, key=lambda r: -r.hits):
        flag = "  <-- never fires" if rule.hits == 0 else ""
        print(f"{rule.hits:8d}  {rule.kind:<6} {rule.pattern}{flag}")


def write_output(reduced: list[str], path: Path | None) -> None:
    """Write one line per input row, in input order, blanks preserved."""
    body = "\n".join(reduced) + "\n"
    if path is None:
        sys.stdout.write(body)
        return
    path.write_text(body)
    kept = sum(1 for r in reduced if r)
    print(f"wrote {path}  ({len(reduced)} rows, {kept} non-empty)")


if __name__ == "__main__":
    try:
        main()
    except BrokenPipeError:
        # Piping into head/less closes the pipe early.  Python would otherwise
        # flush stdout again at exit and report a second error on stderr.
        os.dup2(os.open(os.devnull, os.O_WRONLY), sys.stdout.fileno())
        sys.exit(1)
