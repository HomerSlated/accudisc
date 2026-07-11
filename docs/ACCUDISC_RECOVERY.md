# ACCUDISC_RECOVERY — AccuDisc's half of the read-recovery strategy

AccuDisc's recovery capabilities: what exists, what is planned, and exactly
where AccuDisc's responsibility ends and the calling application's begins.

This document pairs with **`CDDA2IMG_RECOVERY.md`** in the cdda2img project
(the reference consumer), which describes the other half — gating, parity
repair, and orchestration. Both documents are updated to match the current
state of their project; the component numbering below follows cdda2img's
`docs/reference/RECOVERY.md` so the two sides can cross-reference precisely.

**Living document.** Last updated: 2026-07-11 (machine interface delivery,
commits `796cab4`…`5d67001`).

---

## 1. Component map

The recovery toolkit as cdda2img defines it, and AccuDisc's position on each
component. "Caller" below always means the application invoking AccuDisc.

| # | Component | AccuDisc status |
|---|-----------|-----------------|
| 2.1 | C2 error pointers | **Supported.** Requested by default on `read`; probed by `features` (claim + smoke + 5-combo, exit 0 iff usable). Bitmap passed through untouched. C2-guided rescue via `--c2-retries`. |
| 2.2 | CTDB per-track CRCs | **Out of scope, permanently** (network gate — caller's). |
| 2.3 | CTDB parity repair | **Out of scope, permanently** (repairer — caller's). AccuDisc feeds it: C2 bitmap + zero-fill erasures + targeted span reads. |
| 2.4 | AccurateRip checksums | **Out of scope, permanently** (absolute gate — caller's). |
| 2.5 | Multi-pass speed sweeps | **Primitive supported.** Span reads at `--speed X`, speed never auto-restored; the caller owns the sweep loop. Per-sector speed ladder (`--ladder`) inside one invocation. |
| 2.6 | Intra-read verification | **Supported.** `--verify P` independent cache-defeated passes. |
| 2.7 | Boundary overlap checking | **Supported.** `--overlap K` seam check + slip classification; plus the Accurate Stream probe that tells you whether overlap is needed at all. |
| 2.8 | Zero-fill erasure marking | **Supported, always on.** The failure contract of every read. |
| 2.9 | Retry ladder + cache-defeat | **Supported.** `--retries K`, ~5000-sector seek-away between attempts, sense classification. |
| 2.10 | Mode-page 01 tuning | **Missing, implementable.** Rejected by experiment for the miscorrection class; would return only as a manual diagnostic flag. Deferred until a disc that fails commands appears. |
| 2.11 | Cross-pass consensus | **Supported (per-sector form).** Any-two-byte-identical among ≤6 speed-diverse samples. C2-*weighted* voting: missing, implementable, deferred. |
| 2.12 | C1/C2/CU census | **Supported.** `cxscan` via the Plextor vendor driver (opt-in, external `.so`). |
| 2.13 | Offset-domain management | **Honored by abstention.** AccuDisc never offset-corrects; it supplies the factual per-drive offset (`info` / `accudisc_read_offset`, REDUMP table). The one-domain discipline is the caller's. |

Planned probes (both report-only, no database dependency):

1. **C2↔audio lag probe** — the C2 bitmap is misaligned with the audio by a
   small per-drive sample-pair count (PX-716A: 2 pairs). Probe by
   cross-correlating per-byte instability across rereads with the bitmap.
2. **Achievable-speed-ladder probe** — set each candidate speed, verify what
   the drive actually achieves (mode page 2A can lie; timed reads do not).

---

## 2. Users

*You don't need to know any of the theory below to rip a disc. This section
explains what the recovery machinery does in plain terms.*

### 2.1 What can go wrong when reading a CD

An audio CD player hides errors from you: when it can't read a spot it
guesses (interpolates) and keeps playing. A ripping program can't accept
guesses — it wants the exact original bytes. Three kinds of trouble occur:

- **The drive knows it failed.** Scratches or rot damaged a spot beyond the
  disc's built-in error correction. The drive can flag exactly which bytes
  are bad (these flags are called *C2 pointers*).
- **The drive got it wrong and doesn't know.** Occasionally the error
  correction "fixes" data into something wrong, or the drive momentarily
  loses tracking and reads the *wrong part of the disc* perfectly. No flag
  is raised. This is the sneaky case, and most of AccuDisc's machinery
  exists because of it.
- **The drive gives up entirely.** The sector is returned as an error, not
  data.

### 2.2 What AccuDisc does about it

All of these are options on the `accudisc read` command. Everything is off
by default except C2 flags — you choose how hard to try:

- **C2 flags (default on).** The drive marks bytes it knows are bad. Free —
  no extra reading.
- **Retries (`--retries K`).** If a sector fails outright, try it again up
  to K times, deliberately reading elsewhere in between so the drive can't
  just repeat its cached answer. A sector that never succeeds is written as
  **silence** and clearly marked — your files never lose sync.
- **C2 rescue (`--c2-retries N`).** For every flagged sector, hunt for a
  clean copy with up to N rereads; the best copy wins.
- **Verify passes (`--verify P`).** Read everything P times and compare. If
  two reads of a sector disagree, keep re-reading until two copies agree
  exactly ("recovered") or give up and mark it "suspect".
- **Overlap checking (`--overlap K`).** Each batch of sectors is read
  slightly past its end, and the overlap is compared against the next
  batch — this catches the drive drifting position, which no flag ever
  reports. Run `accudisc features` first: if it says
  `accurate_stream yes`, your drive doesn't drift and you can skip this.
- **Speed ladder (`--ladder 32,16,8,4`).** Problem sectors are re-tried at
  different speeds — a spot that fails at 32× often reads fine at 8×, and
  *variety* matters more than slowness.

### 2.3 What comes back

- **Your data**: PCM audio (`--pcm`), the C2 flag stream (`--c2f`), the
  subchannel (`--subf`) — always exactly one sector's worth per sector, in
  step with each other.
- **A per-sector status map** (`--map` for a picture in the terminal,
  `--map-file` for a file other programs can watch live): every sector is
  marked *ok*, *had C2 flags*, *recovered*, *suspect*, or *unreadable* —
  the same information an EAC-style colored disc map displays.
- **A summary**: totals of hard errors, flagged sectors, recovered/suspect
  counts, extra reads, and position slips (human-readable on screen;
  machine-readable via `--progress-fd`).
- **An honest exit code**: `0` = clean, `3` = finished but degraded
  (something is suspect, unreadable, or still flagged), `1`/`2` = didn't
  finish.

### 2.4 What a calling program typically does with it

AccuDisc tells you *how the read went*; it cannot tell you the data is
*correct* — only a comparison against an outside reference can. A typical
ripper (cdda2img is the reference example):

1. runs a fast `read` with C2 on;
2. checks the result against **AccurateRip/CTDB** (online databases of
   checksums from thousands of other people's rips of the same pressing);
3. if a track fails the check, uses AccuDisc's C2 flags and silence-marks
   as a *damage map* to drive repair (CTDB parity can rebuild bytes without
   re-reading) or re-reads the failed span at several speeds until the
   checksum passes;
4. records anything unrecoverable honestly rather than hiding it.

If your disc isn't in any database, the relative signals (suspect counts,
the status map) are all there is — treat them as "how confident the drive
was", never as proof of correctness.

---

## 3. Developers

### 3.1 The maths: CIRC, and why erasures matter

Audio CD data is protected by **CIRC** — two concatenated Reed-Solomon
stages over GF(2⁸) with cross-interleaving. C1 = RS(32,28) corrects small
random errors in each frame; a delay-line de-interleave then scatters burst
errors across many frames; C2 = RS(28,24) uses C1's failure flags as
erasures to correct the scattered remains. The interleave is why a 2.4 mm
scratch is survivable: the burst becomes many small, separately-correctable
errors.

Two RS facts drive the whole recovery design:

- A code with `npar` parity symbols corrects `⌊npar/2⌋` errors at
  **unknown** positions, but up to `npar` **erasures** (errors at known
  positions). *Knowing where the damage is doubles repair capacity.*
- Decoding beyond capacity does not fail cleanly — it can produce
  confidently wrong output ("miscorrection"). Anything downstream must
  re-validate.

What survives both stages uncorrected is reported to the host — if asked —
as **C2 error pointers**: 2352 bits per sector, one per audio byte, set
where decoding failed. Measured behaviour (PX-716A, against a
database-verified oracle): precision ≈ 99% (a fired flag almost always
marks a genuinely wrong byte), recall ≈ 99% *for decode failures*, and
**zero** recall for positioning errors. Flags are also non-deterministic
read-to-read: marginal signals fall on either side of the decoder's
threshold per revolution.

### 3.2 The defects, and the failure regimes

Physical damage — scratches, pinholes, delamination, dye/aluminium rot —
degrades the analogue signal off the platter. The drive's response happens
in layers: the servo tracks the spiral, the slicer/EFM demodulator recovers
bits, CIRC corrects, and (on the *audio playback* path only) concealment
interpolates over what CIRC couldn't fix. `READ CD` data transfers bypass
concealment; you get the decoder's real output plus, optionally, its
confession (C2).

Three distinct failure regimes, requiring different tools:

| Regime | What the drive does | Signal you get | Counter-tool |
|--------|--------------------|----------------|--------------|
| Decode failure | CIRC gives up on bytes | C2 flags fire | C2 capture, rescue rereads, erasure-fed parity repair |
| Miscorrection / marginal read | wrong bytes, no error | **nothing** (or unstable rereads) | verify passes, consensus, speed diversity, external checksums |
| Positioning slip | servo loses lock, streams coherent audio from the wrong place, re-syncs | **nothing** — CIRC decodes it perfectly | overlap checking + shift detection |
| Command failure | sector read fails outright (CHECK CONDITION) | SCSI sense (medium/hardware) | retry ladder + cache-defeat, then zero-fill |

The slip regime is the one that dictates the trust architecture: a
documented case returned 8,852 wrong samples on a defect-free track with
zero flags. Any strategy that trusts "no C2 = correct" is structurally
broken. This is why relative checks can never be promoted to gates.

Reads of marginal regions behave as a **stochastic lottery**: whether a
revolution decodes depends on rotational phase, servo state, and linear
velocity. Empirically there is no "magic speed" — *speed diversity* is the
lever, and same-speed repetition can fail persistently (a systematic
miscorrection at that speed). AccuDisc's ladder and consensus honour this:
deciding votes are taken at rungs different from the streaming speed.

### 3.3 What the drive itself does, and how AccuDisc steers it

Firmware fights you in three ways and AccuDisc counters each:

- **Caching.** A reread served from cache proves nothing. Every
  rescue/verify/consensus read is preceded by a throwaway read ~5000
  sectors away, forcing a genuine platter access.
- **Internal retries.** Mode page 01 gives the drive its own retry budget
  (default 10 on the PX-716A) before it ever reports failure. Experiment
  verdict: tuning it is *inert* for the miscorrection regime, because no
  command ever fails there — the page only governs the command-failure
  regime. AccuDisc deliberately does not touch it (a readback-verified
  diagnostic flag may be added if a disc that needs it appears).
- **Speed-change recalibration.** Every speed change costs seconds of
  recalibration on real drives. AccuDisc never issues a no-op speed change,
  streams verify passes at the pass speed, and confines ladder switching to
  per-sector arbitration reads. Whole-range speed sweeps belong to the
  caller (repeat invocations with different `--speed`), which is also why
  the set speed is **never auto-restored** between invocations.

### 3.4 Access via the C API

Everything below is in `include/accudisc/accudisc.h` (the ABI contract;
bindings are generated against it, never against `src/`).

```c
accudisc_device *dev = accudisc_open("/dev/sr0", 0, &err);

/* Capability gate: claim + smoke + combos; and the slip-risk probe. */
accudisc_features f;   accudisc_probe_features(dev, &f);
uint8_t as;            accudisc_probe_accurate_stream(dev, lba, &as);

/* One commanded read with caller-selected strategies. */
uint8_t *map = /* count bytes, caller-owned, e.g. mmap'd shared */;
accudisc_read_req req = {
    .lba = start, .count = n,
    .c2  = ACCUDISC_C2_PTRS,        /* C2 capture              (2.1) */
    .retries = 2,                    /* retry ladder            (2.9) */
    .c2_retries = 4,                 /* C2 rescue               (2.1) */
    .verify_passes = 3,              /* intra-read verification (2.6) */
    .overlap_sectors = 2,            /* boundary overlap        (2.7) */
    .speed_ladder = (uint16_t[]){32,16,8,4}, .ladder_len = 4,
    .speed_x = 24,                   /* streaming speed         (2.5) */
    .status_map = map,               /* frame-accurate status surface */
    .cancel = &cancel_flag,
};
accudisc_read_stats st;
accudisc_read_cdda(dev, &req, sink_fn, sink_user, &st);
```

- **Data delivery**: the sink callback receives chunks; per sector the
  layout is AUDIO (2352) ‖ C2 (294/296) ‖ SUB (96/16), always captured in
  a *single* `READ CD` transfer so the three never desync. Accepted
  rescue/consensus reads replace the whole sector.
- **Status map**: one byte per sector, written with relaxed atomic stores —
  poll it from any thread/process at zero syscall cost. Low nibble =
  state (`ACCUDISC_MAP_PENDING/OK/C2/HARD/RECOVERED/SUSPECT`), high
  nibble = severity (log₂ C2 bits, attempts used, or log₂ disagreeing
  bytes). All states are **relative claims** (see §3.7).
- **Stats**: `hard_errors`, `sectors_flagged`, `c2_bits`,
  `max_bits_sector`, flagged-span LBAs, `sectors_recovered`,
  `sectors_suspect`, `rereads`, `slips`, sense-class tallies.
- **Zero-fill contract**: a hard-unreadable sector is delivered as PCM 0 /
  C2 all-ones / SUB 0; the synthetic C2 is excluded from stats. This is
  the erasure feed for downstream RS repair.
- **Planned probe entry points** (placeholders, not yet in the header):
  `accudisc_probe_c2_lag(dev, lba, int8_t *sample_pairs)` and
  `accudisc_probe_speed_ladder(dev, uint16_t *rungs, size_t *n)`.

### 3.5 Access via the binary

The subprocess contract is frozen in `docs/cli-machine-interface.md`;
summary of the status outputs:

- **Exit codes**: `0` clean · `1` usage/local · `2` fatal · `3` completed
  with caveats (`read`: hard/suspect/residual-C2; `cdtext`/`fulltoc`/
  `scan`: data absent). `features` exits 0 iff C2 is usable (frozen).
- **`--progress-fd N`**: `progress <done> <total>` lines, then one
  `summary hard= c2= recovered= suspect= rereads= slips=` line. Parse
  tokens, not positions.
- **`--map-file F`**: the status map as a file — exactly `count` bytes,
  byte *i* = LBA `start+i`, `ACCUDISC_MAP_*` encoding, updated live
  through a `MAP_SHARED` mapping. No header. Progress = non-PENDING count.
- **`--map`**: human terminal rendering (`.` ok, `r` recovered, `!` C2,
  `?` suspect, `X` hard) — not an interface, do not parse.
- Human stderr (progress line, summary block, log messages) is explicitly
  **not** parseable interface.

A typical secure invocation:

```sh
accudisc read --start 111142 --count 9480 \
    --retries 3 --c2-retries 4 --verify 3 --overlap 2 \
    --ladder 32,16,8,4 --speed 24 \
    --pcm t8.pcm --c2f t8.c2 --sub raw --subf t8.sub \
    --map-file t8.map --progress-fd 3 -q
```

### 3.6 MMC over SG_IO, and vendor opcodes

AccuDisc talks to drives with standard **MMC** commands over Linux
`SG_IO` — unprivileged: membership of the `cdrom` group suffices, and
plain reading works on a read-only fd (the kernel's SG_IO filter allows
`READ CD`, `READ TOC`, `MODE SENSE`, …). Recovery-relevant commands:

- `READ CD (0xBE)` — the workhorse: expected-sector-type, C2 field
  selection (none / pointers / pointers+block-error-bits), subchannel
  selection, all in one transfer.
- `READ TOC/PMA/ATIP (0x43)` — formats 0 (TOC), 2 (raw full TOC), 5
  (CD-Text packs); passed through byte-identical.
- `MODE SENSE/SELECT (10)` — capability pages (2A) and, if ever needed,
  error-recovery page 01. MODE SELECT and vendor opcodes are blocked on
  read-only fds — hence `ACCUDISC_OPEN_RDWR` for those paths.
- `CDROM_SELECT_SPEED` ioctl — best-effort speed control.
- **Sense data** on failure (fixed and descriptor formats decoded): key
  0x3 = medium error, 0x4 = hardware error — tallied separately, because a
  hardware-heavy pattern implicates the transport, not the disc.

**Proprietary opcodes** (e.g. Plextor `0xEA` — the C1/C2/CU census behind
`cxscan`) live exclusively in external driver `.so` files
(`include/accudisc/driver.h` ABI). The core is pure MMC/SG; the gate order
is: identify drive → locate driver → *the caller's attach call is the
permission grant* → selftest (read/set/re-read real device state) → use;
any failure falls back to generic MMC, and `accudisc_access_method()`
reports which path is live. Vendor raw-read paths (like the historical
0xD8) could be hosted by the same mechanism if a drive ever requires one —
so far none has: all five 0xBE C2/sub combinations work on tested hardware.

### 3.7 Separation of duties — what AccuDisc will not do for you

**AccuDisc reads and reports. It never verifies, never looks anything up,
never repairs beyond re-acquisition, and never post-processes** (no offset
correction, no de-emphasis, no gap handling). Its every quality claim is
**relative** — derived from the drive's own flags and from agreement
between its own reads. A drive that misreads *deterministically* passes
every relative check forever. Only **absolute gates** — comparison against
the pressing's canonical bytes via AccurateRip/CTDB or an existing
verified image — establish correctness, and those live in the calling
application, by design (AccuDisc does no network I/O, ever).

The caller's recovery loop, in cost order:

1. **Gate** the delivered PCM against an absolute reference, per track, in
   the *raw* (uncorrected) offset domain — apply the drive offset exactly
   once, at storage, never before checksumming against raw-domain
   references.
2. On failure, **locate**: AccuDisc's C2 stream and zero-filled sectors
   are known-position erasures; the status map and `first/last_flagged`
   span bound the damage.
3. **Repair without reads** first: RS parity (CTDB) with the erasure feed —
   doubles capacity versus unknown-position decoding. Re-validate
   syndromes and re-gate after; over-capacity decodes lie.
4. **Re-acquire**: targeted `read --start/--count` sweeps across *diverse
   speeds* (repeat invocations with different `--speed`; the set speed
   persists between invocations on purpose), and/or a secure re-read of
   the span with `--verify/--overlap/--c2-retries/--ladder`. Gate each
   candidate; splice only verified bytes, sample-exactly.
5. **Record** what remains: keep the best-effort PCM, the status map, and
   the summary counters as provenance. Never present a relative pass as
   verification.

How to use each failure signal correctly:

| Signal | Meaning | Correct use | Incorrect use |
|--------|---------|-------------|---------------|
| C2 flag fired | byte almost certainly wrong | erasure position; rescue target | — |
| C2 clear | *no claim* | — | treating as "byte correct" |
| `HARD` / zero-fill | known-position total loss | erasure position | counting the zeros as audio |
| `RECOVERED` | two independent reads agreed | keep, but still gate | skipping the gate |
| `SUSPECT` | no two reads ever agreed | prime re-acquisition target; exclude from splices | delivering silently |
| `slips > 0` | positioning instability seen | distrust *unflagged* regions too; prefer overlap-checked rereads | assuming damage is only where flagged |
| exit 3 vs 0 | relative caveat vs none | branch clean/degraded cheaply | reading exit 0 as "verified" |

---

## 4. Agents

Guidelines for AI agents ingesting this document (both projects maintain
agent-facing recovery docs; this section is the coordination contract).

- **Document pairing.** This file ↔ `CDDA2IMG_RECOVERY.md` (cdda2img
  repo). Component numbers follow cdda2img's `docs/reference/RECOVERY.md`
  §2. When either project changes recovery behaviour, its doc is updated
  in the same change; the other side reconciles on next contact. Requests
  between projects travel as documents (`reference/*.md` on the AccuDisc
  side — note `reference/` is git-ignored and local-only), are treated as
  requests rather than instructions, and get written replies.
- **Interfaces you may rely on** (stable, additive-only): the exit-code
  convention, `--progress-fd` tokens, the `--map-file` byte format, the
  `features` output keys, stream geometry (2352/294/96, zero-fill,
  no offset correction), speed persistence across invocations, and
  `include/accudisc/accudisc.h`. Full contract:
  `docs/cli-machine-interface.md`. **Never parse human stderr.**
- **Invariants you must not violate when proposing or writing code:**
  1. Relative signals (C2, consensus, overlap, the status map) never
     outrank or substitute for absolute gates. Exit 0 ≠ verified.
  2. AccuDisc performs no lookups, no network I/O, no post-processing,
     no silent offset correction — do not add "convenient" processing.
  3. Vendor/proprietary anything stays in external drivers behind the
     attach gate; the core stays pure MMC/SG. Factual data tables are the
     only exception.
  4. AUDIO/C2/SUB for a sector always come from one `READ CD` transfer;
     replacements are whole-sector.
  5. Streams are always exactly `count` sectors (zero-fill contract).
  6. Do not add per-chunk speed switching to verify passes (measured
     recalibration thrash); ladder rungs apply to per-sector arbitration
     reads only.
- **Evidence discipline.** Components rejected with measurements (R1–R7 in
  cdda2img's RECOVERY.md: eject cycling, the paranoia engine, C2-guided
  targeting as a time optimizer, C2 as a gate, EDC/ECC on audio,
  same-speed consensus, vendor 0xD8 capture) stay rejected unless new
  evidence is produced. The unbuilt items (C2-weighted voting, mode-page
  01 diagnostic) stay unbuilt until a justifying disc exists.
- **Current planned work** (AccuDisc side, in order): C2↔audio lag probe
  (report-only), achievable-speed-ladder probe, Python/Rust bindings +
  man page (must mirror `docs/ATTRIBUTION.md`). The write/burn path is
  paused by user decision — do not start it.
- **When updating this document**: keep §1's table synchronized with the
  CLI/API actually on `main`, move items between supported/planned/missing
  with the commit hash that moved them, and keep the three audiences
  (Users/Developers/Agents) intact.
