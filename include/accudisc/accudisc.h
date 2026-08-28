/* SPDX-License-Identifier: MIT */
/* accudisc.h — public API for libaccudisc.
 *
 * This header is the ABI contract: the CLI and all language bindings are
 * built against it exclusively. Keep it C-only, self-contained, and free of
 * internal types.
 */

#ifndef ACCUDISC_H
#define ACCUDISC_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* MINOR moves whenever a struct layout changes, so that the .so version moves
 * with the layout and a version comparison can see it. That rule was stated in
 * API_PLAN §8 and then not followed: accudisc_speed_rung went 6 -> 10 -> 14 and
 * accudisc_write_opts gained a size field, all inside 0.2.0. A binding compiled
 * against any of those and loaded against another compares 0.2 to 0.2 and finds
 * them equal. cdda2img caught it (§113.2) as a live hazard rather than a
 * theoretical one — their venv was about to load a two-day-old extension.
 *
 * The lesson is not that the check was too coarse. It is that a version check
 * of ANY granularity is worth exactly what the discipline of bumping it is
 * worth, and is not a substitute for the per-struct size guards. */
#define ACCUDISC_VERSION_MAJOR 0
#define ACCUDISC_VERSION_MINOR 31 /* 0.31.0: SETTING THE READ SPEED NO LONGER
                                  * RETUNES THE WRITE SPEED. No ABI change and
                                  * no signature change in the public header —
                                  * accudisc_set_speed and _set_speed_range are
                                  * as they were; the fix is inside them.
                                  * The SET STREAMING performance descriptor
                                  * carries a Write Size field which we filled
                                  * with the READ rate, commented "mirror;
                                  * unused for read". The drive uses it. So
                                  * every accudisc_set_speed — a READ-speed
                                  * call — silently retuned writing, and did it
                                  * PAST a ceiling the caller had left in place:
                                  * with the Plextor SpeedRead uncap off (max
                                  * 40x), asking for 48x correctly clamped the
                                  * read field to 40x and set the WRITE field
                                  * to 48x. Measured on a PX-716A 2026-08-28.
                                  * The field has NO "leave alone" encoding: 0
                                  * means MAXIMUM, exactly as 0xFFFF does in
                                  * SET CD SPEED — so the obvious one-line fix
                                  * (stop mirroring, write zero) was measured
                                  * WORSE than the bug. It now carries the
                                  * drive's CURRENT write speed, read from page
                                  * 2A immediately before the command.
                                  * 0.30.0: THE CLI NO LONGER REPORTS A
                                  * TRUNCATED RIP AS A SUCCESS. Library
                                  * UNCHANGED — this is entirely a cli/ fix, and
                                  * it is recorded at the package version
                                  * because the CLI's exit codes are a
                                  * documented interface (see
                                  * docs/reference/cli-machine-interface.md).
                                  * `read_sink` issued four fwrite calls and
                                  * checked none, never consulted ferror, and
                                  * always returned 0; `dump_to_file` wrote and
                                  * closed blind and then printed a byte count
                                  * it had not verified. A full filesystem
                                  * therefore produced a SHORT output file and
                                  * exit 0. Now every write, flush, fsync and
                                  * close is checked, the rip stops at the first
                                  * failure, and `read` exits 2 saying which
                                  * lane failed and how many sectors landed.
                                  * An in-process caller was never affected:
                                  * the library hands chunks to a sink and
                                  * never opens a file.
                                  * 0.29.0: THE REQUESTED WRITE SPEED NOW
                                  * REACHES THE DRIVE. accudisc_write_opts.speed
                                  * was documented "0 = leave the drive's
                                  * current write speed" and was read by NOBODY
                                  * below the API boundary: no SET CD SPEED, no
                                  * SET STREAMING, nothing. `--speed 4`
                                  * constrained the drive not at all — measured
                                  * 19.4x delivered on a PX-716A while 4x was
                                  * asked for, 2026-08-28.
                                  * It is worse than a wrong rate. The write
                                  * FIFO is sized in SECONDS against that
                                  * number, so a ring the CLI reported as
                                  * "5.0 s at 4x" was 1.02 s of real
                                  * ride-through. The reassuring figure was the
                                  * wrong one, while the alarming signals
                                  * (0% buffer fill) were correct.
                                  * The command is SET CD SPEED (0xBB) with
                                  * CLV, per cdrecord's speed_select_mmc — NOT
                                  * SET STREAMING, which it uses only on DVD.
                                  * Includes cdrecord's climb for drives that
                                  * refuse a speed below their minimum with
                                  * 5/24 rather than rounding up. No ABI change.
                                  * 0.28.0: A FAILED BURN NOW RELEASES THE
                                  * DRIVE. No ABI change; the behaviour change
                                  * is that accudisc_write returning an error
                                  * leaves the drive usable. Before this, an
                                  * error unwound the host and left the DEVICE
                                  * holding the DAO session opened by SEND CUE
                                  * SHEET — still answering TEST UNIT READY, so
                                  * not visibly wedged, but refusing READ DISC
                                  * INFORMATION and READ ATIP with 5/2C/00
                                  * COMMAND SEQUENCE ERROR, and a tray cycle did
                                  * not clear it. Measured on a PX-716A after a
                                  * deliberate FIFO starvation, 2026-08-28. The
                                  * release is one FLUSH CACHE (0x35), which is
                                  * cdrdao's entire abortDao() and cdrecord's
                                  * entire generic-MMC abort. Callers that
                                  * treated an error as "the drive may need a
                                  * power cycle" no longer need to.
                                  * 0.27.0: THE WRITE FIFO. accudisc_write_opts
                                  * gained `fifo_bytes` — and it landed in what
                                  * 0.26.0 left as TAIL PADDING, so sizeof is 32
                                  * either way and the `size` field CANNOT tell
                                  * the two apart. The version is the only
                                  * signal, which is why this is a minor bump
                                  * for a struct that did not grow. A 0.26.0
                                  * caller's uninitialised padding would
                                  * otherwise be read as a buffer size; the
                                  * value is clamped on entry for that reason.
                                  * 0.26.0: BURN-Proof is no longer forced on
                                  * every drive. The CD Mastering feature
                                  * (002Eh) is probed, accudisc_features gained
                                  * five write-capability flags (11 -> 16 B) and
                                  * accudisc_write_opts gained `burnproof`
                                  * (24 -> 32 B, appended). Until now the engine
                                  * asked EVERY drive for a failover many do not
                                  * have, and could not tell which — the
                                  * question that decides whether an underrun
                                  * costs a link or costs the disc.
                                  * 0.25.0: the write-offset locator met real
                                  * drive output for the first time and its
                                  * threshold-not-correlation property became a
                                  * documented CONTRACT, pinned by a test that
                                  * locates a FOREIGN waveform. No behaviour
                                  * change — the property was always there,
                                  * nothing protected it.
                                  * 0.24.0: the write-offset MEASUREMENT
                                  * reaches Python. No library change — 0.20.0
                                  * shipped _signal/_locate and the binding
                                  * never exposed them, so two of the three
                                  * documented steps were unreachable from the
                                  * one place a consumer works. Binding surface
                                  * only; the C API is untouched.
                                  * 0.23.0: RIP ACCURACY. accudisc_offset_info
                                  * gained ar_acc_ok/ar_acc_bad, appended, from
                                  * AccurateRip's periodic drive-accuracy
                                  * report. BOTH ZERO MEANS NOT MEASURED — it
                                  * is never a score of nought, and 85% of rows
                                  * carry nothing because the report only
                                  * publishes common drives.
                                  * 0.22.0: THE ACCUBUFFER. A bounded chunk
                                  * ring between the engine and the caller's
                                  * sink, so time spent in the sink is not time
                                  * the drive is not being read.
                                  * accudisc_read_req gained buffer_bytes
                                  * (64 -> 72) and accudisc_read_stats gained
                                  * buffer_peak_chunks + buffer_stalls
                                  * (144 -> 160). Both APPENDED; nothing above
                                  * them moved.
                                  *
                                  * OFF by default in 0.22.0 — CHANGED in
                                  * 0.27.0, where 0 means the default size and
                                  * ACCUDISC_BUFFER_NONE means off. The
                                  * measurement below stands; what changed is
                                  * the conclusion drawn from it, because it
                                  * describes the steady state and a buffer is
                                  * for the tail. Measured first:
                                  * against a write-rate-capped sink at half
                                  * the drive's rate, 95.5% of the disc read
                                  * was ALREADY hidden by the page cache, so
                                  * this earns nothing on an ordinary file sink
                                  * and the header says so. What it earns is
                                  * work done INSIDE the sink callback, which
                                  * no kernel buffer covers.
                                  *
                                  * WHEN ENABLED THE SINK RUNS ON ANOTHER
                                  * THREAD. Overlap requires it. That is a real
                                  * contract change for a caller, which is why
                                  * it is opt-in per read rather than a global
                                  * improvement.
                                  * 0.21.0: Q-POSITION CHECK. A new subq_map
                                  * state, ACCUDISC_SUBQ_MISPOSITION, and a
                                  * new read_stats counter, subq_misposition,
                                  * for the case where a CRC-VALID ADR=1 Q
                                  * frame names a different LBA than the one
                                  * commanded — the drive read somewhere else
                                  * and said so in its own subcode.
                                  *
                                  * Additive, and the struct did NOT grow: the
                                  * counter went into the 4 bytes of tail
                                  * padding the 0.9.0 comment reserved. But
                                  * ACCUDISC_SUBQ_OK now means strictly less
                                  * than it did — a mis-positioned sector used
                                  * to be reported OK and is now its own state.
                                  * A consumer switching exhaustively on the
                                  * lane sees a value it has never seen, which
                                  * is exactly the changed-MEANING case the
                                  * 0.4.0 note bumped the minor for.
                                  *
                                  * Costs nothing when subchannel is not read:
                                  * with no Q there is no claim to contradict,
                                  * and the whole check is skipped.
                                  * 0.20.0: WRITE-OFFSET MEASUREMENT, the last
                                  * of the eight offset items and the one that
                                  * never transferred, because a
                                  * burn-and-read-back is a PROCEDURE rather
                                  * than a table.
                                  * New accudisc_write_offset_signal() and
                                  * accudisc_write_offset_locate(), plus
                                  * accudisc_write_offset_info. The library
                                  * supplies the signal and the arithmetic —
                                  * what every consumer would otherwise
                                  * reimplement and get subtly wrong, the same
                                  * rationale as accudisc_ctdb_repair. It does
                                  * NOT orchestrate: the burn is accudisc_write
                                  * and the read-back is the ordinary read
                                  * path, so no library call destroys a disc.
                                  * THE READ OFFSET IS A REQUIRED INPUT. The
                                  * read-back carries both offsets summed, so a
                                  * defaulted 0 would return a confident number
                                  * wrong by exactly the drive's read offset —
                                  * and 0 is legitimate for hundreds of drives,
                                  * so nothing downstream could tell. The CLI
                                  * refuses rather than assuming.
                                  * TWO pulses, at 1 s and 60 s, so a defective
                                  * disc can be told from a real offset. They
                                  * are never averaged: disagreement returns
                                  * ACCUDISC_ERR_AMBIGUOUS with both values and
                                  * ACCUDISC_WOFF_F_INCONSISTENT.
                                  * Also fixes a PHANTOM NAME: the header and
                                  * gen_offsets.py both cited
                                  * accudisc_measure_write_offset as though it
                                  * shipped. It never existed — a name for work
                                  * not yet done, leaked into the contract.
                                  * ADDITIVE. No existing call, struct or field
                                  * changes.
                                  *
                                  * 0.19.0: THREE DRIVES CHANGE OFFSET. A
                                  * spelling backed by ONE submission was
                                  * answering against a spelling of the SAME
                                  * drive backed by hundreds, and nothing
                                  * collided so nothing refused:
                                  *   DVD-RAM GH24NS95  +667/1  -> +6/1315
                                  *   DVDRAM GSA-E60L   +667/1  -> +102/247
                                  *   DVDRAM- GP65NB60  +102/1  -> +6/1151
                                  *   CDDVDW SE -218GN  +102/1  -> +6/197
                                  * Same vendor, same model number, one
                                  * punctuation mark apart. A CALLER THAT CACHED
                                  * ANY OF THESE HAS A WRONG OFFSET — up to 661
                                  * samples of misalignment, previously reported
                                  * at exit 0 with no warning.
                                  * GH24NS95 and GSA-E60L point OPPOSITE ways —
                                  * hyphenated is the minority in one and the
                                  * majority in the other — which is why
                                  * KEY_ALIAS is a reviewed list and not a
                                  * "strip the hyphen" rule. Of 146 spacing and
                                  * punctuation groups in this corpus 132 agree
                                  * and 14 do not, and some of the 14 are two
                                  * genuinely different drives; the remaining
                                  * ten are deliberately untouched.
                                  * Two more families merge with NO verdict
                                  * change, only pooled evidence: HP DVDROM
                                  * DT30N (+103, 9+3=12) and the two ATAPI CD
                                  * spellings of ATAPI CD-ROM (+12, 1+1=2).
                                  * "16X DVD-" + "ROM" stays its own key, so the
                                  * product "ROM" alone is still ambiguous.
                                  * NO API CHANGE, and no row gained or lost —
                                  * 5881 either way, with every spelling still
                                  * emitted. Getting that right needed two arms
                                  * that did not exist: an aliased REDUMP row is
                                  * KEPT by the retraction rule (dropping it
                                  * deleted three names outright) and
                                  * contributes its SPELLING but no VALUE, since
                                  * REDUMP is AccurateRip's own 2022 import and
                                  * re-entering its copy made merge() read one
                                  * datum as two sources disagreeing.
                                  *
                                  * 0.18.0: ONE DRIVE, SEVERAL NAMES — and a
                                  * phantom product removed.
                                  * New KEY_ALIAS in tools/gen_offsets.py pools
                                  * the evidence of a drive AccurateRip lists
                                  * under several names that no existing fold
                                  * reaches. Lenovo's Ultraslim DVD is listed
                                  * four ways — two badges, the ThinkPlus brand,
                                  * and a spelling missing its space — all at
                                  * +6, 424 submissions between them. A caller
                                  * querying the second was told 21. All four
                                  * spellings now report 424, and EVERY spelling
                                  * is still emitted as its own row: the alias
                                  * pools at build time and never lets the
                                  * runtime answer for a name no source sent.
                                  * EXACT WHOLE-KEY, one line per human
                                  * decision. A rule was measured and REJECTED:
                                  * squashing spacing and punctuation collapses
                                  * 146 groups, of which 132 agree on the offset
                                  * and 14 DO NOT, some being two genuinely
                                  * different drives.
                                  * ALSO: AccurateRip publishes "LG Electronics
                                  * -", a vendor with an EMPTY product. The
                                  * separator rule needed whitespace on BOTH
                                  * sides, so it parsed as the PRODUCT "LG
                                  * ELECTRONICS -" and shipped as a phantom
                                  * string no drive reports, answering +103 to
                                  * anyone who sent it. Now split correctly, it
                                  * is an empty-product row and unreachable —
                                  * which is what a measurement with no product
                                  * identifier deserves. Fixed in the fetcher
                                  * AND repaired on read, because the split is
                                  * baked into the committed json at fetch time.
                                  * Table stays 5881 rows: one changes identity,
                                  * four change their submission count.
                                  *
                                  * 0.17.0: SUBTRACTIVE. One row RETRACTED and
                                  * two products blocked from answering alone,
                                  * all three from ONE cause: the INQUIRY vendor
                                  * field is EIGHT BYTES, and a drive whose name
                                  * is longer simply continues into the product
                                  * field. What lands there is a fragment of a
                                  * name rather than a name.
                                  * "DVDROM 8X" and "DVDROM 10X" are cut at that
                                  * boundary, leaving the products "X" and "0X".
                                  * A ONE-CHARACTER product was answering +564
                                  * for any vendor at all, so both now carry
                                  * ACCUDISC_OFFSET_F_GENERIC: the rows still
                                  * ship and still answer for the vendor half
                                  * they were measured under, which is the whole
                                  * of the drive's reported identity. +564 is
                                  * right for that generation — 14 other rows
                                  * hold it — so dropping them would ignore data
                                  * to fix a matching rule.
                                  * GENERIC_PRODUCTS is therefore no longer only
                                  * category words. Two causes, one remedy, and
                                  * no rule detects either: the corpus is
                                  * faithfully recording what the firmware
                                  * reported, and the firmware is wrong.
                                  * SEPARATELY, ("DVDROM", "") at +564 is gone.
                                  * It was in AccurateRip's 2022 list and is
                                  * absent from the live one, so it should have
                                  * been dropped as RETRACTED in 0.13.0. It was
                                  * not: AccurateRip writes a vendor with no
                                  * product as "DVDROM -", and the generator's
                                  * provenance parser needed whitespace on BOTH
                                  * sides of the separator, so the name keyed as
                                  * the PRODUCT "DVDROM -" and joined nothing.
                                  * A miss there is not loud — apply_retractions
                                  * takes its unjoined branch and continues, so
                                  * the live-AR check never runs and the row is
                                  * KEPT, merely counted as unknown provenance.
                                  * Table 5882 -> 5881, retracted 6 -> 7,
                                  * unknown provenance 14 -> 13.
                                  * NO API CHANGE. It was the LAST empty-product
                                  * row, so that guard now has nothing in the
                                  * shipped table to catch; it stays for the next
                                  * corpus refresh, and is tested against a
                                  * fixture that does hold one.
                                  *
                                  * 0.16.0: SUBTRACTIVE, and the other half of
                                  * 0.15.0. Six product strings that name a
                                  * CATEGORY rather than a model — DVD, DVDRW,
                                  * DVD+RW, COMBO, OPTICAL DRIVE, CD-ROM DRIVE —
                                  * no longer answer a product-only query. Each
                                  * was submitted by exactly one drive, so
                                  * nothing collided with it and 0.15.0 handed
                                  * that one drive's offset to any caller
                                  * reporting the same category word.
                                  * THE ROWS STILL SHIP and still answer for the
                                  * vendor that submitted them: "BUFFALO
                                  * OPTICAL DRIVE" rests on 85 submissions, and
                                  * dropping it would ignore data to fix a
                                  * matching rule. New
                                  * ACCUDISC_OFFSET_F_GENERIC marks such a row
                                  * and is reported when the vendor narrowed to
                                  * it, so a caller knows the vendor earned the
                                  * answer.
                                  * NOT extended to generic names that COLLIDE
                                  * ("CD-ROM", four offsets): those already
                                  * refuse to pick, which is a safer failure
                                  * than a confident wrong number.
                                  * 0.15.0: THE MATCHING RULE CHANGED. No
                                  * struct moved, no error code changed, no
                                  * function signature moved — what changed is
                                  * WHICH DRIVES MATCH. The lookup keys on the
                                  * PRODUCT identifier and lets the vendor
                                  * narrow: a vendor matching no row no longer
                                  * rejects, because firmware reports that field
                                  * inconsistently and requiring it to match
                                  * answers only for the spelling one submitter
                                  * sent. A caller that cached "this drive is
                                  * unknown" is holding a stale answer.
                                  * Ambiguity is counted in DISTINCT OFFSETS,
                                  * not matching rows — the table carries a row
                                  * per spelling, so several rows matching is
                                  * normal and means nothing.
                                  * Verified across all 5882 keys in the table
                                  * that exactly ONE answer changes: 'DVDROM'
                                  * with an EMPTY product, which now returns
                                  * ERR_NOTFOUND. An empty product identifies
                                  * nothing, and keyed on the product alone it
                                  * would have answered for every drive
                                  * reporting no product string.
                                  * New ACCUDISC_OFFSET_F_TRUNCATED says
                                  * values[] could not carry every distinct
                                  * offset found; n_values alone cannot.
                                  * 0.14.0: ADDITIVE, in the data. No struct
                                  * moved and no error code changed. A REVIEWED
                                  * REBADGE TABLE now keeps a retracted row
                                  * where a cited, exact mapping says it is a
                                  * rebadge of a live OEM drive AND the two
                                  * agree on the offset. One row so far:
                                  * Philips DVD-ROM PCDV632 +116, a Toshiba
                                  * SD-M1212 rebadge carrying +116 on 38
                                  * submissions, so that name returns an offset
                                  * instead of ERR_NOTFOUND.
                                  * IT SHIPS AS REDUMP-ONLY WITH ZERO AR
                                  * FIGURES: the submissions were made against
                                  * the OEM name, and claiming them for this one
                                  * would be a measurement nobody took. The
                                  * corroboration is named in offsets_db.inc.
                                  * The table is RESCUE-ONLY — it may keep a row
                                  * that exists and may never synthesise one,
                                  * which the generator asserts rather than
                                  * trusting a reviewer to remember.
                                  * 0.13.0: ADDITIVE, in the data. No struct
                                  * moved and no error code changed. The
                                  * build-time key now folds UNDERSCORE to
                                  * SPACE, because AccurateRip spells one drive
                                  * both ways — "DVDRAM_GHA2N" beside
                                  * "DVDRAM GHA2N" — and a separator is a
                                  * spelling of a name, not a different drive.
                                  * Two names that returned ERR_NOTFOUND now
                                  * return an offset (HL-DT-ST DVDRAM_GHA2N
                                  * +667, TSSTcorp CDDVDW +6), 96 rows carry a
                                  * higher ar_submissions because the spellings
                                  * pool their evidence, and NO offset changed
                                  * on any name that already resolved.
                                  * THE RUNTIME STILL DOES NOT FOLD
                                  * UNDERSCORES: every spelling is emitted as
                                  * its own row and matched literally, which
                                  * the generator now asserts against a fresh
                                  * read of its own input rather than claiming
                                  * in a comment.
                                  * 0.12.0: SUBTRACTIVE, in the data. No
                                  * struct moved and no error code changed. The
                                  * table now drops REDUMP values AccurateRip
                                  * has withdrawn — REDUMP's table being
                                  * AccurateRip's own list frozen in 2022, not a
                                  * second source. 8 keys that returned an
                                  * offset now return ERR_NOTFOUND, and 9 return
                                  * a DIFFERENT offset than they did in 0.11.0;
                                  * every one is named in offsets_db.inc. A
                                  * caller that cached a per-drive offset must
                                  * re-query, which is what the bump is for. The
                                  * table also has NO conflicting keys left, so
                                  * ERR_AMBIGUOUS is currently unreachable from
                                  * the shipped data — the code path stays,
                                  * because the next corpus refresh can revive
                                  * it.
                                  * 0.11.0: offset matching became
                                  * case-INSENSITIVE. No struct moved and no
                                  * error code changed — what changed is which
                                  * drives MATCH. A firmware reporting "AOpen"
                                  * against a table row storing "AOPEN" used to
                                  * return ERR_NOTFOUND and now returns its
                                  * offset, so a caller that cached "this drive
                                  * is unknown" is holding a stale answer.
                                  * Verified lossless first: of 5888 rows, ZERO
                                  * pairs differ only by case, so folding
                                  * collides nothing that was distinct.
                                  * 0.10.0: the offset portal. New
                                  * accudisc_offset_for_inquiry /
                                  * accudisc_offset_for_device and the
                                  * accudisc_offset_info they fill; new
                                  * ACCUDISC_ERR_AMBIGUOUS (-14). Additive in
                                  * layout, but accudisc_read_offset CHANGED
                                  * MEANING: a drive whose sources disagree now
                                  * returns ERR_AMBIGUOUS where it used to
                                  * return whichever row the table listed first.
                                  * That is a silent-wrong-answer path closing,
                                  * and a caller mapping error codes to actions
                                  * needs the bump to notice it.
                                  * 0.9.0: accudisc_speed_rung gained band_cx[3]
                                  * — the per-radius figures the sweep already
                                  * measured and then threw away, keeping only
                                  * their min and max. 14 -> 20 bytes. THIS IS A
                                  * HARD BREAK: the rung is an OUT ARRAY with no
                                  * size field, so a caller allocating 14-byte
                                  * elements against a 20-byte library is
                                  * overrun, not truncated. Rebuild both sides.
                                  * 0.8.0: SUBTRACTIVE. Removed the uncap's
                                  * driver-free INFERENCE — the stock-ceiling
                                  * table, adsc_uncap_classify, and the
                                  * ACCUDISC_UNCAP_LIKELY_ON value it returned
                                  * (2, now retired and never to be reused).
                                  * accudisc_speed_uncap_probe still reports
                                  * max_x but no longer draws a verdict from it:
                                  * page 2A reports what the drive ACCEPTS, not
                                  * what it delivers, so the comparison was not
                                  * evidence. No struct moved; the ENUM did, and
                                  * a consumer switching on it needs to know.
                                  * 0.7.0: accudisc_read_stats gained
                                  * speed_requested_x/speed_honoured_x and grew
                                  * 136 -> 144 bytes. Additive at the END of an
                                  * OUT struct, so a shorter caller is refused
                                  * rather than truncated (adsc_abi_export) and
                                  * simply never sees the fields — soname stays
                                  * .so.0. Bumped because the layout moved,
                                  * which is what the minor is for.
                                  * 0.6.0: the FIRST SUBTRACTIVE change. Removed
                                  * accudisc_read_req.allow_unsafe and
                                  * ACCUDISC_ERR_UNSAFE_COMBINATION (-11, now
                                  * retired and never to be reused) along with
                                  * the SpeedRead subchannel guard they served.
                                  * The `size` field CANNOT cover this: it makes
                                  * a SHORT caller safe by zero-extending, and
                                  * says nothing about a caller that sets a
                                  * field this build no longer has. sizeof is
                                  * unchanged at 64 — the byte was padding — so
                                  * a stale caller's `allow_unsafe = 1` lands in
                                  * padding and is ignored, which is the benign
                                  * outcome but is NOT something the ABI
                                  * machinery guarantees. The minor bump is the
                                  * only real signal, which is why it is here.
                                  * 0.5.0: accudisc_read_req gained subq_map and
                                  * grew 56 -> 64 bytes. Purely additive (the
                                  * field is last, an older caller zero-extends
                                  * to NULL), so the soname stays .so.0 — but
                                  * the layout moved, which is exactly what the
                                  * minor is for.
                                  * 0.4.0: ACCUDISC_ERR_NOT_BLANK = -13 split
                                  * out of ERR_UNSUPPORTED on the write path. No
                                  * struct moved — bumped anyway, because a
                                  * consumer that maps error codes to user
                                  * actions is as broken by a silently changed
                                  * MEANING as by a moved field, and the version
                                  * is the only signal it gets.
                                  * 0.3.0: accudisc_speed_rung 6 -> 14 bytes and
                                  * accudisc_write_opts gained `size`; the probe
                                  * is now bound, so the rung layout is frozen.
                                  * 0.2.0: read_req/read_stats layout changed
                                  * (API_PLAN §7.1). soname stays .so.0. */
#define ACCUDISC_VERSION_PATCH 0 /* 0.12.1: ar_submissions got more accurate,
                                  * not different in meaning. AccurateRip lists
                                  * some drives twice; where the duplicate rows
                                  * AGREE on the offset their counts are now
                                  * summed instead of the largest being kept.
                                  * 91 rows carry a higher count, one carries a
                                  * lower agreement (the weighted mean of 100%
                                  * and 75%), and NO offset moved. */

#if defined(_WIN32)
#  define ACCUDISC_API __declspec(dllexport)
#else
#  define ACCUDISC_API __attribute__((visibility("default")))
#endif

/* ---- version ------------------------------------------------------------- */

/* Version of the library actually linked (compare against the macros
 * above to detect header/library skew). */
ACCUDISC_API const char *accudisc_version_string(void);
ACCUDISC_API void accudisc_version(int *major, int *minor, int *patch);

/* Free any buffer the library allocated for the caller (raw TOC/CD-Text
 * dumps). Bindings must route through this, not their runtime's free. */
ACCUDISC_API void accudisc_free(void *p);

/* ---- CD-DA sizes ----------------------------------------------------------
 * One CD-DA sector = 1/75 s of audio = 2352 bytes. The optional per-sector
 * companions a drive can return alongside it in the same READ CD transfer: */
#define ACCUDISC_BYTES_AUDIO   2352 /* raw s16le PCM user data */
#define ACCUDISC_BYTES_C2      294  /* C2 pointers: 2352 bits, 1/byte, MSB-first */
#define ACCUDISC_BYTES_C2_BEB  296  /* C2 + block-error-bits variant */
#define ACCUDISC_BYTES_SUB_RAW 96   /* raw P-W subcode, interleaved */
#define ACCUDISC_BYTES_SUB_Q   16   /* formatted Q subchannel block */

/* ---- errors ---------------------------------------------------------------
 * All fallible functions return ACCUDISC_OK (0) or a negative accudisc_err.
 * ACCUDISC_ERR_SENSE means the drive itself rejected the command
 * (CHECK CONDITION) — the decoded sense is available via
 * accudisc_last_sense() on the device the call was made against. */
typedef enum accudisc_err {
    ACCUDISC_OK              = 0,
    ACCUDISC_ERR_INVAL       = -1, /* invalid argument */
    ACCUDISC_ERR_NOMEM       = -2, /* allocation failure */
    ACCUDISC_ERR_OPEN        = -3, /* device could not be opened */
    ACCUDISC_ERR_IO          = -4, /* transport/host/driver failure */
    ACCUDISC_ERR_SENSE       = -5, /* drive returned CHECK CONDITION */
    ACCUDISC_ERR_SHORT       = -6, /* response shorter than required */
    ACCUDISC_ERR_UNSUPPORTED = -7, /* not supported by drive or build */
    ACCUDISC_ERR_CANCELLED   = -8, /* stopped by cancel flag or sink */
    ACCUDISC_ERR_CRC         = -9, /* checksum failed (Q frame, CD-Text pack) */
    ACCUDISC_ERR_NOTFOUND    = -10, /* requested data legitimately absent
                                      (MCN/ISRC/CD-Text/driver/offset) —
                                      never a transport failure */
    /* -11 IS RETIRED AND MUST NEVER BE REUSED. It was
     * ACCUDISC_ERR_UNSAFE_COMBINATION, removed in 0.6.0 with the SpeedRead
     * guard it existed to report (Keith's ruling, 2026-08-09: the drive cannot
     * read CD-DA above 40x whatever the uncap says, so the combination it
     * called unsafe was never unsafe). A future error assigned -11 would be
     * mapped to "unsafe combination" by every consumer compiled before 0.6.0 —
     * well-formed data, wrong referent, nothing able to tell. The gap is
     * cheaper than the collision. */
    ACCUDISC_ERR_ABI         = -12, /* a caller-allocated struct declared a
                                      `size` this library cannot honour: zero
                                      (never initialised), or larger than this
                                      build's, with fields set that this build
                                      does not know. Distinct from ERR_INVAL
                                      because it means "rebuild against this
                                      header", not "fix your arguments". */
    ACCUDISC_ERR_AMBIGUOUS   = -14, /* the answer exists but is not unique, and
                                     * picking one would be a guess. Today: an
                                     * offset key whose sources disagree. The
                                     * candidates come back with the error so the
                                     * caller can choose; see accudisc_offset_info */
    ACCUDISC_ERR_NOT_BLANK   = -13 /* accudisc_write refused: the loaded disc
                                      is not blank. Nothing was written.
                                      Split out of ERR_UNSUPPORTED in 0.4.0.

                                      Why it is its own code rather than a
                                      documented special case: it was exact
                                      only BY CENSUS. ERR_UNSUPPORTED was
                                      reachable from accudisc_write in exactly
                                      one place, so callers could map it to
                                      "insert a blank disc" — but any future
                                      ERR_UNSUPPORTED under the write path
                                      would have joined that meaning silently,
                                      and the resulting failure is well-formed
                                      at both ends: the caller tells the user
                                      to insert a blank disc they are already
                                      holding, and no test on either side can
                                      tell. A distinct code makes the mapping
                                      true BY CONSTRUCTION.

                                      ERR_UNSUPPORTED keeps its ordinary
                                      meaning on this path ("the drive or this
                                      build cannot do what was asked"), and is
                                      now free to appear there without
                                      colliding. */
} accudisc_err;

/* Static human-readable name for an accudisc_err value. */
ACCUDISC_API const char *accudisc_strerror(int err);

/* Decoded SCSI sense from the most recent failed command on a device.
 * valid is 0 when the failure produced no usable sense data. */
typedef struct accudisc_sense {
    uint8_t valid;
    uint8_t key;  /* sense key, e.g. 3 = MEDIUM ERROR, 4 = HARDWARE ERROR */
    uint8_t asc;  /* additional sense code */
    uint8_t ascq; /* additional sense code qualifier */
} accudisc_sense;

/* ---- device ---------------------------------------------------------------
 * A handle to one optical drive. Handles are not thread-safe; use one per
 * thread or serialize externally. (Reading a status map while another thread
 * drives the device is safe and is the intended progress-tracking pattern.) */
typedef struct accudisc_device accudisc_device;

/* Open read-write. Required for vendor opcodes, MODE SELECT, and writing:
 * the kernel's unprivileged SG_IO command filter blocks those on read-only
 * fds. Plain reading should not set this (least privilege). */
#define ACCUDISC_OPEN_RDWR 0x1u

/* Open the drive at path (e.g. "/dev/sr0"). Returns NULL on failure; if err
 * is non-NULL it receives the accudisc_err cause. */
ACCUDISC_API accudisc_device *accudisc_open(const char *path, unsigned flags,
                                            int *err);
ACCUDISC_API void accudisc_close(accudisc_device *dev);

/* Sense from the most recent ACCUDISC_ERR_SENSE/_IO failure on dev. */
ACCUDISC_API void accudisc_last_sense(const accudisc_device *dev,
                                      accudisc_sense *out);

/* Why the most recent ACCUDISC_ERR_IO happened, as a short human string —
 * "timeout (driver=0x06 host=0x00)", "ioctl: No medium found",
 * "host=0x07 driver=0x00". An ERR_IO carries no sense data, so without this a
 * transport failure cannot be attributed after the fact. Returns "" when the
 * last command succeeded or failed some other way. Owned by dev; valid until
 * the next command. */
ACCUDISC_API const char *accudisc_last_io(accudisc_device *dev);

/* INQUIRY identification strings, space-trimmed and NUL-terminated. */
typedef struct accudisc_drive_id {
    char vendor[9];
    char product[17];
    char revision[5];
} accudisc_drive_id;

ACCUDISC_API int accudisc_drive_identify(accudisc_device *dev,
                                         accudisc_drive_id *out);

/* --- Drive offsets: the single portal ---------------------------------
 *
 * AccuDisc REPORTS offsets and never applies one. The caller stores the number
 * and corrects exactly once, at storage — one offset domain, one site for the
 * shift. A library able to apply would give every consumer a second such site,
 * and double correction is silent: the output is well-formed PCM, wrong by
 * twice the offset, and no downstream gate can see it.
 *
 * THE UNIT IS A SAMPLE = ONE STEREO FRAME = 4 BYTES, 588 per sector. This is
 * REDUMP's unit and AccurateRip's, and the reason the splice arithmetic is
 * `track_start * 2352 + read_offset * 4`. A reader who assumes 2 bytes is wrong
 * by exactly 2x. Sign convention: POSITIVE means the drive reads early.
 *
 * The table is compiled from the two live primary sources on the development
 * cycle (tools/gen_offsets.py) — READ offsets only; a write offset is a
 * MEASUREMENT (accudisc_write_offset_signal / _locate below), never a table
 * lookup. There is no runtime lookup of any kind, online or
 * otherwise. Where sources disagree the query says so and refuses to pick;
 * where nothing holds the drive it says that instead. Both are explicit
 * outcomes rather than a default, because a plausible wrong offset is worse
 * than no offset. */

/* Which collections hold the returned value. Presence, not confidence: only
 * AccurateRip publishes per-drive counts, so a "combined score" across the
 * three would be dominated by whichever source can count — ranking a
 * corroborated drive below an uncorroborated one. */
#define ACCUDISC_OFFSET_SRC_REDUMP 0x01u
#define ACCUDISC_OFFSET_SRC_AR     0x02u

#define ACCUDISC_OFFSET_F_CONFLICT    0x01u /* sources disagree; see values[] */
#define ACCUDISC_OFFSET_F_TRUNCATED   0x02u /* MORE distinct offsets exist than
                                             * values[] can carry, so n_values
                                             * is a cap and not a count. Without
                                             * this a caller reads "4 values"
                                             * and cannot tell it from "4 of the
                                             * 6 we found" — a silent narrowing
                                             * of exactly the kind the sentinel
                                             * read_offset exists to prevent.
                                             * Unreachable from the shipped
                                             * table, whose worst product holds
                                             * exactly ACCUDISC_OFFSET_MAX_VALUES
                                             * offsets; set the moment a corpus
                                             * refresh exceeds it */
#define ACCUDISC_OFFSET_F_GENERIC     0x08u /* this row's PRODUCT string names a
                                             * category rather than a model
                                             * ("DVD", "OPTICAL DRIVE"), so it
                                             * cannot identify a drive on its
                                             * own. Such a row is reachable ONLY
                                             * when the caller's vendor narrows
                                             * to it; a product-only query does
                                             * not see it. Set here when it did
                                             * narrow, so a caller knows the
                                             * vendor is what earned the answer
                                             * and the product alone would not
                                             * have. Reviewed by hand, one entry
                                             * per decision — see
                                             * GENERIC_PRODUCTS in
                                             * tools/gen_offsets.py */
#define ACCUDISC_OFFSET_F_ADJUDICATED 0x04u /* sources disagreed and two or more
                                             * agreed on this value; the losing
                                             * value(s) were dropped at build
                                             * time. Reported, not hidden. */

/* Not a value any drive can have: INT32_MIN. Deliberately NOT 0 — zero is a
 * legitimate offset for hundreds of real drives, so a caller that ignored the
 * return code and used a zeroed field would apply a plausible wrong
 * correction. This one cannot be mistaken for a measurement. */
#define ACCUDISC_OFFSET_NONE       (-2147483647 - 1)
#define ACCUDISC_OFFSET_MAX_VALUES 4

#define ACCUDISC_OFFSET_INFO_INIT { .size = sizeof(accudisc_offset_info) }

typedef struct accudisc_offset_info {
    uint32_t size;           /* = sizeof(accudisc_offset_info). NOT optional */
    int32_t  read_offset;    /* samples; ACCUDISC_OFFSET_NONE unless the call
                              * returned ACCUDISC_OK */
    uint16_t ar_submissions; /* AccurateRip's count, 0 if AR does not hold it */
    uint8_t  ar_agree_pct;   /* AccurateRip's own agreement rate for the drive,
                              * 0 if unknown. A WITHIN-source measure, so unlike
                              * cross-source agreement it does not depend on the
                              * sources being independent — which they are NOT;
                              * see `sources` below */
    uint8_t  sources;        /* ACCUDISC_OFFSET_SRC_* bitmask.
                              *
                              * PRESENCE, NOT CORROBORATION, and weaker than it
                              * looks. Both bits set does NOT mean two parties
                              * measured this drive: REDUMP's offset table is
                              * AccurateRip's published list, imported once in
                              * 2022 and frozen. Established 2026-08-22 by set
                              * comparison against redumper's own git history —
                              * 4595 rows each way, ZERO in either that the
                              * other lacks, the only change being marketing
                              * vendor names rewritten to INQUIRY ones. Both
                              * bits therefore mean ONE source agreeing with its
                              * own past, which is worth something (the value
                              * has not been revised) and is not a second
                              * witness. Weight it as "unrevised since 2022",
                              * never as corroboration. ar_submissions is the
                              * only count of actual measurements here */
    uint8_t  flags;          /* ACCUDISC_OFFSET_F_* bitmask */
    uint8_t  n_values;       /* values[] filled; 1 unless ERR_AMBIGUOUS */
    int32_t  values[ACCUDISC_OFFSET_MAX_VALUES];       /* every value found */
    uint8_t  value_sources[ACCUDISC_OFFSET_MAX_VALUES]; /* who holds each one */

    /* RIP ACCURACY — appended in 0.23.0, so every field above keeps its offset.
     *
     * Tracks this drive submitted to AccurateRip that matched the reference,
     * and that did not. BOTH ZERO MEANS NOT MEASURED. That is the same
     * convention ar_submissions already uses, and it is deliberate: there is no
     * "0% accurate" here to confuse it with, and a caller must not invent one.
     *
     * WHY ABSENCE IS THE COMMON CASE AND MEANS NOTHING BAD. The report only
     * covers drives with over 4000 submissions from 40+ users, so roughly one
     * row in seven of our table carries a figure. A drive is missing because it
     * is UNCOMMON. Scoring absence as zero would rank every rare drive below
     * the worst measured one — the exact inversion the data cannot support.
     *
     * A PRIOR, NOT A GATE, AND NOT ABOUT YOUR DISC. The figure rests on the
     * report's own premise that every drive's owners have equally damaged discs
     * on average; it therefore measures a population of owners as much as a
     * drive. Fine for "this drive has a good history", useless for "this rip is
     * good". AccurateRip and CTDB, in the calling application, remain the
     * absolute gates (docs/reference/RECOVERY.md). Nothing in the library
     * enforces this — it is a documented limit, not a check.
     *
     * GRANULARITY DIFFERS FROM ar_submissions/ar_agree_pct. Those describe the
     * single row that answered. These describe the whole group of INQUIRY
     * spellings the build-time join treats as one drive, so sibling rows repeat
     * the same counts. The two pairs are not commensurate; do not combine them.
     *
     * Derive a percentage only when the denominator is non-zero:
     *     if (i.ar_acc_ok + i.ar_acc_bad)
     *         pct = 100.0 * i.ar_acc_ok / (i.ar_acc_ok + i.ar_acc_bad);
     * The counts rather than a stored percentage, because the denominator is
     * the confidence: the published rows run from ~4k submissions to ~81k. */
    uint32_t ar_acc_ok;      /* accurate submissions, 0 if not measured */
    uint32_t ar_acc_bad;     /* inaccurate submissions, 0 if not measured */
} accudisc_offset_info;

/* Look up a drive by INQUIRY strings, WITHOUT a device. The table is drive
 * knowledge, not a hardware operation, so asking about a drive that is not in
 * the tray is a normal thing to want — a caller with a corpus of captures from
 * many drives cannot open any of them.
 *
 * MATCHING is case-INSENSITIVE and collapses whitespace runs, so pass the
 * INQUIRY bytes as the drive reported them and do not pre-normalise: real
 * drives pad these fixed fields ("DVDR   PX-716A") and vendors disagree with
 * themselves about capitalisation ("AOpen" and "AOPEN" are one company). The
 * table is stored upper-cased and the query is folded to match. Aliasing is
 * NOT done here — HL-DT-ST and LG ELECTRONICS are one company but two INQUIRY
 * strings, and the table carries a row for each rather than the lookup
 * asserting the identity.
 *
 * SINCE 0.15.0 THE KEY IS THE PRODUCT; THE VENDOR ONLY NARROWS. A vendor that
 * matches no row is not a rejection — firmware reports that field
 * inconsistently (empty, the host adapter's "SATA", the OEM rather than the
 * badge, run into the product), so requiring it to match answers only for the
 * spelling one submitter happened to send. Pass whatever the drive reported,
 * including an empty vendor: if some row with this product carries that vendor
 * the answer narrows to those rows, and otherwise every row with this product
 * is considered. AN EMPTY PRODUCT is refused outright — it identifies nothing,
 * and keyed on the product alone it would answer for every drive that reports
 * no product string.
 *
 * Ambiguity is therefore counted in DISTINCT OFFSETS, not matching rows. The
 * table deliberately carries a row per spelling, so several rows matching is
 * the normal case and says nothing about disagreement.
 *
 * Returns ACCUDISC_OK (one value, in read_offset), ACCUDISC_ERR_AMBIGUOUS (the
 * candidates disagree: n_values/values/value_sources are filled, read_offset
 * stays ACCUDISC_OFFSET_NONE, and the caller must choose and pass its choice
 * through its own configuration — check ACCUDISC_OFFSET_F_TRUNCATED, which says
 * values[] could not carry them all), or ACCUDISC_ERR_NOTFOUND (no row holds
 * this product, or the product was empty).
 *
 * ar_submissions/ar_agree_pct on an ACCUDISC_OK answer describe the SINGLE
 * best-evidenced row backing that offset, as a pair. Several AccurateRip
 * entries can back one offset under different vendor spellings; their counts
 * are not summed, because the table gives every spelling of one entry the same
 * pooled figure and adding them would multiply it.
 *
 * ar_acc_ok/ar_acc_bad are filled on ACCUDISC_OK and CLEARED on
 * ACCUDISC_ERR_AMBIGUOUS, for the same reason the AccurateRip figures are: an
 * ambiguous product matched rows from several vendors, which are different
 * drives, and no single accuracy figure describes them. */
ACCUDISC_API int accudisc_offset_for_inquiry(const char *vendor,
                                             const char *product,
                                             accudisc_offset_info *out);

/* The same query, keyed on an open device's INQUIRY. */
ACCUDISC_API int accudisc_offset_for_device(accudisc_device *dev,
                                            accudisc_offset_info *out);

/* Manufacturing read offset in samples for the identified drive (positive:
 * the drive reads early), from the built-in offset table.
 * ACCUDISC_ERR_NOTFOUND when the model is unknown, and since 0.10.0
 * ACCUDISC_ERR_AMBIGUOUS when the sources disagree — previously such a drive
 * silently returned whichever row the table listed first. Prefer
 * accudisc_offset_for_inquiry, which can report what the disagreement was. */
ACCUDISC_API int accudisc_read_offset(accudisc_device *dev, int32_t *samples);

/* ---------------------------------------------------------------------------
 * WRITE OFFSET — a measurement, and why there is no table for it
 *
 * A drive's WRITE offset is how far early or late it lays audio down on a disc.
 * Unlike the read offset it cannot be looked up here: no live source publishes
 * one. (AccurateRip and REDUMP publish READ offsets only — redumper's
 * offsets.ixx declares DriveReadOffset and nothing else, and its "write offset"
 * code is per-DISC detection, a property of the disc in the tray rather than of
 * the drive. The ~112 drive write offsets that did exist were EAC's OffsetBase,
 * dropped as a source in 2026-08: a 2004 Wayback snapshot of a page that no
 * longer exists is not an independent collection.)
 *
 * So it is obtained by BURNING A KNOWN SIGNAL AND READING IT BACK. That is a
 * procedure, and the procedure belongs to the caller: this library supplies the
 * two pieces every consumer would otherwise reimplement and get subtly wrong —
 * the signal, and the arithmetic that finds it again. The burn is
 * accudisc_write and the read-back is the ordinary read path.
 *
 *     1. accudisc_write_offset_signal() -> PCM
 *     2. caller burns it (accudisc_write), reads it back
 *     3. accudisc_write_offset_locate(read-back, drive READ offset) -> W
 *
 * SIGN CONVENTION, the same one the read offset uses: POSITIVE means the drive
 * burns LATE — the audio sits W samples further into the disc than it should.
 * To correct a burn, shift the source by -W: W > 0 trims W samples from the
 * front, W < 0 prepends |W| samples of silence.
 *
 * THE READ OFFSET IS AN INPUT, NOT AN OPTIONAL ONE. What comes back off the
 * disc carries both offsets summed, so the write offset is only recoverable if
 * the read offset is already known. A caller that does not know it must say so
 * rather than pass 0: zero is a legitimate read offset for hundreds of drives,
 * so a defaulted 0 returns a confident number that is wrong by exactly the
 * drive's read offset. Same reasoning as ACCUDISC_OFFSET_NONE.
 * ------------------------------------------------------------------------- */

/* Signal geometry. Fixed constants rather than parameters so that a disc burnt
 * by one tool can be measured by another — the positions are the contract, and
 * the same two are used by cdda2img's `setup --write-offset`. 75 seconds with a
 * one-frame noise burst at 1 s and at 60 s: two independent measurements of the
 * same quantity, which is what lets a defective disc be told from a real
 * offset. Both sit inside AccurateRip's 2940-sample exclusion boundary, so a
 * disc made this way still verifies. */
#define ACCUDISC_WOFF_SAMPLES   3307500u /* stereo sample pairs = 75 s */
#define ACCUDISC_WOFF_PULSE_A     44100u /* 1 s  */
#define ACCUDISC_WOFF_PULSE_B   2646000u /* 60 s */
#define ACCUDISC_WOFF_PULSE_LEN     588u /* one CD frame */
#define ACCUDISC_WOFF_SEARCH       8820  /* +/- samples scanned around each
                                          * expected position: 200 ms, far
                                          * wider than any real offset (the
                                          * read-offset corpus tops out near
                                          * 1300) and far narrower than the
                                          * 59-second gap between the pulses,
                                          * so the two windows cannot overlap
                                          * and a hit is unambiguous */

#define ACCUDISC_WOFF_F_INCONSISTENT 0x01u /* the two pulses disagree. The
                                            * result is still returned — it is
                                            * evidence about the DISC — but it
                                            * is not a drive measurement, and a
                                            * caller must not store it as one */

#define ACCUDISC_WRITE_OFFSET_INFO_INIT \
    { .size = sizeof(accudisc_write_offset_info) }

typedef struct accudisc_write_offset_info {
    uint32_t size;         /* = sizeof(accudisc_write_offset_info). NOT optional */
    int32_t  write_offset; /* samples; ACCUDISC_OFFSET_NONE unless the call
                            * returned ACCUDISC_OK */
    int32_t  offset_a;     /* per-pulse results, so a caller can see WHY a */
    int32_t  offset_b;     /* run was rejected rather than only that it was */
    int32_t  found_a;      /* where each pulse actually landed, in READ-OFFSET-
                            * CORRECTED sample coordinates; ACCUDISC_OFFSET_NONE
                            * if that pulse was not located at all */
    int32_t  found_b;
    uint8_t  flags;        /* ACCUDISC_WOFF_F_* */
} accudisc_write_offset_info;

/* Fill pcm with the test signal: `samples` stereo pairs, 2 int16 each, s16 host
 * order — ACCUDISC_WOFF_SAMPLES pairs is the full 75 s. Deterministic: the same
 * bytes every call, on every machine, so a disc can be re-measured later.
 *
 * Returns ACCUDISC_ERR_INVAL if pcm is NULL or samples is not exactly
 * ACCUDISC_WOFF_SAMPLES — a short signal would place pulse B off the end and
 * silently halve the measurement to one pulse. */
ACCUDISC_API int accudisc_write_offset_signal(int16_t *pcm, uint32_t samples);

/* Locate both pulses in audio read back off the burnt disc and report the write
 * offset. `pcm` is the read-back, `samples` stereo pairs; `read_offset` is the
 * READING drive's offset in samples (see above — it is required, and passing a
 * wrong one biases the answer by exactly its error).
 *
 * THE LOCATOR IS THRESHOLD-BASED, NOT CORRELATION-BASED, and that is a
 * CONTRACT rather than an implementation note: any sufficiently loud burst at
 * the documented positions measures the same, whoever generated it. Measured
 * 2026-08-27 — cdda2img ran this against a real PX-716A read-back of a disc
 * burnt from THEIR generator (a different noise distribution, no forced leading
 * edge) and it located both pulses, agreeing with their own tool on the offset
 * and on both absolute positions. The GEOMETRY above is what interoperability
 * rests on; the noise is not part of the contract and the seed never was.
 *
 * ACCUDISC_OK when both pulses were found AND agree. ACCUDISC_ERR_NOTFOUND when
 * either could not be located — found_a/found_b say which. ACCUDISC_ERR_AMBIGUOUS
 * when both were found and DISAGREE: that is a defective disc rather than a
 * drive property, so it is refused rather than averaged, and F_INCONSISTENT is
 * set with both values left in place. */
ACCUDISC_API int accudisc_write_offset_locate(const int16_t *pcm, uint32_t samples,
                                              int32_t read_offset,
                                              accudisc_write_offset_info *out);

/* ATIP (Absolute Time In Pregroove) of a recordable disc: the lead-in start
 * time doubles as the manufacturer identification code (97:SS:FF for CD-R),
 * the lead-out last-possible start gives the disc capacity, and `erasable`
 * distinguishes CD-RW. `manufacturer` is looked up from the built-in ATIP
 * catalog (NULL if the code is not listed). All fields are reported raw as the
 * disc encodes them; AccuDisc does not judge them. */
typedef struct accudisc_atip {
    uint8_t lead_in_min, lead_in_sec, lead_in_frame;
    uint8_t lead_out_min, lead_out_sec, lead_out_frame;
    int         erasable;      /* 1 = CD-RW, 0 = CD-R, -1 = unknown */
    const char *manufacturer;  /* static string, or NULL if unlisted */
} accudisc_atip;

/* Read and decode the disc ATIP. Returns ACCUDISC_ERR_NOTFOUND when the drive
 * answers but the disc carries no ATIP (e.g. a pressed CD), distinct from
 * ACCUDISC_ERR_SENSE. Non-destructive (a read). */
ACCUDISC_API int accudisc_read_atip(accudisc_device *dev, accudisc_atip *out);

/* Look up a manufacturer name from an ATIP code directly (min:sec:frame).
 * Matches on min:sec (the manufacturer key); frame is a per-media variant.
 * Returns a static string or NULL. Pure function, no device needed. */
ACCUDISC_API const char *accudisc_atip_manufacturer(uint8_t min, uint8_t sec,
                                                    uint8_t frame);

/* ---- recording (DAO write) -------------------------------------------------
 * Burn one audio session Disc-At-Once. The caller supplies a cdrdao .toc and
 * the raw audio BIN it references; AccuDisc only moves the bits. Requires a
 * blank disc and an ACCUDISC_OPEN_RDWR handle. Provisional API — the write
 * engine is young; fields may grow, which is exactly why it carries `size`.
 *
 * This is an IN struct on the one path in the library that is not idempotent.
 * Growing it without the guard would not fail loudly — it would have the
 * library read past the end of a shorter caller's struct and burn a disc from
 * whatever was there. The size negotiation is the same one accudisc_read_req
 * uses (see its block comment below for the full IN/OUT rules): a shorter
 * struct is zero-extended, a longer one is accepted only if every byte past
 * this build's end is zero, and a declared size of 0 is always ERR_ABI.
 *
 * Use the macro; do not set the field by hand:
 *
 *     accudisc_write_opts o = ACCUDISC_WRITE_OPTS_INIT;
 *
 * Note that `size` landed in existing padding, so sizeof() did NOT change when
 * the guard was added. That is not a compatibility win — a caller built
 * against the older header passes 24 bytes whose first 4 are `simulate`, which
 * is 0 or 1 and so refused as a size. Old callers fail loudly, which is the
 * intent; they do not silently pass with `simulate` read as a length. */
typedef struct accudisc_write_opts {
    uint32_t size;  /* = sizeof(accudisc_write_opts); ACCUDISC_WRITE_OPTS_INIT */
    int simulate;   /* test-write: run the full path with the laser off */
    int byteswap;   /* swap each 16-bit audio sample before writing */
    int speed;      /* 0 = leave the drive's current write speed */
    /* Optional CD-Text (pass-through). Path to a raw READ TOC format-0x05 blob,
     * byte-for-byte as accudisc_read_cdtext emits it; it is laid into the
     * lead-in verbatim. NULL = burn no CD-Text. Appended field: zero-init
     * callers get NULL and the prior behaviour unchanged. */
    const char *cdtext_path;
    /* BURN-Proof (buffer-underrun-free recording). One of the
     * ACCUDISC_BURNPROOF_* values; appended, so a zero-init caller gets AUTO
     * and the library decides from what the drive claims.
     *
     * Until 0.26.0 this was forced ON unconditionally, which asked every drive
     * for a feature many do not have — and, worse, left the engine with no way
     * to know whether a failover existed. That matters because it decides what
     * happens when the host cannot keep up: with a failover, defer to it; with
     * none, stop, because we own the pipeline and a coaster is the alternative. */
    int burnproof;
    /* Write FIFO capacity in BYTES. Appended in 0.26.0.
     *
     * 0 means USE THE DEFAULT (a few seconds of audio, see
     * ACCUDISC_FIFO_DEFAULT_SECONDS) — NOT "no FIFO", because a zero-init
     * caller should get the protection rather than opt into it. Pass
     * ACCUDISC_FIFO_NONE to run the old synchronous path deliberately.
     *
     * SIZE IT IN TIME, NOT BYTES, and let this field carry the conversion. The
     * quantity that matters is how long a host stall the burn can absorb, and
     * that is bytes / write-rate — so the same byte count is 3.4 s at CD 8x and
     * under 0.2 s at BD speeds. accudisc_fifo_bytes_for() does the conversion
     * with the rate stated rather than assumed.
     *
     * uint32_t, NOT size_t. size_t is 4 bytes on a 32-bit target and 8 on a
     * 64-bit one, so a size_t here would make this struct's layout differ by
     * PLATFORM — and the platforms that would diverge are exactly the small
     * boards and legacy hosts this buffer is most needed on. A FIFO above
     * 4 GiB is not a thing anyone wants. */
    uint32_t fifo_bytes;
} accudisc_write_opts;

/* fifo_bytes sentinel: run without a FIFO, deliberately. Not 0, because 0 is
 * what a zero-init caller passes and such a caller should be PROTECTED by
 * default rather than unprotected by accident. */
#define ACCUDISC_FIFO_NONE ((uint32_t)0xFFFFFFFFu)
/* The read side's twin, and the same reasoning: 0 is what a zero-init caller
 * passes, and such a caller should be protected rather than unprotected. */
#define ACCUDISC_BUFFER_NONE ((uint32_t)0xFFFFFFFFu)
#define ACCUDISC_BUFFER_DEFAULT_SECONDS 3.0

/* Default ride-through. Conservative on purpose: enough to cross a writeback
 * storm or a page-cache stall, small enough to lock on a modest machine. */
#define ACCUDISC_FIFO_DEFAULT_SECONDS 5.0
/* Byte ceiling for the duration form. 5 s at 48x would be ~42 MB of LOCKED
 * memory, which on a small board is a refusal to start rather than a buffer. */
#define ACCUDISC_FIFO_MAX_BYTES (32u * 1024u * 1024u)

/* Bytes for `seconds` of CD audio at `speed_x`, clamped to
 * ACCUDISC_FIFO_MAX_BYTES. Exposed so a caller sizing in time uses the same
 * arithmetic the engine does, rather than a second implementation of one rule.
 *
 * `speed_x` is the WRITE speed you intend to request. It cannot be read back
 * from the drive: mode page 2A reports the speed that was REQUESTED, not the
 * one delivered, and its fields describe reading. Pass what you will ask for,
 * or the drive's maximum for a conservative size. */
ACCUDISC_API uint32_t accudisc_fifo_bytes_for(double seconds,
                                             unsigned speed_x);

/* accudisc_write_opts.burnproof. AUTO is 0 so a zero-init caller gets it. */
#define ACCUDISC_BURNPROOF_AUTO 0 /* enable when the drive claims BUF (002Eh) */
#define ACCUDISC_BURNPROOF_OFF  1 /* never — the caller wants the raw pipeline */
#define ACCUDISC_BURNPROOF_ON   2 /* force, even where the drive does not claim
                                   * it. For a drive whose firmware under-reports
                                   * its own capability; the request may simply
                                   * be refused by MODE SELECT, which is honest
                                   * and is reported rather than swallowed. */

#define ACCUDISC_WRITE_OPTS_INIT { .size = sizeof(accudisc_write_opts) }

/* A POSITIVE accudisc_write() return: the burn COMPLETED, but with a caveat the
 * caller should surface (CLI maps it to exit 3, "completed with caveats"). The
 * disc was written. Distinct from ACCUDISC_OK (clean) and from any negative
 * ACCUDISC_ERR_* (the burn did not complete). Test with `rc > 0`, not
 * `rc != ACCUDISC_OK`. The specific caveat is emitted via the log sink
 * (accudisc_set_log). Today the only caveat is a CD-Text SIZE_INFO pack whose
 * declared track range disagrees with the .toc being burned. */
#define ACCUDISC_WROTE_WITH_CAVEATS 1

/* Burn toc_path (a cdrdao .toc) + bin_path (the raw s16 audio it names).
 * progress (may be NULL) is called with sectors done / total. Returns:
 *   ACCUDISC_OK (0)             clean burn (or clean simulate);
 *   ACCUDISC_WROTE_WITH_CAVEATS the burn completed but see the log (e.g. the
 *                               CD-Text SIZE_INFO disagrees with the .toc);
 *   ACCUDISC_ERR_NOT_BLANK      the disc is not blank — nothing was written
 *                               (was ERR_UNSUPPORTED before 0.4.0; see the
 *                               enum for why the census-exact mapping was not
 *                               good enough);
 *   ACCUDISC_ERR_ABI            opts->size is 0, or declares fields this build
 *                               does not have and cannot honour — nothing was
 *                               written, and nothing was read past the struct;
 *   other negative ACCUDISC_ERR_*  a transport/parse/local error.
 * A negative return means the burn did NOT complete; a non-negative return
 * means it did. */
ACCUDISC_API int accudisc_write(accudisc_device *dev, const char *toc_path,
                                const char *bin_path,
                                const accudisc_write_opts *opts,
                                void (*progress)(void *user, uint32_t done,
                                                 uint32_t total),
                                void *user);

/* Optional log sink for library/driver diagnostics (default: discarded). */
ACCUDISC_API void accudisc_set_log(accudisc_device *dev,
                                   void (*fn)(void *user, const char *msg),
                                   void *user);

/* ---- vendor drivers ---------------------------------------------------------
 * All proprietary/hardware-specific features live in external driver .so
 * files (see accudisc/driver.h); the core library is pure MMC/SG. Calling
 * accudisc_driver_attach IS the application's permission grant — without it
 * a device never issues a vendor opcode. Order: identify -> locate driver
 * (explicit name, or match by drive ID) -> load -> selftest (read/set/
 * re-read real device state) -> attach; any failure leaves the device on
 * generic MMC/SG, fully usable.
 *
 * name: driver to request ("plextor"), or NULL to auto-match the drive.
 * dir:  driver directory, or NULL for $ACCUDISC_DRIVER_DIR, falling back to
 *       the installed default.
 * Returns ACCUDISC_OK (attached), ACCUDISC_ERR_NOTFOUND (no driver file /
 * no match — warn-only situation, device stays usable), or
 * ACCUDISC_ERR_UNSUPPORTED (driver found but selftest failed; not attached).
 * Vendor opcodes need the kernel's full SG_IO command set: open the device
 * with ACCUDISC_OPEN_RDWR or selftest will fail. */
ACCUDISC_API int accudisc_driver_attach(accudisc_device *dev,
                                        const char *name, const char *dir);
ACCUDISC_API void accudisc_driver_detach(accudisc_device *dev);

/* Human-readable access method for logging by the calling application:
 * "generic MMC" or "driver <name> (<description>)". Never NULL. */
ACCUDISC_API const char *accudisc_access_method(accudisc_device *dev);

/* ---- hardware error counters (driver capability) ---------------------------
 * C1/C2/CU error census counters as exposed by vendor firmware (e.g.
 * Plextor Q-Check). ACCUDISC_ERR_UNSUPPORTED without an attached driver
 * offering the capability. read returns the counts accumulated since the
 * previous read and resets the interval. */
typedef struct accudisc_counters {
    uint32_t c1; /* correctable at C1 stage */
    uint32_t c2; /* correctable at C2 stage */
    uint32_t cu; /* uncorrectable */
} accudisc_counters;

ACCUDISC_API int accudisc_counter_scan_begin(accudisc_device *dev);
ACCUDISC_API int accudisc_counter_scan_read(accudisc_device *dev,
                                            accudisc_counters *out);
ACCUDISC_API int accudisc_counter_scan_end(accudisc_device *dev);

/* ---- counter census (the scan built on the three calls above) --------------
 * Read a span while the counters are armed, sampling them every `cadence`
 * sectors. The default cadence of 75 sectors is one second of audio, and that
 * is the whole reason the reported figures are comparable with other C1/C2
 * tools — their units are per second because the sample window is a second.
 * Change it only if you also change how the results are labelled. */
#define ACCUDISC_CENSUS_CADENCE 75

typedef struct accudisc_census_sample {
    uint32_t lba;               /* first sector of this sample */
    uint32_t count;             /* sectors covered (< cadence at the tail) */
    accudisc_counters counters; /* accumulated over those sectors */
    int read_err;               /* the read's result: ACCUDISC_OK, or why not.
                                 * A failed read does NOT stop the census — the
                                 * point is to map damage — but its sample is
                                 * flagged so a caller never mistakes "unread"
                                 * for "read clean". */
} accudisc_census_sample;

/* Called once per sample. Return nonzero to stop the census early (reported as
 * ACCUDISC_ERR_CANCELLED); the counters are always disarmed regardless. */
typedef int (*accudisc_census_fn)(const accudisc_census_sample *sample,
                                  void *user);

typedef struct accudisc_census_opts {
    uint32_t start;    /* first sector */
    uint32_t end;      /* one past the last — normally toc.leadout_lba */
    uint32_t cadence;  /* sectors per sample; 0 = ACCUDISC_CENSUS_CADENCE */
    uint16_t speed_x;  /* set before scanning; 0 = leave as-is */
    const volatile int *cancel; /* poll: nonzero aborts; or NULL */
} accudisc_census_opts;

typedef struct accudisc_census_stats {
    uint64_t c1, c2, cu;                    /* totals over the span */
    uint32_t peak_c1, peak_c2, peak_cu;     /* worst single sample */
    uint32_t samples;                       /* samples delivered */
    uint32_t read_errors;                   /* samples whose read failed */
} accudisc_census_stats;

/* Arms the counters, scans [start, end), disarms — including on every error
 * path, which is the reason this exists as one call rather than three. Returns
 * ACCUDISC_ERR_UNSUPPORTED without an attached driver offering the counters,
 * and in that case nothing was armed. stats may be NULL. */
ACCUDISC_API int accudisc_counter_census(accudisc_device *dev,
                                         const accudisc_census_opts *opts,
                                         accudisc_census_fn fn, void *user,
                                         accudisc_census_stats *stats);

/* ---- read-speed uncap (driver capability) ----------------------------------
 * Firmware caps CD read speed on some media; where the vendor offers an
 * override (Plextor: "SpeedRead"), this toggles it, raising the ceiling
 * reported by accudisc_get_speed (PX-716A: 40x -> 48x). Speed is still
 * commanded through accudisc_set_speed — this only lifts the limit.
 * ACCUDISC_ERR_UNSUPPORTED without an attached driver offering it.
 *
 * The setting is DRIVE state: it persists after the handle is closed, until
 * changed again or the drive is power-cycled. A caller that flips it for one
 * operation should read the prior value first and restore it. */
ACCUDISC_API int accudisc_speed_uncap_get(accudisc_device *dev, int *on);
ACCUDISC_API int accudisc_speed_uncap_set(accudisc_device *dev, int on);

/* Whether the uncap's state is KNOWN. Every value here is now authoritative or
 * absent; there is no hedged one.
 *
 * This enum used to carry a third, inferred value, and the change is worth
 * understanding before adding another. The inference compared page 2A's
 * advertised maximum read speed against a per-model stock ceiling — but page 2A
 * reports the largest request the drive ACCEPTS, not what it delivers. On CD-DA
 * the governor caps the rate regardless of the uncap, so a raised maximum was
 * never evidence about the drive's behaviour. An inference drawn from a
 * quantity that does not answer the question is not a weak answer; it is a
 * different question's answer wearing this one's type.
 *
 * The setting is also only reachable through a vendor driver. With no driver
 * and no set of our own, UNKNOWN is the whole truth, and callers get it. */
typedef enum accudisc_uncap_state {
    ACCUDISC_UNCAP_OFF = 0,   /* authoritative */
    ACCUDISC_UNCAP_ON,        /* authoritative */
    /* 2 IS RETIRED AND MUST NEVER BE REUSED. It was ACCUDISC_UNCAP_LIKELY_ON,
     * an INFERENCE from page 2A's advertised maximum against a per-model stock
     * ceiling, removed in 0.8.0 with the table behind it. Anything compiled
     * before 0.8.0 reads a 2 as "likely on", so a future state assigned 2 would
     * be misreported by that consumer with nothing able to detect it. Every
     * value below is now authoritative or absent — there is no longer a
     * hedged one. */
    ACCUDISC_UNCAP_UNKNOWN = 3 /* nobody who can answer has: no driver attached
                                * and we did not set it through this handle.
                                * NOT "off" — it is the absence of an answer,
                                * and a caller must not read it as one. */
} accudisc_uncap_state;

/* Report the vendor read-speed uncap's state, and the drive's advertised
 * maximum read speed. Two sources, both authoritative:
 *
 *   1. this handle set it (accudisc_speed_uncap_set on this dev)  -> ON / OFF
 *   2. a driver is attached and answers speed_uncap_get           -> ON / OFF
 *   otherwise                                                     -> UNKNOWN
 *
 * A third source existed until 0.8.0: an inference from max_x against a
 * per-model stock ceiling, so that a setting left on by a previous session
 * could be spotted without a driver. It is gone, and UNKNOWN is now the answer
 * in that case. The uncap is reachable only through a vendor driver, and
 * inferring an unreachable setting from a number that reports what the drive
 * ACCEPTS rather than what it delivers produced a well-formed value that was
 * not evidence of anything.
 *
 * WHAT TO USE INSTEAD, because the underlying need was real: nothing about the
 * uncap tells you what a read will achieve. accudisc_probe_speed_ladder TIMES
 * reads at each candidate speed and reports the rate the drive actually
 * delivered, which is the question anyone querying this was really asking.
 *
 * max_x, if non-NULL, receives the drive's reported maximum read speed in Nx
 * (0 when it could not be read). Returns ACCUDISC_OK whenever *state was set —
 * including UNKNOWN, which is an answer, not a failure. */
ACCUDISC_API int accudisc_speed_uncap_probe(accudisc_device *dev,
                                            accudisc_uncap_state *state,
                                            unsigned *max_x);

/* Scoped uncap: set it for one operation and put it back afterwards.
 *
 * The uncap is persistent drive state, so an operation that flips it and exits
 * has reconfigured the user's drive. push/pop is the discipline: pop only what
 * you pushed, and restore to the PRIOR value rather than to a factory default —
 * the caller does not know what the drive was set to before it arrived.
 *
 * push reads the prior value FIRST and does not attempt the set if that read
 * fails: changing persistent state you have no way to undo is worse than not
 * changing it. *prior_out receives -1 when nothing needs restoring, which pop
 * treats as a no-op, so the caller can pass it to pop unconditionally on every
 * exit path — including error paths — without tracking whether it got that far.
 *
 * On failure, *prior_out also says which half failed: still -1 means the prior
 * could not be read; >= 0 means it was read and the set failed. The prior is
 * recorded before the set is attempted, deliberately, because a failed set may
 * have partially applied — an unnecessary restore is harmless, a skipped one is
 * not.
 *
 * Returns ACCUDISC_ERR_UNSUPPORTED without an attached driver offering it. */
ACCUDISC_API int accudisc_speed_uncap_push(accudisc_device *dev, int on,
                                           int *prior_out);
ACCUDISC_API int accudisc_speed_uncap_pop(accudisc_device *dev, int prior);

/* Best-effort drive read-speed control, in Nx CD speed (176 kB/s units).
 * Prefers SET STREAMING (0xB6, a ceiling the drive enforces; needs
 * CAP_SYS_RAWIO), falling back to the unprivileged CDROM_SELECT_SPEED path. */
ACCUDISC_API int accudisc_set_speed(accudisc_device *dev, unsigned speed_x);

/* Read-speed ceiling scoped to an LBA range and/or pinned exact. This is SET
 * STREAMING only (0xB6) — SET CD SPEED cannot express a range or Exact — so
 * there is NO block-layer fallback: a drive/handle that cannot honour it
 * returns an error rather than silently applying a whole-disc speed. flags:
 * ACCUDISC_SPEED_EXACT pins the rate (forces CLV; a CAV-only drive may refuse
 * with Illegal Request, which is itself the answer). */
#define ACCUDISC_SPEED_EXACT 0x01u
ACCUDISC_API int accudisc_set_speed_range(accudisc_device *dev, unsigned speed_x,
                                          int32_t start_lba, int32_t end_lba,
                                          unsigned flags);

/* Mode page 2A max/current read speed in kB/s (divide by 176 for Nx). */
ACCUDISC_API int accudisc_get_speed(accudisc_device *dev,
                                    unsigned *max_kbps, unsigned *cur_kbps);

/* ---- drive rotation / nominal performance curve (GET PERFORMANCE 0xAC) -----
 * The read-speed curve the drive reports for the loaded medium, as a list of
 * {start_lba, start_kbps, end_lba, end_kbps} segments, and a classification of
 * its shape. Pure MMC and disc-independent on the drives tested (the curve is
 * RPM-derived); a drive that rejects the command yields count 0, which
 * classifies as ACCUDISC_ROTATION_UNKNOWN — the shape is never inferred. */
typedef enum {
    ACCUDISC_ROTATION_UNKNOWN = 0, /* command rejected / no descriptors */
    ACCUDISC_ROTATION_CLV,         /* constant linear velocity: one flat level */
    ACCUDISC_ROTATION_CAV,         /* constant angular velocity: rising rate */
    ACCUDISC_ROTATION_PCAV,        /* partial CAV: rises then caps flat */
    ACCUDISC_ROTATION_ZCLV,        /* zoned CLV: stepped flat levels */
} accudisc_rotation;

typedef struct accudisc_perf_desc {
    uint32_t start_lba;
    uint32_t start_kbps;
    uint32_t end_lba;
    uint32_t end_kbps;
} accudisc_perf_desc;

/* Fetch the nominal-performance curve. Writes up to max_out descriptors into
 * out and sets *count to the number returned (0 if the drive rejects GET
 * PERFORMANCE). Returns the command status; a rejection is not fatal — treat
 * it as "curve unknown". */
ACCUDISC_API int accudisc_get_performance(accudisc_device *dev,
                                          accudisc_perf_desc *out,
                                          uint32_t max_out, uint32_t *count);

/* Classify a performance curve's rotation strategy. Pure function over
 * drive-supplied descriptors (count 0 => UNKNOWN); no hardware access. */
ACCUDISC_API accudisc_rotation
accudisc_classify_rotation(const accudisc_perf_desc *desc, uint32_t count);

/* Spin the spindle down without ejecting (START STOP UNIT, straight to the
 * drive rather than through block-layer quirks). */
ACCUDISC_API int accudisc_spindle_stop(accudisc_device *dev);

/* Open the tray / unload the disc (START STOP UNIT, LoEj=1 Start=0). Straight
 * to the drive, so it works without a mounted block device. */
ACCUDISC_API int accudisc_eject(accudisc_device *dev);

/* Close the tray / load the disc (START STOP UNIT, LoEj=1 Start=1). A slot
 * loader with no disc may reject this; the drive's sense is returned. */
ACCUDISC_API int accudisc_load(accudisc_device *dev);

/* ---- TOC ------------------------------------------------------------------ */

typedef struct accudisc_track {
    uint8_t number;    /* 1..99 */
    uint8_t adr_ctrl;  /* raw ADR (high nibble) / CTRL (low nibble) */
    uint8_t session;   /* 1..99; 0 = unknown (format-0 degrade, see below) */
    uint32_t lba;      /* INDEX 01 — where the track's USER DATA begins. This is
                        * what the lead-in describes and it never moves. */
    uint32_t sectors;  /* from lba, to the next track in the SAME session; the
                        * session's last track runs to that session's lead-out —
                        * never across a session boundary (see accudisc_session) */
    uint32_t pregap;   /* sectors immediately BEFORE lba that belong to this
                        * track, so its full extent is
                        *     [lba - pregap, lba + sectors)
                        *
                        * ECMA-130 §20 is explicit that a Pause is "a part of an
                        * Information Track", not a gap between tracks: the
                        * sectors before INDEX 01 belong to the track that
                        * follows them. Building extents from INDEX 01 alone
                        * therefore under-attributes, leaving real audio owned
                        * by nobody.
                        *
                        * Only ONE pregap is derivable from the TOC, and this is
                        * the whole of it: the program area begins at LBA 0, so
                        * if the first track's INDEX 01 is at LBA n > 0, those n
                        * sectors are its pregap. Every other track's pregap
                        * lives in the subchannel (INDEX 00), which the lead-in
                        * does not carry, so it takes a program-area scan —
                        * accudisc_index_map_decode over a raw subchannel read.
                        * (This used to cite a `pregaps` token in
                        * accudisc_toc_info; no such field exists or ever did.)
                        * So this is non-zero only for the first track of the
                        * first session, and 0 elsewhere.
                        *
                        * It matters archivally: a rip that starts at INDEX 01
                        * silently drops those sectors, shifting every LBA
                        * against the audio stream and producing a wrong disc
                        * ID. Hidden-track-one audio lives here too. */
} accudisc_track;

#define ACCUDISC_TRACK_IS_AUDIO(t) (((t)->adr_ctrl & 0x04) == 0)

/* ---- sessions --------------------------------------------------------------
 * A multi-session disc is not one contiguous program area. Between one
 * session's last track and the next session's first track sit that session's
 * LEAD-OUT, the next session's LEAD-IN, and the next track's pregap — on a
 * typical Enhanced CD roughly 11,400 sectors that hold no track payload and
 * cannot be read as CD-DA. Measured on a PX-716A, 2026-07-22: an Enhanced CD
 * whose session 1 ends at track 13 (LBA 184300) reports session 1 lead-out
 * 195656, while session 2's track 14 starts at 207056.
 *
 * Track extents are therefore bounded by the OWNING SESSION's lead-out, not by
 * the next track start on the disc. A caller that walks "track n start to
 * track n+1 start" across that seam drives 11,400 sectors into unreadable
 * territory; that is a real defect this model exists to prevent.
 *
 * Only READ TOC format 2 carries session structure. On the format-0 degrade
 * path (accudisc_read_toc_src) the drive returns a flat track list with no
 * session tags and only the LAST session's lead-out, so session_count is 0 and
 * every track's session is 0 — "unknown", not "one". See
 * accudisc_check_audio_range(), which refuses rather than guesses. */

typedef struct accudisc_session {
    uint8_t number;       /* 1..99 */
    uint8_t first_track;  /* from this session's A0 point, else observed */
    uint8_t last_track;   /* from this session's A1 point, else observed */
    uint8_t audio_tracks; /* census over the tracks this session owns */
    uint8_t data_tracks;
    uint32_t leadout_lba; /* this session's A2 point */
} accudisc_session;

typedef struct accudisc_toc {
    uint8_t first_track;
    uint8_t last_track;
    uint8_t track_count;
    uint32_t leadout_lba; /* the LAST session's lead-out = end of the disc */
    accudisc_track tracks[99];
    /* Session table, ascending by number: the sessions we can MAP, meaning we
     * know which tracks they own and where they end. 0 means the source could
     * not report session structure — NOT that the disc has none. */
    uint8_t session_count;
    accudisc_session sessions[99];
    /* How many sessions the disc HAS, from whichever source could say (the
     * lead-in, or READ DISC INFORMATION, which answers from the drive's disc
     * model and so survives an unreadable lead-in). 0 = nobody could say.
     *
     * Deliberately distinct from session_count: sessions_total > session_count
     * is the honest description of a degraded read of a multi-session disc —
     * we know the seams exist but not where they fall, which is strictly more
     * dangerous than knowing nothing, and must not be silently flattened. */
    uint8_t sessions_total;

    /* Structural anomalies found while parsing the lead-in — a bitmask of
     * ACCUDISC_TOC_ANOM_*. Zero on every well-formed disc.
     *
     * These exist because copy-protection schemes work by DELIBERATELY
     * malforming the TOC (Kaspersky, "CD Cracking Uncovered", ch. 6-7: bogus
     * track addresses, tracks placed in the lead-out, lead-out pointers aimed
     * back into the program area). Such a disc must fail INFORMATIVELY. The
     * failure mode to avoid is not a crash — it is silently "helpfully"
     * normalising a malformed geometry into a plausible-looking one, because
     * the audio-range guard then vets a map that does not describe the disc. */
    uint16_t anomalies;
} accudisc_toc;

/* Bit values for accudisc_toc.anomalies.
 *
 * Split by CONSEQUENCE, not by cause: the _UNTRUSTED_GEOMETRY group means the
 * track map cannot be relied on to say which sectors are audio, so
 * accudisc_check_audio_range() refuses outright. The rest are reported and
 * otherwise harmless — the map still describes the disc. */
typedef enum {
    /* --- geometry cannot be trusted (guard refuses) --------------------- */

    /* Track numbers ascend but their addresses do not. Kaspersky ch. 6,
     * "Incorrect Starting Address for the Track". Extents are computed in
     * ADDRESS order so the map stays correct, but a TOC that lies about this
     * is malformed by intent and nothing else it says is trustworthy. */
    ACCUDISC_TOC_ANOM_LBA_ORDER       = 1u << 0,
    /* Two tracks claim the same sector. Kaspersky ch. 6, "Fictitious Track
     * Coinciding with the Genuine Track" / "Audio Overlapped by Data". No
     * ordering makes this consistent, so it is never guessed at. */
    ACCUDISC_TOC_ANOM_OVERLAP         = 1u << 1,
    /* The lead-out points at or before a track start — Kaspersky ch. 7,
     * "Castrated Lead-Out". Every extent derived from it is suspect. */
    ACCUDISC_TOC_ANOM_LEADOUT_BEFORE  = 1u << 2,

    /* --- reported only (map still describes the disc) -------------------- */

    /* A track starts at or beyond the lead-out: Kaspersky ch. 6, "Fictitious
     * Track in the Lead-Out Area". It gets a zero extent and owns no sector. */
    ACCUDISC_TOC_ANOM_PAST_LEADOUT    = 1u << 3,
    /* A track with a zero-length extent. Red Book's minimum is 4 s (300
     * sectors), so this is always malformed; it owns no sector either way. */
    ACCUDISC_TOC_ANOM_EMPTY_TRACK     = 1u << 4,
    /* A track point addressed before LBA 0 — Kaspersky ch. 7, "Negative
     * Starting Address of the First Audio Track". The point is dropped. */
    ACCUDISC_TOC_ANOM_NEGATIVE_LBA    = 1u << 5,
    /* An A0/A1 point naming a first/last track outside 1..99. Reported, and
     * the observed track list is used instead. */
    ACCUDISC_TOC_ANOM_BAD_TRACK_NUM   = 1u << 6,
    /* A0/A1 disagree with the tracks actually present — Kaspersky ch. 6,
     * "Invalidating Track Numbering" and the gap/duplicate family. */
    ACCUDISC_TOC_ANOM_RANGE_MISMATCH  = 1u << 7,
    /* An entry claiming a session number outside 1..99. Dropped. */
    ACCUDISC_TOC_ANOM_BAD_SESSION     = 1u << 8
} accudisc_toc_anomaly;

/* The anomalies that make the track map untrustworthy. */
#define ACCUDISC_TOC_ANOM_UNTRUSTED_GEOMETRY                                   \
    (ACCUDISC_TOC_ANOM_LBA_ORDER | ACCUDISC_TOC_ANOM_OVERLAP |                 \
     ACCUDISC_TOC_ANOM_LEADOUT_BEFORE)

/* Short stable slug for one anomaly BIT (not a mask), e.g. "lba_order".
 * Returns "unknown" for an unrecognised or composite value. */
ACCUDISC_API const char *accudisc_toc_anomaly_str(unsigned bit);

/* READ TOC format 0, parsed. Requires a disc. */
ACCUDISC_API int accudisc_read_toc(accudisc_device *dev, accudisc_toc *out);

/* Raw full TOC (READ TOC format 2: session structure, undecoded) — *out is
 * library-allocated (accudisc_free), *len includes the 2-byte length field. */
ACCUDISC_API int accudisc_read_full_toc(accudisc_device *dev,
                                        uint8_t **out, uint32_t *len);

/* ---- TOC acquisition path --------------------------------------------------
 * READ TOC format 2 ("full TOC") and format 0 ("TOC") are different physical
 * operations, not two views of one thing: format 2 replays the raw Q-channel
 * of the LEAD-IN, while format 0 returns the drive's already-decoded track
 * descriptors. A marginal lead-in can therefore fail format 2 outright while
 * format 0 still answers perfectly — observed on a PX-716A with an MPO CD-R,
 * 2026-07-21, where the program area read clean.
 *
 * accudisc_read_toc_src() prefers format 2 and degrades to format 0, reporting
 * which path answered and why it degraded. The degrade is a DISC-HEALTH signal,
 * not plumbing: a lead-in that has become unreadable while the program area is
 * still perfect predicts what fails next, so it is surfaced rather than hidden.
 *
 * What format 2 adds over format 0 is SESSION STRUCTURE (session numbering,
 * disc type, multi-session pointers) — NOT pregaps. INDEX 00 exists only in the
 * program-area Q subchannel, never in the lead-in, so pregap data requires a
 * program-area read (accudisc_index_map_decode) regardless of which path
 * answered here. Callers wanting pregaps must ask for them separately; a
 * successful format 2 does not supply them. */

typedef enum {
    ACCUDISC_TOC_SRC_FULLTOC = 0, /* READ TOC format 2: + session structure */
    ACCUDISC_TOC_SRC_TOC     = 1  /* READ TOC format 0: boundaries + lead-out */
} accudisc_toc_source;

typedef enum {
    ACCUDISC_TOC_DEGRADE_NONE = 0,       /* format 2 answered; no degrade */
    ACCUDISC_TOC_DEGRADE_LEADIN_UNREADABLE = 1, /* format 2 failed (transport
                                          * or CHECK CONDITION) — the lead-in
                                          * could not be read */
    ACCUDISC_TOC_DEGRADE_LEADIN_ABSENT = 2,     /* format 2 answered "no data"
                                          * (header only) — nothing there */
    ACCUDISC_TOC_DEGRADE_LEADIN_MALFORMED = 3   /* format 2 answered but the
                                          * response did not parse into a
                                          * usable TOC */
} accudisc_toc_degrade;

typedef struct accudisc_toc_info {
    uint8_t source;        /* accudisc_toc_source */
    uint8_t degrade;       /* accudisc_toc_degrade */
    int32_t degrade_err;   /* the ACCUDISC_ERR_* that forced the degrade, or 0.
                            * ERR_IO/ERR_SENSE distinguish a transport failure
                            * from a drive rejection. */
    uint8_t first_session; /* valid when source == FULLTOC, else 0 */
    uint8_t last_session;
    /* Session COUNT from READ DISC INFORMATION — a different opcode from
     * READ TOC, answered from the drive's own disc model rather than by
     * re-reading the lead-in. It is therefore available on the DEGRADE path,
     * where first_session/last_session are not: a flat format-0 track list
     * cannot reveal how many sessions produced it. 0 = unobtainable.
     *
     * Distinct from the fulltoc range above: this is "how many", not "which".
     * A caller whose policy is session-1-only should refuse a degrade with
     * session_count > 1 rather than infer structure from a track census that
     * cannot see it. */
    uint8_t session_count;
    uint8_t disc_type;     /* A0 psec: 0x00 CD-DA/CD-ROM, 0x10 CD-i,
                            * 0x20 CD-ROM XA. Valid when source == FULLTOC. */
} accudisc_toc_info;

/* TOC with the acquisition path reported. Prefers format 2, degrades to
 * format 0. info may be NULL. Returns ACCUDISC_OK if EITHER path produced a
 * usable TOC; only a failure of BOTH is an error (and then the format-0 error
 * is returned, since that is the one that left the caller with nothing). */
ACCUDISC_API int accudisc_read_toc_src(accudisc_device *dev, accudisc_toc *out,
                                       accudisc_toc_info *info);

/* Stable lowercase tokens for the machine interface ("fulltoc"/"toc";
 * "none"/"leadin_unreadable"/"leadin_absent"/"leadin_malformed"). Never NULL. */
ACCUDISC_API const char *accudisc_toc_source_str(unsigned source);
ACCUDISC_API const char *accudisc_toc_degrade_str(unsigned degrade);

/* ---- session selection and the audio-range guard ---------------------------
 * Two pure functions over a parsed TOC. No hardware access, no hidden reads:
 * the library still only moves the bits the caller asks for, but it will no
 * longer let the caller ask for bits that cannot exist.
 *
 * Policy, as specified: when exactly ONE session contains audio tracks, that
 * session is the default and needs no argument. When more than one does, there
 * is no defensible default and the caller must name the session it wants —
 * iterating over sessions is the calling application's business, not ours. */

/* Resolve the session to rip. Returns the session number (>= 1), or:
 *   ACCUDISC_ERR_NOTFOUND    no session contains an audio track
 *   ACCUDISC_ERR_INVAL       toc is NULL, or session structure is unknown
 *                            (session_count == 0 — the format-0 degrade path)
 *   ACCUDISC_ERR_UNSUPPORTED more than one session contains audio: ambiguous
 *                            by construction, so the caller must choose. */
ACCUDISC_API int accudisc_toc_default_audio_session(const accudisc_toc *toc);

/* Sector range of one session's tracks: first track's start through that
 * session's lead-out. Excludes the lead-out itself. ACCUDISC_ERR_NOTFOUND if
 * the session is not in the table. */
ACCUDISC_API int accudisc_toc_session_range(const accudisc_toc *toc,
                                            uint8_t session, uint32_t *lba,
                                            uint32_t *count);

/* Sector range of one session's AUDIO tracks: the first audio track's start
 * through the end of the last audio track in that session.
 *
 * Session-level selection is too coarse for a Mixed Mode CD, where one session
 * holds a data track (first, where a filesystem is expected) followed by audio
 * tracks. accudisc_toc_session_range() returns the whole session there, which
 * the range guard then correctly refuses. This narrows to the audio.
 *
 *   ACCUDISC_ERR_NOTFOUND    session absent, or it holds no audio tracks
 *   ACCUDISC_ERR_UNSUPPORTED the session's audio tracks are NOT contiguous — a
 *                            data track sits between them, so no single range
 *                            can express the audio and the caller must select
 *                            tracks explicitly. Not known to occur in any
 *                            shipped format, but legal on the wire. */
ACCUDISC_API int accudisc_toc_session_audio_range(const accudisc_toc *toc,
                                                  uint8_t session,
                                                  uint32_t *lba,
                                                  uint32_t *count);

/* Sector range spanning tracks first..last inclusive (track NUMBERS, not
 * indices). Both must exist and lie in the same session, and last >= first.
 *
 *   ACCUDISC_ERR_NOTFOUND    either track number is not on the disc
 *   ACCUDISC_ERR_INVAL       last < first, or bad arguments
 *   ACCUDISC_ERR_UNSUPPORTED the two tracks are in different sessions
 *
 * Track type is NOT checked here — that stays with accudisc_check_audio_range()
 * so there is exactly one place that decides what is rippable. */
ACCUDISC_API int accudisc_toc_track_range(const accudisc_toc *toc,
                                          uint8_t first, uint8_t last,
                                          uint32_t *lba, uint32_t *count);

typedef enum {
    ACCUDISC_RANGE_OK = 0,
    ACCUDISC_RANGE_DATA_TRACK,    /* overlaps a track whose CTRL says data —
                                   * unreadable as CD-DA; the drive rejects
                                   * every sector of it */
    ACCUDISC_RANGE_NOT_IN_TRACK,  /* overlaps sectors owned by no track: a
                                   * session's lead-out, the next lead-in, or
                                   * the gap between sessions */
    ACCUDISC_RANGE_CROSSES_SESSION, /* spans two sessions — legal sectors on
                                   * both sides, a wasteland between */
    ACCUDISC_RANGE_BEYOND_LEADOUT,
    ACCUDISC_RANGE_NO_SESSION_INFO, /* session structure unknown AND the disc
                                   * carries a data track, so the geometry
                                   * cannot be trusted. Refuse, do not guess. */
    ACCUDISC_RANGE_SESSION_UNMAPPED, /* the disc is KNOWN to have more than one
                                   * session, but the degraded lead-in did not
                                   * say which tracks belong to which. Format 0
                                   * hands back the LAST session's lead-out, so
                                   * the final track's extent is wrong and the
                                   * seams are invisible. */
    ACCUDISC_RANGE_EMPTY,         /* count == 0 */
    ACCUDISC_RANGE_TOC_UNTRUSTED  /* the lead-in is malformed in a way that
                                   * makes the track map unreliable — see
                                   * ACCUDISC_TOC_ANOM_UNTRUSTED_GEOMETRY.
                                   * Typically a copy-protection scheme. The
                                   * map may claim a span is audio when it is
                                   * not, so it is not vetted, it is refused. */
} accudisc_range_reason;

typedef struct accudisc_range_check {
    uint8_t ok;             /* 1 = every sector is audio payload, one session */
    uint8_t reason;         /* accudisc_range_reason */
    uint8_t session;        /* the session the range starts in; 0 if unknown */
    uint8_t track;          /* the offending track number, 0 if not a track */
    uint32_t first_bad_lba; /* first sector that is not readable audio */
} accudisc_range_check;

/* Verify that [lba, lba+count) is entirely audio payload within one session.
 * Returns ACCUDISC_OK when it is, ACCUDISC_ERR_INVAL on bad arguments, and
 * ACCUDISC_ERR_UNSUPPORTED when the range is not rippable — *out always
 * carries the reason and the first offending sector either way. Pure. */
ACCUDISC_API int accudisc_check_audio_range(const accudisc_toc *toc,
                                            uint32_t lba, uint32_t count,
                                            accudisc_range_check *out);

/* Stable lowercase token for the machine interface. Never NULL. */
ACCUDISC_API const char *accudisc_range_reason_str(unsigned reason);

/* ---- read-range planning ---------------------------------------------------
 * What to read, decided from the TOC alone. The primitives above are all
 * public already; what was not was the ORDERING and the DEFAULTS — which lived
 * in the CLI, so a binding author had to reconstruct them from the man page.
 *
 * Pure: TOC in, plan out, no device. That is the point. Every branch here
 * (HTOA pregap, Mixed Mode split, multi-session ambiguity, degraded lead-in)
 * used to be reachable only with the right physical disc in the drive.
 *
 * Precedence: TRACKS beat SESSION. Naming tracks already names an extent, and
 * a session is a coarser way of saying the same thing, so there is nothing for
 * the session to add and it must not overwrite what the tracks decided. */

/* Why a plan was refused. DISTINCT from accudisc_range_reason above, which
 * describes why a *sector range* is unreadable — these say why no range could
 * be chosen at all. Two enums, disjoint meanings, overlapping small integers:
 * passing one to the other's _str() yields a well-formed token that is simply
 * wrong, so they are deliberately named apart, down to the struct field. */
typedef enum {
    ACCUDISC_PLAN_OK = 0,
    ACCUDISC_PLAN_TRACKS_NOT_FOUND,        /* a named track is not on the disc */
    ACCUDISC_PLAN_TRACKS_CROSS_SESSION,    /* first and last in different
                                            * sessions: the span would include
                                            * the lead-out and lead-in between */
    ACCUDISC_PLAN_TRACKS_NO_EXTENT,        /* last < first, or the pair yields
                                            * no usable extent */
    ACCUDISC_PLAN_MULTIPLE_AUDIO_SESSIONS, /* no defensible default; the caller
                                            * must name one. Enumerating them
                                            * is the application's business */
    ACCUDISC_PLAN_NO_AUDIO_SESSION,        /* every track is marked data. May be
                                            * a lie — SunnComm MediaCloQ presents
                                            * audio as data to a computer drive,
                                            * which is the whole protection — so
                                            * a caller SHOULD say so and offer
                                            * the force override */
    ACCUDISC_PLAN_SESSION_SPLIT_BY_DATA,   /* the session's audio tracks sit
                                            * either side of a data track; no
                                            * single range covers them */
    ACCUDISC_PLAN_SESSION_NOT_FOUND,       /* session absent, or holds no audio */
    ACCUDISC_PLAN_START_PAST_LEADOUT,
    ACCUDISC_PLAN_EMPTY_RANGE,             /* count <= 0; see resolved_count,
                                            * which is where a NEGATIVE count
                                            * (start beyond the named extent)
                                            * stays visible */
    ACCUDISC_PLAN_GUARD_REFUSED,           /* a range resolved, but is not
                                            * readable as CD-DA. .check carries
                                            * the detail; .lba/.count carry what
                                            * was refused */
    ACCUDISC_PLAN_BAD_ARGUMENT             /* NULL, or out of representable range */
} accudisc_plan_reason;

typedef struct accudisc_range_spec {
    int32_t session;     /* -1 = unspecified */
    int32_t first_track; /* -1 or 0 = unspecified */
    int32_t last_track;
    int64_t start;       /* -1 = unspecified */
    int64_t count;       /* -1 = unspecified (through the end) */
    uint8_t force;       /* skip the audio-range guard; does NOT skip
                          * resolution — the two are separate questions */
} accudisc_range_spec;

typedef struct accudisc_range_plan {
    uint32_t lba;
    uint32_t count;
    /* The count before it was known to be valid. Equals .count on success and
     * is the ONLY field carrying information when the reason is EMPTY_RANGE:
     * a negative value there says the start lies beyond the extent that was
     * named, which an unsigned field would render as a huge positive number. */
    int64_t resolved_count;
    uint8_t session;     /* session actually chosen; 0 = flat / no structure */
    uint8_t plan_reason; /* accudisc_plan_reason — NOT .check.reason */
    accudisc_range_check check; /* populated when the guard refused */
} accudisc_range_plan;

/* Resolve a read request against a TOC. Returns ACCUDISC_OK with *out
 * describing the range, ACCUDISC_ERR_INVAL for NULL or unrepresentable
 * arguments, and ACCUDISC_ERR_UNSUPPORTED for every refusal — the detail is in
 * out->plan_reason, never in the return code, so that a caller can distinguish
 * "name a session" from "this disc claims to have no audio" and say something
 * useful about each. *out is always written. Pure. */
ACCUDISC_API int accudisc_plan_read_range(const accudisc_toc *toc,
                                          const accudisc_range_spec *spec,
                                          accudisc_range_plan *out);

/* Stable lowercase token for the machine interface. Never NULL. */
ACCUDISC_API const char *accudisc_plan_reason_str(unsigned plan_reason);

/* Raw CD-Text packs from the lead-in (READ TOC format 5, undecoded).
 * Returns ACCUDISC_ERR_NOTFOUND when the drive answers but the disc carries
 * no CD-Text; a drive that rejects format 5 outright still surfaces as
 * ACCUDISC_ERR_SENSE (deliberately not conflated with "absent"). */
ACCUDISC_API int accudisc_read_cdtext(accudisc_device *dev,
                                      uint8_t **out, uint32_t *len);

/* ---- disc-kind guard -------------------------------------------------------
 * Pre-flight answer to "which operation is legal for the disc in the drive",
 * so nothing attempts the impossible. Composes three commands the library
 * already issues — GET CONFIGURATION (current profile), READ DISC INFORMATION
 * (status + erasable) and READ TOC (audio/data track census). No new opcodes,
 * no filesystem inspection: this deliberately does not distinguish CD-ROM
 * layouts, DVD or BD beyond "not a CD-DA we can rip, not a blank we can burn".
 *
 * Non-destructive: every command is a read. */

typedef enum {
    ACCUDISC_DISC_NEITHER = 0, /* refuse — neither path is legal */
    ACCUDISC_DISC_BLANK   = 1, /* recordable, no sessions -> the BURN path */
    ACCUDISC_DISC_AUDIO   = 2  /* has audio tracks -> the RIP path */
} accudisc_disc_kind;

/* Why that verdict. Stable lowercase slugs on the machine line. */
typedef enum {
    ACCUDISC_DISC_WHY_AUDIO = 0,       /* >= 1 audio track */
    ACCUDISC_DISC_WHY_BLANK,           /* CD-R/RW, disc status empty */
    ACCUDISC_DISC_WHY_DATA_CD,         /* CD, tracks present, none audio */
    ACCUDISC_DISC_WHY_CLOSED_DATA,     /* as above and the disc is closed */
    ACCUDISC_DISC_WHY_APPENDABLE,      /* open session, nothing rippable yet */
    ACCUDISC_DISC_WHY_NO_MEDIUM,       /* no disc loaded (see tray) */
    ACCUDISC_DISC_WHY_NOT_CD_PROFILE,  /* DVD/BD/unknown — not a CD at all */
    ACCUDISC_DISC_WHY_UNREADABLE       /* a CD, but nothing could be read */
} accudisc_disc_reason;

/* Only meaningful when reason == NO_MEDIUM; from sense ASC 0x3A qualifiers. */
typedef enum {
    ACCUDISC_TRAY_UNKNOWN = 0,
    ACCUDISC_TRAY_CLOSED  = 1, /* tray shut, no disc */
    ACCUDISC_TRAY_OPEN    = 2
} accudisc_tray_state;

#define ACCUDISC_DISC_STATUS_UNKNOWN 0xff /* also used for erasable */

typedef struct accudisc_disc_probe {
    uint16_t profile;      /* GET CONFIGURATION current profile: 0x08 CD-ROM,
                            * 0x09 CD-R, 0x0A CD-RW, 0 = none/unrecognised */
    uint8_t erasable;      /* 1 = CD-RW, 0 = CD-R/pressed,
                            * ACCUDISC_DISC_STATUS_UNKNOWN if not obtained */
    uint8_t disc_status;   /* 0 empty, 1 incomplete (open), 2 complete (closed),
                            * ACCUDISC_DISC_STATUS_UNKNOWN if not obtained */
    uint8_t audio_tracks;  /* CTRL bit 2 clear */
    uint8_t data_tracks;   /* CTRL bit 2 set */
    uint8_t kind;          /* accudisc_disc_kind */
    uint8_t reason;        /* accudisc_disc_reason */
    uint8_t tray;          /* accudisc_tray_state */
} accudisc_disc_probe;

/* Classify the loaded disc. Returns ACCUDISC_OK whenever a verdict was
 * reached — including ACCUDISC_DISC_NEITHER, which is a successful
 * classification, not an error. Only a failure to talk to the drive at all
 * returns an error code. */
ACCUDISC_API int accudisc_probe_disc(accudisc_device *dev,
                                     accudisc_disc_probe *out);

/* Stable machine tokens ("BLANK"/"AUDIO"/"NEITHER"; "audio"/"blank"/"data_cd"/
 * "closed_data"/"appendable"/"no_medium"/"not_cd_profile"/"unreadable";
 * "unknown"/"closed"/"open"). Never NULL. */
ACCUDISC_API const char *accudisc_disc_kind_str(unsigned kind);
ACCUDISC_API const char *accudisc_disc_reason_str(unsigned reason);
ACCUDISC_API const char *accudisc_tray_state_str(unsigned tray);

/* ---- feature probe ---------------------------------------------------------
 * What the drive CLAIMS (GET CONFIGURATION, CD Read feature 0x1E) versus what
 * it DOES (functional smoke reads at LBA 0, so a disc must be loaded).
 * Drives are known to advertise C2 they don't honour and vice versa; the
 * verdict is conservative: only "claimed AND functional" earns SUPPORTED. */

typedef enum accudisc_c2_verdict {
    ACCUDISC_C2_UNSUPPORTED = 0, /* C2 read fails outright */
    ACCUDISC_C2_SUPPORTED   = 1, /* advertised and functional */
    ACCUDISC_C2_UNVERIFIED  = 2  /* reads succeed but not advertised — don't trust */
} accudisc_c2_verdict;

typedef struct accudisc_features {
    uint8_t feature_present; /* CD Read feature descriptor returned */
    uint8_t current;         /* feature active for the loaded medium */
    uint8_t dap;             /* claims DAP (digital audio play) */
    uint8_t c2_claimed;      /* claims C2 error pointers */
    uint8_t cdtext_claimed;  /* claims CD-Text */
    /* functional smoke reads (1 = data returned): */
    uint8_t ok_c2;
    uint8_t ok_sub_raw;
    uint8_t ok_sub_q;
    uint8_t ok_c2_sub_raw;
    uint8_t ok_c2_sub_q;
    uint8_t c2_verdict;      /* accudisc_c2_verdict */

    /* ---- WRITE capability, from CD Mastering (002Eh) — appended in 0.26.0.
     *
     * A CLAIM, NOT A VERIFIED CAPABILITY, and the distinction is load-bearing
     * here in a way it is not for the fields above. Every other flag in this
     * struct is cross-checked against a functional read, because drives
     * advertise C2 they do not honour. BUF cannot be checked that way: proving
     * buffer-underrun-free recording works means deliberately starving a real
     * burn and inspecting the disc, which costs a blank per drive and destroys
     * it. So this is acted on and never verified, and a consumer must report it
     * as claimed rather than as fact.
     *
     * mastering_current is a SEPARATE question from buf_claimed. Current means
     * "active for the loaded medium", so a drive that supports this reports
     * Current=0 with a finished disc and Current=1 with a blank; treating the
     * two as one question refuses BURN-Proof on every burn. */
    uint8_t mastering_present;   /* the 002Eh descriptor came back */
    uint8_t mastering_current;   /* active for the LOADED medium */
    uint8_t buf_claimed;         /* BUF: zero-loss linking (BURN-Proof) */
    uint8_t sao_claimed;         /* SAO: Session-At-Once, our write type */
    uint8_t test_write_claimed;  /* the drive can --simulate */
} accudisc_features;

ACCUDISC_API int accudisc_probe_features(accudisc_device *dev,
                                         accudisc_features *out);

/* Accurate Stream probe: does this drive read audio positionally
 * deterministically? Reads a span, then re-reads it from several different
 * starting LBAs (cache-defeated) and demands the overlapping sectors match
 * byte-for-byte. Probe a CLEAN disc area (damage reads as jitter).
 * *accurate = 1: positioning slips are largely prevented by the drive and
 * boundary overlap checking is near-redundant; 0: the drive can slip —
 * overlap checking is the only defence against the error class C2 is
 * structurally blind to. Factual drive capability: record it alongside the
 * read offset and C2 verdict. */
ACCUDISC_API int accudisc_probe_accurate_stream(accudisc_device *dev,
                                                uint32_t lba,
                                                uint8_t *accurate);

/* C2/audio alignment probe. Some drives return the C2 bitmap misaligned
 * with the audio bytes of the same sector by a small, constant, per-drive
 * amount (e.g. 2 sample pairs on the Plextor PX-716A). Anything consuming
 * fired bits as byte-exact damage positions (erasure feeds for parity
 * repair) must correct for it — misplaced erasures actively harm decoding.
 *
 * Sign convention: a fired bit at bitmap position i describes audio byte
 * i - 4*lag_pairs. Positive lag = the bitmap trails the audio.
 *
 * Method (no external reference needed): fired flags mark bytes the CIRC
 * decoder failed on, and failed bytes are unstable across cache-defeated
 * rereads — so flag positions are cross-correlated against reread
 * instability over candidate shifts; the agreement peak is the lag. The
 * probe scans [lba, lba+count) for C2-active sectors and rereads those, so
 * it needs DAMAGED media (and a span/speed where flags actually fire):
 * ACCUDISC_ERR_NOTFOUND = not enough C2/instability evidence to conclude
 * (clean disc, clean span, or flags incoherent with instability) — never
 * an I/O failure.
 *
 * REPORT-ONLY: AccuDisc never applies the lag to delivered bitmaps; it is
 * a factual drive property for the caller to record and apply. peak_milli
 * is agreement against a PROXY oracle (reread instability), which cannot
 * see bytes that fail identically in paired reads — expect it well below
 * a database oracle's precision. A verdict is only returned when the peak
 * dominates every other shift (3x contrast) on top of evidence floors, so
 * an OK result is already an unambiguous alignment. */
typedef struct accudisc_c2_lag {
    int32_t  lag_pairs;     /* the peak shift, in sample pairs (4 bytes) */
    uint32_t sectors_active;/* C2-active sectors seen in the scan pass */
    uint32_t flags_used;    /* fired C2 bits contributing at the peak */
    uint32_t diff_bytes;    /* unstable byte observations accumulated */
    uint16_t peak_milli;    /* flags landing on unstable bytes at the peak, ‰ */
    uint16_t runner_milli;  /* best agreement at any OTHER shift, ‰ */
} accudisc_c2_lag;
/* On ACCUDISC_ERR_NOTFOUND the struct is still filled with whatever
 * evidence was gathered (all-zero = no C2 fired in the span at all), so
 * callers can distinguish "clean span" from "C2 seen but inconclusive". */

ACCUDISC_API int accudisc_probe_c2_lag(accudisc_device *dev, uint32_t lba,
                                       uint32_t count, accudisc_c2_lag *out);

/* Achievable-speed-ladder probe. CDROM_SELECT_SPEED is best-effort and
 * mode page 2A reports the SETTING, not reality — the only ground truth
 * for what a rung delivers is a timed streaming read. For each candidate
 * speed this sets it, lets the drive settle with a warm-up read, then
 * times a streaming read (~1 second's worth of audio at the requested
 * speed) in a fresh window inside [lba, lba+count). Every window of every
 * rung is disjoint from every other, so the drive cache can never serve a
 * remeasure.
 *
 * `points` selects how many radii each rung is measured at:
 *   1  one window per rung, inside [lba, lba+count) — the original
 *      behaviour, and what a caller wants when the span is already a
 *      chosen radius.
 *   3  the span is cut into three equal bands and each rung is measured
 *      once in each, giving the rung a RANGE instead of a point. Pass a
 *      whole-disc span for this to mean inner/middle/outer.
 * 0 is accepted as 1. Any other value is ACCUDISC_ERR_INVAL.
 *
 * A LARGER span is needed as `points` rises: every one of the
 * points * ncand windows must fit, or the probe returns
 * ACCUDISC_ERR_INVAL rather than overlapping them. This refusal matters
 * more than it looks — overlapping windows would be cache-served and
 * would report a rung as perfectly FLAT across radii, which is also the
 * signature of a genuinely CLV-clamped rung.
 *
 * Interpretation notes, in the order they bite:
 *
 * - measured_cx is the achieved rate at ONE radius: the band containing
 *   it is [lba, lba+count/points) for points == 1 and the MIDDLE band for
 *   points == 3. It is the same quantity in both cases, and the same
 *   quantity it has always been.
 *
 * - band_cx[] is where to read the shape. measured_cx/min_cx/max_cx are
 *   three summaries of it and cannot be inverted back into it: min and max
 *   say how far the rate moved, never WHERE it was fastest. On a healthy
 *   CAV rung that is a distinction without a difference — and that is the
 *   point, because a run where it does make a difference is a run where
 *   something happened that the summaries silently absorb.
 *
 * - CROSS-RUNG COMPARISON CARRIES A RADIUS TERM. Rungs are laid out along
 *   the span, so rung i and rung j are measured at different radii, and on
 *   a CAV drive radius alone changes the rate. With the conventional
 *   descending candidate list the fast rungs land innermost — a bias
 *   AGAINST them. Treat a modest cross-rung inversion as unproven, not as
 *   a measured fact about the rungs.
 *
 * - WITHIN a rung, min_cx/max_cx are sound: the three bands are a fixed
 *   distance apart whichever rung it is, the timed length is identical
 *   across them, and one speed setting covers all three. That is the
 *   comparison to trust — and on a CAV rung the spread IS the CAV curve,
 *   so a rung that comes out flat is either clamped (CLV) or not being
 *   measured properly.
 *
 * - A RUNG NORMALLY READS BELOW ITS OWN REQUESTED SPEED, AND THAT IS
 *   GEOMETRY RATHER THAN A FAULT. Under CAV the rate is proportional to
 *   radius, and a drive's advertised "Nx" is the rate at some outer radius
 *   the drive chooses; a disc whose lead-out falls short of that radius
 *   cannot reach Nx at any setting, in any band, however healthy it is.
 *   The shortfall is therefore not evidence of overhead, media or wear
 *   until the geometry has been subtracted. Judge a rung by the SHAPE of
 *   band_cx[] against the disc's length, never by the gap between the
 *   outer band and `req`. Where vendor documentation exists it states the
 *   two constants this needs: the address at which the nominal rate is
 *   reached, and the rung's rate at the innermost radius.
 *
 * - Rungs whose measured_cx collapse to the same value are
 *   indistinguishable on this rig (bus or firmware limited) and one of
 *   them suffices in a recovery ladder.
 *
 * THE ADMITTED LADDER (`verdict`). Page 2A advertises settings, not
 * rungs: a drive will accept a speed it cannot deliver, and will deliver
 * the same rate for two different settings. With points == 3 each rung is
 * judged and marked, so a caller can build a recovery ladder out of the
 * settings that are actually distinct on this drive AND this disc:
 *
 *   QUANTIZED  page 2A came back BELOW the request. The drive itself said
 *              it snapped (e.g. req 16 answered as 8), so this needs no
 *              measurement and no comparison — it is exact.
 *   DUPLICATE  measures no faster than the next lower admitted rung.
 *   ADMITTED   measures materially faster than it. Rungs are walked from
 *              slowest to fastest, so the LOWEST setting achieving a given
 *              rate is the one kept — the faster setting that buys nothing
 *              is the one discarded.
 *
 * How this survives the radius term above: it does not compare raw rates.
 * A rung's own max_cx - min_cx is the speed change across the whole span,
 * and adjacent rungs sit exactly one window apart, so that spread yields
 * the rate difference attributable to RADIUS ALONE between neighbours.
 * A gap must beat that by a margin before it counts as a real difference.
 * This is what min_cx/max_cx are for beyond reporting, and it is why no
 * verdict is possible without them: with points == 1 every rung comes
 * back ACCUDISC_RUNG_UNKNOWN rather than being judged on point samples.
 *
 * The verdict is about THIS disc. A rung admitted on a short disc may be
 * unreachable on a longer one (more radius, higher speeds), and media
 * whose achievable rate falls off toward the outer edge — observed, not
 * hypothetical — can invalidate a rung admitted mid-disc. Probe per disc;
 * never cache a ladder across discs.
 *
 * Report-only, like the other probes: this marks rungs but never discards
 * them (all ncand entries are filled, in the order given), never rewrites
 * a caller's accudisc_read_req.speed_ladder, and never applies a
 * correction. The drive is LEFT at the last candidate tested (speed is
 * never auto-restored, as with reads). */
/* Per-rung verdict: is this setting a REAL rung of this drive's ladder on
 * this disc? Report-only — the library never rewrites a caller's
 * accudisc_read_req.speed_ladder from it. */
#define ACCUDISC_RUNG_UNKNOWN   0 /* no verdict: points == 1 (no interval to
                                   * judge), or the rung did not measure */
#define ACCUDISC_RUNG_ADMITTED  1 /* delivers materially more than the next
                                   * lower admitted rung */
#define ACCUDISC_RUNG_DUPLICATE 2 /* delivers no more than equiv_x, after
                                   * discounting the radius term */
#define ACCUDISC_RUNG_QUANTIZED 3 /* the DRIVE said so: page 2A came back
                                   * below the requested speed (equiv_x is
                                   * what it snapped to) */

typedef struct accudisc_speed_rung {
    uint16_t requested_x;  /* the candidate passed in */
    uint16_t reported_x;   /* page 2A current speed after the set (0 = n/a) */
    uint16_t measured_cx;  /* timed streaming rate, centi-x (531 = 5.31x);
                            * points == 3 reports the MIDDLE band here */
    uint16_t min_cx;       /* slowest band for this rung, centi-x */
    uint16_t max_cx;       /* fastest band for this rung, centi-x.
                            * Both are 0 when points == 1 — "no gradient
                            * was measured", which is deliberately NOT the
                            * same as a measured gradient of zero. */
    uint16_t equiv_x;      /* for DUPLICATE/QUANTIZED: the rung this one
                            * collapses onto. 0 otherwise. */
    uint8_t  verdict;      /* ACCUDISC_RUNG_* */
    uint16_t band_cx[3];   /* the rate in each band, centi-x, IN SPAN ORDER:
                            * [0] lowest LBA, [2] highest. With a whole-disc
                            * span that is inner/middle/outer. 0 = that band
                            * did not measure; at points == 1 only [0] is
                            * filled and it holds the same figure as
                            * measured_cx.
                            *
                            * NOT interchangeable with min_cx/max_cx, and the
                            * difference is the reason this field exists.
                            * min/max are ORDER STATISTICS, these are
                            * LOCATIONS. They coincide only while the curve
                            * rises monotonically with radius — which is the
                            * normal CAV case and therefore exactly the case
                            * that would hide a mix-up. When they disagree
                            * (a governor step part-way across the disc, one
                            * cache-served band, another process on the bus)
                            * the disagreement is itself the finding, and it
                            * is unreachable from min/max alone.
                            *
                            * min_cx/max_cx keep their all-or-nothing rule:
                            * one failed band withdraws the pair. A band
                            * figure is an exact claim about one band, so it
                            * stands on its own and is reported whether or
                            * not its neighbours did. */
} accudisc_speed_rung;

ACCUDISC_API int accudisc_probe_speed_ladder(accudisc_device *dev,
                                             uint32_t lba, uint32_t count,
                                             const uint16_t *candidates,
                                             uint8_t ncand, uint8_t points,
                                             accudisc_speed_rung *out);

/* ---- status map ------------------------------------------------------------
 * The frame-accurate progress surface. The caller owns a buffer of one byte
 * per sector and passes it to a read (later: write) request; the engine
 * updates the byte for each sector with a single relaxed atomic store as its
 * state settles. Any thread — or, if the caller puts the buffer in shared
 * memory, any process — can poll it at zero syscall cost to draw progress
 * bars or EAC-style per-sector disc maps. No pipes, no events, no locks;
 * byte i is always the current best knowledge of sector (lba + i).
 *
 * Every state below is a RELATIVE claim — "stable/clean/unstable across the
 * reads of this run" — never verification against the pressing's canonical
 * bytes. A drive that misreads deterministically passes every relative
 * check; absolute gates (AccurateRip, CTDB) are the calling application's
 * job and always outrank anything recorded here.
 *
 * Byte layout: low nibble = state, high nibble = severity:
 *   C2        ~log2 of the sector's fired C2 bit count (1..15)
 *   RECOVERED number of extra reads it took (1..15)
 *   SUSPECT   ~log2 of the disagreeing byte count between reads
 *   others    0 */
#define ACCUDISC_MAP_PENDING   0x0 /* not yet attempted */
#define ACCUDISC_MAP_OK        0x1 /* read clean */
#define ACCUDISC_MAP_C2        0x2 /* delivered with fired C2 pointer(s) */
#define ACCUDISC_MAP_HARD      0x3 /* unreadable — zero-filled in the output */
#define ACCUDISC_MAP_RECOVERED 0x4 /* problem seen, clean/agreeing copy won */
#define ACCUDISC_MAP_SUSPECT   0x5 /* reads disagree — best-effort delivered */

/* ONE BYTE, SO A HIGHER STATE MASKS A LOWER ONE THAT ALSO APPLIES. The engine
 * classifies hard > suspect > recovered > C2 > ok, and only the winner is
 * stored. The reachable case: a sector recovered by consensus whose winning
 * copy still had C2 pointers fired is written RECOVERED, and its C2 is not
 * visible in the map at all.
 *
 * So COUNTING `C2` CELLS IS NOT THE COUNT OF C2-FLAGGED SECTORS — that is
 * accudisc_read_stats.sectors_flagged, which is accounted unconditionally. Use
 * the map to draw, use the stats to count.
 *
 * Three independent request fields make RECOVERED reachable, and a caller that
 * watches only one will be surprised by the others: `overlap_sectors`,
 * `c2_retries`, and `verify_passes >= 2`. With all three at their defaults
 * (a plain single-pass read) neither RECOVERED nor SUSPECT can occur. */
#define ACCUDISC_MAP_STATE(b)    ((uint8_t)(b) & 0x0f)
#define ACCUDISC_MAP_SEVERITY(b) ((uint8_t)(b) >> 4)

/* ---- Q-subchannel health map ------------------------------------------------
 * A second, independent lane of one byte per sector, requested by setting
 * `subq_map` on the read request. Same allocation, lifetime and live-read
 * semantics as the status map above: caller-owned, `count` bytes, one relaxed
 * atomic store per sector as its state settles, pollable from any thread or,
 * in shared memory, any process.
 *
 * It answers a DIFFERENT question. The status map is about the AUDIO — whether
 * the bytes handed to the sink can be trusted. This is about the Q subchannel
 * of that same delivered sector: whether its CRC-16 verified, and whether it
 * carried a position at all. A sector can be audio-clean with a dead Q (a lost
 * pregap or index) or the reverse, so neither map is derivable from the other.
 * The referent is always the sector as DELIVERED — after a rescue or consensus
 * pass, the winning copy's Q, matching read_stats' subq_total / subq_ok.
 *
 * REQUIRES ACCUDISC_SUB_RAW. Any other `sub` with subq_map set is
 * ACCUDISC_ERR_INVAL, not a uniform map: a lane that is all one colour because
 * nothing was measured looks exactly like a lane that is all one colour because
 * everything was fine, and no renderer can tell those apart.
 *
 * Why this lives here rather than in the caller's loop — the argument is
 * correctness, not convenience. Hard-unreadable sectors are delivered
 * ZERO-FILLED (see accudisc_chunk below), and a zero-filled Q frame FAILS
 * CRC-16; that is not a case the CRC recognises and excuses. A caller
 * recomputing this lane from the delivered subchannel therefore paints
 * fabricated Q damage on exactly the sectors whose audio is already gone —
 * where it corroborates the real failure sitting beside it and so reads as
 * confirmation rather than as a bug. Hence SUBQ_NO_AUDIO, stored before the
 * frame is examined at all.
 *
 *   0x0 PENDING      not yet attempted (byte untouched — zero your buffer)
 *   0x1 OK           CRC-16 verified, ADR=1 position frame
 *   0x2 BAD          CRC-16 failed; nothing in the frame means anything
 *   0x3 NO_POSITION  CRC-16 verified, ADR != 1 — an MCN or ISRC frame
 *   0x4 NO_AUDIO     sector was hard-unreadable; no frame was delivered
 *
 * NO_POSITION is HEALTHY, and it is the state a consumer is most likely to get
 * wrong. MCN and ISRC frames are legitimately interleaved into the position
 * stream by the pressing: ~1% of frames on a disc that carries them, and 0.00%
 * on one that carries neither. Under any worst-wins aggregation into cells (a
 * progress bar drawing one pixel per few thousand sectors) treating it as
 * damage flags EVERY cell on a perfect disc. The rate varying by disc makes it
 * more dangerous rather than less — validate on the wrong pressing and the
 * state looks unnecessary. Note also that the two known consumers read it with
 * opposite polarity, healthy for a health lane and signal for an ISRC/MCN scan,
 * which is why it is a state rather than a flag.
 *
 * BAD outranks NO_POSITION, necessarily: accudisc_q_parse fills `adr` from the
 * frame's first byte whether or not the CRC verified (it must — that byte is
 * the frame-type header), so a corrupt frame can present ADR=2 or 3. Classify
 * on `adr` first and a CRC-bad frame whose garbage header decodes to MCN gets
 * painted NO_POSITION, i.e. reported as HEALTHY, on precisely the frames this
 * lane exists to find.
 *
 * The severity nibble is ALWAYS ZERO. Q integrity is one CRC-16 over one
 * 12-byte frame — it verified or it did not — so anything graded there would be
 * a proxy for something else measured elsewhere.
 *
 * The numbering is deliberately PARALLEL to ACCUDISC_MAP_* so that one renderer
 * can draw both lanes, but the vocabularies are DISJOINT and the decoders are
 * NOT interchangeable. ACCUDISC_MAP_STATE() applied to a subq byte returns a
 * well-formed ACCUDISC_MAP_* value naming a state that never occurred —
 * NO_AUDIO reads back as RECOVERED, BAD as C2. Use the macro below. */
#define ACCUDISC_SUBQ_PENDING     0x0 /* not yet attempted */
#define ACCUDISC_SUBQ_OK          0x1 /* CRC-16 verified, ADR=1 position */
#define ACCUDISC_SUBQ_BAD         0x2 /* CRC-16 failed */
#define ACCUDISC_SUBQ_NO_POSITION 0x3 /* CRC-16 verified, ADR != 1 (MCN/ISRC) */
#define ACCUDISC_SUBQ_NO_AUDIO    0x4 /* hard-unreadable; no frame delivered */
#define ACCUDISC_SUBQ_MISPOSITION 0x5 /* CRC-16 verified, ADR=1 — and the
                                       * position it reports is NOT the sector
                                       * that was asked for. See below. */

/* ACCUDISC_SUBQ_MISPOSITION — the drive was somewhere else.
 *
 * A CRC-valid ADR=1 frame carries the drive's own claim about where the head
 * is. Comparing that claim against the LBA the command asked for is the only
 * check in this library that can contradict the drive on ITS OWN account
 * rather than by comparing two of its answers to each other.
 *
 * It exists because a real drive was measured doing exactly this: on a
 * PX-716A at maximum CAV speed, a whole-disc read returned 17 consecutive
 * sectors of perfectly valid audio from 2048 sectors earlier, each carrying a
 * CRC-valid Q frame that agreed with the displaced audio. C2 was silent, four
 * independent passes agreed byte-for-byte, and the chunk-seam check saw
 * nothing — the fault sat mid-transfer with correct data either side. Every
 * RELATIVE check in this engine failed simultaneously; the drive's own
 * position report was the one signal that dissented, and before this state
 * existed the lane labelled all 17 sectors ACCUDISC_SUBQ_OK.
 *
 * Measured over 1 404 000 sectors (four whole-disc passes): 17/17 true
 * positives, ZERO false positives, and zero on a pass carrying 2052
 * bit-error-corrupt sectors — a bit error is not a positioning fault and this
 * state correctly stays silent on one. Note the raw Q CRC failure rate on the
 * same passes was ~1600 per pass and is far too noisy to use as a signal;
 * position disagreement is the clean one.
 *
 * NOT a substitute for the absolute gates (AccurateRip / CTDB) in the calling
 * application, per docs/reference/RECOVERY.md — it catches a drive that lost
 * its place, not a rip that is wrong for some other reason. Requires
 * ACCUDISC_SUB_RAW: with no subchannel there is no claim to contradict. */

#define ACCUDISC_SUBQ_STATE(b) ((uint8_t)(b) & 0x0f)

/* ---- reading ---------------------------------------------------------------
 * One commanded read: the caller says what (range), with what companions
 * (C2 / subchannel), and how (chunking, retries, speed); the engine streams
 * raw sectors to the sink and reports per-sector status via the map.
 * AUDIO, C2 and SUB for a sector always come from the same READ CD transfer
 * (single-read alignment — the property C2-guided recovery depends on). */

/* c2 field */
#define ACCUDISC_C2_NONE     0
#define ACCUDISC_C2_PTRS     1 /* 294-byte pointer bitmap */
#define ACCUDISC_C2_PTRS_BEB 2 /* 296-byte pointers + block-error bits */
/* sub field */
#define ACCUDISC_SUB_NONE 0
#define ACCUDISC_SUB_RAW  1 /* raw interleaved P-W, 96 B */
#define ACCUDISC_SUB_Q    2 /* drive-formatted Q, 16 B */

/* ---- caller-declared struct size (the ABI hazard, API_PLAN §7.1) -----------
 * accudisc_read_req and accudisc_read_stats are caller-allocated, transparent,
 * cross an FFI boundary, and have both grown in place — measured across this
 * repo's own history:
 *
 *     accudisc_read_req    32 -> 40 -> 56 bytes
 *     accudisc_read_stats  80 -> 104 -> 128 -> 136 bytes
 *
 * That list is prose and drifts; it was stale by one growth (stopping at 128)
 * until 2026-08-01, when the 8trax agent quoted it back to us as something it
 * had learned. tests/test_abi.c pins the CURRENT sizes with _Static_assert, so
 * the assertions are authoritative and this paragraph is not.
 *
 * The CLI never noticed, because it is rebuilt with the library. A binding
 * compiled against one header and loaded against a different .so would, by
 * running off the end of a struct — and in the worst case by reading garbage
 * into a pointer field like `subq_map` or `cancel`, which the library would
 * then dereference or poll. Well-formed data, wrong referent, nothing
 * downstream able to tell.
 *
 * (This paragraph used to name `allow_unsafe` — a one-byte flag whose garbage
 * value silently disabled a guard. That field was removed in 0.6.0. The
 * example changed; the hazard did not, and the pointer version of it is worse,
 * because a wrong flag misbehaves while a wrong pointer corrupts or crashes.)
 *
 * So each carries its own size as its first field, and the caller sets it:
 *
 *     accudisc_read_req   req = ACCUDISC_READ_REQ_INIT;
 *     accudisc_read_stats st  = ACCUDISC_READ_STATS_INIT;
 *
 * The rules are asymmetric because the direction of the write is:
 *
 *   IN  (read_req, library reads): a SMALLER size than this build's is
 *       accepted and the missing tail is treated as zero — an older caller
 *       gets older behaviour. A LARGER size is accepted only if every byte
 *       past this build's end is zero; a nonzero one means the caller is
 *       asking for a feature this library does not have, and gets ERR_ABI
 *       rather than silence.
 *
 *   OUT (read_stats, library writes): a SMALLER size is honoured by writing
 *       only that many bytes. A LARGER size is ERR_ABI — the library would
 *       have to leave counters the caller believes in unfilled, and a zero
 *       that means "not computed" is indistinguishable from a zero that means
 *       "none observed".
 *
 * A zero size is always ERR_ABI: it is what a caller that forgot the macro
 * produces, so forgetting fails loudly on the first call instead of scribbling.
 * The only values with a meaning are sizeof() as some released header saw it.
 *
 * Structs NOT carrying a size are frozen: growing one is an soname bump, and
 * tests/test_abi.c pins their sizes so that growth cannot happen by accident.
 * accudisc_chunk is the one that matters — the library allocates it and the
 * sink reads it, so the hazard runs the other way. It has been 32 bytes since
 * it was introduced. */
#define ACCUDISC_READ_REQ_INIT   { .size = sizeof(accudisc_read_req) }
#define ACCUDISC_READ_STATS_INIT { .size = sizeof(accudisc_read_stats) }

typedef struct accudisc_read_req {
    uint32_t size;     /* = sizeof(accudisc_read_req); see above. NOT optional */
    uint32_t lba;      /* first sector */
    uint32_t count;    /* sectors to read (> 0) */
    uint8_t c2;        /* ACCUDISC_C2_* */
    uint8_t sub;       /* ACCUDISC_SUB_* */
    uint8_t any_type;  /* 0: expected type CD-DA; 1: any (mixed-mode spans) */
    uint8_t retries;   /* per-sector attempts after a chunk fails; 0 = 2 */
    uint16_t chunk_sectors; /* per READ CD command; 0 = max under 64 KiB */
    uint16_t speed_x;  /* set read speed first; 0 = leave as-is. NOT restored on
                        * exit: the drive is left at speed_x (or, if a ladder
                        * rung fired while speed_x == 0, at the drive's own
                        * management) — never at whatever the caller had set
                        * before the call. Deliberate: a recovery loop steps
                        * rungs without re-spinning, and restores once itself
                        * afterwards. Contrast accudisc_pregap_scan_opts.speed_x,
                        * which does restore on every exit path. */
    /* accuracy strategy (all off = single-pass fast read): */
    uint8_t c2_retries;    /* cache-defeated rereads hunting a C2-clean copy
                            * of each flagged sector (requires c2 != NONE);
                            * best read wins, whole sector replaced so
                            * AUDIO/C2/SUB stay single-read aligned */
    uint8_t verify_passes; /* >= 2: reread every chunk with cache defeat and
                            * compare audio; disagreeing sectors resolved by
                            * consensus (any two identical independent reads),
                            * else delivered best-effort as SUSPECT */
    uint8_t overlap_sectors; /* boundary overlap check: extend each chunk
                            * read by k trailing sectors and compare them
                            * against the next chunk's head — catches drive
                            * slips at chunk seams that back-to-back reads
                            * can't see. Mismatches go to consensus.
                            * 0 = off; clamped to 8 */
    /* speed ladder for problem-sector rereads: rescue/consensus attempt n
     * runs at ladder[min(n-1, len-1)] (e.g. {32,16,8,4} — descend toward
     * slow, careful reads). Pick rungs that differ from speed_x: consensus
     * votes must be speed-diverse, since a drive can misread the same way
     * at the same speed every time. (Verify passes themselves stream at
     * speed_x — drives recalibrate on every speed change, so per-chunk
     * speed switching thrashes; run whole-range passes at different
     * speed_x yourself for a full speed-diverse sweep.) The pass speed is
     * restored before the next streaming chunk. NULL/0 = reread at the
     * current speed. Caller-owned; must outlive the call. */
    const uint16_t *speed_ladder;
    uint8_t ladder_len;
    /* `uint8_t allow_unsafe` was here until 0.6.0. It opted out of a refusal to
     * capture subchannel while the vendor read-speed uncap was on. Both are
     * gone: the premise was that the uncap raises the CD-DA read rate, and it
     * does not — the drive's governor caps CD-DA at 40x regardless, and mode
     * page 2A reports the REQUEST rather than the governed throughput, so the
     * 48x that made the combination look dangerous was never reachable on an
     * audio disc. It occupied padding beside ladder_len, so its removal does
     * not move any following field. */
    uint8_t *status_map;        /* count bytes, or NULL; see status map above */
    const volatile int *cancel; /* poll: nonzero aborts at the next chunk; or NULL */
    /* count bytes, or NULL; see the Q-subchannel health map above. Requires
     * sub == ACCUDISC_SUB_RAW — anything else is ACCUDISC_ERR_INVAL.
     *
     * Appended here rather than beside status_map, where it belongs by meaning:
     * next to its sibling it would shift `cancel`, breaking every caller
     * compiled against 0.4. At the end it is purely additive — an older
     * caller's shorter struct zero-extends to subq_map == NULL and gets exactly
     * its old behaviour (the IN rule above), so no soname bump. */
    uint8_t *subq_map;
    /* AccuBuffer: bytes of chunk ring between the engine and `sink`, or 0 for
     * none (the default, and byte-for-byte the previous behaviour).
     *
     * WHAT IT BUYS, and what it does not. The kernel already decouples us from
     * slow STORAGE: measured against a write-rate-capped container at half the
     * drive's streaming rate, a whole-disc read took 301.1 s against a
     * pure-sink floor of 292.5 s — 95.5% of the disc read was hidden behind
     * the sink with no help from us, because fwrite returns into the page
     * cache and kworkers drain it. For an ordinary buffered file sink this
     * field is close to pointless and should be left 0.
     *
     * What no kernel buffer covers is work done INSIDE your sink: encoding,
     * hashing, AccurateRip/CTDB checksums, a GUI repaint. That time is time
     * the drive is not being read, and it is what this decouples. Pipes,
     * sockets, O_DIRECT and synchronous network filesystems are the same
     * shape.
     *
     * It CANNOT create bandwidth. A sink sustainably slower than the drive
     * fills any ring and then bounds the read regardless — a ring turns a
     * BURST into a delay. Size it for the longest stall worth surviving, not
     * for the disc: at a 16x rate, 64 MiB rides out about 23 s.
     *
     * SIZING, MEASURED. Start small. The 95.5% figure above was achieved by a
     * kernel dirty budget measured at 3.8 MiB hard-capped, with the working
     * set under 2.6 MiB — against 1.6 GiB on this machine's ordinary storage,
     * a factor of 432. So a few megabytes of buffer hid almost all of a 191.9 s
     * read behind a sink running at 1.6x slower, in the hostile case. There is
     * no evidence here that hundreds of megabytes buy anything, and the ring is
     * touched at allocation, so an oversized one costs real resident memory up
     * front. Single-digit MiB is the figure to try first; raise it only if
     * `buffer_stalls` says the ring was actually the constraint.
     *
     * >>> YOUR SINK RUNS ON ANOTHER THREAD. <<< That is the whole mechanism —
     * overlap requires it — and it is the one thing a caller must adapt to:
     *
     *   - The sink must be thread-safe with respect to your own state. It is
     *     called from exactly one thread, never concurrently with itself, and
     *     never concurrently with accudisc_read_cdda's return.
     *   - `chunk.data` points into the ring and is valid for the duration of
     *     the callback only, exactly as before — but now a slow sink HOLDS A
     *     RING SLOT, so retaining that pointer stalls the producer rather than
     *     merely reading stale bytes.
     *   - Cancellation DISCARDS what is queued. With a ring the engine may be
     *     several chunks ahead when your sink returns non-zero or *cancel goes
     *     nonzero; those chunks are never delivered. Draining instead would
     *     make a cancel take as long as the backlog.
     *   - status_map and subq_map are still written by the reading thread, so
     *     a sector's map byte may settle BEFORE its chunk reaches your sink.
     *     Without a buffer the two were effectively simultaneous.
     *
     * Rounded DOWN to whole chunks and clamped to at least two (one filling,
     * one draining — a single slot is the synchronous path with extra steps).
     * If the ring cannot be created the read FAILS with ACCUDISC_ERR_NOMEM; it
     * never silently falls back, because a caller who asked for a buffer and
     * got different behaviour without being told is exactly the defect shape
     * this library refuses. Appended last, so a shorter caller's struct
     * zero-extends to 0 and gets the old path (the IN rule above). */
    uint32_t buffer_bytes;
    /* 0 means the DEFAULT (a few seconds of audio), not "off" — since 0.27.0.
     * Pass ACCUDISC_BUFFER_NONE to run without one deliberately.
     *
     * WHY THIS FLIPPED, since the paragraph at the version macro above still
     * records the measurement that said it should be off. That measurement
     * stands and is not being disowned: against a rate-capped file sink, 95.5%
     * of a disc read was already hidden by the page cache, so the ring added
     * almost nothing THERE. What it measured was a quiet machine doing one
     * thing. A buffer earns its keep in the tail — the writeback storm, the
     * filled tmpfs, the desktop that stops responding — and a measurement of
     * the steady state cannot speak to that. Defaulting it off meant the
     * protection was absent exactly when nobody was thinking about it, which
     * is when it is needed. */
} accudisc_read_req;

/* One delivered chunk. data holds nsec sectors, each sector_len bytes laid
 * out AUDIO (audio_len) + C2 (c2_len) + SUB (sub_len). Hard-unreadable
 * sectors arrive zero-filled with an all-ones C2 bitmap so the streams never
 * desync. The pointer is only valid during the call. */
typedef struct accudisc_chunk {
    uint32_t lba;
    uint32_t nsec;
    const uint8_t *data;
    uint32_t sector_len;
    uint32_t audio_len;
    uint32_t c2_len;
    uint32_t sub_len;
} accudisc_chunk;

/* Return 0 to continue; nonzero cancels the read (ACCUDISC_ERR_CANCELLED). */
typedef int (*accudisc_sink_fn)(void *user, const accudisc_chunk *chunk);

typedef struct accudisc_read_stats {
    uint32_t size;            /* = sizeof(accudisc_read_stats); see above.
                               * Set it BEFORE the call even though the rest is
                               * output — it is how the library learns how much
                               * of your allocation it may write. */
    uint64_t sectors_read;    /* returned by the drive (excludes zero-fills) */

    /* CONTRACT — the three fields below are the caveat verdict's inputs.
     *
     *     hard_errors || sectors_suspect || sectors_flagged
     *
     * is what the CLI projects onto exit 3, and by ruling (2026-07-29) it is
     * what every API consumer re-derives for itself: the library deliberately
     * exports no verdict helper and no verdict field.
     *
     * So these three are a STABILITY PROMISE, not internal accounting. Changing
     * what feeds one of them is a semantic break even though the struct does
     * not move — a consumer showing a user "this rip is clean" would silently
     * change its mind — and it takes a version bump for the same reason
     * ACCUDISC_ERR_NOT_BLANK did. See docs/reference/cli-machine-interface.md
     * ("Exit codes -> library semantics"); the note is repeated here because
     * that document's audience is CLI consumers and these fields' audience is
     * not. */
    uint64_t sectors_flagged; /* >= 1 C2 bit set */
    uint64_t c2_bits;         /* total fired C2 bits (real reads only) */
    uint64_t hard_errors;     /* sectors zero-filled after retries */
    uint32_t max_bits_sector; /* worst single sector's C2 bit count */
    int64_t first_flagged_lba; /* -1 if none */
    int64_t last_flagged_lba;  /* -1 if none */
    uint64_t sense_medium;    /* hard failures: sense key 3 */
    uint64_t sense_hardware;  /* sense key 4 */
    uint64_t sense_other;     /* any other terminal sense */
    uint64_t rereads;         /* problem-driven extra sector reads issued */
    uint64_t sectors_recovered; /* problem seen, clean/agreeing copy won */
    uint64_t sectors_suspect;   /* consensus failed, best-effort delivered.
                                 * CONTRACT — third input to the caveat verdict;
                                 * see the note at sectors_flagged above. */
    uint64_t slips;           /* disagreements that were a pure positional
                               * shift (reads identical modulo offset) — the
                               * C2-invisible slip class; a nonzero count on
                               * a drive says: use overlap checking */
    /* Q-subchannel health, counted only for --sub raw reads over the sector
     * data actually delivered. The subchannel has no CIRC (C1/C2) protection —
     * a per-frame CRC-16 is its only integrity check, and it fails
     * independently of the audio C2 stats above. subq_bad = subq_total -
     * subq_ok is the pregap/index/MSF metadata lost on this pass. */
    uint64_t subq_total;      /* Q frames examined (delivered sectors w/ raw sub) */
    uint64_t subq_ok;         /* frames whose CRC-16 verified */

    /* THE HONOURED PASS SPEED. A drive implements only certain rungs and snaps
     * anything else DOWN to the next one it has — a PX-716A asked for 16x
     * adopts 8x — so a read can silently run at half the requested rate. These
     * two make that observable without a second drive command by the caller,
     * and without re-reading their own request struct.
     *
     * FOUR STATES, and the zero cases are NOT the same:
     *
     *   requested == 0                  no speed was requested (drive-managed)
     *   requested == N, honoured == 0   asked, but no answer: either the set
     *                                   failed or page 2A did not read back.
     *                                   NOT "it ran at 0x", and not evidence
     *                                   the request was honoured
     *   honoured  <  requested          QUANTIZED — this is the signal
     *   honoured  == requested          honoured
     *
     * So the test is `honoured && honoured < requested`. The CLI derives it
     * exactly that way; anything else will disagree with it eventually.
     *
     * `honoured` is populated ONLY when the speed set returned OK. A failed set
     * still leaves page 2A reporting some current speed, quite possibly below
     * the request, and exporting that as "quantized" would describe a set that
     * never happened — a different failure needing a different response.
     *
     * SCOPE: the PASS speed (req->speed_x), read back once immediately after
     * it is applied. RECOVERY-LADDER RUNGS ARE NOT COVERED — speed_ladder moves
     * the speed mid-read, and a per-rung answer needs per-rung storage. Use
     * accudisc_probe_speed_ladder, whose ACCUDISC_RUNG_QUANTIZED verdict is
     * exactly that question asked per rung, before the read. */
    uint16_t speed_requested_x; /* echo of req->speed_x; 0 = none asked */
    uint16_t speed_honoured_x;  /* page 2A current after the set; 0 = unknown */
    uint32_t subq_misposition;  /* sectors whose CRC-valid ADR=1 Q reported a
                                 * DIFFERENT LBA than the one commanded — the
                                 * drive read somewhere else and said so.
                                 * Counts ONLY the sectors that actually
                                 * disagreed — the engine additionally treats a
                                 * small margin either side as suspect, because
                                 * the leading edge of a slip carries correct Q
                                 * with already-wrong audio, but those are not
                                 * counted here. Always 0 without SUB_RAW. */
    /* These two took the struct 136 -> 144 (measured, both compiled), and left
     * 4 bytes of tail padding behind: the next two uint16_t, or one uint32_t,
     * are free. Growth is safe here because this is an OUT struct — the caller
     * declares `size`, adsc_abi_export REFUSES rather than truncates when we
     * have less than they declared, and a shorter caller simply never sees
     * these fields. That is the opposite direction from the IN rule. */
    /* AccuBuffer high-water mark: most chunks queued at once, and how many
     * times the producer had to wait for a free slot. Both 0 when no buffer
     * was requested.
     *
     * APPENDED AT THE END, not beside the other throughput counters where they
     * read better. 0.21.0 shipped subq_misposition at a fixed offset one week
     * -- one commit -- ago, and inserting anything above it would move it
     * under every consumer already built against that release. Same reasoning
     * as subq_map's note in accudisc_read_req: once a field is public, meaning
     * loses to layout.
     *
     * These are the honest answer to "did the buffer help?", which a
     * throughput figure cannot give: a rip that was never sink-bound looks
     * identical either way. `buffer_stalls == 0` means the ring was never the
     * constraint and the buffer did nothing. A peak at capacity with stalls
     * climbing means it was full — undersized, or a sink sustainably slower
     * than the drive, which no size fixes. */
    uint32_t buffer_peak_chunks;
    uint64_t buffer_stalls;
} accudisc_read_stats;

/* Blocking. Streams req->count sectors from req->lba into sink (which may be
 * NULL to read for status/stats only). stats may be NULL. */
ACCUDISC_API int accudisc_read_cdda(accudisc_device *dev,
                                    const accudisc_read_req *req,
                                    accudisc_sink_fn sink, void *user,
                                    accudisc_read_stats *stats);

/* ---- MSF <-> LBA ----------------------------------------------------------
 * MSF as it appears on disc; LBA 0 == 00:02:00 (the 150-sector pregap). An LBA
 * below -150 (deep lead-in) is before 00:00:00, which MSF cannot represent, so
 * accudisc_lba_to_msf clamps it to 00:00:00. */
ACCUDISC_API int32_t accudisc_msf_to_lba(uint8_t m, uint8_t s, uint8_t f);
ACCUDISC_API void accudisc_lba_to_msf(int32_t lba, uint8_t *m, uint8_t *s,
                                      uint8_t *f);

/* ---- Q subchannel ----------------------------------------------------------
 * Pure decoders for the 96-byte raw interleaved P-W stream captured with
 * ACCUDISC_SUB_RAW (bit 6 of each byte is the Q channel). All BCD fields are
 * decoded to binary; CRC-16 (X.25) is verified before anything is trusted. */

/* Q ADR values */
#define ACCUDISC_Q_POSITION 1 /* track/index/relative + absolute MSF */
#define ACCUDISC_Q_MCN      2 /* media catalog number */
#define ACCUDISC_Q_ISRC     3 /* track ISRC */

typedef struct accudisc_q {
    uint8_t adr;      /* ACCUDISC_Q_* */
    uint8_t control;  /* CTRL nibble: bit 2 = data track, bit 0 = pre-emphasis */
    uint8_t crc_ok;
    /* adr 1 (position): */
    uint8_t tno;      /* track number (0 in lead-in) */
    uint8_t index;    /* 0 = pregap */
    uint8_t rel_m, rel_s, rel_f; /* within track */
    uint8_t abs_m, abs_s, abs_f; /* on disc */
    /* adr 2: */
    char mcn[14];     /* 13 digits, NUL-terminated */
    /* adr 3: */
    char isrc[13];    /* 12 chars, NUL-terminated */
} accudisc_q;

/* Extract the 12 Q bytes from one raw interleaved 96-byte subcode block. */
ACCUDISC_API void accudisc_sub_extract_q(const uint8_t raw[96], uint8_t q[12]);

/* Parse a 12-byte Q frame. On ACCUDISC_ERR_CRC (CRC did not verify) only adr,
 * control and crc_ok are set; all position/MCN/ISRC fields are left zero rather
 * than decoded, since a bad frame yields out-of-range BCD/ISRC values. They are
 * populated only on ACCUDISC_OK. */
ACCUDISC_API int accudisc_q_parse(const uint8_t q[12], accudisc_q *out);

/* ---- R-W subchannel (CD+G) --------------------------------------------------
 * Graphics ride in the R to W subchannels. Structure, per the Philips/Sony
 * "Subcode/Control and Display System, Channels R-W" specification (Nov 1991)
 * §5.1:
 *
 *     6 bits (one frame's R..W)  = 1 SYMBOL
 *     24 SYMBOLS                 = 1 PACK
 *     4 PACKS                    = 1 PACKET
 *
 * A sector carries 98 frames; the first two are the S0/S1 subcode sync, leaving
 * 96 symbols = 4 packs = exactly ONE packet per sector. At 75 sectors/s that is
 * 75 packets/s and 300 PACKS/s. The `.cdg` file format is the 24-byte pack
 * stream at 300/s (7200 B/s), one byte per symbol with 6 bits significant — so
 * a pack, not a packet, is the unit that leaves here.
 *
 * Unlike the Q channel, whose only check is a per-frame CRC-16, R-W is properly
 * error-protected: a (24,20) Reed-Solomon code over GF(2^6) across the pack, 8x
 * interleaved for burst tolerance, plus a (4,2) Reed-Solomon code guarding
 * symbols 0..3 — the MODE/ITEM and INSTRUCTION fields that say how to read the
 * rest. Both are decoded here. Correcting them is recovering bits that WERE
 * recorded, not interpreting them, so it belongs on this side of the
 * "AccuDisc only moves bits" line; rendering packs to images does not.
 *
 * Recovery is REPORTED, never hidden: each pack carries how many symbols were
 * repaired and whether either code gave up. Nothing is interpolated. */

#define ACCUDISC_RW_PACK_SYMBOLS  24
#define ACCUDISC_RW_PACKS_PER_SEC 4  /* packs per SECTOR, not per second */
/* The de-interleave is convolutional with a span of 8 packs, so the first 7
 * channel packs fed produce no output. Callers that care about exact stream
 * alignment need this; most do not. */
#define ACCUDISC_RW_PRIME_PACKS   7

typedef struct accudisc_rw_pack {
    uint8_t symbol[ACCUDISC_RW_PACK_SYMBOLS]; /* 6-bit values, one per byte */
    uint8_t p_fixed;  /* symbols repaired by the (24,20) P code (0..2) */
    uint8_t q_fixed;  /* symbols repaired by the (4,2) Q code (0..1) */
    uint8_t p_failed; /* 1 = P syndromes non-zero and not correctable */
    uint8_t q_failed; /* 1 = Q syndromes non-zero and not correctable */
} accudisc_rw_pack;

/* Symbol 0 packs a 3-bit MODE and a 3-bit ITEM. CD+G is MODE 1 / ITEM 1, which
 * is why every .cdg player tests byte 0 for 0x09. */
#define ACCUDISC_RW_MODE(p) (((p)->symbol[0] >> 3) & 0x07)
#define ACCUDISC_RW_ITEM(p) ((p)->symbol[0] & 0x07)
#define ACCUDISC_RW_MODE_ZERO      0 /* no R-W data recorded */
#define ACCUDISC_RW_MODE_GRAPHICS  1 /* ITEM 0 = LINE GRAPHICS, 1 = TV (CD+G) */
#define ACCUDISC_RW_ITEM_TV_GRAPHICS 1

typedef struct accudisc_rw accudisc_rw;

/* A streaming R-W decoder. Stateful because the de-interleave spans 8 packs. */
ACCUDISC_API accudisc_rw *accudisc_rw_open(void);
ACCUDISC_API void accudisc_rw_close(accudisc_rw *rw);

/* Feed one sector's 96 raw P-W subcode bytes (as ACCUDISC_SUB_RAW delivers).
 * Emits 0 to 4 fully de-interleaved, error-corrected packs into out[], and
 * writes the count to *emitted. Fewer than 4 only while priming (the
 * de-interleave spans 8 packs). max must be 0 (prime-only; out may be NULL)
 * or >= ACCUDISC_RW_PACKS_PER_SEC, so a sector's output is never truncated —
 * a smaller non-zero max is rejected rather than silently desyncing the ring.
 * Returns ACCUDISC_OK, or ACCUDISC_ERR_INVAL on bad arguments. */
ACCUDISC_API int accudisc_rw_feed(accudisc_rw *rw, const uint8_t raw[96],
                                  accudisc_rw_pack *out, unsigned max,
                                  unsigned *emitted);

typedef struct accudisc_rw_stats {
    uint64_t packs;        /* packs emitted */
    uint64_t p_fixed;      /* symbols repaired by the P code */
    uint64_t q_fixed;      /* symbols repaired by the Q code */
    uint64_t p_failed;     /* packs where P could not correct */
    uint64_t q_failed;     /* packs where Q could not correct */
    uint64_t mode_zero;    /* packs carrying no R-W data */
    uint64_t mode_graphics;/* packs in a GRAPHICS mode */
} accudisc_rw_stats;

ACCUDISC_API void accudisc_rw_get_stats(const accudisc_rw *rw,
                                        accudisc_rw_stats *out);

/* Convenience: scan the disc's Q stream (raw subchannel reads starting at
 * lba) for an MCN / a track ISRC; ACCUDISC_ERR_NOTFOUND when the disc does
 * not carry one. For ISRC, start at the target track's first sector. */
ACCUDISC_API int accudisc_scan_mcn(accudisc_device *dev, uint32_t lba,
                                   char mcn[14]);
ACCUDISC_API int accudisc_scan_isrc(accudisc_device *dev, uint32_t lba,
                                    char isrc[13]);

/* ---- index / pregap map ----------------------------------------------------
 * The TOC gives only index-1 (track start). Pregaps (index 0) and intra-track
 * indices live ONLY in the Q subchannel, which carries no CIRC — a per-frame
 * CRC-16 is its sole integrity check. This decodes a raw-subchannel scan into
 * a per-track index/pregap map, cross-referenced against the TOC's
 * authoritative index-1 boundaries, gating on CRC so damage cannot inject a
 * false index. It is purely observational: where the boundary approach is
 * damaged it reports UNKNOWN rather than guessing — model-based reconstruction
 * across the gap is a separate step. */

typedef enum {
    ACCUDISC_PREGAP_NO_DATA = 0, /* scan did not cover this boundary */
    ACCUDISC_PREGAP_NONE,        /* gapless: clean approach, no index-0 frames */
    ACCUDISC_PREGAP_PRESENT,     /* pregap observed; start/length reconstructed */
    ACCUDISC_PREGAP_UNKNOWN,     /* boundary damaged; presence indeterminate */
} accudisc_pregap_state;

typedef struct accudisc_index_map {
    uint8_t  track;         /* 1..99 */
    uint8_t  pregap_state;  /* accudisc_pregap_state */
    uint8_t  max_index;     /* highest CRC-good index seen for this track */
    int32_t  index1_lba;    /* authoritative track start (from the TOC) */
    int32_t  q_index1_lba;  /* index-1 start as seen in Q, or -1 (cross-check) */
    int32_t  index0_lba;    /* reconstructed pregap start, or -1 */
    uint32_t pregap_frames; /* index1_lba - index0_lba when PRESENT, else 0.
                             * A lower bound if the transition frame itself was
                             * CRC-bad (recovered exactly only by the model). */
    uint32_t crc_ok;        /* CRC-good position frames in the boundary window */
    uint32_t crc_bad;       /* CRC-bad frames in the boundary window */
} accudisc_index_map;

/* Decode a per-sector raw subchannel scan (count*96 bytes for sectors
 * [base_lba, base_lba+count)) into a per-track index/pregap map. Writes up to
 * max_out entries (one per track in toc), returns the number written. The scan
 * need not be whole-disc: only the neighbourhood of each track boundary must be
 * covered, else that track is reported ACCUDISC_PREGAP_NO_DATA. */
ACCUDISC_API uint32_t accudisc_index_map_decode(const uint8_t *raw,
                                                int32_t base_lba, uint32_t count,
                                                const accudisc_toc *toc,
                                                accudisc_index_map *out,
                                                uint32_t max_out);

/* ---- pregap / index scan ---------------------------------------------------
 * Read each track boundary's neighbourhood and decode it. The acquisition
 * POLICY here — how much to read around a boundary, and that boundaries are
 * read one at a time — was the CLI's, so every binding would have had to guess
 * it, and two callers guessing differently produce index maps that are not
 * comparable while looking identical.
 *
 * ONE READ PER BOUNDARY is the load-bearing part, not an implementation
 * detail: the seek between boundaries is what defeats the drive's cache. A
 * caller that "optimises" this into one long read gets the cache's answer for
 * every boundary after the first. */

/* Defaults for accudisc_pregap_scan_opts. Public because they define what the
 * numbers a scan reports actually mean: crc_ok/crc_bad are counts over exactly
 * this window, so a caller comparing two scans taken with different windows is
 * comparing nothing, and it cannot know that if the window is invisible. */
#define ACCUDISC_PREGAP_WINDOW 400u /* sectors read BEFORE each track start */
#define ACCUDISC_PREGAP_TAIL   4u   /* and after, to catch index 01 itself */

typedef struct accudisc_pregap_scan_opts {
    uint32_t window;  /* 0 = ACCUDISC_PREGAP_WINDOW */
    uint32_t tail;    /* 0 = ACCUDISC_PREGAP_TAIL */
    uint16_t speed_x; /* 0 = leave the drive's speed alone. When set, the prior
                       * speed is restored on every exit path — see the note in
                       * src/cdda/pregap_scan.c about what "prior" can mean. */
    const volatile int *cancel; /* poll: nonzero aborts between boundaries */
} accudisc_pregap_scan_opts;

/* Scan every track boundary in toc, writing up to max entries and the number
 * written to *n_out. Returns ACCUDISC_OK, or the read error from the FIRST
 * boundary that failed.
 *
 * A failed boundary read ABORTS the scan; entries already written stay valid
 * and *n_out says how many. It deliberately does not mark the failed boundary
 * and continue: the only value available to mark it with is
 * ACCUDISC_PREGAP_NO_DATA, which already means "the scan did not cover this
 * track", and overloading it would make "not scanned" and "unreadable"
 * indistinguishable to every caller. Reporting per-boundary read errors needs a
 * field in accudisc_index_map and is not smuggled in here. */
ACCUDISC_API int accudisc_scan_pregaps(accudisc_device *dev,
                                       const accudisc_toc *toc,
                                       const accudisc_pregap_scan_opts *opts,
                                       accudisc_index_map *out, uint8_t max,
                                       uint8_t *n_out);

/* ---- full TOC (session structure) ------------------------------------------
 * Parses the blob from accudisc_read_full_toc (READ TOC format 2): raw
 * lead-in entries per session. Points 0x01-0x63 are track starts (address in
 * pmin/psec/pframe); 0xA0 = first track (+ disc type in psec), 0xA1 = last
 * track, 0xA2 = lead-out start. MSF values are kept raw — use
 * accudisc_msf_to_lba. */

typedef struct accudisc_fulltoc_entry {
    uint8_t session;
    uint8_t adr_ctrl; /* ADR high nibble, CTRL low */
    uint8_t point;
    uint8_t min, sec, frame;    /* running time in lead-in */
    uint8_t pmin, psec, pframe; /* the entry's address / payload */
} accudisc_fulltoc_entry;

typedef struct accudisc_fulltoc {
    uint8_t first_session;
    uint8_t last_session;
    uint16_t entry_count;
    accudisc_fulltoc_entry entries[136]; /* 99 tracks + 3/session + slack */
} accudisc_fulltoc;

ACCUDISC_API int accudisc_fulltoc_parse(const uint8_t *raw, uint32_t len,
                                        accudisc_fulltoc *out);

/* ---- CD-Text ----------------------------------------------------------------
 * Decodes the blob from accudisc_read_cdtext (18-byte packs). v0 scope:
 * block 0 (first language), single-byte character packs, types title /
 * performer / songwriter / UPC-ISRC. Bytes are copied through verbatim —
 * no character-set conversion, the caller interprets (passes UTF-8-authored
 * discs through undamaged). Packs failing CRC are skipped. */

#define ACCUDISC_TEXT_MAX 160

typedef struct accudisc_cdtext_strings {
    char title[ACCUDISC_TEXT_MAX];
    char performer[ACCUDISC_TEXT_MAX];
    char songwriter[ACCUDISC_TEXT_MAX];
    char code[ACCUDISC_TEXT_MAX]; /* UPC (album) / ISRC (track), type 0x8E */
} accudisc_cdtext_strings;

typedef struct accudisc_cdtext {
    accudisc_cdtext_strings album;      /* pack track number 0 */
    accudisc_cdtext_strings track[100]; /* indexed by track number, 1..99 */
} accudisc_cdtext;

/* *out is library-allocated (accudisc_free). ACCUDISC_ERR_SHORT when the
 * blob holds no usable packs. */
ACCUDISC_API int accudisc_cdtext_decode(const uint8_t *raw, uint32_t len,
                                        accudisc_cdtext **out);


/* ---- CTDB parity repair -------------------------------------------------
 *
 * Applies a CueTools DB parity blob to a rip, correcting the samples CIRC
 * could not. This is the one place AccuDisc does arithmetic on audio rather
 * than moving it; see CLAUDE.md §Scope.
 *
 * WHAT A SUCCESSFUL RETURN MEANS, precisely, because the distinction is the
 * whole safety argument:
 *
 *   ACCUDISC_OK  every column whose syndromes disagreed with CTDB's was
 *                corrected, and every correction was re-verified against the
 *                error syndromes before being applied. out_pcm holds the
 *                repaired audio.
 *
 *   ACCUDISC_CTDB_UNVERIFIED
 *                the same, EXCEPT that report->unverified_columns of them were
 *                determined rather than verified. A STRICTLY WEAKER CLAIM —
 *                see below. Positive, so a caller testing `rc == ACCUDISC_OK`
 *                declines it and must opt in deliberately.
 *
 * It does NOT mean the audio is right. **This is a relative check, not an
 * absolute gate.** CTDB publishes per-track CRCs, not a whole-image CRC, so
 * nothing here can compare the result against a published value; crc32_after
 * is computed by this library, not fetched. The absolute gates — CTDB
 * per-track CRC and AccurateRip — live in the calling application and must
 * still be applied afterwards (docs/reference/RECOVERY.md: relative checks
 * never outrank absolute gates).
 *
 * DETERMINED VERSUS VERIFIED, the distinction ACCUDISC_CTDB_UNVERIFIED marks.
 * A column carrying exactly npar erasures consumes every check equation the
 * parity has: npar syndromes, npar unknown magnitudes at known positions. The
 * system is exactly determined, so it always has a solution, and re-deriving
 * the syndromes from that solution is an identity — it cannot disagree. The
 * re-verification that makes every other column trustworthy is therefore
 * vacuous at exactly full erasure capacity, and this is true of any correct
 * errata decoder, not a defect in this one. What it means in practice: such a
 * column is right if and only if the erasure list for it was COMPLETE. Ours
 * comes from C2, which under-flags as well as over-flags. One unflagged error
 * alongside npar-1 correct flags yields a confident wrong repair with no
 * residual redundancy left to notice.
 *
 * Every word this can touch lies inside the CTDB image window, which is also
 * the window the per-track CRCs cover — so the caller's absolute gate does
 * close over it. That is why this is reported rather than refused: refusing
 * would discard the at-capacity repairs that were correct, and residual error
 * capacity cannot recover a position dropped from the erasure list.
 *
 * A HOSTILE PARITY BLOB IS NOT A MEMORY-SAFETY PROBLEM AND IS STILL A PROBLEM.
 * Parity computed over an attacker-chosen target image will rewrite the audio
 * into exactly that target and return success — measured, all 4704 words of a
 * synthetic image. No bound is violated: the rewrite stays inside the codeword
 * region and within npar/2 symbols per column. That is what a parity code IS,
 * and it is the other reason the caller's per-track CRC gate is load-bearing
 * rather than advisory. Fetch the blob and the CRCs from the same entry.
 *
 * There is deliberately no way to obtain the correction list without the
 * verdict. A decoder handed a wrong npar or a wrong offset will emit a
 * populated, plausible-looking correction list alongside a failed verdict —
 * measured, 531 corrections at npar=2 on audio that was undamaged at
 * npar=16 — so a caller able to read one without the other can corrupt good
 * audio by ignoring a flag. This returns the repaired image or an error, the
 * same shape as accudisc_write.
 *
 * TWO DOMAINS, never conflated. `pcm` is the whole rip, [0, lead-out). The
 * CTDB image is the window [image_first_frame, image_first_frame +
 * image_frames) inside it. They coincide only when track 1 INDEX 01 sits at
 * LBA 0, which is why a bug here stayed invisible until a disc with a
 * pressed-in pregap turned up. Frames are 2352-byte sectors throughout;
 * offsets are in stereo sample PAIRS (4 bytes), the unit AccurateRip and
 * CTDB both use. */

/* A POSITIVE accudisc_ctdb_repair() return: the repair was applied and out_pcm
 * is written, but report->unverified_columns columns were determined rather
 * than verified (see above). Test with `rc >= 0` for "audio was written" and
 * `rc == ACCUDISC_OK` for the full claim; the two differ ON PURPOSE, so that a
 * caller which never heard of this code keeps the strong contract it was
 * written against instead of silently inheriting a weaker one.
 *
 * Deliberately not the same value as ACCUDISC_WROTE_WITH_CAVEATS: the two say
 * different things and nothing should be able to conflate them if a return
 * code is ever logged or compared away from its call site. */
#define ACCUDISC_CTDB_UNVERIFIED 2

#define ACCUDISC_CTDB_REQ_INIT    { .size = sizeof(accudisc_ctdb_req) }
#define ACCUDISC_CTDB_REPORT_INIT { .size = sizeof(accudisc_ctdb_report) }

typedef struct accudisc_ctdb_req {
    uint32_t size;              /* = sizeof; ACCUDISC_CTDB_REQ_INIT */
    uint32_t npar;              /* parity symbols per column, from the entry */
    uint32_t wire_stride;       /* the entry's `stride` field, sample pairs */
    uint32_t image_first_frame; /* CTDB bounds[0], a PCM sector index */
    uint32_t image_frames;      /* bounds[-1] - bounds[0], in sectors */
    int32_t  offset_pairs;      /* alignment of our PCM against the entry */

    /* Whole rip, 16-bit LE stereo. pcm, parity and out_pcm are all read and
     * written as 16-bit words, so all three must be 2-BYTE ALIGNED; an odd
     * address is refused with ACCUDISC_ERR_INVAL rather than being undefined
     * behaviour. Anything malloc() returned already satisfies this. */
    const uint8_t *pcm;
    uint64_t       pcm_bytes;

    const uint8_t *parity;      /* the entry's blob, exactly as fetched */
    uint64_t       parity_bytes;

    /* C2 erasures: one bit per 16-bit WORD, bit (i & 7) of byte (i >> 3),
     * ABSOLUTE over `pcm` — index it by PCM word, never by image word. The
     * library does the domain shift; a caller that pre-shifts will shift
     * twice. NULL (with _bytes 0) means error-only decoding, which is a
     * normal mode and not a degraded one.
     *
     * pcm_erasures_bytes is TRUSTED, exactly as pcm_bytes is: the library
     * checks that the length you declare is large enough to cover the PCM, and
     * cannot check that it is true. A declared length longer than the memory
     * you actually allocated is read past the end, and no arithmetic on this
     * side can detect it. Stated because it was once mistaken for something a
     * bounds check had closed. */
    const uint8_t *pcm_erasures;
    uint64_t       pcm_erasures_bytes;
} accudisc_ctdb_req;

/* Filled on success AND on ACCUDISC_ERR_NOTFOUND, so a caller can tell "clean
 * already" (dirty_columns 0) from "damaged beyond this entry's capacity"
 * (refused_columns > 0) without a second call. Untouched on argument errors,
 * where no measurement happened. */
typedef struct accudisc_ctdb_report {
    uint32_t size;              /* = sizeof; ACCUDISC_CTDB_REPORT_INIT */
    int32_t  offset_pairs;      /* the offset used, echoed back */
    uint32_t dirty_columns;     /* columns whose syndromes did not reconcile */
    uint32_t repaired_columns;  /* of those, the ones that DECODED. On the
                                 * ACCUDISC_ERR_NOTFOUND path nothing was
                                 * applied, so this can be non-zero while
                                 * `corrections` is 0: it describes the disc,
                                 * not the buffer. */
    uint32_t refused_columns;   /* of those, the ones beyond capacity */
    uint32_t erasure_columns;   /* dirty columns carrying at least one erasure.
                                 * NOTE this counts columns that HAD erasures,
                                 * not columns the erasures helped — the two
                                 * are different questions and conflating them
                                 * loses the simpler one. */
    uint32_t corrections;       /* symbols actually changed; zero-magnitude
                                 * errata (C2 over-flagging) are not counted */
    uint32_t crc32_before;      /* over the codeword region, before repair */
    uint32_t crc32_after;       /* ditto after; equal when nothing changed */
    uint32_t unverified_columns; /* repaired columns that were DETERMINED, not
                                  * verified: exactly npar erasures consumed
                                  * every check equation. Non-zero is what
                                  * makes the return ACCUDISC_CTDB_UNVERIFIED.
                                  * A subset of repaired_columns. */
} accudisc_ctdb_report;

/* out_pcm receives pcm_bytes of repaired audio and MAY alias req->pcm, in
 * which case the repair is in place and no copy is made. Nothing is written
 * unless every dirty column decoded, so a failed call cannot leave a
 * half-repaired buffer even when aliasing.
 *
 *   ACCUDISC_OK             repaired (or already clean); out_pcm written
 *   ACCUDISC_CTDB_UNVERIFIED  repaired and out_pcm written, but
 *                           report->unverified_columns of the columns were
 *                           determined rather than verified. Positive: test
 *                           `rc >= 0` for "written", `== ACCUDISC_OK` for the
 *                           full claim.
 *   ACCUDISC_ERR_NOTFOUND   a column exceeded capacity; out_pcm untouched.
 *                           A NORMAL outcome, not an error condition —
 *                           there is no error taxonomy here because there is
 *                           nothing a caller could usefully do differently.
 *   ACCUDISC_ERR_INVAL      geometry or buffer sizes do not agree
 *   ACCUDISC_ERR_ABI        a struct `size` this build does not know
 *   ACCUDISC_ERR_NOMEM      working set (npar * stride symbols) unallocatable
 *
 * report may be NULL. */
ACCUDISC_API int accudisc_ctdb_repair(const accudisc_ctdb_req *req,
                                      uint8_t *out_pcm,
                                      accudisc_ctdb_report *report);

#ifdef __cplusplus
}
#endif

#endif /* ACCUDISC_H */
