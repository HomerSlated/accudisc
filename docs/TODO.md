# AccuDisc — deferred work

Ideas parked for a later session. Not scheduled; not commitments. Recovery
methods are considered complete (see `docs/ACCUDISC_RECOVERY.md`); this is
everything else worth remembering.

## NEXT SESSION — real read-speed control, then Q-channel preservation

Agreed order (2026-07-15). Speed control gates everything: until we can hold a
commanded speed mid-disc and prove it, all Q-vs-speed testing is moot.

### Task 1 — Set read speed via SET STREAMING (0xB6) — DONE + live-verified 2026-07-15
Implemented and hardware-confirmed: commanded 4x/8x delivered exactly 4.01x/8.01x
at outer windows (CAV would give ~30x) — a binding ceiling CDROM_SELECT_SPEED
never could. Details in drivers/plextor/PROTOCOL.md. Original notes kept below.

**Found during verification — read-engine throughput cap (follow-up).** The
recovery `read` engine sustains only ~5x on a *clean* disc where raw streaming
(`speeds`, bare READ CD) hits ~12–19x at the same radius — ~70 ms/command of
per-chunk overhead (6000 sectors: read=380 sec/s vs speeds-probe ~19x). Not a
speed-control bug; a ripping-throughput issue. Investigate: default
chunk_sectors, per-chunk cache-defeat, status-map write cost, or a synchronous
stall between commands. Matters because whole-disc Q baselines run through this
path.

### (original) Task 1 — Set read speed via SET STREAMING (0xB6), the CAV-correct way
- `CDROM_SELECT_SPEED` / SET CD SPEED (0xBB) — what `accudisc_set_speed` uses
  today (`device.c:169` → `sgio.c:75`) — is advisory; the PX-716A overrides it
  with its disc-init hardware governor (the "stuck at 32×/40×" behaviour, which
  resets on eject; it is NOT a mode we armed).
- **SET STREAMING (MMC `0xB6`, standard MMC — NOT a Plextor opcode)** hands the
  drive a performance *contract*: a 28-byte descriptor {Start LBA, End LBA,
  Read Size, Read Time, …}; speed = Read Size ÷ Read Time scoped to an LBA
  range → commands the CAV curve directly. This is what PlexTools uses (traced:
  builder 0x489740, caller 0x49c95e; see drivers/plextor/PROTOCOL.md §session 4).
- Do it in core `src/mmc/` (pure MMC, not the vendor driver). Route
  `accudisc_set_speed` through 0xB6, fall back to `CDROM_SELECT_SPEED` on reject.
- **Verify on hardware** a commanded speed actually holds mid-disc (current
  failure). Task 1 needs no drive until this verify step.

### Task 2 progress (2026-07-15)
- **Q-CRC counters DONE.** `subq_total/subq_ok` in accudisc_read_stats (engine
  counts extract_q+parse+crc_ok per delivered raw-sub sector); CLI read summary
  prints `subchannel Q : ok/total CRC-ok (%), bad`; machine mirror adds
  subq_total/ok/bad. No new signal code — reused stage-3 subq.c.
- **Clean-disc baseline (ZZ Top, brand new, free-run max ~18.7x w/ sub).**
  Radius sweep, 3000-sector windows: 0%→99.87, 19%→99.83, 44%→99.97,
  68%→100.0, 93%→99.93. **Uniform ~99.9%, NO radius dead zone.** Conclusion:
  max speed does NOT corrupt Q on a clean disc → the earlier dead-zone Q loss
  and cdda2img's §9 missing pregaps are **damage-driven, not speed-driven**.
  Validates the hypothesis that damaged discs still HAVE pregaps we're failing
  to read. ~0.1-0.2% bad frames = noise floor (location not yet recorded).
- **SET STREAMING = a streaming contract, not a plain speed knob (important).**
  Setting *any* ceiling (even 40x, above natural) collapses the recovery
  engine's throughput to ~5x, because the drive tunes for constant-rate
  delivery and the engine's ~37ms/chunk C2+Q processing reads as "host not
  consuming" → drive throttles. So: **first-pass = FREE-RUN (no --speed);
  slow-a-damaged-span = SET STREAMING low (4x/8x were exact).** Do NOT use
  SET STREAMING to set a high/max ceiling for bursty reads.
- **Max speed WITH subchannel is ~18.7x** on the PX-716A (drive can't
  deinterleave subcode faster); 25.9x with C2-but-no-sub, 21.4x audio-only.
  This is the true "max CDDA speed" for a Q-preserving read.

#### Damaged-disc findings (2026-07-16, ABBA Gold, confirmed Q damage)
Same 19-track disc as the Disc-ID work (leadout 347208). Probed the track 1->2
boundary (index-1 at LBA 17395). Offline decoder: scratchpad/qdecode.py (ADR-aware:
only ADR=1 frames are position; ADR=2=MCN, ADR=3=ISRC — critical, or the ~1-per-98
MCN frames masquerade as index-0 boundaries and manufacture phantom pregaps).
- **Q vs C2 orthogonality confirmed on real damage:** a 800-sector window read
  0 C2-flagged sectors but 22 CRC-bad Q frames. Clean audio, dead metadata.
- **Track 2 pregap IS present and reconstructable:** 50 frames, LBA 17345..17394,
  countdown rel 00:00:50 -> index-1 at 17395 (matches TOC). Two frames INSIDE the
  pregap (17346, 17357) are permanently dead, yet zero pregap info is lost — the
  start frame (rel 50), the index-1 boundary, and the countdown anchors pin it.
  This is the "9 of 19" mechanism: if the one boundary frame is CRC-bad and no
  reader interpolates, the pregap looks absent though it is fully there.
- **TWO failure populations (the key result). 3x cache-defeated re-reads of the
  same region:** 6 LBAs bad in ALL passes (static physical defect — re-reads never
  fix), ~6 LBAs bad in only 1-2 passes (transient — recovered by re-read+consensus,
  exactly the PCM strategy). Neither lever alone suffices: consensus for transient,
  deterministic-model interpolation for static.
- **Speed barely affects Q error rate:** 4x->9 bad, 8x->7 bad, free-run(~2.4x)->7-9
  bad on the same window. Defect-driven, not RPM-driven (matches clean baseline).
  The old "40% @ max / 98% @ 24x" was almost certainly the SpeedRead subchannel-
  destruction artifact, NOT honest speed. **Lever = re-reads + model, NOT slow reads.**

#### Whole-disc pregap census (2026-07-16) — resolves "9 of 19" definitively
Probed all 19 boundaries (read [i1-400, i1+30] each) + a radius damage sweep.
scratchpad/pregap.py does the per-boundary analysis.
- **9 tracks have real pregaps** (t2-t7, t9, t14, t18), 47-50 frames each, plus
  track 1's 33-frame lead-in gap. Every one has its start frame (rel=len) and
  index-1 boundary intact; dead frames inside a pregap (e.g. t2: 17346, 17357)
  are fully covered by surviving countdown anchors -> zero pregap info lost.
- **The other 9 tracks are genuinely GAPLESS** — index-1 -> index-1 with NO
  index-0 frames. Confirmed even for the 2 tracks (t16, t19) whose boundary
  regions were damaged: the surviving CRC-good frames show continuous
  previous-track index-1 counting UP right to the boundary, no index-0.
- **Verdict:** the "9 of 19" the good rip found is CORRECT and COMPLETE, not a
  damage artifact. The starting hypothesis ("all tracks have pregaps, damage
  hides them") is REFUTED for this disc — it has a gapless master for 9 of its
  transitions. Where pregaps exist they survive; where they don't, clean frames
  prove absence.
- **CRC gate is load-bearing:** rejected frames decoded as abs142:38, index19,
  adr9 — garbage that a non-CRC-checking reader would splice in as phantom
  indices. Damage does not just lose Q metadata, it injects false Q metadata;
  the per-frame CRC-16 is the only thing standing between the two.
- **Damage is localized, not radius-graded.** Sweep (2000-sector windows):
  0.6%->100, 11.5%->97.1, 25.9%->100, 40.3%->100, 54.7%->100, 69.1%->99.9,
  83.5%->100, 97.9%->96.0. Mostly pristine with a few damaged spots (inner
  ~LBA 40k, outer ~LBA 340k, and pinpoint hits like t16@281733). Surface
  blemishes at specific spots, NOT an inner dead zone. So this disc needs no
  Q recovery to get pregaps right — the recovery machinery is for a WORSE disc
  that loses a boundary anchor entirely.

#### Next build (unblocked by the above) — unified re-read predicate + Q consensus
A sector fails if C2-dirty OR Q-CRC-bad; both come from ONE READ CD. Re-read
Q-failures cache-defeated across passes, keep any CRC-good Q (harvests the
transient population). Record bad-Q-frame LBAs (status-map second dimension:
audio | Q). Residual static-bad frames -> deterministic Q reconstruction from
surviving anchors (abs MSF +1/sector; rel countdown in pregap; track/index
piecewise-constant), targeting index 0->1 boundaries. Reconstruction is the
larger follow-on piece.

### Task 2 — Preserve the Q subchannel (recover pregaps) — after speed is real
Why it matters: a damaged disc loses metadata (pregap/index/MSF), not just PCM.
As a full-disc archival tool we must recover it; conventional rippers don't care
(they only want track files). There is **no AccurateRip/CTDB analog for Q** — no
external absolute gate — so recovery = blind re-reads + cross-read consensus.

- **Key architecture fact:** C1/C2/CU protect ONLY the main (audio) channel via
  CIRC Reed-Solomon. The subchannel is NOT inside CIRC — Q's only integrity is a
  single 16-bit CRC-16/CCITT per frame: **detection, no correction.** Hence a
  sector can have pristine audio (0 C2) yet a dead Q frame. Orthogonal domains.
- **Q's compensating advantage:** Q is near-deterministic (abs MSF +1/sector;
  track/index piecewise-constant; rel MSF counts down in pregap, up in track).
  So we don't need every frame clean — enough CRC-valid *anchors* let us fit the
  model and interpolate gaps. The one non-interpolable event is the index 0→1
  **transition** (pregap boundary): needs ≥1 clean-ish anchor near each boundary.
  → Target re-reads at index boundaries, not uniformly.
- Steps:
  1. Q-CRC counters in the read `summary` (`subq_total/ok/bad`, + per-region so
     the inner dead-zone shows). Decoder prototyped: `scratchpad/qcrc.py` → port
     to C in `src/cdda/`. (cdda2img §9.2 asked for this too.)
  2. Speed sweep with the NOW-WORKING speed control, one variable at a time; map
     Q-CRC vs speed vs radius. Re-establish the provisional "40% @ max / 98% @
     24×" numbers cleanly — the old ON/OFF run was confounded (was `--speed 48`
     vs `40`; then the drive wedged). Trust the *shape* (inner dead zone, clean
     audio), not the exact %.
  3. Unified re-read predicate: a sector fails if C2-dirty OR Q-CRC-bad; both
     acquired in ONE READ CD (0xBE already returns audio+C2+SUB together —
     `read/engine.c:8`). Status map gains a second per-sector dimension (audio |
     Q). Acquire fast, re-read Q-failures slow (engine already has a speed
     ladder).
- **"9 of 19 tracks have pregaps" suspicion:** unresolved — could be a genuinely
  gapless master OR damage eating every index-0 frame. Q-CRC counters (step 1)
  distinguish them: clean index-1 start + zero CRC-valid index-0 frames before it
  (while neighbours show them) = damage signature, not gapless.

## Eject feature
- accudisc eject and accudisc load (to me this makes more sense than
  "eject -t")

## Investigate how Plextor drives handle speeds.
- "When a Plextor drive initialises a disc, it determines the optimal read
  speed and thereby  imposes a limit that you cannot exceed on hardware level
  which is a good thing."
  https://hydrogenaudio.org/index.php/topic,28739.0.html
- Previously reported drive was "permanently stuck on 32x", but when I eject
  the disc, now I see the speed reported is 40x.

  drdao drive-info --device /dev/sr0
  /dev/sr0: PLEXTOR DVDR   PX-716A	Rev: 1.11
  CD-TEXT writing is supported.
  Using driver: Generic SCSI-3/MMC - Version 2.0 (options 0x0010)

  Maximum reading speed: 7056 kB/s
  Current reading speed: 7056 kB/s

  accudisc speed-report --driver plextor --drivers-dir /home/kgr/Git/accudisc/build/drivers/
  accudisc: plextor: 0xEA arm refused
  accudisc: driver plextor: selftest failed on PLEXTOR DVDR   PX-716A — staying on generic MMC
  accudisc: using generic MMC
  speed max_kbps 7056 current_kbps 7056 max_x 40 current_x 40

  I assume "0xEA arm refused" is because you can only request a speed change when the drive
  knows what kind of disc is loaded.

## ATIP / media identification

- ~~**Wire the ATIP catalog into a lookup + CLI.**~~ *Done (2026-07-12):*
  `accudisc media` reads the disc ATIP (READ TOC/PMA/ATIP fmt 4) and returns
  manufacturer + code + capacity + CD-R/RW. `accudisc_read_atip()` /
  `accudisc_atip_manufacturer()` in `src/drive/media_db.c`; lookup keys on
  `sec` + frame-decade (matching resolves 97:24:01 → the 97:24:00 Taiyo Yuden
  entry; the decade distinguishes makers that share a `sec`). Unit test
  `tests/test_media.c`; live-verified on a blank Taiyo Yuden (`97:24:01`).
  *Possible follow-up:* also surface spiral length (record+0xDC) and the
  ATIP reference-speed/indicative-power fields.
- **Public ATIP cross-reference pass.** *Largely done (2026-07-12):* diffed
  against cdrecord's `diskid.c` (independent source; 107/123 agree) and Nero
  2026 — both carry the same effectively-frozen registry, so "post-2007 gaps"
  turned out moot. Folded cdrecord's 3 high-confidence uniques into the union
  (`gen_media_db.py` → 134 codes). *Remaining (optional):* a broader web ATIP
  database diff to catch any obscure codes none of these three list.

## Recording

- **CD-Text on write (next recording feature).** The write path does NOT burn
  CD-Text yet, so a round-tripped disc loses album/track titles/performer (the
  "supplemental metadata"). Have: CD-Text *read* (meta/cdtext.c) and the 2448
  block-type in the write-parameters page (wparams.c `cdtext`). Need: a CD-Text
  encoder (pack the CD_TEXT blocks the .toc parser currently ignores) + inject
  it into the SEND CUE SHEET lead-in (dataForm 0x41 lead-in entry + the R-W
  sub-channel packs). Reference: cdrdao `CdTextEncoder` / `writeCdTextLeadIn`.
  Note: CD-Text does NOT affect the Disc ID (pure TOC) — this is content
  fidelity, separate from the pregap item below.
- **Disc-ID round-trip mismatch = pregap/TOC, upstream (cdda2img).** Root cause
  pinned via 3-way compare: original disc track 1 @ LBA 33 with a 33-frame
  pregap, lead-out 347208; cdda2img's RBI + our burn both track 1 @ 0, lead-out
  347175 (Δ = 33). Our burned Disc IDs are byte-identical to the RBI mount, so
  AccuDisc reproduced the .toc *exactly* — the loss is in cdda2img's extract
  (its .toc drops track 1's pregap and must match the original TOC's index-1
  offsets AND lead-out, not just track 1). AccuDisc verified ready: a declared
  track-1 `START` yields the right index1_lba. Consider an AccuDisc read-back
  verify (`--verify-toc`) that diffs the burned TOC vs the source .toc and warns
  on any offset delta.

## Probes / diagnostics

- **Timed-read cache detection.** Borrowed from libcdio-paranoia
  (`cdrom_cache_handler`): a re-read that returns implausibly fast was
  served from the drive's cache, not the platter. Paranoia flags a backseek
  read completing in under ~6 ms. Use it as a probe that *verifies our
  cache-defeat actually works* on a given drive — time a normal read, then
  time a re-read at our 5000-sector flush distance; if the re-read is not
  meaningfully slower, the flush distance is too small for this drive's
  cache and every "independent" reread (c2_retries, verify, consensus,
  c2lag) is silently reading cache. Report-only, per-drive, like the other
  probes. Would also let the flush distance auto-tune instead of being a
  fixed constant.

## Deferred (explicitly, by user decision)

- Python / Rust bindings (generated against `include/accudisc/*.h` only).
- Man page (must mirror `docs/ATTRIBUTION.md`).
- Write / burn (DAO) path — paused; do not start without direction.
