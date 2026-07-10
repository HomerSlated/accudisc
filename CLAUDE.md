# AccuDisc

Low-level Red Book CD-DA read/write tool: a C shared library (`libaccudisc`),
a CLI (`accudisc`), and language bindings (Python, Rust). Think "a far more
advanced cdrdao", scoped strictly to CD-DA — audio CDs and the audio portion
of Mixed Mode discs. No ISO9660/CD-ROM data processing, no DVD/BD.

## Scope

- **Read** CD-DA with full data/metadata: TOC, subchannel (P–W), C2 error
  pointers, CD-Text, ISRC, MCN, pregaps/indices, lead-in/lead-out where the
  drive allows. Eventually SA-CD and CD+G (R–W subchannel graphics).
- **Write** Red Book CD-R/RW (DAO).
- **No post-processing, no lookups** — no CDDB/MusicBrainz, no analysis;
  AccuDisc only moves bits. Reads (whole disc / track / TOC / subchannel /
  targeted sectors, strategy chosen by the caller, some strategies gated on
  probed hardware support) are handed back to the calling application; writes
  accept TOC + BIN (+ optional SUB) from it.
- **Frame-accurate status surface** (core design constraint): per-sector
  read/write status trackable by the caller with minimal overhead — richer
  than a FIFO/pipe — to drive progress bars and EAC-style color-coded disc
  maps. Design target: shared-memory status map with atomic per-sector
  updates.
- Hardware access via MMC over SG_IO/ioctl plus **proprietary vendor opcodes**
  (drive/firmware reverse engineering is in scope; data from Redumper, EAC,
  dbPoweramp, etc. informs the drive database).

## Layout

- `include/accudisc/` — public API header(s), the installed interface
- `src/` — library internals, split by domain:
  - `transport/` — SCSI pass-through (SG_IO, ioctl), device discovery
  - `mmc/` — MMC command construction/parsing (READ CD, READ TOC/PMA/ATIP…)
  - `drive/` — drive identification, capabilities, quirks, read offsets,
    cache behavior, proprietary opcodes (Plextor, MediaTek D8, …)
  - `cdda/` — sector/frame model, C2 pointers, subchannel decode/deinterleave
  - `toc/` — TOC/session/track model, cuesheet in/out
  - `read/` — ripping engine (rereads, verification, offset correction)
  - `write/` — DAO burning engine
  - `meta/` — CD-Text, ISRC, MCN
- `cli/` — the `accudisc` command-line tool (thin layer over the library)
- `bindings/python/`, `bindings/rust/`
- `reference/` — third-party sources and imported code for analysis
  (**git-ignored**; local-only). Includes the original `c2read` once imported.
- `docs/`, `tests/`, `tools/` (dev scripts), `cmake/`

## Build

```sh
cmake -B build && cmake --build build
./build/cli/accudisc --version
```

C11, `-Wall -Wextra`. Public API is C, prefix `accudisc_` / `ACCUDISC_`;
opaque handles, no libc types leaking into the ABI where avoidable.

## Conventions

- The public header is the contract — bindings are generated/written against
  `include/accudisc/*.h` only, never against `src/` internals.
- Anything learned by reverse engineering (opcodes, quirk behavior) gets
  documented in `docs/` as it lands, with the drive/firmware it applies to.
- Reference sources in `reference/` are read-only inputs for analysis; code is
  rewritten for this codebase, not copy-pasted (mind licenses: GPL sources
  present).
