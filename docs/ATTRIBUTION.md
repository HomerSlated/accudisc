# Attribution

AccuDisc's core (MIT-licensed) is original code, but several kinds of
*knowledge* in it were learned from prior art. This file records the sources;
anything listed here must also be credited in the man page when it exists.

## Data

- **Drive read-offset table** (`src/drive/offsets_db.inc`): user-submitted
  factual offset measurements collected by the **REDUMP Disc Preservation
  Project** (https://redump.org), converted from the table shipped with
  redumper. Regenerated via `tools/gen_offsets.py`.

## Techniques and command knowledge

Implementations are original; the following projects documented the
behaviors, command layouts, and hardware quirks we relied on:

- **redumper** — READ CD CDB layout details, DATA_C2_SUB sector ordering,
  full-TOC session semantics.
- **cdrtools (readcd/cdrecord)** by Jörg Schilling — cache-defeat reread
  pattern, mode page 01 error-recovery handling, and the original
  documentation of the Plextor C1/C2/CU scan opcodes.
- **cdrdao** — mode page 2A speed-field offsets, DAO writing model
  (write path, upcoming).
- **libcdio-paranoia / cd-paranoia** — verification and reread strategy
  background.

## Vendor drivers

Drivers under `drivers/` are standalone modules with their own provenance
and licensing; see `drivers/README.md` and each driver directory. The
Plextor driver additionally incorporates knowledge derived from first-party
reverse engineering (probed hardware opcodes only — no redistributed vendor
binaries or sources).

- **QPxTool** (https://qpxtool.sourceforge.io, GPL) — the Plextor vendor
  MODE (0xE9) page numbers, GET/SET direction bits, per-feature CDB framing,
  and the GigaRec rate-code table used in `drivers/plextor/` were cross-checked
  against QPxTool's `lib/qpxplextor/plextor_features.cpp` as *factual*
  reference (analogous to the REDUMP offsets), then independently verified by
  raw SG_IO against the user's own PX-716A. No QPxTool code is copied; only the
  command-layout facts are used, and they live only in the non-free Plextor
  driver zone. Recorded here so the man page credits QPxTool. See
  `drivers/plextor/PROTOCOL.md` / `FEATURES.md`.
