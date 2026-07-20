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
- **Vendor isolation** (core design constraint): libaccudisc is pure MMC/SG.
  Proprietary/hardware-specific features live ONLY in external driver .so
  files (`drivers/`, ABI in `include/accudisc/driver.h`). Gate order:
  identify drive → locate driver → app permission (the attach call) →
  selftest proving the opcode path works (read/set/re-read device state,
  once per command invocation) → use; any failure falls back to generic
  MMC/SG. Missing driver files are never fatal (warn only when explicitly
  requested). The access method is queryable (`accudisc_access_method`) so
  callers can log e.g. "using generic MMC". Factual data tables (read
  offsets) are not features and may live in the core.
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
- `tests/`, `tools/` (dev scripts), `cmake/`

### Public vs internal (the docs/ ÷ private/ split)

Two parallel trees, mirroring cdda2img's layout:

- `docs/` — **PUBLIC** (tracked, pushed). Anything here ships with the repo.
  - `reference/` — docs that must track the code: `TODO.md`,
    `cli-machine-interface.md`, `RECOVERY.md`, `RECORDING_PLAN.md`,
    `ATTRIBUTION.md`
  - `research/` — shareable research findings
  - `flow/` — language-agnostic Mermaid flow docs (agent-generated)
  - `man/` — man page
- `private/` — **INTERNAL** (git-ignored, never pushed). Nothing here is
  redistributable; several items must not be.
  - `code/` — third-party source snapshots, mostly *symlinks* into
    `cdda2img/private/code/`; `code/MMC/` is the **licensed T10 MMC-5 spec —
    never redistribute** (summaries of it are fine, the source never leaves);
    `code/c2read/` is the retired prototype AccuDisc replaced
  - `drives/` — vendor/firmware material (Plextor, drive firmware)
  - `docs/` — internal-only notes (cross-agent correspondence, assessments)
  - `bench/`, `research/incoming/` — measurement data, raw research
  - `bugs/`, `optimiser/`, `tracer/` — agent report output
  - `guardian/` — Guardian agent keyring; holds a **private signing key**
  - `setup/` — agent role-descriptions and provisioning material

`private/` is git-ignored *by rule, deliberately ahead of being populated* —
it holds a licensed document and a signing key, and this repo is public.
Never `git add -f` anything under it.

`docs/reference/RECOVERY.md` is **hardlinked** to
`cdda2img/docs/reference/RECOVERY.md` — one document, both repos. Git does not
enforce the link: edit once, but commit on both sides.

> **Editing it is fragile.** Any editor that saves atomically (write temp +
> rename — most of them, including agent Edit tools) replaces the directory
> entry with a *new inode* and silently severs the link, leaving the two repos
> diverged. Either edit in place (`cat new > file`, `sed -i`) or re-link
> afterwards (`rm A && ln B A`). **Always verify after editing:**
> `stat -c '%i %h' docs/reference/RECOVERY.md` — link count must be 2.

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
- Third-party sources in `private/code/` are read-only inputs for analysis; code is
  rewritten for this codebase, not copy-pasted (mind licenses: GPL sources
  present).
- `docs/reference/RECOVERY.md` is the source of record for recovery design:
  roles (triage/gate/locate/repair/re-acquire), the rejected approaches
  (R1-R7) not to rebuild, and the invariant that relative checks (C2,
  consensus, overlap) never outrank absolute gates (AccurateRip/CTDB, which
  live in the calling application).
- **Licensing**: whole package is MIT (LICENSE), including drivers/plextor —
  its vendor opcodes are functional hardware identifiers (facts, not
  copyrightable; documented in QPxTool/cdrtools, verified on hardware). The
  driver stays a separate dlopen module for **architectural** vendor-isolation
  (core = pure MMC/SG), not licensing. Factual data tables likewise: offsets
  (REDUMP data) and the ATIP media catalog (public ATIP codes informed by
  PlexTools — src/drive/media_atip_db.inc). Credit reference sources in
  docs/reference/ATTRIBUTION.md and the future man page.
