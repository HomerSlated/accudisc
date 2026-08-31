# Plextor vendor protocol — reverse-engineering notes (PX-716A)

**Licensing: MIT** (same as the core; see `LICENSE.md`). The opcodes and CDB
layouts here are functional hardware identifiers — facts, not copyrightable
expression — documented in QPxTool/cdrtools/cdrdao (cited as references) and
independently verified on the owner's own PX-716A. This file lives in the
driver zone for **architectural** vendor-isolation, not licensing; the core
stays pure MMC/SG. No third-party source is copied.

Working document. Session 1 (2026-07-12): method established, full vendor
opcode inventory harvested and validated. Session 3 (2026-07-12): the whole
consumer-feature map was pinned and **live-verified on the PX-716A** — see
`FEATURES.md` for the per-feature table. Opcode/page constants and CDB framing
were cross-referenced against QPxTool (GPL; credited in
`../../docs/reference/ATTRIBUTION.md`) after the user authorised it as a factual
cross-reference, then independently confirmed by raw SG_IO against the user's
own drive. **Correction to session 2: GigaRec is 0xE9 page `0x04`, not `0x06`
(see below).**

## Sources analysed

- `private/drives/Plextor/PTPXL/PTPXL.exe` — PlexTools Professional XL 3.x
  (PE32 x86, 8.6 MB, C++/RTTI). The application that builds and issues the
  vendor CDBs. Primary source.
- `private/drives/firmware/plextor/716A_111/rome_111.bin` — PX-716A firmware
  v1.11 (960 KiB), for the drive's **Sanyo LC897496K** signal-processing LSI.
  CPU core identified session 5 as an **Intel 80251** (see below); the
  firmware's internal layout (entry point, vector table, governor logic) is
  not yet mapped.
- `private/drives/firmware/plextor/716A_111/PXFirm3.exe` — the Windows
  flashing utility (PE32 x86, MFC, 2003). Confirms the update path: standard
  SCSI WRITE BUFFER (0x3B) mode 5 ("download microcode and save"), no
  host-side transform of the firmware bytes before they're sent.
- `private/drives/Plextor/Plextor-716.pdf` — end-user install manual. No SCSI
  content, but more than feature naming: its §5 contents list *is* the
  documented feature set for this drive, and §6 publishes the diagnostic LED
  codes (see the documented-feature audit below).
- `private/drives/Plextor/PTPXL/Help/PTPXLEN.chm` — PlexTools Professional XL
  3.x help (2007). The application's documented feature set, broader than the
  PX-716 (also PX-755/760, Blu-ray, TV-tuner hardware). **CP1252 — iconv
  before grepping**, see the method note below.

Tooling: `objdump -d -M intel` (cached full `.text` disassembly), `radare2`,
and a stack-tracking harvester (`scratchpad/re/harvest2.py`) that resolves
each CDB's byte template despite push/pop esp shifts. All inputs are
read-only; all working files live in the scratchpad.

## How PlexTools issues a command

Windows SPTI. The chokepoint is a single wrapper at **VMA 0x47aa30** that
fills a `SCSI_PASS_THROUGH_DIRECT` (0x2c bytes) and calls `DeviceIoControl`
with `IOCTL_SCSI_PASS_THROUGH_DIRECT` (0x4D014, found at file offsets
0x7ab82 / 0x7ac40).

Above it sits the common issue helper **fcn 0x47b240** (120 call sites =
the app's entire SCSI vocabulary). Each caller builds a 12-byte CDB on the
stack, `lea`s its address, and passes it in. CDB[0] is the opcode.

Validation of the method: the harvester's resolved opcodes for standard MMC
are all correct (0x12 INQUIRY, 0x1A/0x5A MODE SENSE, 0x28 READ(10), 0x43
READ TOC, 0x46 GET CONFIGURATION, 0xBE READ CD, 0xBF SEND DVD STRUCTURE,
0xAD READ DVD STRUCTURE, 0x51 READ DISC INFO …), and the 0xEA it finds
matches our existing `plextor.c` Q-Check driver. A method that reproduces
the known cases is trusted for the unknown ones.

## Vendor opcode inventory (verified this session)

17 distinct vendor opcodes (CDB[0] ≥ 0xD0) across 32 call sites. `rr` = a
byte supplied at runtime from a register/argument (address, length, mode
value); hex = a hard-coded template byte.

| op | builder fn | CDB[0..] template (hard-coded bytes) | notes |
|----|-----------|--------------------------------------|-------|
| D4 | 0x48abb0 | `d4 ..` | |
| D5 | 0x48ac20 | `d5 01 ..` | subcmd 0x01 at CDB[1] |
| D8 | 0x486b00 | `d8 ..` | READ CD-DA (classic Sony/Plextor raw audio read) |
| DE | 0x486dc0 | `de ..` | |
| DF | 0x48b0a0 | `df 10 .. 02 …… ec …… ec` | **mode-set family** (see below) |
| DF | 0x48b170 | `df .. .. 02 …… f1 …… f1` | selector f1 |
| DF | 0x48b2a0 | `df .. .. 02 …… f6 …… f6` | selector f6 |
| DF | 0x48b3d0 | `df .. .. 02 …… f2 …… f2` | selector f2 |
| E1 | 0x486ed0 | `e1 04 ff ..` | |
| E2 | 0x486f40 | `e2 ..` | |
| E3 | 0x48ada0 | `e3 06 ..` | subcmd 0x06 |
| E4 | 0x48a690 | `e4 03 .. 10 ..` | |
| E4 | 0x48aa30 | `e4 ..` | |
| E4 | 0x4967f0 | `e4 01 …… 12 ..` | |
| E5 | 0x48aac0 | `e5 ..` | |
| E9 | 0x489c20 | `e9 10 ..` | vendor MODE command (get/set by CDB[1]); ~~GigaRec = page 0x06~~ — **superseded, GigaRec is page `0x04`** (the `0x06` was `resp[1]`, a constant header present in every page's response; see the verified model below) |
| E9 | 0x489cc0 | `e9 ..` | |
| E9 | 0x495860 | `e9 10 21 ..` | subcmd pair 0x10/0x21 |
| EA | 0x489ad0 | `ea ..` | **Q-Check counters** — matches plextor.c |
| EA | 0x489b80 | `ea .. 20 ..` | Q-Check variant (mode byte 0x10/0x20 by arg) |
| EB | 0x489520 | `eb ..` | |
| ED | 0x4896b0 | `ed ..` | |
| F1 | 0x486d30 | `f1 01 ..` | |
| F1 | 0x48bb40 | `f1 ..` | |
| F3 | 0x487f60 | `f3 ..` | heavily used (6 sites) — likely a get/set dispatcher |
| F3 | 0x48a7a0 | `f3 1f 23 40 …… 08` | |
| F5 | 0x487ff0 | `f5 ..` | |

Cross-reference with what the core already knows:

- **0xEA** — Q-Check error census. `plextor.c` uses subcommands 0x15/0x16/
  0x17. The two EA builders here are the C1/C2 and PI/PO census variants;
  the mode byte (0x10 vs 0x20 at CDB[2]) selects sub-modes. Consistent.
- **0xD8** — READ CD-DA. Long documented in cdrtools/cdrdao; not needed for
  capture (all five 0xBE combos work — see the core), but confirms the
  drive's classic raw path.

## The 0xDF mode-set family (partially decoded)

Four builders (0x48b0a0 / 0x48b170 / 0x48b2a0 / 0x48b3d0), one per selector
byte at CDB[8] and CDB[11] (`ec`, `f1`, `f6`, `f2`), all with `02` at
CDB[3]. Called from an adjacent cluster of setter functions around
0x728250–0x728f19 — i.e. one dialog/class exposing four related toggles.
This is the shape expected of the SpeedRead / SilentMode / single-session /
book-type group. The selector→feature binding is **not yet pinned** — the
feature strings sit several call levels up in the C++ dialog hierarchy.

## The 0xE9 vendor-MODE command — verified model (session 3)

Opcode **0xE9** is a generic vendor GET/SET of small "mode pages", live-tested
on the PX-716A. The command layout:

```
CDB:  E9  DIR  PAGE  VAL  ..  ..  ..  ..  ..  L9  L10  ..
       0   1    2     3                      9  10
```

- **DIR** = CDB[1]: `0x00` = GET (read current), `0x10` = SET. (Session-2 had
  this inverted — it guessed 0x10 = GET.)
- **PAGE** = CDB[2]: the feature page (map below).
- **VAL** = CDB[3..]: value(s) to write on SET; 0 on GET.
- **Length**: the drive returns a fixed **8-byte page** and is lenient about
  where the `0x08` sits — CDB[10] for most pages, CDB[9] for SS/Hide. The
  transfer is always **data-in of 8 bytes, even for SET** (the drive echoes the
  resulting state, so a SET doubles as a read-back — a free self-test anchor).
- **Response**: `resp[0]` = page echo; **`resp[1]` = constant `0x06` header**;
  `resp[2..]` = state/values. This `0x06` is the byte session-2 misread from
  the status formatter as "GigaRec = page 6". It appears in *every* page's
  response.

Verified page map (all GET-confirmed live unless noted):

| page | feature | value bytes |
|------|---------|-------------|
| 0x01 | Single-Session / Hide-CD-R | resp[2] bit0=SS, bit1=hide |
| 0x02 | VariRec (CD; CDB[3]=`0x02\|disc_type`) | resp[2]=state, resp[3]=power, resp[5]=strategy |
| 0x04 | **GigaRec** | resp[3]=rate, resp[4]=disc-rate |
| 0x06/07/08 | SilentMode disc/tray/main | main returns full settings block |
| 0x21 | Test-Write (DVD+) | resp[2]=state |
| 0x22 | Book-Type / bitset | resp[2]=type |
| 0xBB | **SpeedRead** | resp[2]=state |
| 0xD5 | SecuRec state | resp[3]=state, resp[4]=disc |

Non-0xE9 consumer opcodes (also live-checked): **0xE4/0xE5** = AutoStrategy
("Write Strategy") read/write; **0xED** = PoweRec (GET: CDB[1]=00 CDB[2]=00
len@CDB[9], resp[2]=state, `ntoh16(resp[4..5])`=recommended kB/s); **0xEA** =
Q-Check (already in `plextor.c`). Danger opcodes catalogued in `FEATURES.md`
(0xEE = drive reset, 0xE3 = PlexEraser — never probe live).

## SpeedRead — bound and live-verified (session 3)

**SpeedRead = 0xE9 page `0xBB`.** SET ON (`E9 10 BB 01 … 08` at CDB[10]) flips
the drive's mode-page-2A max CD read speed **40× → 48×** (7056 → 8467 kB/s);
SET OFF restores 40×. This is the one consumer feature fully testable without
burning, and the round-trip (SET → observe page 2A → GET reads back `resp[2]=1`)
confirms the whole 0xE9 model end-to-end. Two-way toggle verified; drive left
as found (OFF).

## GigaRec — corrected binding (session 2 → 3)

**GigaRec = Plextor vendor opcode 0xE9 (vendor MODE get/set), page 0x04.**
(Session 2 said page 0x06; that was the constant `resp[1]` header byte, not the
page. Opcode 0xE9 and the rate table below were correct.)
GigaRec is a write-time recording-density control: it packs 0.6×–1.4× the
Red/Yellow-Book data onto a CD-R by adjusting the channel bit clock, trading
capacity against compatibility. It is set before a burn and read back for
status.

Trace (three independent confirmations):

1. RTTI class `CGigaRec` exists (`.?AVCGigaRec@@` at .data 0x97b250).
2. The status/label formatter at 0x42e900 requests a vendor MODE page via
   the get-helper **0x48cf20** (which builds opcode 0xE9), then gates on the
   response: `resp[1] == 0x06` identifies the GigaRec page, and it switches
   on `resp[3]` (the rate code) through a jump table (byte-map 0x42f838 →
   pointers 0x42f810) into the "GigaRec: N.Nx" strings.
3. Every rate string 0.6×–1.4× is referenced only from that switch.

The get-helper 0x48cf20 builds the CDB (base = the 0xE9 byte):

```
E9 | flag | page | 00 | .. | 00 | 00 | 00 | len | .. | .. | 00
 0    1      2                        8..
```

- CDB[0] = 0xE9
- CDB[1] = 0x00, or 0x10 when a caller flag is set (current-vs-default /
  direction selector — GET vs SET lives here; the symmetric setter at
  0x48d080 builds the same opcode with a data payload)
- CDB[2] = **page selector = 0x04 for GigaRec** (live-confirmed; session-2's
  "0x06" was the `resp[1]` constant header, not the page)
- length byte = 0x08 at CDB[10] (CDB[9] for some pages); response = 8 bytes

**Rate-code table (fully decoded from the jump table, HIGH confidence).**
The code is the page-data rate byte (`resp[3]` on read):

| code | GigaRec rate | meaning |
|------|--------------|---------|
| 0x00 | 1.0×         | Normal (no density change) |
| 0x04 | 1.1×         | expand |
| 0x01 | 1.2×         | expand |
| 0x02 | 1.3×         | expand |
| 0x03 | 1.4×         | expand (max overburn) |
| 0x84 | 0.9×         | compress |
| 0x81 | 0.8×         | compress |
| 0x82 | 0.7×         | compress |
| 0x83 | 0.6×         | compress (max reliability) |

The 0x80 bit marks the sub-1.0 (compression) rates; the low nibble is the
firmware's per-rate index (note 1.1× is 0x04, out of numeric order — use
the table, not arithmetic). Codes 0x05–0x80 hit the default handler (no
valid rate).

Confidence: opcode 0xE9, page 0x04, and the rate-code table are now HIGH
(the page and framing were live-verified reading GigaRec state back from the
PX-716A; the rate table matches QPxTool's `gigarec_tbl` byte-for-byte). No
driver code written — the write/burn path is paused and GigaRec is write-time,
so this is documentation only. When implemented, the GET form (0xE9 page 0x04)
is the natural selftest anchor: read rate → set → re-read.

## SecuRec — the mechanism (session 3)

State is an 0xE9 page (`0xD5`), but activation uses the **auth opcode 0xD5
(SEND_AUTH)**: a 16-byte data-out `[00][len][14×password]` with CDB[2]=01
CDB[3]=01 CDB[4]=02 CDB[10]=0x10; OFF = 0xD5 with no data. So SecuRec is a
drive-enforced read-lock (GET_AUTH/SEND_AUTH `0xD4`/`0xD5` handshake), **not**
container encryption — the disc structure stays intact but the drive refuses
the protected content until the password is loaded. The "special reader" the
manual mentions is simply a drive that implements this auth command. This is
the one consumer feature that touches AccuDisc's read path: a SecuRec disc will
read-fault until unlocked, so the core should surface "locked" rather than
mystify the caller (report-only; unlock stays a vendor-driver action).

## Method note (session 3)

The remaining features were pinned by cross-referencing QPxTool's
`lib/qpxplextor/plextor_features.cpp` (installed locally at
`/home/kgr/Git/qpxtool`) — authorised as a factual cross-reference — then
independently confirmed with raw SG_IO (`e9 00 <page> …`) against the user's
PX-716A. The static PlexTools RE (sessions 1–2) supplied the opcode inventory
and the get/set helper structure; QPxTool supplied exact pages and CDB framing;
the live drive was the final arbiter. `FEATURES.md` is the consumer-facing
progress table.

## Read-speed control — SET STREAMING, not a vendor opcode (session 4)

**Finding: PlexTools sets CD read speed with standard MMC `SET STREAMING`
(0xB6), not a proprietary opcode.** This explains why other rippers "can't set
the speed" on later Plextors: they issue `SET CD SPEED` (0xBB) / the Linux
`CDROM_SELECT_SPEED` ioctl (which our own `accudisc_set_speed` uses), and the
PX-716A honours the *streaming* path instead. The control is therefore **core
MMC, not a driver feature** — it belongs next to `accudisc_set_speed`, gated on
GET CONFIGURATION feature 0x0107 (Real-Time Streaming).

Trace (static, PTPXL.exe):

- **Builder fn 0x489740** — `ret 0xc` thiscall; assembles a CDB with
  `CDB[0]=0xB6` (at `489795`) and calls the verified issue helper `0x47b240`.
  The sibling builders 0x489f.. issue `GET PERFORMANCE` (0xAC) — the read-back
  of the same performance state.
- **Sole caller 0x49c95e** passes a **28-byte (`push 0x1c`) parameter list** —
  exactly one MMC *performance descriptor* — pointer `lea edx,[esp+0x10]`.
- Descriptor population in the caller:
  - an **End-LBA / capacity** value from a get-capacity call (`0x496770`),
    written big-endian, with `0xFFFFFFFF → all-0xFF` = "to end of disc"
    (`49c81e–49c848`);
  - a **rate** value computed by divide-by-constant then `imul …,0x546`
    (×1350) and split big-endian into a 4-byte field = **Read Size**
    (`49c8b2–49c8ef`);
  - constant **`0x03E8` (1000)** written into two 16-bit fields = the
    **1000 ms** time base (Read Time / Write Time) — i.e. "Read Size bytes per
    1 second" (`49c8f3–49c90c`);
  - a flags/`0x40` write (`49c91c–49c92b`) into the head of the descriptor.

**Why this gives a CAV *range* (the 17–40X cells).** A performance descriptor
is `{Start LBA, End LBA, Read Size, Read Time}`: "sustain Read Size ÷ Read Time
over [Start, End]." The rate is a *ceiling*. With the descriptor's Exact bit
clear the drive is free to run **constant angular velocity** — constant RPM, so
the linear transfer rate climbs with radius: ~17× at the hub to the 40× ceiling
at the rim (PlexTools' own note: "max speed for CAV is achieved at address
68:00:00"). SpeedRead (0xE9/0xBB) raises that ceiling to the 48× rung; the two
are complementary — ceiling vs. rung. To command a *fixed* CLV speed instead,
set the Exact bit and a single rate.

**Implication for AccuDisc.** Implement `SET STREAMING` in the core `mmc/`
layer as the real speed control (start/end-LBA scoped, so a first pass can be
pinned to the fast outer region, or a damaged span slowed in place). Keep the
`CDROM_SELECT_SPEED` path as the fallback for drives without feature 0x0107.

**Built (2026-07-15).** `adsc_cdb_set_streaming` + `adsc_cdb_set_streaming_desc`
(src/mmc/cdb.c), `adsc_mmc_set_streaming` (src/mmc/mmc.c), and `accudisc_set_speed`
routes through it with a `CDROM_SELECT_SPEED` fallback that latches on once the
0xB6 path proves unusable (unsupported opcode, or blocked for want of
CAP_SYS_RAWIO — SET STREAMING is data-OUT and does not pass the kernel's
unprivileged SG filter). Descriptor: flags `0x40`, Read Time 1000 ms, Read Size
= speed_x*1764/10 kB (1x = 176.4 kB/s → 7056 at 40x, 8467 at 48x). Layout
unit-tested (tests/test_cdb.c).

**Live-confirmed 2026-07-15** on the PX-716A (ZZ Top disc, setcap
cap_sys_rawio+ep). `accudisc speeds` with the streaming path active: commanded
4x and 8x rungs measured at the *outer* windows (where CAV alone would give
~30x) delivered **exactly 4.01x and 8.01x** — a binding, enforced ceiling the
old CDROM_SELECT_SPEED path could never produce. Where the ceiling sits above
the radius-limited CAV speed (inner windows, high commands) it correctly does
not bind (req=40 measured 11.83x at the hub). The Exact bit (fixed CLV vs CAV
ceiling) is still not pinned to a descriptor bit, so only the CAV-ceiling form
(flags 0x40, Exact clear) is implemented — which is the useful form for a
speed-scoped recovery pass. One quirk: a 16x request quantized to the drive's
8x rung (page2a reported 8); the 4x/8x rungs are exact.

## Firmware CPU identification (session 5, 2026-08-30)

Investigating a silent, damage-reactive read-speed governor found during a
joint AccuDisc/cdda2img recovery-ladder test (see `docs/reference/RECOVERY.md`
§12.10) led to `rome_111.bin` — item 3 of the session-4 next-steps below,
picked up here.

**The chip.** The PX-716A's signal-processing LSI is the **Sanyo LC897496K**
— confirmed via the `LC897390K` datasheet (a documented same-family sibling,
`private/research/LC897390K.PDF`) and Sanyo's own 2005 DVD/CD LSI product
brochure, which independently confirms the **LC897492** is used in the
**Plextor PX-716AL** (the 716A's sibling model). "LC89" is Sanyo's DVD/CD
signal-processing LSI prefix, distinct from the unrelated general-purpose
"LC87" MCU family — worth stating because an unverified LLM claim (Google
Gemini, relayed by the user) conflated the two, describing the core as "a
highly modified, single-cycle clone of the 8051 instruction set" derived from
LC87. That framing didn't survive contact with the one independent primary
source available (jaycarlson.net's LC87 writeup, found in this session's own
first search — it describes LC87 as "a fully orthogonal instruction set...
3-cycle 8-bit MCU," inconsistent with 8051 timing or encoding, and never
mentions LC897xxx). The LLM's one verifiable claim (the part number, and its
use in the PX-716AL) held up under an independent check; the specific
architecture claim did not — record kept as a caution for future sourcing,
not a rule against consulting one.

**The CPU core: Intel 80251, confirmed by primary source.** Sanyo's own 2005
brochure (`2005_Sanyo_DVD_CD_LSI_Brochure.pdf`, fetched via Wayback since the
origin host refused a direct fetch — see `ATTRIBUTION.md`), describing the
direct sibling chips LC897491/LC897492, states plainly: *"Built-in
CPU(80251)"* and *"Source code compatible with the Intel 8051, full
compatibility with the Intel 80251."* The 80251 is Intel's documented,
enhanced MCS-51-family core — a real superset, not an unofficial clone.

**Empirical cross-check against the actual firmware.** Before the primary
source was found, a multi-architecture SLEIGH opcode-density scan (`rz-ghidra`
via `rizin`, ~30 candidate ISAs plausible for 2007 embedded hardware — ARM,
MIPS, SH, PowerPC, z80, tricore, 8051, CR16, HC08/HCS08/HCS12, MCS96, and
others) had already narrowed the field: fixed-width 4-byte ISAs (MIPS, PowerPC)
showed 45-51% invalid-instruction rates over sampled 3000-instruction windows
— clearly wrong — while 8051-family candidates sat under 1%. Two apparent
"perfect" 0%-invalid results (`HCS12:BE:24`, `CR16AB:LE:16`) turned out to be
broken SLEIGH modules echoing their own error text as fake instructions
("Language ... is deprecated" / "No sleigh specification ..."), not real
matches — caught by reading the actual decoded output rather than trusting
the aggregate percentage. Once the primary source named the exact core,
`80251:BE:24:default` was re-run directly and decodes `rome_111.bin` as
genuinely idiomatic 80251/8051-family code (accumulator-centric arithmetic —
`ADD A,r6`, `MOV r0,A`; register-indirect addressing — `MOV @r1,A`) at every
sampled location, no invalid opcodes in any sample checked.

## Firmware internal layout — first pass (session 5, 2026-08-30) — ⚠️ SUPERSEDED, LARGELY WRONG

> **RETRACTED 2026-08-31 by session 6 (next section).** Everything below was
> produced by decoding the image as **8051 / MCS-251 *binary* mode**. The
> firmware is MCS-251 ***source* mode**, a different opcode map. Binary-mode
> output was therefore noise that happened to be readable, and the section's
> two headline claims are false:
>
> * "the code is real and coherent throughout the file" — **no**. Measured
>   against a proper control, binary-mode decode of this image is statistically
>   indistinguishable from random bytes (see next section, "Why this went
>   undetected").
> * "every control-transfer target stays inside its 64 KiB page" — **no**.
>   The image is full of `ECALL`/`EJMP` with flat 24-bit addresses. The
>   conclusion "no far-reach instructions observed" rested on a ~1600-
>   instruction sample of a *mis-decode*.
>
> Also void: the "verified interrupt handler at `0x1b1a`
> (`NOP`/`ORL 0x69,A`/`RETI`)". Those bytes are not an instruction boundary in
> source mode; the real vector table is at file `0x50`. The retraction of the
> `0x50` SFR-pair lead stands — it was wrong — but for a different reason than
> recorded: `0x50` is a **data** table (`EJMP addr24` vectors), not code at all.
>
> Kept unedited below as the record of a wrong turn, and because the *negative*
> results in it (no ASCII strings, no LJMP vector table) remain valid — they
> were byte-level observations, not decode-dependent ones.

Picked up directly from the "not yet done" note above, same session. Goal:
locate the entry point / vector table / true base address, as a prerequisite
to finding the governor logic. Result: **partially mapped, base/entry point
still unresolved** — recorded here as a real, sourced negative rather than
left as a bare TODO, per this codebase's convention of writing up what a check
ruled out and not just what it confirmed.

**Method.** Sampled `rome_111.bin` at seven 64 KiB-aligned offsets spanning
the file (`0x0`, `0x10000`, `0x40000`, `0x60000`, `0x90000`, `0xa0000`,
`0xc0000`) under `rz-ghidra`'s `80251:BE:24:default` SLEIGH module, both
spot-checking short windows and — to get a real opcode-frequency picture
rather than another eyeballed sample — collecting ~1600 instructions across
eight 400-instruction chunks starting at `0x40000`.

**Finding 1 — the code is real and coherent throughout the file, not just at
the handful of points checked previously.** All seven sampled regions decode
as idiomatic 8051-family code (no invalid opcodes), and the frequency table
from the ~1600-instruction sample is exactly what a real 8051 program's should
look like: `MOV`/`ADDC`/`ADD`/`XRL`/`INC`/`DEC` dominate as expected for
accumulator-centric arithmetic, and both `RET` (8) and `RETI` (2) appear —
genuine subroutine and interrupt-handler structure, not decoder noise.

**Finding 2 — every observed control-transfer target stays inside the 64 KiB
page that issued it.** `LJMP`/`LCALL` (16-bit absolute) and `AJMP` (11-bit
page-relative) targets sampled at each of the seven offsets consistently share
the high byte of the *current* address — e.g. at `0x90040`, `LJMP 0x901bd` and
`LJMP 0x90212`; at `0xc0040`, `LJMP 0xc0400`, `0xc014d`, `0xc0165`, `0xc0468`.
This is expected from the instruction encoding alone (`LJMP`'s operand is only
16 bits, so it cannot address outside a 64 KiB window regardless of how the
file is laid out) — it does **not** by itself distinguish "one flat ~950 KiB
image, code just never happens to jump across a page in the samples taken" from
"N independently-based 64 KiB banks, each its own relocatable unit," which was
the original question. **This point is genuinely unresolved**, not decided
either way by what was checked.

**Finding 3 — no 80251 "far" (24-bit-reach) instruction appeared in ~1600
samples.** Despite selecting the 24-bit SLEIGH variant, only the classic
8051-compatible near-instruction set was observed (`LJMP`/`LCALL`/`AJMP`/
`ACALL`/`SJMP` plus the full arithmetic/logic/branch complement — see the raw
mnemonic table in session notes). Either genuine far calls are rare enough not
to have been hit yet, or this firmware doesn't use the 80251's extended-reach
instructions at all and crosses page boundaries some other way (a bank-select
SFR write is the classic 8051-with-banked-ROM pattern).

**The `0x50` SFR-pair lead did not hold up — chased and retracted.** Read
back from a raw hexdump alone, `0x50` looked like a clean repeating `MOV
0xfd,r2` / `MOV 0xfe,r2` loop. A proper linear disassembly of the same bytes
(`pd 60 @ 0x44`) shows it isn't: `MOV 0xfe,r2` recurs at 0x54, 0x58, 0x5c, and
0x64 — four times in twenty bytes, at irregular strides, interleaved with
`DEC r2`/`RR A`/`CJNE`/`MOVX` and one instruction (`MOV 0x8a,#0xfe`, direct-
to-immediate) that doesn't fit an SFR-pair-write loop at all. That's the
signature of decoding starting a few bytes off true instruction alignment —
likely still padding/filler rather than a real bank-select routine — not a
deliberate repeated write. **Retracting this lead**; the correct instinct
(check a raw-hex read against the actual disassembly before trusting it) is
what caught it, consistent with [[silent-narrowing]] in the AccuDisc memory
index.

**What chasing it turned up instead: a real, verified interrupt/dispatch
landmark.** The same window contains `NOP` / `LJMP 0x1b1a` at file offset
`0x8b`, and — checked against raw bytes, not just disassembly, since the
prior lead's failure mode was exactly disassembly-without-byte-verification —
the encoded 3-byte sequence is confirmed genuine: `02 1b 1a` (`LJMP` opcode +
16-bit target). The same `NOP`/`LJMP 0x1b1a` shape recurs at roughly a dozen
points through the rest of page 0 (`0x14e`, `0x165`, `0x17c`, `0x198`,
`0x1b7`, `0x1d6`, `0x1f5`, `0x273`, `0x28c`, …, `0x35e`), at irregular but
plausible-for-similarly-shaped-functions strides (17–195 bytes) rather than a
rigid fixed-size table. **`0x1b1a` itself is real code, not a coincidence
target:** `NOP` / `ORL 0x69,A` / `RETI` — a minimal "set a flag, return from
interrupt" stub, exactly the shape of a shared default handler that several
nearby dispatch routines fall through to.

**Caution on how far this generalises — checked, and it doesn't.** A
whole-file search for the literal byte triplet `02 1b 1a` returns 454 hits
spread across every 64 KiB page sampled (page 0: 117, page 1: 43, … page
0xa0000: 80). Tempting to read as "454 real calls into a shared cross-page
handler," which would have been a strong data point for the base-address
question — but the within-page offsets outside page 0 don't cluster the way
page 0's do (page 1's hits sit at `0x15a9`, `0x40be`, `0x432c`, …; page 0xa's
at `0x6cb`, `0xa1b7`, `0xbc06`, …, no local grouping at all). A 3-byte pattern
recurs often enough in dense 8051 code by construction (`0x02` alone is a
common encoding byte) that most of these are almost certainly coincidental
matches at non-instruction boundaries, not real `LJMP` instructions. Only
page 0's cluster — independently corroborated by the earlier `NOP`-padded
trampoline pattern seen at session-5's very first sample — is trustworthy.
**Do not cite the 454 figure as evidence of a cross-page handler**; it isn't
one. This is recorded because it's the same trap Finding 2 above warns about
in a different shape: a check whose positive result is guaranteed by
coincidence rather than by the structure under test.

**Net effect on the open question.** Doesn't resolve base address or the
bank-crossing mechanism. It does add one more corroborating data point that
page 0's early bytes are genuine, purposefully-structured code (a defensive
dispatch pattern typical of what sits near a reset/init path), which is
consistent with — though still short of proving — file offset 0 mapping
directly onto the CPU's own low code space. No SFR-pair or other bank-select
write was located; that mechanism is still unfound.

**Finding 4 — the vector-table search is now confirmed negative, not just
inconclusive.** Re-checked directly against raw bytes (not just disassembly):
file offset `0x44`–`0x4f`, right after the ASCII header, is a literal run of
`0xff` bytes — decoded as `MOV r7,A` only because `0xff` happens to encode
that instruction, not because it *is* one. So "base = 0, image includes the
header" is now ruled out on hard evidence, not just absence of a jump-table
pattern; the true base/entry point relative to this file remains unknown.

**Tooling caveat.** This machine's `rizin` build segfaults deterministically
on some `pd`-then-large-batch and `s`-then-`pd` sequences against this file
under the 80251 SLEIGH module (reproduced 3/3 at `pd 10 @ 0x90000` after a
prior `s`, and on every `pd 3000`-in-one-call attempt) — a rizin/SLEIGH
instability, not a finding about the firmware. Worked around with `pd N @
addr` inline addressing and batches ≤400 instructions. Anyone re-running this
should expect the same crashes and use the same workaround rather than
mistaking them for masked/protected memory.

**Stopping point.** Per the standing rule that RE threads with no committed
consumer stay bounded: the entry point and true base address are still
unknown, and finding them requires either locating a genuine bank-select
mechanism (the `0x50` SFR-pair candidate is retracted above; none found to
replace it) or a documented boot sequence Sanyo never published. This is
queued as the next step, not being pursued further right now — the governor
logic (§ below, "Next steps" item 5) stays blocked on it.

## Firmware decoded — MCS-251 *source mode*, base 0xF00000 (session 6, 2026-08-31)

The layout thread was not stuck for lack of ideas. It was stuck because **every
byte had been decoded with the wrong opcode map**, and the wrong map produced
output plausible enough to reason about for a whole session.

### The finding

`rome_111.bin` is **MCS-251 *source mode*** code. The MCS-251 has two opcode
maps, selected by `UCONFIG0.0`:

| mode | opcodes 0x60–0xFF mean | legacy 8051 reached via |
|---|---|---|
| **binary** (8051-compatible) | the 8051 instruction | — (new instrs. use `A5` escape) |
| **source** | the *new MCS-251* instruction | `A5` escape prefix |

Every previous session used binary mode, which is `rz-ghidra`'s default. Under
source mode the same bytes decode as ordinary firmware:

```
0x0ad60d  9afcc279   ECALL 0xfcc279     ; 24-bit inter-bank call
0x0ad614  7eb3b877   MOV   acc,0xb877   ; read memory-mapped register
0x0ad618  1eb0       SRL   acc
0x0ad61c  5401       ANL   A,#0x1       ; isolate a status bit
0x0ad61e  780d       JNE   0x98
```

### Consequences — every open question from session 5 resolves

* **Base address = `0xF00000`.** The image is 0xF0000 bytes = 1 MiB flash minus
  a 64 KiB top boot block (which the update file does not carry). `file_off =
  addr − 0xF00000`. Confirmed across the whole image, not inferred from a
  sample: of 19890 `ECALL` (`0x9A`) sites, **95.6% carry a 24-bit operand
  inside `0xF00000–0xFEFFFF`** against a **5.86%** random-chance baseline for
  that window — a 16× enrichment spanning *all fifteen* high bytes `f0`–`fe`,
  i.e. the entire image rather than the region first sampled. 2048 distinct
  in-range targets; that set is also the Ghidra call-graph seed set.
  **`EJMP` (`0x8A`) does not replicate this and must not be cited as if it
  did** — only 3.4% of `0x8A` sites carry an in-range operand, *below* chance,
  so most `0x8A` bytes are operand/data rather than opcodes. `EJMP` is
  genuinely rare (vector table plus occasional tail jumps); the base-address
  evidence rests on `ECALL` alone.
* **There is no bank-select SFR, and the search for one was misconceived.**
  Bank crossing is `ECALL`/`EJMP` with **flat 24-bit addresses** —
  `A5 9A <addr24>` and `A5 8A <addr24>` in binary-mode bytes, i.e. `9A`/`8A`
  in source mode. This is why no such SFR was ever found.
* **The vector table is at file `0x50`,** immediately after the 0x50-byte
  header — a table of 4-byte `EJMP addr24` entries. Entry 1 → `0xfe1a03`
  (file `0x0e1a03`), which decodes as `SETB` then
  `PUSH dr8/dr16/dr20/dr24/dr28`: an ISR prologue saving register banks.
  Its entries shift by a constant between firmware revisions, as address-table
  entries must.

### Image container (byte-level, decode-independent)

```
0x00000  "PLEXTOR "  "DVDR   PX-716A  "  "1.11"   <- exact SCSI INQUIRY fields
0x0001b  "03/23/07  15:10"                        <- build stamp
0x00030  "PLEXTOR ROME    000"                    <- internal codename
0x00044  ff-padding to 0x50
0x00050  EJMP vector table
...
0x efffe  16-bit big-endian additive checksum of bytes [0, 0xEFFFE)
```

Verified: `sum(d[0:-2]) & 0xFFFF == 0xB6F2` == the trailing two bytes. Flat,
uncompressed, unencrypted (2.5–10% `0xFF` fill across all 15 banks).

### How it was established (and why the earlier "verification" was empty)

The trap here is that **all 256 byte values are valid 8051 opcodes**, so "it
disassembles cleanly" is a check that *cannot fail* and is therefore worth
nothing. Session 5's confidence rested entirely on that non-check.

The instrument that could return "no" is **conditional-branch target alignment**:
in real code a relative branch must land on an instruction boundary. Calibrated
against controls, on 1200-byte windows in four banks:

| decode | on-boundary |
|---|---|
| random bytes (negative control) | ~62% |
| synthetic valid 8051 (positive control) | ~86% |
| **`rome_111.bin` as 8051 / binary mode** | **77.7%** (n=103) |
| **`rome_111.bin` as MCS-251 source mode** | **96.2%** (n=53) |

**Read that table with its caveat.** The same run reported 6.9–61.7% of slots
as *invalid* in source mode, partly a harness artifact (the probe drops a slot
when `pd 2`'s output rows fail to pair) and partly real data regions. The
sample sizes are small and selected by which windows produced enough
conditional branches. It is directional support, **not** the load-bearing
evidence — that is the RPC1 patch decode below, which is semantic and does not
depend on any of these numbers. Do not cite 96.2% as a settled measurement.

Reframed usefully: per-region source-mode invalid rate is a **code/data
segmentation map**. Banks 6 and 12 score ~95% invalid in *both* modes (data
tables); bank 9 scores 6.9% source vs 25.2% binary (code). That map is what
stops a dispatcher search from stride-scanning data.

Supporting, independently derived: `0x7E` is the most common byte in the image
at 5.67% — absurd as 8051 `MOV R6,#imm`, exactly right as source-mode `MOV`;
the `s03` operand nibble correctly *predicts* instruction length; and 16-bit
`dir16` operands cluster in `0xa000–0xbfff` at 3–4× the base rate, i.e. they
are memory-mapped register addresses.

**The decisive evidence is semantic, not statistical** — see the RPC1 patch below.

### Firmware corpus

Three images, all 983040 bytes (thanks to Keith for the 1.10 pair):

| image | file |
|---|---|
| 1.11 (stock) | `cdda2img/private/drives/firmware/plextor/716A_111/rome_111.bin` |
| 1.10 (stock) | `716A110.exe` → `rome110.bin` |
| 1.10 (RPC1-modified) | `716A_110.ZIP` → `RPC1_110.BIN` |

`1.10 stock` vs `1.11` differ in 51.8% of bytes — that is **bulk relocation, not
a rewrite**: isolated changed sites show 16-bit operands shifting by a constant
`+0x6D`, and the `0x50` vector entries by `+0x7D0`.

### The RPC1 patch — a known-purpose semantic anchor

`1.10 stock` vs `1.10 RPC1` differ in exactly **four bytes**: three code bytes
in one 1687-byte cluster, plus the trailing checksum.

| file offset | stock | RPC1 | source-mode meaning |
|---|---|---|---|
| `0x0ad64d` | `54` | `14` | `JE 0xcf` → `JE 0x8f` (branch target redirected) |
| `0x0ada6e` | `6d` | `1a` | branch operand in the same check |
| `0x0adce4` | `68` | `80` | **`JE 0x94` → `SJMP 0x94`** — conditional made unconditional |
| `0x0effff` | `fb` | `80` | checksum fixup |

The three code edits sum to −0x7B and the checksum byte moves by −0x7B: the
container model closes exactly. `JE → SJMP` at `0x0adce4` is the textbook
region-code defeat, so **the region-code enforcement path is at ≈ `0xFADCE4`**.
A known-purpose patch decoding as precisely the expected instruction
transformation is stronger evidence for the ISA than any alignment statistic.

### Tooling

`re-tools/srcdis.py` — source-mode disassembler. Source mode is byte-identical
to binary mode with `A5` prepended (SLEIGH `GROUP3`), so it probes the stock
`80251:BE:24:default` module with an injected `A5` and subtracts the prefix,
falling back to GROUP1 (plain 8051, opcodes < 0x60) when GROUP3 misses.

Two `rz-ghidra` defects it works around, both confirmed here and neither a
firmware property:

* **`pdj` (JSON) is broken for this module** — it returns `invalid` for
  instructions its own `pd` *text* output decodes correctly. Parse `pd` text.
* **the `@@=` offset iterator silently caps at ~115 results** regardless of how
  many offsets are supplied. Batch with `;`-separated `-c` commands instead,
  which scales (verified to 800).

Ghidra 12.1.3 is installed and its 8051 module **already implements source
mode** — `8051_main.sinc:234` defines a `srcMode` context bit, defaulting to 0.
Setting it is the route to full `analyzeHeadless` auto-analysis; `srcdis.py`
then remains useful as an independent cross-check.

## Exposed capability surface — measured on hardware (session 6, 2026-08-31)

Read-only enumeration of what the PX-716A actually exposes, run against the
live drive (`/dev/sg3`, fw 1.11) under `flock /var/tmp/sr0.lock`. Tools:
`re-tools/mmcsweep.c` (presence) and `re-tools/mmcdetail.c` (payloads). No
`MODE SELECT`, no `WRITE BUFFER`, no vendor opcodes.

**Correction to the running assumption: media was present**, not an empty
tray (`TEST UNIT READY` → 00). Everything issued was read-only so nothing
turned on it, but the "empty tray" precondition claimed beforehand was false
and the LBA-dependent results below describe *the loaded disc*.

### Mode pages — 11 present, and the speed page is not one the host may touch

| page | name | changeable mask (params) |
|---|---|---|
| 0x01 | Read/Write Error Recovery | `3f ff ff ff 00 00 ff 00` |
| 0x02 | Disconnect/Reconnect | `ff ff …` |
| 0x05 | Write Parameters | `7f ff 0f ff 00 3f ff 00` |
| 0x07 | Verify Error Recovery | `3f ff …` |
| 0x08 | Caching | `04 00 …` (WCE only; **RCD not changeable**) |
| 0x0d | CD Device Parameters | `00 0f …` |
| 0x0e | CD Audio Control | `06 00 00 00 00 00 0f ff 0f ff` |
| 0x1a | Power Condition | `00 03 ff ff ff ff ff ff ff ff` |
| 0x1d | Timeout & Protect | `00 00 04 00 ff ff` |
| **0x2a** | **MM Capabilities** | **all zero over all 52 bytes** |
| 0x3f | (return-all alias of 0x01) | — |

**`MODE SENSE` PC=1 is the drive stating which bits the host may alter, so an
all-zero mask on page 0x2A is a hard negative, not an inference: read speed is
not controllable through the mode page.** Page 0x2A currently reports max read
`0x1b90` = 7056 kB/s = 40.0x and *current* read speed identical at 40.0x.

Genuinely useful and currently unused by AccuDisc: **page 0x01 byte 3 is the
read retry count, is host-changeable, and currently reads 10**, with the
recovery flag bits (TB/RC/EER/PER/DTE/DCR) changeable too — but the recovery
*time limit* (bytes 10-11) is **not** changeable. That is a real exposed
read-behaviour lever, relevant to the recovery ladder.

### Features — 34 reported, 3 vendor

`0x0107` Real-Time Streaming is CURRENT with payload `1e 00 00 00`: SCS,
MP2A, WSPD and SW set, RBCB clear — i.e. the drive claims `SET CD SPEED` and
`SET STREAMING` support. Both are already implemented in AccuDisc.

The three vendor features are **write**-speed capability tables, not read
control — worth documenting, but they are not the governor:

* `0xff00` = `01 03 01 01`
* `0xff10` = per-CD-profile write speeds — profile `0x0A` (CD-RW) 24x/10x/4x,
  profile `0x09` (CD-R) 48x/32x/16x/8x/4x
* `0xff11` = per-DVD-profile write speeds — profiles `0x2b`/`0x1b`/`0x1a`/
  `0x14`/`0x11` (DVD+R DL / +R / +RW / -RW / -R)

### READ BUFFER is *not* a firmware-memory window

Worth recording as a negative because it was the hoped-for shortcut to
locating the governor's variables at runtime. Only buffer **id 0** exists
(ids 1-15 report zero capacity), capacity 8355840 bytes — that is the 8 MiB
**data** buffer, and mode 2 returns disc data, not CPU RAM. There is no
exposed live-memory read path here.

### GET PERFORMANCE — the CAV curve is published, degradation is not

Type 0 (nominal), one descriptor for the loaded disc:

```
start LBA 0        ->  2999 kB/s (17.0x)
end   LBA 359487   ->  7056 kB/s (40.0x)
```

That is the drive publishing its own CAV read-performance model — slow at the
inner radius, full rate at the outer. Type 3 confirms a constant 40.0x read
ceiling across all five write-speed descriptors.

**`EXCEPT=1` (performance exceptions) returns CHECK CONDITION `5/24/00`
INVALID FIELD IN CDB — the drive does not implement performance-exception
reporting at all.** This matters for `RECOVERY.md` §12.10: the drive has no
standard mechanism by which it could ever *announce* that it has reduced read
speed. A governor here is necessarily silent, because the reporting channel
MMC defines for exactly this is not implemented.

### Where that leaves the governor question

Enumerated exposed levers over read speed, complete for the read-only surface:
`SET CD SPEED` (0xBB) and `SET STREAMING` (0xB6) — both already implemented —
plus page 0x01 error-recovery parameters, indirectly. **Not** page 0x2A, and
**no** exception reporting. Still open, and needing a *timed* experiment
rather than an enumeration: whether 0xBB/0xB6 actually override the governor
in practice. Per [[entropy-not-mystery]] that must measure the **delivered**
rate; page 0x2A reports the request, and is in any case read-only to the host.

## The SCSI command dispatcher — located (session 6, 2026-08-31)

Phase 2: walk outward from the region-code anchor at `0xFADCE4` to the command
dispatcher. Found — but not by any of the mechanisms first assumed, and the
four failed searches are recorded because each is a real negative.

### What the dispatcher is not

| hypothesis | result |
|---|---|
| `CMP Rm,#opcode` compare chain | **no** — 90 sites image-wide, best cluster 2 distinct opcodes |
| `EJMP addr24` jump table | **no** — exactly **one** stride-4 EJMP table exists in the image, the `0x50` vector table |
| table of opcode *values* at constant stride | **no** — the one candidate (stride-8 run `42 43 …4b` at file `0x093742`) is **unrolled copy code**, `MOV DPTR,#0x2128 / MOVX A,@DPTR / MOV 0xf738,acc`; the "opcodes" were incrementing *destination addresses* |
| opcode-*indexed* table of handler indices | **no** — 68 candidates from a constant-run prefilter, best scores 19/46 implemented vs 33/98 never-used, i.e. random |

The third row is a caution worth keeping: **ascending byte runs are worthless as
an opcode-table signal**, because incrementing addresses look exactly like
ascending opcodes. That false positive survived two separate scans.

### What it actually is

**Subtract-and-branch chains.** The opcode is loaded from a memory-mapped
register and walked down a chain of `ADD A,#-delta` / `JE`, with a trailing
`SJMP` to the unsupported path:

```
0xfbffa4  7eb3b7e7   MOV acc,0xb7e7    ; the CDB opcode
0xfbffa8  2400 6843  ADD A,#0x00 / JE  ; -> 0x00 TEST UNIT READY
0xfbffac  24fd 683f  ADD A,#0xfd / JE  ; -> 0x03 REQUEST SENSE
0xfbffb0  24f1 683b  ADD A,#0xf1 / JE  ; -> 0x12 INQUIRY
   ...
0xfbffcf  1bb1 681c  DEC acc,#2  / JE  ; -> 0x5a MODE SENSE(10)
0xfbffe0  8000       SJMP              ; default: unsupported
```

Chain arms also use `INC A`, `DEC A` and `DEC acc,#Short` (`1b b0/b1/b2` =
1/2/4), which is why a first detector that only knew `ADD A,#imm` under-counted
this chain as 10 arms when it has 15.

**Dispatch is split across at least 12 such chains**, not centralised. The one
above contains exactly the commands legal with no media loaded
(`00 03 12 1a 1b 1e 35 4a 52 58 5a 5b 5c`), and it is entered after a bit test
on register `0xb878` — so the chains are almost certainly **per-drive-state
command filters** rather than one dispatcher.

Chains found (`*` = known vendor opcode, `?` = not in our opcode set):

| address | arms | opcodes tested |
|---|---|---|
| `0xfda911` | 11 | `12 1b 46 4a 52 5c ac bf eb* f3* f5*` |
| `0xfd4edc` | 11 | `12 1b 46 4a 5c ac bf e4* eb* f3* f5*` |
| `0xfbffa8` | 15 | `00 03 12 1a 1b 1e 35 4a 52 58 5a 5b 5c f4? f5*` |
| `0xfbd075` | 13 | `08 28 35 44 53 5b a8 aa b9 be d8* d9? df*` |
| `0xfd3eca` | 18 | `08 0a 1b 28 2a 2e 35 4a 54 5b a1 a8 aa ad b9 be f2? 04` |

Union over all 12 chains: **35 opcodes** — 29 MMC
(`00 03 04 12 1a 1b 1e 28 2a 2e 35 44 46 4a 52 53 54 58 5a 5b 5c a1 a8 aa ac ad
b9 be bf`) and 6 vendor (`d8 df e4 eb f3 f5`).

### Three candidate vendor opcodes not in the PlexTools set

**`0xD9`, `0xF2`, `0xF4`.** Each sits mid-chain immediately adjacent to a
*known* vendor opcode (`d8`→`d9`, `f4`→`f5`), and a chain whose cumulative
arithmetic were wrong would be unlikely to land on known vendor opcodes at all.
The PlexTools RE could only ever find opcodes *PlexTools issues*; the firmware
may implement more. **Unverified on hardware** — testing them is vendor-opcode
probing and is gated on Keith's consent.

`0x08`/`0x0A`/`0x04` also appear and are *not* discoveries — they are legacy
`READ(6)`/`WRITE(6)`/`FORMAT UNIT`, absent from the opcode set used for scoring.

### The harvest is partial — do not read the union as the command surface

Two opcodes known to be implemented appear in **no** detected chain: `0x3C`
READ BUFFER (used successfully against the live drive in the Phase 1 sweep
above) and `0xE9` (the vendor MODE command, live-verified in session 3). So
further dispatch sites exist that this detector does not match — most likely
chains using compare forms it does not model, or handlers reached by other
means. The 35-opcode union is a **floor on** the command surface, not the
surface.

### Call graph

An `ECALL`-derived call graph (19009 edges over 2033 distinct entries; a
function entry is simply an `ECALL` target) puts the region check inside the
function at `0xfad888`, reached by a narrow funnel: one caller (`0xfcd060`),
one caller (`0xf74bd6`), then widening to 2, 3, 12. That extraction is also the
Ghidra seed set.

## Three new vendor opcodes, the speed ladder, and Ghidra source mode (session 6, 2026-08-31)

### 0xD9, 0xF2, 0xF4 — predicted from firmware, CONFIRMED on hardware

The three candidates from the dispatcher chains are all **implemented**. Probed
on `/dev/sg3` under `flock`, 12-byte zeroed CDBs, with controls proving the
discriminator can return either answer:

| opcode | sense | reading |
|---|---|---|
| `0x12` INQUIRY (**positive control**) | good status | implemented |
| `0xC1` unassigned (**negative control**) | `5/20/00` | INVALID COMMAND OPERATION CODE |
| `0xC5` unassigned (**negative control**) | `5/20/00` | INVALID COMMAND OPERATION CODE |
| `0xD8` known READ CD-DA | `4/00/00` | implemented |
| **`0xD9`** | `5/64/00` | ILLEGAL MODE FOR THIS TRACK — parsed, rejected on *track mode* (⚠ see retraction below) |
| **`0xF2`** | `2/30/05` | CANNOT WRITE MEDIUM, INCOMPATIBLE FORMAT — a **write-side** command |
| **`0xF4`** | `5/24/00` | INVALID FIELD IN CDB — parsed, rejected on a parameter |

Drive healthy afterwards (INQUIRY unchanged). **What they *do* is still unknown**
— only that they exist and parse. `FEATURES.md` should not gain entries until
their semantics are established.

**The safety step was load-bearing, not ceremony.** The tray held a disc and
`START STOP UNIT` eject was refused (`5/53/02` MEDIUM REMOVAL PREVENTED). Rather
than override the lock, the disc was shown to be **factory-pressed** — `READ
DISC INFORMATION` gives Erasable=0/Complete, and `READ TOC` format 4 returns an
**empty ATIP**, which exists only on recordable media. A pressed disc is
physically unwritable, so probing was safe with it loaded and no drive state was
disturbed. `0xF2` then answered "cannot write medium" — i.e. it *is* a write
command, and the only reason it did nothing is that the medium could not be
written. Against a CD-R, an all-zero CDB to an unknown write opcode is exactly
the probe that silently destroys data. **Verify the medium is unwritable, or
empty the tray, before probing unknown vendor opcodes.**

### The read-speed quantisation ladder — located at 0xF65B83

Mode page 2A is built at runtime (its byte pattern appears nowhere in the
image), but the speed constants do. `MOV wr6,#0x1b90` (40x) and
`MOV wr6,#0x2113` (48x) sit ten bytes apart — the SpeedRead pair. The
surrounding structure is a switch of **eleven identical 10-byte arms** whose
`SJMP` displacements form an exact arithmetic progression (`74 6a 60 56 4c 42
38 2e 24 1a 10`, step −10) converging on one target:

```
7e 34 <speed16>    MOV wr6,#<speed>
79 3f ff ff        (store; exact semantics not yet established)
80 <rel>           SJMP common
```

Decoded against 176.4 kB/s = 1x, the table is the **complete read-speed ladder**:

| `00b0` | `0161` | `02c2` | `0583` | `06e4` | `0845` | `0b06` | `0dc8` | `108a` | `160d` | `1b90` | `2113` |
|---|---|---|---|---|---|---|---|---|---|---|---|
| 1x | 2x | 4x | 8x | 10x | 12x | 16x | 20x | 24x | 32x | 40x | 48x |

Twelve steps, exact. Ghidra places it inside the function at **`0xF65B36`**.
This is the concrete form of "you request a speed and the governor tells you
what you can have" — the quantiser's data. Note the arithmetic progression is
what proves the parse: `srcdis.py` mis-decodes `79 3f ff ff 80` as one 5-byte
instruction, and eleven displacements in exact progression could not survive
that reading.

### Ghidra source mode works — analyzeHeadless recipe

`8051_main.sinc:234` defines `srcMode` as a context bit defaulting to 0. Setting
it to 1 over the whole block makes Ghidra decode this firmware correctly:

```sh
analyzeHeadless <proj> px716 -import rome_111.bin \
  -processor "80251:BE:24:default" -loader BinaryLoader -loader-baseAddr 0xF00000 \
  -scriptPath <dir> -preScript SetSrcModeAndSeed.java -postScript Report251.java
```

The prescript sets `srcMode` via `getProgramContext().setValue(...)` over
`getLoadedAndInitializedAddressSet()`, then disassembles the ECALL-derived
seeds. Results:

* `SRCMODE: set to 1 over [[RAM:f00000, RAM:feffff]]` — block placed exactly at
  the derived base.
* **`SEEDS: 2048/2048 disassembled`** — every ECALL target is a valid
  instruction boundary. A wrong base or wrong mode would fail many.
* **33.1% of the image resolved to code** from ECALL seeds alone.
* Independent confirmation of the hand analysis: the dispatcher chain at
  `0xfbffa8` lands inside function **`0xfbff8e`** — exactly where hand
  disassembly found the `PUSH dr12` prologue — and `0xf00050` is correctly
  *not* code.

**Tooling trap:** Ghidra caches compiled scripts per directory and reports
`"<scriptPath> hasn't changed, with 1 file failing in previous build(s)"`,
re-showing a **stale** compiler error after the file is fixed. Rename the script
or `touch` the directory to force a rebuild; re-run against the saved project
with `-process <name> -noanalysis` rather than re-importing.

### Command surface expanded to 42 opcodes — still a floor

The dispatcher's default arm calls a common reject routine at **`0xFDE7C5`**
(81 inbound references in Ghidra; 325 ECALL sites image-wide). Scanning
backwards from those sites for a chain finds 38 dispatchers and raises the union
to **42 opcodes**:

* MMC: `00 01 03 04 08 0a 12 1a 1b 1e 28 2a 2e 35 3b 44 46 4a 52 53 54 58 5a 5b
  5c a1 a8 aa ac ad b9 be bf`
* vendor: `d8 d9 df e4 eb f2 f3 f4 f5`

This independently recovers `d9`, `f2` and `f4` as dispatch arms, corroborating
the hardware result by a second route. **Four known-implemented opcodes are
still unlocated** — `0x3C` READ BUFFER, `0xB6` SET STREAMING, `0xBB` SET CD
SPEED, `0xE9` vendor MODE — so at least one further dispatch idiom exists. Do
not read 42 as the command surface.

## The second dispatch mechanism — an indirect table, and why the enumeration stops there (session 6, 2026-08-31)

The four known-implemented opcodes missing from the chain harvest (`0x3C`,
`0xB6`, `0xBB`, `0xE9`) are dispatched by a **different mechanism**, now
identified. This closes the question rather than leaving it open.

### Finding every dispatch site, idiom-independent

The chain dispatcher loads the CDB opcode from **dir16 `0xB7E7`**. Every dispatch
site must touch that register whatever its idiom, so `7e/7a <sd> b7 e7` enumerates
them all: **98 references**. Classified by what follows the load:

| follows | count | idiom |
|---|---|---|
| `0x0a` MOVZ | 34 | **indirect table dispatch** (below) |
| `ADD/JE` chain | 19 | the subtract-and-branch chains already documented |
| `0xb4` CJNE | 16 | compare against a literal |
| `0xbe` CMP | 9 | compare against a literal |
| other | 20 | — |

A generalised sweep confirms **`0xB7E7` is the only location in the image whose
load is followed by an opcode-valued chain** — there is no second opcode register.

### The indirect dispatch (Ghidra, srcMode=1)

```
MOV  R7,0x00b7e7        ; CDB opcode
MOVZ WR2,R7             ; widen to word
XRL  WR0,WR0            ; clear high half
MOV  A,#0x2
ADD  DR0,DR0 / DEC A / JNE   ; x2 twice -> DR0 = opcode * 4
ADD  WR0,#0x85
ADD  DR0,#0x6111        ; + per-site table base
MOV  WR6,@DR0+0x20b     ; load handler pointer from the table
ECALL @DR4              ; INDIRECT call
```

The per-site table bases are `0x6111`, `0x6511`, `0x6911`, `0x6d11` — **stride
`0x400`**, exactly 256 entries x 4 bytes. So this is a classic opcode-indexed
function-pointer table, with several tables (per drive state, matching the
chains' role as state filters).

**This explains the enumeration gap exactly.** Under table dispatch the opcode is
never compared against a literal, so it appears in no chain — and indeed `0xE9`
occurs as a compare immediate **nowhere in the image**. `0x3C`/`0xB6`/`0xBB`
likewise have no compare site within 400 bytes of any `0xB7E7` access.

### Why the static enumeration stops here — a bounded negative

* The table base `0x85xxxx` is **outside** the flash window `0xF00000-0xFEFFFF`,
  so the tables live in **RAM**, populated at boot.
* They are not copied verbatim from flash: no 1024-byte window in the image reads
  as 256 in-range 32-bit code addresses (searched; zero hits). The entry loaded is
  16-bit (`MOV WR6,...`) at a 4-byte stride, so the table is *constructed*, not
  stored.
* Consequently the speed-ladder function `0xF65B36` has **no inbound references
  at all** in Ghidra's database — it is reached only through the indirect call.

So **opcode -> handler cannot be completed by static call-graph analysis alone**
on this firmware. Completing it needs either the boot-time code that populates
`~0x85631C`, or a live read of drive RAM — and the Phase 1 sweep already
established there is no exposed RAM window (`READ BUFFER` id 0 is the 8 MiB data
buffer and nothing else exists).

### Working model of the command path

Two cooperating mechanisms, offered as a **hypothesis** consistent with all
evidence, not as established fact: the subtract-and-branch chains act as
**per-drive-state legality filters** (the `0xfbffa8` chain holds exactly the
no-media-legal commands), while the indirect table performs the **actual
dispatch**. That would explain why both exist, why the chains are partial, and
why they are entered after a state-register bit test.

### Caveat on operand-level detail

`80251.sinc` opens with `NOTE! 80251 implementation is preliminary and has not
tested !!`. The instruction *stream* is validated here from several directions
(2048/2048 seeds, the RPC1 patch semantics, the speed ladder's arithmetic
progression), but individual **operand** renderings — register naming in
`ECALL @DR4`, the exact widths above — should be re-derived before anything is
built on them.

## Timed delivered-rate experiment — the ladder confirmed on hardware (session 6, 2026-08-31)

Measured, not inferred: page 2A reports the *request*, so the only honest figure
is a timed read. Tool: `re-tools/mmcspeed.c` (+ `mmcquant.c` for the quantiser
sweep). Same LBA range every trial so CAV radius is constant, 8 MiB buffer
evicted before each trial, 11.8 MB read per trial so the buffer cannot serve it.
Pressed audio CD, lead-out LBA 163404.

### The quantiser has five steps for CD-DA, and all five are on the firmware ladder

Sweeping `SET CD SPEED` and reading back page 2A's accepted value:

| requested | accepted | |
|---|---|---|
| 100 kB/s (0.6x) | 706 (**4x**) | floor — will not go slower |
| 1500 (8.5x) | 1411 (**8x**) | |
| 4300 (24.4x) | 4234 (**24x**) | |
| 5700 (32.3x) | 5645 (**32x**) | |
| 7100 (40.2x) | 7056 (**40x**) | ceiling |

**Every accepted value is a member of the 12-step ladder read out of the
firmware at `0xF65B83`** (1 2 4 8 10 12 16 20 24 32 40 48x). Five quantiser
outputs all landing inside a 12-element subset of the possible kB/s range is not
coincidence — this is the static finding confirmed empirically. For CD-DA only
5 of the 12 steps are offered; **1, 2, 10, 12, 16, 20 and 48x are never used**,
which independently corroborates that the 48x SpeedRead band is Mode-1 only.

### Delivered rate vs requested

| requested | page 2A | delivered |
|---|---|---|
| max | 40.0x | **29.3x** (5168 kB/s) |
| 24x (and 28.3x) | 24.0x | **15.3x** (2703 kB/s) |
| 8x .. 21.5x | 8.0x | **6.7x** (1177 kB/s) |
| 4x .. 5.7x | 4.0x | **3.5x** (614 kB/s) |

Uncapped delivery of 29.3x at LBA 150000 is **the CAV curve, not a fault**.
Taking nominal Red Book radii (program area 25-58 mm) and LBA proportional to
area, LBA 150000 of a full-size disc sits at r ~ 42.0 mm = 0.725 of the outer
radius, predicting 40x x 0.725 = **29.0x** against 29.3x measured — agreement
within ~1%. At the capped settings the shortfall is command-turnaround overhead
(26 sectors per `READ CD`), not a governor.

### Both exposed levers work, and are equivalent

`SET STREAMING` (0xB6) at 8x delivers 1177 kB/s — **identical to `SET CD SPEED`
(0xBB) at 8x**. So the two exposed levers over read speed both function and land
on the same quantised step.

**A/B on the CDB, because this bug has bitten this project before:** the 0xB6
parameter-list length lives at **CDB bytes 9-10**, not 8-9. Placing it at byte 9
returns `4/1b/00` and looks exactly like "this drive does not support SET
STREAMING"; placed correctly it returns good status. Reproduced deliberately
here as a paired test so the failure signature is on record.

### Tooling defect found and fixed

`re-tools/sgsend.c` had a fixed 512-byte data-in buffer with **no bound check on
`--in`**, so `--in 2352` (one raw CD sector) smashed the stack. Buffer raised to
64 KiB and both `--in` and `--pl` now bounds-checked.

## Public-source search on 0xD9 / 0xF2 / 0xF4 — and a retraction (session 6, 2026-08-31)

Full report: `private/research/incoming/plextor-vendor-opcodes-d9-f2-f4.md`.

**RETRACTED: "0xD9 is a READ CD-DA MSF variant."** That was my inference from
its firmware chain adjacency to 0xD8 (READ CD-DA) plus a half-remembered
D8=LBA / D9=MSF pairing. Checked against primary sources and **refuted**:
cdrtools uses opcode 0xD8 only (4 call sites; C2 is selected by a CDB flag, not
a second opcode); libcdio's vendor-unique enum lists C4/C9/D8/DB/DF with no
0xD9; FreeBSD CAM's Plextor quirk table has a single 0xD8 entry; redumper's
operation-code enum, QPxTool's `qpx_opcodes.h`, and DiscImageCreator likewise
have nothing. **No public source associates 0xD9 with Plextor at all.** All we
may say is what the drive said: it parses the CDB and rejects on track mode.

**0xF2 and 0xF4 are genuinely undocumented** — zero opcode-position hits across
seven independent sources, so this is a real absence rather than a narrow
search. (Two apparent local hits were false: QPxTool's `0xF2` at
`qscan_cmd.cpp:46` is CDB byte 1 of a *BenQ* 0xFD command, and `0xF4` at
`pioneer_spdctl.cpp:21` is CDB byte 2 of a *Pioneer* command. Neither is an
opcode.)

**Two facts that did surface, both primary-sourced:**

* **`0xF1` = `PLEXTOR_EEPROM_READ`** (QPxTool) — fills a blank in our 17-opcode
  inventory.
* **`0xF8` is a real Plextor opcode**, blacklisted from `pxfw`'s brute-force
  prober alongside `0xDE`/`0xDF`. Function unknown, but **its presence on a
  hazard blacklist is itself the finding** — do not probe it casually. It is a
  21st candidate we had not identified.

Apparent conflicts on `0xD5`/`0xF1` between tools are same-number reuse across
different vendors' namespaces, not contradictions.

**Consequence:** the function of `0xD9`/`0xF2`/`0xF4` is not recoverable from
public sources. Our own firmware dispatch mapping is now the only primary
source on them — which makes the RE work the reference rather than a
duplicate of one. Gaps not covered: dvd+rw-tools (host unreachable), PxScan /
CDVDlib (closed source), and a dedicated MyCE/CDFreaks forum-archaeology pass.

## An LLM attribution for 0xD9 / 0xF2 / 0xF4 — checked and REFUTED (session 6, 2026-08-31)

A Gemini-sourced claim (relayed by Keith, explicitly "with a pinch of salt")
assigned specific functions to the three new opcodes. Checked against primary
sources on disk. **The two specific function assignments are refuted, because
both functions are already assigned to different opcodes.**

| claim | check | verdict |
|---|---|---|
| `0xF2` = "Q-Check PI/PO read" | QPxTool `qpx_opcodes.h:131` — `PLEXTOR_QCHECK = 0xEA` | **REFUTED** |
| `0xF4` = "Read Beta/Jitter" (analogue OPU) | Beta/Jitter is `0xEA` — QPxTool `qscan_cmd.cpp:125,140,381` (`cmd_cd_jb_init`/`cmd_dvd_jb_init`/`cmd_jb_getdata`). *(This cell first cited `0xF3`/`0xF5` as the Beta/Jitter pair; those are FE/TE and TA. Verdict unchanged, supporting detail corrected 2026-08-31.)* | **REFUTED** |
| `0xD9` = part of the "GigaRec / SilentMode / VariRec sub-control engine" | that engine is a *single* opcode, `0xE9`, page-selected (GigaRec=page 0x04, VariRec=0x02, SilentMode=0x06/07/08) — live-verified in session 3 | **REFUTED** |
| "triggers when PlexTools runs …" | our own PlexTools CDB harvest found `0xEA` but **not** `0xF2`/`0xF4`/`0xD9` — PlexTools never issues them | **REFUTED** |

No QPxTool source uses `0xF2`, `0xF4` or `0xD9` in opcode position at all.

**The failure mode is worth naming, because it is the same one as the earlier
LC87 claim about this chip: the right neighbourhood with invented specifics.**
The claim's framing — that `0xF0`-`0xF5` is Plextor's diagnostic block — is
*true* (`0xF1` EEPROM read, `0xF3` TA/FE-TE scan, `0xF5` FE-TE readout). Real
surrounding facts make a fabricated specific assignment read as credible. Treat
an LLM assertion about this part exactly as any other unverified claim: check
the checkable parts first.

### What we do know about 0xD9, measured

`0xD9` is **not** a drop-in READ CD-DA variant: given the exact CDB shape that
`0xD8` accepts (LBA 1000, 1 block), `0xD8` returns 2352 bytes and **`0xD9`
returns `5/24/00`**. Its CDB layout differs. Field map, one byte set to `0x01`
at a time against an all-zero baseline on a pressed audio CD:

| CDB byte | sense | reading |
|---|---|---|
| 1, 2, 3, 4, 6 | `5/24/00` INVALID FIELD IN CDB | **validated** — `0x01` is illegal there |
| 5, 7, 8, 9, 10 | `5/64/00` ILLEGAL MODE FOR THIS TRACK | **accepted** — reaches the track-mode check |
| (all-zero baseline) | `5/64/00` | — |

So `0xD9` parses a structured CDB and operates on **disc content** — it reaches
a track-mode decision. `5/64/00` on an all-audio disc suggests it wants a track
mode this disc does not provide. **The decisive next experiment is to repeat
this against a Mode-1 data disc**, which needs different media in the drive.

### A second LLM theory for 0xD9 — also REFUTED, by data already in this file

Relayed by Keith the same evening: `0xD9` as a "Vendor-Specific Subchannel /
Session Interrogation" command, with `CDB[2]` = target track number or session
index, and `5/64/00` explained as the requested subchannel matrix not existing
yet on blank or unfinalised media.

Both novel claims are contradicted by measurements already recorded above.

| claim | check | verdict |
|---|---|---|
| `CDB[2]` = target track number / session index | The field map is on this page: `0x01` at byte 2 returns **`5/24/00` INVALID FIELD IN CDB**, on a pressed audio CD whose track 1 plainly exists. A track-number field must accept 1 | **REFUTED** |
| `5/64/00` means "blank or single unfinalised track — the subchannel matrix does not exist yet" | `5/64/00` came back **identically** from a finalised pressed multi-track audio CD, a pressed data CD-ROM, an appendable CD-R with a data track, and a blank CD-R. The explanation predicts variation across exactly the axis we swept and found constant | **REFUTED** |
| a subchannel/session interrogation command | On DVD `0xD9` returns `5/20/00` INVALID COMMAND OPERATION CODE — the opcode ceases to exist. Session and subchannel interrogation are not CD-only concepts | **unexplained by the theory** |
| "vendor-specific Read Sub-channel" as a known legacy command | No public source uses `0xD9` in opcode position — QPxTool, cdrtools, libcdio, redumper, DiscImageCreator, FreeBSD all checked earlier in this session | **unsupported** |

**Third instance of the same failure shape, and now it is a pattern worth
stating as a rule.** Everything the theory gets right — the structured CDB, the
3-bit field at `CDB[1]` bits 7:5, the track-mode gate — is *our own trace, fed
back to it in the prompt*. Its only original content is the two rows above, and
both are false. The LC87 claim and the Q-Check claim failed the same way: real
surrounding facts, restated, wrapped around an invented specific.

**So the check that has power is not "is this plausible?" but "what does this
claim assert that we did not tell it?"** Isolate that, and test only that. Here
it took two greps of this file and no drive time at all.

## Media sweep of 0xD9 / 0xF2 / 0xF4 — validation order, and 0xF4 executes (session 6, 2026-08-31)

All earlier results were taken against a single profile (`0x0008`, CD-ROM), so
the medium was swept. Tool: `re-tools/mmcvendor.c`.

| medium | `0xD9` | `0xF2` | `0xF4` |
|---|---|---|---|
| **none** (tray open) | `2/3a/02` MEDIUM NOT PRESENT | `2/3a/02` | `2/3a/02` |
| **pressed CD-ROM** (audio tracks) | `5/64/00` ILLEGAL MODE FOR THIS TRACK | `2/30/05` CANNOT WRITE MEDIUM | `5/24/00` INVALID FIELD IN CDB |
| **CD-R** (data track, appendable) | `5/64/00` (unchanged) | *gated, not probed* | **status 00 — ACCEPTED** |

### What this establishes

**A validation order:** medium presence → medium type → CDB fields → track
mode. Each opcode's failure point then reads as a statement of what it still
needs, which a single sense code cannot give you.

* **`0xF4` requires recordable media.** It is rejected on a pressed disc
  (`5/24/00`) and **accepted on a CD-R**, returning good status and zero bytes.
  Three candidate causes remain unseparated by two data points — recordable
  *media type*, *writable state*, or the presence of a *data track*. A blank
  CD-R discriminates: it is recordable and writable but has no data track.
  Field map on CD-R: **only CDB byte 4 is validated**; bytes 1,2,3,5-10 all
  accept `0x01` and still return good status.
* **`0xD9` is not satisfied by a data track either.** It returns
  `5/64/00 ILLEGAL MODE FOR THIS TRACK` on *both* an all-audio disc and a
  data-track CD-R, with an identical field map on both. So it wants a track
  mode neither disc provides, or a CDB field selects a mode and the zeroed
  default is never valid. Value-sweeping the validated bytes (1,2,3,4,6) is the
  next step, and must be done on **unwritable** media.
* `0xF2` remains untested beyond the pressed disc; it needs a writable medium
  by construction, which is the one case that carries real risk.

### The safety gate was wrong, and is now fixed

The gate originally covered only `0xF2`, on the reasoning that its sense code
(`2/30/05 CANNOT WRITE MEDIUM`) proved it write-side, whereas `0xF4` merely
rejected a CDB field and looked inert. **That reasoning was wrong**: `0xF4`
looks inert on read-only media and *executes* on a CD-R. The disc in question
was expected to be a pressed CD-ROM and turned out to be a CD-R, so an ungated
opcode ran against writable media.

**Rule now enforced in the tool: gate on the MEDIUM, never on a per-opcode
guess about which opcodes are dangerous.** No opcode of unknown function is
issued against a writable medium without an explicit `--allow-write-probe`.
An opcode's behaviour against read-only media tells you nothing about its
behaviour against writable media — which is the entire reason the gate exists.

The affected disc was checked afterwards and shows no damage: TOC intact
(1 data track, lead-out 239676), `READ DISC INFORMATION` consistent, and five
sampled LBAs read with good status. **Byte-identity cannot be proven** — there
was no prior checksum — and that limitation is recorded rather than glossed.

## Blank CD-R probe — 0xF2/0xF4 require writable media but WRITE NOTHING (session 6, 2026-08-31)

Run against an expendable blank CD-R with `--allow-write-probe`, Keith
consenting to lose the disc. Probe order changed so `0xF2` runs **last**, and a
full disc-state snapshot was taken **before and after** — the measurement the
previous phase could not make.

| medium | `0xD9` | `0xF2` | `0xF4` |
|---|---|---|---|
| none | `2/3a/02` | `2/3a/02` | `2/3a/02` |
| pressed CD-ROM (audio) | `5/64/00` | `2/30/05` | `5/24/00` |
| CD-R, data track, appendable | `5/64/00` | *gated* | **accepted** |
| **blank CD-R** | `5/64/00` | **accepted** | **accepted** |

### The disc was not written to

Before/after `READ DISC INFORMATION`, `READ TRACK INFORMATION` and ATIP are
**byte-identical**: disc status `00` (empty), NWA `ffffff6a` (LBA −150), free
blocks 359335. Twenty-two executions of `0xF2`/`0xF4` returning good status
changed nothing observable, and the disc remains blank and usable.

**So `0xF2` and `0xF4` require recordable/writable media but perform no write.**
`0xF2`'s `2/30/05 CANNOT WRITE MEDIUM` on a pressed disc was a *write-class
precondition check*, not evidence that it writes. "Requires recordable media,
writes nothing" is the signature of a **calibration or measurement** command —
OPC, test-write, or reading recordable-only structures such as the PCA.

That partially rehabilitates the *spirit* of the refuted LLM claim (these are
diagnostics) while leaving its specific assignments refuted: Q-Check is `0xEA`
and the analogue FE/TE pair is `0xF3`/`0xF5`.

**Hypothesis eliminated:** the blank has **no data track**, yet `0xF4` is still
accepted — so `0xF4` needs recordable/writable media, *not* a data track.
Recordable-media-type and writable-state remain unseparated; a **finalised**
CD-R would separate them.

### CDB structure recovered by value-sweeping

| field | accepted values | structure |
|---|---|---|
| `0xD9` byte 1 | `00 20 40 60 80 a0 c0 e0` | **3-bit field at bits 7:5**; bits 4:0 must be 0 |
| `0xD9` byte 2 | `00` only | reserved |
| `0xF4` byte 4 | `00`, `80`–`ff` | **single flag at bit 7**; bits 6:0 must be 0 |

### Limitation of the 0xD9 sweep, stated rather than buried

That sweep ran on a **blank** disc, which has no tracks, so
`ILLEGAL MODE FOR THIS TRACK` could not have passed for *any* byte-1 value.
The result therefore establishes byte 1's **shape** but says nothing about
which of its 8 values is correct. Repeat on media that has tracks — ideally a
**pressed data CD-ROM**, which is the only medium that is both unwritable
(so an executing `0xD9` is safe) and carries a data track.

## Pressed data CD-ROM — a correction to "0xF4 requires recordable media" (session 6, 2026-08-31)

Fourth medium: pressed CD-ROM (profile `0x0008`, no ATIP) **with a data track** —
the only medium that is both unwritable and carries a data track, so `0xD9` can
be swept safely even if it executes.

### CORRECTION: 0xF4 does not simply require recordable media

Published earlier in this document as "`0xF4` requires recordable media". **Too
strong.** Byte 4 bit 7 is a *mode flag*, and the media requirement applies to
only one of the two modes:

| `0xF4` byte 4 | pressed CD-ROM | CD-R |
|---|---|---|
| `0x00` (bit7=0) | `5/24/00` rejected | **accepted** |
| `0x80`-`0xff` (bit7=1) | **accepted** | **accepted** |

So with bit 7 set the command is accepted on *any* medium. The earlier
conclusion was drawn before byte 4 had been value-swept on pressed media — the
field map only ever set it to `0x01`, which is rejected in both modes.

### 0xF4 returns no data, ever

With byte 4 = `0x80` and a **512-byte** allocation, `0xF4` returns **0 bytes**
for the baseline and for `0x08` in every one of CDB bytes 1,2,3,5,6,7,8,9,10.
It is therefore **not a data-returning read command** — it is a set / trigger /
no-data command. This is a further, independent refutation of the LLM claim
that `0xF4` is "Data In" and "returns RF signal quality metrics": it returns
nothing.

Also mapped: with bit 7 set, byte 1 rejects `0x02` and `0xff` but accepts
`0x01`/`0x08`/`0x20`, so byte 1 bit 1 must be zero.

**A test that was run and must NOT be counted as evidence:** the
trigger-then-readout hypothesis (`0xF4` arms a measurement, `0xF3`/`0xF5` read
it out) was tested by issuing `0xF5` before and after `0xF4`. Both returned 0
bytes — but so did `0xF5` on its own, and `0xF5` is a *known* readout command
(`PLEXTOR_FETE_READOUT`). Its own CDB must therefore also be wrong, so the test
cannot distinguish "no change" from "my CDB is wrong". **The control did not
work, so the result has no power.** Recorded so it is not later mistaken for a
negative finding.

### 0xD9 has now failed on every CD medium

| medium | byte-1 sweep result |
|---|---|
| pressed CD-ROM, audio tracks | all 8 values → `5/64/00` |
| CD-R, data track | all 8 values → `5/64/00` |
| blank CD-R, no tracks | all 8 values → `5/64/00` |
| **pressed CD-ROM, data track** | all 8 values → `5/64/00` |

Identical field maps throughout. `0xD9` is satisfied by no CD medium of any
type, and the only untested medium class left is **DVD**.

## Pressed DVD — the command surface itself is media-dependent, and 0xF2 RUNS (session 6, 2026-08-31)

Fifth medium: pressed DVD-Video (profile `0x0010` DVD-ROM, no ATIP, unwritable).

| | `0xD9` | `0xF4` | `0xF2` |
|---|---|---|---|
| pressed CD-ROM (audio) | `5/64/00` | `5/24/00` (bit7=0) / accepted (bit7=1) | `2/30/05` |
| pressed CD-ROM (data) | `5/64/00` | as above | `2/30/05` |
| CD-R (data / blank) | `5/64/00` | accepted | accepted |
| **pressed DVD-ROM** | **`5/20/00`** | **`5/20/00`** | **accepted** |

### The drive's command surface changes with the medium

`5/20/00` is INVALID COMMAND OPERATION CODE — the same sense the negative
controls (`0xC1`, `0xC5`) return for opcodes that do not exist. So with a DVD
loaded, **`0xD9` and `0xF4` cease to exist**: they are CD-only commands.

**This is direct empirical confirmation of a static prediction.** The firmware
analysis found ≥12 subtract-and-branch dispatch chains, each entered after a bit
test on a state register, and inferred they were *per-drive-state command
filters*. Loading a DVD makes two opcodes disappear — exactly that behaviour,
observed independently of the disassembly.

### CORRECTION: "0xF2 requires writable media" is false

`0xF2` is accepted on a **pressed, unwritable** DVD-ROM. Rejected on CD-ROM,
accepted on CD-R and DVD-ROM. That is the third generalisation about these
opcodes withdrawn in one session, all the same shape — **a rule inferred from N
media states, falsified at N+1**. Record the measurement table; do not state a
rule until the media axis is exhausted.

### 0xF2 executes a long, quiet physical operation — NOT a hang

Probing `0xF2` with non-zero CDB parameters on the DVD made the drive stop
answering: `TEST UNIT READY` returned `host=07` (DID_ERROR), block reads,
`eject` and INQUIRY all timed out, while the kernel still reported the device
`running`. `SG_SCSI_RESET` needs privilege we do not have. Keith power-cycled
the drive, which fully recovered it (INQUIRY normal, no lasting harm).

**Initial reading — "the drive is wedged" — was WRONG, corrected by Keith's
direct observation of the front panel:** the LED blinked *twice, roughly every
5 seconds*, very quietly, with no spindle or seek noise. The drive was **busy
executing a long-running operation**, not crashed; the timeouts were the
transport giving up on a drive that was mid-command. A slow periodic
double-blink with a near-stationary disc is the signature of a **physical
calibration** — laser power measurement / servo test — which corroborates the
"calibration or measurement command" reading of `0xF2` from an entirely
independent channel. It ran on an *unwritable* disc, so nothing could be
written.

**Consequences for method:**
* Give unknown vendor opcodes **minutes-long** SG_IO timeouts, and expect the
  drive to be unavailable meanwhile. Do not read a timeout as a hang.
* **Trace every CDB to stderr, unbuffered, BEFORE issuing it.** The probe's
  stdout was block-buffered, so when the process was killed the entire log was
  lost and *which* parameter started the operation is still unknown. Now fixed
  in `re-tools/mmcvendor.c` (`setvbuf` + pre-issue CDB trace).
* Front-panel behaviour is data a probe cannot see. Ask.

### Was 0xF2 the firmware-upload command? No — checked, refuted

Keith raised the possibility after observing the drive's behaviour, and it was
worth taking seriously: an unknown opcode triggering a flash write with no valid
payload could brick the drive. **Refuted from primary source.** QPxTool ships
`pxfw`, a Plextor *firmware* tool; `console/pxfw/pxfw.cpp` uses **`0x3B`
(standard SCSI WRITE BUFFER)** at lines 173 and 200 for the firmware path, plus
`0xF1` (EEPROM read) at line 149. It does not use `0xF2`. Firmware upload on
these drives goes through standard MMC `WRITE BUFFER`, which this project has
never issued and which remains on the never-probe list.

**Drive state verified intact** against the baseline captured earlier the same
day: 11 mode pages / 10 changeable with byte-identical masks, 34 features,
`READ BUFFER` capacity 8355840, page 2A max read `1b90`. Two bytes differ and
both are *live* state rather than stored settings — page 2A byte 6 `2b`→`29` is
the tray **lock-state** bit, cleared deliberately by our own `ALLOW MEDIUM
REMOVAL`, and page 0x0D's inactivity timer was reset by the power cycle.

**What 0xF2 actually did is still unknown, and "silence" narrows it further.**
Keith clarified that the drive was *silent* — not merely quiet. No rotation at
all. That rules out the disc-calibration reading as well, since laser power
calibration requires the disc to spin. What remains: an internal operation
touching no mechanism (an EEPROM/NVRAM access — note `0xF1` is EEPROM *read*,
though opcode adjacency has already produced three retractions this session and
is not evidence), or a command **blocking on an internal timeout** waiting for a
hardware condition that never arrives, with the LED pattern signalling that
wait. Both fit; nothing distinguishes them yet.

**0xF2 is hereby classed with the DANGER opcodes** (`0xE3` PlexEraser, `0xEE`
reset, `0xF8`): do not probe it further without a specific hypothesis and a
plan, because it blocks the drive for minutes and needs a power cycle to clear.
Identifying *which* CDB triggers it is now possible — `mmcvendor.c` traces every
CDB to stderr unbuffered before issuing it — but costs another block-and-power-
cycle and should not be done casually.

### 0xF1 EEPROM read works — and gives us the baseline we lacked

Keith noted that `0xF2` sits directly beside `0xF1` (`PLEXTOR_EEPROM_READ`).
Adjacency is a hypothesis generator, not evidence — it has produced three
retractions this session — **but unlike the earlier guesses this one is
testable, because the sibling's CDB is documented in source we hold.**

`pxfw` shows the PX-716-specific form. Plain `0xF1` fails on this drive and the
code retries with a **sub-command selector in byte 1**:

```
F1 01 00 00 00 00 00 <block> <sz_hi> <sz_lo> 00 00     -> 256-byte read
```
(`console/pxfw/pxfw.cpp:149`, `lib/qpxplextor/plextor_features.cpp:45-60`,
whose comment reads *"The Plextor PX-716 does not understand this command…"*)

Verified live. The EEPROM is **4 blocks x 256 = 1024 bytes**; block 4 returns
CHECK CONDITION, so that is the whole device. All four blocks are distinct and
none is blank. Block 0 carries the drive identity string; block 3 contains a
smooth monotonically-descending 16-bit sequence, consistent with a calibration
or power table.

**A dump is now stored at `private/drives/Plextor/eeprom/` as a durable
baseline.** It stays in the git-ignored tree deliberately: it contains the
drive's identity/serial data and must not enter the public repo, so no EEPROM
contents are reproduced in this document. Tool: `re-tools/eedump.c`.

The value is that the earlier `0xF2` incident could only be assessed against
mode pages and the feature list. **Any future change to non-volatile drive
state is now detectable by diffing against this dump** — which is exactly what
was missing when it mattered.

That byte-1 convention also sharpens the open question: `0x01` is the PX-716
sub-command selector for `0xF1`, and `0x01` was among the first values the
`0xF2` field map tried. The long silent operation was plausibly started by
`F2 01 …`. Plausibly — the trace that would have proved it was lost to output
buffering, which is why the tool now traces every CDB before issuing it.

## The documented-feature audit, and what the manual's LED codes say (session 6, 2026-08-31)

Keith's question: *do we already know all the opcodes for every **documented**
feature of the PX-716?* Answering it required enumerating from the documented
side inward rather than from our opcode table outward — the full audit is in
`FEATURES.md`. Three results belong here.

### The EEPROM dump validates itself, and it is the Silent Mode persist target

QPxTool decodes the *saved* (flash-persisted) Silent Mode block out of the
EEPROM, not out of a mode page: `plextor_get_silentmode_saved()`
(`lib/qpxplextor/plextor_features.cpp:338-359`) calls `plextor_read_eeprom()`
and reads offsets `0x100`-`0x108`. Applied to our own 1 KB dump from `0xF1`:

```
0x100:  00 00 28 10 30 10 ff ff ff
        |  |  |  |  |  |
        |  |  |  |  |  +-- 0x10 = 16   DVD write max
        |  |  |  |  +----- 0x30 = 48   CD  write max
        |  |  |  +-------- 0x10 = 16   DVD read  max
        |  |  +----------- 0x28 = 40   CD  read  max
        |  +-------------- access time: 0 = FAST
        +----------------- saved-state flag: 0 = no saved Silent Mode
```

Four consecutive bytes decode to **exactly** the PX-716A's four published
maxima — 40x CD read, 16x DVD read, 48x CD write, 16x DVD write. That is not
coincidence, and it establishes three things at once: QPxTool's offset map is
right for this drive, our dump is correctly aligned and real, and the
documented "Save Changes To Drive ... into the drive's flash memory" persists
**into the region `0xF1` can read**.

That last point matters for the `0xF2` DANGER classification. The worry was
that `0xF2` might write non-volatile state our 1 KB baseline could not see. It
remains true that the baseline covers only what `0xF1` returns and that there
is no exposed window onto the rest (`READ BUFFER` id 0 is the 8 MiB data
buffer and nothing else exists) — so "EEPROM unchanged after `0xF2`" is still
**not** proof that `0xF2` wrote nothing. But the one documented non-volatile
write on this drive lands inside the baseline, which is better than we knew.

**Narrow retraction.** The refutation of "`0xF2` = EEPROM write" rested on
three legs; leg (b), the arithmetic ("1 KB at 10 ms/byte is ~10 s, the
observed run was minutes"), is withdrawn as a *general* timing argument — it
sized the 1 KB EEPROM, and a flash erase/program cycle on 2005 hardware is
minutes, silent and non-rotating. Legs (a) dispatch grouping and (c) media
gating stand, and the audit adds an independent third: Silent Mode save **is**
a PlexTools feature, so its opcode is in the 120-call-site harvest, and `0xF2`
is not. The conclusion is unchanged; one of its supports was measuring the
wrong device.

### The manual documents an LED code that matches the 0xF2 event

`Plextor-716.pdf` is described above as "no SCSI content ... feature naming,
nothing else". That undersells it: §6 "Using the PX-716A Self-Test
Diagnostics" (p.100-103) publishes the drive's diagnostic LED vocabulary.

> *If there was a problem, the disc is not ejected, and you see the LED
> indicator blink green. One green blink indicates a write or read error.
> **Two green blinks indicate an initialization error.***

The observed `0xF2` event was: silent, no rotation, LED **blinking twice
roughly every 5 s**, drive unresponsive until power-cycled. The self-test also
**requires a blank DVD+-R or CD-R** and reports the wrong medium separately —
which rhymes with `0xF2`'s media gating (`2/30/05 CANNOT WRITE MEDIUM` on a
pressed CD).

**This is a hypothesis with a named weakness, not a finding.** The documented
self-test is entered by *hardware* means only — an extra jumper plus holding
eject at power-on, **with the interface cable physically disconnected** (both
the 716A and 716SA procedures say so). So the routine is specified as
unreachable from the host, and the 2-blink code is documented as a *completion*
result, whereas what we saw was a repeating pattern. What the manual does
establish is that a repeating green double-blink is this drive's published way
of saying **initialization error**, which is a far better-sourced reading of
the observation than "wedged".

It also gives the `0xF2` classification a cheaper next step than any probe: if
that reading is right, the state was an error report, not an in-progress
operation. No further `0xF2` traffic is needed to test it, and none is
proposed — this is recorded so the observation is not re-interpreted from
scratch next time.

### Method note: these vendor documents are CP1252

Both the manual's extracted text and the CHM's HTML are CP1252. GNU grep in a
UTF-8 locale silently skips lines containing invalid multibyte sequences, so a
first pass over the help file reported that SpeedRead, Silent Mode and SecuRec
are **not documented** — three false negatives, no error, no warning. `iconv
-f CP1252 -t UTF-8` before grepping, or `grep -a`. Same shape as every other
silent-narrowing failure in this project: the output was well-formed, so
nothing downstream could catch it.

## Next steps (session 4+)

1. **Write-path features** (GigaRec/VariRec/SecuRec/AutoStrategy effects) —
   verify by burning once the write/burn path resumes; SET framing is known.
2. **PoweRec/QCheck detail** — 0xED and 0xEA sub-modes for reporting.
3. ~~**Firmware correlation.**~~ CPU identified session 5: Intel 80251.
   Internal layout partially mapped session 5 (above) — code confirmed real
   and coherent throughout the file, and page 0 has a verified interrupt
   handler (`0x1b1a`: `NOP`/`ORL 0x69,A`/`RETI`) with a cluster of real
   callers; base address / entry point / the bank-crossing mechanism remain
   open — the `0x50` SFR-pair candidate for the latter was chased and
   retracted, no replacement found.
4. **Selftest design** per feature for the driver's attach gate — SpeedRead is
   the model (GET → SET → observe page 2A → restore).
5. **Governor logic.** The read-side speed governor found in
   `docs/reference/RECOVERY.md` §12.10 has no library exposure and no located
   firmware routine yet — the reason this firmware RE thread was picked back
   up. Still blocked on item 3's base-address problem.

Only the 0xEA Q-Check counters are implemented in `plextor.c` today.
