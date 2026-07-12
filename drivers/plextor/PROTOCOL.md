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
| E9 | 0x489c20 | `e9 10 ..` | drive/media mode & status (get) |
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

## Feature surface (from strings/RTTI, not yet bound to opcodes)

RTTI class names present: `CGigaRec`, `CVariRec`, `CVariRecDVD`, `CSecuRec`,
`CSilentMode`, `CPoweRecInfo`. Q-Check test families in the strings: C1/C2,
PI/PO (SUM1/SUM8), FE/TE, Beta/Jitter, TA. GigaRec rate table present
(0.6×–1.4× plus Normal). These are the features whose opcodes we want to
finish binding.

## Next steps (session 2+)

1. **Opcode→feature binding.** Walk up from each vendor builder through its
   callers to the `C<Feature>::Apply`-style methods and the dialog strings;
   bind GigaRec/VariRec/SecuRec/SilentMode/SpeedRead/PoweRec/single-session/
   book-type to their opcode + subcommand + parameter byte.
2. **Parameter semantics.** For each bound command, decode the `rr` runtime
   bytes (which CDB positions carry the mode value, LBA, length, direction).
3. **Firmware correlation.** Identify the `rome_111.bin` CPU and command
   dispatch table; cross-check the opcode set and discover any commands
   PlexTools does not exercise.
4. **Selftest design.** For each feature we implement in `plextor.c`, define
   the read/set/re-read state proof the driver's selftest gate requires.

Nothing here is implemented in the driver yet; `plextor.c` still exposes
only the 0xEA Q-Check counters.
