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
`../../docs/ATTRIBUTION.md`) after the user authorised it as a factual
cross-reference, then independently confirmed by raw SG_IO against the user's
own drive. **Correction to session 2: GigaRec is 0xE9 page `0x04`, not `0x06`
(see below).**

## Sources analysed

- `reference/Plextor/PTPXL/PTPXL.exe` — PlexTools Professional XL 3.x
  (PE32 x86, 8.6 MB, C++/RTTI). The application that builds and issues the
  vendor CDBs. Primary source.
- `reference/firmware/plextor/716A_111/rome_111.bin` — PX-716A firmware
  v1.11 (960 KiB). Not yet analysed (CPU/entry not yet identified).
- `reference/Plextor/Plextor-716.pdf` — end-user install manual only; no
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
| E9 | 0x489c20 | `e9 10 ..` | vendor MODE command (get/set by CDB[1]); **GigaRec = page 0x06** |
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

## Next steps (session 4+)

1. **Write-path features** (GigaRec/VariRec/SecuRec/AutoStrategy effects) —
   verify by burning once the write/burn path resumes; SET framing is known.
2. **PoweRec/QCheck detail** — 0xED and 0xEA sub-modes for reporting.
3. **Firmware correlation.** Identify the `rome_111.bin` CPU and cross-check
   the opcode/page set against the dispatch table.
4. **Selftest design** per feature for the driver's attach gate — SpeedRead is
   the model (GET → SET → observe page 2A → restore).

Only the 0xEA Q-Check counters are implemented in `plextor.c` today.
