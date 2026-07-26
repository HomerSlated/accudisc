# AccuDisc — deferred work

Ideas parked for a later session. Not scheduled; not commitments. Recovery
methods are considered complete (see `docs/reference/RECOVERY.md`); this is
everything else worth remembering.

Completed work is kept as one- or two-line summaries with any durable lesson
attached; the blow-by-blow reasoning that produced it is not retained.

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
Two bugs fixed, hardware-verified on the PX-716A.

**Durable lessons:**
1. **SET STREAMING's Parameter List Length lives at CDB bytes 9-10, not 8-9** —
   it is the one MMC command that shifts its length field off the normal Group-5
   slot (schily: `/* Sz not G5 alike */`, cdrecord/scsi_mmc.c:991). At 8-9 the
   drive reads 0x1C00 = 7168 expected bytes, gets 28, and answers **4/1b
   SYNCHRONOUS DATA TRANSFER ERROR** — which reads exactly like "this drive does
   not implement 0xB6". Portable correctness fix, not a Plextor quirk.
2. **Descriptor flag bits are RA=0x01 / Exact=0x02 / RDD=0x04**; a normal ceiling
   uses 0x00. This PX-716A **rejects RDD** (5/26/00) — it has no "restore
   defaults", so any restore must set an explicit prior-speed descriptor. That is
   also what the push/pop restore-to-prior SOP wants.
3. 0xBB (SET CD SPEED) stays the portable read-speed lever (libcdio uses it);
   0xB6 adds the LBA-ranged ceiling.

Follow-up still open: **device.c latch bug** — `accudisc_set_speed` latches
streaming off only on ERR_IO or sense key 0x05; HARDWARE ERROR (key 0x04) never
latches. Moot while 0xB6 works, but wrong on drives that genuinely lack it.

### PHASE 1 — speed + rotation — DONE 2026-07-17 (commits 7e4aced, 702b5ac)
Hardware-verified on the PX-716A. Shipped: `accudisc_get_performance` +
`accudisc_classify_rotation` (CLV/CAV/P-CAV/Z-CLV/UNKNOWN, discriminated on
intra-segment slope so a Z-CLV step-up is not read as CAV; PX-716A CD curve is
one rising 17x..40x segment = CAV); the `speed [X] [--exact] [--start L --count
N]` subcommand over `accudisc_set_speed` (0xB6, 0xBB fallback) and
`accudisc_set_speed_range` (0xB6 only); the `features` flag split; and the
empty-tray `features --c2` false negative (now C2_UNVERIFIED, not UNSUPPORTED —
medium-not-present is sense 0x02/0x3a). `speed-report` removed.

Still open (small, deferred from Phase 1):
- **--exact discrimination**: it was accepted at 8x, where the drive runs CLV
  anyway. Test `--exact` at 24-32x to see if it forces CLV where the drive
  prefers CAV, or refuses. Not needed for correctness; a characterization nicety.
- **Push/pop for `read --speed X`**: standalone `speed X` persists (done). The
  read engine's per-read restore-to-prior and the `speeds` probe's stale "never
  auto-restored" header were NOT touched this pass — revisit with the engine.
- **Re-run the Q-vs-speed sweep** now that cap'd 0xB6 actually commands speed
  (last session's numbers were the 0xBB fallback).

### DISC-KIND GUARD — DONE 2026-07-22, fully hardware-verified

`accudisc_probe_disc()` + `accudisc_disc_probe` + the `disc` subcommand; verdict
logic isolated as the pure `adsc_disc_classify()` and unit-tested
(`tests/test_disc.c`, 14/14). Interface settled with cdda2img (§17.2) and frozen
in `cli-machine-interface.md`, which is now the reference for the token line,
the precedence order and the `reason=` slugs.

Scope (confirmed 2026-07-22): no medium (tray open/closed), CDDA including
CD-R/RW CDDA, blank CD-R/RW, unknown media. Nothing finer — CD-ROM layouts, the
Mixed Mode data half and DVD/BD need filesystem support and all land in
`NEITHER` with a slug saying which.

All five paths hardware-verified on the PX-716A (audio CD-R, blank CD-R, DVD-R,
no-medium tray-open, no-medium tray-closed). No open verification items.

**Durable lessons:**
- **The profile gate must precede the track census.** The DVD-R *did* answer
  READ TOC, reporting `data_tracks=1`. Without the gate it would have classified
  from its census, and a medium whose CTRL bits happened to read as audio would
  have reached the CD-DA rip path. Keep the order.
- **AUDIO outranks BLANK** so a written-but-appendable audio CD-R classifies
  rippable, not blank; the reverse would offer to burn over music.
- **`disc_status`/`erasable` emit -1 when not obtainable**, never 0 — 0 means
  "empty" and would read as blank.
- `tray=open|closed|unknown` comes from the ASC 0x3A qualifier and is the only
  branch depending on sense extraction rather than command output.

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
  2026-07-23** (cdda2img §30 → §31). **Lesson:** ECMA-130 §20 makes a Pause part
  of the track that *follows* it, so extents built from INDEX 01 alone leave
  hidden-track-one audio owned by nobody — the default read started past it,
  shifting every LBA against the stream and computing a wrong disc ID. New
  `accudisc_track.pregap` records it (non-zero only for session 1's first
  track); `toc` emits `pregap <n>`.
- ~~`features` no-disc false negative (C2_UNSUPPORTED/exit 1) -> UNVERIFIED.~~
  **DONE** in the Phase-1 `features` split; this line was a stale duplicate.
- `--speed 16` silently honoured as 8x, unreported. [P1] **BLOCKED on hardware.**
  The honoured rate is not a reliable MMC read-back — the {4,8,24,32} ladder is
  PX-716A-measured, and the only empirical answer is timed streaming reads (what
  the `speeds` command already does). A "report the honoured speed" path on
  `speed X` / `read --speed X` is therefore a measurement change whose thresholds
  must be validated on the drive; not a safe source-only fix. Do it in a focused
  PX-716A session, reusing `speeds.c`'s timing.
- **`cdtext` with no FILE reports itself as an unknown command. [P2]** —
  Keith 2026-07-26: *"If a value is mandatory but not provided, and you continue
  anyway, that's a silent failure."* `cli/main.c:1686` dispatches on
  `!strcmp(command, "cdtext") && nrest > 0`, so a missing argument makes the
  branch fail and control falls through the whole chain to the catch-all at
  `cli/main.c:1723`, printing `accudisc: unknown command 'cdtext'`.
  **The exit code (1, usage/argument) is already correct — the defect is the
  diagnostic.** A known command is reported as unknown, so anyone debugging it
  hunts for a typo or a version/feature mismatch rather than a missing argument;
  the device is also opened before dispatch, so the drive spins up only to emit
  a false message. This is the house failure mode in miniature: well-formed
  output, right exit code, wrong referent, and nothing downstream can catch it
  because "unknown command" is exactly what a script expects from an older
  binary.
  **Fix:** dispatch on the command name alone, validate the argument inside, and
  emit e.g. `accudisc: cdtext: FILE argument is required` plus usage; still
  exit 1. **Name the rule so it is not reintroduced: never fold "missing
  mandatory argument" into "unknown command".**
  Scope: `cdtext` is the only instance of this shape — `fulltoc` uses a ternary
  because its FILE is genuinely optional (bare form runs `cmd_fulltoc_parsed`),
  and every other command dispatches on the name alone. **Do not disturb** the
  separate and correct behaviour where a disc carrying no CD-Text writes no file
  and exits 3 (data absent). While there, disambiguate the usage text at
  `cli/main.c:25` — "(no file if absent)" reads as if it might mean the FILE
  argument; it means the disc's CD-Text.
- **A value-taking option given no value silently consumes the NEXT FLAG as its
  value. [P1]** — Raised by Keith 2026-07-26 from the man page. Every option in
  `cmd_read`'s parser (`cli/main.c:1102-1194`) tests
  `!strcmp(a, "--chunk") && i + 1 < argc`, so:
  1. **Trailing** (`read --chunk`): the guard fails, control falls to the final
     `else` at `:1191`, which dumps the whole usage text and exits 1 **with no
     message naming the offending argument**.
  2. **Mid-command** (`read --chunk --map`): `i + 1 < argc` is *true*, so `--map`
     is consumed as the value. `strtol("--map")` returns 0, and **0 is the
     sentinel for "use the default"** (`accudisc.h:1152`), so the command runs
     clean, exits 0, and silently (a) does not apply the chunk the user asked
     for and (b) never renders the map, because `--map` was eaten.
  **This is the house failure mode in its purest form: a sentinel meaning
  "default" makes a parse failure indistinguishable from an unset option.**
  Worst instance is `--progress-fd --map` → fd **0**, i.e. progress tokens
  written to *stdin*.
  Affects `--start --count --session --chunk --retries --c2-retries --verify
  --overlap --speed --progress-fd` and the path-taking options (which at least
  fail visibly later, on open). **`--tracks` is already correct and is the model
  for the fix** — it captures `strtol`'s `end` pointer and rejects trailing
  junk (`cli/main.c:1113-1126`). Fix: a shared helper that requires the next
  argv, rejects one beginning with `-`, and validates the full parse, emitting
  `accudisc: --chunk: expected a value` rather than a bare usage dump. Check
  `cmd_write`, `cmd_speeds`, `cmd_c2lag`, `cmd_cxscan` and `cmd_features` for
  the same shape.
- **The man page says `--chunk`/`--retries` are "off by default"; they are
  always in effect. [P2]** — `docs/man/accudisc.1`, "Accuracy and recovery":
  *"All of these are off by default"* is true for `--c2-retries` (0 = off),
  `--verify` (1 = off), `--overlap` (0 = off) and `--ladder` (unset), but
  **false for `--chunk` and `--retries`**, whose 0 means *use the default*
  (`engine.c:402`, `:414`), not *disable*. There is no state in which chunking
  or per-sector retries are off. Split the group, or reword to "these tune a
  single-pass streaming capture; the last four are off unless set". Keith
  2026-07-26: as written it reads as a contradiction — a default value on a
  flag that is simultaneously off.
- **`cxscan`'s options are undiscoverable. [P3]** — `cmd_cxscan` accepts
  `--start` and `--speed` (`cli/main.c:189-199`), but the usage line
  (`cli/main.c:61`) documents none, and any argument it does not recognise falls
  to `usage(stderr)` + exit 1. So the two supported options exist only in the
  source. One-line usage fix; also cover them in `docs/man/accudisc.1`.
- **The `--progress-fd` summary line emits nine keys; two documents say six.
  [P2]** — `cli/main.c:1537-1549` writes `hard c2 recovered suspect rereads
  slips subq_total subq_ok subq_bad`. Both the usage text (`cli/main.c:~103`)
  and `docs/reference/cli-machine-interface.md:109` list only the first six.
  **Not a contract break** — the keys are additive and the documented rule is
  token-primary parsing, which cdda2img follows — but
  `cli-machine-interface.md` declares this format *stable* and is the authority
  a binding is written against, so the authority is currently behind the code.
  Fix both, and add the three `subq_*` counter meanings to the existing table.
- **`ATTRIBUTION.md:25` still calls the DAO write path "upcoming". [P3]** — it
  shipped and was hardware-verified. Stale in a file the man page's new CREDITS
  section now mirrors.
- Logical type must be gated on a CD profile. [P2]
- `accudisc_eject`/`accudisc_load` header comments describe START STOP UNIT
  (LoEj), but the implementation uses block-layer CDROMEJECT/CDROMCLOSETRAY
  (device.c explains why). One-line comment fix; contract vs implementation.
- ~~`adsc_toc_parse_cue` directive-injectable through a quoted string~~ **FIXED
  2026-07-24.** A newline inside a `CD_TEXT TITLE "…"` value spilled its tail
  onto the next physical line, where a column-0 keyword parsed as a real
  directive — phantom track, forged ISRC, changed lead-out, all returning
  `ACCUDISC_OK`. The line-scan now tracks quote context and rejects an
  unterminated quote at EOL (matching cdrdao's lexer: a string may not span a
  line). Regression in `tests/test_tocparse.c`. **Owed: notify cdda2img** —
  their `escape_toc_string` was our only guard until this landed.
- ~~`toc` reported `leadin_unreadable` on roughly half of all discs~~ **FIXED
  2026-07-24 (field report).** READ TOC's second transfer was sized from the
  returned data-length header; a full TOC is `37 + 11*ntracks` bytes, i.e. **odd
  on every disc with an EVEN track count**, and ATAPI moves 16 bits at a time, so
  the host adapter rejected it before the drive answered. Fix: shared
  `adsc_alloc_even()` (`src/mmc/mmc.h`), applied to READ TOC and to
  `mode_sense10`, which had the identical latent defect. Regression:
  `tests/test_alloc_even.c`.
  **Two lessons worth keeping.** (1) A *transport* fault was being reported as a
  *disc health* verdict — the tool blamed the media. That is the dangerous half:
  the failure was well-formed and pointed at the wrong subject. (2) It went
  unattributed for months because `ACCUDISC_ERR_IO` discarded
  `errno`/`host_status`/`driver_status`; there was a bare DID_ERROR (0x07) and no
  sense to read. Hence `accudisc_last_io()`.
- **[P2] Parser hostile-input sweep** (RECORDING_PLAN.md §11.9 REVIEW QUESTION,
  adopted from cdda2img §40.3): apply *"what does this accept if the producer is
  hostile, or merely wrong?"* to every parser fed from a boundary we don't
  control — drive responses in `mmc/`, `cdda/subq.c`, `meta/cdtext.c`, `toc/`.
  The `adsc_toc_parse_cue` injection above was one instance of a pattern that
  surfaced three times in one session across three codebases (each trusted its
  boundary because the *usual* producer is well-behaved); do this as one sweep,
  not three separate fixes as they bite.
- **[P2] `.toc` FILE start/length: accept the bare-integer forms.** cdrdao's
  grammar (`private/code/cdrdao/trackdb/TocParser.g`, rules `samples` and
  `dataLength`) accepts *either* an MSF *or* a plain integer for the two fields
  of `FILE "x" <start> <length>` — and the units differ per field: a bare start
  is **samples**, a bare length is **bytes** (`dataLength` multiplies only the
  MSF form by the block size). `adsc_toc_parse_cue` requires MSF for both and
  returns `ACCUDISC_ERR_INVAL` otherwise, so a perfectly legal cdrdao .toc is
  refused as `write: invalid argument` with no indication of which line lost.
  Refusing is the *right* failure mode (misreading a sample count as frames
  would silently mislocate every track), so this is a compatibility gap, not a
  correctness bug — but the diagnostic is useless and the input is legal.
  Two parts: accept both forms with the correct per-field units, and report the
  offending line number instead of a bare INVAL. Matters for interop: cdda2img
  generates .toc files and may well emit the integer form.

#### Bug audit 2026-07-23 (full report: `private/bugs/2026-07-23-bug-audit.md`)
Correctness sweep of the whole tree, 7 findings, 0 critical. `rw.c` RS/GF math
and the pregap/extent/guard interaction both audited **clean**. Top three
verified against source before recording. **All seven fixed 2026-07-23**, each
with a regression test; suite clean under ASan+UBSan.

| id | defect | where |
|---|---|---|
| F-001 [P1] | SG_IO `resid` never checked — silent corrupt audio | `transport/sgio.c` |
| F-002 [P2] | `accudisc_rw_feed` desynced the de-interleave when `max < 4` | `cdda/rw.c` |
| F-003 [P2] | DAO cue buffer 32 bytes short of the 400-entry worst case | `write/burn.c` |
| F-004 [P3] | `--cdg` open failure leaked the status map / open files | `cli/main.c` |
| F-005 [P3] | c2lag pass-2 window exceeded `ADSC_MAX_XFER` | `drive/c2lag.c` |
| F-006 [P3] | `accudisc_q_parse` decoded payload before the CRC gate | `cdda/subq.c` |
| F-007 [P4] | `accudisc_lba_to_msf` cast a negative quotient to `uint8_t` | `cdda/subq.c` |

**Durable lessons:**
- **F-001 is the worst class: a short read that *succeeds*.** Any GOOD status was
  treated as a full transfer, so a short one left stale bytes in the chunk tail,
  streamed to `--pcm` marked `MAP_OK`, with the C2/consensus net never running.
  Fixed at the correct seam: the transport *reports* the residual and does not
  judge it (allocation-length commands legitimately transfer short); only
  `adsc_mmc_read_cd`, which alone knows its exact expected length, promotes
  `resid != 0` to `ACCUDISC_ERR_SHORT`. Deferred nicety: re-read only the missing
  tail sectors rather than the whole span.
- **F-002/F-006: a sink or a decoder must never silently drop or half-decode.**
  Both were fixed by rejecting the impossible request up front rather than
  truncating (`max < ACCUDISC_RW_PACKS_PER_SEC` → `ERR_INVAL`; CRC-failed frame
  → `ERR_CRC` before any BCD/ISRC decode, leaving those fields zeroed).

### Meta — a caution for next session
Four confident spec-derived claims were overturned by hardware today: "setcap is
an artifact of the RO open", "page 2A is a placebo", "MMC has no rotation
lever", and "SpeedRead defeats the governor". **Measure first; the drive wins.**

---

## (superseded) NEXT SESSION — real read-speed control, then Q-channel preservation

Agreed 2026-07-15, executed, and superseded by the plan above. The plan itself is
gone; what follows is the durable result of running it. Where a finding is still
open it says so.

### Speed control — DONE + live-verified 2026-07-15

SET STREAMING (0xB6) works: commanded 4x/8x delivered 4.01x/8.01x at outer
windows where CAV would give ~30x — a binding ceiling `CDROM_SELECT_SPEED` never
could. Details in `drivers/plextor/PROTOCOL.md`.

**STILL OPEN — read-engine throughput cap.** The recovery `read` engine sustains
only ~5x on a *clean* disc where raw streaming (`speeds`, bare READ CD) reaches
~12–19x at the same radius: roughly 70 ms/command of per-chunk overhead. Not a
speed-control bug, a ripping-throughput one. Suspects: default `chunk_sectors`,
per-chunk cache-defeat, status-map write cost, a synchronous stall between
commands. Matters because whole-disc Q baselines run through this path.

### SET STREAMING is a streaming *contract*, not a speed knob

Setting *any* ceiling — even 40x, above natural — collapses the recovery engine's
throughput to ~5x: the drive tunes for constant-rate delivery, and the engine's
~37 ms/chunk of C2+Q processing reads as "host not consuming", so the drive
throttles. **Rule: first pass free-runs (no `--speed`); slow a damaged span with
SET STREAMING low (4x/8x are exact). Never use it to set a high ceiling for a
bursty read.**

Ceiling on the PX-716A for a Q-preserving read: **~18.7x with subchannel** (the
drive cannot deinterleave subcode faster), 25.9x with C2 but no sub, 21.4x
audio-only.

### The PX-716A disc-init governor — SOLVED (2026-07-16)

The long-standing "stuck at 32x, but 40x after an eject" mystery is **not** a mode
we armed: **the drive profiles the disc at init and pins a quality-appropriate
ceiling.** Repeatable across eject/load cycles — ZZ Top (pristine) inits at 40x
(= page 2A max), ABBA (scratched) at 32x. The disc is the only variable.

- **Page 2A was never a placebo.** `current_x` is real, readable state: it tracks
  SET STREAMING exactly and reports the governor's init ceiling before we set
  anything. Earlier "always 40x" readings were because no set had ever *succeeded*
  (silent fallback to 0xBB).
- **Free damage triage.** At init, `current_x < max_x` is the drive's own quality
  verdict on the medium — an absolute, vendor-authored signal costing zero reads.
  **Caveat:** it only holds with the read-speed uncap OFF, or a pristine disc
  reads 40 < 48 and is falsely flagged. `accudisc_speed_uncap_probe` (landed
  2026-07-25) answers ON/OFF/LIKELY_ON/UNKNOWN without a driver attached; triage
  built on it must treat UNKNOWN as "do not flag", never as "off".
- **SpeedRead does NOT defeat the governor.** Measured on both discs including a
  full eject/load re-init with SpeedRead on: the ceiling holds (40x pristine, 32x
  scratched). The two act on **orthogonal axes** — the governor caps DATA RATE,
  SpeedRead raises RPM (nominal curve scales ×1.2, 17–40x → 20–48x).
  **Why Q dies:** at the inner radius the natural rate under SpeedRead (20x) is
  below the cap, so the cap never binds, the drive spins at full SpeedRead RPM,
  and Q — which has no CIRC — fails. At the outer radius natural 48x exceeds the
  cap, RPM is throttled, and Q survives. The governor guards the wrong axis and
  never protects the inner tracks. This predicts the measured dead zone exactly
  (curve crosses 32x at ~44% into ABBA; measured inner 10–60% dead, 70–100%
  clean).
  **SpeedRead is therefore a pure liability for CD-DA:** it cannot raise the rate
  ceiling, so its only headroom is the inner radius — exactly where its RPM kills
  Q. The whole-disc A/B showed no throughput gain at all (both 24.2x) while Q
  fell 99.2% → 40.6%.
  **Partly landed 2026-07-25:** the library refuses a subchannel read when the
  uncap is *authoritatively* on (`ACCUDISC_ERR_UNSAFE_COMBINATION`, overridable
  via `allow_unsafe`) and warns when it is only inferred. Escalating to "refuse
  the uncap for any CD-DA read" is still open and would want the "no throughput
  gain" claim re-measured on a second drive first — it rests on one A/B.
  (See also item 1 of the 2026-07-26 outstanding list, which directs the guards
  be removed.)
- **Honoured speed ladder is discrete: {4, 8, 24, 32}.** 1–3 → 4; 6 → 4; 9–23 →
  8; 28 → 24; 40/48 → 32. Two disjoint regimes, explained by the nominal CAV
  curve starting at 17.00x: {4,8} are CLV (a ceiling below 17x binds at every
  radius, so the curve is flat), {24,32} are CAV (clamping only the outer
  region). The 9–23 dead zone is the gap between the top CLV rung and the CAV
  floor. **40x is not settable** — it is only reached by free-running at the outer
  edge. Still open: `speed X` reports what was asked, not what the drive honoured
  (see the `--speed 16` item above).
- **GET PERFORMANCE nominal is RPM-derived, not medium-measured.** Identical
  across no-disc / ABBA / ZZ Top — `end_lba` is always 359999, never the real
  lead-out — but it *does* track SpeedRead. Constant across discs, not across
  drive state. DVD case untested.

### Q subchannel — architecture, findings, and what is left

**Key architecture fact.** C1/C2/CU protect only the main (audio) channel, via
CIRC Reed-Solomon. The subchannel is **not** inside CIRC: Q's only integrity
check is one CRC-16/CCITT per frame — **detection, no correction.** So a sector
can carry pristine audio (0 C2) and a dead Q frame. Confirmed on real damage: an
800-sector window read 0 C2-flagged sectors and 22 CRC-bad Q frames. **Q and C2
are orthogonal domains.**

**Q's compensating advantage** is that it is near-deterministic — abs MSF +1 per
sector, track/index piecewise-constant, rel MSF counting down in a pregap and up
in a track. Enough CRC-valid *anchors* fit the model and interpolate the gaps.
The one non-interpolable event is the index 0→1 transition, which needs at least
one clean anchor near each boundary. **So target re-reads at index boundaries,
not uniformly.**

**Two failure populations — the key result.** Three cache-defeated re-reads of the
same region: ~6 LBAs bad in *all* passes (static physical defect; re-reads never
fix), ~6 bad in only 1–2 passes (transient; recovered by re-read + consensus).
Neither lever alone suffices — consensus for the transient population,
deterministic-model interpolation for the static one.

**Speed barely affects the Q error rate inside the drive's governor envelope**
(4x → 9 bad, 8x → 7, free-run → 7–9 on the same window): defect-driven, not
RPM-driven. *Outside* the envelope — with the uncap defeating the ceiling — speed
matters enormously. Both datasets are correct; they sample different regimes.
**Lever = re-reads + model. Never defeat the governor on a damaged disc.**

**Clean-disc baseline (ZZ Top, new).** Radius sweep, 3000-sector windows:
uniform ~99.9% Q-CRC-ok at every radius, **no dead zone**. Max speed does not
corrupt Q on a clean disc, so the dead-zone Q loss and cdda2img's §9 missing
pregaps are **damage-driven, not speed-driven**.

**Whole-disc pregap census (ABBA, all 19 boundaries) — "9 of 19" RESOLVED.**
9 tracks have real pregaps (t2–t7, t9, t14, t18) of 47–50 frames, plus track 1's
33-frame lead-in gap; the other 9 are **genuinely gapless** — index-1 → index-1
with no index-0 frames, confirmed even where the boundary region was damaged. The
starting hypothesis ("all tracks have pregaps, damage hides them") is **refuted
for this disc**. Dead frames *inside* a pregap are fully covered by the surviving
countdown anchors, so zero pregap information is lost.

**The CRC gate is load-bearing.** Rejected frames decoded as abs142:38, index19,
adr9 — garbage a non-CRC-checking reader would splice in as phantom indices.
Damage does not merely lose Q metadata, it **injects false Q metadata**; the
per-frame CRC-16 is the only thing between the two. (Related: only ADR=1 frames
are position — ADR=2 is MCN, ADR=3 is ISRC — or the ~1-in-98 MCN frames
masquerade as index-0 boundaries and manufacture phantom pregaps.)

**Damage is localized, not radius-graded.** Sweep of 2000-sector windows across
ABBA: 96–100% Q-ok everywhere, with a few damaged spots (inner ~LBA 40k, outer
~LBA 340k, pinpoint hits such as t16@281733). Surface blemishes at specific
places, not an inner dead zone. This disc therefore needs no Q recovery to get
pregaps right — the recovery machinery is for a **worse** disc that loses a
boundary anchor entirely.

**Index/pregap decoder — DONE 2026-07-16.** `accudisc_index_map_decode`
(`src/cdda/index_map.c`) + `accudisc pregaps` + `test_index_map`. CRC-gates every
frame and classifies each TOC boundary PRESENT / NONE / UNKNOWN / NO_DATA.
**Key rule: a pregap ABUTS index-1**, so gaplessness is decided by the
boundary-abutting frame (walk down from L-1, skipping MCN/ISRC frames), never by
any bad frame in a wide window — that over-flags. Live on ABBA: 9 pregaps, 9
gapless, exactly 1 UNKNOWN (t16, whose L-1 frame 281732 is physically dead),
correctly separated from t19 which the manual pass had lumped with it.
`pregaps` exits 3 if any boundary is UNKNOWN.

**Still open — Q recovery.** Same items as "Carried over — Q recovery" above:
model reconstruction across a dead abutting frame (t16 is the standing test case,
captured offline as `tests/data/abba_t16_unknown_boundary.sub`, LBA
281333..281762); targeted re-read + consensus for boundaries whose neighbours are
also damaged; and the unified re-read predicate — a sector fails if C2-dirty OR
Q-CRC-bad, both from one READ CD, with the status map gaining a second dimension
(audio | Q). Q-CRC counters themselves are **done**
(`subq_total`/`subq_ok` in `accudisc_read_stats`, printed by the read summary and
mirrored on `--progress-fd`).

## Eject feature — DONE

`accudisc eject` / `accudisc load` ship (block-layer `CDROMEJECT` /
`CDROMCLOSETRAY`, not START STOP UNIT — see the header-comment item above).

## Investigate how Plextor drives handle speeds — SOLVED

The "permanently stuck at 32x, but 40x after an eject" report is the disc-init
governor, explained in full above. The original hydrogenaudio thread
(<https://hydrogenaudio.org/index.php/topic,28739.0.html>) had it right: the
drive determines an optimal read speed at disc init and imposes it in hardware.
The `0xEA arm refused` seen alongside it was the driver selftest failing on a
read-only open, not a speed matter.

## ATIP / media identification

- ~~**Wire the ATIP catalog into a lookup + CLI.**~~ **DONE 2026-07-12.**
  `accudisc media` reads the disc ATIP (READ TOC/PMA/ATIP fmt 4) →
  manufacturer + code + capacity + CD-R/RW; `accudisc_read_atip()` /
  `accudisc_atip_manufacturer()` in `src/drive/media_db.c`, unit-tested and
  live-verified on a Taiyo Yuden blank. **Lookup keys on `sec` + frame-decade**,
  so 97:24:01 resolves the 97:24:00 entry and the decade separates makers
  sharing a `sec`. *Optional follow-up:* surface spiral length (record+0xDC) and
  the ATIP reference-speed / indicative-power fields.
- **Public ATIP cross-reference pass.** *Largely done 2026-07-12:* diffed against
  cdrecord's `diskid.c` (107/123 agree) and Nero 2026 — both carry the same
  effectively-frozen registry, so the feared "post-2007 gaps" are moot.
  cdrecord's 3 high-confidence uniques folded in (`gen_media_db.py` → 134 codes).
  *Remaining (optional):* a broader web ATIP database diff for obscure codes none
  of the three list.

## Recording

- **CD-Text on write — two modes; design of record is `RECORDING_PLAN.md` §11.**
  - **v0 — PASS-THROUGH: SHIPPED 2026-07-24.** `write --cdtext FILE` consumes the
    raw READ TOC format-0x05 blob byte-for-byte and injects the packs into the
    SEND CUE SHEET lead-in (dataForm 0x41 + R-W subchannel packs, ring-filled).
    No encode step, so it handles re-burns of a captured disc. Verified on CDEmu
    (§11.7) and on physical media (§11.8, PX-716A + Taiyo Yuden): 760 bytes in,
    760 out, `cmp` identical, alongside 19 correct ISRCs and the MCN.
    **Lesson from the first, failed attempt: one wrong byte in the cue sheet
    produced perfect audio/MCN/ISRC and *no CD-Text at all*, with every command
    returning success.** A byte-for-byte read-back compare is the only assertion
    that catches that class; decoding for a human diagnostic is not.
  - **v1 — AUTHORED (design settled 2026-07-24; NOT IMPLEMENTED — `adsc_toc_parse_cue`
    still ignores `CD_TEXT`, and there is no strings→packs encoder in the tree).**
    strings / `.toc` `CD_TEXT` blocks → 18-byte packs → the blob v0 already knows
    how to lay down.
    **INPUT SURFACE IS SETTLED, and it is the `.toc`.** An extracted RBI holds one
    s16le PCM stream and one ASCII `.toc`; there is no CD-Text blob block, so v0
    has no reachable input in this pipeline and the Step C/D fixture was
    manufactured via a CDEmu detour the workflow cannot reproduce. First task is
    therefore `adsc_toc_parse_cue`, which ignores `CD_TEXT` today — a new parser
    fed from a boundary we don't control, so it lands inside the `[P2]` hostile-
    input sweep above.
    **Authoring is also the stronger path on charset**, not a compromise: a blob's
    payload is never inspected (by design), so an illegal codepoint reaches the
    disc and fails to render on standalone hi-fi equipment — `cdemu_utf8__33packs`
    in our corpus is UTF-8 declared as charset 0x00. Mapping strings to bytes
    forces every character to be confronted, which is why the rule below is
    enforceable here and unenforceable in v0.
    Needed because
    cdda2img has NO strings→packs encoder (their `cdtext.py` is decode-only) and
    cdrdao is what encodes for them today — so once cdrdao leaves, a *fresh* disc
    authored from metadata has no CD-Text unless AccuDisc encodes it. cdda2img's
    interim until this lands is re-burn-only (their option (a); they are NOT
    porting their own encoder). **Locked first-cut scope (cdda2img §43):** block
    0, single language, single-byte, pack types 0x80 title (disc+track) + 0x81
    performer (disc+track) + 0x86 disc-id (disc-level, conditional) + mandatory
    0x8f size-info. NB 0x86 is NOT in our current decoder (cdtext.c does 0x80/0x81
    only) — new to us but cheap (encoder is pack-type-agnostic); consider adding
    0x86 *decode* too for the `cdtext` display (separate, off critical path). OUT:
    0x82 songwriter (never authored), 0x8e UPC/ISRC (that's Q-subcode via
    CATALOG/ISRC → cuesheet, not a CD-Text pack). Unencodable codepoint fails
    BEFORE the burn (§11.9 INVARIANT rule 4). Reference: cdrdao `CdTextEncoder` /
    `writeCdTextLeadIn`, libmirage `cdtext-coder.c` — rewrite, never copy.
    Sequenced after v0.
    - **UNENCODABLE CODEPOINTS — the failure mode is the requirement** (adopted
      from cdda2img §55.2, whose evidence is a disc we both handled). cdrdao,
      given `"Voulez‐Vous (edit)"` from a `.toc`, **silently discarded the
      whole TITLE and renumbered the following packs**, so the gap decodes as a
      well-formed "track 13 has no title" — indistinguishable from a disc
      genuinely authored that way. It cost a day of wrong theories about where
      the gap came from. Our encoder must not reproduce that.
      **MANDATORY: never drop silently.** An unencodable field is either
      substituted *with a report* or refused *with a report* naming the field and
      the codepoint. `ACCUDISC_WROTE_WITH_CAVEATS` / exit 3 already exists and
      fits the substitution case exactly. A silent drop is a well-formed lie,
      which is worse than an error — nothing downstream can detect it.
      **OPTIONAL: the transliteration itself** (U+2010 HYPHEN → `-`, U+2019 →
      `'`, typographic quotes/dashes/ellipsis, NBSP). Deliberately *not*
      mandatory, per cdda2img §56.2: transliteration tables are not
      standardised, so two tools folding independently can disagree and make a
      *verifier* report phantom CD-Text mismatches between the `.toc` and the
      disc. Callers that fold upstream (cdda2img's `fold_cdtext` does) make ours
      a no-op; ours is a net for callers that don't, never a second authority on
      someone else's strings.
      **Licensing: do NOT pull in a transliteration dependency.** AccuDisc is
      MIT throughout; a GPL-3 folding library would break that, and the 95 % case
      is one small punctuation class (U+2010–2015, U+2018/19/201C/201D, U+2026,
      NBSP) — a few dozen lines, with everything else reported rather than
      discarded.
      Applies to the *decoder* display too: nothing should ever present "no
      title" without a way to tell "absent on the disc" from "we couldn't
      render it".
  Note: CD-Text does NOT affect the Disc ID (pure TOC) — content fidelity,
  separate from the pregap item below.
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

- **A per-sector Q-CRC map, alongside the audio one. [P3], NICETY — Keith
  2026-07-26, "just a nicety, if possible, not an absolute necessity".** Today
  `--map`/`--map-file` cover audio only; Q health is reported solely as the
  `subq_total`/`subq_ok`/`subq_bad` summary. Wanted: the same per-sector picture
  for Q.

  **Possible, and cheaper than it looks — the verdict already exists and is
  discarded.** `src/read/engine.c:569-580` runs
  `accudisc_sub_extract_q` → `accudisc_q_parse` per delivered sector and
  collapses `qd.crc_ok` straight into the counters. This is a *store*, not a new
  measurement: no extra drive I/O, no second pass. It is also the "status map
  gains a second dimension (audio | Q)" line already promised under the unified
  re-read predicate — building it here would discharge that.

  **Four design points, each with a trap:**
  1. **Do NOT pack Q into the existing map byte.** A sector can be audio-clean
     *and* Q-dead at once — that orthogonality is the core Q finding (CIRC
     protects the main channel; Q has only a CRC-16, detection without
     correction), and the byte holds one state, so the two dimensions would
     compete for one slot. Use a second byte array mirroring `--map-file`'s
     existing contract (one byte per sector, mmap-able, `--qmap-file`).
     Incidentally there IS room in the byte — low-nibble states 6..15 are
     unused (`accudisc.h:1072-1077`) — which is the tempting wrong answer.
  2. **"Q ok" must mean CRC-valid, not "position present".** ADR alternates
     1=position / 2=MCN / 3=ISRC, roughly one MCN frame per 98. A CRC-good
     ADR=2 frame is healthy but carries no position; treating it as missing
     position is precisely the bug that manufactured phantom pregaps in the
     early ABBA analysis. Encode ADR as well as CRC state — the second nibble
     is the natural home.
  3. **"Not captured" needs a state distinct from "ok".** Q is only meaningful
     with `--sub raw`; the engine's own comment notes `SUB_Q` is CRC-gated
     inside the drive, so there is no verdict to report on that path. Without a
     distinct value, absence renders as health — the same defect as
     `disc_status` returning 0 instead of -1 when unobtainable.
  4. **`--map` rendering needs its own legend**, reusing the worst-state-in-
     bucket condensation. Do not reuse the audio glyphs for different meanings.

  Interface note: `--qmap-file` is purely additive, so
  `cli-machine-interface.md`'s stability guarantee is untroubled. The public
  `ACCUDISC_MAP_*` constants stay as they are; a Q map wants its own
  `ACCUDISC_QMAP_*` set rather than extensions that make the audio encoding
  ambiguous.

- **`speeds`: report min/max as well as the current single figure. [P3],
  NICETY — Keith 2026-07-26, explicitly "not essential".** Measure each rung at
  the inner ring, the middle and the outer ring instead of one location, so a
  rung reports a range rather than a point. Wanted for its own sake and as a
  **sanity check** that our timing arithmetic and the drive are both behaving.

  **It is a good check, and here is why it works:** on a CAV drive the spread
  *is* the CAV curve — inner slow, outer fast — so a rung with no spread on a
  CAV drive means our timing is wrong, the cache is serving us, or the rung is
  actually clamped. Conversely the {4,8} rungs are CLV (a ceiling below the
  17.00x curve floor binds at every radius) and **must come out flat**, while
  {24,32} are CAV and must spread. That makes this an independent cross-check on
  `accudisc_classify_rotation` and on the GET PERFORMANCE nominal curve, which
  today are the only things asserting rotation mode.

  **Four confounds must be controlled or the instrument reports the wrong
  thing** — this is a measurement, so it inherits every lesson the Q work
  learned:
  1. **The disc-init governor** pins a per-disc ceiling at load (clean 40x,
     scratched 32x). A damaged disc's spread reflects the governor, not
     geometry.
  2. **SpeedRead / the uncap** scales the nominal curve x1.2 and is persistent
     drive state a previous session may have left on. Record its state with the
     result.
  3. **Drive contention** — another process on the same device collapses
     measurements. Take `flock /var/tmp/sr0.lock` and write `/var/tmp/sr0.owner`.
  4. **Media dependence**, which Keith raised: the curve is per-medium. A figure
     is only comparable against another run on the *same disc*.

  **Two design decisions, both with a trap:**

  - **DO NOT rename `measured=` to `avg=`.** `cli-machine-interface.md:176`
    declares the `speeds` line stable and token-primary, and explicitly permits
    *appending* new keys — appending `min=`/`max=` is therefore free, but a
    rename breaks any parser keyed on `measured`. cdda2img is on our live tree,
    so a rename additionally owes an API_PLAN §8 ledger row *before* the commit.
    Separately, **"avg" would be a false label**: the mean of three spot samples
    is not the disc average, and naming it so claims more than the measurement
    supports. Keep `measured=` as the existing single-window figure, append
    `min=`/`max=`, and document all three.
  - **`accudisc_speed_rung` is a caller-allocated transparent OUT struct with no
    size field** (`include/accudisc/accudisc.h:1041`; `{requested_x,
    reported_x, measured_cx}`, 6 bytes, filled through `out`). Adding
    `min_cx`/`max_cx` grows it — **exactly the API_PLAN §7.1 hazard**, on a
    struct that never got the `uint32_t size` treatment `read_req`/`read_stats`
    did. Either give it a size field in the same change, or take the break
    knowingly while nothing outside this repo links the library — and note that
    window **closes when the Python binding ships** (§7.1's own reasoning).

  **Implementation notes:** cost is 3x the timed windows per rung (default
  ladder is up to 8 rungs after page-2A filtering). Each of the three locations
  needs its *own* cache-fresh window per rung, so the window allocator must not
  collide across rungs *or* radii — the existing guarantee is only per-rung.
  Prefer keeping the current single-location behaviour as the default and
  putting the three-point sweep behind an opt-in flag, so the default output and
  probe time are unchanged; `--start` then keeps its present meaning instead of
  being silently reinterpreted as a centre point.

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

### Full-TOC → TOC automatic fallback — DONE + HARDWARE-VERIFIED 2026-07-21/22

`accudisc_read_toc_src()` prefers READ TOC format 0x02 (the raw lead-in
Q-channel, which carries session structure) and degrades to format 0x00,
reporting `source=` and `degrade=` (`none` / `leadin_unreadable` /
`leadin_absent` / `leadin_malformed`). `toc` exits **0** on a degrade — the
command promises track geometry and a degrade still delivers it in full; failing
would regress exactly the discs the fallback exists to serve. Frozen in
`cli-machine-interface.md`; the pure conversion `adsc_toc_from_fulltoc()` is
unit-tested (`tests/test_tocsrc.c`, 13/13). Both paths hardware-proven on the
PX-716A, and the degrade path's geometry cross-validated **byte-identical**
against the drive's own cooked format-0x00 answer — two independent decodes
agreeing on the MSF→LBA arithmetic, track slotting and extents.

**Durable lessons:**
- **Format 0x02 does NOT carry INDEX 00 / pregap data.** The lead-in TOC holds
  track *starts* (INDEX 01), A0/A1/A2 and session structure, nothing more.
  Pregaps exist solely in the program-area Q subchannel, so a successful 0x02
  supplies no more pregap data than a degraded 0x00 — the degrade costs only
  session structure. Both an earlier note of ours and cdda2img's §26.2 assumed
  otherwise. **Proven empirically, not just read off the spec:** on an MPO CD-R
  whose lead-in is completely unreadable, `pregaps` still recovered all 11
  pregaps (tracks 2–12, every one 149f) with near-perfect Q CRC.
- **A failing lead-in on a healthy program area is a real disc-health signal**,
  and it is disc-specific rather than a CD-R class property (the same session's
  Ritek CD-R read 0x02 clean). That is what `degrade=` exists to carry; callers
  archiving provenance should record it.
- **Cost:** preferring 0x02 is free when it succeeds (~5 ms either way) and costs
  ~166 ms only on a degrade — the drive giving up on the lead-in. Trivial once,
  significant for a caller polling `toc` in a loop on a degraded disc.

- **Phase B (`toc --pregaps`) — DROPPED 2026-07-22 at the requester's request.**
  cdda2img §27 withdrew the ask: their pregaps come from the Q stream and never
  from a TOC, and their rip path always captures `--sub raw`, so this would be a
  second program-area pass over data they already hold. No other consumer wants
  it; `pregaps` stays the standalone diagnostic. The token still ships (always
  `none`), **renamed `pregaps=` → `subq_indices=` on 2026-07-25** because the old
  spelling collided with the per-track `pregap <n>` field and real output read as
  a self-contradiction. **Do not build this without a new requester.**
- **Phase C (retry counter) — WITHDRAWN 2026-07-22 by the requester.** cdda2img
  §26.5 assumed a retry loop existed in the 0x02 path; there is none anywhere in
  `src/mmc/` or `src/transport/`. They then argued the current behaviour is
  *better*: a single-attempt failure means one specific thing, "failed after N
  tries" blurs it, and retries cost time on exactly the discs already failing.
  **Do not add retries to make a counter possible.** Retry *behaviour*, if ever
  wanted, is a separate deliberate decision.

- ~~**[P1] Emit a session COUNT on the degrade path**~~ (cdda2img §28) — **DONE
  and hardware-verified 2026-07-22.** `accudisc_toc_info.session_count` /
  `accudisc_toc.sessions_total`, emitted as `session_count=<n>` on the `toc` line.

  **Why it was needed:** a **multi-session all-audio** disc is otherwise
  undetectable. It passes an "all audio ⇒ safe" test while format 0x00 hands back
  the *last* session's lead-out rather than session 1's — wrong lead-out, wrong
  disc ID, silently. A track census provably cannot see session boundaries.

  **Why it works:** READ DISC INFORMATION answers from the drive's own disc
  model, not by re-reading the groove, so it still speaks when the lead-in will
  not. Confirmed on the MPO CD-R whose lead-in does not read
  (`degrade=leadin_unreadable ... session_count=1`), and the count corroborated
  three independent ways: READ DISC INFORMATION byte 4 (sessions = 1), byte 5
  (first track in last session = 1), and libcdio `cd-info`'s
  `Last CD Session LSN: 0` — a different tool issuing a different command.

  Three behaviours where there was one: **1** = fully reconstructible (one
  session owns every track and format 0's lead-out *is* its lead-out), so the
  model is synthesised and a dead-lead-in disc stays wholly rippable — exercised
  end to end, `read` resolving `session 1, lba 0 count 236435`; **>1** refuses
  with `session_unmapped` (the seams are known to exist, their positions are
  not); **0** falls back to the conservative all-audio walk.

- ~~**[P1] Mixed Mode: session selection is too coarse**~~ — **CLOSED
  2026-07-22.** On a Mixed Mode CD one session holds a data track first, then
  audio. Every step was individually right — the default audio session *is*
  session 1, its whole-session range *does* start at LBA 0, and the guard *does*
  correctly refuse a range containing a data track — and the outcome was that
  `accudisc read` with no arguments failed on the format. **Lesson: correct
  components can compose into a wrong behaviour; the defect was in the seam, not
  in any of the parts.**

  Fix: `accudisc_toc_session_audio_range()` narrows the default to the session's
  **audio tracks**, plus `accudisc_toc_track_range()` and `--track N` /
  `--tracks A-B`. **The guard was not relaxed.** Verified on hardware (PX-716A,
  Taiyo Yuden CD-R: data track 1 of 138230 sectors, audio 2–11, lead-out 342197):
  the default resolves to `lba 138230 count 203967`, `--tracks 2-11` resolves
  identically (independent confirmation, not one code path agreeing with itself),
  and the full rip produced 203967 × 2352 bytes exactly with 0 C2-flagged
  sectors. Still correctly refused: `--track 1`, `--tracks 1-11` (`data_track`),
  and a cross-session span.

  One case is **refused rather than solved**: audio tracks either side of a data
  track within one session cannot be expressed as a single range, so
  `ACCUDISC_ERR_UNSUPPORTED` is returned and the caller must name tracks. Legal
  on the wire, unit-tested, never observed on real media.

- **[P3]** Bindings (`bindings/python`, `bindings/rust`) do not yet expose
  `accudisc_read_toc_src` or `accudisc_probe_disc`; they are generated against
  the public header, so both are additive whenever next regenerated.

## Formats and specs

- ~~**[P2] CD+G capture and pack extraction**~~ — **DONE 2026-07-22.**
  `src/cdda/rw.c`, public API `accudisc_rw_*`, CLI `read --cdg FILE`.

  Built against the normative Philips/Sony *Subcode/Control and Display System —
  Channels R-W* (Nov 1991). Structure per §5.1: 6 bits = SYMBOL, 24 symbols =
  PACK, 4 packs = PACKET, so one packet per sector, 75 packets/s and **300
  packs/s**; `.cdg` is the 24-byte **pack** stream. Three stages: extract R-W as
  the low 6 bits of each subcode byte; undo the 8-pack convolutional interleave
  and its position permutation (transpositions (1,18), (2,5), (3,23)); RS-decode
  over GF(2^6), `P(X)=X^6+X+1`. Both codes are conventional RS with consecutive
  roots, so one routine serves both parameterised by length and parity count —
  (24,20) across the pack correcting 2, (4,2) over symbols 0–3 correcting 1.

  **Two corrections the spec forced on an earlier version of this entry:** the
  pack/packet nesting was inverted, and R-W *is* Reed-Solomon protected where it
  had been claimed not to be.

  **Findings worth keeping:**
  - **The stray-symbol count VARIES between reads of the same sectors** (39, 66,
    48 on three passes). R-W gets no C1/C2 correction from the drive, so these
    are transient channel errors, not pressed-in bits — which is the empirical
    argument for doing the RS decode at all. Without it every rip of the same
    disc would differ.
  - Measured on a Mixed Mode CD-R with **no** CD+G (still a real test): 1000
    sectors → 3993 packs (= 4n−7, the de-interleave span costing 7 at the tail),
    95,832 bytes exactly, all MODE ZERO. In one read capturing `--subf` and
    `--cdg` together the raw subchannel held 48 stray symbols and the P code
    repaired exactly 48 across 47 packs, one of which had two — so the t=2 path
    fired on real data.
  - Verification is a round trip against an **independent** encoder built by a
    different method (the encoder solves H*V=0 by Gaussian elimination, the
    decoder works from syndromes), so a shared mistake is unlikely to cancel out.

  **Still open: end-to-end verification needs an actual CD+G disc.** The one
  unproven assumption is the order in which the drive returns the 96 subcode
  bytes; that rests on the Q path in `subq.c`, which reads bit 6 of the same
  bytes and is hardware-verified. Worth acquiring a karaoke disc.

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
  (CD-R) Vols 1–2 and Part III (CD-RW) Vol 1 acquired, with the Multisession CD,
  Enhanced Music CD, R–W subcode, CD Text Mode, CD-ROM XA, MMC-3, SCSI-2 and SACD
  specs. **Public** ECMA-130 / ECMA-394 / ECMA-395 are cited but deliberately
  **not committed** — ECMA publishes them for free download, so a citation beats
  megabytes of permanent history.

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

  **The audit found one real hole, and it was not a crash.** Memory safety was
  already sound (the suite runs clean under ASan + UBSan with
  `-fno-sanitize-recover=all` against `tests/test_toc_hostile.c`). The defect was
  the third failure mode — **silently normalising a contradictory TOC into a
  plausible-looking one.** `toc_fill_extents()` walked tracks in *track-number*
  order and treated that as *address* order; Kaspersky ch. 6's "Incorrect
  Starting Address for the Track" exists to break that coincidence. When it did,
  the out-of-order data track's extent collapsed to zero — invisible to the map —
  while its neighbour's stretched over the region it vacated, and
  `accudisc_check_audio_range()` returned **ok** for a span covering a data
  track.

  **Fixed two ways, deliberately keeping both.** Extents are computed in
  *address* order (`next_by_address()`) — not a hardening measure but the correct
  definition, identical on honest media; and `accudisc_toc.anomalies` records
  structural defects, the three meaning the map cannot be believed (`lba_order`,
  `overlap`, `leadout_before`) making the guard refuse with `toc_untrusted`.
  **The first defence is only as good as our imagination about which orderings
  can be violated; the second does not depend on having predicted the trick.**
  Six further flags are reported only — their discs are still described
  correctly, and over-refusing would break media that reads fine.

  **Deliberately not defended against: "Data Track Disguised as Audio".** CTRL is
  the TOC's only statement about track type; if it lies, no self-consistency
  check catches it. It surfaces at read time as sense key 5 / ASC 0x64, which the
  read engine already stops on. Recorded so nobody later "fixes" it with a
  heuristic that guesses track type from content — that is analysis (out of scope
  per CLAUDE.md), and it would be guessing besides.

  Two observations bounding what our parser can ever see: a drive may **remap
  non-standard point numbers into the legal range** (an NEC unit reporting `0xAB`
  as `0x6F`), so an in-range track number is not proof it was recorded that way;
  and non-standard points are **invisible to READ TOC entirely**, format 2
  included — reaching them needs subchannel reads of a later session's lead-in.

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

## Library API completion — PLAN ONLY, do not execute without direction

Raised 2026-07-25. Audit finding: the C API is **complete at the ABI level and
incomplete at the policy level** — `libaccudisc.so` and `accudisc.h` agree
exactly (65/65 symbols, no leakage, no reach-through from `cli/`), but the
*judgement* lives in `cli/main.c`: 1838 lines against a 1199-line header.

**The full design is `docs/reference/API_PLAN.md`** — scope, the guard/policy/
convention split, proposed signatures, the ABI questions gating the bindings,
the cdda2img communication ledger, and the effort estimate. Do not duplicate it
here; that document is the source of record.

Headline items, for grep:

- ~~`[P1]` Two silent-failure guards must move INTO the library~~ — **DONE
  2026-07-25, and the SpeedRead half is now being REMOVED entirely** by Keith's
  2026-07-26 ruling (see "Outstanding" task 1 below). The read-only-fd
  vendor-opcode diagnosis stays: it reports a condition rather than predicting a
  failure, and is correct under both the setcap and default regimes.
- `[P2]` Promote four acquisition strategies to public API — **and rewrite the
  CLI onto them in the same commit**, or the policy exists three times (CLI,
  Python, Rust) and drifts.
- `[P2]` Do NOT promote exit codes, `--progress-fd`, or `render_map` — those are
  process conventions, not library concerns. Document the mapping instead.
- ~~`[P3]` Bindings: settle the transparent-struct ABI hazard and the FFI
  callback design first.~~ **DONE 2026-07-26** (API_PLAN §7.1-7.3, commits
  `638db16` / `41591a9`): `uint32_t size` on `read_req`/`read_stats`,
  `ACCUDISC_ERR_ABI`, `accudisc_chunk` frozen, version single-sourced from the
  header. Note the free-to-break window is now narrower than "soname is `.so.0`"
  suggests — what actually made it free is that **nothing outside this repo
  links the library**, and that expires when the Python binding ships.
- ~~cdda2img is pinned to a snapshot fork of the binary for the duration.~~
  **Pin retired 2026-07-26** — both symlinks point at this tree's
  `build/cli/accudisc` again, so §8 is a *live* obligation: write the ledger row
  before the commit, not at the end of a phase.

## Outstanding — carried from 2026-07-26 (phase 3 landed; these did not)

### 0. RECOVERED sectors were returned WRONG, 9/9 — `[P1]`, cdda2img §89.5, NOT DIAGNOSED

**The most serious open claim against the read engine.** Keith ran five
whole-disc reads of Tracy at 40/32/24/8/4 (`--retries 3 --c2-retries 4
--verify 1 --overlap 2 --ladder 40,32,24,8,4 --driver plextor`, each with
`--pcm --c2f --sub raw --map-file`), diffed against a CTDB-repaired reference
that then verified 11/11 AccurateRip at confidence 200. Result: **nine corrupt
sectors, ten flagged map bytes, every flag state 4 = RECOVERED, and every flag
sits at exactly corrupt_LBA + 1** — never 0, −1, or +2. At documented indexing
recall is 0/9; shifted one, 9/9 with precision 9/10. All five `.c2f` files are
47,890,248 bytes of **zeros**, so C2 was silent throughout.

Mechanisms, none excluded yet:

- **H1 — storage off by one.** Byte for sector *N* written at *N*+1.
- **H2 — indexing correct, RECOVERED is simply false.** *N*+1 really was
  recovered and *N* was mis-delivered carrying no flag. Requires the recovered
  neighbour to land at +1 nine times running; cdda2img calls that a stretch.
- **H3 — units mismatch at the boundary.** We never apply the read offset
  (`cli/main.c:679`), so our PCM and map are both raw drive-space. If their
  artefact was offset-corrected for CTDB, "corrupt LBA" and "flagged LBA" are
  different coordinate systems. Does not obviously give a clean +1 nine times.

**AUDITED 2026-07-26 — no index error found anywhere in the recovery path.**
Every site checked and correct as written: the classify/publish loop
(`engine.c:554-556`, `idx = cur - req->lba`), the map file
(`cli/main.c:1376,1382,1396` — `ftruncate(count)` / `mmap(count)`, no header or
padding), `read_span`'s per-sector fallback (`engine.c:243-251`, `sec` and `cur`
both derived from the same `s`), `read_sector` (`engine.c:211-215`, a thin
wrapper with no arithmetic), both consensus write-backs (`engine.c:288, 321`),
the seam stash (`engine.c:623-627`) and the chunk advance (`engine.c:629`,
`lba += n`, so the extension is genuinely re-read rather than skipped).

**Still unexamined, and now the only place a whole-sector displacement could
originate:** `adsc_mmc_read_cd` and the SG_IO layer under it. Start there.

**Excluded by measurement, not by argument:**

- *Chunk geometry, the whole family.* A seam-misattribution mechanism was
  constructed and refuted: `sector_len` 2742 → `65535/2742` = 23, capped at 32,
  minus overlap 2 → **chunk 21**, and the corrupt LBAs scatter across residues
  0, 2, 9, 10, 13, 14, 16 mod 21. No C in 2..32 puts them at one residue. The
  adjacent pair 113056/113057 rules it out structurally too. Anything tied to
  chunk position — the seam check, `prev_ext`, the overlap extension — predicts a
  signature that is absent.
- *C2 lag.* 2 sample pairs = 8 bytes (`accudisc.h:987`) against a 2352-byte
  displacement, and it is **report-only by contract** (`accudisc.h:1004`), not an
  omission. Do not re-chase this.
- *Straddle H2* (one defect across the N/N+1 seam, C2 firing above and CIRC
  miscorrecting below) failed the in-sector-position test — 8 of 9 end 582–1776
  bytes short of the boundary. **But the test is weak** (cdda2img §92.2, conceded
  in §bk): CIRC interleave delocalizes a defect across ~100 frames, so byte
  offset is a poor proxy for physical position. Do not bank this negative. The
  sharp version needs the F1/F2 frame index, which neither side has.

**INDEPENDENT FINDING, outlives the whole +1 question — and the ROOT CAUSE of the
data loss, even though it does not explain the +1.**

*Corrected 2026-07-26 by cdda2img §93, which retracted their own §92.3; the
earlier "reproducible across a 10x speed range" reading in this entry was wrong
and is replaced.* The error is stable **per speed**, not per defect:

| LBA | 40x | 32x | 24x | 8x | 4x |
|---|---|---|---|---|---|
| 112612 / 112737 / 112751 | OK | OK | OK | OK | **WRONG** |
| 112765 | OK | OK | OK | **WRONG** | **WRONG** |
| 113043 | **WRONG** | **WRONG** | OK | OK | OK |
| 113056 | OK | OK | **WRONG** | OK | OK |
| 113057 | OK | OK | OK | **WRONG** | OK |

**No sector is wrong at all five speeds**, the failing speeds differ per sector,
and "slower is better" is false at sector granularity. (Caveat: `disc40.pcm` and
`disc32.pcm` are byte-identical disc-wide, so those two requests likely landed on
one rung — the quantised-rung problem. The only clean two-speed case is 112765.)
A plain per-sector **majority vote across the five captures reconstructs all seven
sectors correctly** — no parity, no CTDB, no AccurateRip.

**So the pit is readable and this drive can get the bits.** The failure is
entirely recovery *policy*. `consensus()` is blind not because the damage is
irreducible but because **it resamples the same deterministic function**: at a
fixed speed the drive returns the same wrong bytes, so agreement is guaranteed and
carries no information.

**Why the `--ladder` did not save it (answered from source, §bl.1).** Speed varies
only inside the per-sector rereads — `ladder_speed` is called at exactly two
sites, `engine.c:280` (`c2_rescue`) and `engine.c:311` (`consensus`). Every
whole-span read is preceded by `ladder_restore` (`engine.c:461` primary,
`engine.c:520` every verify pass), which sets `req->speed_x`
(`engine.c:191-197`). For a silently-wrong sector: no C2 fires so `c2_rescue` is
skipped; the verify pass re-reads at the *same* speed, gets identical bytes,
`diff == 0`, and takes the "confirmed" `continue` at `engine.c:531-532`.
**`consensus()` is never called — the ladder is unreachable.** The one mechanism
that would catch this is gated behind the symptom it removes.

**The comment at `engine.c:510-518` is itself a defect.** It claims "speed
diversity against persistent same-speed misreads (RECOVERY_STRATEGY R6) comes from
the consensus/rescue rereads". False for this failure class — those rereads are
unreachable when the misread is silent; the claim holds only for sectors that
already announced themselves. Its *other* claim, that whole-range speed-diverse
sweeps are the caller's layer, is **confirmed** by the 7/7 majority vote. The
split of responsibilities is right; the coverage claim inside it is wrong, and it
currently tells the next reader the hole is covered.

This is RECOVERY.md's invariant demonstrated: relative checks must never outrank
absolute gates, and the absolute gate is the caller's layer.

**`ACCUDISC_MAP_OK` is the worse lie, ahead of `RECOVERED`.** A sector confirmed by
N same-speed passes is marked identically to one that was right first time — every
one of the seven corrupt sectors is `OK` in some capture. Fix candidates, in
order: correct the `510-518` comment; make `MAP_OK` distinguish "confirmed by
same-speed passes" from "clean"; consider **one final verify pass at a different
ladder rung** (per-chunk speed switching is rejected by the recalibration-thrash
objection in that same comment, which is real and presumably measured).

Cross-check from their side: `sector-hammer` (repeat at one setting, max retries)
scored 2/20 while the speed-varying ladder scored 19/20. §93.2 supplies the
mechanism that ranking was missing — repetition at a fixed speed samples a
constant.

**Candidate mechanism for H2, with a known hole.** Sector N holds a *stable*
concealed error → the verify pass sees `diff == 0`, takes the "confirmed"
`continue` at `engine.c:531-532`, and writes `MAP_OK`. Sector N+1 holds a
*marginal* defect from the same damage region → unstable → `consensus()` fires →
`RECOVERED`. One damage region, two sides of the stability threshold, no
off-by-one required. **Hole: it does not explain why the stable sector is always
the lower-numbered one** (9/9 at +1, never −1). CIRC delay-line directionality is
the place to look; it is not an explanation we have. H1-local stays open beside
it.

**The second-drive test is SETTLED and withdrawn** — it was going to decide
whether the pit is readable, and cdda2img's own five captures already answer it:
this drive reads 113043 correctly at 24x, 8x and 4x. A second drive would only
confirm generality. Not worth the time.

**`MAP_OK` on the corrupt sector CONFIRMED 9/9** (cdda2img §94.1): every one of
the nine reads `0x01` at the corrupt LBA and RECOVERED at LBA+1. Not one carries
C2, HARD or SUSPECT. The actual errors are all wearing the state that announces
nothing.

**The +1 adjacency is not small-numbers coincidence** (§94.2): 10 RECOVERED bytes
in 814,460 map bytes — base rate 1.2e-5 — with nine of them at exactly corrupt+1,
and per-capture counts tracking (1/1 at 40x and 32x, 2/2 at 8x, 4/4 at 4x, 1/2 at
24x). **Acceptance test for any eventual explanation: one event, one sector apart,
direction never varies.**

### 0b. Recovery rereads use the read mode c2lag.c measured as DIFFERENT on this drive — `[P2]`

Found reading two modules against each other; independent of the +1 and of the
speed result. `consensus()` and `c2_rescue()` both do `cache_defeat(r, lba)` then
`read_sector(r, lba, ...)` (`engine.c:281-282`, `312-313`) — an **isolated
single-sector read immediately after a 5000-sector seek**. Meanwhile
`src/drive/c2lag.c:21-27` states, from live measurement on this same PX-716A:

> Rereads are STREAMING WINDOW reads, not isolated single-sector reads: marginal
> defects fire C2 while the drive streams and decode cleanly on a careful
> post-seek single-sector read (verified live on the PX-716A — **a streaming pass
> flagged ~40 sectors where per-sector rereads of the same LBAs flagged zero**).

c2lag uses `C2LAG_RUNUP` 16 sectors of lead-in specifically to be streaming before
it crosses the damage. So one module designed around a measured mode difference
and the recovery path uses the mode it designed around: **delivery is a streaming
span read, recovery is an isolated post-seek single-sector read**, and we have our
own live evidence on this drive that the two disagree.

This weakens `consensus()` a second way, on top of the speed issue: it is not only
resampling the same speed, it is resampling in the *quieter mode* — the one that
tends not to reproduce the defect. The condition it adjudicates ("did the
streaming read get this sector right?") is not the condition it measures. Fix
direction: give the recovery rereads a streaming run-up, as c2lag already does.

**Still unexplained, and do not let any of the above close it:** the +1 asymmetry.
Nine of nine flags at N+1, never N−1. Neither the speed-stability result nor 0b
touches the direction. One speculative lead, recorded as speculation only: CIRC
delay lines spread symbol errors along the direction of travel, so the leading
sector of an encounter may stay within correction capacity and be silently
miscorrected while the following one exceeds it and announces itself. **No
measurement behind that** — testing it needs the F1/F2 frame coordinate, which
neither side has.

Discriminator requested in §bh.2, costs nothing: a uniform +1 shift never writes
index 0, so **byte 0 of their five existing map files** decides it — `0x01` (OK)
excludes whole-array H1, `0x00` (PENDING) on a capture that read sector 0 fine
confirms it. Byte `count-1` is the same test at the far end.

Independent of the +1, the semantic defect is real and ours: `consensus()`
(`engine.c:317-323`) returns 1 on the first byte-for-byte agreement between any
two reads. **That is a stability test, not a correctness test** — two reads can
agree on the same wrong bytes.

**Scope of the doc fix, corrected 2026-07-26 — it is NARROWER than first
recorded.** The block contract at `accudisc.h:1061-1065` already says the right
thing and always did: "Every state below is a RELATIVE claim … **A drive that
misreads deterministically passes every relative check**; absolute gates
(AccurateRip, CTDB) are the calling application's job and always outrank anything
recorded here." That is cdda2img's entire finding, written down before they
measured it. So the contract does **not** need rewriting — only the per-state
one-liner for `ACCUDISC_MAP_RECOVERED` ("problem seen, clean/agreeing copy won"),
which reads as fidelity and contradicts the block six lines above it.

The real gap was never documentation: `consensus()` implements precisely the thing
the block comment tells consumers not to trust, and we labelled its output as
though the comment did not exist.

Next experiment (needs the drive, Tracy loaded, Keith's call): re-read one of the
nine LBAs and compare delivered bytes against the CTDB reference. Clean ⇒ the
consensus is unstable rather than wrong, a different defect with a different fix.

**Also fixed en route:** §89.6 asserted our span-finder shares
`MAP_NEEDS_RECOVERY = {0x2,0x3,0x5}` (0x4 excluded). **We have no span-finder** —
`recovery_bench.py` is in their tree. Our only map consumer is the CLI glyph
table (`cli/main.c:1049-1054`), where RECOVERED already ranks above OK and
renders `r`. Corrected in §bh.4.

Severity is **not comparable across states** and this should be documented:
C2 = `log2(fired bits)` (`engine.c:50`), SUSPECT = `log2(differing bytes)`
(`engine.c:62`), RECOVERED = raw **attempt count** (`engine.c:57`). That last one
explains §89.5's "unexplained" asymmetry — 40x sev 1 vs 32x sev 3 on the same LBA
with byte-identical PCM is expected, not anomalous.

### 1. Remove all SpeedRead guards — `[P1]`, USER-DIRECTED, NOT STARTED

**Keith's ruling 2026-07-26: "Remove all guards for SpeedRead. CDDA is
completely unaffected by it. Those guards have no reason to exist."**
Manual-backed — `private/drives/Plextor/Plextor-716.pdf` **p.15** publishes three
ceilings for this model by media class: DATA (CD-ROM/CD-RW/CD-R) 48×, **CD-DA
and CD-R audio 40×**, CD-RW audio 32×. The 48× the uncap exposes into page 2A is
a *data-media* figure, so the uncap cannot make an audio disc spin above 40× and
the throughput half of the hypothesis is closed.

Scope agreed before stopping: remove **enforcement only**. Keep
`accudisc_speed_uncap_set/get/push/pop`, the `speed-uncap` subcommand, the
`--uncap` flag and the plextor driver — a queryable state is not a guard, and the
standing goal is 100 % Plextor feature coverage.

Inventory, already built — every site:

| remove | where |
|---|---|
| the refusal block + `adsc_uncap_authoritative` call | `src/read/engine.c:370-391` |
| `allow_unsafe` field | `include/accudisc/accudisc.h:1189` (+ the §7.1 comment at :1109) |
| `ACCUDISC_ERR_UNSAFE_COMBINATION` | `include/accudisc/accudisc.h:68` |
| its `strerror` case | `src/device.c:74` |
| the `--uncap` + `--sub` interlock | `cli/main.c` (~1502 pre-edit numbering) |
| the `LIKELY_ON` pre-read warning | `cli/main.c:1444-1457` |
| guard-specific assertions | `tests/test_uncap.c:96, 226-245` |
| `allow_unsafe` references | `tests/test_abi.c:85, 109-110` |

Notes for whoever does it:

- **`allow_unsafe` sits in existing padding**, so removing it leaves
  `sizeof(accudisc_read_req) == 56` unchanged — but `tests/test_abi.c`'s
  `_Static_assert` and the short-struct case both name that field, so they need
  re-pointing at another field rather than deleting.
- It is still a **public API removal** (a field and an error code), so bump
  `ACCUDISC_VERSION_MINOR` to 3. CMake derives the `.so` version from the header.
- **cdda2img is on our live tree now** — write the API_PLAN §8 ledger row
  *before* the commit, per §8's post-relink discipline, not after.
- `drivers/plextor/FEATURES.md:18` still documents the refuted *mechanism*. Fix
  it with this change. **Keep the observation** — a real ABBA A/B, 40.6 % Q with
  SpeedRead on vs 99.2 % off, 0 % across the 10–60 % radius band — recorded as
  unexplained data with its confounds named (n=1 per arm, damaged media, taken
  before drive contention was known to produce the same signature). Removing a
  guard is not a reason to delete a measurement.

### 2. `setcap` the INSTALLED binary, not the build-tree one — `[P1]`

`CMakeLists.txt:34-38` already documents this and we have not been doing it. The
capability binds to the **inode**, so every rebuild that relinks the CLI drops
it. Three independent reasons now, the third new:

1. It interrupts Keith on every rebuild.
2. It silently disarms the vendor path mid-session (four occurrences).
3. **Since the 2026-07-26 relink, cdda2img executes the same inode**, so our
   rebuild drops the capability from *their* binary too.

### 3. Audit `if (!quiet)` around anything that is not progress — `[P2]`

cdda2img passes `-q` on all three read paths (`accudisc_reader.py:335`, `:439`,
`tools/recovery_bench.py:777`), and the pre-read uncap warning was gated on
`!ctx.quiet` — so the flag that makes a caller a *machine* consumer suppressed
the notice written for machine consumers. The specific warning goes away with
task 1; **the pattern is the task**. Quiet means "no human is watching", which is
the strongest claim on being told something is wrong, not the weakest. Sweep
`cli/main.c` for any other data-integrity notice behind a verbosity flag, and
prefer a machine-readable token over stderr prose — stderr wording is explicitly
not a stable interface (`cli-machine-interface.md`).

### 4. The phantom 48× ladder rung — `[P2]`, live on both sides

API_PLAN §9.3. With the uncap on, page 2A genuinely reports 48, `accudisc speeds`
genuinely returns `req=48 page2a=48`, and a ladder admitting rungs on the strict
`req == page2a` rule carries a 48 label over a measured ~21×. The premise that
retired this ("we cannot set the uncap") died when the capability landed on the
binary. Evidence a monotonicity rule would need: a `speeds` table at three passes
with the uncap on — which the SpeedRead discriminator would produce as a
by-product if it ever runs.

**Mechanism, from Keith 2026-07-26 (cdda2img §96.2) — this is UNDETECTABLE from
the bus, by construction.** Keith, on the drive he owns: *"the drive governor will
cap the speed at 40x for a healthy disc, or less for an unhealthy one. Nothing
anyone does in software alone can ever force a CDDA to read at 48x… That is a
function of the drive which is not exposed via MMC/SG."* So with the uncap on,
`req=48 page2a=48` has **both operands derived from the same advertised ceiling**,
agreeing with each other and both wrong about what the drive will do. **`req ==
page2a` cross-checks the quantiser, never the ceiling**, and no MMC field exposes
the gap.

**Our side needs no change — the field and the rule already ship**, and this is
what to tell anyone who asks: `accudisc_speed_rung.measured_cx`
(`accudisc.h:1043`, a *timed streaming read*, not a report) is emitted per rung by
`cli/main.c:672`. And `accudisc.h:1036-1038` already states the rule: "rungs whose
`measured_cx` collapse to the same value are indistinguishable on this rig (bus or
firmware limited) and one of them suffices in a recovery ladder." A 48 rung
measuring ~21× is exactly that case.

**Confound to carry into any such rule** (`accudisc.h:1034-1036`): `measured_cx` is
the achieved rate **at this radius**, and CAV drives read outer tracks faster, so
rungs probed at different LBAs are not comparable — geometry will manufacture
"violations". Probe mid-disc. This is the same confound behind the `speeds`
min/max item (inner/middle/outer) under "Probes / diagnostics".

### 5. Stock-ceiling table — PARTLY closed. The A-vs-B discriminator is REOPENED `[P3]`

**REOPENED 2026-07-26 (Keith caught it; cdda2img §95 retracts their §91.1).** This
entry briefly recorded mechanism B as refuted. **It is not**, and the CD-RW audio
disc is still the only discriminator. Do not close this again without that disc.

*The invalid step, kept as a worked example of the house failure mode:* all the
measurements were taken with the uncap **OFF**, and a conclusion was drawn about
what the uncap **DOES**. B is defined (§bg.2) as "the uncap lifts the media ceiling
by one class" — a claim about the *transformation*, carrying no commitment about
the uncap-off reading. B is perfectly consistent with "stock 40 whatever is loaded;
uncapped, media ceiling + one class". A prediction B never made was refuted.
*Accepted uncritically on this side too* (§bj.1 called their reasoning right), so
the fault is not theirs alone: a well-formed measurement answering a different
question than the one asked.

**SURVIVES — genuinely settled, all uncap-OFF questions:**

- `max_x` is media-invariant **with the uncap off**: 40 across audio (Tracy), data
  (closed CD-R, Taiyo Yuden, profile 0x0009) and **empty tray**.
- `stock_ceilings[]` records the **stock** ceiling, which *is* the uncap-off value,
  so **one row per model is correct and `(model)` stays the key**. No code change.
- **§bg.3's false-ON is empty.** It needs a stock drive (uncap off); data disc,
  uncap off, `max_x` 40 → `OFF`, correct.

**STILL OPEN — A vs B, and the hazard is a silent false negative:** with the uncap
**on**, Keith measured the data disc going 40 → 48, so the uncap does move `max_x`.
But data is already the top class, so A and B agree on every medium touched so far:

| medium, uncap ON | A: reports data ceiling | B: media ceiling + one class | observed |
|---|---|---|---|
| data disc | 48 | 48 (already top) | **48** |
| audio disc | 48 | 40 → 48 | **48** (§au.1) |
| **CD-RW audio** | **48** | **32 → 40** | **NOT MEASURED** |

Under B that disc yields `max_x` 40, `40 > 40` is false at `uncap.c:83`'s strict
`>`, and `adsc_uncap_classify` returns `ACCUDISC_UNCAP_OFF` **while the uncap is
on** — the silent false negative on the safety value. Live and unmeasured.

Task 5b below is independent of all of this and unaffected.

**Drive state changed (§95.4):** Keith has left SpeedRead **ON** (page 2A now
`48x (8467 kB/s)`, curve `20.0x..48.0x`) with the Taiyo Yuden CD-R reloaded. Any
`speeds` table taken from now on is an uncapped reading.

<details><summary>Original open question (superseded)</summary>

cdda2img §88.2, open question, **not a known defect**. `stock_ceilings[]`
(`src/drive/uncap.c:43-48`) has one row, PX-716A → 40, derived from observed
ON/OFF transitions on audio media. The manual publishes three ceilings for the
same model by media class. If page-2A `max_x` tracks the loaded media class, a
CD-RW audio disc would report 32 against a stock 40, and `adsc_uncap_classify`
must decide whether 32 < 40 means OFF or means "a class this table does not
model". If `max_x` is media-invariant the question evaporates.

**Sharpened 2026-07-26 (reply §bg).** The low reading they framed it around is
the *benign* branch: 32 < 40 resolves to OFF and the uncap really is off. Two
worse branches, both untested, both invisible so far because every page-2A
reading either side has taken was with an **audio disc loaded**:

- **False negative.** One observation (40 off / 48 on, audio media) fits two
  mechanisms. **A:** the uncap reports the *data* ceiling regardless of media →
  CD-RW audio goes to 48, `48 > 40`, `LIKELY_ON`, row is fine. **B:** the uncap
  lifts the media ceiling by one class → CD-RW audio goes to 40, and `uncap.c:83`
  uses strict `>`, so equality returns `ACCUDISC_UNCAP_OFF` **while the uncap is
  on**. Our data cannot separate A from B.
- **False positive, on a stock drive.** Uncap OFF + a **data** disc: if `max_x`
  is media-derived it reads the data figure, `> 40`, → `LIKELY_ON` on a drive
  nobody touched. cdda2img refuses on `LIKELY_ON`.

Discriminator asked for in §bg.4 — page-2A `max_x`, uncap off, under three tray
states: audio / data / **empty** (no media class to derive from, so it is
diagnostic alone). Same number under all three ⇒ media-invariant, close this.
Their CD-RW audio disc then settles A vs B. Not run by us: it needs disc swaps
and Tracy is loaded for their §9.3 ladder work.

**Only matters if the classifier survives task 1** — if the whole inference path
goes with the guards, close this as moot. Note the *reporting* path plausibly
survives even so, since task 1 removes enforcement only.

</details>

### 5c. The "--driver auto" advice contradicts itself when a driver WAS named — `[P2]`

cdda2img §91.2, reproduced on an empty tray with `--driver plextor` passed
explicitly:

```
accudisc: driver plextor: selftest failed on PLEXTOR DVDR   PX-716A — staying on generic MMC
accudisc: using generic MMC
accudisc: read-speed uncap unsupported via generic MMC — a vendor driver is required (--driver auto)
```

Line 1 says the driver was found, attempted, and failed its selftest. Line 3 tells
the user to pass `--driver auto` — a flag they have already superseded by naming
the driver, and which cannot help, because the driver was never missing. The
advice is hard-coded and does not distinguish **"no driver permitted"** (where
`--driver auto` is the right fix) from **"driver attempted and failed"** (where it
is noise). Four identical sites: `cli/main.c:216, 841, 852, 1467`.

Fix: make the suffix conditional on whether a driver was permitted/attempted this
invocation. When one was attempted and failed, say so and name the reason instead
of suggesting a flag.

### 5d. `speed-uncap` needs a disc; `speed` does not — undocumented envelope split — `[P3]`

Also §91.2. The Plextor selftest issues a real vendor opcode, which requires a
medium, so **`speed-uncap` — report *or* set — is unavailable on an empty drive**.
`accudisc speed` reads mode page 2A directly, needs no vendor driver, and answered
the same question (`page2A max 40x`) with an empty tray. The two subcommands
report overlapping numbers with **different availability envelopes** and nothing
says so. Document in `accudisc.1` and the header; note that anything assuming it
can query uncap state before a disc is loaded cannot.

### 5b. `ACCUDISC_UNCAP_OFF` is labelled authoritative but source 3 infers it — `[P3]`

Found while writing §bg. `adsc_uncap_classify` (`src/drive/uncap.c:83`) returns
`LIKELY_ON` **or** `OFF` from the same speed comparison, but only one branch
carries the hedge: `accudisc.h:319` documents `ACCUDISC_UNCAP_OFF = 0` as
*authoritative*, and sources 1–2 do produce it authoritatively. A consumer cannot
tell a driver-confirmed off from a speed-inferred off — they collapse to one
value, and there is no `LIKELY_OFF`. Independent of the media-class question
above; the asymmetry exists even if `max_x` turns out media-invariant.

Cheapest fix is a fourth enum value; that is an ABI addition, so it wants the
same treatment as any other (existing consumers must keep compiling, and
`LIKELY_OFF` must not silently read as `OFF = 0`). cdda2img told not to build on
the distinction yet.

### 6. Phase 4 — the Python binding — `[P2]`

API_PLAN §7.2/§7.3 already fixed its shape: a **streaming API over the sink**,
not a subprocess replacement. Copy by default; zero-copy behind an explicit
opt-in with the view **released on return**, because a retained `memoryview`
reads freed memory as plausible PCM. `ERR_NOTFOUND` is absence, not failure;
`accudisc_write`'s caveat is a **positive** return. The sink fires only
~13–15 k times per whole disc, so per-call FFI cost is a non-issue — the ~1 GB
copy is the real cost.

### 7. cdda2img §88 — ANSWERED 2026-07-26 (§bg)

Premise conceded (we quoted the manual's three ceilings in our own §au.1 and
then built a one-row table), the question sharpened into the A/B discriminator
and the false-positive direction, and the three-tray-state measurement requested.
**Waiting on them** for the readings — see task 5. Outbox
`cdda2img/private/AccuDisc.md`; last sent §bg.

### 8. Their binary depends on our `build/` existing — `[P3]`, note only

The CLI **dynamically** links `libaccudisc.so.0` (`cli/CMakeLists.txt:3` links
the SHARED target) with `RUNPATH` pointing into `build/src`. Since the relink,
`rm -rf build` on our side leaves cdda2img's `accudisc` unable to start, and a
rebuild has a window where the `.so` is being replaced. Their end-of-run engine
re-hash catches the mixed-build case; nothing catches the missing-`.so` case.
Installing properly (task 2) would fix both.

## Deferred (explicitly, by user decision)

- Python / Rust bindings (generated against `include/accudisc/*.h` only).
  **Reopened 2026-07-25** — see "Library API completion" above for the
  prerequisites.
- Man page (must mirror `docs/reference/ATTRIBUTION.md`).
  **No longer deferred — first cut written 2026-07-26:** `docs/man/accudisc.1`
  (CLI) and `docs/man/accudisc.8` (library/API). Both are untracked and neither
  is installed by CMake yet. **Outstanding against this entry:** the pages do not
  yet mirror `ATTRIBUTION.md` — they credit nothing beyond the MIT statement, so
  the reference-source credits still need adding.
- Write / burn (DAO) path — paused; do not start without direction.
  **This line is stale as written.** The DAO path shipped and is
  hardware-verified (`src/write/`, `RECORDING_PLAN.md` §9 phases 1–2 and §11.7/
  §11.8). Left in place rather than deleted because the *decision* it records —
  do not extend the write path without direction — still stands, and RECORDING_
  PLAN §9 phases 3–5 (`--sub` passthrough, vendor write features) are genuinely
  not started.
