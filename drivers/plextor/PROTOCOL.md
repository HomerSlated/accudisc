# Plextor vendor protocol — reverse-engineering notes (PX-716A)

**Licensing: non-free.** This file documents Plextor's proprietary vendor
opcodes and lives in the isolated driver zone on purpose — it is NOT part of
the MIT core and must never be referenced from `src/` or `docs/`. See
`LICENSE.md` in this directory. Clean-room provenance: derived by analysing
the user's own PlexTools binary and firmware for interoperability with
hardware the user owns; no vendor code is copied.

Working document. Session 1 (2026-07-12): method established, full vendor
opcode inventory harvested and validated. Opcode→feature binding and
parameter semantics are in progress.

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

## GigaRec — bound (session 2)

**GigaRec = Plextor vendor opcode 0xE9 (vendor MODE get/set), page 0x06.**
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
- CDB[2] = **page selector = 0x06 for GigaRec** (coherent with the
  `resp[1]==0x06` echo; the exact request byte carries the usual
  push-adjust caveat, MEDIUM-HIGH confidence)
- CDB[8] ≈ allocation length

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

Confidence: opcode 0xE9 and the rate-code table are HIGH (validated three
ways). The page value 0x06 and the GET/SET direction bit are MEDIUM-HIGH
(coherent with the response echo; final byte positions want the write-path
decode, which is deferred). No driver code written — the write/burn path is
paused, and GigaRec is a write-time feature, so this is documentation only
for now. When implemented, the GET form (0xE9 page 6, read current rate) is
the natural selftest anchor: read rate → set → re-read.

## Feature surface (remaining, from strings/RTTI, not yet bound)

RTTI classes still to bind: `CVariRec`, `CVariRecDVD`, `CSecuRec`,
`CSilentMode`, `CPoweRecInfo`. Given GigaRec resolved to an 0xE9 page, the
likely structure for the others is *also* 0xE9 pages (VariRec, SecuRec,
SilentMode, PoweRec) plus the 0xDF four-selector group for the SpeedRead /
single-session / book-type toggles. Bind each the same way: find its label
strings, the switch that decodes them, and the page the get-helper requests.

## Next steps (session 3+)

1. **Bind the remaining features** (VariRec/SecuRec/SilentMode/PoweRec via
   0xE9 pages; SpeedRead/single-session/book-type via 0xDF) using the
   GigaRec method above.
2. **Enumerate the 0xE9 page numbers** — one focused pass over 0x48cf20's
   callers listing each page selector and its decode switch would map the
   whole vendor-MODE page space at once.
3. **Firmware correlation.** Identify the `rome_111.bin` CPU and command
   dispatch; cross-check the opcode/page set.
4. **Selftest design** per feature for the driver's attach gate.

Only the 0xEA Q-Check counters are implemented in `plextor.c` today.
