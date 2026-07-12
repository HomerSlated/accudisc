# AccuDisc — deferred work

Ideas parked for a later session. Not scheduled; not commitments. Recovery
methods are considered complete (see `docs/ACCUDISC_RECOVERY.md`); this is
everything else worth remembering.

## ATIP / media identification

- **Wire the ATIP catalog into a lookup + CLI.** `src/drive/media_atip_db.inc`
  is committed data; add the lookup and a CLI surface (e.g. `accudisc media` /
  an ATIP-info path). Read the disc ATIP via READ TOC/PMA/ATIP (opcode 0x43,
  format 0x04), take the lead-in start time (bytes 8–10 = min:sec:frame), and
  return manufacturer + spiral length. **Match on min:sec** (the manufacturer
  key); the frame varies per media within a manufacturer, so treat it as a
  secondary/optional discriminator, not exact-equality. Live example: a blank
  Taiyo Yuden reads `97:24:01`, table has TY at `97:24:00` — matches on 97:24.
  Core read-path work (any drive, pure MMC), report-only.
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
