# AccuDisc — deferred work

Ideas parked for a later session. Not scheduled; not commitments. Recovery
methods are considered complete (see `docs/reference/RECOVERY.md`); this is
everything else worth remembering.

Completed work is kept as one- or two-line summaries with any durable lesson
attached; the blow-by-blow reasoning that produced it is not retained.

## READ SPEED — ONE SESSION, THEN PERMANENTLY ABANDONED (Keith, 2026-08-08)

> *"We will set aside a session, not today, where the only topic we discuss is
> read speed. At the end of that session, anything not completed will be
> permanently abandoned. So that means no hours-long tests."*
> — and, on the general subject: *"We need to move past this, quickly."*

**The rule is a guillotine, so what goes into that session matters.** The
discriminator is **does this need the drive?** — not whether the word "speed"
appears in the title. Filing desk work into the session kills fixes that never
needed hardware; filing hardware work outside it re-opens the open-ended
investigation the ruling closed.

**DRIVE-BOUND — this is the session, and items 3–5 cannot be advanced on the
hardware we own, so expect them to be abandoned by default:**

1. `--speed 16` silently honoured as 8×, unreported — "Known bugs" below, `[P1]`.
   **Less blocked than that entry says**; see the note there.
2. The read-engine throughput cap (~5× vs 12–19× raw) — "Speed control" below.
   The one most likely to be *felt*: every whole-disc read goes through it.
3. Task 5's A-vs-B uncap discriminator — needs a **CD-RW audio disc we do not
   have**.
4. Phase 3 ranged SET STREAMING — needs **other drives**; this one cannot
   advance it.
5. The `device.c` latch bug — needs a drive that genuinely lacks 0xB6, i.e. not
   this one. Falsifiable nowhere on this rig.

**DESK WORK — no drive, NOT session-bound, and must not be abandoned with it:**

- Task 1, remove the SpeedRead guards. Source-only, inventory complete. Gated on
  one ABI ruling from Keith, stated in that entry.
- Task 5c, the self-contradicting `--driver auto` advice. Pure logic.
- Task 5d, document the `speed` / `speed-uncap` availability split.
- Task 5b, the `LIKELY_OFF` asymmetry. An ABI decision, possibly moot after
  task 1.
- Task 4b's residual: the cross-rung timed-window **length** term is still
  undocumented (the *radius* term was documented 2026-07-28).

**CLOSED — do not re-open, and do not spend session time re-deriving:** the
`speeds` min/max sweep (hardware-validated 2026-07-28), the
`probe_speed_ladder` binding (`ea11d56`), task 4a's phantom 48× rung (an answer
to hand out, not work), and 4b's radius half.

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
**Read-speed session, item 5 — and unfalsifiable here**: the bug only presents
on a drive without working 0xB6, so no test on the PX-716A can reach it. The
fix is three lines and obvious from the sense-key list; what cannot be done is
*confirming* it. Decide in that session whether to take it unverified or drop
it.

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

**Read-speed session, item 4 — and not advanceable on this rig.** The one
experiment that would move it is "try another drive", and we have one drive. So
the session cannot resolve this, and under its abandonment rule it will lapse by
default. That is an acceptable outcome for a deferred feature; what would not be
acceptable is letting the lapse be recorded as *"impossible"* rather than
*"unknown, never retested"* — the distinction this entry exists to protect.

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

  **Re-assessed 2026-08-09 — this is LESS BLOCKED than the paragraph above
  says, and the instrument already exists.** The `--sweep` acceptance run of
  2026-07-28 (table under "Probes / diagnostics") contains this exact defect,
  measured: the `req=16` rung came back `page2a=8, measured=8.01, spread 0.00`
  — asked 16, honoured 8, and the timing said so unambiguously, with the flat
  spread independently predicting a CLV-clamped rung. So the "thresholds must
  be validated on the drive" objection is already satisfied for at least one
  point of the ladder by a run that has been accepted.

  What remains is therefore a **cost decision, not a measurement design**:
  reporting the honoured rate on `speed X` / `read --speed X` means a timed
  read before every such command. Options: do it only when asked
  (`--verify-speed`), or report the *quantised* prediction from the measured
  {4,8,24,32} ladder and say it is a prediction. **Read-speed session, item 1**
  — and the highest-value item in it, since it is the only one a user meets.
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

**Read-speed session, item 2 — and the one most likely to be felt as a rough
edge**, because every whole-disc read goes through it and the gap is a factor
of 2–4 in wall-clock time on a *clean* disc, where no recovery work is being
done to justify it. It is also the only drive-bound speed item that is
plausibly fixable in a short session: the four suspects are separable by
instrumenting the per-chunk loop rather than by long comparative rips, which
the "no hours-long tests" rule forbids anyway. Isolate the 70 ms first — a
timing breakdown of one chunk answers which suspect it is, and costs one read
of a few hundred sectors.

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

- ~~**`speeds`: report min/avg/max as well as the current single figure.**~~ —
  **CODE DONE 2026-07-28, NOT YET HARDWARE-VALIDATED.** `--sweep` /
  `points=3` landed: each rung timed at inner, middle and outer, `min=`/`max=`
  appended, `measured=` kept as the middle band. **The acceptance test has not
  been run** — see "What is still owed" at the end of this item. Three
  numbers appearing is not evidence the instrument measures what it claims.

  Decisions taken, with the reasoning, since each closed off an alternative:

  - **`measured=` is the MIDDLE band, not the mean of three.** The token is
    declared stable in `cli-machine-interface.md`; a mean would have kept
    every parser working while changing the quantity underneath it — the
    referent-drift class, arriving through the exact door the "do not rename
    to `avg=`" trap was built to guard. Consequently there is no `avg=` key
    at all, and the item's own title is now a slight misnomer.
  - **No `size` field on `accudisc_speed_rung`; the break was taken instead.**
    6 → 10 bytes (measured, both compiled). A per-element `size` on an OUT
    *array* is structurally wrong under API_PLAN §7.1's own OUT rule — it
    means trusting N independent caller claims — and §7.1 explicitly warns
    that widening the guarded set "turns a decidable problem into an
    unbounded one". `points` is a plain parameter, so this is one deliberate
    break rather than two. **The window is now spent**; see task 6.
  - **Windows are laid out by `adsc_speeds_layout` (`src/internal.h`), which
    is pure and tested device-free** (`tests/test_speeds.c`, 33rd test).
    Extracted precisely because the failure is silent: a guard that did not
    scale with `points` would overlap the bands, the re-reads would be
    cache-served, and every rung would report a FLAT gradient — which is
    also the signature of a genuinely CLV-clamped rung. The bug would have
    presented as the instrument's own diagnostic firing correctly. The test
    was verified by injecting that exact bug (guard left at `count/ncand`):
    69 failures, including the named case.
  - **Cross-rung radius bias is now documented rather than fixed** (item 4b's
    option (a), header + man + machine interface). Reversing the rung order
    between bands would have cancelled the bias in the *mean* — but we do not
    report a mean, so it would have bought nothing.

  **ALL THREE CLAUSES NOW IMPLEMENTED (2026-07-28).** Keith restated the
  full specification — "read the page 2a settings on the ladder, look at the
  actual throughput at all 3 sections of the disc, then decide which of the
  page 2a readings represent real, actual speeds that are achievable under
  the many conditions that influence the governor's choice, discard the
  readings that are obvious duplicates/unachievable, then use the remaining
  settings as the *real* speed ladder" — and the deciding half landed too.

  **Where the verdict lives, decided by Keith:** the library decides and
  reports; the read engine does **not** apply it. `accudisc_speed_rung`
  gains `verdict` + `equiv_x` (ADMITTED / DUPLICATE / QUANTIZED / UNKNOWN),
  the CLI prints `verdict=` per rung plus a `ladder admitted=` line, and
  `accudisc_read_req.speed_ladder` is never rewritten. Rationale: putting it
  in the library is what stops every consumer reimplementing the rule —
  `RECOVERY.md:196`'s `drive_speed.admitted_ladder` on cdda2img's side is
  now redundant, which is the reference-consumer principle working.

  **How the rule survives the cross-rung radius term**, which was the real
  design problem: it never compares raw rates against a modelled curve. A
  rung's own `max_cx - min_cx` is the speed change across the whole span,
  and adjacent rungs sit exactly one window apart, so that spread *measures*
  the per-neighbour radius effect on this disc — self-calibrating. A gap
  must beat `K` radius-steps to count. **This is what min/max are for
  beyond reporting, and why clause 1 had to land first.**

  Three findings worth keeping, all from making the test fail:
  - **`K` does not manufacture duplicates.** The obvious reading is wrong.
    `req=48` measured *slower* than `req=40` (23.01 vs 23.73) — the radius
    bias made directly visible — so it fails "faster than the rung below"
    outright and never consults the margin. `K`'s only load-bearing job is
    the other direction: not collapsing genuinely distinct rungs.
  - **`K` is therefore uncritical, and that is asserted, not asserted-about.**
    The binding constraint is the closest genuine pair (40-vs-32: 4.18x gap
    against a 0.72x radius-step), so any `K` below 5.8 keeps it. The test
    sweeps `K` 1..5 for an identical ladder **and requires `K=6` to differ**,
    so the sweep cannot pass by the rule ignoring `K`.
  - **A first version of the stability check was wrong.** It widened the
    intervals rather than varying `K` — which fabricates a disc with twice
    the gradient, where adjacent rungs genuinely *are* less distinguishable.
    Correct physics, wrong test. `K` was made a parameter of the internal
    entry point precisely so the sweep tests the threshold itself.

  `points == 1` yields UNKNOWN for every rung and no ladder line, rather
  than judging on point samples — the refuse-don't-narrow rule.

  Verified against the recorded PX-716A run as a test vector (not invented
  numbers, since the difficulty is exactly the radius term): admitted ladder
  **`40,32,24,8,4`**, with 48 duplicate:40 and 16 quantized:8.
  Measure each rung at
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

    **Status 2026-07-28: SPENT.** The break was taken; the struct is 10 bytes
    and carries no size field. Nothing outside this repo linked the library at
    the time, and `accudisc_probe_speed_ladder` was still unbound. It is now
    free to bind — and binding it is what makes the next growth of this struct
    a real version break rather than a free one.

  **Implementation notes:** cost is 3x the timed windows per rung (default
  ladder is up to 8 rungs after page-2A filtering). Each of the three locations
  needs its *own* cache-fresh window per rung, so the window allocator must not
  collide across rungs *or* radii — the existing guarantee is only per-rung.
  Prefer keeping the current single-location behaviour as the default and
  putting the three-point sweep behind an opt-in flag, so the default output and
  probe time are unchanged; `--start` then keeps its present meaning instead of
  being silently reinterpreted as a centre point.

  **As built** (all of the above honoured): opt-in `--sweep`, default path and
  its probe time untouched, `--start` unchanged. Windows are indexed
  `band*ncand + rung` over the span, so all `points*ncand` are disjoint across
  rungs *and* radii; the fit guard scales with `points` and refuses rather
  than overlapping. `--sweep` widens the default span from the middle half to
  the whole disc, since three bands of the middle half would be three samples
  of the same neighbourhood rather than inner/middle/outer.

  **HARDWARE-VALIDATED 2026-07-28 — the acceptance test PASSED.** PX-716A
  rev 1.11, Tracy (11 audio tracks, leadout 162892, clean), under
  `flock /var/tmp/sr0.lock`. Confounds recorded: governor NOT pinned below
  max (page 2A `max 48x / current 48x` at init — the
  [[plextor-speedread-subq]] triage signal is `current < max`, absent here);
  SpeedRead uncap found ON and left ON; single-agent, lock held; same disc
  throughout.

  The CLV/CAV discriminator came out exactly as the reasoning above
  required, and it is the whole point of the item:

  | req | page2a | measured | min | max | spread |
  |---|---|---|---|---|---|
  | 48 | 48 | 23.01 | 17.15 | 27.67 | **10.52** |
  | 40 | 40 | 23.73 | 18.08 | 28.28 | **10.20** |
  | 32 | 32 | 19.55 | 15.14 | 23.09 | **7.95** |
  | 24 | 24 | 14.98 | 11.75 | 17.57 | **5.82** |
  | 16 | **8** | 8.01 | 8.01 | 8.01 | **0.00** |
  | 8 | 8 | 8.01 | 8.00 | 8.01 | **0.01** |
  | 4 | 4 | 4.01 | 4.01 | 4.01 | **0.00** |

  The CAV rungs spread by 5.8–10.5x; the CLV-clamped rungs are flat to
  within 0.01x. **A flat result was the ambiguous one** — it is equally the
  signature of an overlapping-window bug — so the fact that flatness appears
  *only* on the rungs independently predicted to be clamped, and never on
  the CAV rungs, is what makes this a pass rather than three plausible
  numbers. `req=16` snapping to `page2a=8` is the drive quantizing, which is
  exactly what the `page2a` token exists to expose.

  **The uncap does not affect it on this disc, measured A/B/A rather than
  assumed** (Keith's prediction, confirmed). Matched ladder
  `40,32,24,16,8,4` so `ncand` — and therefore the window layout — is
  identical across states; without pinning it the uncap changes the
  page-2A max, filters the 48 rung out, and silently re-lays every window.
  Largest ON-vs-ON difference (run-to-run noise) **0.40x**; largest
  OFF-vs-mean-ON difference (the effect) **0.21x**; ratio **0.53**, i.e.
  the effect is half the noise. **The robust reason is that nothing in this
  run came near either ceiling** — the fastest single measurement anywhere
  was 28.28x, so 40x versus 48x cannot bind whatever the limiter is. That
  statement does not depend on knowing *why* we top out at ~28x, which is
  the honest position: the nominal curve predicts ~32.7x at this lead-out,
  we measured below it, and whether the shortfall is geometry or a separate
  CD-DA read cap is a further question (Keith's `readcd` data-disc test).
  **Do not generalise to a full-length disc** either way — an 80-minute
  disc runs further out the curve.

  Not re-run under `--sub`: [[plextor-speedread-subq]] applies unchanged and
  this probe reads audio-only (`ACCUDISC_SUB_NONE`, `speeds.c`), so the Q
  cliff is out of its path by construction.

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

## Consumer requests — 8trax (Rust/FLTK GUI), recorded 2026-08-01

8trax is the third correspondent and the first **GUI** consumer. Keith's ruling
(their §b.1): it is a GUI **alternative** to cdda2img that will never interact
with it — not a peer sharing work, not a front end over it. He also wants
**nothing built until both AccuDisc and cdda2img go gold** (a few weeks), so
**neither item below is scheduled before gold.** They are recorded because 8trax
has written no code against us yet, which will never be true again, and asking
them what they needed while their migration cost was zero was the whole point.

### 1. Read-path progress callback — `[P2]`, their top ask

`accudisc_read_cdda` has no progress callback; `accudisc_write` does
(`accudisc.h:270-275`). One consumer, two shapes.

**The constraint that makes it worth anything: identical signature to the write
callback**, `void (*progress)(void *user, uint32_t done, uint32_t total)`. Their
point is that the value is the two paths becoming interchangeable behind one
abstraction; a differently-shaped read callback is worth much less.

Why not "just scan the map": every progress consumer that is *not* the grid —
window title, taskbar, ETA, an aggregate bar across a batch — would scan ~350,000
bytes per frame at 60 Hz to recover one integer we already have.

**`done` MUST count sectors ATTEMPTED, including `HARD` — do not wire it to
`sectors_read`.** Found by 8trax (§d.1) before the thing existed, and verified:
`src/read/engine.c:558-562` classifies a `HARD` sector, publishes it to the map,
then `continue`s **past** `r.st.sectors_read++`. So `sectors_read` is "delivered",
not "processed" — which is exactly what `accudisc.h:1393` says it is ("returned
by the drive (excludes zero-fills)"), so the field is correct and must not
change. But a progress bar fed from it **stalls short of 100% on a damaged disc
and never completes** — on precisely the disc where the user is already anxious,
where a stuck bar reads as a hung application. If `done` is ever defined the
other way, say so in the header rather than leaving it to be discovered.

Same rule for the burn equivalent: whatever an unwritable sector is, it still
advances `done`.

### 2. Status map on the WRITE path — `[P3]`, below item 1 by their own ranking

`accudisc.h:1218-1219` has said "passes it to a read (later: write) request"
since the map was introduced. 8trax asked explicitly (§b.3.2), which is what
turns it from a parenthetical into a task.

Their justification is better than the header's, which never gave one: **a rip
failure costs time and is repeatable; a burn failure costs a physical disc and is
not undoable.** That is where a user wants to watch it happen rather than read a
percentage. Symmetric Rip/Burn tabs drawing one widget from one byte layout is
the secondary benefit.

### 3. Documentation defects this exposed — `[P3]`, cheap

- **The priority chain is not in the header — DONE 2026-08-08**, landed beside
  `ACCUDISC_MAP_STATE`. `engine.c:584-585` classifies
  `hard > suspect > recovered > C2 > ok`, one byte per sector, so **a higher
  state masks a lower one that also applies**. Reachable case: `recov[s]` is set
  by boundary-overlap consensus (`engine.c:498-517`) *before* `bits[]` is
  computed (`engine.c:520-523`), and `RECOVERED` outranks `C2`, so a
  consensus-recovered sector whose winning copy still has C2 fired displays as
  `RECOVERED` with the C2 invisible. Consequence for any consumer: **counting
  `C2` cells in the map is not the count of C2-flagged sectors** — that is
  `stats.sectors_flagged` (`engine.c:621-622`), accounted unconditionally. Map
  for the picture, stats for the numbers.

  Written up when a **second** consumer reached it from the other direction:
  cdda2img (§159.2) checked whether the masking could affect them, concluded
  correctly that it cannot at their defaults, but enumerated only **two** of the
  three levers — `c2_retries` (`engine.c:527`) and `verify_passes >= 2`
  (`engine.c:551`). The third is `overlap_sectors`, which reaches `recov[s]`
  through `ext` (`engine.c:479`) and `prev_ext_n` with no dependence on the
  other two. Their conclusion survived only because *all three* are zero at
  `ACCUDISC_READ_REQ_INIT`. A consumer enabling overlap alone — a plausible
  profile — gets `RECOVERED` and the masking with it, and the two-lever account
  would not have predicted that. The header now names all three.
- **`RECOVERED` vs `OK` is not an ordering**, and the header does not say so.
  About the *bytes*, `RECOVERED` has strictly more evidence (multiple agreeing
  reads, or a C2-clean copy found — `engine.c:495-508` sets it only when C2 went
  to **zero**). About the *medium* it is worse: a problem was observed there.
  Neither is verification — a deterministic miscorrection passes both, and
  `RECOVERY.md §12.4` has the measured instance where recovery drove C2 to 0–1
  and AccurateRip v2 + CTDB still failed. Guidance given: give `RECOVERED` its
  own hue off the `OK`→bad ramp; severity there is *effort* (extra reads taken),
  and it is the state most likely to differ on a re-rip.
- **Severity is not comparable across states.** Larger is worse within each, and
  that is deliberate so one saturation ramp works — but severity 7 under `C2`
  (~128 fired bits) and under `RECOVERED` (7 extra reads) are unrelated
  quantities. Never sum severity across states.

### 4. The Rust binding is OURS — Keith's ruling, 2026-08-01 — `[P2]`

*"We will build the bindings, and coordinate with the other agents to service
their needs."* 8trax offered to write `accudisc-sys` + the safe wrapper from
their side (their §2.1.2); **declined**, and they have been told not to
scaffold it. Not before gold, like everything else here.

"Service their needs" is the operative half: the wrapper gets designed against
8trax's real call sequence, not against the header's full surface. Four things
to get from them before writing it — asked, with no reply expected before gold:

1. Which calls they actually make, and in what order.
2. Where the subprocess boundary sits. Per their §b.2 they are subprocess-first
   *permanently* for anything privileged (burn, vendor features) because of the
   `CAP_SYS_RAWIO`-binds-to-the-inode point — so a wrapper covering the whole
   API is partly dead weight, and knowing which part matters.
3. What the status map should look like in Rust — raw `&[u8]`, a typed cell
   iterator, or per-state counts without walking it. They redraw at 60 Hz.
4. Whether the sink callback survives contact with FLTK. They confirmed the
   blocking-call-on-a-worker shape is right, but the sink runs *inside* that
   call, and a Rust closure crossing FFI has constraints the C header cannot
   anticipate.

`bindings/rust/README.md` still says "scaffolded once the C API surface
stabilizes"; that condition is now met (see item 1's stability note), and the
blocker is schedule, not readiness.

### 5. Answered, no work — recorded so it is not re-litigated

- **Threading**: blocking `accudisc_read_cdda` on a worker thread is the shape
  8trax wants (FLTK is single-UI-thread; a callback/reactor API would force them
  to marshal every callback onto the UI thread anyway). The status map is a
  particularly good fit *because* it sidesteps marshalling — worker writes bytes,
  UI reads on a redraw timer, nothing crosses the boundary. **Do not "improve"
  this into a callback.**
- **`CAP_SYS_RAWIO` decided their architecture.** It binds to the executable's
  inode, so subprocessing the CLI carries it for free, while linking
  `libaccudisc` would need `setcap` on the **GUI binary itself**. They will not
  ship a setcap'd desktop GUI, so their plan is subprocess-first *permanently*
  for anything privileged (burn, vendor features), library for the unprivileged
  read/probe/parse work. Worth remembering: for a GUI consumer the machine
  interface is not a fallback, it is the primary path for half the product.

## Consumer requests — cdda2img, recorded 2026-08-07

### 1. `subq_map` — a per-sector Q-health lane beside `status_map` — DONE 2026-08-08

**Keith ruled yes on 2026-08-08 and it shipped the same morning**, on every
surface: `accudisc_read_req.subq_map` (56 → 64 bytes, appended last so it is
additive — API_PLAN §8 row 9), the five `ACCUDISC_SUBQ_*` states, version
0.4.0 → 0.5.0, CLI `--subq-map-file`, and Python `read(subq_map=True)` /
`ReadResult.subq_map` / `SubQState` / `subq_state()`.

Verified on the PX-716A with an 11-track audio disc carrying an MCN (which is
what made the acceptance precondition below satisfiable): 3000 sectors gave
2938 OK, 32 BAD, **30 NO_POSITION (1.00%)**, and a separate read of 16
unreadable sectors gave 16 NO_AUDIO. Two things that could not be argued from
the code alone —

- `OK + NO_POSITION == 2968` matched the CLI's independently accumulated
  `subq_ok` exactly. Two code paths, same answer.
- The status lane was **all OK** across those 3000 sectors while the Q lane
  carried 32 CRC failures, which is the independence claim measured rather
  than asserted.
- The counterfactual, also measured: CRC-ing the delivered subchannel for the
  16 unreadable sectors yields **16 CRC failures** — the fabricated damage a
  DIY lane would have recorded, against 16 `NO_AUDIO` from the engine.

Kept below: the reasoning, because the *why* is the part that has to survive,
and the acceptance precondition, because it still binds on any future test.

**One thing this exposed, and it was worse than the feature was valuable.**
`CMAKE_BUILD_TYPE=Release` puts `-DNDEBUG` on the test targets, which compiles
every `assert()` in the suite to nothing. Measured 2026-08-08: `assert(0)` on
the first line of `test_map`'s `main()` exited 0. Most of the suite had been
passing without checking anything, and the green result is what hid it — a
skipped test is loud, a test that runs and passes vacuously is not. Fixed with
`-UNDEBUG` per test target plus `tests/test_assert.c`, which fails if the
mechanism ever goes inert again. With asserts live for the first time, 41/41
still passed, so nothing real had been hiding underneath.

<details><summary>The original case for the feature (kept — the reasoning is the durable part)</summary>

cdda2img §148 (answered in `2026-08-07a/b/c`). They are building the rip progress
bar as a live disc map, and after reading the binding they withdrew two requests
before sending — `status_map` and `read_span(**kwargs)` already gave them the C2
lane and the frontier. **The one thing missing is the Q lane**, and this is the
whole ask: a second `count`-byte array requested by `subq_map=True`, same
allocation, lifetime and live-read semantics as `status_map`.

**Nothing has been implemented.** The three answers they asked for needed no
code; a new field on `accudisc_read_req` is scope, and scope is Keith's call.
They are shipping the C2 lane meanwhile with the Q lane dark (§148.6), so
nothing is blocked either way.

**Why it belongs here rather than in their loop — the argument is correctness,
not cost.** Their DIY path would read `_split_streams` and CRC every sector's Q.
Hard-unreadable sectors are delivered **zero-filled** (`accudisc.h:1371-1374`),
and a zero-filled Q frame **fails** CRC-16 (measured 2026-08-07: `rc == -9`,
`ACCUDISC_ERR_CRC`, for both all-zero and all-ones fills). So a DIY lane paints
fabricated subchannel damage on exactly the sectors whose audio is already gone
— corroborating the real failure beside it, so it reads as confirmation rather
than as a bug. The engine does not have this because it `continue`s at
`engine.c:558-561` before the Q check; the only way to know that is to read the
engine. They confirmed they would have shipped it (§149.1).

The marginal cost in the engine is **not** the CRC — that already runs
unconditionally for every SUB_RAW read at `engine.c:569-580`. The only new work
is one relaxed atomic byte store per sector through the existing `map_store`
(`engine.c:124-128`). Already paid for.

**ABI: additive, no soname bump.** `accudisc_read_req` carries its own `.size`
and the IN rule (`accudisc.h:1289-1294`) treats a shorter caller's tail as zero.
`tests/test_abi.c:49` pins the current 56 bytes and would need updating with it.

**The five states, agreed by both sides** (state in the low nibble, mirroring
`ACCUDISC_MAP_STATE`/`SEVERITY`):

```
0x0  SUBQ_PENDING      not yet attempted (byte untouched)
0x1  SUBQ_OK           CRC-16 verified, ADR=1 position frame
0x2  SUBQ_BAD          CRC-16 failed
0x3  SUBQ_NO_POSITION  CRC verified, ADR != 1 (MCN / ISRC)
0x4  SUBQ_NO_AUDIO     sector was hard-unreadable; no frame delivered
```

`SUBQ_NO_POSITION` is the state neither side would have designed unprompted, and
it is **load-bearing, not a nicety**. Measured 0.98% of frames (1,590 of
162,892) are CRC-good non-position frames — MCN/ISRC, legitimately interleaved,
which `index_map.c:84-85,124-125` already skips as such. Under their worst-wins
aggregation over ~7,000-sector cells that is **every cell flagged on a perfect
disc** (§149.2). A per-frame rate that looks negligible is total after
aggregation. Note the two consumers read it with opposite polarity: healthy for
a health lane, signal for `subq_toc`, which is the argument for it being a state
rather than a boolean.

**The rate varies by DISC, 0% to ~1%, and that makes it more necessary rather
than less** (their §150.4b: an entire pressing with no MCN and no ISRCs, 0.00%
across all fifteen captures). A state whose necessity depends on the disc is the
dangerous kind — validate the Q lane on that disc alone and you prove the state
optional. **So any acceptance test for `subq_map` must name a disc carrying MCN
or ISRCs as a precondition**; otherwise `SUBQ_NO_POSITION` is unreachable and
the arm passes without testing anything.

Two design calls, both agreed (§149.4): **refuse** `subq_map` without
`ACCUDISC_SUB_RAW` (`ACCUDISC_ERR_INVAL`) rather than return a uniform map a
renderer will draw as a lane; and the **severity nibble stays zero**, because Q
integrity is one CRC-16 and anything else there would be a proxy.

A third emerged during implementation and neither side had spotted it:
**`crc_ok` must be consulted before `adr`.** `accudisc_q_parse` fills `adr` from
`q[0]` whether or not the CRC verified (`src/cdda/subq.c:52-53` — it must, that
byte is the frame-type header), so a corrupt frame routinely presents ADR=2 or
3. Classify on `adr` first and such a frame is painted `NO_POSITION`, i.e.
reported as **healthy**, on exactly the frames the lane exists to find. Nothing
downstream could catch it: the byte is well-formed and names a real state.
`adsc_subq_byte()` exists as a named function purely so the order is testable,
and `tests/test_map.c` asserts the CRC really failed *and* `adr` really is still
2 before checking the verdict — without those two the case would pass while
testing nothing.

</details>

### 2. Q lag — MEASURED, no lag on this drive; `tools/qlag.c` shipped

Their question 3 was whether Q lags the audio, which would make an LBA-indexed
map wrong by *k* everywhere. Settled for this drive and left open for the fleet.

**Structural half (certain):** an ADR=1 frame is self-locating, so lag is
invisible if you index by the frame's own address — but `accudisc_q_parse` leaves
every position field zero on CRC failure by deliberate design
(`accudisc.h:1481-1485`), so a CRC-**bad** frame can only be placed by transfer
slot. Lag is irrelevant for the frames you can locate and decisive for the ones
you cannot, which are exactly the frames the lane exists to draw.

**Measured half:** `tools/qlag.c` (new, public-header-only, device-free) over a
whole-disc raw-sub capture on the PX-716A: **157,871 of 157,914 position frames
at delta 0** — no lag, and not a near-zero average. The 43 exceptions are six
short contiguous runs whose deltas are all exact multiples of 512 sectors (a
buffer number, not a disc number). **No mechanism claimed.** What it does prove
is that a frame can pass CRC-16 and still be positionally wrong, at ~0.03%.

Falsified before it was trusted: `LAG +3` on a shifted capture, `SPREAD` (41
deltas) on a jittered one, refusal on a wrong stride, and a base-mismatch warning
on a partial capture. **The SPREAD threshold is 8** (`SPREAD_MAX`), not the
64-entry histogram capacity — `2026-08-07c` corrects that after we sent the wrong
number.

**CLOSED at scale by cdda2img** (their §150.2 and §151 — the per-capture numbers
live in the correspondence itself; their sweep file is under *their* `private/`,
machine-local and in no clone, so it is deliberately not cited as a path):
42 captures, 3 discs, 4×/8×/24×/32×/40×, three passes each —
**NO LAG on every one**, nothing near the threshold. The result we could not have
obtained: two independent Q-collapse events (the vendor speed cliff, and one
degraded pass among identical siblings) drop CRC-good to **47.79%** and
**38.73%** while in-slot stays at **99.988%** and **99.978%**. A Q yield below
half does not disturb slot alignment — damage removes frames without moving the
ones that remain. That rules out the plausible failure (whatever destroys Q CRC
also disturbs delivery timing, so survivors drift exactly when the lane most
needs to be right) and it could not have been argued from first principles.

They also rebuilt the shifted arm independently, and the detail that fell out of
it is worth keeping: the minority deltas **tracked the shift** (`-2048 → -2045`),
which makes the buffer-aligned anomalies positional facts about the capture
rather than artefacts of the measurement.

One defect their sweep exposed, since fixed: `nonpos` was printed only as a
percentage of *all* frames, which moves when Q yield moves — so comparing
captures across speeds suggests the interleave thins under load. It does not;
normalised against CRC-good it is flat to three digits across a 2× change in
yield. Both denominators now print.

If a nonzero lag ever appears the unit is **whole frames** (one Q frame per
sector, not sample pairs) and the sign convention is
`accudisc_probe_c2_lag`'s — positive = the companion stream trails the audio.

## Outstanding — carried from 2026-07-26 (phase 3 landed; these did not)

### 0. RECOVERED sectors were returned WRONG, 9/9 — **DROPPED 2026-08-08 by Keith. Do not chase.**

**Ruling: "If there really is a bug, let it present itself, then we can deal
with it."** Not closed as fixed and not claimed to be a non-bug — deliberately
abandoned as an investigation, with the evidence kept below so a recurrence is
recognisable rather than re-derived.

**The provenance, which is the whole reason it was droppable.** One run, one
disc, one day: Tracy Chapman, five whole-disc reads 2026-07-26 with the **full
recovery ladder** — `--retries 3 --c2-retries 4 --verify 1 --overlap 2 --ladder
40,32,24,8,4`. Never witnessed again.

**It is unreachable on any default read.** The surviving hypothesis after the
discriminator run (their §90.2) is **H1-local**: the ordinary publish loop is
correct — proven by byte 0 and 162,882 correct `OK` bytes — and only the
*recovery* path records at a different index. Every one of the ten anomalous
bytes was state 4 `RECOVERED`; not one was `OK`, `C2`, `HARD` or `SUSPECT`. With
`retries`/`c2_retries`/`verify_passes`/`overlap_sectors` at their
`ACCUDISC_READ_REQ_INIT` zeros, `RECOVERED` cannot occur at all (the three levers
are named at `ACCUDISC_MAP_STATE`), so no default read can hit this.

**Honest limit on "never seen again":** the disc has been read dozens of times
since, but not once with those recovery flags. The absence is unrepeated
conditions, not a repeated negative. That cuts both ways and is exactly why
"let it present itself" is the cheap policy rather than the risky one.

**Keith's AccurateRip point, checked:** `RECOVERY.md` §12.3 records AR v2 ✅ at
40/32/8× and ❌ at 24/4× *in that same run*, so the gate did fire and was noticed.
An undetected silent corruption is not what this was.

**If it ever recurs, the one test worth running** (cdda2img §90.3, needs no drive
time): find a capture where a sector went `RECOVERED` **and** its delivered bytes
were correct. Under H1-local the flag sits at +1 from a *correct* sector, so the
sector it names would be clean. Their data could not supply that case; all nine
of theirs came back wrong.

<details><summary>Original entry, kept for recognisability (superseded)</summary>

#### RECOVERED sectors were returned WRONG, 9/9 — cdda2img §89.5

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

**New evidence 2026-07-27 (cdda2img §102.2, then measured properly in §103), and
it sharpens the claim rather than weakening it.** Their A/B control read the
damaged region (LBA 112500 +700) twice through the *same* transport and got **36
differing sectors**; a follow-up four-reads-per-condition run confirmed
fixed-speed determinism is false (3/4 pinned reads distinct, 1–4 sectors apart —
full table and its unseparated confound under "the pit is readable" above).
So same-speed reads on this disc are not generally stable —
which is what makes the 9 corrupt sectors a different phenomenon rather than
more of the same. Where the disc is merely jittery the reads disagree and a
verify pass would flag SUSPECT; the 9 were **stably wrong**, agreed with
themselves, and took the `diff == 0` "confirmed" early-out. The unreachable-
ladder root cause is unaffected — a disagreement is exactly what would have
reached `consensus()` — but "the drive misreads the same way every time" is now
known to be true *of those sectors*, not of the drive.

It also closes off an instrument: byte-level differential comparison cannot
adjudicate anything in a damaged region, because nothing there agrees with
itself. Absolute gates only — the RECOVERY.md invariant, reconfirmed from a new
direction.

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
entirely recovery *policy*. `consensus()` is blind because **it largely resamples
the same function**: at a fixed speed the drive mostly returns the same wrong
bytes, so agreement is cheap and carries little information.

> **Weakened 2026-07-27 by measurement (cdda2img §103, retracting their own
> §102.2 gloss).** This paragraph used to say agreement at a fixed speed was
> *guaranteed* and the resampled function *deterministic*. Measured on the
> damaged span (LBA 112500 +700, four reads per condition, PX-716A):
>
> | condition | distinct results | pairwise sectors differing |
> |---|---|---|
> | unpinned | **4/4** | 31, 32, 35, 35, 42, 43 |
> | pinned `--speed 8` | **3/4** | 1, 1, 3, 3, 4 (one identical pair) |
>
> So: **speed is the dominant variable** — pinning drops divergence by roughly an
> order of magnitude, which supports the directional part of §93/§94 from a
> second angle — but **fixed-speed determinism is FALSE**. Repetition at one rung
> has a small non-zero yield, so "more retries only deliver the wrong answer with
> more confidence" was too strong.
>
> **Do not lift that table without its confound**, which cdda2img stated before
> anyone could: a slower rung produces fewer wrong sectors to begin with, so
> "fewer differ at 8x" is what you would see even if erroring sectors were
> equally random at both speeds. Divergence and error rate are not separable from
> these two conditions alone — it needs an AccurateRip/CTDB reference to express
> divergence as a fraction of sectors *wrong at all*. Not done, not claimed.
>
> **None of this moves the root cause.** The ladder is unreachable for structural
> reasons (below), not because rereads are information-free — and the nine
> corrupt sectors still agreed with themselves and took the `diff == 0` early-out,
> which now reads as a sharper anomaly rather than a vaguer one, since same-speed
> reads on this disc are *not* generally stable.

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

</details>

### 0b. Recovery rereads use the read mode c2lag.c measured as DIFFERENT on this drive — `[P2]`

> **STAYS OPEN — item 0 being dropped does NOT drop this.** Reviewed
> 2026-08-09, because the numbering invites exactly that mistake. Item 0 was
> dropped as a possibly-ephemeral observation from a single run of a single
> disc. **0b is not an observation at all** — it is a design inconsistency
> visible by reading two modules against each other, and the measurement it
> rests on is `c2lag.c`'s own live one on this drive (a streaming pass flagged
> ~40 sectors where per-sector rereads of the same LBAs flagged zero). It would
> still be true if item 0 had never been reported. **Not a read-speed item
> either** — it is about read *mode*, so it does not belong to that session and
> must not be abandoned with it.

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

### 1. Remove all SpeedRead guards — USER-DIRECTED, NOT STARTED — **for the read-speed session**

**Re-affirmed 2026-08-08**, with the reasoning stated more sharply than the
original ruling: *"SpeedRead is for CD/DVD-ROM only. Having a guard against
something the hardware is physically incapable of doing doesn't make any
sense."* The guard defends against a throughput increase the drive cannot
produce on CD-DA.

**CORRECTED 2026-08-09 — this paragraph used to say the task "belongs to the
single dedicated read-speed session … anything unfinished when that session ends
is permanently abandoned", and that was wrong in a way that could have destroyed
the task.** It is **DESK WORK**: source-only, no drive, no measurement — see the
banner at the top of this file, which lists it as such. Nothing about it consumes
session time, so nothing about it should be exposed to the session's abandonment
rule. And it is a **standing directive from Keith**, given 2026-07-26 and
re-affirmed 2026-08-08; a scheduling note is not licence to let a directive
lapse.

What is true is that it is **gated**, which is a different thing from scheduled:
it needs the ABI ruling below before anyone writes code. A gate waits for an
answer; a schedule waits for a clock. This one waits for an answer.

**One thing to weigh in that session, because it is the only real cost.**
Removing `allow_unsafe` from `accudisc_read_req` is the first *subtractive* ABI
change we have made — every other has been additive. The `.size` IN rule
(`accudisc.h`) cannot help: a 0.5 caller setting `allow_unsafe = 1` would have
that byte silently reinterpreted as whatever occupies the offset next, which is
the "well-formed data, wrong referent" failure the size field exists to prevent.
Two ways out, and the choice is a design decision rather than a detail:
**(a)** keep the field as a reserved, ignored byte and delete only the
enforcement — zero ABI cost, honours "remove enforcement only" literally; or
**(b)** remove it and bump the soname. (a) is the cheaper reading of the ruling
and does not foreclose (b).

**Original ruling 2026-07-26: "Remove all guards for SpeedRead. CDDA is
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

### 2. `setcap` the INSTALLED binary, not the build-tree one — **DONE. Verified 2026-08-08.**

Keith: *"I'm pretty sure that P1.3 was closed weeks ago. Double check the
installer."* Correct — it shipped and the entry was simply never closed. Read
back today rather than recalled:

- `ACCUDISC_SETCAP_ON_INSTALL`, default **ON** (`CMakeLists.txt:74`).
- The install rule is `cli/CMakeLists.txt:28-57`, and its own comment states the
  point this entry was opened for: `install(TARGETS)` copies the binary and an
  xattr does not survive the copy, *"so this is not a duplicate of the post-link
  setcap; it is the only thing that arms what actually gets installed."*
- **DESTDIR-aware**, which the original entry did not ask for: capabilities do
  not survive tar/cpio/rsync, so a staging build skips the cap and prints the
  exact command for the package's post-install script instead of silently
  producing an unarmed package.
- Failure is `FATAL_ERROR`, not a warning — deliberate, because an unarmed
  binary fails *quietly* (the vendor path falls back to generic MMC with no
  error), so a warn-and-continue would ship something everyone believes is armed.
- `ACCUDISC_INSTALL_SETCAP_COMMAND` is bare `setcap` rather than the build-time
  `doas …` form, since `cmake --install` to a system prefix is already root.

Nothing outstanding. The build-tree `setcap` target remains for the
rebuild-disarms-it case, which is a separate convenience and not this item.

<details><summary>Original entry (superseded — the work landed 2026-07-29)</summary>

`CMakeLists.txt:34-38` already documents this and we have not been doing it. The
capability binds to the **inode**, so every rebuild that relinks the CLI drops
it. Three independent reasons now, the third new:

1. It interrupts Keith on every rebuild.
2. It silently disarms the vendor path mid-session (four occurrences).
3. **Since the 2026-07-26 relink, cdda2img executes the same inode**, so our
   rebuild drops the capability from *their* binary too.

> **The mechanism now exists** (`make install`, 2026-07-29):
> `ACCUDISC_SETCAP_ON_INSTALL` caps the installed binary, strip runs first so
> the xattr survives, and `DESTDIR` skips it with the command a package script
> must run. Verified with `getcap` on a real install.
>
> **It is enabled, not closed, and the difference is reason 3.** Nothing is
> fixed until the binaries actually invoked come from the install — ours *and*
> cdda2img's, which still executes the build-tree inode. Until that switch
> happens the rebuild keeps disarming both. **Deciding that is Keith's**, not
> something to change under either project's feet.
>
> cdda2img's §119.2 raises the severity and the argument holds: this is a
> **measurement-validity** defect, not an inconvenience. A rip with the vendor
> path disarmed is a *different configuration*, so any bench run, A/B or ladder
> probe straddling a rebuild silently compares two configurations while looking
> like one series. Their `disc_ab.py` refuses a stale extension and re-hashes
> the engine; neither check can see a dropped capability. The cost is wrong
> answers, not lost minutes.

</details>

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

**AUDIT RUN 2026-08-09. Six `!ctx.quiet` sites in `cli/main.c`; one new finding,
and it is not the one task 1 removes.**

| site | what it gates | verdict |
|---|---|---|
| `:1118` | the human progress line | **Fine.** `--progress-fd` carries the same information on a machine channel *outside* the `quiet` test — the pattern the other sites should follow. |
| `:1489` | "tracks A-B, lba N count M" | Descriptive, not integrity. Leave. |
| `:1585` | the `LIKELY_ON` uncap warning | The known defect. Removed by task 1. |
| **`:1622`** | **"read-speed uncap: on (was off)"** | **THE FINDING — see below.** |
| `:1632` | a newline | Cosmetic. |
| `:1711` | the CD+G summary | Statistics for a human. Leave. |

**`cli/main.c:1622` is a notice that we MUTATED PERSISTENT DRIVE STATE, and
`-q` hides it.** It is a strictly worse case than the warning that prompted this
item, for two reasons. First, it reports something we *did* rather than a risk
we suspect — and the uncap survives the process, so a machine consumer passing
`-q` gets its drive silently reconfigured for every later invocation, including
other tools' (this is the [[drive-contention-flock]] failure mode arriving by a
different door). Second, **it does not go away with task 1**: that ruling removes
enforcement while explicitly keeping `speed-uncap`, `--uncap` and the queryable
state, so this line survives the change that retires `:1585`.

Fix, in the shape `:1118` already demonstrates: emit the state change on a
machine channel that `quiet` does not gate, and keep the stderr prose as the
human rendering of it. The `(was %s)` prior value is the part that matters — it
is what a caller needs to restore the drive, and the push/pop SOP depends on it.

**There is no anomaly. 48 and 40 are the same physical read wearing two labels.**
Keith ran four whole-disc reads (`--start 0 --count 162892`, uncap on, no vendor
driver) instead of either discriminator — **better than both, because a full-disc
read covers every radius identically at both settings, so the CAV term cancels
exactly rather than being modelled**:

| req | seconds | sectors/s | whole-disc avg | C2 sectors | C2 bits |
|---:|---:|---:|---:|---:|---:|
| 48 | 91.5 | 1780.3 | **23.74×** | 63 | 1160 |
| 40 | 89.8 | 1814.1 | **24.19×** | 54 | 1133 |
| 32 | 113.1 | 1439.8 | 19.20× | 1 | 12 |
| 24 | 150.2 | 1084.8 | 14.46× | 2 | 24 |

48 vs 40 differ by **0.45×, 1.9%**, with 40 marginally the *faster* — noise. 40 vs
32 differ by 20.6%, a real rung. The C2 column corroborates independently and was
not part of the timing. So the residual chased through §97–§99 was ~2%, and the
mid-disc `speeds` delta of 1.19× was almost entirely the radius term from 4b.

**The collapse rule (`accudisc.h:1036-1038`) is vindicated** — 48/40 is exactly the
indistinguishable pair it describes. What was wrong was the *input*: mid-disc
`measured_cx` at per-rung radii cannot support that comparison; whole-disc
throughput can.

**Page 2A is NOT lying** (Keith, explicitly): it correctly reports the requested
speed *ceiling* and needs reading in context. Both sides had drifted toward
treating it as faulty. The ceiling is 48 because the uncap is on; actual CD-DA is
governed to 40. `speed-uncap off` changes only the data-disc maximum and the
`speeds` display.

**`speeds` figures vary with disc degradation** — Keith has seen the governor cap at
32× and as low as 8× on damaged media, with the uncap having zero influence.
Throttled speeds are real and measurable, not a page-2A artefact.

**Consequence for the recovery side, unexpected and useful:** at 40–48× this disc
flags 63 and 54 C2 sectors across a ~3,200-sector span; at 32× it flags **one
sector, 12 bits**. A ~50× reduction in flagged sectors for a 21% throughput cost.
"Slower is better on damaged media" now has a direct hardware measurement, on the
same disc where their bench ranked the speed-varying ladder 19/20 against
`sector-hammer`'s 2/20.

**Neither discriminator will be run** and 4b's instruments are not needed — they
would characterise a confound the settled method avoids by construction. 4b's
*contract* findings stand on their own (see below).

<details><summary>Original entry (superseded)</summary>

### 4a. The phantom 48× ladder rung — (SUPERSEDED — historical, no work outstanding)

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
"violations". This is the same confound behind the `speeds` min/max item
(inner/middle/outer) under "Probes / diagnostics". **See 4b — it is worse than a
caveat, it operates inside a single table.**

</details>

### 4b. `speeds` biases every table against its own fast rungs — `[P2]`, OUR DEFECT

**RECONCILED 2026-08-09 — this entry is now MOSTLY CLOSED, and what is left is
one paragraph of documentation.**

- **The radius term: DOCUMENTED 2026-07-28**, which was fix option (a).
  `accudisc.h:1136-1141` states it outright — rungs are laid out along the span,
  a descending candidate list puts the fast rungs innermost, and *"treat a modest
  cross-rung inversion as unproven, not as a measured fact about the rungs."* The
  header also now says which comparison to trust instead (within a rung, where
  the bands are a fixed distance apart and the timed length is identical). Option
  (b), reversing rung order between bands, was considered and rejected: it
  cancels the bias in the **mean**, and we deliberately do not report a mean.
- **Fix option (c) landed too** — the min/max sweep, hardware-validated
  2026-07-28. See "Probes / diagnostics".
- **The discriminator instruments are moot** (whole-disc reads avoid the confound
  by construction) and will not be run.

**STILL OPEN — the second confound, and it is DESK WORK, not session work.** The
timed-window **length** varies across rungs (`want = req * 75` clamped to
[150, 2250]: 2250 sectors at 32× and above, 1800 at 24×, 1200 at 16×, 600 at 8×,
300 at 4×), and the header does not say so. What it *does* say —
`accudisc.h:1143-1145` — is that the timed length is identical **within** a rung,
across its three bands. That is true, and it is the sentence most likely to be
read as covering the cross-rung case it says nothing about. So 48-vs-40 is
length-clean and the documented radius caveat is complete there, but any rule
spanning 48 down to 4 compares rungs differing in radius **and** in timed span,
and the second effect has never been measured. Fix: one paragraph beside the
radius one. Measuring it is optional; **not claiming it is absent is not.**

Found answering cdda2img §97.2. Bare `speeds` probes the **middle half**
(`cli/main.c:658-662`: `lba = leadout/4`, `count = leadout/2`), so it is *not*
inner-disc — that part of their hypothesis fails. But each rung gets its own
window and **the windows march outward**:

```c
/* src/drive/speeds.c:56, 66 */
uint32_t stride = count / ncand;
uint32_t wlba = lba + (uint32_t)i * stride;
```

The default candidate list is **descending** (`cli/main.c:647`,
`{52,48,40,32,24,16,8,4}`), so **the fastest rungs are measured at the innermost
radii and the slowest at the outermost** — a systematic bias against fast rungs, on
a CAV drive, in exactly the direction of the task-4 phantom rung.

Worked example, their uncap-on run (leadout ≈ 162,892, so ncand 7, stride ≈
11,635): the 48 rung is measured at LBA 40,723 and the 40 rung at 52,358 — **11,635
sectors further out**. Against the drive's own curve (`0..359,997 → 20.0x..48.0x`)
that gap is worth **0.90×** linear-in-LBA or **1.13×** under a radius model
(speed ∝ r, LBA ∝ area). Their observed gap is 1.84×. **So geometry accounts for
~half to two-thirds of the "faster setting measures slower" anomaly**, and a
residual of ~0.7–0.9× survives and may be real. The bias has the same sign as the
effect, which is the worst arrangement for judging it.

**Contract defect, not just a docs gap.** `accudisc.h:1036-1038`'s collapse rule
compares `measured_cx` across rungs, and `accudisc.h:1030-1032` explains the
per-rung window (cache freshness — a sound reason) without ever stating that the
consequence is a radius term in every cross-rung comparison. Fix options: (a) say
so in the header; (b) interleave windows so the bias cancels; (c) the
inner/middle/outer item, which reports the gradient instead of hiding it — best of
the three.

**Zero-build discriminators** (`--ladder` preserves order, no sort, no dedup —
`cli/main.c:622-630`). Cross-rung `measured_cx` is not safe today.

- **`--ladder 48,48,48,48`** — the good one. Windows at 40,723 / 61,084 / 81,445 /
  101,806 (61,083-sector span, nominal gradient **4.75×**), and all four time the
  *same* window length, since `want = req*75` clamps to `SPEEDS_MAX_SECTORS` 2250
  (`speeds.c:70-73`) and 48×75 = 3600 clamps. Speed, length and candidate held
  constant, only radius varies → the correction curve for every default-start
  table.
- **`--ladder 48,40,48,40`** — interleaved, a *within-run* control: a 48 on each
  side of a 40, so the trend differences out instead of needing a model.
- **NOT `--ladder 48,40`** — corrected 2026-07-26, this was wrong when first
  written here. `stride = count/ncand`, so **fewer rungs spreads the windows
  further**: a 2-rung ladder puts them 40,723 apart, a **+3.17× bias — larger than
  the entire 1.84× anomaly**. It would look like a spectacular confirmation of the
  effect while measuring only geometry.
- **`--passes` does not exist.** `cmd_speeds` accepts `--start` and `--ladder` and
  nothing else (`cli/main.c:620-636`); anything else prints usage and returns 1.
  Repeat by re-running the command.

**Second, uncharacterised confound: timed window LENGTH also varies by rung.**
`want = req * 75` clamped to [150, 2250] (`speeds.c:70-73`) gives 2250 sectors for
every rung at 32× and above, but 1800 at 24×, 1200 at 16×, 600 at 8×, 300 at 4×.
So 48-vs-40 is length-clean (radius is the whole confound there, which is why the
correction above is complete), but **any rule spanning 48 down to 4 compares rungs
differing in radius *and* in timed span**, and the second effect is unmeasured.

### 5. Stock-ceiling table — PARTLY closed. The A-vs-B discriminator is REOPENED `[P3]`

**REOPENED 2026-07-26 (Keith caught it; cdda2img §95 retracts their §91.1).** This
entry briefly recorded mechanism B as refuted. **It is not**, and the CD-RW audio
disc is still the only discriminator. Do not close this again without that disc.

> **Read-speed session, item 3 — and BLOCKED ON MEDIA WE DO NOT HAVE, so expect
> it to be abandoned by the session's own rule.** Recorded 2026-08-09 so nobody
> re-derives this. The only discriminator is a **CD-RW audio disc**; no amount of
> drive time on the discs we own separates A from B, because data and audio media
> both yield 48 under either mechanism (the table below). If Keith wants this
> settled he must supply the disc *before* the session — otherwise the honest
> outcome is to abandon it and leave `adsc_uncap_classify`'s strict `>` documented
> as unvalidated at the boundary. **It may also go moot with task 1**, if the
> classifier goes with the guards; check that first, since it costs nothing and
> could retire the whole entry.

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

### 5c. The "--driver auto" advice contradicts itself when a driver WAS named — `[P2]`, DESK WORK

**Not read-speed session work** (classified 2026-08-09): it needs no drive and no
measurement, only the conditional. Do not let the session's abandonment rule take
it.

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

### 5d. `speed-uncap` needs a disc; `speed` does not — undocumented envelope split — `[P3]`, DESK WORK

**Not read-speed session work** (classified 2026-08-09): documentation only, and
the behaviour it documents is already measured. Do not let the session's
abandonment rule take it.

Also §91.2. The Plextor selftest issues a real vendor opcode, which requires a
medium, so **`speed-uncap` — report *or* set — is unavailable on an empty drive**.
`accudisc speed` reads mode page 2A directly, needs no vendor driver, and answered
the same question (`page2A max 40x`) with an empty tray. The two subcommands
report overlapping numbers with **different availability envelopes** and nothing
says so. Document in `accudisc.1` and the header; note that anything assuming it
can query uncap state before a disc is loaded cannot.

### 5b. `ACCUDISC_UNCAP_OFF` is labelled authoritative but source 3 infers it — `[P3]`, DESK WORK

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

### 6. Phase 4 — the Python binding — FIRST CUT LANDED 2026-07-27

`bindings/python/`: cffi **API mode** (`build_accudisc.py`), the wrapper in
`accudisc/__init__.py`, 33 device-free tests wired in as ctest
`test_python_binding` (skips on missing python3/cffi, never fails silently).

**API_PLAN §7.3's central premise was wrong and is superseded.** It justified
"streaming API over the sink" with the claim that the subprocess path
"structurally cannot hand the caller the PCM without copying it through a pipe".
No such pipe exists: `--pcm FILE` has the CLI write the file itself. cdda2img
caught this (§101.2) and they are right. The corrected shape:

- **`read_span()` returns `bytes`** — bounded, no sink, no temp file. This is
  the call that earns the binding, because the subprocess path forces a
  write-file/read-back/unlink round-trip *per recovery attempt*
  (`passes x rungs` per failed track).
- **Whole-disc reads stay file-based, and `read_to_file` IS the recommended
  path** — corrected 2026-07-29. This entry used to say it was not, on a
  device-free measurement of the copy (0.02 s cache-warm over 0.941 GB). That
  number was right and answered the wrong question; the on-drive A/B/A puts
  the binding **0.06 s** from the subprocess over a 112 s whole-disc rip,
  inside a 3.68 s noise floor. See the closed `read_to_file` item below.

Held as they were: `ERR_NOTFOUND` is absence (returns `None`), `accudisc_write`'s
caveat is a **positive** return (`_check` tests `rc < 0`), copy by default with
zero-copy opt-in.

**Two Python-specific hazards §7.2 did not cover, both measured:**

1. cffi's default callback returns **0** when the Python sink raises — and 0 is
   what the engine reads as *continue*. A sink bug would have produced a
   completed rip and a traceback on stderr. Fixed with `error=1` + `onerror=`,
   re-raising the cause out of `read()`.
2. "View released on return" does **not** reach a *slice* the sink took — a
   memoryview slice re-exports from the underlying buffer, so the parent's
   release misses it. A refcount detector for this was built and **withdrawn**:
   it read differently per calling frame and flagged correct code. Documented
   limit + `copy=True` default instead, with a test pinning that the hole
   exists so a future CPython closing it is noticed.

**Still to do on this entry:**

- ~~**`accudisc_probe_speed_ladder` is NOT bound**~~ — **BOUND 2026-07-29**
  (`ea11d56`). `Device.probe_speed_ladder()`, A/B'd against the CLI on Tracy at
  **both** `points=3` and `points=1`. The CLI-vs-CLI control fired and settled
  it: binding-vs-CLI and CLI-vs-CLI show the same max delta (0.41x) and mean
  (0.027x), while every structural field — 7 verdicts, 7 page-2A readings, the
  admitted ladder — is identical across all three runs. **The ABI window is
  spent:** `accudisc_speed_rung` is frozen at 14 bytes and pinned in
  `tests/test_binding.py`. The span defaults live in the binding rather than in
  a note asking the caller to reproduce them.
- ~~**`accudisc_write` is NOT bound**~~ — **BOUND 2026-07-29**.
  `Device.write()` returns `WriteResult`; OK/CAVEATS both mean the disc WAS
  written, `not_blank`/`error` arrive as exceptions (`Unsupported` vs other).
  **Both paths device-verified 2026-07-29:** refusal on the PX-716A with Tracy
  loaded, and success on a CDEmu virtual blank — `WriteResult.OK`, progress
  callback fired 6 times ending `(150, 150)`, and a full **round-trip through
  the binding** (write, then `read_span` of the burned track) came back
  byte-identical, 0 flagged, 0 hard. CDEmu state restored to the empty device
  it was found as.

  **`WriteResult.CAVEATS` is still device-free-only** — reaching it needs a
  CD-Text blob whose SIZE_INFO disagrees with the `.toc`, which the round-trip
  does not construct. The four-token mapping is tested; that one path is not.

<details><summary>the two entries as originally written</summary>

- **`accudisc_probe_speed_ladder` is NOT bound — but the reason it was held
  back is gone `[P2]`.** It gated on the `speeds` min/avg/max item, which
  landed 2026-07-28 and spent the ABI window: `accudisc_speed_rung` is now
  10 bytes (`requested_x`, `reported_x`, `measured_cx`, `min_cx`, `max_cx`)
  and still has no `size` field, deliberately. **Binding it is now the next
  step, and it is what makes any further growth of that struct a real
  version break.** So bind the struct as it is, or decide the size field is
  wanted, before writing the cdef — not after.

  Two things the binding must carry across, both of which are silent if
  missed: `points` is 1 or 3 and *nothing else* (2 is `ERR_INVAL`, not a
  round-down), and `min_cx`/`max_cx` of 0 mean "no gradient measured", not
  "measured zero" — a binding that surfaces them as plain ints hands the
  consumer a number that reads as a stalled rung.

  **cdda2img is waiting on this and will not infer it.** They keep their
  `speeds` regex until we send an explicit sentence saying it can go
  (§ca.4). Do not send that sentence when the code lands — send it when the
  probe is *bound* and they can read the numbers from the struct.
- **`accudisc_write` is NOT bound** — the one destructive path. Order agreed
  with cdda2img (§101.6): probes → `read_span` → status map → whole-disc (if at
  all) → `write` last, only after the read path is A/B'd on real media. It will
  need the `summary … result=` token and the positive-return rule.

</details>

- ~~**No hardware validation yet.**~~ **VALIDATED 2026-07-27 (cdda2img §102).**
  Their `tools/binding_ab.py` on the PX-716A with Tracy: **3/3 spans
  byte-identical** subprocess vs binding (inner LBA 300, middle 72415, outer
  162092 — sampled across radius deliberately, since the §9.3 phantom rung was a
  CAV radius term), and every compared TOC field agrees (11 track LSNs,
  `disc_last_lsn`, source, degrade, trusted, anomaly set, data_tracks,
  session_count). Reproduced after a harness refactor.

  **Read the scope precisely — the pass is narrower than "it works".** Each span
  is read three times (subprocess, subprocess, binding) and scored only if the
  two subprocess reads agree. On the damaged region (LBA 112500 +700) that
  control **fired**: two consecutive reads of the same span by the same
  transport differed in **36 sectors**. Without the control that would have been
  reported as "the binding differs in 36 sectors" — well-formed, reproducible-
  looking, wrong referent.

  So: the binding agrees with the subprocess path **where the disc reads
  deterministically**, and has not been shown to agree on a damaged region
  because nothing agrees with itself there. **Consequence for us:** byte-level
  A/B is not available as a check on recovery behaviour, and any future claim
  about the binding's recovery path needs an absolute gate (AccurateRip/CTDB),
  never a differential one. That is the RECOVERY.md invariant arriving from a
  new direction.
- Packaging is a **source package requiring a built libaccudisc**, not a wheel;
  a wheel wants task 2 (install properly) first. The pin is
  `accudisc_version_string()`; `pyproject.toml`'s version is checked against the
  loaded library by a test that fails on drift, and `Device()` refuses on
  compiled-vs-loaded major/minor skew with `AbiMismatch`.
- **`pip install .` works but the result is NOT relocatable** — `[P2]`, and it
  is another argument for task 2. Verified into a clean venv: it installs,
  imports, and raises correctly. But linked against an *uninstalled* library the
  extension carries an absolute `RUNPATH` to this build tree
  (`readelf -d …/_accudisc*.so` → `/home/kgr/Git/accudisc/build/src`). Correct
  here, meaningless elsewhere, and on a machine with some *other* libaccudisc it
  could resolve to the wrong one. Build-from-source-per-environment until
  discovery goes through `pkg-config accudisc`. cdda2img told (§bt.1) before
  they wrote CI against the looser claim in §bs.4.
- **The `toc` line has no token for `sessions_total` or the mapped-session
  count** — `[P3]`, raised by cdda2img §102.5. Confirmed there that
  `cli/format.c:47` prints `info->session_count`, i.e. the READ DISC INFORMATION
  count, so their `session_safe` gate was reading the right measurement all
  along — no bug. But the state we called the dangerous one,
  `sessions_total > mapped_session_count` (seams known to exist, not locatable),
  is **invisible to a subprocess consumer**: there is no token for either
  number, so no CLI caller can refuse on it. The binding can express it. Two
  more tokens on the `toc` acquisition line would close it for everyone.
  cdda2img is migrating and explicitly did not ask; recorded because the gap is
  real rather than cosmetic.
- **`accudisc_toc` is not pinned in `tests/test_abi.c`** — `[P3]`. It has no
  `size` field, so a field added in C arrives in a binding as zero with nothing
  complaining, and the pure guards (`check_audio_range` and friends) would then
  answer about a TOC that is not the disc's. The Python suite now pins
  `sizeof(accudisc_toc)`; the C side arguably should too, since API_PLAN §7.1
  claims size-less structs are pinned there and this one is not.

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

**STILL LIVE 2026-08-09, but the blocker is gone and the remedy is now one
symlink.** Verified rather than assumed:
`/home/kgr/Git/cdda2img/tools/accudisc/accudisc` still resolves to
`/home/kgr/Git/accudisc/build/cli/accudisc` (symlink dated 2026-07-26), so the
dependency is real today. But an installed copy now exists —
`/usr/local/bin/accudisc` with `/usr/local/lib/libaccudisc.so.0` beside it — so
repointing that symlink fixes it outright, with no build-tree coupling left.

**And there is a second, larger reason to repoint it, found while checking the
first.** `getcap` on both binaries: `/usr/local/bin/accudisc` carries
`cap_sys_rawio=ep`; **`build/cli/accudisc` carries nothing.** So cdda2img's
symlink currently resolves to an *unprivileged* binary, and every vendor-opcode
path it tries — the Plextor selftest, therefore the whole uncap surface — fails
for want of a capability that is sitting on the installed copy it is not using.
This is not new damage from any one rebuild; it is the steady state, because
`ACCUDISC_SETCAP_AFTER_BUILD` is OFF in this build tree while
`ACCUDISC_SETCAP_ON_INSTALL` is ON. The build tree is the *unarmed* artefact by
configuration, and their symlink points at it.

Worth telling them explicitly rather than filing: a caller seeing generic-MMC
fallback on a drive that has a working driver would reasonably suspect our
driver gating, and the actual cause is which of two binaries their symlink
names.

**Two reasons this is note-only and stays `[P3]`.** It is *their* symlink, so
the change is theirs to make, not ours to make for them. And it is
self-resolving: item 9's ruling deprecates cdda2img's use of the CLI in favour
of the Python binding, and the binding loads the installed library, so the
dependency disappears when that migration completes. Repointing the symlink is
worth suggesting as an interim, not worth engineering around.

### 9. cdda2img §106 — the CLI is **not** retired; cdda2img's *use* of it is deprecated — **CLOSED on our side**

**Reconciled 2026-08-09: nothing outstanding here is ours.** The ruling below was
relayed, the `speeds` span was answered from source, and the two pieces of work
it created have both landed — `ACCUDISC_ERR_NOT_BLANK` (0.4.0, 2026-07-29) and
the last unbound calls (`probe_speed_ladder` and `write`, both bound 2026-07-29,
device-verified). What remains is cdda2img completing their migration, which is
theirs. Kept in full rather than summarised because the ruling itself is the
durable part and was mis-relayed once already.

> **SETTLED BY KEITH 2026-07-27, and it is not what §106.2 relayed.** cdda2img
> reported "the CLI is retired", subprocess and fallback gone. The actual
> decision is narrower and points the other way: **the CLI stays. What is
> deprecated is cdda2img's use of it.** Verbatim: *"The whole purpose of the API
> is that all consumers use it exclusively. The CLI is our consumer of the API.
> Everyone else creates their own. There's little point in having an API if
> nobody uses it."*
>
> So the CLI is the **reference consumer** — our own proof that the public
> header is sufficient — not a supported integration surface for other tools.
> This *strengthens* `CLAUDE.md`'s existing line (the CLI uses the public header
> only, and its exit codes / `--progress-fd` / stderr conventions stay there by
> design, API_PLAN §3) rather than overturning it.
>
> **What actually changes:** binding the last three calls is required work —
> cdda2img can no longer shell out. **What does not:** the CLI is not going
> away, so their §106.5 fear is unfounded. `binding_ab.py`'s A side survives as
> a *test* instrument even once production use is deprecated; byte-level
> cross-transport comparison is theirs to keep or spend deliberately, not
> something removal takes from them.

Answered from source as §by; all three folded into their plan (§107.2).

- **The `speeds` span** they must reproduce is `cli/main.c:659-664` — `lba =
  leadout_lba / 4`, `count` clamped to `leadout_lba / 2` (middle half,
  representative CAV radius), clamp skipped when `--start` is explicit; rungs
  `{52,48,40,32,24,16,8,4}` filtered to the page-2A max. It is **derived per
  disc**; they had half-decided to record a constant from one Tracy run, which
  would silently mean a different radius on every disc of another length.

- ~~**`ACCUDISC_ERR_NOT_BLANK = -13`**~~ — **LANDED 2026-07-29, 0.4.0**, approved
  by Keith after cdda2img (§120.1) showed the deferral's precondition was about
  to be false: `result=not_blank` was a CLI token, and it was the insurance
  against the census-vs-construction gap, so retiring the CLI cashed it in.
  Three places as costed, plus `strerror`, the binding's `NotBlank` class and
  the cdef. `NotBlank` is a **sibling** of `Unsupported`, not a subclass —
  subclassing would keep `except Unsupported` catching a not-blank disc, i.e.
  backward compatibility bought by preserving the bug.

  Bumped to **0.4.0 with no struct change**, deliberately: a consumer that maps
  error codes to user actions is as broken by a silently changed *meaning* as by
  a moved field, and the version is the only signal it gets.

  The original entry, kept because the reasoning is the reusable part:

  Today
  `ACCUDISC_ERR_UNSUPPORTED` doubles as "disc is not blank". Verified this is
  *exact*: `src/write/burn.c:155` is the only `ERR_UNSUPPORTED` reachable from
  `accudisc_write`, and `src/write/` calls nothing from the driver/plan/session
  layers where the others live. But it is exact **by census, not by
  construction** — any future `ERR_UNSUPPORTED` under `adsc_write_run` silently
  joins "not blank", and the failure is well-formed on both sides: we would tell
  a user to insert a blank disc they are already holding, and neither suite would
  catch it. `-12` is `ERR_ABI`, so `-13` is free. Touches three places:
  `burn.c:155`, the CLI branch at `cli/main.c:946`, the contract at
  `accudisc.h:194`. Their `write_disc` is last in their own ordering, so this is
  not on the critical path.

- ~~**`read_to_file`'s docstring gives advice its own audience may not take**~~
  — **CLOSED 2026-07-29 by measurement (cdda2img §115).** The docstring is
  rewritten as a cost statement, and **the whole-disc entry point is decided:
  do not build one.**

  A/B/A, Tracy, whole disc (162892 sectors), req=40, pcm + c2 + raw sub on
  both arms, `accudisc 0.3.0` at both ends:

  | arm | wall clock | rate |
  |---|---|---|
  | A1 subprocess | 116.43 s | 18.65x |
  | B **binding** | 112.69 s | 19.27x |
  | A2 subprocess | 112.75 s | 19.26x |

  **0.06 s over 112 s against a 3.68 s subprocess-vs-subprocess noise floor.**
  It lands on the faster side, which is a rounding error with a sign rather
  than a result. Page 2A read identically before every arm, so all three ran
  on the same rung. PCM and C2 byte-identical between the binding and the
  *adjacent* subprocess arm over 383 MB and 48 MB; A1, the cold first pass,
  differs from both — which is "the disc warmed up", not "the transport is
  broken". Only `A1 == A2 != B` would have indicted the carrier.

  **Both of the old docstring's claims were wrong, and differently so.** It
  said the CLI "writes the file inside the library's address space" —
  `read_sink()` (`cli/main.c:1000`) also loops per sector and `fwrite`s, so
  the library never writes the file either way. And it called the cost "small",
  which was a guess nobody had numbered. Note the §110 pre-screen and this run
  answered *different* questions — 273x headroom said the sink is not
  intrinsically too slow; this says the nonlinear failure that harness could
  not see (callback falls behind, cache drains, read collapses to a
  seek-per-chunk crawl) does not occur here. Only the second was worth acting
  on.

  Scope, stated because the table reads as more general than it is: **one
  drive, one disc, one speed.** Not a claim about a 48x drive on a clean
  pressing.

- **The single-spin sequence test — `[P2]`, THEY ASKED FOR IT (§107.2), NOT
  BUILT.** Q3 dissolved: there is no cross-call lead-in cache *and no inline
  mechanism to lose*. `--fulltoc`'s "single-spin capture" is two ordinary public
  calls (`accudisc_read_full_toc`, `accudisc_read_cdtext`) made before
  `accudisc_read` on the same open handle (`cli/main.c:1354-1368`); the "one
  spin-up instead of three" is a property of **one process holding the handle
  open**, not of anything the library combines. `Device.__init__` calls
  `accudisc_open` and nothing else (verified), so a binding consumer holding one
  `Device` across the same three calls in the same order issues an identical
  command sequence. What the test must pin is the property that **has no other
  way to fail**: a consumer that opens/closes between the calls gets every byte
  correct and three spin-ups, and nothing on either side goes red. Assert the
  command sequence on one handle, lead-in before audio.

  **§109.4 confirms they want it and are pinning the other half.** They will
  pin the one-`Device` discipline on their side; we pin the command sequence
  on ours. Their framing is worth keeping: the restructuring of
  `read_disc_c2` from one CLI call into three binding calls *is* the
  migration, and it is the one part where getting it wrong "produces a
  correct image, three spin-ups and nothing red".

### 10. cdda2img §109 — `accudisc_write_opts` has the same defect, older and on the destructive path — **DONE 2026-07-29**

> **Landed.** `uint32_t size` leads the struct, `ACCUDISC_WRITE_OPTS_INIT`
> sets it, `accudisc_write` runs `adsc_abi_import` **before the device
> check** — mirroring `accudisc_read_cdda` (`engine.c:355`) for the two
> reasons that code states: a stale binding is diagnosed as ERR_ABI
> ("rebuild") rather than ERR_INVAL ("fix your arguments"), and the guard
> stays reachable without a drive.
>
> **The field landed in padding, so `sizeof` did not move (24 bytes before
> and after) — and that is a hazard, not a saving.** It means an
> already-compiled caller still type-checks and still passes 24 bytes. What
> saves it is that those bytes now start with `simulate`, which is 0 or 1,
> and both are below `sizeof(uint32_t)` and refused. Old callers fail loudly
> on the first call. `tests/test_abi.c` drives *both* values rather than
> leaving that to be inferred from the layout, and pins
> `offsetof(cdtext_path) == 16` so a later field that does move things is
> noticed by the assertion instead of by a burn.
>
> **The guard was made to fail before it was trusted:** replacing the
> negotiation with a plain `memcpy` failed exactly the four refusal
> assertions and left the acceptance ones passing — the correct signature,
> since a bypass accepts everything. Restored, 33/33.
>
> API_PLAN §8 row 7. Man page `accudisc.8` updated (the struct, the macro,
> and `ACCUDISC_ERR_ABI` in the return list).

<details><summary>original entry</summary>

Raised by cdda2img while reading our tree, and it is a fair catch. We held
`accudisc_probe_speed_ladder` unbound to keep the `accudisc_speed_rung` ABI
window open (task 6, spent 2026-07-28) — and while in the header they
checked which structs carry the §7.1 `size` guard:

| struct | `size` field | direction |
|---|---|---|
| `accudisc_read_req` | yes | caller → library |
| `accudisc_read_stats` | yes | library → caller |
| `accudisc_speed_rung` | **no** | library → caller |
| `accudisc_write_opts` | **no** | caller → library |

**The two unguarded ones fail differently, and `write_opts` is the worse.**
`speed_rung` is an OUT array — a stride mismatch corrupts everything past
element 0, loudly and probably immediately. `write_opts` is an IN struct:
growing it means the library reads *past the end* of a short struct the
caller passed. Quiet, and on the burn path.

Their case, checked against our header and correct on both points:
`accudisc_write_opts` is labelled *"Provisional API — the write engine is
young; fields may grow"*, and **it has already grown once** — `cdtext_path`
was appended, its safety resting on "zero-init callers get NULL and the
prior behaviour unchanged". That reassurance holds only while every consumer
recompiles against the current header, and a Python binding shipping a built
`.so` is precisely the consumer for whom it stops holding. It is also last
in the binding queue, so on current sequencing the unguarded struct sits
exposed longest while the one guarded by omission got fixed first.

**Their ask: add `uint32_t size` to `accudisc_write_opts` before
`accudisc_write` is bound.** Same one-line fix already applied twice
(`src/abi.c`, IN rule: short zero-extends, long accepted only if every byte
past our end is zero, zero always refused). Measured today at 24 bytes;
check whether the field lands in existing padding as it did for `read_req`.

Not done here because it is a separate change from the `speeds` work and
deserves its own commit and its own device-free accept/refuse test — but it
is genuinely inside the same window, and the window is closing.

- **Also from §109, no action needed, worth keeping:** their general rule for
  the rest of the migration — *"where the CLI publishes a scalar, ask what
  struct it was computed from, because the binding consumer gets the
  struct."* Derived from finding that `read`'s exit 3 is not carried by the
  library at all but computed by the CLI from `accudisc_read_stats`
  (`hard_errors || sectors_suspect || sectors_flagged`), so there is nothing
  for us to surface. Same shape as §by.1's `speeds` span being a function of
  `leadout_lba` rather than a constant — twice in one migration.
- **§109.3 corrects our `read_to_file` docstring on the facts, not just the
  advice** — folded into the `[P3]` item in task 9. The docstring says the
  CLI "writes the file inside the library's address space"; `read_sink()`
  (`cli/main.c:1000`) also loops per sector and `fwrite`s, so the library
  never writes the file either way. The real delta is a C callback versus a
  Python callback per chunk — bounded, not architectural. Verify before
  rewriting.
- **The wall-clock number will arrive as A/B/A on one disc**, not one rip
  each, with engine-reported throughput alongside wall clock. Their
  reasoning: two ~95 s whole-disc rips vary run-to-run by more than the
  quantity being measured. Still the gate on any library-side whole-disc
  entry point — build nothing before it lands.

</details>

## Deferred (explicitly, by user decision)

- Python / Rust bindings (generated against `include/accudisc/*.h` only).
  **Reopened 2026-07-25** — see "Library API completion" above for the
  prerequisites. **No longer deferred as of 2026-07-27: the Python binding is
  built** (task 6 above). Rust is still deferred and inherits the Python
  answers, per §7.3's "Python first, then Rust" — including the two hazards
  §7.2 missed, of which one (`catch_unwind` across `extern "C"`) it did name.
- Man page (must mirror `docs/reference/ATTRIBUTION.md`).
  **No longer deferred — first cut written 2026-07-26:** `docs/man/accudisc.1`
  (CLI) and `docs/man/accudisc.8` (library/API).

  **Correction 2026-08-09 — the next two clauses used to say "both are untracked
  and neither is installed by CMake yet", and both halves are false.** They are
  tracked (`git ls-files docs/man/` returns `accudisc.1`, `accudisc.8` and
  `.gitkeep`) and they are installed (`CMakeLists.txt`, `install(FILES
  docs/man/accudisc.1 …man1)` and the `.8` into `man8`). The line was written
  when the pages were new and was never revisited; nothing was wrong with the
  work, only with this record of it.

  **Genuinely outstanding against this entry, and it is the whole of it:** the
  pages do not mirror `ATTRIBUTION.md`. They credit nothing beyond the MIT
  statement, so the reference-source credits — QPxTool, cdrtools, REDUMP,
  PlexTools-derived ATIP codes — still need adding. That obligation is the
  reason this entry named the man page in the first place.
- Write / burn (DAO) path — paused; do not start without direction.
  **This line is stale as written.** The DAO path shipped and is
  hardware-verified (`src/write/`, `RECORDING_PLAN.md` §9 phases 1–2 and §11.7/
  §11.8). Left in place rather than deleted because the *decision* it records —
  do not extend the write path without direction — still stands, and RECORDING_
  PLAN §9 phases 3–5 (`--sub` passthrough, vendor write features) are genuinely
  not started.
