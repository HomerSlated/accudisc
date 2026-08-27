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
};

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
            if (waited_ms > fl->worst_ms)
                fl->worst_ms = waited_ms;
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

int adsc_write_run(struct accudisc_device *dev,
                   const struct adsc_write_toc *toc, int bin_fd,
                   const struct adsc_burn_opts *opts,
                   adsc_burn_progress cb, void *user)
{
    uint8_t cue[ADSC_CUE_MAX_BYTES];
    uint32_t cuelen = 0;
    uint8_t *chunk = NULL, *zero = NULL;
    struct write_flow fl = {0};
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
    wp.burnproof = 1;
    wp.cdtext = have_cdtext;
    if ((ret = adsc_write_set_params(dev, &wp)) != ACCUDISC_OK)
        return ret;

    /* 2. Refuse anything but a blank disc. */
    struct adsc_disc_info di;
    if ((ret = adsc_write_read_disc_info(dev, &di)) != ACCUDISC_OK)
        return ret;
    if (di.status != 0)
        return ACCUDISC_ERR_NOT_BLANK;

    /* 3. Power calibration for a real burn (fires the laser at the PCA).
     * Skipped in simulate. A drive that reports "invalid command" (SK 5 /
     * ASC 0x20) simply doesn't need it — proceed. */
    if (!opts->simulate) {
        ret = adsc_mmc_send_opc(dev);
        if (ret == ACCUDISC_ERR_SENSE && dev->last_sense.key == 0x05 &&
            dev->last_sense.asc == 0x20)
            ret = ACCUDISC_OK;
        if (ret != ACCUDISC_OK)
            return ret;
    }

    /* 4. SEND CUE SHEET — the whole-disc DAO layout. With CD-Text this also
     * declares the lead-in as carrying P-W (data form 0x41) at the media's
     * lead-in start MSF, so di must be read before it. */
    if ((ret = adsc_cuesheet_build(toc, &di, cue, sizeof cue, &cuelen)) !=
        ACCUDISC_OK)
        return ret;
    if ((ret = adsc_mmc_send_cue_sheet(dev, cue, cuelen)) != ACCUDISC_OK)
        return ret;

    /* 4b. CD-Text lead-in, written BEFORE the gap: it occupies the lead-in
     * extent immediately preceding LBA -150 (cdrdao order: cue sheet ->
     * writeCdTextLeadIn -> gap -> audio). */
    if (have_cdtext) {
        if ((ret = write_cdtext_leadin(dev, toc, &di, &fl)) != ACCUDISC_OK)
            return ret;
    }

    chunk = malloc(CHUNK * SECTOR);
    zero = calloc(CHUNK, SECTOR);
    if (!chunk || !zero) {
        ret = ACCUDISC_ERR_NOMEM;
        goto done;
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

    /* 6. Track audio, contiguous from LBA 0. */
    uint32_t total = toc->leadout_lba, done_sec = 0;
    for (int i = 0; i < toc->ntracks; i++) {
        const struct adsc_write_track *t = &toc->track[i];
        uint64_t off = t->file_offset;
        for (uint32_t rem = t->sectors; rem > 0;) {
            uint32_t n = rem < CHUNK ? rem : CHUNK;
            ssize_t got = pread(bin_fd, chunk, (size_t)n * SECTOR, (off_t)off);
            if (got != (ssize_t)(n * SECTOR)) {
                ret = ACCUDISC_ERR_IO;
                goto done;
            }
            if (opts->byteswap)
                byteswap16(chunk, (size_t)n * SECTOR);
            if ((ret = write_chunk(dev, lba, chunk, n, SECTOR, &fl)) != ACCUDISC_OK)
                goto done;
            lba += (int32_t)n;
            off += (uint64_t)n * SECTOR;
            rem -= n;
            done_sec += n;
            if (cb)
                cb(user, done_sec, total);
        }
    }

    /* 7. Flush / close. */
    ret = adsc_mmc_sync_cache(dev);

done:
    /* The tally, logged whatever the outcome — a failed burn is exactly when
     * knowing how the flow behaved is worth most. Read it as described at
     * struct write_flow: stalls are the drive holding US off, which is the safe
     * direction. Zero stalls does NOT mean an underrun occurred; it means the
     * host never got ahead, and nothing here can see the other side of that. */
    adsc_dev_log(dev, "write: flow — %llu buffer-full stalls, %llu ms waited, "
                      "worst single hold-off %u ms",
                 (unsigned long long)fl.stalls,
                 (unsigned long long)fl.stall_ms, fl.worst_ms);
    free(chunk);
    free(zero);
    return ret;
}
