/* Write-offset measurement — the signal, and the arithmetic that finds it again.
 *
 * A drive's WRITE offset has no table to look it up in: the live sources publish
 * READ offsets only. It is obtained by burning a known signal and reading it
 * back, which is a PROCEDURE and therefore the caller's. What lives here is the
 * pair of pieces every consumer would otherwise rebuild — the signal, and the
 * locator — for the same reason accudisc_ctdb_repair lives in this library: they
 * are arithmetic on audio, easy to get subtly wrong, and wrong in a way that
 * still looks like a number.
 *
 * Nothing here touches a device. The full contract is in the public header.
 */

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "../internal.h"

/* Above the noise floor of digital silence, far below a real burst sample.
 * The signal is silence plus full-scale noise, so this separates them by three
 * orders of magnitude rather than by a judgement. */
#define WOFF_THRESHOLD 500

/* A single sample over threshold is a click, a dropout artefact, or one loud
 * sample of a burst that has not started yet. Requiring most of a short run to
 * be loud costs nothing on a real burst — a full-scale noise burst puts ~98.5%
 * of its samples over 500 — and rejects a lone spike, which is the failure that
 * would otherwise silently move the answer by hundreds of samples. */
#define WOFF_RUN_LEN  16
#define WOFF_RUN_MIN   8

/* xorshift32, fixed seed. The value is arbitrary; being FIXED is the point, so
 * the same disc can be re-measured months later and compared. It is NOT a claim
 * that another tool generates identical bytes: the locator is threshold-based,
 * not correlation-based, so any sufficiently loud burst at the documented
 * positions measures the same — which is what makes a cdda2img-burnt disc
 * readable here and vice versa.
 *
 * THAT LAST SENTENCE IS NOW MEASURED RATHER THAN INTENDED, 2026-08-27. cdda2img
 * ran this locator against a real PX-716A read-back of a disc burnt from THEIR
 * generator — full-scale uniform noise, a different distribution from the
 * half-scale bursts above, with no forced leading edge — and it located both
 * pulses and returned -30, agreeing with their own tool on the offset AND on
 * the absolute found positions (44070 / 2645970). A correlation-based locator
 * would have returned NOTFOUND on that file.
 *
 * It also happens to be the locator's first contact with real drive output at
 * all; everything in our own suite is a shifted array, which tests the
 * arithmetic and never the jitter.
 *
 * What made the cross-check possible is that both projects independently chose
 * the SAME four numbers — 75 s, 1 s, 60 s, one frame. Interoperability rests on
 * that geometry, not on the noise, which is why the constants are in the public
 * header as a contract and the seed is not. */
static uint32_t woff_rand(uint32_t *st)
{
    uint32_t x = *st;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    return *st = x;
}

int accudisc_write_offset_signal(int16_t *pcm, uint32_t samples)
{
    static const uint32_t pos[2] = { ACCUDISC_WOFF_PULSE_A,
                                     ACCUDISC_WOFF_PULSE_B };
    int16_t burst[ACCUDISC_WOFF_PULSE_LEN][2];
    uint32_t st = 0x9E3779B9u;
    uint32_t i, k;

    if (!pcm)
        return ACCUDISC_ERR_INVAL;
    /* Exactly, not "at least". A short buffer would put pulse B off the end and
     * quietly reduce the measurement to a single pulse — losing the cross-check
     * that is the whole reason there are two. */
    if (samples != ACCUDISC_WOFF_SAMPLES)
        return ACCUDISC_ERR_INVAL;

    for (i = 0; i < ACCUDISC_WOFF_PULSE_LEN; i++) {
        burst[i][0] = (int16_t)(woff_rand(&st) >> 17) - 16384;
        burst[i][1] = (int16_t)(woff_rand(&st) >> 17) - 16384;
    }
    /* THE LEADING EDGE IS FORCED FULL SCALE, and this is LOAD-BEARING rather
     * than a refinement. Two facts make it so.
     *
     * FIRST, the rate. Samples are uniform on [-16384, 16383] and woff_loud
     * tests |L| > 500 OR |R| > 500, so a pair is quiet with probability
     * (1001/32768)^2 = 1 in 1072. Measured over 2e7 draws of this exact
     * generator: 1 in 1068. (This comment said "one in 4000" until 2026-08-27,
     * which understated it 3.7x — that figure was reasoned about full-scale
     * noise, and these samples are half scale.) If the FIRST pair is quiet the
     * locator reports the burst starting a sample late, and the measured offset
     * moves by 1.
     *
     * SECOND, and this is what makes it the only guard: BOTH BURSTS ARE THE
     * SAME WAVEFORM — one `burst` array written at pos[0] and pos[1] below. So
     * a quiet leading pair would bias A and B IDENTICALLY, off_a == off_b would
     * hold, and the two-pulse consistency check in accudisc_write_offset_locate
     * would pass while both values were wrong by one. That check tests the
     * DISC, not the SIGNAL, and it is structurally blind to any defect the two
     * pulses share. (Identified by cdda2img 2026-08-27, whose own tool has the
     * same shared-waveform shape and no forced edge; their seed happens not to
     * trigger it, which is precisely the point — it is a property of the seed,
     * so "it has never fired" is not evidence that it cannot.)
     *
     * The ambiguity is therefore removed at the source rather than papered over
     * in the locator, and tests/test_write_offset.c pins it so it cannot be
     * tidied away as redundant. */
    burst[0][0] = 32767;
    burst[0][1] = -32768;

    memset(pcm, 0, (size_t)samples * 2 * sizeof(int16_t));
    for (k = 0; k < 2; k++)
        for (i = 0; i < ACCUDISC_WOFF_PULSE_LEN; i++) {
            pcm[(pos[k] + i) * 2]     = burst[i][0];
            pcm[(pos[k] + i) * 2 + 1] = burst[i][1];
        }
    return ACCUDISC_OK;
}

/* Is the READ-OFFSET-CORRECTED sample at index i loud? Corrected index i is raw
 * index i + read_offset; the caller has already bounds-checked the mapping. */
static int woff_loud(const int16_t *pcm, size_t raw)
{
    int16_t l = pcm[raw * 2], r = pcm[raw * 2 + 1];
    int32_t al = l < 0 ? -(int32_t)l : l;
    int32_t ar = r < 0 ? -(int32_t)r : r;
    return al > WOFF_THRESHOLD || ar > WOFF_THRESHOLD;
}

/* First corrected index in [lo, hi) that starts a run. Returns -1 for none.
 *
 * The scan is done in CORRECTED coordinates — the read offset is applied by
 * indexing rather than by shifting a 13 MB buffer, which keeps the correction
 * in one expression where its sign can be read. Corrected i is raw i + R, so a
 * drive that reads EARLY (R > 0) has the audio we want further along the
 * buffer, and subtracting R is what puts it back. */
static int32_t woff_find(const int16_t *pcm, uint32_t samples, int32_t read_offset,
                         int64_t lo, int64_t hi)
{
    int64_t i;

    for (i = lo; i < hi; i++) {
        int64_t raw = i + read_offset;
        int64_t j, end;
        int hits = 0;

        if (raw < 0 || (uint64_t)raw >= samples)
            continue;
        if (!woff_loud(pcm, (size_t)raw))
            continue;

        end = raw + WOFF_RUN_LEN;
        if ((uint64_t)end > samples)
            end = (int64_t)samples;
        for (j = raw; j < end; j++)
            if (woff_loud(pcm, (size_t)j))
                hits++;
        if (hits >= WOFF_RUN_MIN)
            return (int32_t)i;
    }
    return -1;
}

int accudisc_write_offset_locate(const int16_t *pcm, uint32_t samples,
                                 int32_t read_offset,
                                 accudisc_write_offset_info *out)
{
    static const int32_t pos[2] = { (int32_t)ACCUDISC_WOFF_PULSE_A,
                                    (int32_t)ACCUDISC_WOFF_PULSE_B };
    int32_t found[2], off[2];
    uint32_t k;

    if (!pcm || !out)
        return ACCUDISC_ERR_INVAL;
    if (out->size == 0 || out->size > sizeof(*out))
        return ACCUDISC_ERR_ABI;
    /* A read-back shorter than the signal cannot hold pulse B. Refused rather
     * than measured on pulse A alone, for the reason _signal refuses a short
     * buffer: one pulse is a number with nothing to check it against. */
    if (samples < ACCUDISC_WOFF_SAMPLES)
        return ACCUDISC_ERR_INVAL;

    memset((char *)out + sizeof(out->size), 0, out->size - sizeof(out->size));
    out->write_offset = ACCUDISC_OFFSET_NONE;
    out->found_a = out->found_b = ACCUDISC_OFFSET_NONE;

    for (k = 0; k < 2; k++) {
        int64_t lo = (int64_t)pos[k] - ACCUDISC_WOFF_SEARCH;
        int64_t hi = (int64_t)pos[k] + ACCUDISC_WOFF_SEARCH;

        if (lo < 0)
            lo = 0;
        found[k] = woff_find(pcm, samples, read_offset, lo, hi);
        off[k] = found[k] < 0 ? 0 : found[k] - pos[k];
    }

    out->found_a = found[0] < 0 ? ACCUDISC_OFFSET_NONE : found[0];
    out->found_b = found[1] < 0 ? ACCUDISC_OFFSET_NONE : found[1];
    out->offset_a = off[0];
    out->offset_b = off[1];

    if (found[0] < 0 || found[1] < 0)
        return ACCUDISC_ERR_NOTFOUND;

    if (off[0] != off[1]) {
        /* Two measurements of one quantity that do not agree. Averaging them
         * would produce a number no pulse supports; picking one would be a
         * silent choice. The disc is the suspect, not the drive. */
        out->flags |= ACCUDISC_WOFF_F_INCONSISTENT;
        return ACCUDISC_ERR_AMBIGUOUS;
    }

    out->write_offset = off[0];
    return ACCUDISC_OK;
}
