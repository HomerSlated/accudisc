/* C2/audio alignment probe (see accudisc.h for the contract and the sign
 * convention). The trick that makes this work without an external oracle:
 * a fired C2 flag marks a byte the CIRC decoder FAILED on, and failed
 * bytes come back different across genuine (cache-defeated) rereads. So
 * the flag positions of a read, scored against the byte diff between two
 * reads of the same sector, agree best at the drive's true bitmap shift —
 * the same TP-argmax that pinned the PX-716A's 2-pair lag against an
 * AccurateRip oracle, with reread instability standing in for the oracle.
 *
 * Damage is what powers the probe: sectors without fired flags contribute
 * nothing, so the span is scanned for C2-active sectors first and only
 * those are reread. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../mmc/mmc.h"
#include "c2lag.h"

/* Rereads are STREAMING WINDOW reads, not isolated single-sector reads:
 * marginal defects fire C2 while the drive streams and decode cleanly on
 * a careful post-seek single-sector read (verified live on the PX-716A —
 * a streaming pass flagged ~40 sectors where per-sector rereads of the
 * same LBAs flagged zero). Each C2-active chunk from the scan pass is
 * reread as a whole window with a run-up lead-in so the drive is in
 * streaming state when it crosses the damage. */
#define C2LAG_WINDOW_READS 4   /* window reads paired = 6 pairs/sector */
#define C2LAG_MAX_WINDOWS  8   /* C2-active chunks reread */
#define C2LAG_RUNUP        16  /* streaming run-up sectors before a window */
/* Cache-defeat distance, as in the read engine. */
#define C2LAG_FLUSH_DISTANCE 5000

void adsc_c2lag_add(struct adsc_c2lag_acc *acc,
                    const uint8_t *audio_a, const uint8_t *c2_a,
                    const uint8_t *audio_b, const uint8_t *c2_b)
{
    uint8_t diff[ACCUDISC_BYTES_AUDIO];
    const uint8_t *c2[2] = { c2_a, c2_b };

    for (uint32_t i = 0; i < ACCUDISC_BYTES_AUDIO; i++)
        diff[i] = audio_a[i] != audio_b[i];
    for (uint32_t i = 0; i < ACCUDISC_BYTES_AUDIO; i++)
        acc->diff_bytes += diff[i];

    for (unsigned r = 0; r < 2; r++) {
        for (uint32_t f = 0; f < ACCUDISC_BYTES_AUDIO; f++) {
            if (!((c2[r][f >> 3] >> (7 - (f & 7))) & 1))
                continue;
            for (int s = -ADSC_C2LAG_MAX_SHIFT; s <= ADSC_C2LAG_MAX_SHIFT;
                 s++) {
                /* Candidate lag s: this flag would describe audio byte
                 * f - 4s (positive lag = bitmap trails the audio). */
                int32_t t = (int32_t)f - 4 * s;
                unsigned idx = (unsigned)(s + ADSC_C2LAG_MAX_SHIFT);

                if (t < 0 || t >= (int32_t)ACCUDISC_BYTES_AUDIO)
                    continue;
                acc->flags[idx]++;
                acc->tp[idx] += diff[t];
            }
        }
    }
}

int adsc_c2lag_result(const struct adsc_c2lag_acc *acc, accudisc_c2_lag *out)
{
    unsigned best = 0;
    uint32_t prec[ADSC_C2LAG_NSHIFT];

    for (unsigned i = 0; i < ADSC_C2LAG_NSHIFT; i++) {
        prec[i] = acc->flags[i]
                      ? (uint32_t)(acc->tp[i] * 1000 / acc->flags[i]) : 0;
        if (prec[i] > prec[best])
            best = i;
    }

    uint32_t runner = 0;
    for (unsigned i = 0; i < ADSC_C2LAG_NSHIFT; i++)
        if (i != best && prec[i] > runner)
            runner = prec[i];

    if (getenv("ACCUDISC_C2LAG_DEBUG"))
        for (unsigned i = 0; i < ADSC_C2LAG_NSHIFT; i++)
            fprintf(stderr, "c2lag shift %+d: tp=%llu flags=%llu prec=%u\n",
                    (int)i - ADSC_C2LAG_MAX_SHIFT,
                    (unsigned long long)acc->tp[i],
                    (unsigned long long)acc->flags[i], (unsigned)prec[i]);

    /* Fill the evidence either way — an inconclusive verdict with its
     * numbers is diagnosable; a bare error is not. */
    out->lag_pairs = (int32_t)best - ADSC_C2LAG_MAX_SHIFT;
    out->flags_used = (uint32_t)acc->flags[best];
    out->diff_bytes = (uint32_t)(acc->diff_bytes > 0xffffffffu
                                     ? 0xffffffffu : acc->diff_bytes);
    out->peak_milli = (uint16_t)prec[best];
    out->runner_milli = (uint16_t)runner;

    if (acc->flags[best] < ADSC_C2LAG_MIN_FLAGS ||
        acc->tp[best] < ADSC_C2LAG_MIN_TP ||
        acc->diff_bytes < ADSC_C2LAG_MIN_DIFFS ||
        prec[best] < ADSC_C2LAG_MIN_PEAK_MILLI ||
        prec[best] < ADSC_C2LAG_MIN_CONTRAST * runner)
        return ACCUDISC_ERR_NOTFOUND;
    return ACCUDISC_OK;
}

/* ---- device half ----------------------------------------------------------- */

static void cache_defeat(struct accudisc_device *dev, uint32_t cur,
                         uint32_t span_lo, uint8_t *flush)
{
    uint32_t away = cur >= span_lo + C2LAG_FLUSH_DISTANCE
                        ? cur - C2LAG_FLUSH_DISTANCE
                        : cur + C2LAG_FLUSH_DISTANCE;

    adsc_mmc_read_cd(dev, away, 1, ADSC_SECTOR_CDDA, ACCUDISC_C2_NONE,
                     ACCUDISC_SUB_NONE, flush, ACCUDISC_BYTES_AUDIO);
}

static uint32_t c2_bits(const uint8_t *sec)
{
    uint32_t bits = 0;

    for (uint32_t i = 0; i < ACCUDISC_BYTES_C2; i++)
        bits += (uint32_t)__builtin_popcount(sec[ACCUDISC_BYTES_AUDIO + i]);
    return bits;
}

int accudisc_probe_c2_lag(accudisc_device *dev, uint32_t lba, uint32_t count,
                          accudisc_c2_lag *out)
{
    const uint32_t sector_len = ACCUDISC_BYTES_AUDIO + ACCUDISC_BYTES_C2;
    /* Sized so the *pass-2* window read (chunk + C2LAG_RUNUP sectors) stays
     * under one transfer: (8 + 16) * 2646 = 63504 < ADSC_MAX_XFER (65535).
     * Sizing chunk for the pass-1 read alone (24 * 2646) overflowed the window
     * read to 40 sectors / ~105 KB and failed on small-max_sectors HBAs. */
    const uint32_t chunk = 8;
    const uint32_t wmax = chunk + C2LAG_RUNUP;

    if (!dev || !out || count == 0)
        return ACCUDISC_ERR_INVAL;
    memset(out, 0, sizeof(*out));

    uint8_t *buf = malloc((size_t)chunk * sector_len);
    uint8_t *win = malloc((size_t)C2LAG_WINDOW_READS * wmax * sector_len);
    uint8_t *flush = malloc(ACCUDISC_BYTES_AUDIO);
    struct { uint32_t lba, n; } windows[C2LAG_MAX_WINDOWS];
    uint32_t nwin = 0;
    int rc = ACCUDISC_ERR_NOTFOUND;

    if (!buf || !win || !flush) {
        rc = ACCUDISC_ERR_NOMEM;
        goto out_free;
    }

    /* Pass 1: stream the span chunk by chunk and remember which chunks
     * carry fired C2 (the probe's fuel). */
    for (uint32_t off = 0; off < count && nwin < C2LAG_MAX_WINDOWS;
         off += chunk) {
        uint32_t n = count - off < chunk ? count - off : chunk;

        if (adsc_mmc_read_cd(dev, lba + off, n, ADSC_SECTOR_CDDA,
                             ACCUDISC_C2_PTRS, ACCUDISC_SUB_NONE, buf,
                             sector_len) != ACCUDISC_OK)
            continue; /* hard sectors carry no flags; skip the chunk */

        uint32_t fired = 0;
        for (uint32_t s = 0; s < n; s++)
            fired += c2_bits(buf + (size_t)s * sector_len) != 0;
        out->sectors_active += fired;
        if (fired) {
            windows[nwin].lba = lba + off;
            windows[nwin].n = n;
            nwin++;
        }
    }
    if (nwin == 0)
        goto out_free; /* no C2 anywhere in the span: honestly inconclusive */

    /* Pass 2: reread each active chunk as a streaming window (run-up
     * lead-in, cache-defeated between reads) and score every pair of
     * successful window reads per sector. */
    struct adsc_c2lag_acc acc;
    memset(&acc, 0, sizeof(acc));

    for (uint32_t w = 0; w < nwin; w++) {
        uint32_t wlba = windows[w].lba >= lba + C2LAG_RUNUP
                            ? windows[w].lba - C2LAG_RUNUP : lba;
        uint32_t wn = windows[w].lba + windows[w].n - wlba;
        unsigned got = 0;

        for (unsigned k = 0; k < C2LAG_WINDOW_READS; k++) {
            cache_defeat(dev, wlba, lba, flush);
            if (adsc_mmc_read_cd(dev, wlba, wn, ADSC_SECTOR_CDDA,
                                 ACCUDISC_C2_PTRS, ACCUDISC_SUB_NONE,
                                 win + (size_t)got * wmax * sector_len,
                                 sector_len) == ACCUDISC_OK)
                got++;
        }
        for (unsigned i = 0; i + 1 < got; i++)
            for (unsigned j = i + 1; j < got; j++)
                for (uint32_t s = 0; s < wn; s++) {
                    const uint8_t *a = win +
                        ((size_t)i * wmax + s) * sector_len;
                    const uint8_t *b = win +
                        ((size_t)j * wmax + s) * sector_len;

                    adsc_c2lag_add(&acc, a, a + ACCUDISC_BYTES_AUDIO,
                                   b, b + ACCUDISC_BYTES_AUDIO);
                }
    }

    rc = adsc_c2lag_result(&acc, out);

out_free:
    free(buf);
    free(win);
    free(flush);
    return rc;
}
