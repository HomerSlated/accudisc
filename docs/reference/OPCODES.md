# Command surface — every opcode, its binding, and its test status

**Scope: the PX-716A.** The MMC opcodes are generic and AccuDisc issues them on
any drive; the *enumeration axis* — "found in the firmware" — is one drive's
flash image (fw 1.11), so absence from a table here is a statement about that
image, never about MMC.

Three questions this file exists to answer, each previously answerable only by
reading `../../drivers/plextor/PROTOCOL.md` end to end:

1. What opcodes does the firmware implement?
2. Which of them does AccuDisc actually issue?
3. Which of *those* are tested, at what level, and against what?

Companions, not duplicates:

| file | axis |
|---|---|
| `drivers/plextor/PROTOCOL.md` | **chronological** — how each opcode was found, what was retracted |
| `drivers/plextor/FEATURES.md` | **consumer feature → opcode**, from the marketing name inward |
| **this file** | **opcode → code → test**, the status matrix |

### Finding a feature by name

Several Plextor features are **not opcodes** — they are pages or sub-commands
*inside* one. Searching this file for "VariRec" used to return nothing, because
VariRec is page `0x02` of opcode `0xE9`. This index exists so a name search
lands:

| feature | it lives at | wired |
|---|---|:---:|
| SpeedRead | `0xE9` page `0xBB` | ✅ |
| PoweRec | `0xED` mode code 0 | ✅ |
| Q-Check C1/C2/CU, PI/PO, Jitter/Beta | `0xEA` sub-cmds `0x15`/`0x16`/`0x17` | ✅ |
| **VariRec** (manual laser power) | `0xE9` page `0x02` | — |
| **GigaRec** (CD-R density) | `0xE9` page `0x04` | — |
| Silent Mode | `0xE9` pages `0x06`/`0x07`/`0x08`; persistence is a CDB[3] bit | — |
| Single Session / Hide CD-R | `0xE9` page `0x01` | — |
| Book Type / bitset | `0xE9` page `0x22` | — |
| Test Write / simulation | `0xE9` page `0x21` | — |
| SecuRec | state `0xE9` page `0xD5`; set `0xD5` SEND AUTH | — |
| AutoStrategy | `0xE4` read / `0xE5` write | — |
| Media Quality Check | `0xE4` CDB[1]=`0x01` (DVD only) | — |
| Q-Check FE/TE, TA Test | `0xF3` scan + `0xF5` readout | — |
| PlexEraser | `0xE3` ⛔ | — |
| Spindown time, audio volume, Buffer Underrun Proof | standard MODE pages `0x0D`/`0x0E`/`0x05` | — |

`FEATURES.md` is the authority on the feature side and carries the manual's own
wording; this file carries the wiring and test status.

---

## The enumeration is a FLOOR, and eight opcodes prove it

Static analysis of the firmware found **42 opcodes** (33 MMC + 9 vendor) across
38 subtract-and-branch dispatch chains. That number is a **lower bound on the
command surface**, and the gap is not small.

At least **eight** opcodes are *proven implemented* — AccuDisc or an `re-tools/`
probe issues them successfully against this very drive — and appear in **no**
harvested chain:

| opcode | proven implemented by |
|---|---|
| `0x3C` READ BUFFER | `re-tools/mmcsweep.c`; buffer id 0 = 8355840 B |
| `0x43` READ TOC/PMA/ATIP | every rip this project has ever done |
| `0x51` READ DISC INFORMATION | before/after disc snapshots, blank CD-R probe |
| `0x55` MODE SELECT(10) | write-parameters page 0x05, every burn |
| `0x5D` SEND CUE SHEET | the DAO burn that passed 2026-07-24 |
| `0xB6` SET STREAMING | timed delivered-rate experiment |
| `0xBB` SET CD SPEED | same experiment; both levers equivalent |
| `0xE9` vendor MODE | SpeedRead, live-verified both directions |

`PROTOCOL.md` names four of these (`0x3C`/`0xB6`/`0xBB`/`0xE9`). **The other
four fall out of this cross-check and are recorded here for the first time** —
they were never noticed because nobody had previously laid the harvest beside
the list of opcodes our own code issues.

The harvest's near-misses make the partiality concrete: it found `0x44` READ
HEADER but not `0x43`, and `0x52` READ TRACK INFORMATION but not `0x51`.
Adjacent opcodes, one caught and one missed — which is what a partial detector
looks like, and is *not* what an arithmetic error in the chain-walker would look
like (that would miss neighbours together).

**The reason is known**, and it bounds the negative rather than leaving it open:
those opcodes are dispatched through an opcode-indexed function-pointer table
that lives in **RAM** at approximately `0x85631C`, outside the flash window
`0xF00000-0xFEFFFF`, and is *constructed at boot* rather than stored (no 1024-byte
window in the image reads as 256 in-range code addresses). Under table dispatch
the opcode is never compared against a literal, so it can appear in no chain.
Static call-graph analysis cannot complete this map; it would need the boot-time
population code or a live read of drive RAM, and the Phase 1 sweep established
there is **no exposed RAM window** (READ BUFFER id 0 is the data buffer and
nothing else exists).

So: **42 is a floor, 50 is a better floor, and neither is the surface.**

---

## Reading the columns

### Provenance — where we know it from (an opcode may have several)

| tag | meaning |
|---|---|
| `FW` | firmware dispatch harvest, PX-716A fw 1.11 |
| `PT` | PlexTools static RE — 17 vendor opcodes over 32 CDB-builder call sites |
| `PUB` | public source: QPxTool, cdrtools, `pxfw`, libcdio, FreeBSD CAM |
| `HW` | live-confirmed on this PX-716A |

This column is load-bearing. `0xEA`, `0xE9`, `0xED` and `0xF1` are **absent from
the firmware union** yet three of them are what the shipped driver uses and all
four are live-verified. Without provenance the table would read as "the firmware
does not implement Q-Check", which our own hardware runs refute.

### Wired — does AccuDisc issue it?

| tag | meaning |
|---|---|
| `core` | `src/mmc/cdb.c` builder + `src/mmc/mmc.c` wrapper + **at least one shipped consumer** |
| `drv` | issued by `drivers/plextor/plextor.c`, the dlopen'd vendor module |
| `re` | issued only by an `re-tools/` probe — **not built, not shipped, not installed** |
| `—` | not issued anywhere in this tree |

`core` deliberately requires a real consumer. A builder plus a wrapper with no
caller is *plumbing*, not a wired command, and the distinction has already
caught one dead constant (see §G).

### Fixture — hardware-free test coverage

Split into three, because they are different guarantees and collapsing them is
how a stubbed test comes to look like a verified command:

| tag | meaning |
|---|---|
| `cdb` | **byte layout** asserted in `tests/test_cdb.c` |
| `dec` | **response decode** asserted against captured or synthetic vectors |
| `pay` | **outbound payload** bytes asserted — the data the command carries, as distinct from its CDB header |
| `flow` | **sequence** asserted with the whole MMC layer stubbed (`tests/test_burn_flow.c`) — checks ORDER, never bytes |

`◐` on a `flow` entry means the stub is not the whole story — some real code is
linked in behind it, and the entry says which.

`flow` alone is not coverage of an opcode. `test_burn_flow.c` replaces six
`adsc_mmc_*` functions with fakes, so a burn-flow test passes identically whether
the real CDB is right or garbage.

### Production — graded, not a tick

"A burn passed" is real evidence, but it is not an opcode-level record, and
writing it as one would overstate what we can cite.

| grade | meaning |
|---|---|
| `observed` | a citable line naming **this opcode** against this drive |
| `exercised` | necessarily issued by a named recorded run; no opcode-level record |
| `—` | not recorded |

Two things this column refuses to merge: *issued and returned GOOD* is not *its
response was validated*. `0x5C` is the case that proves the distinction — see its
entry.

**A citation naming an `re-tools/` probe is marked †.** Those probes hand-build
their own CDBs (`mmcsweep.c:90` writes `{0x5a, 0, pg, 0,0,0,0,0, 254, 0}` directly),
so they evidence **the drive's behaviour, not our builder's bytes**. It is the same
distinction `0x5C` turns on, and applying it there but not here would be
inconsistent. Most † rows *are* additionally exercised through the library path on
every invocation; where that is the stronger evidence, the entry says so.

**Rule applied throughout: no citation ⇒ not a tick.**

---

## A. Opcodes AccuDisc issues

| op | name | prov. | wired | fixture | production |
|----|------|-------|-------|---------|------------|
| `0x12` | INQUIRY | FW PT PUB HW | core | `cdb` | observed |
| `0x1B` | START STOP UNIT | FW PUB HW | core | `cdb` | observed † |
| `0x2A` | WRITE(10) | FW | core | — | exercised |
| `0x35` | SYNCHRONIZE CACHE | FW | core | `flow` | exercised |
| `0x43` | READ TOC/PMA/ATIP | HW | core | `cdb` `dec` | observed |
| `0x46` | GET CONFIGURATION | FW HW | core | `cdb` | observed |
| `0x51` | READ DISC INFORMATION | HW | core | `dec` | observed |
| `0x54` | SEND OPC INFORMATION | FW | core | — | exercised |
| `0x55` | MODE SELECT(10) | HW | core | `cdb` | exercised |
| `0x5A` | MODE SENSE(10) | FW HW | core | `cdb` | observed |
| `0x5C` | READ BUFFER CAPACITY | FW | core | `flow` | exercised ⚠ |
| `0x5D` | SEND CUE SHEET | HW | core | `pay` | exercised |
| `0xAC` | GET PERFORMANCE | FW HW | core | `cdb` | observed † |
| `0xB6` | SET STREAMING | HW | core | `cdb` | observed |
| `0xBB` | SET CD SPEED | HW | core | `flow` ◐ | observed |
| `0xBE` | READ CD | FW HW | core | `cdb` `dec` | observed |
| `0xE9` | Plextor vendor MODE | PT PUB HW | drv | — | observed |
| `0xEA` | Plextor Q-Check | PT PUB HW | drv | — | observed |
| `0xED` | Plextor drive mode 2 | PT PUB HW | drv | — | observed |

### `0x12` INQUIRY

Standard SPC device identification: vendor, product, firmware revision. Used by
`src/device.c` on open, and it is the backbone of drive identification — the
whole quirk/offset/driver-selection chain keys off what this returns.

It doubles as the project's **positive control** for opcode probing: an
implemented opcode returns good status where an unassigned one returns
`5/20/00`. Every vendor-opcode probe in `PROTOCOL.md` runs it before and after,
which is also how the drive was shown intact after the `0xF2` incident.

*Production:* observed — used as the positive control in the `0xD9`/`0xF2`/`0xF4`
confirmation, and as the post-incident health check.

### `0x1B` START STOP UNIT

Spin up / spin down / load / eject, selected by the Start and LoEj bits.

**We issue only one form of it: spin-down (`start=0, loej=0`)**, and it is the
whole of `accudisc stop` — `cli/main.c:2668` → `accudisc_spindle_stop`
(`src/device.c:404`) → `adsc_mmc_start_stop(dev, 0, 0)`. Note that mode page
`0x0D`'s inactivity timer is **not** part of this and is not a spindown control
at all; see §E. Eject and load do *not* go through this opcode either — `src/device.c:418,425` call
`adsc_transport_eject`/`_load`, which use the kernel's `CDROMEJECT` and
`CDROMCLOSETRAY` ioctls (`src/transport/sgio.c:128,139`). Worth knowing before
anyone debugs a tray problem by looking for a CDB.

*Production:* observed † — `PROTOCOL.md` records a `START STOP UNIT` eject refused
`5/53/02` MEDIUM REMOVAL PREVENTED during the pressed-disc safety check. That is
a probe-built CDB **and** the *eject* form, which our shipped path does not use.
So the spin-down form we actually ship has **no production record at all**.

### `0x2A` WRITE(10)

The DAO payload command: `nblocks` sectors at `lba`. The lead-in gap is written
at the two's-complement of −150, i.e. the raw 32-bit LBA is handed through
unmodified rather than clamped at zero — a detail that would be very easy to
"fix" into a bug.

*Fixture:* `cdb` (added 2026-09-02; before that, **none** — the byte layout of
the command that actually puts audio on a disc was asserted nowhere
hardware-free). `test_cdb.c:test_write_path_cdbs` now pins the −150 lead-in
address as `FF FF FF 6A`, the short 15-sector remainder at −15 that lands the
gap exactly on LBA 0, the 27-sector `CHUNK` count, and the zero bytes 1/6/9 (no
FUA). What it still cannot reach is `burn.c` *walking* the gap: delete the loop
at `src/write/burn.c:536-543` and `test_cdb` stays green.

*Production:* exercised — necessarily issued by the Step D acceptance burn
(`RECORDING_PLAN.md` §11.8, PASSED 2026-07-24) and by the ABBA "Gold" burn
verified **bit-exact** on read-back (§ around line 180). Both are end-to-end
records, so the evidence is strong for the *path*; there is no opcode-level
trace of the CDB itself.

### `0x35` SYNCHRONIZE CACHE

Flush the drive's write cache. In this codebase it is also **the entire abort
path**: a failed burn is closed with one FLUSH CACHE and nothing more, which is
what both reference tools do (commit `95f1d33`, which fixed a failed burn
leaving the drive mid-session).

*Fixture:* `cdb` + `flow` — sequence position, and (2026-09-02) the CDB, whose
one substantive bit is that **Immed stays clear**: the command must not return
before the flush completes, or the abort path above reports success on an
unfinished disc.
*Production:* exercised, same runs as `0x2A`.

### `0x43` READ TOC/PMA/ATIP

Four formats in use, and they are genuinely different commands wearing one
opcode:

| format | returns | consumer |
|---|---|---|
| `0x00` | track descriptors | `src/toc/toc.c` |
| `0x02` | raw/full TOC incl. sessions | `src/toc/fulltoc.c` |
| `0x04` | ATIP — recordable pregroove info | `src/drive/media_db.c` |
| `0x05` | CD-Text packs from the lead-in | `src/meta/` |

Format 4 carries a safety role beyond metadata: **an empty ATIP is the proof a
disc is factory-pressed**, hence physically unwritable, which is the precondition
the vendor-opcode probes gate on.

*Fixture:* `cdb` (`test_cdb.c`) plus heavy `dec` coverage — `test_toc.c`
(synthetic format-0), `test_session.c`, `test_tocsrc.c`, `test_media.c` (ATIP
manufacturer lookup), `test_cdtext_blob.c` (format 5).
*Production:* observed — the ATIP read is cited in the pressed-disc safety check.
**Absent from the firmware harvest** despite being unmissably implemented.

### `0x46` GET CONFIGURATION

MMC feature enumeration. `src/drive/disc.c` uses it for the current profile
(`0x08` CD-ROM, `0x09` CD-R, `0x0A` CD-RW, DVD profiles); `src/drive/features.c`
uses the RT=10b form to ask about one feature.

*Production:* observed † — `re-tools/mmcsweep.c:113` issues the full RT=0 list and
recorded **34 features, 3 of them vendor** (`0xff00`, `0xff10`, `0xff11` — all
*write*-speed capability tables, notably **not** the read governor). Feature
`0x0107` Real-Time Streaming is CURRENT with SCS/MP2A/WSPD/SW set, i.e. the drive
itself claims both `SET CD SPEED` and `SET STREAMING` support.

### `0x51` READ DISC INFORMATION

Disc status (empty / appendable / complete), erasable flag, first/last track,
lead-in start MSF, NWA. Three consumers: `src/drive/disc.c`, `src/toc/toc.c`,
`src/write/discinfo.c`.

*Fixture:* `dec` — `test_discinfo.c` covers deriving the writable lead-in extent
from the lead-in start MSF. No `cdb` case.
*Production:* observed † for the cited record, but this opcode also runs through
our own builder on every disc operation. The before/after snapshots that proved
`0xF2`/`0xF4`
wrote nothing to a blank CD-R were `READ DISC INFORMATION` reads: disc status
`00`, NWA `ffffff6a` (LBA −150), 359335 free blocks, byte-identical across 22
executions. **Absent from the firmware harvest.**

### `0x54` SEND OPC INFORMATION

Optimum Power Calibration, DoOPC set. Run before writing so the drive picks its
laser power for the medium.

There is an open, *recorded* hypothesis attached to this opcode: simulate holds
off 13.2 s at LBA −150 where a real burn held off 8.2 s, and the suspected cause
is that `burn.c` **skips SEND OPC in simulate** (`RECORDING_PLAN.md` §9,
`LIVE_BURN_QUEUE.md` A2). Untested.

*Fixture:* `cdb` (2026-09-02). The assertion that earns its place is
**DoOPC set** — with byte 1 bit 0 clear the drive returns its existing OPC data
and calibrates nothing, succeeding either way. A silent no-op on the write
path's power calibration, and well-formed enough that nothing downstream could
reject it.
*Production:* exercised — a real burn necessarily calibrated.

### `0x55` MODE SELECT(10)

Writes mode page `0x05` (Write Parameters): write type, track mode, data block
type, session format. `src/write/wparams.c` is the only consumer.

This is the command whose *cue-sheet Data Form* counterpart caused a live bug
that CDEmu could not reproduce — a reminder that the write-parameters path is
one where the virtual target and real hardware diverge.

*Fixture:* `cdb`. *Production:* exercised. **Absent from the firmware harvest.**

### `0x5A` MODE SENSE(10)

Read mode pages. Two consumers: `src/device.c` (page `0x2A` capabilities) and
`src/write/wparams.c` (read-modify-write of page `0x05`).

*Production:* observed † for the sweep, though `src/write/wparams.c` also
read-modify-writes page `0x05` through our own builder on every burn. The sweep
(`re-tools/mmcsweep.c:90`) enumerated
**11 pages present** (`01 02 05 07 08 0d 0e 1a 1d 2a 3f`) with PC=1 changeable
masks. Two findings from that sweep belong here:

- **Page `0x2A`'s changeable mask is all zero over all 52 bytes.** That is the
  drive stating the host may alter nothing, so "read speed is not controllable
  through the mode page" is a hard negative rather than an inference.
- **Page `0x01` byte 3 is the read retry count, is host-changeable, and reads
  10.** A real exposed read-behaviour lever, relevant to the recovery ladder,
  and **currently unused by AccuDisc.**

Standing caution recorded in memory and worth repeating beside this opcode:
**never infer a delivered setting from page `0x2A` — it reports the REQUEST, not
the rate the drive is running.**

### `0x5C` READ BUFFER CAPACITY ⚠

Block=0 form (bytes, not blocks), polled by `src/write/burn.c` to watch the
drive's buffer during a burn.

**The Block=1 form is deliberately not offered**, and the reason is the exact
shape of failure this document's grading exists to prevent. `src/mmc/cdb.h`
records it: the field layout was never read out of the spec here (the extracted
table was truncated), and **on an idle drive — where blank == length — the two
candidate readings are indistinguishable**, so the "it matches" check that would
normally settle it proves only that the buffer is empty.

So the `⚠` on its production grade is precise: the command is issued and returns
good status on every burn, but **its response semantics have not been
independently validated** against a drive state that could distinguish them.

*Fixture:* `flow` — `test_burn_flow.c:150` fakes it to drive the starvation
tests, which is exactly a sequence guarantee and not a byte or semantic one.

### `0x5D` SEND CUE SHEET

The DAO cue sheet: one 8-byte entry per track/index/lead-in/lead-out, 24-bit
payload length. This is what makes DAO *disc-at-once* — the whole layout goes to
the drive before any audio does.

*Fixture:* `pay`, and strongly — `tests/test_cuesheet.c` asserts the **exact entry
sequence** for a known 2-track audio layout: MCN, lead-in, per-track pre-gap and
index 1, lead-out, each with the right CTL/ADR, track number and absolute MSF
(LBA + 150), checked byte by byte. `cuesheet.c` is linked for real rather than
stubbed, because it is pure layout arithmetic with no device in it. **The payload
is well covered; the 10-byte CDB header that carries it is not.** — **closed
2026-09-02**, `cdb` added. The header's one trap is that the length is **24-bit
at bytes 6-8**, not the usual 16-bit Group-5 slot at 7-8: get it wrong and byte
6 reads as zero, so every sheet under 64 KB still works and the failure hides
until one does not.
*Production:* exercised — Step D burn, and the CD-Text pass that came back
byte-exact on real media 2026-07-24. **Absent from the firmware harvest.**

### `0xAC` GET PERFORMANCE

Nominal performance descriptors: 8-byte header plus N × 16-byte
`{start_lba, start_kbps, end_lba, end_kbps}`.

*Production:* observed † — the sweep read the drive's **CAV curve** out of it. The
recorded limitation matters: the published curve is the *nominal* one, and
**degradation is not published**, so this cannot be used to detect a drive
slowing down on a damaged region.

### `0xB6` SET STREAMING

The read-speed lever AccuDisc actually uses, via a 28-byte performance
descriptor.

Two hard-won facts live on this opcode:

- **The parameter-length field is at CDB bytes 9-10, not 8-9.** Our earlier
  `4/1b` failures were our own bug, not a drive limitation (fixed in `1419783`);
  the PX-716A does support it. Trust schily/libcdio over recalled spec text.
- **`write_kbps` in the descriptor is not optional and zero is the worst
  value.** Measured 2026-08-28: Write Size = read rate ⇒ write speed becomes the
  read speed; Write Size = 0 ⇒ write speed jumps to the drive's **maximum**;
  Write Size = current write rate ⇒ preserved. Until 0.31.0 this mirrored the
  read rate under the comment "unused for read", so `accudisc speed N` — a
  *read*-speed command in the help, header and man page — silently retuned the
  write speed at every N, **past a ceiling the caller had deliberately left in
  place** (commit `9fbeefd`).

*Fixture:* `cdb`, including a dedicated `adsc_cdb_set_streaming_desc` case.
*Production:* observed, and **this is the one row with unambiguous library-path
evidence**: the write-speed leak was found through `accudisc speed N` itself, i.e.
through our own builder and descriptor, not a probe. Also the timed
delivered-rate experiment, which confirmed the firmware speed ladder at
`0xF65B83` on hardware.
**Absent from the firmware harvest.**

### `0xBB` SET CD SPEED

The other speed lever. Speeds are in **kB/s, not Nx** — the single most likely
way to get this command wrong — with `0xFFFF` meaning "leave alone / maximum".

`adsc_cd_speed_kbps()` converts deliberately *the way the drives do*, not the way
arithmetic does: 1x is 176.4 kB/s so the correct answer is `speed_x*1764/10`, but
cdrecord uses `speed_x*177` and explains why ("the standard is rounding the wrong
way. Fortunately rounding down is guaranteed"). A drive given 176 for 1x can read
it as slightly under 1x and refuse. We follow the reference because the drives
followed it first.

*Fixture:* `flow` ◐ — `test_burn_flow.c:167` fakes the call, including refusal
paths, and captures the `read_kbps`/`write_kbps` it was asked for. **The Nx→kB/s
conversion IS asserted**: `src/mmc/cdb.c` is linked into that test for real, and
`tests/CMakeLists.txt:93` says why — "`adsc_cd_speed_kbps` is the Nx -> kB/s
conversion the speed tests are ABOUT, so a stub would test the stub." What is
**not** asserted is the 12-byte CDB layout itself. — **closed 2026-09-02**,
`cdb` added: read speed at bytes 2-3, write speed at 4-5 with distinct values so
a mirror could not pass, rotational control at byte 1, and `0xFFFF` passing
through intact rather than being clamped.
*Production:* observed — the timed experiment established that **both exposed
levers work and are equivalent**, which is also the answer to the governor
question: `0xBB`/`0xB6` plus mode page `0x01`, and nothing else.
**Absent from the firmware harvest.**

### `0xBE` READ CD

The rip command, and the busiest opcode in the tree — five consumers
(`src/cdda/scan.c`, `src/drive/c2lag.c`, `src/drive/features.c`,
`src/drive/speeds.c`, `src/read/engine.c`).

Selects sector type, C2 error field (none / 294 B pointers / 296 B pointers +
block-error), and sub-channel (none / raw interleaved P-W 96 B / formatted Q
16 B). **All five C2/sub combinations work on this drive**, which is why the
classic vendor path `0xD8` READ CD-DA is not needed.

*Fixture:* `cdb` plus the strongest `dec` coverage in the suite —
`test_decode.c` runs against **real captured vectors** with cdda2img's decoder
as ground truth, and `test_rw.c` covers R-W de-interleave and Reed-Solomon.
*Production:* observed, continuously — every rip, every C2 experiment, the
speed ladder.

### `0xE9` Plextor vendor MODE — `drv`

Vendor get/set of small 8-byte feature pages. Verified model:

```
CDB:  E9  DIR  PAGE  VAL  ..  ..  ..  ..  ..  L9  L10  ..
       0   1    2     3                       9   10
```

`DIR` = `0x00` GET / `0x10` SET; `PAGE` selects the feature; `VAL` at CDB[3].
**Always an 8-byte data-IN, even for a SET** — the drive echoes the resulting
page, so a write reads itself back. `resp[0]` = page echo, `resp[1]` = constant
`0x06` header (the byte session 2 misread as "page 6"), `resp[2..]` = state.

**`0xE9` is a multiplexer, and most of what it reaches is unwired.** Nine feature
pages are identified with CDB framing pinned; the shipped driver uses **one**
(`PX_PAGE_SPEEDREAD`, `plextor.c:54`). Several consumer features people look for
by name live here and nowhere else — VariRec and GigaRec in particular are pages,
not opcodes:

| page | feature | wired | state read out |
|---|---|:---:|---|
| `0x01` | Single Session / Hide CD-R | — | off. `resp[2]` bit 0 = single-session, bit 1 = hide-CD-R; SET value = `2*hide + ss` at CDB[3] |
| `0x02` | **VariRec** (manual laser power) | — | off. CD: CDB[3] = `0x02 \| disc_type`; `resp[2]` state, `resp[3]` power, `resp[5]` strategy. DVD variant is the same page with the disc_type bit |
| `0x04` | **GigaRec** (CD-R density 0.6–1.4×) | — | off / 1.0×. Rate at `resp[3]`, disc-rate at `resp[4]`. **Page `0x04`, not `0x06`** — session 2 misread the constant `0x06` header as the page number |
| `0x06` | Silent Mode — Disc | — | part of the Silent Mode trio |
| `0x07` | Silent Mode — Tray | — | " |
| `0x08` | Silent Mode — Main | — | full settings block `08 06 00 04 08 00 19 0d` |
| `0x21` | Test Write / simulation (DVD+) | — | off |
| `0x22` | Book Type / bitset (DVD±R) | — | `resp[2]` = 1 |
| `0xBB` | **SpeedRead** | **✅ `drv`** | the one shipped page |
| `0xD5` | SecuRec *state* | — | not protected (`resp[3]` = 0). The *set* is a different opcode, `0xD5` SEND AUTH |

Three cautions on that table:

- **Every unwired row is GET-verified only.** The read path returns coherent
  state; write-time *effects* — density, laser power, book type — are not
  observable without a burn, so "identified" here means the exchange works, not
  that the feature was proven to do anything.
- **Silent Mode persistence is not a page and not a command.** "Save Changes To
  Drive" is a **bit**: `CDB[3] = disc_type | 2*!!permanent`. The saved state
  lives in the EEPROM, which is why `0xF1` reads it back (see §C).
- **The `0x2A` speed reading is a request, not a rate.** Standing rule: never
  infer a delivered setting from mode page `0x2A`.

*Production:* observed — SET ON flips page-2A max read 40×→48× (7056→8467 kB/s),
SET OFF restores it; cross-checked against QPxTool's own `cdvdcontrol -c`, which
reports identical states. **That record covers page `0xBB` only** — the other
nine pages were GET-verified in the same session but each is its own exchange.

**Absent from the firmware harvest** — it is table-dispatched, and `0xE9` occurs
as a compare immediate *nowhere* in the image.

*Fixture:* none. The driver is a dlopen module with no hardware-free test.

### `0xEA` Plextor Q-Check — `drv`

The error-counter scan behind PlexTools' Q-Check. Sub-commands from cdrtools
`readcd plextor_*_cx_scan`: `0x15` arm, `0x16` read interval counters (26 B),
`0x17` end. Counter block: `[12..17]` three big-endian words summing to the
interval's C1 count, `[20..21]` CU (uncorrectable), `[22..23]` C2.

**Do not conflate this with READ CD's C2 pointers.** `0xBE`'s C2 field is
per-sector error *pointers* used for rip decisions; `0xEA` is a per-interval
*census* of C1/C2/CU counts used for disc quality assessment. Different
granularity, different purpose.

**CDB[2] is a scan-type selector and we occupy one value of it** — the CD
error-counter arm. A second CD arm, **Jitter/Beta** (`CDB[2]=0x10`), is pinned in
QPxTool and unwired; see §E, which also raises an open question about the
`uncr`/`e32` offsets in the block we already decode.

*Production:* observed — validated against `readcd -cxscan`.
**Absent from the firmware harvest.**

### `0xED` Plextor drive mode 2 — `drv`

A **separate** 8-byte state block from `0xE9`'s pages, reached by a different CDB
shape. Mode code 0 carries **POWEREC**, the automatic *write*-speed governor.

- **GET:** data-IN 8 B, length at CDB[8..9], mode code at CDB[2]. `resp[2]` bit 0
  = POWEREC on; `resp[4..5]` = recommended write speed in kB/s, big-endian.
- **SET:** *no data transfer at all* — the payload is one **bit**, smuggled
  through CDB byte 1. cdrecord assigns two bitfields of that byte and both land
  inside it (`reladr` = bit 0 = the new state; `res` = bits 1..4 = `0x08`), so
  byte 1 is `0x10` for off and `0x11` for on, mode code at byte 2.

Getting that wrong is **not silent**: byte 1 = `0x08` with the value in byte 2
(the first attempt, reading `res = 0x08` as a whole byte) is refused
`5/24` INVALID FIELD IN CDB.

Implemented 0.32.0 (`bf8d61b`), both directions verified.
*Production:* observed. **Absent from the firmware harvest.**

---

## B. Firmware opcodes AccuDisc does NOT issue

All `FW` provenance. One line each — these are T10 spec commands and a paragraph
would add nothing a reader will act on. Ordered by opcode.

| op | name | note |
|----|------|------|
| `0x00` | TEST UNIT READY | Ready/not-ready poll. Used by `re-tools/` probes; not by the library. |
| `0x01` | REZERO UNIT | Legacy SCSI-1 seek-to-zero. |
| `0x03` | REQUEST SENSE | Not needed — SG_IO returns sense with the command. |
| `0x04` | FORMAT UNIT | Format writable media (DVD+RW etc.). Not a CD-DA operation. |
| `0x08` | READ(6) | Legacy short-CDB read. Superseded by `0x28`/`0xBE`. |
| `0x0A` | WRITE(6) | Legacy short-CDB write. Superseded by `0x2A`. |
| `0x1A` | MODE SENSE(6) | 6-byte form; we use the 10-byte `0x5A` throughout. |
| `0x1E` | PREVENT ALLOW MEDIUM REMOVAL | The tray lock. QPxTool's man page calls this "MediaLock", which is why it was once mis-shelved as a Plextor feature — it is plain SPC. |
| `0x28` | READ(10) | Cooked 2048-byte sector read. AccuDisc reads raw via `0xBE`. |
| `0x2E` | WRITE AND VERIFY(10) | Data-CD operation. |
| `0x3B` | **WRITE BUFFER** | ⛔ **The Plextor firmware-upload path** (`pxfw` uses it at lines 173/200). **Never issue.** See §F. |
| `0x44` | READ HEADER | Sector header — data sectors only; CD-DA has none. |
| `0x4A` | GET EVENT STATUS NOTIFICATION | Media-change/state events. Unused; a plausible future addition for tray monitoring. |
| `0x52` | READ TRACK INFORMATION | Per-track NWA/free-blocks. Used by `re-tools/` snapshots, not by the library. |
| `0x53` | RESERVE TRACK | TAO/incremental writing. AccuDisc is DAO-only. |
| `0x58` | REPAIR TRACK | Incremental-write repair. Not applicable to DAO. |
| `0x5B` | CLOSE TRACK/SESSION | Writes the lead-out and finalises. **We do not issue it, and we do not need to — ANSWERED 2026-09-03 on both status fields.** A live CD-RW DAO burn using `0x35` alone left `byte2 = 0x1E`: **Disc Status 2 (complete) AND Last Session Status 3 (complete)**, full TOC readable, lead-out placed. These are separate 2-bit fields that can disagree, so both were read (raw `0x51`; `accudisc disc` surfaces only the first). **Verified on CD-RW DAO single-session only** — CD-R lead-out behaviour differs and is unverified; re-read both fields on the next CD-R burn. The dead `cdb.h` constant was removed 2026-09-02 and nothing needs adding back. |
| `0xA1` | BLANK | Erase CD-RW/DVD-RW. Not implemented; would be needed for a CD-RW workflow. |
| `0xA8` | READ(12) | 12-byte-CDB cooked read. |
| `0xAA` | WRITE(12) | 12-byte-CDB write. |
| `0xAD` | READ DISC STRUCTURE | DVD/BD structures — out of scope (see CLAUDE.md). |
| `0xB9` | READ CD MSF | MSF-addressed sibling of `0xBE`. We address by LBA. |
| `0xBF` | SEND DISC STRUCTURE | DVD structure write — out of scope. |

Also in the union and covered in §A because we *do* issue them: `0x12 0x1B 0x2A
0x35 0x46 0x54 0x5A 0x5C 0xAC 0xBE`.

---

## C. Vendor opcodes not shipped

The PlexTools RE inventory found 17 distinct vendor opcodes over 32 CDB-builder
call sites. Those the shipped driver uses are in §A; the rest are here.

| op | name | prov. | wired | status |
|----|------|-------|-------|--------|
| `0xD4` | GET AUTH | PT PUB | — | SecuRec / PX-755-760 auth handshake. |
| `0xD5` | SEND AUTH | PT PUB | — | Pair of `0xD4`. Apparent conflicts with other tools are same-number reuse across vendors' namespaces, not contradictions. |
| `0xD8` | READ CD-DA | FW PT PUB HW | — | The classic Sony/Plextor raw audio read. **Deliberately unused** — all five `0xBE` C2/sub combinations work here, so it buys nothing. `HW`: returns `4/00/00`, i.e. implemented. |
| `0xDE` | — | PT PUB | — | Unmapped. On `pxfw`'s brute-force **blacklist** alongside `0xDF`/`0xF8`. |
| `0xDF` | mode-set family | FW PT PUB | — | Four builders, one per selector byte at CDB[8]/CDB[11] (`ec`/`f1`/`f6`/`f2`), all with `02` at CDB[3], called from one adjacent cluster of setters — i.e. one dialog exposing four related toggles. **Selector→feature binding never pinned**; the strings sit several levels up the C++ dialog hierarchy. Model-gated to the PX-PH2 external HDD in places. Blacklisted in `pxfw`. |
| `0xE1` | — | PT | — | Unmapped. Template `e1 04 ff ..`. |
| `0xE2` | — | PT | — | Unmapped. Template `e2 ..`. |
| `0xE3` | **PlexEraser** | PT PUB | — | ⛔ Destructive media erase. **Never probe live.** Not a PX-716A feature at all — zero hits in this drive's manual; it is a PX-755/760 feature. |
| `0xE4` | AutoStrategy read | FW PT PUB | — | Also a multiplexer, three builders. Enable/disable = CDB[2] = `0x10 \| state`; strategy-DB read = CDB[1]=`0x02`, CDB[2]=`0x03`. **Media Quality Check** is CDB[1]=`0x01`, CDB[2]=mode, **DVD only**, polled with TEST UNIT READY. GET-verified: AutoStrategy is currently ON (`resp[2] & 0x0F` = 1). |
| `0xE5` | AutoStrategy write | PT PUB | — | Write side of `0xE4` — pushes a custom strategy. Manual write-strategy requires AutoStrategy OFF. |
| `0xEB` | speed LIST readout | FW PT PUB | — | Read-only status (cdrecord `get_speeds_plextor`). **Not POWEREC** — that is `0xED`; the two were once conflated here and the row was corrected 2026-08-28. |
| `0xEE` | **drive reset** | PT PUB | — | ⛔ Reboots the drive. Do not send casually. |
| `0xF1` | EEPROM read | PUB HW | `re` | 4 blocks × 256 = **1024 bytes**; block 4 returns a partial. The PX-716-specific form is needed — plain `0xF1` (the TLA form) fails on this drive. `re-tools/eedump.c`. **The dump stays under git-ignored `private/`: it contains drive identity/serial data.** |
| `0xF3` | TA / FE-TE scan | FW PT PUB | — | Analogue diagnostic: focus/tracking error and tilt. Heavily used in PlexTools (6 sites). **Not** Beta/Jitter — that is `0xEA`; an earlier note here said otherwise and was corrected. |
| `0xF5` | FE-TE readout | FW PT PUB | — | Readout half of `0xF3`. Our CDB for it is **wrong** — it returns 0 bytes standalone, which is why it could not serve as a control (see §D). |
| `0xF8` | — | PUB | — | ⛔ Real Plextor opcode, function unknown, **blacklisted in `pxfw`'s brute-force prober**. Its presence on a hazard blacklist *is* the finding. |

### The EEPROM dump validates itself

Worth recording because it is a rare case of a read proving its own decoding.
QPxTool's `plextor_get_silentmode_saved()` reads offsets `0x100`-`0x108`. Applied
to our own 1 KB `0xF1` dump, bytes `0x100`-`0x105` = `00 00 28 10 30 10`, which
decodes to **40× CD read, 16× DVD read, 48× CD write, 16× DVD write** — exactly
this drive's four published maxima. An independent source's offsets, applied to
our bytes, produce four numbers we already knew from the spec sheet. That is a
decode confirmed rather than assumed.

It also settles where Silent Mode persistence lives: it is a **bit in the EEPROM**
(`CDB[3] = disc_type | 2*!!permanent`), not a command.

---

## D. The three mysteries — `0xD9`, `0xF2`, `0xF4`

**All three remain of UNKNOWN function.** What follows is what was measured, not
what they do. Provenance `FW HW` for all three: predicted from the dispatch
chains, then confirmed implemented on hardware. Wired: `re` only
(`re-tools/mmcvendor.c`) — none is in the shipped driver, and `FEATURES.md` will
not gain entries for them until their semantics are established.

### Why the prediction is credible

Each sits mid-chain immediately adjacent to a *known* vendor opcode (`d8`→`d9`,
`f4`→`f5`). A chain whose cumulative arithmetic was wrong would be unlikely to
land on known vendor opcodes at all. They were then recovered **a second time**
by an independent route — scanning backwards from the 81 inbound references to
the common reject routine at `0xFDE7C5`.

### Existence, against negative controls

| opcode | sense | reading |
|---|---|---|
| `0x12` INQUIRY (**positive control**) | good status | implemented |
| `0xC1`, `0xC5` unassigned (**negative controls**) | `5/20/00` | INVALID COMMAND OPERATION CODE |
| `0xD9` | `5/64/00` | ILLEGAL MODE FOR THIS TRACK — parsed, rejected on track mode |
| `0xF2` | `2/30/05` | CANNOT WRITE MEDIUM — a write-class precondition |
| `0xF4` | `5/24/00` | INVALID FIELD IN CDB — parsed, rejected on a parameter |

The controls matter: they prove the discriminator can return either answer.

### The media sweep

| medium | `0xD9` | `0xF2` | `0xF4` |
|---|---|---|---|
| none | `2/3a/02` | `2/3a/02` | `2/3a/02` |
| pressed CD-ROM (audio) | `5/64/00` | `2/30/05` | `5/24/00` |
| CD-R, data track, appendable | `5/64/00` | *gated* | **accepted** |
| blank CD-R | `5/64/00` | **accepted** | **accepted** |
| pressed CD-ROM (data track) | `5/64/00` | — | **accepted** (bit7=1) |
| pressed DVD-ROM | `5/20/00` | **accepted** | `5/20/00` |

**Two opcodes cease to exist when a DVD is loaded.** `5/20/00` is the same sense
the negative controls return, so `0xD9` and `0xF4` are **CD-only commands**. This
is direct empirical confirmation of a static prediction: the firmware analysis had
inferred the ≥12 dispatch chains were *per-drive-state command filters* because
each is entered after a bit test on a state register. Loading a DVD makes two
opcodes disappear — exactly that behaviour, observed independently of the
disassembly.

### `0xD9` — parses, and no CD medium satisfies it

CDB field map recovered by value-sweeping:

| field | accepted | structure |
|---|---|---|
| byte 1 | `00 20 40 60 80 a0 c0 e0` | 3-bit field at bits 7:5; bits 4:0 must be 0 |
| byte 2 | `00` only | reserved |

It returned `5/64/00` for **all 8 byte-1 values on all four CD media** — pressed
audio, CD-R with a data track, blank CD-R, pressed data CD-ROM. Identical field
maps throughout.

**Stated rather than buried:** the byte-1 sweep that established the *shape* ran
on a blank disc, which has no tracks, so ILLEGAL MODE FOR THIS TRACK could not
have passed for any value. The sweep establishes byte 1's shape and says nothing
about which of its 8 values is correct.

**Retracted:** "0xD9 is a READ CD-DA MSF variant" — my own inference from chain
adjacency to `0xD8` plus a half-remembered D8=LBA/D9=MSF pairing. Refuted against
primary sources: cdrtools uses `0xD8` only (C2 is a CDB flag, not a second
opcode); libcdio's vendor-unique enum lists C4/C9/D8/DB/DF with no `0xD9`;
FreeBSD CAM has a single `0xD8` entry; redumper, QPxTool and DiscImageCreator
have nothing. **No public source associates `0xD9` with Plextor at all.**

**Two LLM attributions were checked and both refuted.** The second assigned
`0xD9` = "Vendor-Specific Subchannel / Session Interrogation" with CDB[2] as a
track/session index. Refuted from data already in `PROTOCOL.md`: `CDB[2]=0x01`
returns `5/24/00` on a disc whose track 1 exists (so byte 2 is not a track
number), and `5/64/00` is **identical** across finalised pressed multi-track
audio, pressed data CD-ROM, appendable CD-R and blank CD-R — constant across
exactly the axis the theory predicts varies.

### `0xF2` — a long, quiet physical operation ⛔

**DANGER-classed. Do not probe casually.**

Probing with non-zero CDB parameters on a DVD made the drive stop answering:
TEST UNIT READY returned `host=07` (DID_ERROR); block reads, eject and INQUIRY
all timed out while the kernel still reported the device `running`. It needed a
**power cycle**, which fully recovered it.

**The initial reading — "the drive is wedged" — was wrong**, corrected by Keith's
direct observation of the front panel: the LED blinked twice, roughly every
5 seconds, very quietly, with no spindle or seek noise. The drive was **busy
executing a long-running operation**, not crashed; the timeouts were the transport
giving up on a drive mid-command. A slow periodic double-blink with a
near-stationary disc is the signature of a **physical calibration** — laser power
measurement or servo test.

**"0xF2 requires writable media" is FALSE** — it is accepted on a pressed,
unwritable DVD-ROM. Rejected on CD-ROM, accepted on CD-R and DVD-ROM.

**"0xF2 is the firmware-upload command" is REFUTED from primary source.** QPxTool
ships `pxfw`, a Plextor *firmware* tool; `console/pxfw/pxfw.cpp` uses standard
**`0x3B` WRITE BUFFER** at lines 173 and 200 for the firmware path, plus `0xF1`
EEPROM read at line 149. It does not use `0xF2`. The drive was verified intact
afterwards: READ BUFFER capacity 8355840, page 2A max read `1b90`.

One argument previously offered for this refutation is **withdrawn**: "1 KB at
10 ms/byte ≈ 10 s, so a minutes-long run cannot be EEPROM" sized the wrong
device — a 2005 flash erase/program cycle is minutes and silent. The dispatch
grouping and media gating legs stand, and the conclusion is unchanged.

### `0xF4` — accepted, and returns nothing, ever

| byte 4 | pressed CD-ROM | CD-R |
|---|---|---|
| `0x00` (bit7=0) | `5/24/00` rejected | **accepted** |
| `0x80`-`0xff` (bit7=1) | **accepted** | **accepted** |

Byte 4 bit 7 is a **mode flag**, and the media requirement applies to only one of
the two modes. (Published earlier as "0xF4 requires recordable media" — too
strong, and withdrawn. The field map had only ever set byte 4 to `0x01`, which is
rejected in both modes.)

With bit 7 set and a **512-byte allocation**, `0xF4` returns **0 bytes** for the
baseline and for `0x08` in every one of CDB bytes 1,2,3,5,6,7,8,9,10. It is
therefore **not a data-returning read command** — it is a set / trigger / no-data
command, which independently refutes the LLM claim that it is "Data In" returning
"RF signal quality metrics".

**A test that was run and must NOT be counted as evidence:** the
trigger-then-readout hypothesis (`0xF4` arms a measurement, `0xF3`/`0xF5` read it
out) was tested by issuing `0xF5` before and after `0xF4`. Both returned 0
bytes — **but so did `0xF5` on its own**, and `0xF5` is a *known* readout command.
Its own CDB must therefore also be wrong, so the test cannot distinguish "no
change" from "my CDB is wrong". The control did not work, so the result has no
power. Recorded so it is not later mistaken for a negative finding.

### What both `0xF2` and `0xF4` write: nothing

Twenty-two executions returning good status against a blank CD-R changed
**nothing observable**: `READ DISC INFORMATION`, `READ TRACK INFORMATION` and
ATIP were byte-identical before and after, and the disc remained blank and usable.

"Requires recordable/writable media, writes nothing" is the signature of a
**calibration or measurement** command — OPC, test-write, or reading
recordable-only structures such as the PCA. The `0xF2` LED observation corroborates
that reading from an entirely independent channel.

### Why the trail stops, and where it resumes

`0xD9`/`0xF2`/`0xF4` are almost certainly **not documented features**, and this is
a *bounded* negative rather than a shrug: the PlexTools harvest enumerated all
**120 call sites** of `fcn 0x47b240` — the application's entire SCSI vocabulary —
and these three appear at none of them. Every remaining route runs through
hardware.

Three generalisations about these opcodes were withdrawn in a single session, all
the same shape: **a rule inferred from N media states, falsified at N+1.** Record
the measurement table; do not state a rule until the media axis is exhausted.

The live lead is elsewhere: **`0xDE`/`0xDF`/`0xE1`/`0xE2`** — opcodes PlexTools
*does* issue, whose feature binding was never pinned. Static RE on `PTPXL.exe`,
**zero drive risk**.

---

## E. Multiplexers — where the unexplored space actually is

`0xE9` is not special; it is just the one that was mapped. **Nine opcodes in this
document carry a selector byte, and for most of them we occupy a single value of
it.** Ranked by unexplored space against risk to the drive.

| opcode | selector | values we use | values known to exist | risk |
|---|---|:---:|---|---|
| `0x5A` MODE SENSE | page | **2** | **11 present** on this drive | none |
| `0xEA` Q-Check | CDB[2] scan type | **1** | ≥2 (`0x00` errors, `0x10` jitter/beta) | none |
| `0xED` drive mode 2 | CDB[2] mode code | **1** | unknown — no source enumerates them | low |
| `0xF3` | CDB[1]/CDB[2] | **0** | TA form pinned; a **6-site dispatcher** unmapped | vendor |
| `0xE9` vendor MODE | CDB[2] page | **1** | 10 identified of a 256-value byte | vendor |
| `0x43` READ TOC | format | 4 | 6 (formats 1, 3 unused) | none |
| `0xE4`/`0xE5` | CDB[1] + CDB[2] | 0 | 3 builders, several modes | vendor |
| `0xF1` EEPROM | CDB[1] sub-cmd | 1 (`0x01`) | unknown | low |
| `0xDF` | CDB[8]/CDB[11] | 0 | 4 selectors (`ec`/`f1`/`f6`/`f2`), **none bound** | ⛔ blacklisted |

### The three worth acting on

**`0x5A` — nine present pages we never read, at zero risk.** The sweep found
pages `01 02 05 07 08 0d 0e 1a 1d 2a 3f` present; AccuDisc reads `0x2A` and
`0x05`. This is standard MMC, read-only, no vendor opcode and no medium
precondition. Page `0x01` alone carries the host-changeable read retry count
(§G). Pages `0x0D`, `0x0E` (audio control) and `0x1D` (timeout & protect) are the
drive's own answers to questions we currently guess at.

> **Page `0x0D` is NOT the spindown control, and calling it one is a mistake this
> file made in its first draft.** MMC-3 §6.3.6 (MMC-5 declares the page legacy and
> defers to it) defines the only host-changeable field as the **Inactivity Timer
> Multiplier**: "the length of time that the Logical Unit shall remain in the
> **hold track state** after completion of a seek or read operation" — 16 steps
> from 125 ms to 32 min. That is head/track hold, not the spindle.
>
> **Spinning the drive down is `0x1B` START STOP UNIT**, and it is already wired:
> `accudisc stop` (`cli/main.c:2668`) → `accudisc_spindle_stop`
> (`src/device.c:404`) → `adsc_mmc_start_stop(dev, 0, 0)`. Page `0x0D` is nowhere
> in that path. The two are different layers — `0x1B` says "stop now", page `0x0D`
> says "hold the track for N after a seek".
>
> The label came from `FEATURES.md`'s "Spindown Time → MODE page `0x0D`" row,
> which is marked **⚠**, defined there as *"standard MMC, present on this drive,
> never explicitly bound to the feature name"*. That is an explicit inference
> flag, and copying the label without it turned a flagged guess into an asserted
> fact. Whether PlexTools' "Spindown Time" setting really writes this page is
> still **unbound** — and it is testable, since the field is host-changeable.
>
> Corroboration worth keeping: the sweep's changeable mask for page `0x0D` is
> `00 0f` — byte 3's low nibble exactly, which is precisely where MMC-3 places
> the 4-bit multiplier. The drive confirms the spec layout independently. The
> timer has also been observed to **reset on a power cycle** (`PROTOCOL.md`
> line 1470).

**`0xEA` — CDB[2] is a scan-type selector, and there is a documented CD arm we
do not implement.** Pinned from QPxTool's `plugins/plextor/qscan_cmd.cpp`:

| CDB[1] | CDB[2] | CDB[3] | meaning | wired |
|---|---|---|---|:---:|
| `0x15` START | `0x00` | `0x01` | CD error counters | ✅ |
| `0x15` START | `0x10` | `0x01` | **CD Jitter/Beta** (`cmd_cd_jb_init`, line 125) | — |
| `0x15` START | `0x10` | `0x00` | DVD Jitter/Beta | — (DVD, out of scope) |
| `0x15` START | `0x00` | `0x00` | DVD PI/PO — CDB[9] `0x10`/`0x11`/`0x12` picks sum8 / +POE / PIF | — (DVD) |
| `0x16` READOUT | `0x01` | — | CD, 26 B (`0x1A`) | ✅ |
| `0x16` READOUT | `0x00` | — | DVD, 52 B (`0x34`) | — (DVD) |
| `0x17` END | — | — | stop | ✅ |

So on CD specifically there is exactly **one** unimplemented arm — **Jitter/Beta**
— and its framing is already pinned. Everything else on this opcode is DVD.

**An open question on the readout we already ship.** QPxTool decodes *eight*
fields from the 26-byte CD block (`bler` at offset 10, then `e31 e21 e11` at
12/14/16, `uncr` at 18, `e32 e22 e12` at 20/22/24); `plextor.c` takes three
(`[12..17]` summed as C1, `[20..21]` as CU, `[22..23]` as C2). **The `uncr`
placement disagrees** — we read CU at 20, QPxTool reads `uncr` at 18 and `e32`
at 20. Before treating that as our bug, note that **QPxTool's own source is
unsure**: lines 273-274 carry the comments `// check where drive returns E32`
and `// and where is UNCR`. Neither side is authoritative here, so this is an
open question, not a defect — and it is settleable on hardware with a disc whose
uncorrectable count is known non-zero.

**`0xED` — one mode code of a byte.** We use mode code 0 (POWEREC). No source on
disk enumerates any other, QPxTool names only the opcode, and the firmware
harvest never located `0xED` at all. Genuinely unexplored, and unusually cheap to
sweep because the GET form is a read.

### `0xF3` is the biggest vendor unknown after the mysteries

`PROTOCOL.md`'s builder inventory flags `0x487f60` (`f3 ..`) as **"heavily used
(6 sites) — likely a get/set dispatcher"**, and that reading has never been
tested. QPxTool documents only the *other* form, the TA scan (`f3 1f 23 …`,
`qscan_cmd.cpp:682-693`, with CDB[5]/[6] carrying DVD layer/zone coordinates and
CDB[7] the pass index). A six-call-site builder with a runtime-supplied byte 1 is
the same shape `0xE9` turned out to have — which is what makes it the best
candidate for a second cluster of features.

> **A trap this section had to avoid.** `qscan_cmd.cpp:664` contains
> `{{0x04,0x00},{0x10,0x00},{0x20,0x00},{0xFA,0x28},{0xEA,0x28},{0xDE,0x28}}`,
> which looks exactly like a `0xEA` selector table with vendor opcodes in it. It
> is neither: it belongs to `0xF3`, and the values are **DVD layer/zone
> coordinates** written to CDB[5]/CDB[6]. The `0xEA` and `0xDE` in it are data
> bytes. This is the same false positive `PROTOCOL.md` already records twice
> (QPxTool's `0xF2` at `qscan_cmd.cpp:46` is a *BenQ* CDB byte; `0xF4` in
> `pioneer_spdctl.cpp:21` is a *Pioneer* one). **An opcode-valued byte is not an
> opcode.**

### What this does not change

Sweeping a selector is still **vendor-opcode probing** wherever the opcode is a
vendor one, so §F's rules apply in full: gate on the medium, verify unwritable
first, trace every CDB unbuffered before issuing. `0x5A` and `0x43` are the two
rows exempt from that — they are standard MMC reads.

---

## F. Safety classes

Marked in the tables above with ⛔. Repeated here because the tables are long and
this is the part that must not be missed.

| op | why |
|----|-----|
| `0x3B` WRITE BUFFER | The Plextor **firmware-upload** path. Never issue. |
| `0xE3` PlexEraser | Destructive media erase. Never probe live. |
| `0xEE` drive reset | Reboots the drive. |
| `0xF2` unknown | Runs a minutes-long physical operation; has required a power cycle. |
| `0xF8` unknown | Blacklisted in `pxfw`'s own brute-force prober. |
| `0xDE`, `0xDF` | Also on `pxfw`'s blacklist. |

**The probing rules, which are about method rather than any list:**

1. **Gate on the MEDIUM, never on a per-opcode guess about which opcodes are
   dangerous.** This gate originally covered only `0xF2`, because its sense code
   proved it write-side while `0xF4` merely rejected a CDB field and looked inert.
   That reasoning was wrong: on a pressed CD `0xF4` returns `5/24/00`, but on a
   CD-R it returns **good status** — it executes. *An opcode's behaviour against
   read-only media tells you nothing about its behaviour against writable media.*
2. **Verify the medium is unwritable before probing** — no ATIP, not erasable,
   read-only profile — or empty the tray. An empty ATIP is the proof.
3. **Trace every CDB to stderr, unbuffered, BEFORE issuing it.** The `0xF2` probe's
   stdout was block-buffered; when the process was killed the entire log was lost,
   and *which* parameter started the operation is still unknown.
4. **Give unknown vendor opcodes minutes-long timeouts** and expect the drive to be
   unavailable meanwhile. **Do not read a timeout as a hang.**
5. **Take a before/after disc-state snapshot** — that is what proved `0xF2`/`0xF4`
   write nothing.
6. **Front-panel behaviour is data a probe cannot see. Ask.**
7. One drive, two agents: hold `flock /var/tmp/sr0.lock`. Contention collapses Q
   quality 99%→13% while audio stays clean, so it presents as a bad disc.

---

## G. Gaps this audit exposes

Findings that fell out of building the matrix, not previously recorded.

1. **`ADSC_OP_CLOSE_TRK_SES` (`0x5B`) was a dead constant — REMOVED 2026-09-02.**
   Defined at `src/mmc/cdb.h:22`, referenced nowhere in the tree: no builder, no
   wrapper, no consumer. Removed on Keith's instruction, **with the usability
   question deferred, not answered** — the deletion records that we do not issue
   it, which is not the same claim as "we do not need it".

   **THE DEFERRED QUESTION IS NOW ANSWERED (2026-09-03): `0x35` alone finalises,
   so `0x5B` is redundant on this path and nothing needs adding back.** The
   discriminator was run as specified — `0x51` READ DISC INFORMATION before and
   after a live DAO burn, reading **both** 2-bit fields of byte 2, since Disc
   Status and Last Session Status can disagree and `accudisc disc` surfaces only
   the first:

       BLANK    byte2=0x10   Disc 0 empty      LastSess 0 empty
       BURNED   byte2=0x1E   Disc 2 complete   LastSess 3 complete

   Full TOC read back, lead-out placed at the session end. The removal cost
   nothing. **Scope: CD-RW, DAO, single session** — the only medium we had, and it
   was consumed. TAO and multi-session are not written by this project; **CD-R is
   unverified and its lead-out behaviour differs**, so re-read both fields on the
   next CD-R burn before generalising.

2. **Six write-path opcodes had no CDB-layout test — CLOSED 2026-09-02.**
   `0x2A` WRITE(10), `0x35` SYNCHRONIZE CACHE, `0x54` SEND OPC, `0x5C` READ
   BUFFER CAPACITY, `0x5D` SEND CUE SHEET and `0xBB` SET CD SPEED are now
   asserted in
   `tests/test_cdb.c:test_write_path_cdbs`, and each assertion was
   **mutation-tested** — seven deliberate breakages of `src/mmc/cdb.c`, seven
   caught — rather than trusted because it passed.

   The hole was `tests/test_burn_flow.c`, which stubs all six and so asserts the
   burn *sequence* while passing identically with garbage CDB headers. The item
   originally said "five", excluding `0x5C`; six is the stub count, and closing
   five of six would have left the sixth hidden behind a heading that read
   CLOSED. Its `write10` stub discarded its arguments outright
   (`(void)lba; (void)nblocks;`), so **no hardware-free test had ever observed
   which LBA the burn writes to** — including the two's-complement −150 lead-in
   address, the one place WRITE(10)'s addressing is unusual enough to matter.

   Three of the five were only half-uncovered, and the distinction is why the
   first draft of this item ("asserted nowhere hardware-free") was wrong — a
   claim about the whole suite drawn from two of its files:

   | opcode | what WAS asserted hardware-free before this | what was not |
   |---|---|---|
   | `0x5D` | the **entire payload**, byte by byte (`test_cuesheet.c`) | the 10-byte CDB header |
   | `0xBB` | the **Nx→kB/s conversion**, via real `cdb.c` linked into `test_burn_flow` | the 12-byte CDB layout |
   | `0x35` | sequence position, incl. its role as the whole abort path | the CDB |
   | `0x2A` | **nothing** — the stub discarded `lba` and `nblocks` outright | everything |
   | `0x54` | **nothing** | everything |
   | `0x5C` | `cdb` (`test_write_path_cdbs`) | response semantics **validated live 2026-09-03**, item 4 |

   **What the fix does not reach**, so it is not read as cover for the burn: the
   new cases pin five CDB layouts and say nothing about `burn.c` issuing them in
   the right order with the right arguments. Delete the lead-in loop at
   `src/write/burn.c:536-543` and `test_cdb` stays green. That half is
   `test_burn_flow`'s job, and the two have to be read together.

3. **Eight of `0xE9`'s nine identified feature pages are unwired**, and so are
   `0xE4`/`0xE5`. Every one is GET-verified with CDB framing pinned — the
   expensive part is done — but the shipped driver reaches only page `0xBB`.
   VariRec, GigaRec, Silent Mode, Single Session, Book Type and Test Write are
   all a few lines of `px_mode()` away. Whether any belongs in a CD-DA tool is a
   scope question, not a technical one; GigaRec and VariRec are write-time and
   would need the burn path, Silent Mode and Single Session are read-side and
   would not.

4. ~~**`0x5C`'s response semantics are unvalidated**~~ — **ANSWERED 2026-09-03.
   Bytes 8-11 are AVAILABLE (blank) space, and `burn.c:171` reads them
   correctly.**

   The reasoning recorded here was right that an idle drive cannot settle it, and
   right that item 2 did not: on idle, `BufferLength(4-7) = 4802784` and
   `bytes 8-11 = 4802784` — equal, so both readings agree (measured with a raw
   `0x5C` probe). **But a burn discriminates, and maximally.** The engine computes
   `fill = (total - blank) * 100 / total` and reported minimum fill **98%** on
   every one of 15 live burns, i.e. bytes 8-11 fell to ~2% of capacity under load.
   The two candidate readings predict opposite things there:

   | if bytes 8-11 are… | behaviour as the buffer fills | computed fill |
   |---|---|---|
   | available space | falls toward 0 | **98% — OBSERVED** |
   | data length | rises toward capacity | 2% — not seen |

   Full capacity at idle, ~2% under load. It is available space. Note the shape of
   the earlier error, which I made in the first write-up of this: **98% fill is
   where the two readings DIVERGE, not where they agree** — "both readings agree"
   is the idle argument, and applying it to a loaded buffer inverts it.

5. **Mode page `0x01` byte 3 — the read retry count — is host-changeable, reads
   10, and AccuDisc never touches it.** A real exposed read-behaviour lever
   sitting unused next to a recovery ladder. The recovery *time limit* (bytes
   10-11) is **not** changeable, so this is the only retry lever the drive offers.

6. **`0x4A` GET EVENT STATUS NOTIFICATION is implemented and unused** — the
   standard way to notice a tray or media change without polling.

7. **The unlocated-but-implemented set is 8, not 4** (§ "floor"). `PROTOCOL.md`
   should be read with that correction; it names only `0x3C`/`0xB6`/`0xBB`/`0xE9`.

---

## Method notes that govern anything added here

- **Vendor documents are CP1252 and grep LIES about them.** GNU grep in a UTF-8
  locale silently skips lines containing invalid multibyte sequences — no error,
  no warning. A first pass over the PX-716 manual and the PlexTools help file
  reported SpeedRead, Silent Mode and SecuRec as *undocumented*: three false
  negatives. Run `iconv -f CP1252 -t UTF-8` first, or `grep -a`. **Any negative
  taken over those files without that step is void.**
- **Enumerate from the side you are not already indexed by.** The opcode table
  cannot audit its own coverage — checking it against the opcode inventory it was
  built from is circular. Three non-overlapping directions exist, each bounded by
  what its source *names*: opcode-side (PlexTools RE), documented-side (vendor
  manual + help file), lexicon-side (cross-vendor feature lexicon, which reaches
  standards and generic terms a marketing manual structurally cannot document).
- **`80251.sinc` warns "implementation is preliminary and has not tested".** The
  instruction *stream* is validated from several directions, but individual
  **operand** renderings should be re-derived before anything is built on them.
- **Never state a rule about vendor-opcode behaviour until the media axis is
  exhausted.** Three were withdrawn in one session for exactly this.

---

*Sources credited in `ATTRIBUTION.md`. QPxTool is GPL and is a read-only
reference here — constants and CDB framing are facts, cross-checked against
hardware, and no code is copied. The licensed T10 MMC-5 specification lives under
`private/code/MMC/` and never leaves it.*
