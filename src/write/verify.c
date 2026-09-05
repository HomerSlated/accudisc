/* verify.c — post-burn verification in three tiers.
 *
 * The contract, the tier meanings and the alignment policy are documented on
 * accudisc_verify in the public header; this file implements them and records
 * only what the header cannot: why the code is shaped the way it is.
 *
 * TWO PASSES, NOT ONE. Tiers 0/1 come from a compare read; tier 2 from a
 * second read with the vendor counters armed. Merging them would mean
 * reproducing accudisc_counter_census's counter lifetime — arm, sample,
 * disarm on every error path — inside the compare loop, duplicating the one
 * part of this that is already proven. Two passes cost time; a second
 * implementation of the counter lifetime costs correctness.
 *
 * ALIGNMENT IS MEASURED. The equation is disc[i] == source[i + shift], with
 * `shift` in samples (one stereo frame, 4 bytes, 588 per sector) and positive
 * meaning the read-back runs EARLY — the project's read-offset sign
 * convention. Nothing here shifts the caller's data; see the offsets note in
 * the header for why applying would be wrong.
 */
/* _FILE_OFFSET_BITS before any header, so off_t is 64-bit where the platform
 * offers the choice. A CD image is ~747 MB, which fits a 32-bit long only just;
 * a longer source (a 99-minute disc, or a caller pointing at a whole image)
 * does not. Byte offsets here are off_t and seeks are fseeko for the same
 * reason accudisc_write_opts.fifo_bytes is uint32_t and not size_t: a struct
 * or an offset whose width changes with the target changes BEHAVIOUR by
 * platform, and the platforms that would diverge are the small boards and
 * legacy hosts least able to report it. */
#ifndef _FILE_OFFSET_BITS
#define _FILE_OFFSET_BITS 64
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>

#include "accudisc/accudisc.h"
#include "internal.h"

#define VERIFY_ANCHOR_SECTORS 16u
#define SAMPLES_PER_SECTOR    588u
#define BYTES_PER_SAMPLE      4u

/* The anchor must actually be able to distinguish one alignment from another.
 * Digital silence matches at EVERY shift, and a check whose input cannot
 * distinguish the hypotheses is worth nothing however well it is written.
 *
 * BE CLEAR ABOUT WHICH GUARD DOES THE WORK. The runner-up margin in
 * score_shifts is what actually guarantees a unique answer, and it catches
 * silence on its own — verified by removing this check and watching the
 * silence case still refuse (2026-09-05). This one is an early-out: it skips
 * a pointless 9409-shift scan over a window that cannot possibly settle
 * anything, and it names the reason in the code. Do not read it as the
 * guarantee, and do not delete it thinking the margin is redundant — the
 * margin is load-bearing and this is not. */
#define ANCHOR_MIN_DISTINCT   64u

/* A winning shift must be BOTH clearly separated from the runner-up AND not
 * absurd in absolute terms.
 *
 * SEPARATION, NOT A RATIO, and the difference is not cosmetic. The first
 * version of this required the runner-up to be ten TIMES worse, which is fine
 * while the best shift is near-perfect and nonsense once it is not: at 30%
 * of the anchor differing, ten times worse is 300% and unreachable, so a
 * badly-burnt disc — the one a verify exists for — would have been reported
 * as "cannot align" rather than as "aligned, and 30% wrong". Requiring the
 * runner-up to be worse by a fixed FRACTION OF THE WINDOW behaves correctly
 * at both ends: silence gives best == second == 0 and separates by nothing,
 * while heavy damage still separates by most of the window.
 *
 * The absolute limit is a different question — "are these two even the same
 * audio?" — and 50% is deliberately loose. Two unrelated 32-bit samples agree
 * with probability 2^-32, so any shift matching a substantial fraction is a
 * real relationship; the limit is there for correlated audio that is NOT this
 * source, where one shift can look better than its neighbours while still
 * being mostly wrong. */
#define ANCHOR_MAX_BAD_PCT    50u
#define ANCHOR_SEP_DIV        10u

struct anchor_ctx {
    uint8_t *buf;
    uint32_t sectors;
    uint32_t got;
    uint32_t lba0;
};

static int anchor_sink(void *user, const accudisc_chunk *chunk)
{
    struct anchor_ctx *a = user;

    for (uint32_t k = 0; k < chunk->nsec; k++) {
        uint32_t idx = chunk->lba + k - a->lba0;

        if (idx >= a->sectors)
            continue;
        memcpy(a->buf + (size_t)idx * SAMPLES_PER_SECTOR * BYTES_PER_SAMPLE,
               chunk->data + (size_t)k * chunk->sector_len,
               SAMPLES_PER_SECTOR * BYTES_PER_SAMPLE);
        a->got++;
    }
    return 0;
}

/* Distinct 32-bit sample values in the window, capped — a cheap entropy proxy
 * that is exact for the case it exists to catch (silence, and a short
 * repeating pattern). */
static uint32_t distinct_samples(const uint8_t *p, uint32_t n)
{
    uint32_t seen[ANCHOR_MIN_DISTINCT];
    uint32_t nseen = 0;

    for (uint32_t i = 0; i < n && nseen < ANCHOR_MIN_DISTINCT; i++) {
        uint32_t v;
        uint32_t j;

        memcpy(&v, p + (size_t)i * BYTES_PER_SAMPLE, 4);
        for (j = 0; j < nseen; j++)
            if (seen[j] == v)
                break;
        if (j == nseen)
            seen[nseen++] = v;
    }
    return nseen;
}

/* Score every shift in the window and return the winner, or -1 if none is
 * both good enough and clearly better than its runner-up. */
static int score_shifts(const uint8_t *disc, uint32_t nsamp,
                        const uint8_t *src, uint32_t src_samples,
                        int32_t src_base_shift, int32_t *out)
{
    uint32_t best = 0xFFFFFFFFu, second = 0xFFFFFFFFu;
    int32_t best_shift = 0;
    uint32_t limit = nsamp / 100u * ANCHOR_MAX_BAD_PCT;

    for (int32_t s = -ACCUDISC_VERIFY_SHIFT_MAX;
         s <= ACCUDISC_VERIFY_SHIFT_MAX; s++) {
        int64_t off = (int64_t)s - src_base_shift;
        uint32_t bad = 0;

        if (off < 0 || (uint64_t)off + nsamp > src_samples)
            continue;
        for (uint32_t i = 0; i < nsamp; i++) {
            if (memcmp(disc + (size_t)i * BYTES_PER_SAMPLE,
                       src + ((size_t)off + i) * BYTES_PER_SAMPLE,
                       BYTES_PER_SAMPLE) != 0) {
                bad++;
                /* Abandon a shift once it cannot beat the incumbent: the
                 * search is over 9409 shifts and only one can win. */
                if (bad >= second)
                    break;
            }
        }
        if (bad < best) { second = best; best = bad; best_shift = s; }
        else if (bad < second) { second = bad; }
    }

    if (best == 0xFFFFFFFFu)
        return -1;                       /* no shift was even evaluable */
    if (best > limit)
        return -1;                       /* not the same audio at all */
    if (second != 0xFFFFFFFFu && second < best + nsamp / ANCHOR_SEP_DIV)
        return -1;                       /* no clear winner: ambiguous audio */
    *out = best_shift;
    return 0;
}

struct cmp_ctx {
    FILE *bin;
    int32_t shift;
    uint32_t lba0;
    uint64_t src_samples;
    uint64_t compared;
    uint64_t differing;
    int64_t first_diff;
    uint8_t *src;          /* scratch, one chunk's worth */
    size_t src_cap;
    int io_err;
};

static int cmp_sink(void *user, const accudisc_chunk *chunk)
{
    struct cmp_ctx *c = user;

    for (uint32_t k = 0; k < chunk->nsec; k++) {
        uint32_t lba = chunk->lba + k;
        int64_t i0 = (int64_t)(lba - c->lba0) * SAMPLES_PER_SECTOR;
        int64_t j0 = i0 + c->shift;
        uint32_t n = SAMPLES_PER_SECTOR;
        uint32_t skip = 0;
        const uint8_t *audio = chunk->data + (size_t)k * chunk->sector_len;

        /* Clip where the source does not reach. The displacement costs |shift|
         * samples at one end of the span; those are not compared and are not
         * counted as compared — a sample we never looked at must never appear
         * in the denominator. */
        if (j0 < 0) {
            if (-j0 >= (int64_t)n)
                continue;
            skip = (uint32_t)(-j0);
            j0 = 0;
            n -= skip;
        }
        if ((uint64_t)j0 + n > c->src_samples) {
            if ((uint64_t)j0 >= c->src_samples)
                continue;
            n = (uint32_t)(c->src_samples - (uint64_t)j0);
        }
        if (n == 0)
            continue;

        if (fseeko(c->bin, (off_t)((uint64_t)j0 * BYTES_PER_SAMPLE), SEEK_SET) != 0 ||
            fread(c->src, 1, (size_t)n * BYTES_PER_SAMPLE, c->bin)
                != (size_t)n * BYTES_PER_SAMPLE) {
            c->io_err = 1;
            return 1;
        }
        for (uint32_t i = 0; i < n; i++) {
            c->compared++;
            if (memcmp(audio + (size_t)(skip + i) * BYTES_PER_SAMPLE,
                       c->src + (size_t)i * BYTES_PER_SAMPLE,
                       BYTES_PER_SAMPLE) != 0) {
                c->differing++;
                if (c->first_diff < 0)
                    c->first_diff = (int64_t)lba;
            }
        }
    }
    return 0;
}

static int align_span(accudisc_device *dev, FILE *bin, uint64_t src_samples,
                      uint32_t start, uint32_t count, uint8_t c2,
                      uint16_t speed_x, int32_t *shift_out)
{
    uint32_t positions[3];
    int rc = ACCUDISC_ERR_INVAL;

    /* Three candidate anchors: the middle first (track starts and disc edges
     * are where silence and damage cluster), then a third in, then the head.
     * A silent or ambiguous window is skipped, not forced. */
    positions[0] = count > 2 * VERIFY_ANCHOR_SECTORS
                       ? start + count / 2 - VERIFY_ANCHOR_SECTORS / 2 : start;
    positions[1] = count > 3 * VERIFY_ANCHOR_SECTORS ? start + count / 3 : start;
    positions[2] = start;

    for (unsigned p = 0; p < 3; p++) {
        struct anchor_ctx a;
        accudisc_read_req req;
        uint32_t nsec = count < VERIFY_ANCHOR_SECTORS ? count : VERIFY_ANCHOR_SECTORS;
        uint32_t nsamp = nsec * SAMPLES_PER_SECTOR;
        int64_t base;
        uint64_t win_lo, win_hi;
        uint8_t *src;

        if (positions[p] + nsec > start + count)
            continue;

        memset(&a, 0, sizeof a);
        a.sectors = nsec;
        a.lba0 = positions[p];
        a.buf = calloc(nsamp, BYTES_PER_SAMPLE);
        if (!a.buf)
            return ACCUDISC_ERR_NOMEM;

        memset(&req, 0, sizeof req);
        req.size = sizeof req;
        req.lba = positions[p];
        req.count = nsec;
        req.c2 = c2;
        req.retries = 2;
        req.speed_x = speed_x;
        rc = accudisc_read_cdda(dev, &req, anchor_sink, &a, NULL);
        if (rc != ACCUDISC_OK || a.got != nsec) {
            free(a.buf);
            continue;
        }
        if (distinct_samples(a.buf, nsamp) < ANCHOR_MIN_DISTINCT) {
            free(a.buf);            /* silence or a flat pattern: unusable */
            continue;
        }

        /* Source window: the anchor's own span, widened by the search bound
         * on both sides. base is the source sample index that shift 0 maps to. */
        base = (int64_t)(positions[p] - start) * SAMPLES_PER_SECTOR;
        win_lo = base > ACCUDISC_VERIFY_SHIFT_MAX
                     ? (uint64_t)(base - ACCUDISC_VERIFY_SHIFT_MAX) : 0;
        win_hi = (uint64_t)base + nsamp + ACCUDISC_VERIFY_SHIFT_MAX;
        if (win_hi > src_samples)
            win_hi = src_samples;
        if (win_hi <= win_lo + nsamp) {
            free(a.buf);
            continue;
        }
        src = malloc((size_t)(win_hi - win_lo) * BYTES_PER_SAMPLE);
        if (!src) { free(a.buf); return ACCUDISC_ERR_NOMEM; }
        if (fseeko(bin, (off_t)(win_lo * BYTES_PER_SAMPLE), SEEK_SET) != 0 ||
            fread(src, 1, (size_t)(win_hi - win_lo) * BYTES_PER_SAMPLE, bin)
                != (size_t)(win_hi - win_lo) * BYTES_PER_SAMPLE) {
            free(src); free(a.buf);
            return ACCUDISC_ERR_IO;
        }

        rc = score_shifts(a.buf, nsamp, src,
                          (uint32_t)(win_hi - win_lo),
                          (int32_t)((int64_t)win_lo - base), shift_out);
        free(src);
        free(a.buf);
        if (rc == 0)
            return ACCUDISC_OK;
    }
    return ACCUDISC_ERR_UNSUPPORTED; /* no anchor could align the span */
}

int accudisc_verify(accudisc_device *dev, const char *bin_path,
                    const accudisc_verify_opts *opts,
                    accudisc_verify_result *out,
                    void (*progress)(void *user, uint32_t done, uint32_t total),
                    void *user)
{
    accudisc_verify_opts o;
    accudisc_verify_result r;
    accudisc_features feat;
    struct cmp_ctx c;
    accudisc_read_req req;
    accudisc_read_stats rs;
    FILE *bin = NULL;
    uint64_t src_samples;
    size_t out_bytes;
    off_t fsz;
    uint8_t tier, c2mode = ACCUDISC_C2_NONE;
    int rc;

    (void)progress; (void)user;

    if (!dev || !bin_path || !opts || !out)
        return ACCUDISC_ERR_INVAL;

    memset(&o, 0, sizeof o);
    rc = adsc_abi_import(&o, sizeof o, opts, opts->size);
    if (rc != ACCUDISC_OK)
        return rc;
    rc = adsc_abi_export(out->size, sizeof r, &out_bytes);
    if (rc != ACCUDISC_OK)
        return rc;

    if (o.require_tier > o.want_tier)
        return ACCUDISC_ERR_INVAL;

    memset(&r, 0, sizeof r);
    r.size = (uint32_t)sizeof r;
    r.first_diff_lba = -1;

    bin = fopen(bin_path, "rb");
    if (!bin)
        return ACCUDISC_ERR_IO;
    if (fseeko(bin, 0, SEEK_END) != 0 || (fsz = ftello(bin)) < 0) {
        fclose(bin);
        return ACCUDISC_ERR_IO;
    }
    src_samples = (uint64_t)fsz / BYTES_PER_SAMPLE;
    if (src_samples == 0) {
        fclose(bin);
        return ACCUDISC_ERR_INVAL;
    }
    if (o.count == 0)
        o.count = (uint32_t)(src_samples / SAMPLES_PER_SECTOR);
    if (o.count == 0) {
        fclose(bin);
        return ACCUDISC_ERR_INVAL;
    }

    /* Tier resolution, top down. Each step is gated on evidence, never on a
     * claim: C2 on the functional probe's verdict, counters on a driver that
     * actually offers the capability. */
    tier = o.want_tier > ACCUDISC_VERIFY_COUNTERS ? ACCUDISC_VERIFY_COUNTERS
                                                  : o.want_tier;
    if (tier >= ACCUDISC_VERIFY_COUNTERS) {
        accudisc_counters probe;

        if (accudisc_counter_scan_begin(dev) != ACCUDISC_OK) {
            tier = ACCUDISC_VERIFY_C2;
        } else {
            /* Armed successfully; prove the readout works too before
             * promising the tier, then disarm. Arming is not evidence that
             * the counters can be read. */
            int ok = accudisc_counter_scan_read(dev, &probe) == ACCUDISC_OK;

            accudisc_counter_scan_end(dev);
            if (!ok)
                tier = ACCUDISC_VERIFY_C2;
        }
    }
    if (tier >= ACCUDISC_VERIFY_C2) {
        memset(&feat, 0, sizeof feat);
        if (accudisc_probe_features(dev, &feat) != ACCUDISC_OK ||
            feat.c2_verdict != ACCUDISC_C2_SUPPORTED)
            tier = ACCUDISC_VERIFY_COMPARE;
        else
            c2mode = ACCUDISC_C2_PTRS;
    }
    if (tier < o.require_tier) {
        fclose(bin);
        return ACCUDISC_ERR_UNSUPPORTED;
    }
    r.tier = tier;
    r.degraded = tier < o.want_tier;

    rc = align_span(dev, bin, src_samples, o.start, o.count, c2mode,
                    o.speed_x, &r.shift_samples);
    if (rc == ACCUDISC_ERR_NOMEM || rc == ACCUDISC_ERR_IO) {
        fclose(bin);
        return rc;
    }
    if (rc != ACCUDISC_OK) {
        /* Could not align. Report that plainly and DO NOT compare: a compare
         * at an unknown displacement produces a mismatch count that looks
         * like a verdict and is not one. */
        r.aligned = 0;
        fclose(bin);
        memcpy(out, &r, out_bytes);
        return ACCUDISC_OK;
    }
    r.aligned = 1;

    memset(&c, 0, sizeof c);
    c.bin = bin;
    c.shift = r.shift_samples;
    c.lba0 = o.start;
    c.src_samples = src_samples;
    c.first_diff = -1;
    c.src_cap = (size_t)SAMPLES_PER_SECTOR * BYTES_PER_SAMPLE;
    c.src = malloc(c.src_cap);
    if (!c.src) {
        fclose(bin);
        return ACCUDISC_ERR_NOMEM;
    }

    memset(&req, 0, sizeof req);
    req.size = sizeof req;
    req.lba = o.start;
    req.count = o.count;
    req.c2 = c2mode;
    req.retries = 2;
    req.speed_x = o.speed_x;
    req.cancel = o.cancel;
    memset(&rs, 0, sizeof rs);
    rs.size = sizeof rs;

    rc = accudisc_read_cdda(dev, &req, cmp_sink, &c, &rs);
    free(c.src);
    fclose(bin);
    if (c.io_err)
        return ACCUDISC_ERR_IO;
    if (rc != ACCUDISC_OK)
        return rc;

    r.samples_compared = c.compared;
    r.samples_differing = c.differing;
    r.first_diff_lba = c.first_diff;
    if (tier >= ACCUDISC_VERIFY_C2) {
        r.c2_bits = rs.c2_bits;
        r.sectors_flagged = rs.sectors_flagged;
        r.hard_errors = rs.hard_errors;
    }

    if (tier >= ACCUDISC_VERIFY_COUNTERS) {
        accudisc_census_opts co;

        memset(&co, 0, sizeof co);
        co.size = sizeof co;
        /* The census scans the SECTOR span, unshifted. It is deliberately not
         * offset by shift_samples: the counters are a property of what the
         * drive read off the disc at those sectors, not of where the audio
         * landed within them, and a sub-sector shift cannot move a sector
         * boundary anyway. Stated here and in the header so nobody later
         * "fixes" it into a rounded shift that changes which sectors are
         * scanned without changing what is being asked. */
        co.start = o.start;
        co.end = o.start + o.count;
        co.speed_x = o.speed_x;
        co.cancel = o.cancel;
        /* A census failure does not invalidate the compare that already ran.
         * Drop the tier and keep what was measured. */
        if (accudisc_counter_census(dev, &co, NULL, NULL, &r.census)
            != ACCUDISC_OK) {
            memset(&r.census, 0, sizeof r.census);
            r.tier = ACCUDISC_VERIFY_C2;
            r.degraded = 1;
        }
    }

    memcpy(out, &r, out_bytes);
    return ACCUDISC_OK;
}
