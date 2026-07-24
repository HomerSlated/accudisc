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

## 11. Phase 3 execution plan (drafted 2026-07-23; revised 2026-07-24)

Completes the write path to **all of: PCM, TOC, lead-in/out, MCN, ISRC, album
title/performer, per-track title/performer, pre-gaps, CD-Text.** CD+G and other
non-essentials are deferred.

Everything *except CD-Text* is derived **purely from the .toc** (no raw-subchannel
input; cdda2img does not store one yet). **CD-Text is different as of the
2026-07-24 revision**: it arrives as the raw `READ TOC` format-0x05 blob and is
passed through unmodified — see 11.1.

### 11.1 Context and locked decisions

- **cdda2img is concurrently deprecating cdrdao (burn) and cd-paranoia (rip) and
  replacing all call sites with AccuDisc.** So `accudisc_write()` is a *drop-in
  for cdrdao*: keep the `(toc_path, bin_path, opts)` file interface and the
  0/1/2/3 exit contract stable; do not churn the signature. cdda2img always
  hands us **raw PCM** — WAV is its `extract --raw` artifact and is **out of
  scope** for burn (no WAV/RIFF handling in the write path).
- **Test article: ABBA "Gold: Greatest Hits"** (`cdda2img/extracted/Gold:
  Greatest Hits_1.{toc,bin}`). Raw `.bin`, **s16be → burn with `--byteswap`**.
  Carries CATALOG (MCN), per-track ISRC, and pre-gaps. Audio already burns
  bit-exact (§9 Phase 2).
  - **CORRECTION 2026-07-24 — ABBA carries NO CD-Text on the pressing.** The
    2026-07-23 draft claimed "full CD-Text (disc + 19 tracks)"; that was wrong.
    `accudisc cdtext --device /dev/sr0` on the real disc returns *absent*. The
    `CD_TEXT` blocks in cdda2img's `.toc` are **synthesised by their `toc.py`
    from MusicBrainz metadata**, not lifted from the disc.
  - **Consequence: ABBA cannot serve as the Step D real-disc CD-Text acceptance
    article** — there is no original blob to round-trip against. It remains the
    article for audio/MCN/ISRC/pregaps.
  - **The general rule this exposes (cdda2img, §38.3): an RBI does not preserve
    the original CD-Text blob.** It stores a *generated* TOC whose `CD_TEXT` is
    synthesised from looked-up metadata. So **no stored image can round-trip an
    *original* pressing's CD-Text, however many of them we hold** — that would
    need a physical disc carrying real CD-Text. This is the trap ABBA sprang, and
    it would spring identically on ZZ Top, Tracy Chapman, Green Day or Avril
    Lavigne.
  - **RESOLVED 2026-07-24 (Keith): there is no original-CD-Text pressing, and one
    is unlikely ever to be acquired — "the only CD-Text discs we will ever have
    are those we create."** (Full rationale in the cdda2img correspondence.) This
    *dissolves* the requirement rather than blocking on it: pass-through v0 never
    reproduces a pressing, it round-trips **a valid blob we already hold**. Step D
    CD-Text acceptance is therefore *write a blob we author → burn to real media →
    read it back byte-identical*, with the blob sourced from our captures
    (`libmirage_abba_gold__42packs.cdtext`, or a synthesised fixture), **not** from
    any disc's lead-in. The RBI limitation above still stands as a recorded fact,
    but it is no longer on the critical path — we are validating that our write
    path lays down and recovers whatever packs it is given, which needs no
    original disc at all.
- **Burn target: CDEmu `/dev/sr1`** (`cdemu unload 0; cdemu create-blank
  --writer-id=WRITER-TOC 0 /var/tmp/cdr` — recreate after each burn). **Final
  acceptance: real Plextor `/dev/sr0`** burn.
- **CD-Text v0 is PASS-THROUGH, not authored** (locked 2026-07-24, at cdda2img's
  request — their §32.1, our reply of 2026-07-24). `accudisc write --cdtext FILE`
  consumes **byte-for-byte what `accudisc read --cdtext FILE` emits**: the whole
  `READ TOC` format-0x05 response, header included. Rationale: rip→burn becomes
  bit-exact because there is no decode/re-encode step in which loss could occur —
  and it is *less* work for us (no string encoder, no charset logic, no CRC
  generation; a drive already produced well-formed packs).
  - Consequence: **we never read the payload bytes**, so we cannot transcode or
    "correct" a charset declaration even in principle. That matters for the real
    non-conforming discs cdda2img hit — cdrdao-authored and CDEmu-mounted images
    carry raw UTF-8 while declaring charset 0x00 — which now round-trip exactly.
  - **Authored mode (strings / `.toc` `CD_TEXT` blocks → packs) is COMMITTED as a
    second mode, sequenced after v0** (Keith, 2026-07-24 — promoted off "deferred",
    now on cdda2img's migration critical path). *Why it can't stay optional:*
    cdda2img has **no strings→packs encoder** (`cdtext.py` is decode-only); cdrdao
    is what encodes their `CD_TEXT` strings at burn time today, so the moment
    cdrdao leaves, a *fresh* disc authored from MusicBrainz metadata has no CD-Text
    unless AccuDisc encodes it. Pack encoding is bit-formatting — libaccudisc's job
    by scope, and a mirror of the decoder we already ship — so it belongs here, not
    in the caller. cdda2img's interim until this lands is **re-burn-only** (they
    are *not* porting their own encoder). **Locked first-cut scope (cdda2img §43,
    grounded in what their `generate_toc` actually authors):** block 0, single
    language, single-byte charset, pack types **0x80 title (disc + per-track) +
    0x81 performer (disc + per-track) + 0x86 disc-id (disc-level, conditional on
    the field being set) + mandatory 0x8f size-info**. Note 0x86 is *not* in our
    current decoder (`src/meta/cdtext.c` decodes 0x80/0x81 only) — it is new to
    us, but cheap: the encoder is pack-type-agnostic, so 0x86 is one more type
    byte, not a subsystem. Explicitly **out**: 0x82 songwriter (never authored),
    and 0x8e UPC/ISRC — MCN/ISRC are Q-subcode directives (`CATALOG`/`ISRC` →
    cuesheet → Q subchannel), not CD-Text packs, so encoding them here would
    duplicate metadata that already reaches the disc another way. An unencodable
    codepoint fails **before** the burn (§11.9 INVARIANT rule 4), never silently
    dropped. v0 pass-through still ships first (simpler, and it is what makes
    re-burns work).
- **Verification is a closed loop against our own reader**: burn → read back with
  AccuDisc (format-05 blob for CD-Text, Q decode for MCN/ISRC/indices) → compare
  to the source. For CD-Text the comparison is now a **byte-for-byte blob
  compare**, which is strictly stronger than the string compare the authored path
  would have allowed.
- **Licensing**: `CdTextEncoder.cc` / `PWSubChannel96.cc` (GPL,
  `private/code/cdrdao/`) are read-only references — **rewrite, never copy**;
  credit in `docs/reference/ATTRIBUTION.md`.
- **Golden reference — capture it before cdda2img deletes cdrdao** (Keith,
  2026-07-24). Our own reader agreeing with our own writer is a closed loop that a
  *systematic* error in the R-W packing would pass perfectly; cdrdao is the last
  independent CD-Text writer on the machine. So: **burn ABBA *Gold* with cdrdao,
  read the result back with `accudisc read --cdtext`, keep that blob**, plus the
  source blob — input → cdrdao → output as a triple. **On CDEmu, not real media**
  (Keith's amendment — costs no blank).
  - **OUTCOME 2026-07-24 — ran clean, but the artifact is NOT cdrdao's packing.**
    CDEmu *does* return CD-Text on READ TOC format 5 (760 B, 42 packs, all CRCs
    valid — verified independently with our own code), so the predicted failure
    did not occur. A subtler one did: **CDEmu decodes the packs to text and
    re-encodes on read.** `/var/tmp/cdr.toc` holds decoded `CD_TEXT { TITLE … }`
    blocks, so cdrdao's actual R-W stream was consumed and discarded at the
    virtual writer. The blob is **libmirage's packing**.
    - **Therefore no virtual writer can ever produce the cdrdao oracle** — a text
      round-trip destroys packing by construction. Getting cdrdao's genuine
      packing needs a real burn to real media: **open, pending a fresh decision
      from Keith. Do not assume into it.**
    - **What it IS good for, and it is not nothing:**
      `libmirage_abba_gold__42packs.cdtext` (+ `abba_gold_source.toc`, the exact
      input) is an independently-encoded, well-formed blob from outside AccuDisc.
      As a **pass-through acceptance fixture it is ideal** — feed it in, read
      back, demand byte-identity. That breaks the closed loop against libmirage
      rather than cdrdao: weaker than asked for, strictly better than
      self-referential.
    - **42 % 4 = 2 — a third counterexample to the multiple-of-4 rule**, and the
      first produced by an *encoder* rather than read off a drive.
  - **Scope limit** (cdda2img's §35.3, and it is the sharper point): cdrdao cycles
    pre-built blocks and therefore *cannot* emit a non-multiple-of-4 stream, so
    this reference validates the **4-aligned path only**. The 33- and 35-pack
    ring-fill cases have a weaker oracle — drive acceptance plus reader
    round-trip — and a failure there will present as a puzzle, not a diff.
- **Test fixtures stay private** (Keith, 2026-07-24). cdda2img's three real
  captures live in `private/research/incoming/cdtext-captures/` (git-ignored;
  commercial-pressing metadata). Tracked unit tests therefore run against
  **synthesised** 33- and 35-pack fixtures, with the real captures as a local-only
  acceptance set.

### 11.2 Step A — verify the EXISTING metadata round-trips (items MCN/ISRC/pregaps)

Before building CD-Text on top, confirm the cue-sheet path actually lands.
Burn ABBA to CDEmu with the current engine (no CD-Text), read back via AccuDisc,
and compare MCN (Q ADR 2), ISRC (Q ADR 3), and pre-gaps/indices (index-map
decode) to the .toc. **Caveat**: CDEmu's subchannel synthesis fidelity is
unknown — if it does not reproduce Q from the cue sheet, defer this check to the
Plextor (Step D). Deliverable: pass/fail on MCN/ISRC/pregaps from a real burn.

### 11.3 Step B1 — data model + CLI intake (src/write/write.h, cli/main.c)

The pass-through decision makes this small — the model carries a **blob**, not
decoded strings:

- `adsc_write_toc`: `const uint8_t *cdtext; uint32_t cdtext_len;` (borrowed
  pointer, caller-owned, NULL = no CD-Text). No `album_title` / `performer` /
  `cdtext_language` fields in v0 — those belong to the deferred authored mode.
- `cli/main.c` `cmd_write`: add `--cdtext FILE`, sitting alongside the existing
  `--toc` / `--bin` / `--simulate` / `--byteswap` / `--speed` / `--progress-fd`.
  Load the file whole (it is a few KiB), pass pointer+len through
  `accudisc_write_opts`. **No `accudisc_write()` signature change** — cdda2img is
  mid-migration off cdrdao and the function shape is frozen (11.9).

### 11.4 Step B2 — blob validation (src/meta/cdtext.c or new cdtext_blob.c)

Replaces the old "parse `CD_TEXT` out of the .toc" step, which is **deferred to
the authored mode (v1)**. `src/write/tocparse.c` keeps ignoring `CD_TEXT`.

Validate the incoming format-05 blob *without interpreting the payload*:

```
[0..1] TOC data length (big-endian, = len-2)   <- READ TOC header
[2..3] reserved
[4..]  N x 18-byte CD-Text packs
```

- `len >= 4 + 18` and `(len - 4) % 18 == 0`; header length field cross-checked
  against the buffer size, **refuse on inconsistency** (some drives pad).
  cdda2img has no counterexample to the cross-check in four captures — treat that
  as "none found", not "never happens".
- **Pack count is NOT required to be a multiple of 4.** The 2026-07-23 draft
  planned to refuse otherwise; **cdda2img's §34.1 killed that with data and it
  must not ship**. **Two of their three** captures fail it — 33 packs
  (`cdemu_utf8`) and 35 packs (a redumper dump off `PLEXTOR - DVDR PX-716A`, our
  own drive model). Neither is truncated: sequence numbers run contiguously
  (verified 0–32 and 0–34 with our own code) and both end with the 3-pack
  `SIZE_INFO` trio, which is exactly what a truncated capture would lack. *Caveat
  on that last point:* the Technotronic trio's third pack fails CRC, so **our
  decoder sees a pair, not a trio** — do not assert on the trio.
  The 4-packs-per-96-byte-block relation governs the *media
  encoding*; the drive hands back one de-duplicated logical stream whose length
  has no such constraint. Consequence for the writer: see 11.5.
- **CRC-16 check every pack**, but with a **three-way** outcome, not two. Verified
  2026-07-24 against cdda2img's delivered captures
  (`private/research/incoming/cdtext-captures/`) using our own `adsc_crc16`:
  - **CRC valid** → write.
  - **CRC field is zero (0x0000)** → **transport artifact, not damage.** The
    35-pack `technotronic_px716a` capture — real hardware, `PLEXTOR - DVDR
    PX-716A` rev 1.11, i.e. our own drive model — has exactly this on its **last**
    pack, a `SIZE_INFO` (0x8f) whose payload is intact and plausible (language
    code 0x09 = English in the right slot) but whose stored CRC is all zeroes.
    The drive dropped the check field in transport; the payload survived.
    Refusing here would reject a real capture, and passing it through verbatim
    would burn a pack that every conforming reader discards — including **our
    own decoder, which already skips it** (`src/meta/cdtext.c`: "corrupt pack:
    skip, never guess"). So a strict pass-through would silently produce a disc
    *worse* than its source. **DECIDED (Keith, 2026-07-24): recompute the CRC for
    zero-CRC packs only**, payload untouched, noted on stderr. This regenerates a
    check field the transport lost; it does not alter content — the payload is
    never read, interpreted or transcoded, only the 16 bits that exist solely to
    validate it, derived from that same payload. The rule is mechanical rather
    than a judgement call: all-zeroes is a signature, wrong-but-nonzero is damage.
    **This is the one place where "AccuDisc only moves bits" carries an
    asterisk** — document it in the man page and `cli-machine-interface.md`, not
    only here.
    *Mechanism — resolved 2026-07-24 from redumper's source, and it is NOT what
    either side inferred.* cdda2img concluded from a 4-capture table that the
    zeroing "follows redumper's dump path" and is therefore one tool's quirk (an
    edge case). Reading the source says otherwise:
    - **redumper does not zero anything.** Its dump path writes the raw READ TOC
      format-05 buffer verbatim — `write_vector(cdtext_path, cd_text_buffer)`
      straight from `cmd_read_toc`, no CRC handling at all
      (`private/code/redumper/cd/cd_common.ixx:190-195`). It cannot be the origin.
    - **redumper independently documents it as a PLEXTOR DRIVE behaviour.**
      `cd/toc.ixx:422-428` hard-codes an exemption for the final descriptor:
      `// PLEXTOR PX-W5224TA: crc of last pack is always zeroed`, skipping the
      check when `i + 1 == descriptors_count`. A separate project, on a
      *different* Plextor model, hit the same thing and blamed the drive.
    - **So it is a Plextor-family behaviour, i.e. our own drive** — which makes it
      the *expected* case in our environment, not an edge case. The stderr note
      should therefore **not** be quietened (cdda2img's §37.3 suggestion rests on
      the wrong premise).
    - **Unresolved, and stated as such:** `stanley_road` was captured off the
      *same PX-716A* with our own tooling and its final-pack CRC is **valid**, so
      "always zeroed" does not hold for the 716A. Firmware, disc, or transport
      differences remain candidates. **Hypothesis worth testing** (cheap, and it
      connects to F-001): an allocation-length short transfer would leave the
      final pack's trailing 2 bytes at their zero-initialised value — i.e. the
      "zeroed CRC" would be a *truncated read*, which our `resid` check now
      detects. Test: re-capture Technotronic with our own tooling and see whether
      the zero reproduces and whether `resid` is non-zero.
  - **CRC non-zero and wrong** → damage. Refuse the burn; escape flag writes
    verbatim anyway.
- **Run the whole of this validation at intake, before any media is touched** —
  before SEND CUE SHEET and before the first write — so a bad capture costs an
  error message rather than a blank (cdda2img's request, §34.1).
- **Consistency warning (not an error)**: the 0x8f size-info pack declares
  first/last track. If that disagrees with the `.toc` being burned, write the
  bytes as given, warn on stderr, **exit 3** ("completed with caveats") — the
  decision stays with the caller.

Unit-testable with zero hardware: feed it a captured ABBA blob plus mutations
(odd length, bad header length, 3-pack stream, flipped CRC byte).

### 11.5 Step B3 — packs → 96-byte R-W blocks — LANDED 2026-07-24 (src/meta/cdtext_encode.c)

Only the *second* layer of the original two-layer plan survives; B3a
(strings → 18-byte packs) moves to the deferred authored mode.

**18-byte packs → 96-byte R-W subchannel blocks**. The lead-in carries CD-Text as
6-bit R-W symbols: **3 bytes → 4 symbols** (MSB first), each symbol in the low 6
bits of one byte, so 96 symbols = 72 bytes = **exactly 4 packs per block**. This is
a *plain* bit packing — **CD-Text uses no Reed-Solomon and no interleave** (unlike
CD+G program-area R-W in `src/cdda/rw.c`); its error protection is the per-pack CRC
that B2 checks. Confirmed against cdrdao `PWSubChannel96::setRawRWdata` (the "used
for CD-TEXT" path) and its inverse `getRawRWdata`. `adsc_cdtext_encode_rw` +
`adsc_cdtext_rw_block_count`; the top two bits (P, Q) are left for the burn path.

**Ring fill — the consequence of 11.4's dropped multiple-of-4 rule.** With a pack
count not divisible by 4 (33 and 35 both occur in the wild), the last block of one
pass through the stream is short. Do **not** pad it with synthesised packs — that
is invention, which is the one thing pass-through exists to prevent. Instead treat
the pack stream as a **ring and fill blocks continuously across the wrap**: block b
carries packs `(4b .. 4b+3) mod npacks`. The minimal set that tiles seamlessly is
`lcm(npacks, 4) / 4 == npacks / gcd(npacks, 4)` blocks (33 → 33, 35 → 35, 42 → 21),
which the burn path then cycles to fill the extent (11.6). Every block is fully
populated with real packs and nothing is invented.

**CORRECTION 2026-07-24 — this is NOT a divergence from cdrdao; it matches it.**
The earlier draft (and cdda2img §35.3) claimed cdrdao "cycles pre-built blocks
rather than packs and so can only ever emit multiples of 4", making ring-fill our
speculative divergence. Reading the source refutes that: `CdTextEncoder::
buildSubChannels` **ring-fills packs** — it walks the pack list gathering four at a
time with wraparound (`if ((prun = prun->next_) == NULL) prun = packs_`) and its
`packCount % 4` switch produces exactly `lcm(npacks,4)/4` blocks (`%4==0 → /4`,
`%4==2 → /2`, odd → `npacks`), closing the ring (`assert(prun == packs_)`). So our
behaviour is **identical to shipping cdrdao**, which burns real non-multiple-of-4
CD-Text discs — it is *proven*, not a hypothesis awaiting Step C/D. (Told cdda2img.)

**Unit test (landed, hardware-free)**: `tests/test_cdtext_encode.c` writes the
inverse extraction (96-byte block → 4 packs, mirroring `getRawRWdata`) and asserts
`packs → blocks → packs` is the identity for 1/2/3/4/6/8/33/35/42 packs — the wild
non-multiples included — plus the exact cdrdao bit vector (FF,00,FF → 3f,30,03,3f)
and that no output byte uses the P/Q bits. Clean under clang ASan+UBSan. The
hardware round-trip (Step C/D) is confirmation, not the only evidence.

### 11.6 Step B4/B5 — write path — LANDED 2026-07-24 (src/write/{wparams,cuesheet,discinfo,burn}.c)

**As built.** `adsc_write_run` now: set params (data block type 3 when CD-Text)
→ blank check + lead-in geometry → OPC → SEND CUE SHEET (lead-in entry data form
0x41 + lead-in start MSF when CD-Text) → **`write_cdtext_leadin`** → lead-in gap →
audio → SYNCHRONIZE CACHE. The lead-in write cycles the B3 block set over
`di.leadin_len` sectors at **96 bytes per block** starting at
`-150 - leadin_len`, ending exactly where the gap begins.
- **96 bytes/block, not 2448** — cdrdao's `writeCdTextLeadIn` transfers `n * 96`;
  the drive generates the main channel. Data block type 3 is set on the mode page
  because MMC-3 requires it to *enable* P-W lead-in writing, but it does not
  govern this transfer's size. Audio still writes at 2352.
- **Mode-page fallback**: a drive that rejects data block type 3 gets a retry with
  it cleared (cdrdao's `WMP_VAR_CDTEXT_NO_DATA_BLOCK_TYPE`) rather than a failed
  burn.
- **`adsc_leadin_len_from_msf`** factored out pure and unit-tested
  (`tests/test_discinfo.c`): in-window starts give `450000 - startLBA`; at/above
  100:00:00, below 80:00:00, or garbage falls back to 4500 — a zero-length lead-in
  would write nothing and an underflow would request ~4 billion sectors.
- **Deferred B2 surfacing now wired**: `adsc_write_load_model` takes a
  `adsc_cdtext_info` out-param and `accudisc_write` logs any zero-CRC
  regeneration. *Still open*: the SIZE_INFO-vs-`.toc` mismatch warning → exit 3,
  which needs a "completed with caveats" return path (an API contract change
  cdda2img depends on — decide before adding).
- Tests: cue-sheet CD-Text lead-in entry (0x41 + MSF, and that a blob without
  disc info falls back rather than emitting a bogus MSF); lead-in geometry.
  Suite 23/23. **Untested without hardware**: the lead-in WRITE(10) itself —
  that is Step C.

Original design notes follow.


- **write params**: when CD-Text is present, set mode-page-05 data-block-type for
  the 96-byte subchannel lead-in write (cdrdao `setWriteParameters` CD-Text
  variant).
- **cue sheet**: lead-in entry data-form → **0x41** (CD-DA with P-W) when CD-Text
  is present (`cuesheet.c`).
- **MMC**: WRITE(10) of 96-byte blocks at negative LBA, starting at
  `lba = -150 - leadInLen`, with the same buffer-full (SK2/04/08 = "not ready,
  long write in progress" → sleep 40 ms, retry) handling as audio.
- **`leadInLen` — RESOLVED 2026-07-24** (was the blocking open question; answer
  read out of `GenericMMC.cc:414-432` and `:1344-1397`, not guessed):
  - It comes from **READ DISC INFORMATION (0x51) bytes 17-19** = lead-in start
    MSF. If that start is **>= 80:00:00** then `leadInLen = 450000 - startLBA`
    (450000 = MSF 100:00:00); otherwise the fallback is **1 minute = 4500
    sectors** (with a 30 s lead-out instead of the usual 90 s).
  - The encoded block list is then **cycled repeatedly to fill the entire
    lead-in** (`scp` wraps at the block count).
  - **Therefore the lead-in extent is a property of the blank media, not of the
    pack count** — which is why pass-through works at any (multiple-of-4) pack
    count, and why nothing in the write path needs to know how much CD-Text
    there is.
- **orchestration** (`adsc_write_run`): set params (CD-Text mode) → SEND CUE
  SHEET → **writeCdTextLeadIn** → lead-in gap → audio → SYNCHRONIZE CACHE.

### 11.7 Step C — verify CD-Text round-trip on CDEmu — **PASSED 2026-07-24**

Burn ABBA (with `--cdtext <blob>`) to CDEmu, then `accudisc read --cdtext` the
result and **compare the returned blob byte-for-byte against the one we fed in**.
Decode both sides as a human-readable diagnostic on mismatch, but the blob
compare is the assertion — pass-through means anything less is a weaker test than
the contract we promised cdda2img.

**RESULT — clean pass, first attempt.** Article: `libmirage_abba_gold__42packs.cdtext`
(760 B) fed to a full 19-track, 347208-sector ABBA burn on the CDEmu blank.

- **CD-Text: 760 bytes in, 760 bytes out, `cmp` IDENTICAL.** The blob carries real
  content (`Gold: Greatest Hits`, `Dancing Queen`, `Knowing Me, Knowing You`…), so
  this is not two identically-empty buffers agreeing.
- The lead-in write behaved exactly as B3/B4 predicted:
  `cdtext: 42 packs -> 21 R-W blocks, filling 10927 lead-in sectors from LBA -11077`
  — 21 = `lcm(42,4)/4` (the ring-fill count), and `-11077 = -150 - 10927`.
- Media-derived extent confirmed live: `leadin_len` 10927 came from the blank, not
  from the pack count, exactly as 11.6 argues.
- **Everything else round-tripped too** (not required by Step C, but free with the
  disc in hand): 19 tracks, lead-out 347208, `source=fulltoc degrade=none`; MCN
  `0731451700729` and per-track ISRCs (`SEAYD7601020`, `SEAYD7601050`, …) read back
  off the burned Q identical to the source `.toc`.
- **`--byteswap` NOT used**: the source here is WAV-derived raw, which is s16**le**.
  The byteswap rule applies to cdda2img's s16**be** BINs — a distinction worth
  keeping straight, as it silently destroys audio rather than failing.

Reproduce: strip the 44-byte RIFF header (`tail -c +45`) to get 347208 exact
sectors, `cdemu create-blank --writer-id=WRITER-TOC 0 /var/tmp/cdr`, burn, then
`accudisc cdtext FILE` and `cmp`.

### 11.8 Step D — real burn on the Plextor /dev/sr0 (acceptance)

Full ABBA burn: audio + MCN + ISRC + pre-gaps + CD-Text. Read **everything** back
with AccuDisc and confirm all 11 items round-trip. **Definition of done:** a real
Plextor burn whose PCM is bit-exact, whose MCN, every ISRC and every pre-gap read
back identical to the source .toc, and whose **CD-Text blob is byte-identical to
the one supplied** — all via AccuDisc's own read path.

Note the CD-Text blob here is **one we author** (a held capture — the 42-pack
libmirage fixture — or a synthesised blob fed through `write --cdtext`), not
ABBA's own lead-in, which carries none (§11.1). This is not a compromise for
pass-through v0: the property under test is "the write path lays down and recovers
the exact packs it was given", and any valid blob exercises it.

**UPDATE 2026-07-24 — a live CD-Text article now exists: Paul Weller, *Stanley
Road*.** It had been set aside on two grounds; the odd-length READ TOC fix
(`8bda198`) deleted one of them. The disc is 12 tracks — `37 + 11*12 = 169`, odd —
so it was in the exact failure class and read `degrade=leadin_unreadable`; cdda2img
re-tested it on the fixed build and it now reads `source=fulltoc degrade=none`
(their §47, hardware we do not have). It carries **genuine on-disc CD-Text** (disc
`TITLE "Stanley Road"` / `PERFORMER "Paul Weller"` + `SIZE_INFO`).
- **It is a *stronger* article than a rich disc**, which is the non-obvious part:
  its per-track blocks carry **empty-string fields** (empty track-1 `PERFORMER`,
  empty track-12 `TITLE`) beside a populated disc block and a real `SIZE_INFO`.
  Empty strings, NUL-separator runs and a non-trivial SIZE_INFO are precisely the
  shapes an encoder fumbles, and a byte-for-byte compare over them is a harder test
  than a tidy capture. When authored mode lands it gives a *real* round-trip target
  rather than a synthesised one.
- This does **not** contradict §11.1's "no original-CD-Text pressing" decision —
  Stanley Road is a CD-R, not a pressing. What changed is that a physical disc
  carrying CD-Text we did not author is now readable, which is all Step D needed.
- Captures requested from cdda2img: the raw format-05 blob (`read --cdtext`) and
  the raw full TOC. Those are what Step C/D consume.

### 11.9 Cross-cutting

- **FIXED 2026-07-24 (was: MUST-FIX BEFORE ANY PHASE 3 BURN) — `adsc_toc_parse_cue`
  was directive-injectable through a quoted string** (found 2026-07-24 while
  reviewing cdda2img's §39.2; reproduced, not theorised). The scanner in
  `src/write/tocparse.c` is
  line-oriented and does **not** track quote context, so a literal newline inside
  a `CD_TEXT` `TITLE "…"` value is parsed as TOC directives. Measured, same
  parser, same base TOC:

  | input | rc | ntracks | lead-out | track 3 ISRC |
  |---|---|---|---|---|
  | clean | 0 | 2 | 35345 | — |
  | title carries `\nFILE …\nTRACK AUDIO\nISRC "ZZZZZ9999999"\n` | **0** | **3** | **39845** | **`ZZZZZ9999999`** |

  It returns **`ACCUDISC_OK`** — a clean success — for a layout with a phantom
  track, shifted boundaries, a changed lead-out and an attacker-chosen ISRC. Burnt,
  that is a wrong disc reported as a good one: **our own violation of the
  invariant below.**
  - **Reachability:** the `.toc` is generated by cdda2img from MusicBrainz
    free text (user-editable). They escape it (`escape_toc_string`, their
    GRD-2026-0531-01) *specifically* to stop this — so today we are safe only
    because the caller sanitises. `accudisc_write()` is public API; any caller can
    hand us a `.toc`, and we must not depend on someone else's escaping.
  - **Fix as landed:** the line-scan now tracks quote context and rejects an
    unterminated quote at end-of-line with `ACCUDISC_ERR_INVAL`; `parse_qstr` also
    stops at a newline for defence in depth. That matches cdrdao's own grammar
    (its flex lexer does not let a quoted string span lines), so it makes the
    parse *correct* rather than merely refusing. Regression added to
    `tests/test_tocparse.c` (the injected TOC above → `ACCUDISC_ERR_INVAL`, plus a
    balanced single-line quoted value → `ACCUDISC_OK`); 19/19 tests pass.
    **Still owed: tell cdda2img it landed** — until they hear so, they must keep
    `escape_toc_string` as the sole guard; after, it becomes defence in depth.
  - **REVIEW QUESTION, adopted from cdda2img §40.3 — apply to every parser, not
    just this one:** *"what does this accept if the producer is hostile, or merely
    wrong?"* Three instances of the identical shape surfaced in one session across
    three projects — cdrdao assumed its producer supplied encodable strings; we
    assumed ours supplied escaped ones; cdda2img's `import` assumes a foreign
    `.toc` is cdrdao-shaped and benign. In each case **the boundary was trusted
    because the usual producer happens to be well-behaved.** Worth a sweep of our
    other parsers (drive responses in `mmc/`, `cdda/subq.c`, `meta/cdtext.c`,
    `toc/`) against that question rather than three separate fixes.
- **INVARIANT — exit 0 must never mean "burned the disc but silently dropped
  metadata."** Learned from a real cdrdao burn on 2026-07-24 that cost a Taiyo
  Yuden blank: cdrdao logged `CD-TEXT writing is supported` and `Writing CD-TEXT
  lead-in...`, then produced a disc with **no CD-Text at all** and **exited 0**.
  Cause: one `U+2010 HYPHEN` in one track title (from MusicBrainz, missed by
  cdda2img's sanitiser) was unencodable in the CD-Text charset, so cdrdao dropped
  **all 20 blocks** and continued. Verified in source:
  `trackdb/CdTextItem.cc:305-315` — `updateEncoding()` logs and returns **void**,
  so the failure cannot propagate to a caller even in principle. Its message is
  itself broken: `log_message(-2, "CD-TEXT: Unable to encode \"%s\" into
  compatible format")` — `%s` with **no argument** (undefined behaviour, prints
  garbage varargs), so it cannot even say which string failed.
  Rules this fixes in place for us:
  1. **Validate at intake, before SEND CUE SHEET and before any write** (already
     11.4) — this is the case that proves the rule.
  2. **A metadata failure is a hard failure, not a downgrade.** If any part of the
     requested CD-Text cannot be written, refuse the burn. Never write the audio
     and drop the metadata.
  3. **Never a bare `%s` without its argument**, and every diagnostic names the
     offending item. Our stderr is cdda2img's end-user text (their §32.5), and
     they have no fallback engine behind us.
  4. When the **authored mode** (v1) lands, an unencodable string fails *before*
     the burn — never "encode what fits and continue".
- **Commit per logical step** through `scripts/sync.py` (build+test gate).
- **Update §9 phase status** as steps land; retire §10's stale "resolve before
  phase 1" framing.
- **API stability for cdda2img**: no `accudisc_write()` signature change — all 11
  items flow in through the .toc. If a field must grow, it is
  `accudisc_write_opts` (documented "provisional") not the function shape.
- **Risks to watch**: CDEmu subchannel/CD-Text read-back fidelity (may force
  Plextor-only verification); the 96-byte subchannel write-params mode may differ
  CDEmu vs Plextor. (`leadInLen` is no longer a risk — resolved, 11.6.)
- **Not Phase 3, but owed to cdda2img from the same exchange** (tracked so they
  are not lost with the plan): freeze `--retries` / `--c2-retries` / `--verify` /
  `--overlap` / `--ladder` in `cli-machine-interface.md` (including that
  `--retries` defaults to **2**, the one flag that does carry a default); fix the
  unguarded `strtol` → `uint8_t` truncation on all five (`--retries 256` silently
  becomes 2 today); document that `speeds` reports `page2a=0` as *"not reported"*,
  not *"quantized to zero"*, since their ladder rule `req == page2a` would
  otherwise admit nothing on a drive with no usable mode page 2A.
