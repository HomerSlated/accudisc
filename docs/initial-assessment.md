# Initial assessment — reference corpus and porting plan

*2026-07-10, after first population of `reference/` (read-only).*

## 1. What's in the corpus

### The prototype: c2read (+ Python drivers)

`reference/c2read/c2read.c` — 1001 lines, single file, dependency-free, Linux
SG_IO. Deliberately mechanism-only ("this tool decides nothing; it reads and
reports"); all policy lives in the Python side of the cdda2img pipeline:

- `c2_reader.py` — pipeline consumer: one full-disc pass captures PCM + C2 +
  raw P–W subchannel + full TOC + CD-Text, replacing a separate
  `cdrdao read-toc` pass.
- `c2bench.py` — C2 confusion matrix (TP/FP/FN/TN per stereo sample) against
  an AccurateRip-verified oracle; also measures read-to-read stability.
- `c2timing.py`, `c2read_recovery_test.py` — timing and mode-page-01
  recovery experiments.

Already proven out in the prototype, mapped to AccuDisc modules:

| c2read mechanism | AccuDisc home |
|---|---|
| `scsi_in()` SG_IO wrapper, sense decode, O_RDONLY/O_RDWR privilege model | `src/transport/` |
| READ CD (0xBE) CDB with C2/sub-channel field selection; READ TOC formats 0/2 (full)/5 (CD-Text) two-step alloc; GET CONFIGURATION 0x1E; MODE SENSE/SELECT(10) pages 01/2A; INQUIRY; START STOP UNIT | `src/mmc/` |
| Plextor 0xEA C1/C2/CU scan (Q-Check); "claim + functional smoke test" feature probing; page-2A speed fields | `src/drive/` |
| Chunked reads <64 KiB, per-sector retry narrowing, cache-defeat far-seek between retries, zero-fill (PCM 0 / C2 all-ones / sub 0) keeping streams length-consistent | `src/read/` |

Key design properties to preserve:

- **Single-read alignment**: PCM, C2 bitmap, and subchannel come from the
  *same* READ CD transfer, so they are inherently aligned to that read. This
  is the property that makes C2-guided recovery meaningful; the engine API
  must never split them across passes.
- **C2 asymmetry**: a fired flag is trustworthy, a clear flag is not
  (RS miscorrection passes wrong bytes silently). Mechanism never trusts
  either; verdicts are conservative (`C2_UNVERIFIED` ≠ usable).
- **Sector field order** DATA + C2 + SUB, probed on real hardware (PX-716A,
  Q CRC validation at offset 2352+294), matching redumper
  `SectorOrder::DATA_C2_SUB`.
- Machine-parseable stdout protocol (`progress`, `cx`, `track`, verdicts) —
  in AccuDisc this becomes the library's callback/event API; the CLI renders.

### Third-party sources (licenses noted — rewrite, never copy)

- **redumper** — modern C++23 modules; the CDB layouts in c2read were pinned
  from its `scsi/mmc.ixx`. `offsets.ixx` is a ~216 KB drive read-offset
  database — the seed for `src/drive/`'s offset table. (zlib license per repo;
  verify.)
- **cdrdao** (GPL-2) — the DAO *writing* reference: `GenericMMC.cc`,
  cue/TOC model, page-2A handling.
- **schily-2024-03-21** (cdrtools; CDDL/GPL mix) — `readcd` (cache-defeat +
  mode-page-01 patterns already borrowed conceptually, plus Plextor cx-scan
  origin), `cdrecord`, `libscg` (the classic portable SCSI transport —
  architecture reference for multi-OS transport), `libparanoia`.
- **libcdio-paranoia** (GPL-3) — verification/reread strategy reference.
- **cyanrip** (LGPL) — a clean, modern C ripper built on libcdio; closest
  existing shape to what AccuDisc's library wants to be.
- **whipper** (GPL-3, Python) — pipeline-level policy: offset finding,
  AccurateRip, pregap/index handling.
- **cuetools.net** (GPL-2, C#) — CUETools DB / AccurateRip verification,
  cue sheet semantics.
- **libmirage** (GPL-2) — disc image format models (session/track/subchannel
  representations worth studying for `src/toc/`).
- **cdrip-tools** — small C/Python AccurateRip utilities (`ckcdda.c`).

### Reverse-engineering material

- `Plextor/PTPXL/` — PlexTools Professional XL (Windows binary): the vendor
  tool exposing Q-Check C1/C2/CU, PoweRec, GigaRec, SecuRec; opcode source
  for RE beyond the 0xEA scan already implemented.
- `firmware/plextor/716A_111` — PX-716A firmware 1.11 image (the drive the
  prototype was validated on).
- `rippers/EAC`, `rippers/dBpoweamp CD Ripper` — proprietary Windows rippers
  (behavioral reference: drive feature detection, offset DB, C2 handling).
- `redbook/` — pits-and-lands→PCM decoding project (EFM, CIRC docs +
  diagrams + `decode.py`/`analyze.py`): the deep-format reference for
  subchannel/CIRC understanding.

## 2. Gap analysis: prototype → AccuDisc goals

Present in c2read: raw CD-DA read path with C2 + subchannel capture, TOC/
CD-Text *capture* (not decode), feature probing, one vendor extension, speed
control, spindle control, recovery-page experiments.

Missing for AccuDisc's stated scope:

1. **Decode layers** (currently "Python's job"): P–W deinterleave, Q parse +
   CRC, MCN/ISRC extraction, pregap/index scan, full-TOC/session parse,
   CD-Text pack decode. → `src/cdda/`, `src/toc/`, `src/meta/` are new C code.
2. **Drive database**: read offsets (seed from redumper `offsets.ixx` +
   AccurateRip conventions), cache sizes/behavior, C2 reliability class,
   overread capability, quirks. → `src/drive/`.
3. **Writing** — entirely absent from the prototype. DAO write path
   (cue → subchannel → SEND CUE SHEET / WRITE), CD-Text lead-in encode.
   References: cdrdao, cdrecord. → `src/write/`, later.
4. **Verification engine**: multi-read consensus and C2-guided rereads
   (c2bench's *findings* become the engine's *policy*), paranoia-style
   overlap checks. → `src/read/`.
5. **Transport portability**: SG_IO only today; libscg and redumper show the
   abstraction shape if other OSes ever matter.
6. **Library-ification**: c2read is a monolithic CLI with `fprintf(stderr)`
   error reporting and exit-code verdicts. The port must split mechanism
   into `libaccudisc` (handles, error codes, callbacks) with the CLI as a
   thin client — same split the skeleton already encodes.
7. Later scope: SA-CD, CD+G (R–W graphics decode).

## 3. Suggested porting order

1. **transport + mmc**: lift `scsi_in`, sense handling, CDB builders out of
   c2read into `src/transport/` + `src/mmc/` behind the public API; add the
   device handle type. Mostly mechanical; c2read.c is the spec.
2. **Public API v0**: device open/close/probe, TOC read, raw read with
   C2/sub, progress callback. CLI regains c2read parity (`--toc`,
   `--features`, `--full` capture) as the acceptance test.
3. **Decode layer**: Q subchannel (CRC, MCN/ISRC, indices), full-TOC parse,
   CD-Text decode — new code, informed by redumper/whipper/libmirage and the
   redbook notes.
4. **Drive layer**: probe + quirk/offset database, Plextor extensions;
   design the vendor-opcode plug-in point here.
5. **Read engine**: chunking/retry/zero-fill port, then C2-guided rereads
   and consensus using c2bench's measured confusion-matrix behavior.
6. **Write path**: last, once the TOC/cue model is solid.

## 4. Risks / open questions

- **License hygiene**: GPL trees surround us; keep the "read, understand,
  rewrite" discipline documented in CLAUDE.md. c2read itself is the user's
  own work — it may be ported directly.
- **c2read hard-codes chunk ≤ 63** (64-bit sector mask) and ≤ 64 KiB
  transfers; the library engine should revisit both (SG reserved buffers
  allow more, but per-command latency vs. retry granularity trades off).
- **Q-validation of sector order** was probed on one drive (PX-716A);
  the library should probe per drive rather than assume DATA_C2_SUB.
- The stdout protocol contract with cdda2img's Python pipeline must keep
  working during migration if the pipeline is to be retargeted onto the
  `accudisc` CLI later.
