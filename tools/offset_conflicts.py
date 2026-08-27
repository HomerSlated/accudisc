#!/usr/bin/env python3
"""Products held by more than one offset in the SHIPPED table.

Reads src/drive/offsets_db.inc — the committed bytes the library compiles in,
not the upstream corpus — and groups by PRODUCT alone, which is the key the
lookup is about to move to. Two populations, told apart by whether the vendor
can settle it:

  RESOLVABLE   every vendor under this product holds ONE offset, so supplying
               the vendor narrows to a single value.
  UNRESOLVABLE some vendor holds two or more offsets by itself; no INQUIRY
               field available to us separates them.
"""
import re, sys, collections, pathlib

SRC = pathlib.Path(sys.argv[1] if len(sys.argv) > 1 else "src/drive/offsets_db.inc")
# The seven fields this tool uses, and then ANYTHING — the row is allowed to
# grow. It ended in `\}` until 0.23.0 appended two accuracy columns, at which
# point this matched nothing and the tool reported "0 rows, 0 conflicting" and
# exited 0: a clean bill of health from a parser that had read the file and
# understood none of it. Hence both halves of this fix — the open tail, and the
# refusal below.
ROW = re.compile(r'\{\s*"([^"]*)"\s*,\s*"([^"]*)"\s*,\s*([+-]?\d+)\s*,\s*(\d+)'
                 r'\s*,\s*(\d+)\s*,\s*(\d+)\s*,\s*(\d+)\s*[,}]')

by_product = collections.defaultdict(list)
n = 0
for line in SRC.read_text(encoding="utf-8").splitlines():
    m = ROW.search(line)
    if not m:
        continue
    n += 1
    vendor, product, off, subs, pct, src, flags = m.groups()
    by_product[product].append((vendor, int(off), int(subs), int(src)))

# A table with no rows is not a table. Nothing downstream can tell "no conflicts"
# from "read nothing", so say so here rather than printing a reassuring zero.
if n == 0:
    sys.exit(f"{SRC}: no rows matched — the row format changed; fix ROW above")

SRCNAME = {1: "R", 2: "A", 3: "RA"}   # REDUMP / AccurateRip / both
resolvable, unresolvable = [], []
for product, rows in by_product.items():
    if len({off for _, off, _, _ in rows}) < 2:
        continue
    per_vendor = collections.defaultdict(set)
    for vendor, off, _, _ in rows:
        per_vendor[vendor].add(off)
    (unresolvable if any(len(v) > 1 for v in per_vendor.values())
     else resolvable).append((product, rows))

def dump(title, group):
    print(f"\n=== {title}: {len(group)} products ===")
    for product, rows in sorted(group):
        vals = sorted({off for _, off, _, _ in rows})
        print(f"{product!r}  {len(rows)} rows, {len(vals)} values: "
              + " / ".join(f"{v:+d}" for v in vals))
        for vendor, off, subs, src in sorted(rows, key=lambda r: (-r[2], r[0])):
            print(f"      {off:+6d}  {subs:5d} subs  {SRCNAME.get(src,'?'):2}  "
                  f"{vendor or '(no vendor)'}")

dump("RESOLVABLE by vendor", resolvable)
dump("UNRESOLVABLE — one vendor, several offsets", unresolvable)
print(f"\n{n} rows, {len(by_product)} distinct products, "
      f"{len(resolvable)+len(unresolvable)} conflicting "
      f"({len(resolvable)} resolvable, {len(unresolvable)} not)")
