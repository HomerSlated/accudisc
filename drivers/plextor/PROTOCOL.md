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
- `private/drives/Plextor/Plextor-716.pdf` — end-user install manual only; no
  SCSI content. Useful for feature naming, nothing else.

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
