# Plextor consumer features — opcode binding progress (PX-716A)

**Licensing: MIT** (same as the core; opcodes are hardware facts — see
`LICENSE.md`). Lives in the driver zone for architectural vendor-isolation, not
licensing. See `PROTOCOL.md` for the detailed protocol notes. Feature names are
taken from the PlexTools / PX-716 user manual.

**This file is indexed by consumer FEATURE.** For the opcode-first view — every
opcode the firmware implements, whether AccuDisc issues it, and whether it is
tested hardware-free and on hardware — see
[`../../docs/reference/OPCODES.md`](../../docs/reference/OPCODES.md).

Legend — **Identified**: opcode + CDB framing pinned. **Working**: exchange
live-verified on the PX-716A via raw SG_IO (`re-tools/sgsend.c`). ☑ = done,
☒ = not yet, ◐ = partially (see note). "GET-verified" means the read path was
confirmed returning coherent state; write-time *effects* (density, laser power,
book type) are not observable without a burn — the write/burn path is paused.

| # | Feature (manual) | Opcode / page | Identified | Working | Notes |
|---|------------------|---------------|:---------:|:-------:|-------|
| 1 | **SpeedRead** (uncap CD read speed) | `0xE9` MODE, page `0xBB` | ☑ | ☑ | **Fully verified**: SET ON flips mode-page-2A max read speed 40×→48× (7056→8467 kB/s); SET OFF restores 40×. Value at CDB[3], state echoed at resp[2]. Page 2A reports the REQUEST, not the governed rate — see below. |

### The mechanism below is REFUTED. The measurement is kept; the explanation was wrong.

**Keith's ruling, 2026-08-09**, on the drive he owns and against the PX-716
manual (p.15, three published ceilings by media class: DATA 48×, **CD-DA and
CD-R audio 40×**, CD-RW audio 32×):

> SpeedRead is for CD/DVD-ROM only. The hardware is physically incapable of
> reading CD-DA above 40×, and the governor ignores the SpeedRead setting for
> CD-DA entirely. The page-2A reading shows the request, not the
> governor-controlled throughput. **The Q corruption below was measured at 40×,
> not 48×.**

So the table that follows is a real, back-to-back, whole-disc A/B and stays on
the record — but the *cause* assigned to it in the next paragraph cannot be
right, because the state it blames (a 48× CAV RPM on an audio disc) is not a
state this drive can enter. Both arms ran at the same governed speed. Whatever
separated them, it was not SpeedRead raising the rate.

Further, cdda2img subsequently measured Q degradation directly and found it to
be **a property of the disc, not of the speed**. Two other confounds were
already recorded against this A/B and still stand: n=1 per arm, on damaged
media, taken before drive contention was known to produce the same signature
(see [[drive-contention-flock]] — a second process on the drive collapses Q
while audio stays clean, which is exactly the shape of the ON row).

**No further speed testing is to be done on this question** (Keith, same
ruling): higher speeds meaning more Q misreads is already common knowledge, and
the remaining questions are not worth the drive time.

### The measurement, kept as data (session 4, live)

Measured on the PX-716A reading ABBA *Gold* whole-disc (`read --start 0 --sub
raw`), SpeedRead ON vs OFF, same command, same ~24.2× average, back-to-back:

| SpeedRead | total Q-CRC ok | 0–10% | 10–60% (inner/mid) | 70–100% (outer) |
|-----------|----------------|-------|--------------------|-----------------|
| **ON**    | **40.6 %**     | 55 %  | **0.0 %** (dead)   | ~99 %           |
| **OFF**   | **99.2 %**     | 98 %  | ~99–100 %          | ~99 %           |

**Cause (REFUTED — see the ruling above; kept because it is what we believed and
why, and the reasoning shows where it went wrong).** SpeedRead pins the drive's
CAV RPM to its 48×-outer target across the
whole disc. On inner tracks the linear velocity is far below what that RPM
implies and the subchannel channel-clock cannot track it, so Q decodes to
garbage; the outer tracks (linear speed matches RPM) stay clean. The **audio
main channel is unaffected** (0 hard errors, 0 C2 both runs) — the damage is
Q-only, and silent. An *isolated* read of an inner LBA is clean even at
`--speed 40`, because the drive then spins only as fast as that radius needs;
the corruption requires the sustained high RPM of a full-disc SpeedRead pass.

**~~Rule for the read engine: never enable SpeedRead when `--sub` is
requested.~~ WITHDRAWN 0.6.0.** This rule was enforced in three places — a
library refusal (`ACCUDISC_ERR_UNSAFE_COMBINATION`), a CLI interlock on
`--uncap --sub`, and a pre-read warning. All were removed with the mechanism
that justified them. `--uncap` with `--sub` is now an ordinary combination.

**Relation to the cdda2img 47 % Q loss (their §9) — NOT established.** Their
incident predates AccuDisc's SpeedRead support, so SpeedRead cannot have been
the cause *unless* another tool (e.g. PlexTools) had left the persistent bit
on. Against that: a whole-disc read here with SpeedRead **off** was 99.2 %
clean — i.e. this test does **not** reproduce their 47 % in their nominal
config. So their cause is still open. The one suggestive link is the *pattern*:
their missing pre-gaps (tracks 5/6/7/9) are inner/mid, matching the inner dead
zone SpeedRead produces — which is a reason to check whether SpeedRead (or any
high-inner-RPM condition) was in fact active, not proof that it was. Q-health
counters in the read summary are needed to settle it by re-running the rip with
SpeedRead verified off.
| 2 | **Write Strategy / AutoStrategy** | `0xE4` read / `0xE5` write | ☑ | ◐ | GET-verified: AutoStrategy currently ON (resp[2]&0x0F=1). Enable/disable = `0xE4` CDB[2]=`0x10\|state`. Strategy DB read `0xE4` CDB[1]=0x02 CDB[2]=0x03; custom strategy push = `0xE5`. Manual write-strategy needs AutoStrategy OFF. Effects need a burn. |
| 3 | **SecuRec** (disc password lock) | state `0xE9` page `0xD5`; set `0xD5` SEND_AUTH | ☑ | ◐ | State GET-verified: not protected (resp[3]=0). Password load = opcode `0xD5`, 16-byte WRITE `[00][len][14×passwd]`, CDB[2]=01 CDB[3]=01 CDB[4]=02 CDB[10]=0x10. OFF = `0xD5` with no data. Drive-enforced read-lock (auth handshake `0xD4`/`0xD5`), **not** container encryption. Not burn-tested. |
| 4 | **GigaRec** (CD-R density 0.6–1.4×) | `0xE9` MODE, page `0x04` | ☑ | ◐ | GET-verified (off / 1.0×). **Corrects session-2: page is 0x04, not 0x06.** Rate at resp[3], disc-rate resp[4]. Rate table validated (see PROTOCOL.md). SET is write-time; effect needs a burn. |
| 5 | **VariRec** (manual laser power) | `0xE9` MODE, page `0x02` | ☑ | ◐ | GET-verified (off). CD: CDB[3]=`0x02\|disc_type`; resp[2]=state, resp[3]=power, resp[5]=strategy. DVD variant same page, disc_type bit. Effect needs a burn. |
| 6 | **SilentMode** (speed/noise caps) | `0xE9` MODE, pages `0x06`/`0x07`/`0x08` | ☑ | ☑ | GET-verified: main page 0x08 returns full settings block (`08 06 00 04 08 00 19 0d`). Disc=0x06, Tray=0x07, Main=0x08. Read/write toggles. |
| 7 | **Single Session / Hide CD-R** | `0xE9` MODE, page `0x01` | ☑ | ☑ | GET-verified (off). resp[2] bit0=single-session, bit1=hide-CD-R. SET value = `2*hide + ss` at CDB[3]. |
| 8 | **Book Type / bitset** (DVD±R) | `0xE9` MODE, page `0x22` | ☑ | ☑ | GET-verified (resp[2]=1). Per-disc-type book-type override for DVD compatibility. |
| 9 | **Test Write / simulation** (DVD+) | `0xE9` MODE, page `0x21` | ☑ | ☑ | GET-verified (off). |
| 10 | **PoweRec** (optimal write power) | `0xED` (MODE2) | ☑ | ☑ | GET-verified: ON, recommended-speed field = `ntoh16(resp[4..5])`. CDB[1]=00 GET, CDB[2]=00, len at CDB[9]=0x08. |
| 11 | **Q-Check** (C1/C2/PI-PO/jitter/beta) | `0xEA` | ☑ | ☑ | Already implemented in `plextor.c` (subcmds 0x15/0x16/0x17). The one shipping feature. |

## Not consumer features (present in the opcode inventory; noted for safety)

| Opcode | Meaning | Caution |
|--------|---------|---------|
| `0xEE` | **Drive reset / reboot** (no data) | Do **not** send casually — resets the drive. |
| `0xD4` / `0xD5` | GET_AUTH / SEND_AUTH | SecuRec + PX-755/760 auth handshake. |
| `0xE3` | PlexEraser | Destructive media erase. Never probe live. |
| `0xEB` | Speed LIST readout (cdrecord `get_speeds_plextor`) | Read-only status. **Not POWEREC** — see 0xED. This row previously said "PoweRec transfer-rate / recommended speed readout", which conflated two commands; corrected 2026-08-28 when the governor was implemented. |
| `0xED` | "Drive mode 2" — 8-byte state block, mode code 0 = **POWEREC**, the automatic WRITE-speed governor. GET is data-IN 8 B (`resp[2]` bit 0 = on, `resp[4..5]` = recommended kB/s BE); SET carries one bit in CDB byte 1 (`0x10` off / `0x11` on) with the mode code at byte 2 and NO data transfer. | Implemented 0.32.0, verified both directions on a PX-716A. A wrong CDB layout is refused 5/24 rather than silently ignored. |
| `0xF1` | EEPROM read (TLA etc.) | PX-716 reportedly rejects the TLA form. |
| `0xF3` / `0xF5` | FE/TE (focus/tracking error) scan + readout | Diagnostic. |
| `0xD8` | READ CD-DA (classic raw audio) | Not needed; all five `0xBE` combos work in the core. |
| `0xDE` `0xDF` `0xE1` `0xE2` | unmapped | Not exposed by QPxTool or the manual; internal/DVD/HDD (0xDF is model-gated to PX-PH2 external HDD). Not CD-DA consumer features — left unmapped. |

## Cross-validation

The whole table was confirmed three independent ways: (1) PlexTools static RE
(sessions 1–2, opcode inventory + helper structure); (2) QPxTool source
(pages + CDB framing); (3) the live PX-716A — both raw SG_IO (`sgsend`) and
QPxTool's own `cdvdcontrol -c`, whose reported states match the raw reads
exactly (SpeedRead OFF, PoweRec ON, GigaRec OFF, SecuRec OFF, AutoStrategy
AUTO[1], TestWrite OFF, …).

## The 0xE9 MODE command (verified model)

```
CDB:  E9  DIR  PAGE  VAL  ..  ..  ..  ..  ..  L9  L10  ..
       0   1    2     3                      9  10
```

- **DIR** (CDB[1]): `0x00` = GET (read current), `0x10` = SET.
- **PAGE** (CDB[2]): feature page (table above).
- **VAL** (CDB[3..]): value(s) to set (GET leaves 0).
- **Length**: an 8-byte page; drive returns a fixed 8-byte block. Framing puts
  `0x08` at CDB[10] for most pages, CDB[9] for SS/Hide and PoweRec — the drive
  is lenient about which. Always an 8-byte **data-in**, even for SET (the drive
  echoes the resulting state).
- **Response**: `resp[0]` = page echo, **`resp[1]` = `0x06` constant header**
  (this is the byte session-2 misread as "page 6"), `resp[2..]` = state/values.

Provenance: opcode/page constants and CDB framing cross-referenced against
QPxTool (GPL) — see `../../docs/reference/ATTRIBUTION.md` — and independently
live-verified on the user's own PX-716A. See `PROTOCOL.md` for the full trace.

## Coverage audit — enumerated from the *documented* side inward (2026-08-31)

The table above was built from the **opcode** side: PlexTools RE gave an opcode
inventory, QPxTool gave pages and framing, the drive confirmed. Asking "is the
table complete?" against that table is circular — it can only report the
features we already had opcodes for. So this section enumerates the other
direction: every feature the **vendor documents**, checked for an opcode.

Two primary sources, neither previously mined for this:

- `private/drives/Plextor/Plextor-716.pdf` — the PX-716 manual. Its §5 contents
  list *is* the documented feature set for this drive.
- `private/drives/Plextor/PTPXL/Help/PTPXLEN.chm` — PlexTools Professional XL
  3.x help (2007). Broader than the PX-716: it also covers PX-755/760 (
  PlexEraser), Blu-ray drives, and TV-tuner/video-capture hardware. Those rows
  are excluded here — counting them would inflate the gap list with things that
  were never PX-716 commands.

> **Extraction trap, and it fails silently.** Both the CHM's HTML and the
> manual's text are **CP1252**, not UTF-8. GNU grep in a UTF-8 locale skips
> lines carrying invalid multibyte sequences, so `grep -i speedread
> DriveSettings.html` returns **nothing** on a file that plainly contains
> "Enable SpeedRead". A first pass here concluded SpeedRead, Silent Mode and
> SecuRec were *undocumented*, all three false, with no error printed.
> `iconv -f CP1252 -t UTF-8` first, or `grep -a`; and treat any negative taken
> over these files without that step as void.

### The audit

☑ = opcode pinned. ⚠ = standard MMC, present on this drive, never explicitly
bound to the feature name. — = not a drive command at all.

| Documented feature | Source | Opcode / page | |
|---|---|---|:-:|
| CD / DVD Read Speed Setting | both | `0xBB` SET CD SPEED, `0xB6` SET STREAMING | ☑ |
| Spindown Time | both | MODE page `0x0D` CD Device Parameters | ⚠ |
| Audio Output Settings (volume) | CHM | MODE page `0x0E` CD Audio Control | ⚠ |
| Buffer Underrun Proof | both | MODE page `0x05` Write Parameters (BUFE) | ⚠ |
| Auto Insert Notification, DMA | CHM | Windows registry, host-side | — |
| PoweRec | both | `0xED` | ☑ |
| Single Session / Hide CD-R | CHM | `0xE9` page `0x01` | ☑ |
| SpeedRead | both | `0xE9` page `0xBB` | ☑ |
| BookType (+R, +R DL) | CHM only [^bt] | `0xE9` page `0x22` | ☑ |
| AUTOSTRATEGY (4 modes) | both | `0xE4` / `0xE5` | ☑ |
| **Media Quality Check** | CHM | **`0xE4` CDB[1]=`0x01`, CDB[2]=mode** — closed below | ☑ |
| VariRec | both | `0xE9` page `0x02` | ☑ |
| GigaRec | both | `0xE9` page `0x04` | ☑ |
| Silent Mode settings | both | `0xE9` pages `0x06`/`0x07`/`0x08` | ☑ |
| **Silent Mode "Save Changes To Drive" / "Reset values"** | CHM | **not an opcode — CDB[3] bit 1** — closed below | ☑ |
| SecuRec | both | `0xD4` / `0xD5` | ☑ |
| Q-Check C1/C2/CU | both | `0xEA` | ☑ |
| Q-Check PI/PO | both | `0xEA` | ☑ |
| **Q-Check Jitter/Beta** | both | **`0xEA`** — corrected below | ☑ |
| Q-Check FE/TE | both | `0xF3` scan + `0xF5` readout | ☑ |
| TA Test (Time Analyzer) | both | `0xF3` + a PX-716-specific histogram build | ☑ |
| Read / Write Transfer Rate test | both | ordinary reads/writes, host-timed | — |
| Erase Disc (Quick / Full) | CHM | `0xA1` BLANK | ☑ |
| **Overburn** | manual (p.6, p.68) | no dedicated command — the host writes past the ATIP-declared capacity and the drive permits it | ☑ |
| DVD Region setting / changes left | manual | `0xA4` REPORT KEY | ☑ |
| Firmware upgrade | manual | `0x3B` WRITE BUFFER mode 5 — **never issue** | ☑ |
| Audio read/write offset (displayed) | CHM | app-side table, not read from the drive | — |
| Self-Test Diagnostics | manual §6 | **hardware-triggered** — see below | — |
| Emergency eject, front-panel colour | manual | mechanical | — |
| PlexEraser | CHM (PX-755/760) | `0xE3` | n/a |

[^bt]: **Corrected 2026-08-31** (cdda2img's check, reproduced here). This row
    first said "both". The PX-716 manual has **zero** hits for `book type`,
    `BookType`, `bitset` or `bit setting`; all 12 `Book` occurrences are colour
    books or "the CD book standard". Their own caveat — that `pdftotext` can
    line-wrap a two-word phrase past a two-word grep — was closed here by
    re-searching the newline-joined text: still zero. The `0xE9` page `0x22`
    binding is unaffected; only the provenance was wrong.

**Overburn was missed by the first pass of this audit.** It is on the manual's
own p.6 feature list ("Overburn: Another way of burning more information onto a
CD") and gets its own paragraph on p.68 explicitly distinguishing it from
GigaRec. It is a *permission*, not a command: no opcode enables it, the host
simply writes beyond the disc's stated capacity. Recorded so the row is not
re-discovered as a gap. Found by cdda2img sweeping the feature lexicon inward,
a third enumeration direction — see their
`2026-08-31-px716-lexicon-capability-map.md`.

**PlexEraser is not a PX-716 feature.** Zero occurrences in the PX-716 manual;
the CHM scopes it "Only available for PX-755 Series / PX-760 Series". It stays
on the DANGER list, but as a cross-model hazard, not a gap in our coverage.

### The three gaps, closed

**Media Quality Check** is an AUTOSTRATEGY subcommand, not a new opcode:
`0xE4` CDB[1]=`0x01`, CDB[2]=mode, no data transfer; the drive then goes
BUSY and the host polls TEST UNIT READY until it clears; the result is read
back with `0xE4` CDB[1]=`0x01`, CDB[10]=`0x12` (18 bytes). QPxTool
`lib/qpxplextor/plextor_features.cpp:1137-1180`, `plextor_media_check()`,
which also states **DVD media only**. We already had that builder in the
static inventory as `e4 01 …… 12` and simply never labelled it.

**Silent Mode persistence** is a *bit*, not a command. Both setters take a
`permanent` flag encoded as `CDB[3] = disc_type | 2*!!permanent`
(`:381-414`, `plextor_set_silentmode_tray/_disc`). "Reset values" is
`plextor_set_silentmode_disable()` — the same setters with defaults and the
same flag. So `0xE9` pages `0x06`/`0x07` already cover it; row 6 of the table
above was simply incomplete, not wrong.

**Q-Check Jitter/Beta is `0xEA`, not `0xF3`.** `cmd_cd_jb_init` /
`cmd_dvd_jb_init` / `cmd_jb_getdata` (`plugins/plextor/qscan_cmd.cpp:125,140,
381`) all issue `PLEXTOR_QCHECK`. `0xF3` is FE/TE (`:158,236`) and TA
(`:660-682`); `0xF5` is the FE/TE readout (`:524`). Worth stating explicitly
because the earlier Gemini-claim refutation cited `0xF3`/`0xF5` as "the
Beta/Jitter pair" — the *refutation* stands (`0xF4` is neither), but that
supporting detail was wrong, and `qscan_cmd.cpp` names the functions plainly.

### What the audit means for 0xD9 / 0xF2 / 0xF4

**Every documented feature now has an opcode, and none of them is
`0xD9`, `0xF2` or `0xF4`.** That is a real constraint rather than an absence
of evidence, because of how the PlexTools harvest was scoped: it enumerated
all **120 call sites** of the single SCSI issue helper `fcn 0x47b240` — the
application's entire SCSI vocabulary, not a traced subset — and PlexTools is
the vehicle through which every feature in the table above is exercised.
`0xD9`/`0xF2`/`0xF4` appear at none of them.

So the inversion resolves in the opposite direction to the one hoped for: they
are almost certainly **not** documented features at all — service, factory or
internal commands. Their remaining scope is bounded by the two assumptions the
harvest rests on: that `fcn 0x47b240` is the only path to the SPTI wrapper at
`0x47aa30`, and that the stack tracker resolved CDB[0] at every site.

The corollary is the useful lead. If any documented behaviour *is* still
unbound, it must live in `0xDE`, `0xDF`, `0xE1` or `0xE2` — opcodes PlexTools
**does** issue and whose feature binding was never pinned (see PROTOCOL.md,
"The 0xDF mode-set family": four builders, one per selector byte, "the shape
expected of the SpeedRead / SilentMode / single-session / book-type group").
That is static RE on `PTPXL.exe` with **zero drive risk**, unlike every
remaining route into `0xD9`/`0xF2`/`0xF4`.

### What this audit does NOT cover, and one conflation it could invite

This file's scope is **Plextor vendor features** — that is what the table is
for, and standard-MMC capabilities live in the core, not here. But scope is
only safe when it is stated, so:

**`READ CD` C2 error pointers are not the same thing as Q-Check C1/C2, and the
audit row above covers only the latter.** Every one of the manual's 25 "C2"
occurrences is Q-Check C1/C2/CU — the media-quality *scan*, `0xEA`. The C2
pointer flags returned alongside audio data by `READ CD` are a different
mechanism entirely, they are what secure ripping actually consumes, and they
are probed in the core by `ok_c2` / `ok_c2_sub_raw`
(`src/drive/features.c:154,169`). Neither enumeration reaches them, because
neither the vendor manual nor the vendor opcode inventory is where they live.
Anyone reading the Q-Check row as "C2 is covered" would be wrong.

**Accurate Stream** is in the same position: probed by
`accudisc_probe_accurate_stream` (`src/drive/features.c:93`), absent from every
Plextor enumeration. It is a read-only capability bit in mode page `0x2A` — a
capability, not a toggle. Confirmed against our licensed MMC-5 copy, Annex E.11:
the MM Capabilities and Mechanical Status page "is read only", legacy, most
recently defined in MMC-3. So there is nothing here for a vendor opcode to set.

Both were surfaced by cdda2img enumerating a **third** way — inward from the
cross-vendor feature lexicon (`private/research/incoming/2026-07-26-optical-
drive-feature-lexicon.md`). That direction reaches standards and generic terms
that a vendor marketing manual structurally cannot document, which is exactly
why it found what the other two passes could not. The same sweep also corrected
two rows above and one row in the lexicon itself ("MediaLock" is QPxTool's
section heading, not Plextor's word for `PREVENT ALLOW MEDIUM REMOVAL`).

Also outside this audit but documented by Plextor and served by standards
commands: subchannel P–W, ISRC/MCN, ATIP, DAE, DAO/SAO/TAO, packet writing,
High/Ultra Speed CD-RW.

### Terminology — the manual's own words differ from ours

Recorded because the audit's premise was "what the vendor documents", and this
table has been using the PlexTools tab labels throughout. Counts are from the
PX-716 manual:

| this table says | the manual says | |
|---|---|---|
| SecuRec | **SecureRecording** | 25 vs 2 — "SecuRec" is only the PlexTools tab |
| AutoStrategy | **AUTOSTRATEGY** (all caps) | 12 |
| SilentMode | **Silent Mode** (two words) | 37 vs 0 |
| — | **Buffer Underrun Proof Technology** | 13; the manual never uses Sanyo's "BURN-Proof" brand |

The opcode bindings are unaffected; this is naming only. Left as-is in the rows
above rather than mass-renamed, since QPxTool and our own driver use the
compact forms — but the manual's spelling is what to search for when mining it.
