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

#include <stdlib.h>
#include <string.h>

#include "../mmc/mmc.h"
#include "c2lag.h"

/* Per C2-active sector: reads taken (all pairs scored) and the reread
 * budget. 4 reads = 6 pairs per sector. */
#define C2LAG_READS_PER_SECTOR 4
/* C2-active sectors used; damage regions are bursty, so a few dozen
 * sectors already carry thousands of flag observations. */
#define C2LAG_MAX_TARGETS 32
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

    if (acc->flags[best] < ADSC_C2LAG_MIN_FLAGS ||
        acc->diff_bytes < ADSC_C2LAG_MIN_DIFFS ||
        prec[best] < ADSC_C2LAG_MIN_PEAK_MILLI)
        return ACCUDISC_ERR_NOTFOUND;

    out->lag_pairs = (int32_t)best - ADSC_C2LAG_MAX_SHIFT;
    out->flags_used = (uint32_t)acc->flags[best];
    out->diff_bytes = (uint32_t)(acc->diff_bytes > 0xffffffffu
                                     ? 0xffffffffu : acc->diff_bytes);
    out->peak_milli = (uint16_t)prec[best];
    out->runner_milli = (uint16_t)runner;
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
    const uint32_t chunk = 24; /* keeps the transfer under 64 KiB */

    if (!dev || !out || count == 0)
        return ACCUDISC_ERR_INVAL;
    memset(out, 0, sizeof(*out));

    uint8_t *buf = malloc((size_t)chunk * sector_len);
    uint8_t *reads = malloc((size_t)C2LAG_READS_PER_SECTOR * sector_len);
    uint8_t *flush = malloc(ACCUDISC_BYTES_AUDIO);
    uint32_t targets[C2LAG_MAX_TARGETS];
    uint32_t ntargets = 0;
    int rc = ACCUDISC_ERR_NOTFOUND;

    if (!buf || !reads || !flush) {
        rc = ACCUDISC_ERR_NOMEM;
        goto out_free;
    }

    /* Pass 1: scan the span for C2-active sectors (the probe's fuel). */
    for (uint32_t off = 0; off < count && ntargets < C2LAG_MAX_TARGETS;
         off += chunk) {
        uint32_t n = count - off < chunk ? count - off : chunk;

        if (adsc_mmc_read_cd(dev, lba + off, n, ADSC_SECTOR_CDDA,
                             ACCUDISC_C2_PTRS, ACCUDISC_SUB_NONE, buf,
                             sector_len) != ACCUDISC_OK)
            continue; /* hard sectors carry no flags; skip the chunk */
        for (uint32_t s = 0; s < n && ntargets < C2LAG_MAX_TARGETS; s++)
            if (c2_bits(buf + (size_t)s * sector_len))
                targets[ntargets++] = lba + off + s;
    }
    if (ntargets == 0)
        goto out_free; /* clean span: honestly inconclusive */

    /* Pass 2: independent rereads of each active sector; score every pair
     * of successful reads. Flags are non-deterministic read to read, so
     * flag-free rereads simply contribute nothing. */
    struct adsc_c2lag_acc acc;
    memset(&acc, 0, sizeof(acc));

    for (uint32_t t = 0; t < ntargets; t++) {
        unsigned got = 0;

        for (unsigned k = 0; k < C2LAG_READS_PER_SECTOR; k++) {
            cache_defeat(dev, targets[t], lba, flush);
            if (adsc_mmc_read_cd(dev, targets[t], 1, ADSC_SECTOR_CDDA,
                                 ACCUDISC_C2_PTRS, ACCUDISC_SUB_NONE,
                                 reads + (size_t)got * sector_len,
                                 sector_len) == ACCUDISC_OK)
                got++;
        }
        for (unsigned i = 0; i + 1 < got; i++)
            for (unsigned j = i + 1; j < got; j++) {
                const uint8_t *a = reads + (size_t)i * sector_len;
                const uint8_t *b = reads + (size_t)j * sector_len;

                adsc_c2lag_add(&acc, a, a + ACCUDISC_BYTES_AUDIO,
                               b, b + ACCUDISC_BYTES_AUDIO);
            }
    }

    rc = adsc_c2lag_result(&acc, out);

out_free:
    free(buf);
    free(reads);
    free(flush);
    return rc;
}
