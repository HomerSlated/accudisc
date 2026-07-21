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

### DISC-KIND GUARD — burn-vs-rip sanity check (PLANNED; deferred execution)
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
- **SA-CD: deferred** (user decision). A hybrid SACD's CD layer is a *genuine*
  Red Book CD — the drive reports CD-DA and is **correct** about the layer it
  can see; the HD layer is invisible to every generic command, so there is no
  "unrecognised" signal to trust. Only a single-layer SACD trips 0x0000/0xFFFF.
  Reporting CD-DA for a hybrid SACD is not a bug: we read the CD layer.
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
- `features` no-disc false negative (C2_UNSUPPORTED/exit 1) -> UNVERIFIED. [P1]
- `--speed 16` silently honoured as 8x, unreported. [P1]
- Logical type must be gated on a CD profile. [P2]
- `accudisc_eject`/`accudisc_load` header comments describe START STOP UNIT
  (LoEj), but the implementation uses block-layer CDROMEJECT/CDROMCLOSETRAY
  (device.c explains why). One-line comment fix; contract vs implementation.

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

### Full-TOC → TOC automatic fallback — PHASE A DONE 2026-07-21, HARDWARE UNVERIFIED

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

- **[P1] Hardware-verify on the MPO disc** (drive was in use by cdda2img when
  this landed). Wanted: `source=toc degrade=leadin_unreadable` on the MPO CD-R,
  `source=fulltoc degrade=none` on the Ritek, and geometry identical to what
  plain format 0x00 returned before the change.
- **[P2] Phase B — the `pregaps=` middle rung.** cdda2img §26.2 asks for
  Q-derived pregaps folded into `toc`. Because pregaps are orthogonal to the
  lead-in (see correction above), this is not a fallback rung but an opt-in
  enrichment: `toc --pregaps` runs the CRC-gated boundary scan
  (`accudisc_index_map_decode`, already shipped as `pregaps`) and emits
  `pregaps=q`. Must stay opt-in — it turns a lead-in-only command into a
  program-area read, a large cost change. Needs the drive.
- **[P3] Phase C — cdda2img §26.5, "signal a lead-in that *nearly* failed".**
  Premise checked and it does **not** hold: there is **no retry logic anywhere**
  in `src/mmc/` or `src/transport/`, so there is no retry count to surface. A
  degradation-warning signal would require *adding* retries to the 0x02 path
  first — a behaviour change that costs real time on exactly the discs that are
  already failing. Decide deliberately; do not bolt a counter onto a loop that
  does not exist.
- **[P3]** Bindings (`bindings/python`, `bindings/rust`) do not yet expose
  `accudisc_read_toc_src`; they are generated against the public header, so this
  is additive whenever they are next regenerated.

## Deferred (explicitly, by user decision)

- Python / Rust bindings (generated against `include/accudisc/*.h` only).
- Man page (must mirror `docs/ATTRIBUTION.md`).
- Write / burn (DAO) path — paused; do not start without direction.
