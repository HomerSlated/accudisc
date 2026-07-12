# AccuDisc — deferred work

Ideas parked for a later session. Not scheduled; not commitments. Recovery
methods are considered complete (see `docs/ACCUDISC_RECOVERY.md`); this is
everything else worth remembering.

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
