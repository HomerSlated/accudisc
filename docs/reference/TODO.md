# AccuDisc — deferred work

Ideas parked for a later session. Not scheduled; not commitments. Recovery
methods are considered complete (see `docs/ACCUDISC_RECOVERY.md`); this is
everything else worth remembering.

## NEXT SESSION — PLAN (agreed 2026-07-16). Execute Phase 0 first.

Phases 0/1/3 are a chain (0 gates the rest); Phase 2 is independent and can be
done any time. All of it was designed against measurements taken 2026-07-16 —
see "disc-init governor — SOLVED" below for the evidence.

### GOVERNING PRINCIPLE — probe, don't bake
Everything we learned about the PX-716A is **runtime-derivable from generic
MMC**. Nothing may be hardcoded, or we ship "PlexTools for Linux" instead of
AccuDisc:

| fact | scope | handling |
| --- | --- | --- |
| performance curve | generic MMC, **but CDEmu rejects it** | probe; failure => report *unknown*, never infer |
| CLV/CAV/P-CAV/Z-CLV | derived from curve SHAPE | generic derivation from drive-supplied data |
| curve endpoints (17-40x) | per-drive, per-medium | comes from the drive |
| speed ladder {4,8,24,32} | per-drive | probe by set->readback; never a table |
| governor ceiling | Plextor-documented; unknown elsewhere | read current_x/max_x; **report the fact, do NOT interpret** |
| SpeedRead (0xE9) | Plextor vendor | already isolated in drivers/plextor |

The PlexTools risk is **semantics, not opcodes**: shipping "current_x < max_x =>
this disc is damaged" bakes a Plextor firmware behaviour into a general tool.
CLAUDE.md forbids it ("no analysis; AccuDisc only moves bits") — report the raw
values, let the calling app decide.
**Test both drives every phase:** /dev/sr0 = PX-716A, /dev/sr1 = **CDEmu**
(virtual; advertises Real-Time Streaming then rejects GET PERFORMANCE — free
negative control for anything that trusts a feature bit over a smoke test).

### PHASE 0 — fix SET STREAMING so the drive actually accepts it — DONE 2026-07-17
Two independent bugs, both fixed and hardware-verified on the PX-716A:

1. **THE showstopper (found this session): CDB Parameter List Length offset.**
   We wrote the 2-byte length at CDB bytes **8-9** (what I misremembered as the
   MMC-5 position). It belongs at bytes **9-10** — SET STREAMING is the one MMC
   command that shifts its length field a byte off the normal Group-5 slot.
   schily flags it `/* Sz not G5 alike */` (cdrecord/scsi_mmc.c:991). With 8-9
   the drive reads byte 9 (0x1C) as the length MSB, expects 0x1C00 = 7168 bytes,
   gets 28, and rejects the whole command with **4/1b (SYNCHRONOUS DATA TRANSFER
   ERROR)** — which I had mis-diagnosed as "drive doesn't implement 0xB6."
   `tools/ss_variants.c` proved it: len@8-9 fails, len@9-10 succeeds and drops
   page 2A 40x -> 8x. This is a portable correctness fix (schily uses 9-10 for
   ALL drives), NOT a Plextor quirk.
2. **Descriptor flag bits.** Old code wrote `0x40` (normal) / `0x20` (RDD) —
   both reserved. Fixed to RA=0x01 / Exact=0x02 / RDD=0x04. Normal ceiling uses
   0x00 (hardware-verified). NOTE: this PX-716A **rejects RDD 0x04** with
   5/26/00 (INVALID FIELD IN PARAMETER LIST) — it has no "restore defaults", so
   Phase 1 restore must set an explicit prior-speed descriptor, never RDD. This
   also happens to be exactly what the push/pop "restore-to-prior" SOP wants.

Follow-ups for Phase 1 (not Phase 0):
- **device.c latch bug**: `accudisc_set_speed` only latches streaming off on
  ERR_IO or sense key 0x05; a HARDWARE ERROR (key 0x04) never latches. Moot now
  that 0xB6 works, but fix for robustness on drives that genuinely lack it.
- The lever the "4x/8x verified" numbers came from last session was the 0xBB
  fallback (SET CD SPEED). Both work now; re-confirm which device.c prefers.
- libcdio's `mmc_set_speed` uses **0xBB (SET CD SPEED)**, not 0xB6 — 0xBB stays
  the portable read-speed lever; 0xB6 adds the LBA-ranged ceiling for Phase 3.

### PHASE 1 — speed + rotation — DONE 2026-07-17 (commits 7e4aced, 702b5ac)
All hardware-verified on the PX-716A (ZZ Top / empty tray):
- **GET PERFORMANCE (0xAC)** -> `accudisc_get_performance` + a pure
  `accudisc_classify_rotation` (CLV/CAV/P-CAV/Z-CLV/UNKNOWN). Discriminator is
  intra-segment slope, so a Z-CLV step-up is not mistaken for CAV. PX-716A CD
  curve = 1 rising segment (17x..40x) -> CAV. Unit-tested per shape.
- **`speed [X] [--exact] [--start L --count N]`** — reports page 2A max/current
  + nominal curve + rotation; with X sets an Nx ceiling. Whole-disc via
  accudisc_set_speed (0xB6 else 0xBB fallback); ranged/exact via the new
  accudisc_set_speed_range (0xB6 ONLY, no downgrade). Standalone set persists.
  VERIFIED: cap'd `speed 8` uses 0xB6 (no fallback line); `speed 8 --exact`
  ACCEPTED (drive honours Exact — no Illegal Request); **`speed 24 --start
  100000 --count 20000` ACCEPTED and page 2A shows 24x = the LBA-ranged ceiling
  works -> Phase 3 is viable on this drive.** (profile in the output is deferred
  to Phase 2, which owns the profile->name table.)
- **`features` split**: --c2 (only flag that gates exit: 0/1/2), --stream,
  --rotation (disc-free CAV/CLV, no current speed), --all/bare. VERIFIED.
- **False-negative fixed**: empty-tray `features --c2` now C2_UNVERIFIED (was
  UNSUPPORTED) — medium-not-present (key 0x02 asc 0x3a) is detected. VERIFIED
  round-trip (empty=UNVERIFIED, loaded=SUPPORTED).
- `speed-report` removed. `need_rdwr` untouched (CAP_SYS_RAWIO is the variable).

Still open (small, deferred from Phase 1):
- **--exact discrimination**: it was accepted at 8x, where the drive runs CLV
  anyway. Test `--exact` at 24-32x to see if it forces CLV where the drive
  prefers CAV, or refuses. Not needed for correctness; a characterization nicety.
- **Push/pop for `read --speed X`**: standalone `speed X` persists (done). The
  read engine's per-read restore-to-prior and the `speeds` probe's stale "never
  auto-restored" header were NOT touched this pass — revisit with the engine.
- **Re-run the Q-vs-speed sweep** now that cap'd 0xB6 actually commands speed
  (last session's numbers were the 0xBB fallback).

### DISC-KIND GUARD — BUILT 2026-07-22, partially hardware-verified

Shipped to the locked interface below: `accudisc_probe_disc()` +
`accudisc_disc_probe` + the `disc` subcommand, verdict logic isolated as the
pure `adsc_disc_classify()` and unit-tested over synthetic combinations
(`tests/test_disc.c`, 14/14). Frozen in `cli-machine-interface.md`.

Scope confirmed with Keith 2026-07-22 — recognise exactly: no medium (tray
open/closed), CDDA including CD-R/RW CDDA, blank CD-R/RW, and unknown media
type. Nothing finer. Other media (CD-ROM layouts, Mixed Mode data half, DVD/BD)
need filesystem support and are deliberately out of scope for now; they all
land in `NEITHER` with a slug saying which.

Two additions beyond the original locked shape, both additive:
- `tray=open|closed|unknown` on the `no_medium` branch (sense ASC 0x3A
  qualifier) — distinguishes "insert a disc" from "close the tray", which
  cdda2img §26.4 specifically wanted.
- `disc_status`/`erasable` emit **-1 when not obtainable** rather than 0, since
  0 means "empty" and would otherwise read as blank.

**Hardware status (PX-716A, 2026-07-22):**

- ✅ **AUDIO** — Ritek audio CD-R: `kind=AUDIO profile=0x0009 disc_status=2
  erasable=0 audio_tracks=10 data_tracks=0 reason=audio`, exit 0. Confirms the
  AUDIO-over-BLANK rule on real media: a CD-R carrying CDDA is rippable.
- ✅ **no_medium, tray open** — `kind=NEITHER profile=0x0000 disc_status=-1
  erasable=-1 audio_tracks=0 data_tracks=0 reason=no_medium tray=open`. This is
  the only path that depends on **sense extraction** (ASC 0x3A + qualifier)
  rather than command output, and both halves work: the ASCQ resolved to
  `tray=open`, and `disc_status`/`erasable` correctly reported **-1** rather
  than a false 0 that would have read as blank.
- ✅ **not_cd_profile** — DVD-R: `kind=NEITHER profile=0x0011 disc_status=2
  erasable=0 audio_tracks=0 data_tracks=1 reason=not_cd_profile`.
- ✅ **no_medium, tray closed** — `... reason=no_medium tray=closed`. Both ASC
  0x3A qualifiers now confirmed distinct on real hardware (0x02 open / 0x01
  closed).
- ✅ **BLANK** — blank CD-R: `kind=BLANK profile=0x0009 disc_status=0
  erasable=0 audio_tracks=0 data_tracks=0 reason=blank`, exit 0.

**All five paths hardware-verified. No open verification items.**

> **The DVD result validates the precedence order empirically.** Note
> `data_tracks=1`: the DVD-R **did** answer READ TOC, reporting one data track.
> So the profile gate is not defensive theatre — without it this disc would have
> reached the census and classified `data_cd`, and a medium whose CTRL bits
> happened to read as audio could have classified **AUDIO**, offering a DVD to
> the CD-DA rip path. Checking the profile *before* the census is what stops
> that. Keep the order.

### The locked interface (as built)
The real objective behind deferring Phase 2 (Keith, 2026-07-17). A pre-flight
guard that answers "which of the two possible operations is legal for the disc
in the drive", so nothing attempts the impossible:
  1. **BLANK** — recordable CD-R/RW, no sessions open OR closed -> the BURN path.
  2. **AUDIO** — CD-DA with audio content present -> the RIP path.
  3. **NEITHER** — everything else (data CD-ROM, already-recorded/appendable CD-R,
     DVD/BD, empty tray, unreadable) -> refuse, say why.

**Dependencies: NONE external, NO new opcodes.** Composes three commands the
library already issues. This is why it's cheap and why it does not carry Phase
2's "may need additional deps" risk (that risk is Phase 2's filesystem/VRS work,
which needs a READ(10) path we don't have yet — unrelated to this guard).

Mechanism:
- **GET CONFIGURATION current profile** (`adsc_mmc_get_configuration`, bytes 6-7):
  0x08 CD-ROM / 0x09 CD-R / 0x0A CD-RW = a CD; anything else -> NEITHER (not a CD).
- **READ DISC INFORMATION** (`adsc_mmc_read_disc_info`, already decoded in
  mediaprobe): disc status (byte 2 bits 1-0) 0=empty, 1=incomplete (open
  session), 2=complete (closed); erasable bit (byte 2 bit 4) = CD-RW vs CD-R.
- **READ TOC** (`accudisc_read_toc`): per-track CTRL bit 2 -> audio vs data census.

Verdict logic (evaluate in THIS precedence order — settled with cdda2img
2026-07-18, §17.2):
- **AUDIO first** = disc has >=1 audio track (CTRL bit 2 clear). Pure CD-DA = all
  audio; mixed-mode has audio too and is still rippable-audio for our scope.
  AUDIO wins over BLANK so a burned audio CD-R classifies AUDIO/rippable, not
  BLANK.
- **BLANK** = profile 0x09/0x0A AND disc status 0 (empty). (Status 1 = open
  session, status 2 = closed -> NOT blank; both are "has sessions".)
- **NEITHER** otherwise (all-data, non-CD profile, no medium, no TOC, unreadable).

Deliverable — INTERFACE SETTLED with cdda2img (2026-07-18, private/AccuDisc.md
§17.2); build to this exact shape:
- Public: `accudisc_disc_kind` enum + `accudisc_disc_probe` struct (profile,
  erasable, disc_status, audio_tracks, data_tracks, kind) + `accudisc_probe_disc`.
- CLI subcommand **`disc`** (CONFIRMED with cdda2img 2026-07-18; name == first
  output token, sibling to `media`/`c2lag`). Machine line — canonical field order
  matches cdda2img's plan D2 byte-for-byte (token-primary, parse tokens not
  positions):
  ```
  disc kind=<BLANK|AUDIO|NEITHER> profile=0x<nn> disc_status=<0|1|2> erasable=<0|1> \
       audio_tracks=<n> data_tracks=<n> reason=<slug>
  ```
  `erasable` added per §17.2 (burn path cares: BLANK CD-RW reusable vs CD-R
  one-shot). `reason=` on every line (`audio`/`blank` on the actionable branch;
  NEITHER slugs: `data_cd`, `closed_data`, `appendable`, `no_medium`,
  `not_cd_profile`, `unreadable`).
- Exit codes: **0 = actionable** (BLANK or AUDIO — caller reads `kind=` to branch);
  **3 = classified-but-not-actionable** (NEITHER; reuses "completed-with-caveats");
  **2 = could not classify** (transport failure). Token is authoritative; exit is
  the coarse branch.
- Unit-test the verdict logic as a pure function over synthetic
  (profile, status, track-CTRL) inputs, like the rotation classifier.

### PHASE 2 — media identification (independent of 0/1/3)
Two layers; the profile is physical, the logical type is not:
- **Layer 1 — profile:** GET CONFIGURATION current profile (bytes 6-7).
  `adsc_mmc_get_configuration` **already exists** (features.c uses it for
  CD_READ 0x001E). Add a profile->name table (codes are facts => MIT, same
  precedent as the ATIP DB): 0x08 CD-ROM, 0x09 CD-R, 0x0A CD-RW, 0x10 DVD-ROM,
  0x11 DVD-R seq, 0x12 DVD-RAM, 0x13/0x14 DVD-RW, 0x15/0x16 DVD-R DL, 0x1A
  DVD+RW, 0x1B DVD+R, 0x2A/0x2B +DL, 0x40 BD-ROM, 0x41/0x42 BD-R, 0x43 BD-RE,
  0x50-0x52 HD DVD, 0x0000 no disc/unrecognised, 0xFFFF non-conforming.
- **Layer 2 — CD logical type:** from data we already read — track CTRL bit 2
  + full-TOC session disc-type byte (0x00 CD-DA/CD-ROM, 0x10 CD-I, 0x20 XA) +
  READ DISC INFO (0x51, already implemented). All audio => CD-DA; all data =>
  CD-ROM; mixed => Mixed Mode; multi-session w/ data session 2 => CD-Extra.
  **MUST be gated on a CD profile (0x08/09/0A)** — `tools/mediaprobe.c`
  demonstrates the bug it prevents: it calls a DVD-R "CD-ROM (data)" by running
  the CD classifier on a DVD's synthetic single-track TOC.
- **Filesystem = a lookup table, and that is where we stop.** Volume
  Recognition Sequence at fixed sectors 16+: 5-byte magic `CD001` (ISO9660),
  `BEA01`/`NSR02`/`NSR03`/`TEA01` (UDF). Read 2 sectors, memcmp constants =>
  ISO9660 / UDF / UDF-Bridge. **Needs a READ(10)** (2048B data sectors) —
  confirmed absent from the codebase; our read path is READ CD (0xBE, 2352B).
  **DVD-Video vs DVD-Audio vs DVD-ROM is NOT done** — all are profile 0x0010
  and only a root-directory walk (VIDEO_TS/AUDIO_TS) separates them, which is
  filesystem parsing, not a lookup. Report physical type only. Revisit if/when
  enhanced/mixed-mode work needs it (user decision 2026-07-16).
- **SA-CD: OUT OF SCOPE, not deferred** (user decision 2026-07-22; CLAUDE.md
  amended). The DSD layer is DVD-density, read at 650 nm, and encrypted — a
  CD/DVD drive cannot address it at all, so this is not a matter of effort.
  (Known SACD rips came from specific Blu-ray players, never a PC drive.)
  A hybrid SACD's CD layer is a *genuine* Red Book CD: the drive reports CD-DA
  and is **correct** about the layer it can see, and the HD layer is invisible
  to every generic command, so there is no "unrecognised" signal to trust.
  Only a single-layer SACD trips 0x0000/0xFFFF. Reporting CD-DA for a hybrid
  SACD is not a bug — it is the whole of our SACD story.
- **Restructure `media`:** profile primary, ATIP supplement. Today it keys on
  ATIP, which only exists on CD-R/RW — a pressed CD-DA or any DVD gets nothing.
- Optional, no scope breach: READ DISC STRUCTURE (0xAD) fmt 0x00 -> DVD/BD
  Physical Format Info incl. Book Type, layers, disc size.

### PHASE 3 — scope the streaming contract to a damaged span — DEFERRED 2026-07-17
`accudisc_set_speed_range(dev, speed_x, start, end, flags)` was built and the
ranged contract was tested on hardware. **Result: on the PX-716A the range is
applied whole-disc, not locally** (measured with `tools/rangeprobe.c`: a 4x
contract over a mid-disc span slowed reads everywhere; a 3-descriptor payload
honoured only the first descriptor, globally).

**Ranged sub-disc throttling is a REAL, documented MMC-5 feature** (§6.39.1, full
text + field defs in git-ignored `private/code/MMC/SET_STREAMING_findings.md`). We
were simply UNSUCCESSFUL enabling it on the one drive tested. Cause undetermined
(single whole-disc GET PERFORMANCE extent? still-wrong CDB framing? firmware?).
No open-source tool uses ranged reads (redumper/cdrdao/libcdio use whole-disc
SET CD SPEED 0xBB; schily uses 0xB6 with start_lba=0), so there is no precedent
to copy. **Status: UNKNOWN — investigate further later (more drives,
GET-PERFORMANCE-derived extents), NOT "impossible".** (See memory
`dont-conclude-impossible`.)

**Interim (agreed 2026-07-17):** the CALLER (cdda2img) owns the "repeat reads
across an LBA range on a speed ladder" loop, invoking AccuDisc per iteration with
a WHOLE-DISC speed. AccuDisc already supports this: `read --start L --count N
--speed X` sets whole-disc speed, reads the span, and does NOT auto-restore
between invocations (a cdda2img hard requirement). Nothing new to build for the
interim; `accudisc_set_speed_range` stays (spec-legitimate, whole-disc-effective
on single-extent drives). Revisit the ranged feature in a future session.

### Carried over — Q recovery (Task 2, was mid-flight)
- **Resolve the lone UNKNOWN boundary (t16)** by model reconstruction: frames
  below the dead L-1 are t15 index-1 counting up in abs-MSF, the frame above is
  t16 index-1. If abs-MSF is continuous across the gap and no rel countdown
  appears, the dead frame was prev-body => upgrade UNKNOWN->NONE. Pure inference
  from surviving CRC-good neighbours; **no re-read needed**. Capture for offline
  work without the disc: `tests/data/abba_t16_unknown_boundary.sub`
  (LBA 281333..281762, decode with `tools/pregap.py`).
- **Unified re-read predicate:** a sector fails if C2-dirty OR Q-CRC-bad, both
  from one READ CD; status map gains a second dimension (audio | Q). Harvests
  the transient Q population (proven: ~half the failures clear on re-read).
- Needs a **more damaged disc** to exercise the lost-anchor-at-boundary regime.

### Known bugs to fix along the way
- ~~Default read range dropped track 1's program-area pregap~~ **FIXED
  2026-07-23** (cdda2img §30 → §31). Extents were built from INDEX 01 alone, so
  a first track whose INDEX 01 is past LBA 0 (hidden-track-one audio) left those
  sectors owned by nobody and the default read started past them — shifting
  every LBA against the stream and computing a wrong disc ID. ECMA-130 §20 makes
  a Pause part of the following track; new `accudisc_track.pregap` records it,
  non-zero only for session 1's first track. `toc` emits `pregap <n>`. ABI:
  `accudisc_track` 12→16 bytes (appended field, offsets stable).
- `features` no-disc false negative (C2_UNSUPPORTED/exit 1) -> UNVERIFIED. [P1]
- `--speed 16` silently honoured as 8x, unreported. [P1]
- Logical type must be gated on a CD profile. [P2]
- `accudisc_eject`/`accudisc_load` header comments describe START STOP UNIT
  (LoEj), but the implementation uses block-layer CDROMEJECT/CDROMCLOSETRAY
  (device.c explains why). One-line comment fix; contract vs implementation.

#### Bug audit 2026-07-23 (full report: `private/bugs/2026-07-23-bug-audit.md`)
Correctness sweep of the whole tree, 7 findings, 0 critical. `rw.c` RS/GF math
and the pregap/extent/guard interaction both audited **clean**. Top three
verified against source before recording. **All seven fixed 2026-07-23**, each
with a regression test; suite clean under ASan+UBSan. History below.

- **[P1] F-001 SG_IO `resid` never checked — silent corrupt audio. DONE
  2026-07-23.** `src/transport/sgio.c` treated any GOOD status as full success
  and never read `io.resid`; a short transfer with GOOD status (marginal USB
  bridge, drive under-run, end-of-disc) left the chunk buffer's tail holding
  stale/uninit bytes, which streamed to `--pcm` marked `MAP_OK` with the
  C2/consensus net never running — the "short read that succeeds" hazard in
  CLAUDE.md, worst class. Fixed at the correct seam: the transport now *reports*
  the residual (`cmd->resid`, clamped via `adsc_resid_clamp`) but does not judge
  it — allocation-length commands (MODE SENSE, READ TOC, GET PERFORMANCE)
  legitimately transfer short. `adsc_mmc_read_cd`, which alone has an exact
  expected length, promotes `resid != 0` to `ACCUDISC_ERR_SHORT`
  (`adsc_exec_check_short`), so `read_span` drops to its existing per-sector
  fallback rather than trusting the buffer. Both decision helpers are pure and
  unit-tested (`tests/test_resid.c`) since the ioctl path can't be mocked;
  hardens the accurate-stream/scan probes too. Deferred nicety: partial-tail
  recovery (re-read only the missing sectors) rather than the whole span.
- **[P2] F-002 `accudisc_rw_feed` desyncs the de-interleave if `max < 4`. DONE
  2026-07-23.** `src/cdda/rw.c`: the `break` on `n >= max` skipped the remaining
  packs' ring pushes, permanently misaligning the 8-pack de-interleave. Fixed:
  the loop now always `push_channel_pack`s and gates only emission (`if (n <
  max)`), and a non-zero `max < ACCUDISC_RW_PACKS_PER_SEC` is rejected up front
  with `ACCUDISC_ERR_INVAL` (a sink too small to hold a sector's output can
  never silently drop the tail). `max == 0` prime-only stays valid. Header
  contract tightened; `tests/test_rw.c` asserts 1..3 reject, 4 passes.
- **[P2] F-003 DAO cue buffer 32 bytes short. DONE 2026-07-23.**
  `src/write/burn.c` `cue[99*8*4]`=3168 vs the 3200 worst case (99 tracks ×
  [ISRC+pregap+track] + MCN×2 + lead-in + lead-out = 400 entries). Bounds-checked
  so it aborted cleanly, but a full 99-track ISRC+pregap disc could not burn.
  Fixed: named `ADSC_CUE_MAX_ENTRIES`/`ADSC_CUE_MAX_BYTES` in `write.h`, buffer
  sized to it; `tests/test_cuesheet.c` builds the 400-entry worst case (OK) and
  one byte short (clean `ERR_SHORT`).
- **[P3] F-004 DONE 2026-07-23.** `cmd_read` `--cdg` open failures used bare
  `return 1` instead of `goto out` (`cli/main.c`), leaking the status map/mmap
  and any open `--pcm`/`--c2` files; the `out:` cleanup handles cdgf/rw NULL-safe.
  Both replaced with `goto out`.
- **[P3] F-005 DONE 2026-07-23.** c2lag `chunk` was sized (24) for the pass-1
  read alone; the pass-2 window adds `C2LAG_RUNUP` (16), reaching 40 sectors
  (~105 KB) past `ADSC_MAX_XFER` (65535) and failing on small-`max_sectors`
  HBAs. `chunk` reduced to 8 → window read (8+16)×2646 = 63504 < 65535; comment
  corrected to size for the window, not the chunk.
- **[P3] F-006 DONE 2026-07-23.** `accudisc_q_parse` (`src/cdda/subq.c`) now
  early-returns `ACCUDISC_ERR_CRC` before decoding any BCD/ISRC payload, so a
  CRC-failed frame leaves position/MCN/ISRC zeroed (adr/control still set).
  Header documents fields as valid only on `ACCUDISC_OK`; regression in
  `tests/test_decode.c`.
- **[P4] F-007 DONE 2026-07-23.** `accudisc_lba_to_msf` clamps a post-offset
  negative (LBA < −150, deep lead-in) to 00:00:00 instead of casting a negative
  quotient to `uint8_t`. Matches the write path's `put_msf`; header documents
  the clamp; regression in `tests/test_decode.c`.

### Meta — a caution for next session
Four confident spec-derived claims were overturned by hardware today: "setcap is
an artifact of the RO open", "page 2A is a placebo", "MMC has no rotation
lever", and "SpeedRead defeats the governor". **Measure first; the drive wins.**

---

## (superseded) NEXT SESSION — real read-speed control, then Q-channel preservation

Agreed order (2026-07-15). Speed control gates everything: until we can hold a
commanded speed mid-disc and prove it, all Q-vs-speed testing is moot.

### Task 1 — Set read speed via SET STREAMING (0xB6) — DONE + live-verified 2026-07-15
Implemented and hardware-confirmed: commanded 4x/8x delivered exactly 4.01x/8.01x
at outer windows (CAV would give ~30x) — a binding ceiling CDROM_SELECT_SPEED
never could. Details in drivers/plextor/PROTOCOL.md. Original notes kept below.

**Found during verification — read-engine throughput cap (follow-up).** The
recovery `read` engine sustains only ~5x on a *clean* disc where raw streaming
(`speeds`, bare READ CD) hits ~12–19x at the same radius — ~70 ms/command of
per-chunk overhead (6000 sectors: read=380 sec/s vs speeds-probe ~19x). Not a
speed-control bug; a ripping-throughput issue. Investigate: default
chunk_sectors, per-chunk cache-defeat, status-map write cost, or a synchronous
stall between commands. Matters because whole-disc Q baselines run through this
path.

### (original) Task 1 — Set read speed via SET STREAMING (0xB6), the CAV-correct way
- `CDROM_SELECT_SPEED` / SET CD SPEED (0xBB) — what `accudisc_set_speed` uses
  today (`device.c:169` → `sgio.c:75`) — is advisory; the PX-716A overrides it
  with its disc-init hardware governor (the "stuck at 32×/40×" behaviour, which
  resets on eject; it is NOT a mode we armed).
- **SET STREAMING (MMC `0xB6`, standard MMC — NOT a Plextor opcode)** hands the
  drive a performance *contract*: a 28-byte descriptor {Start LBA, End LBA,
  Read Size, Read Time, …}; speed = Read Size ÷ Read Time scoped to an LBA
  range → commands the CAV curve directly. This is what PlexTools uses (traced:
  builder 0x489740, caller 0x49c95e; see drivers/plextor/PROTOCOL.md §session 4).
- Do it in core `src/mmc/` (pure MMC, not the vendor driver). Route
  `accudisc_set_speed` through 0xB6, fall back to `CDROM_SELECT_SPEED` on reject.
- **Verify on hardware** a commanded speed actually holds mid-disc (current
  failure). Task 1 needs no drive until this verify step.

### Task 2 progress (2026-07-15)
- **Q-CRC counters DONE.** `subq_total/subq_ok` in accudisc_read_stats (engine
  counts extract_q+parse+crc_ok per delivered raw-sub sector); CLI read summary
  prints `subchannel Q : ok/total CRC-ok (%), bad`; machine mirror adds
  subq_total/ok/bad. No new signal code — reused stage-3 subq.c.
- **Clean-disc baseline (ZZ Top, brand new, free-run max ~18.7x w/ sub).**
  Radius sweep, 3000-sector windows: 0%→99.87, 19%→99.83, 44%→99.97,
  68%→100.0, 93%→99.93. **Uniform ~99.9%, NO radius dead zone.** Conclusion:
  max speed does NOT corrupt Q on a clean disc → the earlier dead-zone Q loss
  and cdda2img's §9 missing pregaps are **damage-driven, not speed-driven**.
  Validates the hypothesis that damaged discs still HAVE pregaps we're failing
  to read. ~0.1-0.2% bad frames = noise floor (location not yet recorded).
- **SET STREAMING = a streaming contract, not a plain speed knob (important).**
  Setting *any* ceiling (even 40x, above natural) collapses the recovery
  engine's throughput to ~5x, because the drive tunes for constant-rate
  delivery and the engine's ~37ms/chunk C2+Q processing reads as "host not
  consuming" → drive throttles. So: **first-pass = FREE-RUN (no --speed);
  slow-a-damaged-span = SET STREAMING low (4x/8x were exact).** Do NOT use
  SET STREAMING to set a high/max ceiling for bursty reads.
- **Max speed WITH subchannel is ~18.7x** on the PX-716A (drive can't
  deinterleave subcode faster); 25.9x with C2-but-no-sub, 21.4x audio-only.
  This is the true "max CDDA speed" for a Q-preserving read.

#### Damaged-disc findings (2026-07-16, ABBA Gold, confirmed Q damage)
Same 19-track disc as the Disc-ID work (leadout 347208). Probed the track 1->2
boundary (index-1 at LBA 17395). Offline decoder: scratchpad/qdecode.py (ADR-aware:
only ADR=1 frames are position; ADR=2=MCN, ADR=3=ISRC — critical, or the ~1-per-98
MCN frames masquerade as index-0 boundaries and manufacture phantom pregaps).
- **Q vs C2 orthogonality confirmed on real damage:** a 800-sector window read
  0 C2-flagged sectors but 22 CRC-bad Q frames. Clean audio, dead metadata.
- **Track 2 pregap IS present and reconstructable:** 50 frames, LBA 17345..17394,
  countdown rel 00:00:50 -> index-1 at 17395 (matches TOC). Two frames INSIDE the
  pregap (17346, 17357) are permanently dead, yet zero pregap info is lost — the
  start frame (rel 50), the index-1 boundary, and the countdown anchors pin it.
  This is the "9 of 19" mechanism: if the one boundary frame is CRC-bad and no
  reader interpolates, the pregap looks absent though it is fully there.
- **TWO failure populations (the key result). 3x cache-defeated re-reads of the
  same region:** 6 LBAs bad in ALL passes (static physical defect — re-reads never
  fix), ~6 LBAs bad in only 1-2 passes (transient — recovered by re-read+consensus,
  exactly the PCM strategy). Neither lever alone suffices: consensus for transient,
  deterministic-model interpolation for static.
- **Speed barely affects Q error rate — but ONLY inside the drive's governor
  envelope (corrected 2026-07-16, see below).** 4x->9 bad, 8x->7 bad,
  free-run(~2.4x)->7-9 bad on the same window. Every one of those ran at or
  below the 32x ceiling the drive itself pinned for this disc, so the drive was
  silently protecting the measurement. Within that envelope: defect-driven, not
  RPM-driven. OUTSIDE it (SpeedRead/uncap, which defeats the governor) speed
  matters enormously — that is what the old "40% @ max / 98% @ 24x" measured.
  Both datasets are correct; they sample different regimes.
  **Lever = re-reads + model. Never defeat the governor on a damaged disc.**
  NOTE: these runs used the CDROM_SELECT_SPEED *fallback*, not SET STREAMING —
  the setcap had evaporated on rebuild and set_speed silently fell back. The
  commanded speeds are therefore unverified; only the shape is trustworthy.

#### The PX-716A disc-init governor — SOLVED (2026-07-16)
The long-standing "drive stuck at 32x, but 40x when I eject" mystery (see the
"Investigate how Plextor drives handle speeds" section below) is **not** a mode
we armed. **The drive profiles the disc at init and pins a quality-appropriate
ceiling.** Measured, repeatable across eject/load cycles:
  - ZZ Top (pristine, leadout 204143) -> inits at **40x** (= page 2A max)
  - ABBA  (scratched, leadout 347208) -> inits at **32x**
The disc is the only variable; the drive re-evaluates on every media change.
Consequences:
- **page 2A was never a placebo.** `current_x` is real, readable state: it
  tracks SET STREAMING exactly (4x->706, 8x->1411, 24x->4234 kB/s) and reports
  the governor's init ceiling before we set anything. Earlier "always 40x"
  readings were because no set had ever *succeeded* (silent fallback).
- **Free damage triage:** at init, `current_x < max_x` is the DRIVE's own
  quality verdict on the medium — an absolute, vendor-authored signal costing
  zero reads. Surface it in `speed` (and consider `media`).
- **SpeedRead does NOT defeat the governor** (measured both discs, incl. full
  eject/load re-init with SpeedRead on):

      disc     condition   governor ceiling   SR max   ceiling after SR+re-init
      ZZ Top   pristine    40x (CD-DA spec)   48x      40x  (holds)
      ABBA     scratched   32x (quality)      48x      32x  (holds)

  The governor enforces the CD-DA spec limit (40x, per the PX-716 manual) on
  clean media and throttles BELOW it only when it judges the disc degraded.
  SpeedRead's raised 48x max is ignored by it. The two act on ORTHOGONAL axes:
  governor caps DATA RATE; SpeedRead raises RPM (nominal curve scales x1.2,
  17-40x -> 20-48x, on both discs).
  **Why Q dies:** at the inner radius under SpeedRead the natural rate (20x) is
  BELOW the cap, so the cap never binds, the drive spins at full SpeedRead RPM,
  and Q (no CIRC) fails. At the outer, natural 48x > cap, so RPM is throttled
  and Q survives. The governor guards the wrong axis; it never protects the
  inner tracks. Predicts the old dead zone: curve crosses 32x at LBA ~154,300
  = ~44% into ABBA; measured dead zone was inner 10-60%, outer 70-100% clean.
  **SpeedRead is a pure liability for CD-DA:** it cannot raise the rate ceiling,
  so its only headroom is the inner radius (17->20x) — exactly where its RPM
  kills Q. The recorded whole-disc A/B showed NO throughput gain (both 24.2x)
  while Q fell 99.2% -> 40.6%. Consider escalating the --uncap+--sub guard to
  "never for CD-DA reads at all".
- **Triage caveat:** "current_x < max_x => degraded" only holds with SpeedRead
  OFF. With it on, a pristine disc reads 40 < 48 and would be falsely flagged.
  Compare current_x against the CD-DA nominal (40x), or check SpeedRead first.
- **GET PERFORMANCE nominal is RPM-derived, not medium-measured:** identical
  across no-disc / ABBA (leadout 347208) / ZZ Top (leadout 204143) — end_lba is
  always 359999, never the real leadout — but it DOES track SpeedRead. Constant
  across discs, not across drive state. DVD test still pending (medium class?).
- **Honoured speed ladder is discrete: {4, 8, 24, 32}.** 1-3 -> 4; 6 -> 4;
  9..23 -> 8; 28 -> 24; 40/48 -> 32. Two disjoint regimes, explained by the
  nominal CAV curve starting at 17.00x: {4,8} are CLV (a ceiling below 17x
  binds at every radius => flat), {24,32} are CAV (clamp only the outer region).
  The 9-23 dead zone is the gap between the top CLV rung and the CAV floor.
  **Confirms CAV-at-high / CLV-at-low.** 40x is NOT settable — it is only
  reached by free-running at the outer edge.
- `speed X` must report what the drive HONOURED, not what we asked: today
  `--speed 16` silently yields 8x and nothing says so.

#### Whole-disc pregap census (2026-07-16) — resolves "9 of 19" definitively
Probed all 19 boundaries (read [i1-400, i1+30] each) + a radius damage sweep.
scratchpad/pregap.py does the per-boundary analysis.
- **9 tracks have real pregaps** (t2-t7, t9, t14, t18), 47-50 frames each, plus
  track 1's 33-frame lead-in gap. Every one has its start frame (rel=len) and
  index-1 boundary intact; dead frames inside a pregap (e.g. t2: 17346, 17357)
  are fully covered by surviving countdown anchors -> zero pregap info lost.
- **The other 9 tracks are genuinely GAPLESS** — index-1 -> index-1 with NO
  index-0 frames. Confirmed even for the 2 tracks (t16, t19) whose boundary
  regions were damaged: the surviving CRC-good frames show continuous
  previous-track index-1 counting UP right to the boundary, no index-0.
- **Verdict:** the "9 of 19" the good rip found is CORRECT and COMPLETE, not a
  damage artifact. The starting hypothesis ("all tracks have pregaps, damage
  hides them") is REFUTED for this disc — it has a gapless master for 9 of its
  transitions. Where pregaps exist they survive; where they don't, clean frames
  prove absence.
- **CRC gate is load-bearing:** rejected frames decoded as abs142:38, index19,
  adr9 — garbage that a non-CRC-checking reader would splice in as phantom
  indices. Damage does not just lose Q metadata, it injects false Q metadata;
  the per-frame CRC-16 is the only thing standing between the two.
- **Damage is localized, not radius-graded.** Sweep (2000-sector windows):
  0.6%->100, 11.5%->97.1, 25.9%->100, 40.3%->100, 54.7%->100, 69.1%->99.9,
  83.5%->100, 97.9%->96.0. Mostly pristine with a few damaged spots (inner
  ~LBA 40k, outer ~LBA 340k, and pinpoint hits like t16@281733). Surface
  blemishes at specific spots, NOT an inner dead zone. So this disc needs no
  Q recovery to get pregaps right — the recovery machinery is for a WORSE disc
  that loses a boundary anchor entirely.

#### Index/pregap decoder — DONE (2026-07-16)
`accudisc_index_map_decode` (src/cdda/index_map.c) + `accudisc pregaps` CLI +
test_index_map. Consumes a raw-sub scan, CRC-gates every frame, and classifies
each TOC boundary PRESENT / NONE (gapless) / UNKNOWN / NO_DATA. Key rule: a
pregap ABUTS index-1, so gaplessness is decided by the boundary-abutting frame
(walk down from L-1, skip MCN/ISRC), NOT by any bad frame in a wide window —
that over-flags. Live on ABBA: 9 pregaps (t2-t7,t9,t14,t18, 47-50f), 9 gapless,
**exactly 1 UNKNOWN (t16)** — the sole boundary whose L-1 frame (281732) is
physically dead. Correctly separates t16 from t19 (t19's boundary frame
survived -> gapless), which the manual pass had lumped together.
`pregaps` exits 3 if any boundary is UNKNOWN.

#### Next build — resolve UNKNOWN boundaries (model reconstruction) + re-read
The decoder isolates the ONLY thing needing recovery: UNKNOWN boundaries (t16).
Two levers, in order:
1. **Model reconstruction across the dead abutting frame.** t16: frames below
   the bad L-1 are prev-track (t15) index-1 counting up in abs-MSF; the frame
   above is t16 index-1. If abs-MSF is continuous across the gap (X, [bad], X+2)
   and no rel-countdown appears, the dead frame was prev-body -> upgrade
   UNKNOWN->NONE. A rel countdown on either side -> PRESENT. This is pure
   inference from surviving CRC-good neighbours; no re-read needed.
2. **Targeted re-read + consensus** for UNKNOWN boundaries whose neighbours are
   ALSO damaged (not t16's case, but the general one): re-read the boundary
   window cache-defeated across passes, harvest the transient Q population
   (proven to exist: ~half the failures cleared on re-read). Unify with C2:
   a sector fails if C2-dirty OR Q-CRC-bad, both from one READ CD; status map
   gains a second dimension (audio | Q).

### Task 2 — Preserve the Q subchannel (recover pregaps) — after speed is real
Why it matters: a damaged disc loses metadata (pregap/index/MSF), not just PCM.
As a full-disc archival tool we must recover it; conventional rippers don't care
(they only want track files). There is **no AccurateRip/CTDB analog for Q** — no
external absolute gate — so recovery = blind re-reads + cross-read consensus.

- **Key architecture fact:** C1/C2/CU protect ONLY the main (audio) channel via
  CIRC Reed-Solomon. The subchannel is NOT inside CIRC — Q's only integrity is a
  single 16-bit CRC-16/CCITT per frame: **detection, no correction.** Hence a
  sector can have pristine audio (0 C2) yet a dead Q frame. Orthogonal domains.
- **Q's compensating advantage:** Q is near-deterministic (abs MSF +1/sector;
  track/index piecewise-constant; rel MSF counts down in pregap, up in track).
  So we don't need every frame clean — enough CRC-valid *anchors* let us fit the
  model and interpolate gaps. The one non-interpolable event is the index 0→1
  **transition** (pregap boundary): needs ≥1 clean-ish anchor near each boundary.
  → Target re-reads at index boundaries, not uniformly.
- Steps:
  1. Q-CRC counters in the read `summary` (`subq_total/ok/bad`, + per-region so
     the inner dead-zone shows). Decoder prototyped: `scratchpad/qcrc.py` → port
     to C in `src/cdda/`. (cdda2img §9.2 asked for this too.)
  2. Speed sweep with the NOW-WORKING speed control, one variable at a time; map
     Q-CRC vs speed vs radius. Re-establish the provisional "40% @ max / 98% @
     24×" numbers cleanly — the old ON/OFF run was confounded (was `--speed 48`
     vs `40`; then the drive wedged). Trust the *shape* (inner dead zone, clean
     audio), not the exact %.
  3. Unified re-read predicate: a sector fails if C2-dirty OR Q-CRC-bad; both
     acquired in ONE READ CD (0xBE already returns audio+C2+SUB together —
     `read/engine.c:8`). Status map gains a second per-sector dimension (audio |
     Q). Acquire fast, re-read Q-failures slow (engine already has a speed
     ladder).
- **"9 of 19 tracks have pregaps" suspicion:** unresolved — could be a genuinely
  gapless master OR damage eating every index-0 frame. Q-CRC counters (step 1)
  distinguish them: clean index-1 start + zero CRC-valid index-0 frames before it
  (while neighbours show them) = damage signature, not gapless.

## Eject feature
- accudisc eject and accudisc load (to me this makes more sense than
  "eject -t")

## Investigate how Plextor drives handle speeds.
- "When a Plextor drive initialises a disc, it determines the optimal read
  speed and thereby  imposes a limit that you cannot exceed on hardware level
  which is a good thing."
  https://hydrogenaudio.org/index.php/topic,28739.0.html
- Previously reported drive was "permanently stuck on 32x", but when I eject
  the disc, now I see the speed reported is 40x.

  drdao drive-info --device /dev/sr0
  /dev/sr0: PLEXTOR DVDR   PX-716A	Rev: 1.11
  CD-TEXT writing is supported.
  Using driver: Generic SCSI-3/MMC - Version 2.0 (options 0x0010)

  Maximum reading speed: 7056 kB/s
  Current reading speed: 7056 kB/s

  accudisc speed-report --driver plextor --drivers-dir /home/kgr/Git/accudisc/build/drivers/
  accudisc: plextor: 0xEA arm refused
  accudisc: driver plextor: selftest failed on PLEXTOR DVDR   PX-716A — staying on generic MMC
  accudisc: using generic MMC
  speed max_kbps 7056 current_kbps 7056 max_x 40 current_x 40

  I assume "0xEA arm refused" is because you can only request a speed change when the drive
  knows what kind of disc is loaded.

## ATIP / media identification

- ~~**Wire the ATIP catalog into a lookup + CLI.**~~ *Done (2026-07-12):*
  `accudisc media` reads the disc ATIP (READ TOC/PMA/ATIP fmt 4) and returns
  manufacturer + code + capacity + CD-R/RW. `accudisc_read_atip()` /
  `accudisc_atip_manufacturer()` in `src/drive/media_db.c`; lookup keys on
  `sec` + frame-decade (matching resolves 97:24:01 → the 97:24:00 Taiyo Yuden
  entry; the decade distinguishes makers that share a `sec`). Unit test
  `tests/test_media.c`; live-verified on a blank Taiyo Yuden (`97:24:01`).
  *Possible follow-up:* also surface spiral length (record+0xDC) and the
  ATIP reference-speed/indicative-power fields.
- **Public ATIP cross-reference pass.** *Largely done (2026-07-12):* diffed
  against cdrecord's `diskid.c` (independent source; 107/123 agree) and Nero
  2026 — both carry the same effectively-frozen registry, so "post-2007 gaps"
  turned out moot. Folded cdrecord's 3 high-confidence uniques into the union
  (`gen_media_db.py` → 134 codes). *Remaining (optional):* a broader web ATIP
  database diff to catch any obscure codes none of these three list.

## Recording

- **CD-Text on write (next recording feature).** The write path does NOT burn
  CD-Text yet, so a round-tripped disc loses album/track titles/performer (the
  "supplemental metadata"). Have: CD-Text *read* (meta/cdtext.c) and the 2448
  block-type in the write-parameters page (wparams.c `cdtext`). Need: a CD-Text
  encoder (pack the CD_TEXT blocks the .toc parser currently ignores) + inject
  it into the SEND CUE SHEET lead-in (dataForm 0x41 lead-in entry + the R-W
  sub-channel packs). Reference: cdrdao `CdTextEncoder` / `writeCdTextLeadIn`.
  Note: CD-Text does NOT affect the Disc ID (pure TOC) — this is content
  fidelity, separate from the pregap item below.
- **Disc-ID round-trip mismatch = pregap/TOC, upstream (cdda2img).** Root cause
  pinned via 3-way compare: original disc track 1 @ LBA 33 with a 33-frame
  pregap, lead-out 347208; cdda2img's RBI + our burn both track 1 @ 0, lead-out
  347175 (Δ = 33). Our burned Disc IDs are byte-identical to the RBI mount, so
  AccuDisc reproduced the .toc *exactly* — the loss is in cdda2img's extract
  (its .toc drops track 1's pregap and must match the original TOC's index-1
  offsets AND lead-out, not just track 1). AccuDisc verified ready: a declared
  track-1 `START` yields the right index1_lba. Consider an AccuDisc read-back
  verify (`--verify-toc`) that diffs the burned TOC vs the source .toc and warns
  on any offset delta.

## Probes / diagnostics

- **Timed-read cache detection.** Borrowed from libcdio-paranoia
  (`cdrom_cache_handler`): a re-read that returns implausibly fast was
  served from the drive's cache, not the platter. Paranoia flags a backseek
  read completing in under ~6 ms. Use it as a probe that *verifies our
  cache-defeat actually works* on a given drive — time a normal read, then
  time a re-read at our 5000-sector flush distance; if the re-read is not
  meaningfully slower, the flush distance is too small for this drive's
  cache and every "independent" reread (c2_retries, verify, consensus,
  c2lag) is silently reading cache. Report-only, per-drive, like the other
  probes. Would also let the flush distance auto-tune instead of being a
  fixed constant.

## TOC reading

### Full-TOC → TOC automatic fallback — PHASE A DONE + HARDWARE-VERIFIED 2026-07-21

`accudisc fulltoc` (READ TOC format 0x02) reads the *raw lead-in Q-channel*; on
a marginal CD-R lead-in the drive can fail that command (transport error, exit
2) while the cooked TOC (format 0x00) reads perfectly — observed on the PX-716A
with an MPO CD-R, 2026-07-21, and *not* on a Ritek CD-R in the same session, so
it is that disc, not CD-R as a class. The CDB is correct (session 1, MSF bit,
per redumper); a drive+media quirk, not a defect.

**Shipped (Phase A):** `accudisc_read_toc_src()` prefers 0x02 and degrades to
0x00, reporting `source=` and `degrade=` (`none` / `leadin_unreadable` /
`leadin_absent` / `leadin_malformed`). `toc` emits a token line and exits **0**
on a degrade — the command promises geometry and still delivers it; failing
would regress the very discs the fallback serves. Frozen in
`cli-machine-interface.md`. Pure conversion `adsc_toc_from_fulltoc()` unit-tested
(`tests/test_tocsrc.c`, 13/13).

> **Correction worth keeping.** Format 0x02 does **not** carry INDEX 00/pregap
> data — the lead-in TOC holds track *starts* (INDEX 01), A0/A1/A2 and session
> structure only. Pregaps exist solely in the program-area Q subchannel. So a
> successful 0x02 supplies no more pregap data than a degraded 0x00, and the
> degrade costs only session structure. Both an earlier note of ours and
> cdda2img's §26.2 assumed otherwise.

**Hardware verification — MPO CD-R (Paul Weller, *Stanley Road*), PX-716A fw
1.11, 2026-07-21.** All three wanted results confirmed:

- `fulltoc` reproduces the original failure exactly: `transport I/O failure`,
  exit 2.
- `toc` emits `source=toc degrade=leadin_unreadable pregaps=none`, exit **0**,
  with all 12 tracks and lead-out 236435.
- Geometry is **byte-identical** to the pre-change binary (847b10a built in a
  scratch worktree and diffed): no regression.
- Measured cost of the degrade: ~5 ms → ~172 ms (3 runs each, tight spread).
  The ~166 ms is the drive giving up on the lead-in. Recorded in
  `cli-machine-interface.md`.

**The correction is now empirically proven, not just read off the spec.** On
this disc the lead-in is *completely unreadable*, yet `pregaps` recovers all 11
pregaps (tracks 2–12, every one 149f) with near-perfect Q CRC — mostly
`[150 ok, 0 bad]`, worst `[147 ok, 3 bad]`. Pregap data therefore cannot be
coming from the lead-in; it is program-area Q, exactly as the descriptor layout
says. A natural experiment we could not have staged deliberately. It also
demonstrates the degradation pattern the `degrade=` signal exists to catch: the
lead-in has died while the program area is still healthy.

**Success path verified — Ritek CD-R (10 tracks), same drive, 2026-07-22.**
`fulltoc` reads clean; `toc` reports
`source=fulltoc degrade=none pregaps=none sessions=1..1 disc_type=0x00`, exit 0.

**The conversion is cross-validated against an independent decode.** This disc
provided the test the MPO one could not: there, both binaries used format 0x00,
so identical output proved little. Here the new binary derives geometry from the
lead-in through `adsc_toc_from_fulltoc()` while 847b10a uses the drive's own
cooked format-0x00 answer — two independent decodes of the same disc. Output is
**byte-identical** across all 10 tracks and lead-out (253937), so the MSF→LBA
arithmetic (including the −150 offset), the track slotting and the extent
computation all agree with the drive's firmware.

**Preferring 0x02 is free when it succeeds:** ~5 ms before and after on this
disc (3 runs each). The ~166 ms penalty is paid only on a degrade. Both figures
are in `cli-machine-interface.md`.

Both paths are now hardware-proven. No open verification items.
- **Phase B (`toc --pregaps`) — DROPPED 2026-07-22, at the requester's request.**
  cdda2img §26.2 originally asked for Q-derived pregaps folded into `toc`; their
  §27 audit withdrew it. Their pregaps come from the Q stream and never from a
  TOC (`subq_toc.build_rip_info` → `_derive_layout` → `derive_track_layout`,
  Q-only; `track_starts` supplies boundaries only), and their rip path always
  captures `--sub raw`, so `toc --pregaps` would be a second program-area pass
  over data they already hold. No other consumer wants it. `pregaps` stays the
  standalone diagnostic — which is what proved the point on Stanley Road. The
  `pregaps=` token still ships (always `none`) so a future value is additive.
  **Do not build this without a new requester.**
- **Phase C (retry counter) — WITHDRAWN 2026-07-22 by the requester.** cdda2img
  §26.5 assumed a retry loop existed in the 0x02 path. It does not — there is no
  retry logic anywhere in `src/mmc/` or `src/transport/`. On being told, they
  withdrew the ask and argued the current behaviour is *better*: a single-attempt
  failure means one specific thing, whereas "failed after N tries" blurs it, and
  retries would cost time on exactly the discs already failing. **Do not add
  retries to make a counter possible.** If retry *behaviour* is ever wanted, that
  is a separate, deliberate decision.
- **[P1] Emit a session COUNT on the degrade path** (cdda2img §28). Their
  archival policy is session-1-only, and `sessions=1..1` is necessarily absent
  when 0x02 fails. Their mitigation is to refuse a degrade carrying any data
  track — sound for Enhanced CD, whose session 2 is always data — but it leaves
  one hole: a **multi-session all-audio** disc passes their "all audio ⇒ safe"
  test while format 0x00 hands back the *last* session's lead-out, not session
  1's. Wrong lead-out, wrong disc ID, silently.

  Fix is cheap and needs no new opcode: **READ DISC INFORMATION carries the
  session count independently of the lead-in TOC** — already decoded at
  `src/write/discinfo.c:32` (`buf[4]`), and the `disc` guard already issues the
  command. Emit `sessions=<n>` (a count, distinct from the fulltoc line's
  `sessions=<a>..<b>` range) on the degrade path so a caller can refuse
  multi-session degrades outright rather than inferring session structure from
  a track census that cannot see it. Confirm on hardware that the count is
  still valid when the lead-in is unreadable — plausible, since the drive
  answers this from its own disc model, but the Stanley Road disc exists to
  test it.

  **DONE 2026-07-22.** `accudisc_toc_info.session_count` and
  `accudisc_toc.sessions_total`; `session_count=<n>` on the `toc` line. Three
  behaviours now, where there was one: a count of **1** is fully
  reconstructible (one session owns every track and format 0's lead-out IS its
  lead-out), so the model is synthesised and a dead-lead-in disc stays wholly
  rippable; a count **> 1** refuses with `session_unmapped` — the seams are
  known to exist and their positions are not; a count of **0** falls back to
  the conservative all-audio walk.

  **VERIFIED on hardware 2026-07-22** (PX-716A, MPO CD-R — the disc whose
  lead-in does not read). `accudisc toc` returned
  `source=toc degrade=leadin_unreadable ... session_count=1`: the count came
  through while the lead-in was failing, which is exactly the premise — the
  drive answers from its own disc model, not the groove.

  That the count is *correct* is confirmed three ways, independently:
    - READ DISC INFORMATION byte 4 (number of sessions) = 1 — the field we ship.
    - READ DISC INFORMATION byte 5 (first track in the LAST session) = 1. A
      different field in the same response: if the last session starts at track
      1, there is only one. Byte 6 = 12, matching the 12 tracks.
    - libcdio `cd-info` reports `Last CD Session LSN: 0` — a different tool
      issuing a different command, agreeing the last session starts at LBA 0.
      Its lead-out (236435) matches ours exactly.

  The count==1 reconstruction then fired end to end: a session table was
  synthesised on a `source=toc` line (`session 1 tracks 1-12 audio 12 data 0
  leadout 236435`), and `read` resolved `session 1, lba 0 count 236435`. A disc
  with a dead lead-in is now fully rippable through the validated path rather
  than a flat fallback.

- **[P1] Mixed Mode: session selection is too coarse** — a gap opened by
  35ba94c and **CLOSED 2026-07-22**. On a Mixed Mode CD one session holds a data
  track (first, where a filesystem is expected) followed by audio tracks. Every
  step was individually right — the default audio session *is* session 1, its
  whole-session range *does* start at LBA 0, and the guard *does* correctly
  refuse a range containing a data track — and the outcome was that
  `accudisc read` with no arguments failed on the format.

  Fix: `accudisc_toc_session_audio_range()` narrows the default to the session's
  **audio tracks**, plus `accudisc_toc_track_range()` and `--track N` /
  `--tracks A-B` for explicit selection. The guard was **not** relaxed.

  **VERIFIED on hardware 2026-07-22** (PX-716A, Taiyo Yuden CD-R: data track 1
  of 138230 sectors, audio tracks 2-11, one session, lead-out 342197).

    - Before: `refusing lba 0 count 342197: data_track at lba 0 (track 1)`.
    - After: `session 1, lba 138230 count 203967`, and 138230+203967 = 342197,
      exactly the lead-out.
    - `--tracks 2-11` resolves to the *identical* range — independent
      confirmation the default picks the right thing rather than one code path
      agreeing with itself.
    - Full rip of that range: **479,730,384 bytes = 203967 × 2352 exactly**,
      0 C2-flagged sectors, exit 0.
    - Still refused, correctly: `--track 1` and `--tracks 1-11` (`data_track`),
      and `--tracks 13-14` on the Enhanced CD (different sessions — a span
      across the seam would swallow 11,400 dead sectors).

  One case is **refused rather than solved**: audio tracks either side of a data
  track within one session cannot be expressed as a single range, so
  `ACCUDISC_ERR_UNSUPPORTED` is returned and the caller must name tracks. Legal
  on the wire, unit-tested, **never observed on real media**.

- **[P3]** Bindings (`bindings/python`, `bindings/rust`) do not yet expose
  `accudisc_read_toc_src` or `accudisc_probe_disc`; they are generated against
  the public header, so both are additive whenever next regenerated.

## Formats and specs

- ~~**[P2] CD+G capture and pack extraction**~~ — **DONE 2026-07-22.**
  `src/cdda/rw.c`, public API `accudisc_rw_*`, CLI `read --cdg FILE`.

  Built against the normative spec (Philips/Sony *Subcode/Control and Display
  System — Channels R-W*, Nov 1991, `private/research/`), which had already
  corrected two errors in the earlier version of this entry: the pack/packet
  nesting was inverted, and R-W is Reed-Solomon protected where I had claimed
  it was not. Structure, per §5.1: 6 bits = SYMBOL, 24 symbols = PACK, 4 packs
  = PACKET, so one packet per sector, 75 packets/s and **300 packs/s**. The
  `.cdg` format is the 24-byte **pack** stream.

  Three stages: extract R-W as the low 6 bits of each subcode byte; undo the
  8-pack convolutional interleave plus its position permutation (three
  transpositions — (1,18), (2,5), (3,23) — read directly off spec Figures
  5.3/5.4, which are images and needed rendering rather than text extraction);
  then Reed-Solomon decode over GF(2^6), `P(X)=X^6+X+1`. Both codes are
  conventional RS with consecutive roots a^0.., so **one routine serves both**
  parameterised by length and parity count: (24,20) across the pack correcting
  2 symbols, (4,2) over symbols 0-3 correcting 1.

  **Verification, with no CD+G disc available.** Correctness rests on a round
  trip against an INDEPENDENT encoder in `tests/test_rw.c`, deliberately built
  by a different method — the encoder solves H*V=0 by Gaussian elimination, the
  decoder works from syndromes — so a shared mistake is unlikely to cancel out.
  Covers: clean round trip, exact-count single-error repair, 2-error repair,
  an 8-symbol channel burst (which the interleave scatters across 8 logical
  packs), beyond-capacity damage reported rather than silently miscorrected,
  and an all-zero stream reading as MODE ZERO.

  **Hardware behaviour, measured on the Taiyo Yuden Mixed Mode CD-R** (a disc
  with NO CD+G, which is still a real test): 1000 sectors gave 3993 packs
  (= 4n-7, the 8-pack de-interleave span costing 7 at the tail), 95,832 bytes
  = 3993 x 24 exactly, all MODE ZERO, output entirely zero. In a single read
  capturing both `--subf` and `--cdg`, the raw subchannel held **48 stray R-W
  symbols and the P code repaired exactly 48**, across 47 packs — one of which
  had two strays, so the t=2 path fired on real data.

  **A finding worth keeping:** the stray count VARIES between reads of the same
  sectors (39, 66, 48 on three passes). R-W gets no C1/C2 correction from the
  drive, so these are transient channel errors, not pressed-in bits. That is
  the empirical argument for doing the RS decode at all — without it every rip
  of the same disc would differ.

  Still open: **end-to-end verification needs an actual CD+G disc.** What is
  unproven is only the assumption that the drive returns the 96 subcode bytes
  in the order assumed; that rests on the Q path in subq.c, which reads bit 6
  of the same bytes and is hardware-verified. Worth acquiring a karaoke disc.

  Scope line with cdda2img holds: we deinterleave, RS-correct and emit packs;
  they render. RS correction recovers recorded bits rather than interpreting
  them, so it stays our side of the "AccuDisc only moves bits" rule.

- **HDCD: nothing for us to build.** It is a watermark in the LSBs of ordinary
  16-bit PCM; a bit-exact rip preserves it with zero special handling, and both
  detection (scanning LSBs for the control-code sync pattern) and decoding
  (peak extend, gain, dither) are analysis of delivered audio — explicitly out
  of scope per CLAUDE.md, and communicated to cdda2img as wholly theirs.
  Recorded here so the question is not reopened.

- ~~**[P3] Obtain the Orange Book**~~ — **DONE 2026-07-22.** Orange Book Part II
  (CD-R) Vols 1–2 and Part III (CD-RW) Vol 1 are in `private/research/`, along
  with the Multisession CD spec, Enhanced Music CD spec, R–W subcode spec, CD
  Text Mode, CD-ROM XA, MMC-3, SCSI-2, the SACD specs and *The CD Family*.
  **Public** ECMA-130 / ECMA-394 / ECMA-395 are cited but deliberately **not
  committed** (`docs/research/.gitignore` excludes `*.pdf`) — ECMA publishes
  them for free download, so a citation beats megabytes of permanent history.

  All four session-overhead constants are now confirmed:

  | constant | sectors | source |
  |---|---|---|
  | first session lead-out | 6750 | ECMA-394 §5.7.1 — **public, citable** |
  | later session lead-out | 2250 | ECMA-394 §5.7.1 — **public, citable** |
  | second+ session lead-in | 4500 | Multisession CD spec — licensed |
  | pregap | 150 | Multisession CD spec — licensed |

  The measurement (6750+4500+150 = 11400) was right. ECMA-394 being *public* is
  the useful part: those two figures may now be quoted and cited in `docs/`.

  **The 99-session ceiling was searched for and not found** in the Multisession
  spec — the document that would define it. Not proof of absence, but it looks
  like folklore borrowed from the real 99-*track* limit. `docs/research/
  disc-formats.md` §4 now records it as actively doubted. Any firmware ceiling
  is measurement work on our hardware, not a spec question.

- ~~**[P3] Read *CD Cracking Uncovered* (Kaspersky)**~~ and ~~**the defensive
  pass over `adsc_toc_from_fulltoc()`**~~ — **BOTH DONE 2026-07-22.** Taxonomy
  and findings in `docs/research/disc-formats.md` §11.

  The audit found **one real hole, and it was not a crash.** Memory safety was
  already sound (every index bounds-checked; the suite now runs clean under
  ASan + UBSan with `-fno-sanitize-recover=all` against a hostile-input test
  file, `tests/test_toc_hostile.c`). The defect was the third failure mode —
  silently normalising a contradictory TOC into a plausible-looking one.

  `toc_fill_extents()` walked tracks in **track-number** order and treated that
  as **address** order. Kaspersky ch. 6's "Incorrect Starting Address for the
  Track" exists to break that coincidence. When it did, the out-of-order data
  track's extent collapsed to zero — so it owned no sector and became
  **invisible** to the map — while its neighbour's extent stretched over the
  region it vacated. `accudisc_check_audio_range()` then returned **ok** for a
  span covering a data track. Measured before and after on a synthetic TOC.

  Fixed two ways, deliberately keeping both:
    - extents are computed in **address** order (`next_by_address()`) — not a
      hardening measure but the correct definition, and identical on honest
      media;
    - `accudisc_toc.anomalies` records structural defects, and the three that
      mean the map cannot be believed (`lba_order`, `overlap`,
      `leadout_before`) make the guard refuse outright with `toc_untrusted`.

  The first defence is only as good as our imagination about which orderings
  can be violated; the second does not depend on having predicted the trick.

  Six further flags (`past_leadout`, `empty_track`, `negative_lba`,
  `bad_track_num`, `range_mismatch`, `bad_session`) are **reported only** —
  their discs are still described correctly, and over-refusing would break media
  that reads fine. Surfaced as `anomalies=` on the `toc` line, absent entirely
  on a well-formed disc.

  **Deliberately not defended against:** "Data Track Disguised as Audio". CTRL
  is the TOC's only statement about track type; if it lies, no self-consistency
  check catches it. It surfaces at read time as sense key 5 / ASC 0x64, which
  the read engine already stops on. Recorded so nobody later "fixes" this with a
  heuristic that guesses track type from content — that is analysis (out of
  scope per CLAUDE.md) and it would be guessing besides.

  Two of his observations worth carrying forward: a drive may **remap
  non-standard point numbers into the legal range** (he cites an NEC unit
  reporting `0xAB` as `0x6F`), so an in-range track number is not proof it was
  recorded that way; and non-standard points are **invisible to READ TOC
  entirely**, including format 2 — reaching them needs subchannel reads of a
  later session's lead-in, which bounds what our parser can ever see.

- **[P2] Acquire copy-protected test discs.** Also on cdda2img's TODO; recorded
  here because the need is ours first — the §11 hardening is verified only
  against **synthetic** TOCs built from Kaspersky's descriptions. That proves we
  survive the taxonomy as documented; it does not prove we survive what the
  schemes actually pressed. Real media is the only thing that closes that gap.

  Wanted: at least one disc per scheme, second-hand, cheap. **Regional pressing
  matters** — the same album is frequently protected in the EU and unprotected
  in the US, so a catalogue or matrix number is part of the requirement, not a
  nicety. A research pass to build the acquisition shortlist was launched
  2026-07-22; output lands in `private/research/incoming/`.

  **Survey done 2026-07-22** — `private/research/incoming/` holds the scheme
  taxonomy, a ~35-row title catalogue with catalogue numbers, and a ranked
  shortlist. Three flagged shapes were then modelled synthetically and are
  permanent tests in `tests/test_toc_hostile.c`; **none needed a disc to
  answer**: CDS-200 cross-session duplicate addresses (already covered — the
  overlap check does not filter by session), key2audio's three sessions
  (already handled — the audio session is chosen by content, not position),
  and MediaCloQ track-type inversion (inside the model; see below).

  Top of the shortlist: Natalie Imbruglia *White Lilies Island* (BMG 74321
  891212) and Right Said Fred *Fredhead* (BMG 74321 87262 2), both CDS-200; and
  a genuine **A/B pair** — Handel *Deidamia*, Virgin Classics 5455502 (Copy
  Control) versus 5456692 (Red Book reissue of the identical recording), plus
  Virgin Veritas 5457112 *Serse* as an explicitly-unprotected control from the
  same imprint and era. That triplet is the cleanest possible check that
  `anomalies=` keys on something real rather than on label/era artifacts.
  Charley Pride *A Tribute to Jim Reeves* is worth promoting — it would confirm
  the track-type-inversion report on real media. **Barcode 7 816190222-2 4**
  (from BinaryObjectScanner), which beats a catalogue number for secondhand
  buying.

  **BUY THIS FIRST — "Karaoke Spotlight Series — Pop Hits Vol. 132", Sound
  Choice SC8732.** Both halves now confirmed, so the earlier inference is
  retired: DRML lists it as a **confirmed MediaCloQ V1 sample**, with the
  protection printed on the disc label ("This disc is copy protected by -
  MediaClōQ - By SunnComm, Inc. - V1"), and an eBay listing's item specifics
  give `File Format: CD+G` / `Media Type: Standard CD+G`.

  One disc, two subsystems: the **R-W/CD+G decoder end-to-end** — the
  verification `tests/test_rw.c` structurally cannot provide, being a round trip
  against our own encoder — **and** the track-type-inversion path. That puts it
  ahead of the CDS-200 pair.

  **The family is wide**: Sound Choice shipped ~35 CD+G discs with MediaCloQ
  across their **8700 series** (discontinued 2003-04-14). So if one listing is
  awkward — the one seen says *"May not ship to United Kingdom"* — any
  8700-series Spotlight disc is a candidate, and the label text is checkable
  from a photo before buying.

  **Safe to read.** MediaCloQ installs no driver and no kernel component; that
  was XCP and MediaMax. The privacy allegations in *DeLise v. Fahrenheit
  Entertainment* concern web-side tracking after a user followed the disc's
  download offer to SunnComm's site — not an on-disc payload. Nothing an SG_IO
  tool touches, and those servers are long dead.

  Also gained from BinaryObjectScanner, closing survey gaps: **DocLoc** is by
  **DocData** (not Optimal Media as the survey had it), works via a
  "non-standard second session", and has three titles — *Yorin FM Hitzone 21*
  (Discogs 790336), *Helium Vola* (Discogs 188439), *Wolfsheim — Casting
  Shadows*. **LabelGate CD2**: Redump entry 95010 / product ID SVWC-7185. And
  key2audio's three-session structure is independently corroborated, with an
  unspecific further claim of a "partially invalid TOC" — **uncertain, do not
  act on it**, but a real disc would settle whether key2audio also trips an
  anomaly slug.

  Next research source, unexamined: **DRML** (the DRM Library,
  github.com/TheRogueArchivist/DRML), cited as BinaryObjectScanner's authority
  for MediaCloQ. Likely the best lead for the schemes still thin here —
  Alpha-Audio, DocLoc's mechanism, and SafeAudio's unidentified US titles.

  Not worth buying for TOC work, stated so effort is not wasted: XCP and
  MediaMax discs are Windows kernel-driver attacks on ordinary, well-formed
  Enhanced-CD-shaped discs, useful only as negative controls.

- **[P2] SafeAudio disc — for the RECOVERY engine, not the parser.** Tracked
  separately because it tests a different subsystem. SafeAudio inserts short
  bursts of unrecoverable noise into the audio, sized so a player's
  interpolation hides them; it never touches the TOC.

  The survey found no title at all. A contemporaneous source
  (audiorevolution.com 2001-07-24, relaying *de Volkskrant* 2001-07-20; fetched
  via `cdda2img/tools/wayback/fetch.py` — WebFetch is blocked from
  web.archive.org) names **Volumia! *Puur*** (Netherlands, BMG, 2001). It also
  explains why identification is so hard: BMG confirmed the system was shipped
  **with no notice on the disc**. A second lead, *Groeten uit Salou 4*, is
  explicitly hedged in that source as possibly key2audio instead — do not buy on
  it alone.

  Why it is worth having, which the survey undersold:
    - the errors are **mastered in**, so they are *static* by construction —
      identical on every re-read. Real damage mixes transient and static
      populations, and separating them is exactly the hard part. This is a
      **known-pure static population**, the control the C2/re-read work has
      never had.
    - it is the case where **a player interpolates and we must not**. A
      SafeAudio rip should surface hard errors *by design*; worth establishing
      before such a rip is read as a damaged disc.
    - diagnostically, systematic unrecoverable C2 at **consistent locations
      across re-reads** on visually clean media is a SafeAudio signature rather
      than a scratch — an inference the recovery engine could surface.

  What each disc would actually buy us, in order of value:
    1. a scheme that does something our taxonomy does **not** cover — the only
       way to find out is to meet one;
    2. confirmation that `anomalies=` fires on real protected media and stays
       silent on the unprotected pressing of the same title (the ideal test
       pair, and the reason pressing identity matters);
    3. evidence about whether the `UNTRUSTED_GEOMETRY` refusal is correctly
       calibrated — if a real protected disc rips fine everywhere else and we
       alone refuse it, we are over-refusing and should demote a flag.

- **[P3] Physical-characteristic protection is untouched** (Kaspersky ch. 9):
  deliberate defects, read-timing and inter-sector angle measurement, weak
  sectors. These bind to the medium rather than malforming the TOC, so nothing
  in the §11 pass addresses them. Listed for completeness, not planned — no
  demand, and the recovery engine's existing C2/reread machinery is the part
  that would meet them.

- **[P4] Vanity project: a backwards-compatible hi-res audio disc.** Replicate
  SACD's *audio quality only* in a format readable by drives >= DVD, using a
  technique in the spirit of HDCD / DTS-CD — payload smuggled inside a
  container existing hardware already plays. Options deliberately open.
  Sketch of the design space, to be argued properly later:
    - *Where the extra bits live.* HDCD hides ~1 bit in PCM LSBs; DTS-CD
      replaces the PCM entirely with a bitstream (so legacy players emit
      noise — the thing Enhanced CD was invented to avoid). A middle path
      keeps a valid 16/44.1 downmix audible and carries the residual
      elsewhere: LSB subcoding, the R-W subchannel (~72 B/sector), or a
      second session's data track.
    - *Why >= DVD matters.* A CD's 74-80 min at 44.1/16 has no headroom for
      a meaningful residual; DVD-density media gives ~4.7 GB, enough for
      24/96 outright, and the question becomes what legacy compatibility is
      even worth preserving at that point.
    - *The honest tension.* "Backwards compatible" and "hi-res" pull opposite
      ways: every bit spent staying compatible is a bit not spent on quality.
      Worth deciding early which one is the constraint and which the goal.
  Pure vanity, no schedule, and explicitly not on the critical path.

## Deferred (explicitly, by user decision)

- Python / Rust bindings (generated against `include/accudisc/*.h` only).
- Man page (must mirror `docs/ATTRIBUTION.md`).
- Write / burn (DAO) path — paused; do not start without direction.
