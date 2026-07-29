# The accudisc CLI machine interface

The contract for programs driving `accudisc` as a subprocess. Everything in
this document is a **stable interface**: changes are additive only. Anything
*not* listed here — in particular the human-readable stderr output (progress
line, summary block, log messages) — may change at any time and must not be
parsed.

## Exit codes (all subcommands)

| code | meaning |
|------|---------|
| 0 | completed, no caveats |
| 1 | usage / argument / local file error |
| 2 | fatal: device, transport, or the command could not complete |
| 3 | completed with caveats (non-fatal, see below) |

Exit 3 per subcommand:

- `read`: the image was delivered in full, but `hard > 0`, `suspect > 0`, or
  C2-flagged sectors remain after recovery. Exit 0 means no *relative* signal
  fired — it is **not** verification; absolute gates (AccurateRip/CTDB) are
  the caller's job.
- `cdtext`, `fulltoc`: the drive answered but the disc has no data of that
  format. No output file is written. A drive that *rejects* the request
  (CHECK CONDITION) is exit 2 — deliberately not conflated with absence.
- `scan`: neither an MCN nor any ISRC was found.
- `pregaps`: at least one track boundary decoded as `UNKNOWN` — the approach to
  it was too damaged to place INDEX 00, so the pregap is undetermined rather
  than absent. Every other boundary in the listing is still valid.

  **`pregaps` is also the one place exit 2 can carry usable stdout.** A boundary
  whose *read* fails (as opposed to decoding as `UNKNOWN`) ends the scan, and
  since 2026-07-25 the boundaries decoded before it are printed rather than
  discarded — stdout is flushed ahead of the stderr diagnostic so the ordering
  is stable. Everywhere else exit 2 means "assume nothing was produced"; here a
  row count from a nonzero exit is a **floor, not a total**. Parse the rows,
  then let the exit code decide whether the listing is complete.
- `c2lag`: the probe ran but was **inconclusive** — either no C2 fired anywhere
  in the span, or too few rereads disagreed to place the lag. Not a failure and
  not a measurement; re-run over a damaged span.
- `media`: the disc carries no ATIP (a pressed disc, or no recordable medium).
  `atip absent` is printed to stdout.
- `write`: the burn completed but a caveat was logged — today, CD-Text
  `SIZE_INFO` disagreeing with the `.toc`. `result=caveats` on `--progress-fd`.
- `disc`: the disc was classified successfully and `kind=NEITHER` — neither
  ripping nor burning is legal for it. Refusing is a *successful*
  classification, which is why it is 3 and not 2. The `reason=` slug says which
  refusal; see the `disc` section below for the precedence and the full list.

Exception: `features` keeps its frozen contract — exit **0 iff C2 is clearly
usable**, 1 otherwise.

## Exit codes → library semantics

For binding authors. A library returns errors; it does not exit, and it has no
`--progress-fd` (API_PLAN §3). What a binding must reproduce is the **decision**
each exit code encodes, from values the library already returns — not the exit
code itself, and never by shelling out to compare.

Every row below was read out of `cli/main.c`, not inferred from this document.

| exit | CLI subcommand | the condition, in library terms |
|------|----------------|---------------------------------|
| 3 | `read` | `accudisc_read_stats`: `hard_errors \|\| sectors_suspect \|\| sectors_flagged` — the same three fields, no threshold |
| 3 | `cdtext` | `accudisc_read_cdtext` → `ACCUDISC_ERR_NOTFOUND` |
| 3 | `fulltoc` | `accudisc_read_full_toc` → `ACCUDISC_ERR_NOTFOUND` |
| 3 | `read --cdtext F` / `--fulltoc F` | same two calls, `ERR_NOTFOUND`; **non-fatal to the read** — the inline capture notes the absence and the rip continues |
| 3 | `scan` | `accudisc_scan_mcn` *and* every `accudisc_scan_isrc` returned `ERR_NOTFOUND` — the caveat is "nothing at all", not "something missing". Where to scan is part of it: MCN at `toc.tracks[0].lba`, ISRC at each track's own `lba`, and **audio tracks only** (`ACCUDISC_TRACK_IS_AUDIO`) |
| 3 | `pregaps` | any entry in the `accudisc_index_map` array has `pregap_state == ACCUDISC_PREGAP_UNKNOWN` |
| 3 | `c2lag` | `accudisc_probe_c2_lag` → `ERR_NOTFOUND`. The struct is still filled, so `sectors_active == 0` distinguishes "clean span" from "C2 seen but inconclusive" |
| 3 | `media` | `accudisc_read_atip` → `ERR_NOTFOUND` |
| 3 | `write` | `accudisc_write` returned `ACCUDISC_WROTE_WITH_CAVEATS` (a **positive** return, not an error) |
| 2 | any | any negative `accudisc_err` from a device call. The CLI adds no judgement here — it prints and exits |
| 1 | any | **no library equivalent.** Argument and local-file validation happen before any device call; in-process that is the caller's own code |
| 0 | any | none of the above |

Three properties of that table are load-bearing:

- **`ERR_NOTFOUND` is the caveat code throughout.** It means the drive answered
  and the data legitimately is not there. A drive that *rejects* the command
  returns `ACCUDISC_ERR_SENSE`, which is exit 2. Conflating them turns "this
  disc has no CD-Text" into "this drive cannot read CD-Text", and a binding that
  maps every non-`OK` to one exception loses exactly that distinction.
- **`write`'s caveat is a positive return.** `rc < 0` is an error, `rc > 0` is
  `ACCUDISC_WROTE_WITH_CAVEATS`, `rc == 0` is clean — so a binding testing
  `if rc:` treats a successful burn as a failure, and one testing `if rc < 0:`
  silently drops the caveat. Both compile and neither is caught by a type check.
  `accudisc.h` says the same thing at the declaration ("Test with `rc > 0`, not
  `rc != ACCUDISC_OK`"); it is repeated here because this is the document a
  binding author reads when deciding what an exit code *meant*.
- **Exit 3 is never a *relative* verification pass.** `read` exiting 0 means no
  C2, suspect or hard-error signal fired — it does not mean the audio is
  correct. The absolute gates (AccurateRip, CTDB) live in the calling
  application and this contract says nothing about them.
- **The `read` caveat verdict is the CONSUMER's to re-derive, and that is a
  ruling, not an omission** (Keith, 2026-07-29). The library deliberately does
  **not** export a `read_verdict()` helper or a verdict field. Every API
  consumer computes `hard_errors || sectors_suspect || sectors_flagged` from
  `accudisc_read_stats` itself, exactly as `cli/main.c` does.

  The alternative — library-side, one authority, CLI consumes it — was
  recommended by both this project and cdda2img and was **not** chosen. It is
  recorded here so nobody re-opens it as an oversight.

  What the ruling obliges us to: **those three fields are contract.** Their
  names, their types and *what feeds them* are a stability promise, not
  internal accounting, because a consumer that never sees an exit code derives
  a user-visible verdict from them. Changing what counts as `sectors_suspect`
  is a semantic break even though nothing in the struct moves — the same class
  of break as `ERR_NOT_BLANK`, and it takes a version bump for the same reason.
  `accudisc.h` repeats this beside the fields, because this document's audience
  is shrinking to CLI consumers while the fields' audience is not.

## `features` output (stdout)

Newline-delimited `key value` / `combo <name> ok|failed` lines. Frozen keys:
`cd_read_feature`, `combo c2`, `combo sub_raw`, `combo sub_q`,
`combo c2+sub_raw`, `combo c2+sub_q`, `verdict`,
`accurate_stream yes|no|unknown`.

## `read --progress-fd N`

Newline-delimited machine tokens on caller-supplied fd `N` (unaffected by
`-q`, which mutes only the human line):

```
progress <done> <total>
summary hard=<n> c2=<n> recovered=<n> suspect=<n> rereads=<n> slips=<n>
```

`progress` lines are throttled (roughly 4/s); the final one always reports
`<total> <total>`. `summary` is emitted exactly once, after a completed read
(clean or degraded); if the process dies or exits 2 there is no summary.
Counter meanings: `hard` = zero-filled unreadable sectors, `c2` = sectors
whose delivered copy still carries fired C2 bits, `recovered`/`suspect` =
consensus outcomes, `rereads` = extra per-sector reads issued, `slips` =
positioning-slip detections. New `key=value` pairs may be appended to the
summary line; parse it as tokens, not positionally.

## `read --map-file F`

`F` is created (truncated) as **exactly `count` bytes, one status byte per
sector** of the requested span, in span order (byte `i` = LBA `start + i`).
There is no header; the file size is the sector count and the caller knows
the start LBA. The byte encoding is the public ABI in `accudisc.h`:
`ACCUDISC_MAP_STATE()` (low nibble: PENDING 0, OK 1, C2 2, HARD 3,
RECOVERED 4, SUSPECT 5) and `ACCUDISC_MAP_SEVERITY()` (high nibble).

The engine updates the bytes in place with single-byte atomic stores through
a `MAP_SHARED` mapping, so another process can `mmap` the file read-only and
watch state live (same-machine page-cache coherence; no polling syscalls
needed beyond the mmap). The file persists after exit for post-mortem reads.
A byte is written once per sector classification; PENDING (0) means not yet
attempted. Progress = count of non-PENDING bytes.

Python reader:

```python
import mmap
with open("status.map", "rb") as f:
    m = mmap.mmap(f.fileno(), 0, prot=mmap.PROT_READ)
state = m[i] & 0x0F        # sector start_lba + i
severity = m[i] >> 4
```

## `c2lag` output (stdout)

One `key=value` token line on success (parse tokens, not positions; new
keys may be appended):

```
c2lag pairs=<L> peak=<milli> runner=<milli> flags=<n> diffs=<n> active=<n>
```

`pairs` is the drive's C2-bitmap/audio lag in sample pairs — sign
convention: a fired bit at bitmap position `i` describes audio byte
`i - 4*pairs` (positive = bitmap trails the audio). `peak`/`runner` are the
flag/instability agreement (‰) at the winning shift and at the best other
shift; the agreement is against a *proxy* oracle (reread instability), so
absolute values run well below database-oracle precision — a verdict is
only printed when the peak dominates every other shift (3× contrast) on
top of evidence floors, so any success line is already unambiguous.
`active` = C2-firing sectors found in the scan pass. Exit 3 = inconclusive
(the stderr message distinguishes "no C2 fired in the span" from "C2 seen
but evidence too thin"); the probe needs damaged media where flags fire,
under streaming reads at a speed where the defect is marginal.
Report-only: AccuDisc never applies the lag to delivered bitmaps.
Validated on the PX-716A: `pairs=2`, matching the oracle-based
measurement of the same drive by cdda2img.

## `speeds` output (stdout)

One line per candidate rung (tokens; new keys may be appended):

```
speed req=<x> page2a=<x> measured=<x.xx> [min=<x.xx> max=<x.xx>]
```

`req` = the setting asked for, `page2a` = what mode page 2A reports after
the set (0 = unavailable), `measured` = achieved rate from a timed
streaming read at that rung's own cache-fresh window. `measured` is the
ground truth; `page2a` is shown so quantization/clamping is visible (e.g.
a drive snapping req=16 to 8). Measured rates are radius-dependent on CAV
drives (default probe location: the middle half of the disc). The drive is
left at the last candidate tested. Rungs with equal `measured` are one
rung for ladder purposes.

`min`/`max` are appended by `--sweep` (added 2026-07-28), which times each
rung once in each third of the probed span instead of once overall. Under
this flag the span defaults to the whole disc, which is what makes those
thirds inner/middle/outer; `--start` narrows it and the bands become thirds
of the narrowed span, noted on stderr. Three points that parsers must not
get wrong:

- **`measured` is unchanged, in both senses.** Under `--sweep` it reports
  the *middle* band — the same quantity it has always reported, not the
  mean of the three. Appending keys is within this document's stability
  guarantee; silently redefining `measured` would not be, so it was not
  done.
- **`min`/`max` appear per line, not per invocation.** A rung whose bands
  did not all produce a measurement omits them. Absence means "no gradient
  obtained", which a printed `0.00` would have made indistinguishable from
  a stalled rung. Parse them as optional.
- **Compare `min`/`max` within a rung, never across rungs.** The rungs sit
  at different radii, so cross-rung comparison carries a radius term that
  runs against the fast rungs under the default descending ladder. The
  within-rung spread is the sound one: equal band spacing, identical timed
  length, one speed setting.

Exit status is unaffected. `--sweep` costs 3x the probe time and needs a
span large enough for every rung in every band; too many rungs in too small
a span exits 2 with a stderr message rather than measuring overlapping
windows.

### The admitted ladder (`--sweep` only, added 2026-07-28)

`--sweep` also appends `verdict=` per rung and emits one summary line:

```
speed req=<x> … verdict=admitted
speed req=<x> … verdict=duplicate:<x>
speed req=<x> … verdict=quantized:<x>
ladder admitted=<x>,<x>,…
```

Page 2A advertises *settings*; this says which of them are real, distinct
rungs on this drive **and this disc**. `quantized:N` means the drive itself
reported a lower speed than requested (page 2A came back at N) — exact, no
measurement involved. `duplicate:N` means the rung measured no faster than
admitted rung N once the radius term is discounted. `admitted` means it
measured materially faster than the next lower admitted rung.

- **`ladder admitted=` is the line to consume.** Same order as the input
  ladder, so the default comes back fastest-first, ready to step down. It
  prints `none` if nothing was admitted; it is **absent entirely** without
  `--sweep`, as are all `verdict=` tokens, because a verdict from point
  samples is a guess. Do not treat a missing ladder line as an empty ladder.
- **Verdicts are per disc, not per drive.** A rung admitted on a short disc
  may be unreachable on a longer one, and media whose rate falls off toward
  the outer edge (observed on a CD-R here) can invalidate a rung admitted
  mid-disc. Probe every disc; never cache a ladder across discs.
- **Report-only.** All candidate rungs are still printed in the order given,
  including duplicates and quantized ones. AccuDisc never rewrites a
  caller's ladder from this — `accudisc_read_req.speed_ladder` is untouched.

## `write` output

DAO audio burn from a cdrdao `.toc` + raw BIN. `--simulate` runs the whole path
with the laser off (test-write); requires a blank disc and an O_RDWR open.

- **stderr**: human `\rwriting <done>/<total> (…%)` line — not a stable
  interface.
- **stdout**: final `write done sectors=<n> mode=<simulate|burn>`, with
  ` caveats=1` appended when the burn completed with a caveat.
- **`--progress-fd N`**: machine tokens, throttled — `progress <done> <total>`
  lines plus a final `summary sectors=<n> mode=<simulate|burn> result=<r>`. The
  `summary` is emitted on **every** outcome (unlike `read`), so a caller can key
  on `result=` rather than the exit code or stderr. `result` is one of:
  - `ok` — clean burn (exit 0);
  - `caveats` — burn completed but see the log, e.g. the CD-Text SIZE_INFO
    disagreed with the `.toc` (exit 3); the disc **was** written;
  - `not_blank` — the disc was not blank; nothing written (exit 2);
  - `error` — a transport/device/local failure; nothing usable written (exit 2).
  `sectors` is the count actually written (0 for `not_blank`).
- **exit**: 0 done; 1 usage / missing `--toc`/`--bin`; 2 fatal (disc not blank,
  or transport/device failure — could not complete); 3 completed with caveats.
  Exit 2 covers both not-blank and other failures; **use `result=not_blank` to
  tell them apart**, not the stderr text.

`done`/`total` are sectors; `total` is the lead-out LBA (sum of track lengths).
`--byteswap` swaps each 16-bit audio sample before writing (audio byte order is
drive-specific — the PX-716A advertises SWABAUDIO; settle empirically by
read-back before trusting a real burn).

## `media` output (stdout)

One line, from the disc ATIP (READ TOC/PMA/ATIP format 4; non-destructive):

```
atip leadin=<mm:ss:ff> leadout=<mm:ss:ff> type=<CD-R|CD-RW> manufacturer=<name>
```

`leadin` is the ATIP lead-in start = the manufacturer identification code;
`leadout` is the last-possible lead-out start = capacity; `type` is the ATIP
disc-type bit. **`manufacturer=` is the last field and its value may contain
spaces** (parse it as rest-of-line); it is empty when the code is not in the
built-in catalog — the `leadin` code is always authoritative and present.
Lookup keys on `sec` + frame-decade, so `97:24:01` resolves the `97:24:00`
entry. A disc with no ATIP (pressed CD, or no recordable media) prints
`atip absent` and exits **3** (data absent, not an error). All fields are the
raw ATIP as the disc encodes them; AccuDisc reports, it does not judge.

## `disc` output (stdout)

Pre-flight guard: which of burn / rip is legal for the loaded disc. One line:

```
disc kind=<BLANK|AUDIO|NEITHER> profile=0x<nnnn> disc_status=<0|1|2|-1> \
     erasable=<0|1|-1> audio_tracks=<n> data_tracks=<n> reason=<slug> [tray=<state>]
```

Token-primary — **parse tokens, not positions**; new keys may be appended.
`disc_status` is 0 empty / 1 incomplete (open session) / 2 complete (closed);
`erasable` is 1 for CD-RW. Both are **`-1` when not obtainable** (no medium, or
the command failed) — never silently 0, since 0 is itself a meaningful status.

| exit | meaning |
|---|---|
| 0 | actionable — `kind=` is `AUDIO` or `BLANK`; branch on the token |
| 3 | classified, but neither path is legal (`kind=NEITHER`) |
| 2 | could not classify (transport failure) |

Refusing is a *successful* classification, hence exit 3 rather than 2.

### `kind=` and its precedence

Evaluated in this order; the order is load-bearing:

1. **no medium** → `NEITHER`, `reason=no_medium`. Short-circuits everything.
2. **not a CD profile** (not 0x08/0x09/0x0A) → `NEITHER`,
   `reason=not_cd_profile`. Checked before the track census, so a nonsense
   count cannot promote a DVD.
3. **AUDIO** — at least one audio track (CTRL bit 2 clear). Pure CD-DA and the
   audio half of Mixed Mode alike.
4. **BLANK** — profile 0x09/0x0A *and* `disc_status=0`.
5. otherwise **NEITHER**, with a slug naming which refusal.

**AUDIO outranks BLANK deliberately.** An audio CD-R written but left
appendable is rippable, not blank; the reverse ordering would offer to burn
over a disc that has music on it.

**The profile gate precedes the census for the same reason.** Non-CD media do
answer READ TOC — a DVD-R measured here returns one data track — so without the
gate a DVD would be classified from its track census, and one whose CTRL bits
read as audio could reach the CD-DA rip path.

### `reason=` slugs

| slug | `kind` | meaning |
|---|---|---|
| `audio` | AUDIO | at least one audio track |
| `blank` | BLANK | recordable, no session started |
| `data_cd` | NEITHER | CD with tracks, none of them audio |
| `closed_data` | NEITHER | as above, and the disc is closed |
| `appendable` | NEITHER | open session, nothing rippable yet |
| `no_medium` | NEITHER | no disc loaded — see `tray=` |
| `not_cd_profile` | NEITHER | DVD, BD, or unrecognised medium |
| `unreadable` | NEITHER | profile says CD, but nothing could be read |

### `tray=`

Emitted **only** with `reason=no_medium`, from the sense qualifier on ASC 0x3A:
`open` (tray out), `closed` (tray shut, no disc), or `unknown` (the drive did
not say). It distinguishes "insert a disc" from "close the tray".

### Scope

Deliberately no filesystem inspection. This answers "rippable CD-DA / blank to
burn / neither" and nothing finer: it does not distinguish CD-ROM layouts,
recognise DVD or BD beyond `not_cd_profile`, or identify data-CD contents.
Non-destructive — every command is a read.

## `toc` output (stdout)

One line per track, then one line per session, then lead-out, then one
acquisition-path line:

```
track <n> lba <lba> sectors <n> audio|data [session <n>] [pregap <n>]
session <n> tracks <first>-<last> audio <n> data <n> leadout <lba>
leadout lba <lba>
source=<fulltoc|toc> degrade=<reason> subq_indices=none [sessions=<a>..<b>] [disc_type=0x<hh>] session_count=<n> [anomalies=<slug>[,<slug>...] [toc_trusted=0]]
```

`pregap <n>` is **appended** to the track line, after `session` when both are
present, and only when non-zero. It is the count of sectors immediately before
this track's `lba` (INDEX 01) that belong to the track — ECMA-130 §20 makes a
Pause part of the track that follows it. It is TOC-derivable only for the first
track of the first session, where the program area's start at LBA 0 fixes the
other edge; it is 0 (and the token absent) everywhere else, because per-track
INDEX 00 lives in the subchannel, which no READ TOC format carries. So `lba`
still marks INDEX 01 and never moves; the track's full extent is
`[lba - pregap, lba + sectors)`.

`pregap <n>` counts SECTORS; `subq_indices=` describes ACQUISITION. They answer
different questions and can never contradict each other — a disc may perfectly
well report `pregap 33` on track 1 while `subq_indices=none` says this command
did not scan the subchannel.

> **Renamed 2026-07-25: `pregaps=` → `subq_indices=`.** The old spelling shared
> a stem with the per-track `pregap <n>` field while describing a different
> axis, so real output like
> `track 1 lba 33 ... pregap 33` alongside `pregaps=none` read as a
> self-contradiction. This is the one **non-additive** change this document has
> taken; it was made while the token was still a constant with no known
> consumer. The value set is unchanged (`none`).

The first five fields of `track`, and the `leadout` line, are frozen in this
form. `lba` and `sectors` are decimal. `session <n>` is **appended** to the
track line, never inserted, and is present only when session structure is
known (`source=fulltoc`). The `session` summary lines likewise appear only
then; a parser that ignores unknown leading keywords is unaffected by their
absence. The final line is `key=value` tokens — **parse it as tokens, not
positionally**; new keys may be appended.

## `read --cdg FILE`

Writes the CD+G pack stream: R-W de-interleaved and Reed-Solomon corrected,
**24 bytes per pack, one byte per 6-bit symbol**, which is the `.cdg` file
format. Requires `--sub raw` (R-W only survives in the raw 96-byte subcode
form; the deinterleaved Q form has discarded those channels), and exits 1
otherwise.

Pack count is `4 * sectors - 7`. The de-interleave is convolutional with an
8-pack span, so the first 7 packs cannot be assembled and the loss lands at the
**tail** of the stream, not the head — emitted pack *k* is logical pack *k*.

A summary goes to stderr unless `-q`:

```
accudisc cdg summary
  packs written    : <n>
  graphics / zero  : <n> / <n>
  RS repaired      : <n> symbols (P <n>, Q <n>)
  RS gave up       : <n> packs (P <n>, Q <n>)
```

`graphics` counts packs in a GRAPHICS mode (MODE 1); `zero` counts MODE 0,
meaning no R-W data recorded. **An ordinary audio CD yields all zero**, and the
summary says so explicitly rather than leaving 0 graphics packs to look like a
decoder fault.

`RS repaired` counts symbols recovered by the two Reed-Solomon codes — (24,20)
across the pack, and (4,2) over symbols 0-3. `RS gave up` counts packs beyond
correction capacity; those packs are still written, with their damage reported
here, and are **never interpolated**.

### `anomalies=` — the lead-in contradicts itself

**Absent entirely on a well-formed disc**, so parsers may treat its presence as
the signal. When present it is a comma-separated list of stable slugs naming
structural defects found while parsing the lead-in.

These exist because copy-protection schemes work by *deliberately* malforming
the TOC (Kaspersky, *CD Cracking Uncovered*, ch. 6–7). The design rule is that
such a disc fails **informatively**: the failure to avoid is not a crash but a
silent "helpful" normalisation, because the audio-range guard would then vet a
map that does not describe the disc and hand back an authoritative-looking
wrong answer.

| slug | meaning |
|---|---|
| `lba_order` | track numbers ascend but their addresses do not |
| `overlap` | two tracks claim the same sector |
| `leadout_before` | the lead-out points at or before the first track start ("castrated lead-out") |
| `past_leadout` | a track starts at or beyond the lead-out; it owns no sector |
| `empty_track` | a track with a zero-length extent (Red Book's minimum is 300 sectors) |
| `negative_lba` | a track point addressed before LBA 0; the point is dropped |
| `bad_track_num` | an A0/A1 pointer naming a track outside 1–99 |
| `range_mismatch` | A0/A1 disagree with the track points actually present |
| `bad_session` | an entry claiming a session outside 1–99; dropped |

`toc_trusted=0` is appended when any of `lba_order`, `overlap` or
`leadout_before` is set. Those three mean the track map cannot be relied on to
say which sectors are audio, so `accudisc_check_audio_range()` refuses any
range outright with reason `toc_untrusted` rather than vetting against it.
The remaining slugs are reported only — the map still describes the disc.

`--force` remains the caller's deliberate way past.

### `sectors` is bounded by the session, not by the next track

`sectors` runs to the next track **in the same session**, and for a session's
last track to **that session's lead-out** — never across a session boundary.

This matters on any multi-session disc. Between one session's last track and
the next session's first track sit that session's lead-out, the next session's
lead-in, and a pregap: no payload, and unreadable as CD-DA. Measured on a
PX-716A (2026-07-22, Enhanced CD): session 1 ends with track 13 at LBA 184300
and a lead-out at 195656, while session 2's track 14 starts at 207056. Track 13
is therefore **11356** sectors, not the 22756 that "distance to the next track
start" would give — 11,400 sectors of difference, all of it unreadable.

On the `source=toc` degrade path there is no session structure, so every track
reports no `session` field and `sectors` reduces to the next track's start.
Callers must treat that geometry as untrustworthy on any disc carrying a data
track; `accudisc read` refuses such ranges (see `no_session_info` below).

### `source=` — which READ TOC format answered

`fulltoc` (format 0x02) or `toc` (format 0x00). These are different physical
operations, not two views of one thing: format 0x02 replays the raw Q-channel
of the **lead-in**, while format 0x00 returns the drive's already-decoded track
descriptors. A marginal lead-in can therefore fail 0x02 outright while 0x00
answers perfectly. `toc` prefers 0x02 (for session structure) and degrades to
0x00 automatically; `sessions=` and `disc_type=` appear only under
`source=fulltoc`, since format 0x00 does not carry them.

### `degrade=` — why it fell back (a disc-health signal)

| value | meaning |
|---|---|
| `none` | format 0x02 answered; no degrade |
| `leadin_unreadable` | 0x02 failed — transport error or CHECK CONDITION |
| `leadin_absent` | 0x02 answered "no data of this format" |
| `leadin_malformed` | 0x02 answered but the response did not parse |

`leadin_unreadable` on a disc whose program area still reads clean is a
**degradation warning about the disc**, not plumbing noise: the lead-in is
failing first. Callers archiving provenance should record it.

**Cost.** Preferring format 0x02 is free when it succeeds and is paid only on a
degrade. Measured on a PX-716A, three runs each:

| disc | before (0x00 only) | now (prefers 0x02) |
|---|---|---|
| healthy lead-in (`degrade=none`) | ~5 ms | ~5 ms |
| unreadable lead-in (`leadin_unreadable`) | ~5 ms | ~172 ms |

The extra ~166 ms is the drive giving up on the lead-in. Trivial for a one-shot
invocation, but a caller polling `toc` in a loop on a degraded disc pays it every
time.

**A degrade does not change the exit code — `toc` exits 0.** The command
promises track geometry and a degrade still delivers it in full; only the
session structure that format 0x00 never carried is missing. (Contrast
`fulltoc`, where the caller asked for the lead-in itself, so absence is exit 3.)
Making a marginal lead-in fail `toc` would regress exactly the discs this
fallback exists to serve. The health signal rides on `degrade=`, which is
strictly more informative than an exit code.

### `session_count=` — a count, and the only structure a degrade keeps

Always present. **A count, not the `sessions=<a>..<b>` range** — the two come
from different opcodes and have different availability:

| | `sessions=<a>..<b>` | `session_count=<n>` |
|---|---|---|
| source | READ TOC format 2 (the lead-in) | READ DISC INFORMATION |
| meaning | *which* sessions | *how many* sessions |
| on a degrade | absent | **present** |

READ DISC INFORMATION is answered from the drive's own disc model rather than
by re-reading the groove, so it still speaks when the lead-in will not. `0`
means nobody could say — never a guess.

This is what makes a degraded read safe to act on:

| `session_count` | consequence |
|---|---|
| `1` | fully reconstructible — one session owns every track and format 0's lead-out **is** that session's lead-out. The model is synthesised, so session selection, extents and the range guard all behave as if the lead-in had answered. |
| `>1` | `read` refuses with `session_unmapped`: the seams are known to exist and their positions are not. Format 0 returns the *last* session's lead-out, so the final track's extent is wrong. |
| `0` | unknown — the conservative all-audio walk, refusing if any data track is present (`no_session_info`). |

The `>1` case matters because a **multi-session all-audio** disc is otherwise
undetectable: nothing in a flat format-0 track list distinguishes it, and a
track census provably cannot see session boundaries.

**Verification status: confirmed on hardware** (PX-716A, 2026-07-22), both
halves. *Accuracy* — READ DISC INFORMATION independently reported 2 on an
Enhanced CD, matching the lead-in's `sessions=1..2`. *Survival of an
unreadable lead-in*, which is the premise for the `1` row — an MPO CD-R whose
lead-in does not read returned `degrade=leadin_unreadable session_count=1`,
cross-checked against READ DISC INFORMATION's first-track-in-last-session field
(1) and against libcdio's independent `Last CD Session LSN: 0`. The
reconstruction was then exercised end to end: session table synthesised on a
`source=toc` line, and `read` resolved session 1 across the whole disc.

### `subq_indices=` — always `none` from this subcommand

Reports whether **this invocation collected INDEX data from the subchannel**.
It is a statement about acquisition, not about the disc: `none` never means
"this disc has no pregaps".

**INDEX 00 exists only in the program-area Q subchannel, never in the lead-in.**
Neither READ TOC format carries it, so a successful `source=fulltoc` supplies no
more of it than a degraded `source=toc` does — hence the constant. The key is
present so that callers branch on the token rather than on `source=`, and so a
future scanned value is additive. Index/pregap data comes from the `pregaps`
subcommand (CRC-gated Q decode) or from a raw subchannel capture.

A parser must not treat `subq_indices=none` as evidence about track geometry.
The one pregap the TOC *can* prove is reported on the track line as
`pregap <n>`, and the two are independent.

## `read` session selection and the audio-range guard

`read` reads the **audio tracks of one session**, not the whole disc and not
the whole session.

With no `--start`, `--count`, `--session` or `--tracks`, it resolves the session
to read:

| situation | behaviour |
|---|---|
| exactly one session has audio tracks | that session, no argument needed |
| more than one session has audio | **exit 1**; the sessions are listed on stderr and `--session N` is required |
| no session has audio | exit 1 |
| session structure unknown (`source=toc`) | falls back to the flat whole-disc range, still vetted by the guard |

Having chosen a session, the range covers that session's **audio tracks only**,
from the first audio track's start to the end of the last — where "start"
includes that track's pregap. On session 1 the first audio track's pregap is the
program area from LBA 0, so **the default range begins at LBA 0**, not at INDEX
01, and those pre-INDEX-01 sectors (hidden-track-one audio, and load-bearing for
the disc ID) are captured. On a Mixed Mode CD —
one session holding a data track first, then audio — this is what makes the
disc rippable at all; the whole-session range would begin on the data track and
the guard would (correctly) refuse it. Verified 2026-07-22 on a Mixed Mode CD-R:
data track 1 of 138230 sectors, audio tracks 2-11, lead-out 342197, resolving to
`lba 138230 count 203967`.

If a session's audio tracks are **not contiguous** — a data track between two
runs of audio — no single range can express them, and `read` exits 1 asking for
explicit `--tracks`. Legal on the wire; not known to occur in any shipped
format; never observed on real media.

### `--track N` / `--tracks A-B`

Selects tracks by **number** (not index), inclusive, overriding `--session`.
Both spellings accept both forms, so `--track 3` and `--tracks 2-11` each read
naturally. Track *type* is not checked here — that stays with the guard below,
so there is exactly one place that decides what is rippable.

| condition | behaviour |
|---|---|
| either number absent from the disc | exit 1, reporting the range the disc does have |
| the two tracks are in different sessions | exit 1 — a span across a seam would include the lead-out and lead-in between them |
| `B < A`, or a malformed argument | exit 1 |

The resolved range is echoed to stderr (suppressed by `-q`), in one of two
forms:

```
accudisc: session <n>, lba <lba> count <n>
accudisc: tracks <a>-<b>, lba <lba> count <n>
```

Before any sector is requested, the range is checked against the TOC. A range
that is not entirely audio payload within one session is **refused with exit
1**, before the drive is touched:

```
accudisc: refusing lba <l> count <n>: <reason> at lba <lba> [(track <n>)]
accudisc: these sectors are not readable as CD-DA; --force overrides
```

`<reason>` slugs:

| slug | meaning |
|---|---|
| `data_track` | overlaps a track whose CTRL bit 2 is set — the drive rejects every sector of it as CD-DA |
| `not_in_track` | overlaps sectors owned by no track: a session's lead-out, the next lead-in, or the seam between them |
| `crosses_session` | spans two sessions — readable on both sides, a wasteland between |
| `beyond_leadout` | runs past the disc |
| `no_session_info` | session structure is unknown *and* the disc carries a data track, so the extents cannot be trusted |
| `session_unmapped` | the disc is **known** to have more than one session (`session_count>1`) but the degraded lead-in did not say which tracks belong to which |
| `empty` | `count` is 0 |
| `toc_untrusted` | the lead-in is malformed such that the track map cannot be believed — see the `anomalies=` field below. Usually copy protection |

`--force` bypasses the guard. It is deliberately **separate** from `--any`,
which only selects the READ CD expected sector type: conflating them would
make it impossible to ask for a CD-DA read of a data track in order to observe
how the drive rejects it.

### Why this is a guard and not a warning

A drive's refusal to read a data track as CD-DA is *categorical* — sense key 5,
ASC 0x64 ILLEGAL MODE FOR THIS TRACK — and identical on every attempt. The read
engine treats such a sense as terminal for that sector rather than retrying it
with a cache-defeat seek. Measured on a PX-716A (2026-07-22) over the 4129-sector
data track of an Enhanced CD: **62.9 s before, 4.9 s after**. The guard exists
so that time is not spent at all.

## `read` inline lead-in capture

`read --fulltoc FILE --cdtext FILE` dumps the raw READ TOC format-2 /
format-5 responses before the audio pass (one spin-up total). Absent CD-Text
writes no file and does not change the read's exit code. The dumps are
byte-identical to the standalone `fulltoc FILE` / `cdtext FILE` subcommands.

## Stream geometry (unchanged, frozen)

PCM 2352 B/sector raw s16le, no offset correction applied; C2 bitmap
294 B/sector passed through from the drive untouched (no realignment); raw
P–W subchannel 96 B/sector; hard-unreadable sectors zero-filled (PCM 0 /
C2 all-ones / SUB 0) so all streams stay exactly `count` sectors. `--speed`
is never auto-restored between invocations.
