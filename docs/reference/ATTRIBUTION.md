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
- **BinaryObjectScanner** by Matt Nadareski (MIT) — a preservation-community
  copy-protection scanner. Consulted for *facts* about commercial CD-DA
  protection schemes: which schemes exist, their mechanisms as documented by
  the preservation community, and the specific releases and barcodes carrying
  them. No code was taken; the two tools look at different things — it
  identifies schemes from installer payloads on the data session, while
  AccuDisc reports what a disc's TOC and session structure actually say. Its
  MediaCloQ description independently confirmed our finding that the scheme
  marks audio tracks as data.
- **DRML — the DRM Library** (https://github.com/TheRogueArchivist/DRML), by
  the same author. A per-scheme documentation project, consulted for the same
  kind of facts: mechanisms, versions, and the specific pressings carrying
  them, each with primary-source citations.

## CTDB parity repair (`src/repair/`)

The Reed-Solomon decoder over GF(2¹⁶) is **clean-room**, written from a spec
(`private/docs/rs16-spec.md`) rather than from any implementation, and the
distinction matters here more than elsewhere: the tool it replaces for this job,
cdda2img's `ctanalyse`, is **GPLv3**, and AccuDisc is MIT. No GPL source was
read, linked, included or executed by the implementation, by the tests, or by
any agent that worked on this subsystem.

- **The algorithms are textbook and unowned**: Berlekamp-Massey, the Forney
  algorithm, and the Chien search, in their standard errata (errors-and-
  erasures) form. The spec cites them rather than restating them, and the
  implementation follows the citations.
- **The CueTools Database (CTDB)** (https://db.cuetools.net), by Grigory
  Chudov, publishes the parity blobs and the per-track CRCs this operates on.
  AccuDisc does no lookups — the calling application fetches the entry — but
  the on-wire geometry the decoder must match is CTDB's. That geometry is
  recorded in `rs16-spec.md` §3a; it was determined by **measurement against
  real parity blobs**, then confirmed in correspondence with the cdda2img
  project, whose author had already solved it independently.
- **cdda2img's `ctanalyse`** was used as an **oracle, not a source**: its JSON
  *output* on three real disc images is compared element-wise against ours by
  `tests/ctdb_ab`. Two programs producing files that are then diffed is
  interoperability testing, and no GPL code enters the build.
- **The vectorised GF(2¹⁶) constant multiply** in `src/repair/sweep.c` uses the
  published split-nibble / `vpshufb` table method — decompose a field element
  into nibbles, replace the multiply with four 16-entry byte-table lookups —
  which entered general use through James S. Plank and collaborators' work on
  SIMD Galois-field arithmetic (the "Screaming Fast Galois Field Arithmetic"
  line of papers, and GF-Complete). The technique is described in the
  literature; this implementation was written from the description and shares
  no code with GF-Complete.
- **Slicing-by-8 CRC-32** is Intel's published table-driven technique, likewise
  implemented from the description.

## Vendor drivers

Drivers under `drivers/` are standalone modules with their own provenance
and licensing; see `drivers/README.md` and each driver directory. The
Plextor driver additionally incorporates knowledge derived from first-party
reverse engineering (probed hardware opcodes only — no redistributed vendor
binaries or sources).

- **QPxTool** (https://qpxtool.sourceforge.io, GPL-2.0) — a reference where the
  Plextor vendor command set used in `drivers/plextor/` is documented (opcode
  0xE9 MODE pages, GET/SET direction bits, per-feature CDB framing, GigaRec rate
  table, SpeedRead/SecuRec/AutoStrategy/PoweRec commands). Those commands are
  functional hardware identifiers (facts, not copyrightable expression); no
  QPxTool source is copied, and every command was independently verified by raw
  SG_IO on the owner's own PX-716A. QPxTool is credited here as a courtesy
  reference; the driver is MIT (see `drivers/plextor/LICENSE.md`).

## Signatures (`*.sig`, `docs/guardian_public.asc`)

- Detached OpenPGP signatures beside source files are generated by this
  project's own automated security-audit agent, not by a third party. The key
  (`Guardian Security Agent <guardian@accudisc.local>`, fingerprint
  `0041E2FB425879321C84D24A60A32C2382E546AC`) is a sign-only ed25519 key held
  locally by the maintainer; it certifies nothing about identity and carries no
  web-of-trust standing. A signature records that the exact file content passed
  an automated audit with no CRITICAL or HIGH findings on that date — it is an
  audit-freshness marker, **not** a security guarantee or an external
  certification. See the Signatures section of `README.md` for verification.
- Because a signature is a freshness marker, **absence is meaningful and is not
  an oversight**. As of 2026-08-02 `src/repair/rs16.{c,h}` carry signatures and
  `src/repair/{gf16.c,gf16.h,sweep.c,ctdb.c}` do not: gf16 and ctdb.c were
  audited and then modified by the remediation the audit itself asked for, and
  sweep.c was written afterwards. Stale signatures were **deleted rather than
  kept**, since one that does not verify is worse than none. They return when
  those files are audited in their current form.

## ATIP / media catalog (`src/drive/media_atip_db.inc`)

- ATIP manufacturer codes (97:SS:FF pressed into every CD-R pregroove) are
  public facts published on the discs themselves and widely catalogued. This
  compilation of code→manufacturer was informed by the media catalog in
  PlexTools Professional XL, and cross-validated against **cdrecord's
  `diskid.c` table** (schily/cdrtools) — the two, from independent sources,
  agree on 107 of ~123 codes. cdrecord's table is comparable in size but
  contains many entries its author marked "tentative"/"guessed" (J. Schilling's
  "Orange Forum Embargo" denied him the official list, so he reverse-engineered
  it); our PlexTools-sourced values are vendor-authoritative for the overlap.
  Three high-confidence cdrecord-only codes are folded into the union. Nero
  2026 (NeroAPIEngine.dll) was also checked and matches — it carries the same
  frozen registry (identical names/codes), adding nothing, which corroborates
  that the ATIP manufacturer space is effectively closed.
  Only the identifying facts (codes and names) are used from either source, not
  program code — so this is MIT factual data (like the read-offset table), in
  the core rather than a driver. Generated by `tools/gen_media_db.py`; both
  cdrtools and PlexTools credited as reference sources.
