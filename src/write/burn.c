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

/* WRITE(10) one chunk, retrying while the drive reports its buffer is full
 * ("Not Ready, long write in progress": SK 2 / ASC 04 / ASCQ 08). block_bytes
 * is 2352 for audio and 96 for the CD-Text lead-in (which transfers subchannel
 * only — the drive generates the main channel). */
static int write_chunk(struct accudisc_device *dev, int32_t lba,
                       const uint8_t *buf, uint32_t nblocks,
                       uint32_t block_bytes)
{
    for (;;) {
        int rc = adsc_mmc_write10(dev, lba, nblocks, buf, block_bytes);
        if (rc == ACCUDISC_OK)
            return ACCUDISC_OK;
        if (rc == ACCUDISC_ERR_SENSE && dev->last_sense.key == 0x02 &&
            dev->last_sense.asc == 0x04 && dev->last_sense.ascq == 0x08) {
            sleep_ms(40); /* then retry the same LBA */
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
                               const struct adsc_disc_info *di)
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
        if ((ret = write_chunk(dev, lba, xfer, n, ADSC_RW_BLOCK_BYTES)) !=
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
        return ACCUDISC_ERR_UNSUPPORTED; /* not blank */

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
        if ((ret = write_cdtext_leadin(dev, toc, &di)) != ACCUDISC_OK)
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
        if ((ret = write_chunk(dev, lba, zero, n, SECTOR)) != ACCUDISC_OK)
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
            if ((ret = write_chunk(dev, lba, chunk, n, SECTOR)) != ACCUDISC_OK)
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
    free(chunk);
    free(zero);
    return ret;
}
