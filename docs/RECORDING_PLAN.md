# AccuDisc — Recording (DAO write) implementation plan

Status: **plan only.** The write/burn path has been paused; this document is
the design to execute when it resumes. Scope is Red Book CD-DA Disc-At-Once,
matching the read path's philosophy: a raw, machine-driven engine that only
moves bits. Primary reference is **cdrdao** (`reference/cdrdao/dao/`), with
**cdrecord/schily** (`reference/schily-2024-03-21/`) as a cross-check for
command details and drive quirks. Both are credited in `docs/ATTRIBUTION.md`.

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
2. **Real single-track audio burn** — one track, no CD-Text, verify by
   read-back + `cmp` against the source BIN (offset-corrected). This is where
   the s16be/s16le SWABAUDIO byte order gets settled (the read-back is the
   oracle) and SEND OPC (power calibration) is added.
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
