# AccuDisc — Recording (DAO write) implementation plan

Status: **Phases 1–2 complete and hardware-verified** (audio DAO burns
bit-exact — see §9); **Phase 3 in progress (2026-07-23): full TOC + CD-Text.**
This document is the design; completed work is marked inline in §9. Scope is Red
Book CD-DA Disc-At-Once, matching the read path's philosophy: a raw,
machine-driven engine that only moves bits. Primary reference is **cdrdao**
(`private/code/cdrdao/dao/`), with **cdrecord/schily**
(`private/code/schily-2024-03-21/`) as a cross-check for command details and
drive quirks. Both are credited in `docs/ATTRIBUTION.md`.

## 1. Scope and non-goals

- **In scope**: DAO (Session-At-Once) burning of one audio session to CD-R/RW —
  lead-in (incl. CD-Text + MCN/ISRC), pregaps/indices, audio tracks, lead-out.
  Caller supplies the layout and the bits.
- **Out of scope** (unchanged project constraints): no ISO9660/data-mode
  encoding, no DVD/BD, no analysis/lookups, no track ripping-from-file decode
  (the caller hands us finished PCM). TAO/packet/multisession-data are not Red
  Book audio and stay out. Multisession *audio* is a maybe-later.
- **Non-negotiable**: the engine never fabricates or "improves" audio. What the
  caller hands in is what gets burned, sample-for-sample (subject only to the
  offset the caller asks for — never silent).

## 2. Inputs — caller supplies TOC + BIN (+ optional SUB)

Symmetric with the read path (which emits TOC/BIN/SUB). The engine consumes:

- **TOC / cuesheet**: track list, ISRCs, MCN, CD-Text, pregap/index points,
  per-track flags (pre-emphasis, copy-permit, SCMS). Reuse the `toc/` model and
  cuesheet parser already in the tree; extend it to be a *write* source, not
  just a read sink. cdrdao's `trackdb/Toc.cc` + `CueParser.cc` are the model
  reference; our `toc/` is the implementation.
- **BIN**: the audio image — raw s16le LE stereo @ 44100, 2352 bytes/sector,
  exactly the read path's `--pcm` output. Byte-order/offset are the caller's
  responsibility; we document the contract and honour it verbatim.
- **SUB (optional)**: if the caller wants to supply raw P–W subchannel (96
  B/sector) rather than have us synthesize P/Q from the TOC. Default: we
  generate P/Q (and R–W for CD-Text/CD+G) from the TOC via a subchannel encoder
  (`PQChannelEncoder`/`PWSubChannel96` are the cdrdao references). Supplying SUB
  is the escape hatch for exact reproduction (e.g. re-burning a captured disc).

## 3. Machine I/O contract (raw engine, driven by a caller)

Same discipline as the read path (`docs/cli-machine-interface.md`): humans get
stderr, machines get frozen tokens. New `write` subcommand:

```
accudisc write --toc FILE --bin FILE [--sub FILE] [--speed X] \
               [--simulate] [--progress-fd N] [--map-file F] \
               [--driver auto|NAME]   # vendor write features, opt-in
```

- **stdin**: reserved for streaming the BIN when `--bin -` (pipe the image);
  otherwise unused. Lets a caller feed a FIFO without a temp file.
- **stdout**: machine tokens only. `--progress-fd N` mirrors the read path:
  `progress <written> <total>` lines + a final
  `summary written=<n> failed=<n> underruns=<n> speed=<x> mode=<sao|sim>`.
- **stderr**: human `\r` status line (explicitly not a stable interface).
- **exit codes**: reuse the 0/1/2/3 convention (0 clean, 1 usage/local-file,
  2 fatal device/transport, 3 completed-with-caveats e.g. a recovered underrun
  or a simulate-only run the caller must not mistake for a real burn).
- **`--map-file F`**: the per-sector *write* status map (§6), same
  `MAP_SHARED`/one-byte-per-sector ABI as the read map, new state nibble values
  for write outcomes.

## 4. The DAO command sequence (from cdrdao `dao/GenericMMC.cc`)

The engine's core, in order. Each step names the cdrdao function to port from
and the MMC opcode.

1. **Set write parameters** — MODE SELECT mode page **0x05**
   (`setWriteParameters`, tries variants). Key bytes: `mp[2]` write-type=0x02
   (SAO) + BURN-Proof (0x40) + test-write bit for `--simulate`; `mp[3]`
   multisession bits; `mp[4]` data-block-type (raw 2352, or `3`=raw+P–W 2448
   when CD-Text is in the lead-in); `mp[8]` session format / TOC type.
2. **Power calibration** — SEND OPC (**0x54**) (`performPowerCalibration`).
   Skipped under `--simulate`; `--force` may bypass a failure (we surface it as
   exit-3, never silently).
3. **Query next writable address** — READ TRACK INFO / `getNWA` (advisory; some
   drives lie, cdrdao starts at LBA −150 regardless).
4. **Send cue sheet** — SEND CUE SHEET (**0x5D**) (`sendCueSheet`): the DAO
   layout descriptor — lead-in, each track's CTL/ADR + start, index points,
   lead-out — built from the TOC. This is the DAO "whole disc plan"; the drive
   then expects a contiguous write.
5. **CD-Text lead-in** — `writeCdTextLeadIn` (`CdTextEncoder` builds the R–W
   packs; needs the 2448 data-block-type from step 1).
6. **Lead-in gap** — write 150 sectors of zero at LBA −150 (`writeZeros`).
7. **Write audio** — WRITE(10) (**0x2A**) in `blocksPerWrite_` chunks
   (`writeData`): 2352 B/block (audio; +16 or +96 if writing subchannel). LBA
   increments. **Buffer-full handling**: on CHECK CONDITION SK=0x2 ASC=0x04
   ASCQ=0x08 ("long write in progress"), sleep and retry — this is normal flow
   control, not an error. BURN-Proof drives handle underruns in hardware; we
   still count and report them.
8. **Finish** — SYNCHRONIZE CACHE (**0x35**) + wait-ready loop (`finishDao`).
   Closes the session/lead-out.

`generic-mmc-raw` (cdrdao's `GenericMMCraw.cc`, opcode 0x2A with raw P–W and
the drive doing less) is the fallback for drives that reject the cooked cue
sheet — worth a second driver strategy but not the first target.

## 5. Module layout

```
src/write/
  dao.c          # the sequence in §4: orchestrates a burn
  wparams.c      # mode page 0x05 build/select (setWriteParameters variants)
  cuesheet.c     # TOC -> SEND CUE SHEET (0x5D) descriptor
  opc.c          # SEND OPC / power calibration
  writer.c       # WRITE(10) loop, buffer-full retry, status-map updates
  leadin.c       # CD-Text + MCN/ISRC lead-in, gap/pregap zero-fill
mmc/  (extend)   # MODE SELECT, SEND CUE SHEET, WRITE(10/12), SEND OPC,
                 # SYNCHRONIZE CACHE, CLOSE TRACK/SESSION, READ TRACK INFO
meta/ (reuse)    # CD-Text / ISRC / MCN encoders (mirror the decoders)
toc/  (extend)   # cuesheet as a write source; subchannel P/Q/R-W synthesis
```

Public API (header-first, per project convention): an opaque
`accudisc_write_job` built from TOC+BIN handles, a `accudisc_write_start` that
takes the same caller-owned status-map pointer as reads, and a blocking
`accudisc_write_run` with a progress callback — the CLI is a thin driver over
it, identical to how `read` wraps the ripping engine.

## 6. Frame-accurate write status surface

Reuse the read path's shared-memory map design verbatim (caller-owned byte per
sector, `MAP_SHARED`, atomic single-byte stores) so a caller can render an
EAC-style burn map live. New state nibble values for the write direction:

- `PENDING` (not yet written) → `WRITTEN` (WRITE(10) acked) →
  `SYNCED` (after SYNCHRONIZE CACHE covers it).
- `UNDERRUN` (a buffer-full retry hit this LBA; recovered by BURN-Proof but
  flagged so the caller knows the seam exists).
- `FAILED` (write CHECK CONDITION that retry didn't clear → fatal).

Progress = count of ≥`WRITTEN`. As with reads, the map is the single source of
truth; the `summary` line is its reduction. Semantics stay *relative*: `SYNCED`
means "the drive accepted and flushed it," **not** "verified on the platter" —
disc verification (read-back + compare/AccurateRip) is the caller's job, exactly
as on the read side.

## 7. Vendor write features (drivers, opt-in, gated)

The Plextor RE (`drivers/plextor/FEATURES.md`) mapped the write-time features.
These stay **out of the MMC core** and live behind the driver attach gate
(identify → locate driver → app permission → selftest → use), never applied
silently. They hook into §4 as follows:

| Feature | Opcode | Hook point | Notes |
|---------|--------|-----------|-------|
| **PoweRec** | `0xED` | before step 2 | optimal write power; drive-managed, query recommended speed |
| **GigaRec** | `0xE9` p0x04 | before step 1 | density 0.6–1.4×; set before write params, changes capacity |
| **VariRec** | `0xE9` p0x02 | before step 1 | manual laser power/strategy; mutually exclusive with AutoStrategy |
| **AutoStrategy** | `0xE4`/`0xE5` | before step 1 | disable to push a manual "Write Strategy" for an untested dye |
| **SecuRec** | `0xD5` auth | after step 8 | drive-side password lock on the finished disc |
| **SilentMode** | `0xE9` p0x08 | any | acoustic/speed cap; orthogonal to the burn |

Each is a `--driver`-gated CLI flag (e.g. `--px-gigarec 1.2`) that the CLI
translates to a driver call; the core `write` path is pure MMC and works on any
drive without them. SpeedRead (`0xE9` p0xBB) is read-only-relevant and already
live-verified as the selftest template.

## 8. cdrecord/schily cross-reference

Consult for: exact SEND CUE SHEET sub-field encoding across drive families;
the "long write in progress" sense variants; SAO vs raw-DAO drive quirks; and
the pre-emphasis/SCMS control-field bits. cdrdao is the primary model (cleaner
CD-DA/DAO focus); cdrecord is the tie-breaker for drive-specific behaviour.

## 9. Phasing

1. **MMC write primitives + `--simulate` DAO** — ✅ **DONE (2026-07-12).**
   Write-parameters page 0x05, READ DISC INFO blank-check, SEND CUE SHEET
   builder + parser, WRITE(10) loop + SYNCHRONIZE CACHE, `accudisc_write` +
   CLI `write --simulate`. Full simulate of the 19-track ABBA "Gold" image
   (347,175 sectors) verified on the PX-716A. `src/write/`. Not yet done from
   the plan: SEND OPC (skipped in simulate), the shared-memory status map
   (progress-fd only for now), CD-Text lead-in.
2. **Real burn** — ✅ **DONE (2026-07-12).** SEND OPC added; the full 19-track
   ABBA "Gold" image burned to a Taiyo Yuden CD-R and verified **bit-exact**
   on read-back (231200/231200 samples identical, offset 0, 0 hard errors, 0
   C2) and confirmed playable. **Byte order settled: the drive wants s16le**
   (cdrdao `GenericMMC::bigEndianSamples()==0`), so an s16be BIN needs
   `--byteswap`; net raw read/write offset is 0 on the PX-716A. Still open:
   CD-Text lead-in, and per-track verify by track.
3. **Full TOC** — multi-track, pregaps/indices, MCN/ISRC, CD-Text lead-in,
   P/Q/R–W synthesis; verify subchannel with the read path.
4. **`--sub` passthrough** — caller-supplied raw P–W for exact reproduction.
5. **Vendor write features** — GigaRec/VariRec/PoweRec/AutoStrategy/SecuRec via
   the Plextor driver, each behind its selftest gate.

## 10. Open questions (resolve before phase 1)

- **Simulate coverage**: how faithfully does the PX-716A test-write exercise the
  cue-sheet/write-params path? (Phase 1 answers this on real hardware, laser
  off.) Test-write bit is `mp[2] |= 0x10`.
- **`blocksPerWrite_` sizing** vs the drive buffer / BURN-Proof behaviour.
- **Offset on write**: the read path already owns the read-offset table; a burn
  needs the *write* offset (per-drive, different value — the user's own
  `write_offset_plextor-dvdr-px-716a.toml` exists). Query-only, explicit,
  caller-driven — never silent, same rule as read offsets.
- Does the caller ever want us to *combine* read-offset and write-offset, or
  keep them fully separate (recommended: separate; the caller composes).

## 11. Phase 3 execution plan (drafted 2026-07-23; execute next session)

Completes the write path to **all of: PCM, TOC, lead-in/out, MCN, ISRC, album
title/performer, per-track title/performer, pre-gaps, CD-Text.** CD+G and other
non-essentials are deferred. Everything is derived **purely from the .toc** (no
raw-subchannel input; cdda2img does not store one yet).

### 11.1 Context and locked decisions

- **cdda2img is concurrently deprecating cdrdao (burn) and cd-paranoia (rip) and
  replacing all call sites with AccuDisc.** So `accudisc_write()` is a *drop-in
  for cdrdao*: keep the `(toc_path, bin_path, opts)` file interface and the
  0/1/2/3 exit contract stable; do not churn the signature. cdda2img always
  hands us **raw PCM** — WAV is its `extract --raw` artifact and is **out of
  scope** for burn (no WAV/RIFF handling in the write path).
- **Test article: ABBA "Gold: Greatest Hits"** (`cdda2img/extracted/Gold:
  Greatest Hits_1.{toc,bin}`). Raw `.bin`, **s16be → burn with `--byteswap`**.
  Carries full CD-Text (disc + 19 tracks), CATALOG (MCN), per-track ISRC, and
  pre-gaps — exercises all 11 items. Audio already burns bit-exact (§9 Phase 2).
- **Burn target: CDEmu `/dev/sr1`** (`cdemu unload 0; cdemu create-blank
  --writer-id=WRITER-TOC 0 /var/tmp/cdr` — recreate after each burn). **Final
  acceptance: real Plextor `/dev/sr0`** burn.
- **CD-Text scope = mirror the decoder's v0**: block 0, single language
  (EN, code 9 from `LANGUAGE_MAP { 0: 9 }`), single-byte, pack types **0x80
  title + 0x81 performer + mandatory 0x8f size-info**. Songwriter/UPC/genre
  deferred. Non-ASCII is **byte-preserving passthrough** (consistent with the
  reader's "no character-set interpretation"; ABBA is ASCII so moot).
- **Verification is a closed loop against our own reader**: burn → read back with
  AccuDisc (`accudisc_cdtext_decode` for CD-Text, Q decode for MCN/ISRC/indices)
  → compare to the source .toc. The encoder additionally round-trips through the
  decoder with **no hardware** (see 11.4).
- **Licensing**: `CdTextEncoder.cc` / `PWSubChannel96.cc` (GPL,
  `private/code/cdrdao/`) are read-only references — **rewrite, never copy**;
  credit in `docs/reference/ATTRIBUTION.md`.

### 11.2 Step A — verify the EXISTING metadata round-trips (items MCN/ISRC/pregaps)

Before building CD-Text on top, confirm the cue-sheet path actually lands.
Burn ABBA to CDEmu with the current engine (no CD-Text), read back via AccuDisc,
and compare MCN (Q ADR 2), ISRC (Q ADR 3), and pre-gaps/indices (index-map
decode) to the .toc. **Caveat**: CDEmu's subchannel synthesis fidelity is
unknown — if it does not reproduce Q from the cue sheet, defer this check to the
Plextor (Step D). Deliverable: pass/fail on MCN/ISRC/pregaps from a real burn.

### 11.3 Step B1 — data model (src/write/write.h)

Add CD-Text carriers to the DAO model:

- `adsc_write_toc`: `char album_title[ACCUDISC_TEXT_MAX]; char
  album_performer[ACCUDISC_TEXT_MAX]; uint8_t cdtext_language; int has_cdtext;`
- `adsc_write_track`: `char title[ACCUDISC_TEXT_MAX]; char
  performer[ACCUDISC_TEXT_MAX];`

Reuse `ACCUDISC_TEXT_MAX` from the public header for symmetry with the decoder.

### 11.4 Step B2 — TOC parser CD_TEXT (src/write/tocparse.c)

Extend the line scanner (today it *ignores* CD_TEXT) to parse:

- disc-level `CD_TEXT { LANGUAGE_MAP { 0: N } LANGUAGE 0 { TITLE ".." PERFORMER
  ".." } }` → album fields + `cdtext_language = N`, `has_cdtext = 1`;
- per-track `CD_TEXT { LANGUAGE 0 { TITLE ".." PERFORMER ".." } }` → track fields.

Needs brace-depth / current-LANGUAGE tracking (the parser is line-oriented).
Ignore non-v0 items (SONGWRITER etc.). **Unit test** (`tests/test_tocparse.c` or
new): parse the ABBA .toc text, assert album + a sample of track titles/
performers, and that a .toc with no CD_TEXT sets `has_cdtext = 0`.

### 11.5 Step B3 — CD-Text encoder (src/meta/cdtext_encode.c, NEW)

Mirror `meta/cdtext.c` (decoder) + cdrdao `CdTextEncoder`. **Two layers, split so
the hard part is unit-testable without hardware:**

- **B3a — strings → 18-byte packs.** For each type (0x80, 0x81): a stream of
  NUL-terminated strings, one per track starting at track 0 (album), chunked into
  12-byte payloads across packs; set `[0]`type `[1]`track `[2]`sequence
  `[3]`block/charpos, compute the complemented CRC-16 into `[16..17]`. Emit the
  **0x8f size-info** pack(s) (char code, first/last track, per-type pack counts,
  language code). This layer is **round-trip unit-tested**: encode → prepend the
  4-byte header → `accudisc_cdtext_decode` → assert strings identical. The
  reader is the oracle; zero hardware.
- **B3b — 18-byte packs → 96-byte R-W subchannel blocks** (one per lead-in
  sector), for the write. Mirror cdrdao `PWSubChannel96`. Verified only on a real
  burn + read-back (READ TOC format 5 does the inverse extraction on read).

### 11.6 Step B4/B5 — write path (src/write/{wparams,cuesheet,burn}.c, mmc/)

- **write params**: when `has_cdtext`, set mode-page-05 data-block-type for the
  96-byte subchannel lead-in write (cdrdao `setWriteParameters` CD-Text variant).
- **cue sheet**: lead-in entry data-form → **0x41** (CD-DA with P-W) when CD-Text
  is present (`cuesheet.c`).
- **MMC**: WRITE(10) of 96-byte blocks at negative LBA. cdrdao writes CD-Text at
  `lba = -150 - leadInLen`, cycling the encoded blocks to fill `leadInLen`
  sectors, with the same buffer-full (SK2/04/08) retry as audio.
- **orchestration** (`adsc_write_run`): set params (CD-Text mode) → SEND CUE
  SHEET → **writeCdTextLeadIn** → lead-in gap → audio → SYNCHRONIZE CACHE.
- **OPEN QUESTION to resolve first**: how is `leadInLen` determined? (cdrdao
  `leadInLen_`.) Read `GenericMMC.cc` for whether it is fixed, disc-reported, or
  computed from the CD-Text size. Do not guess — this sets the write extent.

### 11.7 Step C — verify CD-Text round-trip on CDEmu

Burn ABBA (with CD-Text) to CDEmu → `accudisc_cdtext_decode` on read-back →
assert album + per-track title/performer match the .toc. If CDEmu does not return
CD-Text (READ TOC format 5), this moves to Step D.

### 11.8 Step D — real burn on the Plextor /dev/sr0 (acceptance)

Full ABBA burn: audio + MCN + ISRC + pre-gaps + CD-Text. Read **everything** back
with AccuDisc and confirm all 11 items round-trip. **Definition of done:** a real
Plextor burn whose PCM is bit-exact AND whose MCN, every ISRC, every pre-gap, and
all CD-Text (album + per-track title/performer) read back identical to the source
.toc via AccuDisc's own read path.

### 11.9 Cross-cutting

- **Commit per logical step** through `scripts/sync.py` (build+test gate).
- **Update §9 phase status** as steps land; retire §10's stale "resolve before
  phase 1" framing.
- **API stability for cdda2img**: no `accudisc_write()` signature change — all 11
  items flow in through the .toc. If a field must grow, it is
  `accudisc_write_opts` (documented "provisional") not the function shape.
- **Risks to watch**: CDEmu subchannel/CD-Text read-back fidelity (may force
  Plextor-only verification); the 96-byte subchannel write-params mode may differ
  CDEmu vs Plextor; `leadInLen` (11.6).
