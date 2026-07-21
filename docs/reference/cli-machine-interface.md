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

Exception: `features` keeps its frozen contract — exit **0 iff C2 is clearly
usable**, 1 otherwise.

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
speed req=<x> page2a=<x> measured=<x.xx>
```

`req` = the setting asked for, `page2a` = what mode page 2A reports after
the set (0 = unavailable), `measured` = achieved rate from a timed
streaming read at that rung's own cache-fresh window. `measured` is the
ground truth; `page2a` is shown so quantization/clamping is visible (e.g.
a drive snapping req=16 to 8). Measured rates are radius-dependent on CAV
drives (default probe location: the middle half of the disc). The drive is
left at the last candidate tested. Rungs with equal `measured` are one
rung for ladder purposes.

## `write` output

DAO audio burn from a cdrdao `.toc` + raw BIN. `--simulate` runs the whole path
with the laser off (test-write); requires a blank disc and an O_RDWR open.

- **stderr**: human `\rwriting <done>/<total> (…%)` line — not a stable
  interface.
- **stdout**: final `write done sectors=<n> mode=<simulate|burn>`.
- **`--progress-fd N`**: machine tokens, throttled — `progress <done> <total>`
  lines plus a final `summary sectors=<n> mode=<simulate|burn> result=ok`.
- **exit**: 0 done; 1 usage / missing `--toc`/`--bin`; 2 transport failure;
  **3 disc is not blank**.

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

One line per track, then lead-out, then one acquisition-path line:

```
track <n> lba <lba> sectors <n> audio|data
leadout lba <lba>
source=<fulltoc|toc> degrade=<reason> pregaps=none [sessions=<a>..<b>] [disc_type=0x<hh>]
```

The `track`/`leadout` lines are frozen in this form. `lba` and `sectors` are
decimal; `sectors` runs to the next track's start, and for the last track to
lead-out. The final line is `key=value` tokens — **parse it as tokens, not
positionally**; new keys may be appended.

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

### `pregaps=` — always `none` from this subcommand

**INDEX 00 exists only in the program-area Q subchannel, never in the lead-in.**
Neither READ TOC format carries pregap data, so a successful `source=fulltoc`
supplies no more of it than a degraded `source=toc` does. The key is present so
that callers branch on the token rather than on `source=`, and so a future
program-area-derived value is additive. Pregaps come from the `pregaps`
subcommand (CRC-gated Q decode) or from a raw subchannel capture.

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
