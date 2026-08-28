/* DAO burn orchestration (recording engine, phase 1 slice 4).
 *
 * Ties the slices together into a Disc-At-Once audio burn: write parameters ->
 * disc-blank check -> SEND CUE SHEET -> lead-in gap + track audio (WRITE(10))
 * -> SYNCHRONIZE CACHE. Follows cdrdao's GenericMMC DAO sequence. With
 * opts->simulate the write-parameters test-write bit is set, so the drive runs
 * the whole path with the laser off (safe on any blank disc). Power
 * calibration (SEND OPC) is skipped in simulate and is a TODO for real burns.
 */

#define _POSIX_C_SOURCE 200809L
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static void sleep_ms(long ms)
{
    struct timespec ts = { ms / 1000, (ms % 1000) * 1000000L };
    nanosleep(&ts, NULL);
}

#include "../internal.h"
#include "../meta/cdtext_encode.h"
#include "../mmc/mmc.h"
#include "write.h"
#include "fifo.h"

#define SECTOR   2352u
#define CHUNK    27u        /* sectors/WRITE(10): 27*2352 = 63504 B (< 64 KiB) */
#define LEADIN_GAP 150u     /* the 2-second pre-gap before track 1 (LBA -150) */
#define CDT_CHUNK  640u     /* blocks/WRITE(10): 640*96 = 61440 B (< 64 KiB) */

/* Swap every 16-bit sample in place (audio byte-order fixup). */
static void byteswap16(uint8_t *p, size_t bytes)
{
    for (size_t i = 0; i + 1 < bytes; i += 2) {
        uint8_t t = p[i];
        p[i] = p[i + 1];
        p[i + 1] = t;
    }
}

/* Both are overridable at compile time, on the ADSC_OFFSETS_DB precedent: a
 * test cannot spend two real minutes proving the cap fires, and a cap nobody
 * has watched fire is not a cap. Defaults only — the library builds unchanged
 * and only tests/test_burn_flow.c ever defines them. */
#ifndef STALL_POLL_MS
#  define STALL_POLL_MS  40u    /* wait between retries of one chunk */
#endif

/* How long the drive may hold one chunk off before we call it stuck.
 *
 * DERIVED, not chosen. "Buffer full" is the drive telling us it has no room,
 * which resolves as it writes what it holds, so the longest LEGITIMATE hold-off
 * is the time to drain a full buffer at the slowest write speed: an 8 MiB
 * buffer (the largest we have seen; the PX-716A here reports 4802784 bytes) at
 * 1x = 176400 B/s is 47.6 s. This is 2.5x that, which is loose on purpose —
 * the cost of being too tight is aborting a burn that would have finished, and
 * the cost of being too loose is only that a genuinely wedged drive takes two
 * minutes to say so instead of one.
 *
 * The point is the BOUND, not its exact value. Until 0.24.0 this loop was
 * `for (;;)` with no cap and no count: a drive that stayed not-ready held the
 * burn forever with nothing on stdout, nothing in the log, and no way to tell
 * that from a slow burn making progress. */
#ifndef STALL_CAP_MS
#  define STALL_CAP_MS   120000u
#endif

/* Per-burn flow-control tally. NOT an underrun counter, and the distinction is
 * the whole reason this is written down.
 *
 * These count the drive telling us its buffer is FULL — i.e. the HOST is ahead
 * of the drive, which is the healthy direction. An UNDERRUN is the opposite and
 * MMC gives us no way to count it: with BURN-Proof enabled the drive simply
 * stops, repositions and resumes, reporting nothing, and there is no standard
 * command that says it happened. So a burn with many stalls is demonstrably
 * safe, while a burn with NO stalls is merely unproven — never confuse the
 * second for evidence of trouble, or the first for evidence of it. */
struct write_flow {
    uint64_t stalls;        /* retries across the whole burn */
    uint64_t stall_ms;      /* total time spent waiting for room */
    uint32_t worst_ms;      /* longest single hold-off */
    int32_t  worst_lba;     /* ... and where. A duration with no position
                             * cannot tell "the drive settled once at the
                             * start" from "it struggled throughout". */
    uint32_t stalling_chunks; /* DISTINCT chunks that had to wait at all —
                               * the number `stalls` cannot give, because 326
                               * retries of ONE chunk and 326 chunks waiting
                               * once each are the same figure. */

    /* Drive-buffer fill, from READ BUFFER CAPACITY (0x5C). `live` is 0 until
     * the first successful poll and goes back to 0 permanently on the first
     * refusal — the command is conditional on the Real-time Streaming feature
     * being CURRENT, so a drive may simply not answer, and that must read as
     * UNKNOWN rather than as an empty buffer. */
    int      cap_live;
    uint32_t cap_total;     /* buffer size in bytes */
    uint32_t min_fill_pct;  /* lowest fill seen, 0-100 */
    uint32_t polls;
    uint32_t low_samples;   /* polls under ADSC_BUF_LOW_PCT */

    /* Did the reported buffer ever CHANGE? A device that answers the command
     * with a constant is not measuring anything, and the constant it picks may
     * be "entirely free" — which reads as a drive on the point of underrunning
     * for the whole burn. Measured on CDEmu 2026-08-27: total=131584
     * blank=131584 on every one of 11 polls, bit-identical, through a burn that
     * completed cleanly. Without this the report called that "minimum fill 0%,
     * 11 below 25%", which is a confident description of a catastrophe that did
     * not happen. A real buffer cannot hold one value across a whole burn. */
    int      cap_varied;
    uint32_t first_total, first_blank;

    /* Whether anything is behind the host if it falls behind. Recorded rather
     * than only acted on, because the same burn log has to explain WHY an
     * underrun was survived or was fatal. */
    uint8_t  burnproof_claimed;   /* the drive's CD Mastering BUF bit */
    uint8_t  burnproof_on;        /* what we actually asked MODE SELECT for */
    uint64_t fifo_starved;        /* times the ring was dry when the drive
                                   * wanted data — the HOST-side twin of the
                                   * drive-buffer low-water mark */
};

/* Below this the drive is close to running dry. Not a threshold anything acts
 * on — nothing here can prevent an underrun — only the boundary the report
 * counts, so "it got low N times" is a number rather than an impression. */
#define ADSC_BUF_LOW_PCT 25u

/* Poll every Nth chunk, not every chunk. The poll is a command on the same bus
 * that carries the data, so at ~88 chunks/s polling each one roughly doubles
 * the command rate on a USB link — measuring the thing hard enough to cause it.
 * Every 8th costs ~11 polls/s and still samples far faster than a buffer of a
 * few MB can drain. */
#define ADSC_BUF_POLL_EVERY 8u

/* Sample the drive's buffer fill. CALLED BEFORE THE WRITE, deliberately.
 *
 * The buffer is at its FULLEST just after a WRITE(10) returns and at its
 * emptiest just before the next one, after the host has done its pread and
 * whatever else. Sampling after the write would measure the peak and report a
 * comfortable minimum on a burn that was actually starving.
 *
 * STATE PLAINLY WHAT THIS IS: the true minimum lives INSIDE the WRITE transfer
 * or inside a long pread stall, and cannot be sampled from this thread. So
 * min_fill_pct is an UPPER BOUND on the true minimum — optimistic. It is not a
 * guarantee that the buffer never went lower, and must never be read as one. */
static void buf_sample(struct accudisc_device *dev, struct write_flow *fl)
{
    uint32_t total, blank, fill_pct;

    if (!fl->cap_live)
        return;
    if (adsc_mmc_read_buffer_capacity(dev, &total, &blank) != ACCUDISC_OK) {
        /* One refusal disarms it for the rest of the burn. Retrying every
         * chunk on a drive that does not implement this would spend the whole
         * burn issuing a command that will never work. */
        fl->cap_live = 0;
        adsc_dev_log(dev, "write: drive stopped answering READ BUFFER CAPACITY;"
                          " fill is UNKNOWN from here, not zero");
        return;
    }
    if (fl->polls == 0) {
        fl->first_total = total;
        fl->first_blank = blank;
    } else if (total != fl->first_total || blank != fl->first_blank) {
        fl->cap_varied = 1;
    }
    fl->cap_total = total;
    fill_pct = (uint32_t)(((uint64_t)(total - blank) * 100u) / total);
    if (fill_pct < fl->min_fill_pct)
        fl->min_fill_pct = fill_pct;
    if (fill_pct < ADSC_BUF_LOW_PCT)
        fl->low_samples++;
    fl->polls++;
}

/* WRITE(10) one chunk, retrying while the drive reports its buffer is full
 * ("Not Ready, long write in progress": SK 2 / ASC 04 / ASCQ 08). block_bytes
 * is 2352 for audio and 96 for the CD-Text lead-in (which transfers subchannel
 * only — the drive generates the main channel).
 *
 * On giving up it returns ACCUDISC_ERR_SENSE with dev->last_sense still holding
 * the drive's own 2/04/08, rather than inventing an error code: the sense IS
 * the explanation, a caller that inspects it learns more than a new enumerator
 * would tell it, and adding a code to the public enum is an ABI event this does
 * not need. The log line says we stopped waiting. */
static int write_chunk(struct accudisc_device *dev, int32_t lba,
                       const uint8_t *buf, uint32_t nblocks,
                       uint32_t block_bytes, struct write_flow *fl)
{
    uint32_t waited_ms = 0;

    for (;;) {
        int rc = adsc_mmc_write10(dev, lba, nblocks, buf, block_bytes);
        if (rc == ACCUDISC_OK) {
            if (waited_ms)
                fl->stalling_chunks++;
            if (waited_ms > fl->worst_ms) {
                fl->worst_ms = waited_ms;
                fl->worst_lba = lba;
            }
            return ACCUDISC_OK;
        }
        if (rc == ACCUDISC_ERR_SENSE && dev->last_sense.key == 0x02 &&
            dev->last_sense.asc == 0x04 && dev->last_sense.ascq == 0x08) {
            if (waited_ms >= STALL_CAP_MS) {
                adsc_dev_log(dev, "write: drive still reports its buffer full "
                                  "%u s after LBA %d; giving up (sense 2/04/08)",
                             waited_ms / 1000u, lba);
                return rc;   /* dev->last_sense still describes it */
            }
            fl->stalls++;
            fl->stall_ms += STALL_POLL_MS;
            waited_ms += STALL_POLL_MS;
            sleep_ms(STALL_POLL_MS); /* then retry the same LBA */
            continue;
        }
        return rc;
    }
}

/* Write the CD-Text lead-in: encode the blob's packs into R-W blocks (B3) and
 * cycle that set across the whole lead-in extent, 96 bytes per sector, ending
 * just before the -150 gap. Mirrors cdrdao GenericMMC::writeCdTextLeadIn.
 *
 * The extent comes from the media (di->leadin_len), not from the pack count —
 * so any pack count fills it, and the ring simply wraps as often as needed. */
static int write_cdtext_leadin(struct accudisc_device *dev,
                               const struct adsc_write_toc *toc,
                               const struct adsc_disc_info *di,
                               struct write_flow *fl)
{
    uint32_t npacks = (toc->cdtext_len - 4u) / ADSC_CDTEXT_PACK_BYTES;
    uint32_t nblocks = adsc_cdtext_rw_block_count(npacks);
    uint8_t *blocks = NULL, *xfer = NULL;
    int ret;

    if (npacks == 0 || nblocks == 0 || di->leadin_len == 0)
        return ACCUDISC_ERR_INVAL;

    blocks = malloc((size_t)nblocks * ADSC_RW_BLOCK_BYTES);
    xfer = malloc((size_t)CDT_CHUNK * ADSC_RW_BLOCK_BYTES);
    if (!blocks || !xfer) {
        ret = ACCUDISC_ERR_NOMEM;
        goto out;
    }
    ret = adsc_cdtext_encode_rw(toc->cdtext + 4, npacks, blocks);
    if (ret != ACCUDISC_OK)
        goto out;

    adsc_dev_log(dev, "cdtext: %u packs -> %u R-W blocks, filling %u lead-in "
                      "sectors from LBA %d",
                 npacks, nblocks, di->leadin_len,
                 -(int)LEADIN_GAP - (int)di->leadin_len);

    int32_t lba = -(int32_t)LEADIN_GAP - (int32_t)di->leadin_len;
    uint32_t scp = 0;
    for (uint32_t left = di->leadin_len; left > 0;) {
        uint32_t n = left < CDT_CHUNK ? left : CDT_CHUNK;

        for (uint32_t i = 0; i < n; i++) {
            memcpy(xfer + (size_t)i * ADSC_RW_BLOCK_BYTES,
                   blocks + (size_t)scp * ADSC_RW_BLOCK_BYTES,
                   ADSC_RW_BLOCK_BYTES);
            if (++scp >= nblocks)
                scp = 0;
        }
        if ((ret = write_chunk(dev, lba, xfer, n, ADSC_RW_BLOCK_BYTES, fl)) !=
            ACCUDISC_OK)
            goto out;
        lba += (int32_t)n;
        left -= n;
    }
    ret = ACCUDISC_OK;

out:
    free(blocks);
    free(xfer);
    return ret;
}

/* Ask the drive to WRITE at speed_x, and report what it then says it is set to.
 *
 * Until 0.29.0 nothing here existed: adsc_burn_opts.speed was carried all the
 * way from the CLI and then read by nobody, so `--speed 4` constrained nothing
 * and the drive wrote at whatever it liked (measured 19.4x on a PX-716A while
 * 4x was requested). Worse than the speed being wrong: the FIFO is sized in
 * SECONDS against that number, so a ring reported as 5 s of protection was
 * 1.02 s of it.
 *
 * The command is SET CD SPEED (0xBB) with CLV, not SET STREAMING. cdrecord
 * reaches for SET STREAMING only on DVD (scsi_mmc.c speed_select_mdvd); every
 * CD write goes through 0xBB (drv_mmc.c speed_select_mmc). Credited in
 * docs/reference/ATTRIBUTION.md.
 *
 * The climb is cdrecord's too (drv_mmc.c mmc_set_speed): a drive whose minimum
 * write speed is above the request answers ILLEGAL REQUEST / ASC 0x24 rather
 * than rounding up, so step by one 1x rung and retry. ONLY that sense climbs —
 * any other failure is real and climbing would bury it.
 *
 * *out_kbps is what mode page 2A reports afterwards, which is the drive
 * ACCEPTING the request and is NOT evidence about the medium. Elapsed time is
 * the only instrument for a delivered rate. */
#define SPEED_CLIMB_MAX 8

static int set_write_speed(struct accudisc_device *dev, unsigned speed_x,
                           unsigned *out_kbps)
{
    unsigned want = adsc_cd_speed_kbps(speed_x);
    unsigned kbps = want;
    unsigned read_kbps = 0xFFFFu, cur = 0;
    int climbed = 0;
    int rc;

    /* Preserve the READ speed. 0xFFFF in that field is not "leave alone", it
     * is "use your maximum" — so passing it would silently retune the drive's
     * read speed as a side effect of a burn, and the setting persists after we
     * exit. cdrecord reads it back for the same reason (mmc_set_speed). */
    if (accudisc_get_speed(dev, NULL, &cur) == ACCUDISC_OK && cur > 0)
        read_kbps = cur;

    for (;;) {
        rc = adsc_mmc_set_cd_speed(dev, (uint16_t)read_kbps, (uint16_t)kbps,
                                   ADSC_ROTCTL_CLV);
        if (rc == ACCUDISC_OK)
            break;
        if (!(rc == ACCUDISC_ERR_SENSE && dev->last_sense.key == 0x05 &&
              dev->last_sense.asc == 0x24))
            return rc;
        if (climbed >= SPEED_CLIMB_MAX || kbps + 177u > 0xFFFFu)
            return rc;
        kbps += 177u;
        climbed++;
    }
    if (climbed)
        adsc_dev_log(dev, "write: speed — the drive refused %u kB/s (%ux) with "
                          "INVALID FIELD IN CDB and accepted %u kB/s after %d "
                          "step(s); it cannot write that slowly",
                     want, speed_x, kbps, climbed);

    if (adsc_write_cur_write_kbps(dev, &cur) == ACCUDISC_OK && cur > 0)
        *out_kbps = cur;
    return ACCUDISC_OK;
}

int adsc_write_run(struct accudisc_device *dev,
                   const struct adsc_write_toc *toc, int bin_fd,
                   const struct adsc_burn_opts *opts,
                   adsc_burn_progress cb, void *user)
{
    uint8_t cue[ADSC_CUE_MAX_BYTES];
    uint32_t cuelen = 0;
    uint8_t *chunk = NULL, *zero = NULL;
    struct adsc_wfifo fifo;
    struct adsc_wfifo_seg seg[99];
    int fifo_live = 0;
    struct write_flow fl = {0};
    int session_open = 0;   /* the DRIVE holds a DAO session: must be released */
    int burn_started = 0;   /* ... and we got far enough for the tally to mean
                             * something. Distinct from session_open, which is
                             * cleared by a clean close. */
    unsigned speed_kbps = 0; /* what the drive says its write speed is; 0 =
                              * it would not say, so any duration computed from
                              * it must be labelled as assumed rather than
                              * printed as fact. */
    int ret;

    if (!dev || !toc || !opts || bin_fd < 0)
        return ACCUDISC_ERR_INVAL;

    /* A blob shorter than a header + one pack cannot be written; intake
     * validation (adsc_cdtext_blob_validate) already refuses those, so this is
     * the belt to that braces for callers reaching adsc_write_run directly. */
    int have_cdtext = (toc->cdtext && toc->cdtext_len >= 4u + 18u) ? 1 : 0;

    /* 1. Program DAO write parameters (test-write in simulate). CD-Text needs
     * data block type 3 (raw + P-W, 2448) to enable P-W lead-in writing. */
    struct adsc_write_params wp = {0};
    wp.simulate = opts->simulate;
    wp.cdtext = have_cdtext;

    /* BURN-PROOF, AND WHETHER A FAILOVER EXISTS AT ALL.
     *
     * Until 0.26.0 this was `wp.burnproof = 1`, unconditionally — asking every
     * drive for a feature many do not have, and leaving the engine unable to
     * answer the one question that decides what to do when the host cannot
     * keep up: is there something behind us?
     *
     * With a failover, an underrun costs a link and the burn continues. With
     * none, it costs the disc. Those want opposite responses, so the engine has
     * to KNOW which it is rather than hope.
     *
     * The claim is taken on faith, deliberately and as an exception to this
     * project's usual gate order. Everywhere else a feature bit is smoke-tested
     * because firmware lies; BUF cannot be, because the test is to starve a
     * real burn and inspect the disc — one blank per drive, destroyed. So it is
     * acted on, reported as CLAIMED rather than verified, and never printed as
     * a fact we established. */
    {
        accudisc_features caps;
        int claimed;

        memset(&caps, 0, sizeof caps);
        claimed = (adsc_probe_cd_mastering(dev, &caps) == 0) && caps.buf_claimed;
        fl.burnproof_claimed = (uint8_t)claimed;

        switch (opts->burnproof) {
        case ACCUDISC_BURNPROOF_OFF: wp.burnproof = 0; break;
        case ACCUDISC_BURNPROOF_ON:  wp.burnproof = 1; break;
        default:                     wp.burnproof = claimed; break;
        }
        fl.burnproof_on = (uint8_t)wp.burnproof;

        if (wp.burnproof && !claimed)
            adsc_dev_log(dev, "write: BURN-Proof FORCED on a drive that does not "
                              "claim it (CD Mastering BUF=0 or absent); MODE "
                              "SELECT may simply refuse");
        else if (wp.burnproof)
            adsc_dev_log(dev, "write: BURN-Proof enabled — CLAIMED by the drive, "
                              "not verified here");
        else
            adsc_dev_log(dev, "write: BURN-Proof OFF (%s) — there is no failover "
                              "behind the host",
                         claimed ? "disabled by the caller"
                                 : "not claimed by the drive");
    }
    if ((ret = adsc_write_set_params(dev, &wp)) != ACCUDISC_OK)
        goto done;

    /* 1b. WRITE SPEED. After the write parameters and before anything touches
     * the disc, matching cdrecord's order (it programs page 0x05 first, then
     * sets the speed, in the same function).
     *
     * A failure here is NOT fatal: the burn can proceed at the drive's own
     * rate, and refusing would turn "you did not get the speed you asked for"
     * into "you got no disc". It is said out loud instead, because the FIFO's
     * seconds-of-protection are computed against the rate. */
    if (opts->speed > 0) {
        int srate = set_write_speed(dev, (unsigned)opts->speed, &speed_kbps);
        if (srate != ACCUDISC_OK)
            adsc_dev_log(dev, "write: speed — SET CD SPEED for %ux FAILED "
                              "(rc %d); the drive keeps its own write speed and "
                              "the FIFO below is sized for a rate we did not get",
                         (unsigned)opts->speed, srate);
    } else {
        /* Not setting it is not a reason to stay ignorant of it: the FIFO's
         * duration is meaningless without a rate either way. */
        (void)adsc_write_cur_write_kbps(dev, &speed_kbps);
    }
    if (speed_kbps > 0)
        adsc_dev_log(dev, "write: speed — drive reports %u kB/s (%.1fx)%s. "
                          "PAGE 2A ECHOES THE REQUEST: this is the drive "
                          "accepting, not the medium delivering — only elapsed "
                          "time measures that",
                     speed_kbps, (double)speed_kbps / 176.4,
                     opts->speed > 0 ? "" : ", which we did not set");
    else
        adsc_dev_log(dev, "write: speed — the drive would not report a write "
                          "speed; the FIFO duration below is against an ASSUMED "
                          "rate");

    /* 2. Refuse anything but a blank disc. */
    struct adsc_disc_info di;
    if ((ret = adsc_write_read_disc_info(dev, &di)) != ACCUDISC_OK)
        goto done;
    if (di.status != 0) {
        ret = ACCUDISC_ERR_NOT_BLANK;
        goto done;
    }

    /* 3. Power calibration for a real burn (fires the laser at the PCA).
     * Skipped in simulate. A drive that reports "invalid command" (SK 5 /
     * ASC 0x20) simply doesn't need it — proceed. */
    if (!opts->simulate) {
        ret = adsc_mmc_send_opc(dev);
        if (ret == ACCUDISC_ERR_SENSE && dev->last_sense.key == 0x05 &&
            dev->last_sense.asc == 0x20)
            ret = ACCUDISC_OK;
        if (ret != ACCUDISC_OK)
            goto done;
    }

    /* 4. SEND CUE SHEET — the whole-disc DAO layout. With CD-Text this also
     * declares the lead-in as carrying P-W (data form 0x41) at the media's
     * lead-in start MSF, so di must be read before it. */
    if ((ret = adsc_cuesheet_build(toc, &di, cue, sizeof cue, &cuelen)) !=
        ACCUDISC_OK)
        goto done;
    if ((ret = adsc_mmc_send_cue_sheet(dev, cue, cuelen)) != ACCUDISC_OK)
        goto done;
    /* From here the DRIVE holds an open DAO session and is waiting for the
     * rest of the data. Every exit below must reach `done:` so the session
     * is aborted; a bare return leaves the drive live and refusing almost
     * everything (measured 2026-08-28: READ DISC INFORMATION and READ ATIP
     * both answer 5/2C/00 COMMAND SEQUENCE ERROR until it is released). */
    session_open = 1;
    burn_started = 1;

    /* 4b. CD-Text lead-in, written BEFORE the gap: it occupies the lead-in
     * extent immediately preceding LBA -150 (cdrdao order: cue sheet ->
     * writeCdTextLeadIn -> gap -> audio). */
    if (have_cdtext) {
        if ((ret = write_cdtext_leadin(dev, toc, &di, &fl)) != ACCUDISC_OK)
            goto done;
    }

    chunk = malloc(CHUNK * SECTOR);
    zero = calloc(CHUNK, SECTOR);
    if (!chunk || !zero) {
        ret = ACCUDISC_ERR_NOMEM;
        goto done;
    }

    /* Arm the buffer poll with ONE probe. Gate order as everywhere else here:
     * try the opcode, believe the result, never the feature bit. MMC-5 6.18
     * makes this conditional on Real-time Streaming being CURRENT and says the
     * blank field is UNDEFINED when the feature is present but not current — so
     * an advertisement is not evidence and a smoke test is. */
    {
        uint32_t t, b;

        fl.min_fill_pct = 100;
        if (adsc_mmc_read_buffer_capacity(dev, &t, &b) == ACCUDISC_OK) {
            fl.cap_live = 1;
            fl.cap_total = t;
            adsc_dev_log(dev, "write: drive buffer %u bytes (%.1f chunks of %u)",
                         t, (double)t / (CHUNK * SECTOR), CHUNK * SECTOR);
        } else {
            adsc_dev_log(dev, "write: no READ BUFFER CAPACITY; buffer fill will "
                              "be reported as unknown");
        }
    }

    /* 5. Lead-in gap: LEADIN_GAP zero sectors starting at LBA -150. */
    int32_t lba = -(int32_t)LEADIN_GAP;
    for (uint32_t left = LEADIN_GAP; left > 0;) {
        uint32_t n = left < CHUNK ? left : CHUNK;
        if ((ret = write_chunk(dev, lba, zero, n, SECTOR, &fl)) != ACCUDISC_OK)
            goto done;
        lba += (int32_t)n;
        left -= n;
    }

    /* 6. Track audio, contiguous from LBA 0 — fed through the FIFO. */
    uint32_t total = toc->leadout_lba, done_sec = 0;
    for (int i = 0; i < toc->ntracks; i++) {
        seg[i].file_offset = toc->track[i].file_offset;
        seg[i].sectors     = toc->track[i].sectors;
    }
    if (opts->fifo_bytes) {
        ret = adsc_wfifo_start(&fifo, opts->fifo_bytes, CHUNK, bin_fd,
                               seg, (unsigned)toc->ntracks, opts->byteswap);
        if (ret != ACCUDISC_OK) {
            /* A caller that asked for a buffer and quietly got the synchronous
             * path would be told nothing and protected by nothing. */
            adsc_dev_log(dev, "write: FIFO could not start (rc %d) — refusing "
                              "rather than burning unprotected", ret);
            goto done;
        }
        fifo_live = 1;
        adsc_dev_log(dev, "write: FIFO %zu bytes in %u slots of %u sectors, "
                          "memory %s",
                     fifo.arena_bytes, fifo.nslots, CHUNK,
                     fifo.locked ? "LOCKED"
                                 : "NOT locked (mlock refused; it can be paged "
                                   "out under the pressure it exists to absorb)");
        /* THE RING IN SECONDS, AGAINST THE RATE THE DRIVE ADMITS TO.
         *
         * The caller sized this in seconds against the speed it REQUESTED. If
         * the drive is at a different rate the byte count is unchanged and the
         * protection is not, and that discrepancy is exactly what went unseen
         * before 0.29.0: 3492720 bytes reported as "5.0 s at 4x" was 1.02 s,
         * because the request never reached the drive. Page 2A is in kB/s with
         * k = 1000, so 1x reads as 176 against a true 176400 B/s — a 0.23%
         * understatement of the rate, which we let stand rather than
         * second-guess the drive's own units. */
        if (speed_kbps > 0) {
            unsigned got_x = (speed_kbps * 10u + 882u) / 1764u;
            double secs = (double)fifo.arena_bytes / ((double)speed_kbps * 1000.0);

            adsc_dev_log(dev, "write: FIFO = %.2f s of ride-through at the "
                              "drive's stated %.1fx%s",
                         secs, (double)speed_kbps / 176.4,
                         (opts->speed > 0 && got_x != (unsigned)opts->speed)
                             ? " — NOT the duration the requested speed implies;"
                               " the ring was sized for a rate the drive is not at"
                             : "");
        } else {
            adsc_dev_log(dev, "write: FIFO duration UNKNOWN — the drive would "
                              "not state a write speed, so the seconds this "
                              "ring buys cannot be computed");
        }
        /* Fill it BEFORE the first write. Without this the opening pop reads
         * an empty ring and scores a starvation, which on a drive with no
         * failover stops the burn at sector 0 — every time. And a burn that
         * begins with a full ring is protected from its first sector rather
         * than some seconds in, which is the point. */
        {
            unsigned filled = 0;
            ret = adsc_wfifo_prefill(&fifo, &filled);
            if (ret != ACCUDISC_OK)
                goto done;
            adsc_dev_log(dev, "write: FIFO primed %u/%u slots before the first "
                              "sector", filled, fifo.nslots);
        }
    }

    for (;;) {
        const uint8_t *src;
        uint32_t n;
        int was_empty = 0, got;

        if (fifo_live) {
            got = adsc_wfifo_pop(&fifo, &src, &was_empty);
            if (got < 0) { ret = got; goto done; }
            if (got == 0) break;                    /* end of stream */
            n = (uint32_t)got;
        } else {
            /* Unbuffered path, kept working and kept honest: --no-fifo means
             * exactly the old synchronous behaviour, not a smaller ring. */
            if (done_sec >= total) break;
            n = total - done_sec < CHUNK ? total - done_sec : CHUNK;
            {
                uint64_t off = 0; uint32_t acc = 0; int t;
                for (t = 0; t < toc->ntracks; t++) {
                    if (done_sec < acc + toc->track[t].sectors) {
                        off = toc->track[t].file_offset
                            + (uint64_t)(done_sec - acc) * SECTOR;
                        if (n > acc + toc->track[t].sectors - done_sec)
                            n = acc + toc->track[t].sectors - done_sec;
                        break;
                    }
                    acc += toc->track[t].sectors;
                }
                if (pread(bin_fd, chunk, (size_t)n * SECTOR, (off_t)off)
                        != (ssize_t)((size_t)n * SECTOR)) {
                    ret = ACCUDISC_ERR_IO;
                    goto done;
                }
                if (opts->byteswap)
                    byteswap16(chunk, (size_t)n * SECTOR);
            }
            src = chunk;
        }

        /* THE UNDERRUN POLICY. The ring ran dry: the host lost. What that
         * costs depends entirely on whether anything is behind us.
         *
         * With BURN-Proof the drive stops, repositions and resumes — a link in
         * the stream, and the burn survives. Without it the drive runs its own
         * buffer out and the disc is lost. We own the pipeline, so we stop and
         * say so rather than feed a drive we know cannot recover.
         *
         * BE HONEST ABOUT WHAT STOPPING BUYS: the disc is already spoilt by the
         * time this fires. Stopping does not save it. What it saves is the
         * DIAGNOSIS — a clean, attributed failure instead of a disc that reads
         * as fine until something downstream disagrees, and the minutes that
         * would be spent finishing a burn already known to be bad. */
        if (was_empty) {
            fl.fifo_starved++;
            if (!fl.burnproof_on) {
                adsc_dev_log(dev, "write: FIFO EMPTY at sector %u and there is "
                                  "no failover behind it — stopping. The disc "
                                  "is already spoilt; continuing would only "
                                  "hide that.", done_sec);
                ret = ACCUDISC_ERR_IO;
                goto done;
            }
            adsc_dev_log(dev, "write: FIFO empty at sector %u — deferring to "
                              "BURN-Proof, which links and resumes", done_sec);
        }

        if (done_sec / CHUNK % ADSC_BUF_POLL_EVERY == 0)
            buf_sample(dev, &fl);
        if ((ret = write_chunk(dev, lba, src, n, SECTOR, &fl)) != ACCUDISC_OK)
            goto done;
        if (fifo_live)
            adsc_wfifo_release(&fifo);
        lba += (int32_t)n;
        done_sec += n;
        if (cb)
            cb(user, done_sec, total);
    }

    /* 7. Flush / close. */
    ret = adsc_mmc_sync_cache(dev);
    if (ret == ACCUDISC_OK)
        session_open = 0;

done:
    /* ABORT THE SESSION IN THE DRIVE BEFORE ANYTHING ELSE.
     *
     * Returning an error is not enough: the drive is mid-DAO and waiting for
     * data it will never get. Until 0.28.0 we simply exited, and the drive
     * stayed live -- TEST UNIT READY still passed, so it did not look wedged,
     * but READ DISC INFORMATION and READ ATIP answered 5/2C/00 COMMAND
     * SEQUENCE ERROR and a tray cycle did not clear it. Measured on a
     * PX-716A after a deliberate FIFO starvation, 2026-08-28.
     *
     * FLUSH CACHE (0x35) is the whole abort, and it is what both references
     * do: cdrdao GenericMMC::abortDao() is a bare flushCache(), and
     * cdrecord's generic-MMC cdr_abort_session slot is cmd_dummy (a no-op),
     * leaving scsi_flush_cache() as its entire abort path. Verified here: one
     * 0x35 and READ DISC INFORMATION/READ ATIP answered normally again.
     *
     * The result is deliberately IGNORED, as in both references -- the burn
     * has already failed and its error is the one worth returning. What we do
     * NOT do is claim the drive recovered: we sent the release, we did not
     * verify it took. */
    if (session_open) {
        (void)adsc_mmc_sync_cache(dev);
        adsc_dev_log(dev, "write: session aborted in the drive (FLUSH CACHE) "
                          "-- without this it stays mid-DAO and refuses "
                          "READ DISC INFORMATION until released");
    }
    if (fifo_live) {
        int frc = adsc_wfifo_stop(&fifo, ret != ACCUDISC_OK);
        if (ret == ACCUDISC_OK && frc != ACCUDISC_OK)
            ret = frc;
        adsc_dev_log(dev, "write: FIFO — %llu starvations, low-water %u/%u "
                          "slots, producer waited %llu times",
                     (unsigned long long)fl.fifo_starved,
                     fifo.min_count, fifo.nslots,
                     (unsigned long long)fifo.producer_waits);
    }
    /* Nothing below describes a burn that never started: a refused blank or a
     * rejected cue sheet would otherwise print "0 chunks held off" and
     * "buffer fill UNKNOWN", which read as findings about a burn rather than
     * as the absence of one. */
    if (!burn_started)
        goto cleanup;
    /* The tally, logged whatever the outcome — a failed burn is exactly when
     * knowing how the flow behaved is worth most. Read it as described at
     * struct write_flow: stalls are the drive holding US off, which is the safe
     * direction. Zero stalls does NOT mean an underrun occurred; it means the
     * host never got ahead, and nothing here can see the other side of that. */
    /* REPORT THE NUMBER OF EVENTS, NOT THE NUMBER OF RETRIES.
     *
     * `stalls` counts 40 ms waits, and until 0.28.0 it was the headline. On
     * this drive every burn reported "326 buffer-full stalls, worst hold-off
     * 13040 ms", which reads as sustained flow-control pressure with one bad
     * moment. Measured 2026-08-28: it is ONE moment. 329 retries of a SINGLE
     * WRITE(10) at LBA -150 — the first write of the burn — which the drive
     * would not accept for 13.2 s while it prepared to record. Normal, safe,
     * and nothing like what the line said.
     *
     * The danger of the old wording is not that it was alarming. It is that a
     * burn with 326 GENUINE stalls spread across the disc printed the same
     * headline as this one, so the report could not distinguish the case worth
     * acting on from the case that happens every time. */
    if (fl.stalling_chunks == 1)
        adsc_dev_log(dev, "write: flow — ONE hold-off, %u ms at LBA %d "
                          "(%llu retries of a single write, not %llu events). "
                          "A drive settling before its first write looks like "
                          "this.",
                     fl.worst_ms, fl.worst_lba,
                     (unsigned long long)fl.stalls,
                     (unsigned long long)fl.stalls);
    else
        adsc_dev_log(dev, "write: flow — %u chunks held off, %llu ms total, "
                          "worst %u ms at LBA %d (%llu retries)",
                     fl.stalling_chunks, (unsigned long long)fl.stall_ms,
                     fl.worst_ms, fl.worst_lba,
                     (unsigned long long)fl.stalls);
    if (fl.polls > 1 && !fl.cap_varied)
        /* The device answered, and answered the SAME thing every time. That is
         * not a fill measurement, whatever number it contains — so no number is
         * reported. Saying "0%" here would describe a clean burn as a drive
         * that never had a byte in hand. */
        adsc_dev_log(dev, "write: drive buffer fill UNRELIABLE — %u polls all "
                          "returned an identical %u/%u, so the drive is not "
                          "reporting a live value. No fill figure given.",
                     fl.polls, fl.first_blank, fl.first_total);
    else if (fl.polls)
        adsc_dev_log(dev, "write: drive buffer — %u polls, minimum fill %u%% "
                          "(upper bound), %u below %u%%, capacity %u bytes",
                     fl.polls, fl.min_fill_pct, fl.low_samples,
                     ADSC_BUF_LOW_PCT, fl.cap_total);
    else
        adsc_dev_log(dev, "write: drive buffer fill UNKNOWN — the drive does not "
                          "report it. This is not a reading of zero.");
cleanup:
    free(chunk);
    free(zero);
    return ret;
}
