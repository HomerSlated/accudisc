# Attribution

AccuDisc's core (MIT-licensed) is original code, but several kinds of
*knowledge* in it were learned from prior art. This file records the sources;
anything listed here must also be credited in the man page when it exists.

## Data

- **Drive read-offset table** (`src/drive/offsets_db.inc`): user-submitted
  factual offset measurements, merged by `tools/gen_offsets.py`, from what we
  once described as two independent collections and have since established is
  **one collection at two dates**:
  - the **REDUMP Disc Preservation Project** (https://redump.org), via the
    table shipped with redumper — which is itself the AccurateRip list below,
    imported once in 2022 and frozen, with the marketing vendor names rewritten
    to INQUIRY ones. Verified 2026-08-22 by set comparison against redumper's
    git history: 4595 rows each way, none unique to either. Credit is owed for
    the INQUIRY rewrite and for preserving rows AccurateRip has since dropped —
    not for a second measurement of the same drives;
  - the **AccurateRip drive offset list**
    (http://www.accuraterip.com/driveoffsets.htm), which also supplies the
    per-drive submission count and agreement percentage. Fetched by
    `tools/fetch_ar_offsets.py` on the development cycle — a developer tool,
    excluded from the distribution; the library itself never uses a network.

- **Drive rip-accuracy figures** (`ar_acc_ok`/`ar_acc_bad` in the same table):
  AccurateRip's periodic **CD Drive Accuracy** report, compiled and published by
  **Spoon** (Illustrate / dbPoweramp) on the dbPoweramp forum — the 2026 edition
  at https://forum.dbpoweramp.com/forum/dbpoweramp/cd-ripper/337997-cd-drive-accuracy-2026,
  a series that has run since 2006. Fetched by `tools/fetch_ar_accuracy.py`,
  again a developer tool excluded from the distribution.

  A **third derivation of the same collection**, not a third collection: it is
  AccurateRip's own submission database read the other way round — instead of
  asking whether a disc is accurate, asking which drives disagree with the
  reference most often. Credit is owed for the derivation and for publishing it,
  and the method is Spoon's own and stated in the post: it assumes the owners of
  any given drive have, on average, equally damaged discs. Counts, not
  percentages, are what we store, so the confidence travels with the figure.

  It covers 634 drives against our table's 5881 rows. **Absence is recorded as
  absence** (both counts zero), never as a score of zero — the report's own
  inclusion threshold is 4000 submissions and 40 users, so a missing drive is an
  uncommon one. The one drive it measures that our offset table cannot place is
  named in a `MEASURED BUT UNPLACED` block in `offsets_db.inc`.

  A third KIND of input, though not a third measurement: **rebadge
  identifications**. A rebadged drive reports the badge over INQUIRY, so a row
  withdrawn under that name strands its owner while the identical measurement
  ships under the OEM name. `REBADGE` in `tools/gen_offsets.py` keeps such a row
  where a cited mapping identifies the OEM drive AND the two agree on the offset.
  These are human judgements read off published hardware documentation, not
  measurements, and each is credited at its entry. The one in the table today
  comes from **rpc1.org's DVD firmware list**
  (archive.rpc1.org/farzeno/club-internet/dvd/dvdfi.htm), which identifies the
  Philips PCDV632 as a Toshiba SD-M1212 OEM drive. Every rescued row is named in
  a `RESCUED BY REBADGE` block in `offsets_db.inc`, with the trade stated: it
  republishes a row AccurateRip withdrew, and AccurateRip's reason for the
  withdrawal is not known to us.

  Offsets are facts about hardware, not creative expression. EAC's OffsetBase
  was evaluated as a third source and **rejected**: the only surviving copy is a
  2004 archive of a page that no longer exists, and EAC itself reads offsets
  from AccurateRip — so it is an ancestor of one of the sources above rather
  than an independent check on it. The same test applied to REDUMP later gave
  the same answer, which is why the wording above changed.

## Techniques and command knowledge

Implementations are original; the following projects documented the
behaviors, command layouts, and hardware quirks we relied on:

- **redumper** — READ CD CDB layout details, DATA_C2_SUB sector ordering,
  full-TOC session semantics.
- **cdrtools (readcd/cdrecord)** by Jörg Schilling — cache-defeat reread
  pattern, mode page 01 error-recovery handling, the original
  documentation of the Plextor C1/C2/CU scan opcodes, the DAO abort
  (`scsi_flush_cache`, which is the whole of its generic-MMC abort path), and
  the **CD write-speed procedure**: that a CD write uses SET CD SPEED (0xBB)
  with CLV rather than SET STREAMING (which cdrecord reaches for only on DVD,
  `speed_select_mdvd`); that the field units are kB/s with the deliberate
  `speed_x * 177` rounding rather than the arithmetically correct 176.4,
  because drives round down; that the read-speed field must be read back and
  passed through rather than left 0xFFFF, which means *maximum* and not
  *unchanged*; and the climb by one 1x rung when a drive refuses a speed below
  its own minimum with ILLEGAL REQUEST / ASC 0x24 instead of rounding up
  (`drv_mmc.c` `speed_select_mmc` / `mmc_set_speed`, `scsi_cdr.c`
  `scsi_set_speed`); and the **POWEREC write-speed governor** — vendor opcode
  0xED "drive mode 2", the 8-byte state block's layout, and the unusual SET form
  that carries its one-bit payload in CDB byte 1 rather than a data buffer
  (`drv_mmc.c` `drivemode2_plextor` / `powerrec_plextor` /
  `check_powerrec_plextor`, with the bitfield offsets from
  `libscg/scg/scsicdb.h`).
- **cdrdao** — mode page 2A speed-field offsets, DAO writing model
  (write path — SHIPPED 0.20.0 and hardware-verified on a PX-716A: an image
  burned DAO and read back bit-exact, CD-Text included).
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
  an oversight**. Stale signatures are **deleted rather than kept**, since one
  that does not verify is worse than none, and a file is signed only after an
  audit of the content it has *now*. `src/repair/ctdb.c` was withheld twice
  before being signed on 2026-08-02 — once for a pending fix, once for two
  findings against it — which is the mechanism working rather than failing.
- **A signed report is never amended in place.** Where a later audit finds an
  error in an earlier one, the correction is published as an erratum in the new
  report and the old signature goes on verifying the text that was actually
  signed. This has happened once (2026-08-02, a table cell overstating what a
  superseded bounds check rejected).

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

## CAV read-speed curve (`docs/research/cav-read-speed-geometry.md`)

- The PX-716's published CD read speeds — the CAV rungs with their inner and
  nominal rates, the CLV rungs, the address at which nominal CAV is reached, and
  which rungs each mode is offered — were read from the *Drive Information → CD
  Read* panel of **PlexTools Professional XL V3.16**, the vendor's own tool.
  These are statements of what the hardware does: capability facts, not
  copyrightable expression, and no PlexTools code or data file is redistributed.
  They are used as documentation and as the reference curve for
  `tools/cav_speed_model.py`; no drive behaviour is keyed off them, so nothing
  in the library depends on the table being present or correct.
