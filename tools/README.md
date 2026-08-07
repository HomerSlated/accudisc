# tools/

Dev scripts and hardware probes. Not part of the build or the install; they
exist to answer questions about drives and discs before that knowledge is
turned into library code.

> **`/tmp` on this machine is tmpfs — it is RAM.** Build these into `build/`
> and write their output to `/var/tmp/`, never `/tmp`. A whole-disc rip is
> ~750 MB and a raw-subchannel capture ~15 MB per disc; enough of either in
> `/tmp` exhausts memory and takes the machine down (it has happened twice:
> 2026-07-22 and 2026-08-07). Two independent reasons, so neither one is the
> whole rule — `/tmp` also cannot hold the `security.capability` xattr, so a
> probe built there silently loses its `CAP_SYS_RAWIO` (see `speedprobe.c`).

## Hardware probes (C)

Not wired into CMake — they use library internals (`src/`), which the public
ABI deliberately hides, so they link the static lib directly:

```sh
cmake --build build
gcc -o build/mediaprobe tools/mediaprobe.c -I include -I src build/src/libaccudisc.a -ldl
./build/mediaprobe /dev/sr0
```

- **`mediaprobe.c`** — read-only. GET CONFIGURATION current profile, Real-Time
  Streaming (0x0107) bits, mode page 2A max/current, READ DISC INFORMATION,
  TOC + logical type, and the GET PERFORMANCE (0xAC) nominal curve with a
  CLV/CAV verdict. Changes no drive state; safe on CD/DVD/BD.
  *Known wart (deliberate, mirrors what the real code must avoid): it runs the
  CD track-CTRL classifier unconditionally, so it calls a DVD "CD-ROM (data)".
  Logical type must be gated on a CD profile (0x08/09/0A).*

- **`speedprobe.c`** — SET STREAMING (0xB6) flag-bit harness: does GET
  PERFORMANCE reflect a set ceiling; does Exact (0x02) work; does real RDD
  (0x04) restore. **Needs `CAP_SYS_RAWIO`** (data-OUT does not pass the
  kernel's SG filter without it, regardless of open mode — measured):
  `doas setcap cap_sys_rawio+ep build/speedprobe`. Changes drive state.
  *Build onto the real filesystem (`build/`), NOT `/tmp` — that is tmpfs and
  won't hold the `security.capability` xattr, so the cap silently won't bind.*

- **`ss_variants.c`** — the probe that cracked the SET STREAMING mystery.
  Isolates the CDB Parameter List Length offset: len@8-9 (spec position we
  wrongly used) fails 4/1b; len@9-10 (schily "Sz not G5 alike") succeeds and
  drops page 2A to the commanded ceiling. Also shows this drive rejects RDD
  (0x04) with 5/26/00. Needs `CAP_SYS_RAWIO`; restores to full speed.

- **`speedcheck.c`** — end-to-end check of the *library's* SET STREAMING path
  (`adsc_mmc_set_streaming`) across a speed ladder, reading back page 2A. Needs
  `CAP_SYS_RAWIO`. Companion after the 9-10 offset fix.

- **`rangeprobe.c`** — the Phase 3 question: is a ranged SET STREAMING contract
  *local* (throttle only inside `[L,L+N)`, free-run outside) or *global* on this
  drive? One run sets several contracts (whole-disc control, single-desc ranged,
  3-desc middle-slow, 3-desc first-slow) and times a read INSIDE vs OUTSIDE the
  nominal slow zone, classifying each against a free-run baseline (`SLOW = >2x`).
  On the PX-716A every contract went SLOW everywhere — the ceiling is applied
  **whole-disc**, the ranged extent ignored. That is one drive, not a proof about
  all drives: Phase 3 is deferred, not closed (see
  `private/code/MMC/SET_STREAMING_findings.md`). Needs `CAP_SYS_RAWIO`; restores full
  speed on exit.

## Offline benchmarks (C)

- **`bench_decode.c`** — CPU-isolated microbenchmark of the per-sector decode
  leaves (`adsc_audio_diff`, C2 `popcount`, `adsc_crc16`, `accudisc_sub_extract_q`)
  at whole-disc iteration counts. No device, no `CAP_SYS_RAWIO`. Times current vs
  proposed variants (all local copies at one `-O` level for a fair comparison,
  results cross-checked against the library functions) and reports each against
  the ~120 s wall-clock of a 40× rip. Built for the 2026-07-23 optimisation audit
  (`private/optimiser/`); its finding was that the decode path is ~0.19 % of a
  drive-bound rip, so the proposed popcount/CRC speedups are not worth taking.
  Re-run if the pipeline ever becomes non-drive-bound (offline image re-verify) or
  targets a weak CPU. **Needs `-O2` for meaningful numbers**:
  ```sh
  cmake --build build
  gcc -O2 -o build/bench_decode tools/bench_decode.c -I include -I src build/src/libaccudisc.a
  ./build/bench_decode
  ```

## Offline Q analysis

Operate on a raw subchannel capture (`accudisc read --sub raw --subf FILE`),
96 bytes/sector.

- **`qlag.c`** — does the Q frame in transfer slot *i* describe sector
  `base + i`, or another sector? Per CRC-good ADR=1 frame it histograms
  `(frame's own absolute LBA) - (base + slot)` and returns NO LAG / LAG ±n /
  SPREAD. **Public header only** — no device, no `CAP_SYS_RAWIO`, no `src/`:
  ```sh
  gcc -O2 -o build/qlag tools/qlag.c -laccudisc
  ./build/qlag capture.sub [BASE_LBA] [--toc capture.fulltoc]
  ```
  It exists because Q looks like it cannot have this problem: an ADR=1 frame
  carries its own address, so lag is invisible if you index by it. But
  `accudisc_q_parse` leaves every position field zero on CRC failure, so a
  CRC-**bad** frame can only be placed by slot — and those are exactly the
  frames a subchannel health map draws. Lag is irrelevant where you can locate
  the frame and decisive where you cannot.

  Measured on the PX-716A (2026-08-07, one whole-disc capture): **no lag**,
  157,871 of 157,914 position frames at delta 0. The 43 exceptions are not lag
  — six short contiguous runs, every delta an exact multiple of 512 sectors, so
  a frame can pass CRC-16 and still be positionally wrong at ~0.03 %. No
  mechanism claimed.

  **Confirmed at scale by cdda2img** (their §150.2/§151, 2026-08-07): 42 captures, 3
  discs, 4×/8×/24×/32×/40×, three passes each — **NO LAG on every one**, none
  near the SPREAD threshold. Including two independent Q-collapse events where
  CRC-good fell to 47.79 % and 38.73 % while in-slot stayed ≥ 99.977 %. **A Q
  yield below half does not disturb slot alignment**, so a slot-indexed lane
  survives exactly the failure it exists to draw. Their sweep also found a disc
  with **0.00 % non-position frames** (no MCN, no ISRCs), against ~1 % on
  others — the interleave rate is a property of the pressing, so validating a Q
  lane on the wrong disc would prove the `NO_POSITION` state unnecessary.

  Falsified before it was trusted: a capture shifted by 3 sectors reports
  `LAG +3`, a randomly jittered one reports `SPREAD` (41 deltas), a partial
  capture is told it is not the whole disc, and a non-multiple-of-96 file is
  refused rather than measured. cdda2img rebuilt the shifted arm independently
  rather than take ours on trust; the minority deltas tracked the shift
  (`-2048 → -2045`), which says those anomalies are positional facts about the
  capture rather than artefacts of the measurement.

  Non-position frames are reported against **two denominators** because "of
  all" alone misleads: it moves when Q yield moves, so a reader comparing
  speeds sees the interleave apparently thin under load. Against CRC-good it is
  flat to three digits across a 2× change in yield.

### Python oracles

- **`qdecode.py FILE.sub START_LBA [--only-bad] [--boundaries]`** — per-frame Q
  decode with CRC gating. ADR-aware: only ADR=1 frames carry position; ADR=2 is
  MCN, ADR=3 is ISRC. **This matters** — decoding the ~1-per-98 MCN frames as
  position manufactures phantom index-0 boundaries.

- **`pregap.py FILE.sub START_LBA TRACK INDEX1_LBA`** — per-boundary pregap
  census: extent, damage, and whether the recovery-critical anchors survived.

These were the oracle for `accudisc_index_map_decode` (`src/cdda/index_map.c`)
and `accudisc pregaps`, which supersede them for routine use. Kept because an
independent second implementation is what caught the C decoder's over-strict
UNKNOWN rule.

## Generators

- **`gen_media_db.py`** — ATIP media catalog -> `src/drive/media_atip_db.inc`
- **`gen_offsets.py`** — read-offset table

## Test targets

`/dev/sr1` on the dev box is **CDEmu** (virtual, backend in `private/code/libmirage`).
It is a free negative control: it *advertises* the Real-Time Streaming feature
and then **rejects GET PERFORMANCE** (Illegal Request). Anything that trusts a
feature bit instead of smoke-testing it will assert nonsense there — a virtual
drive has no spindle, no radius, and no rotation.
