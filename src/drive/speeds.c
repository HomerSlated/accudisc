/* Achievable-speed-ladder probe (see accudisc.h for the contract).
 *
 * Recovery ladders (read --ladder / caller-side speed sweeps) are only as
 * real as the speeds the drive actually delivers: CDROM_SELECT_SPEED is
 * best-effort, page 2A echoes the setting rather than the platter, and
 * the bus can cap everything regardless. Timed streaming reads are the
 * ground truth; page 2A is reported alongside so the caller can see when
 * it lies. */

#include <stdlib.h>
#include <time.h>

#include "../internal.h"
#include "../mmc/mmc.h"

/* Audio-only streaming keeps the measurement about the drive, not the
 * C2/sub plumbing. 27 sectors is the largest transfer under 64 KiB. */
#define SPEEDS_CHUNK 27
/* Per-rung measurement: one second of audio at the requested speed,
 * clamped so slow rungs stay quick and fast rungs stay timeable. */
#define SPEEDS_MIN_SECTORS 150
#define SPEEDS_MAX_SECTORS 2250
/* The multi-radius sweep: inner, middle, outer. Three is not a tunable —
 * it is the smallest number that shows a gradient AND a curvature, and
 * the cost is already 3x the probe time. */
#define SPEEDS_BANDS 3

static double mono_now(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

/* Stream [lba, lba+n) audio-only; returns sectors actually read. */
static uint32_t stream_span(struct accudisc_device *dev, uint32_t lba,
                            uint32_t n, uint8_t *buf)
{
    uint32_t done = 0;

    while (done < n) {
        uint32_t c = n - done < SPEEDS_CHUNK ? n - done : SPEEDS_CHUNK;

        if (adsc_mmc_read_cd(dev, lba + done, c, ADSC_SECTOR_CDDA,
                             ACCUDISC_C2_NONE, ACCUDISC_SUB_NONE, buf,
                             ACCUDISC_BYTES_AUDIO) != ACCUDISC_OK)
            break;
        done += c;
    }
    return done;
}

/* Window layout — see internal.h for why this is separable and tested.
 *
 * The span is cut into `points` equal bands, and each band into `ncand`
 * slots; rung i's window in band b is slot (b*ncand + i) counting from
 * the start of the span. Disjointness across BOTH rungs and radii falls
 * out of that indexing, so the only thing that can go wrong is the
 * windows not fitting — which is what the guard below refuses.
 *
 * Note the guard scales with `points`: three bands need three times the
 * span for the same rung count. Getting that wrong would not raise an
 * error, it would produce cache-served re-reads and a flat gradient. */
int adsc_speeds_layout(uint32_t count, uint8_t ncand, uint8_t points,
                       uint32_t *slot, uint32_t *band)
{
    if (ncand == 0 || !slot || !band)
        return ACCUDISC_ERR_INVAL;
    if (points != 1 && points != SPEEDS_BANDS)
        return ACCUDISC_ERR_INVAL;

    uint32_t s = count / ((uint32_t)points * ncand);
    if (s < SPEEDS_MIN_SECTORS + SPEEDS_CHUNK)
        return ACCUDISC_ERR_INVAL; /* span too small to give every rung a
                                    * cache-fresh window in every band */
    *slot = s;
    *band = s * ncand;
    return ACCUDISC_OK;
}

int accudisc_probe_speed_ladder(accudisc_device *dev, uint32_t lba,
                                uint32_t count, const uint16_t *candidates,
                                uint8_t ncand, uint8_t points,
                                accudisc_speed_rung *out)
{
    if (!dev || !candidates || !out || ncand == 0)
        return ACCUDISC_ERR_INVAL;
    if (points == 0)
        points = 1;

    uint32_t slot, band;
    int err = adsc_speeds_layout(count, ncand, points, &slot, &band);
    if (err != ACCUDISC_OK)
        return err;

    uint8_t *buf = malloc((size_t)SPEEDS_CHUNK * ACCUDISC_BYTES_AUDIO);
    if (!buf)
        return ACCUDISC_ERR_NOMEM;

    for (uint8_t i = 0; i < ncand; i++) {
        uint32_t want = (uint32_t)candidates[i] * 75;
        accudisc_speed_rung *r = &out[i];
        uint16_t cx_band[SPEEDS_BANDS] = {0};

        if (want < SPEEDS_MIN_SECTORS)
            want = SPEEDS_MIN_SECTORS;
        if (want > SPEEDS_MAX_SECTORS)
            want = SPEEDS_MAX_SECTORS;
        /* No underflow: adsc_speeds_layout is the only writer of `slot`
         * and floors it at SPEEDS_MIN_SECTORS + SPEEDS_CHUNK, so this
         * subtraction is a consequence of that guard, not a coincidence. */
        if (want > slot - SPEEDS_CHUNK)
            want = slot - SPEEDS_CHUNK;
        /* `want` is now fixed for this rung and used unchanged in every
         * band, so the rung's bands differ in radius and nothing else.
         * That is what makes min_cx/max_cx a gradient rather than a
         * comparison of two different measurements. */

        r->requested_x = candidates[i];
        r->reported_x = 0;
        r->measured_cx = 0;
        r->min_cx = 0;
        r->max_cx = 0;

        /* One speed set per rung, covering all of its bands. */
        accudisc_set_speed(dev, candidates[i]); /* best-effort by design */

        for (uint8_t b = 0; b < points; b++) {
            uint32_t wlba = lba + (uint32_t)b * band + (uint32_t)i * slot;

            /* Warm-up: let the drive recalibrate/spin at the new setting
             * and position the head at the window before the clock
             * starts. Also needed per band — the head has just seeked. */
            stream_span(dev, wlba, SPEEDS_CHUNK, buf);

            double t0 = mono_now();
            uint32_t done = stream_span(dev, wlba + SPEEDS_CHUNK, want, buf);
            double secs = mono_now() - t0;

            if (done && secs > 0) {
                double cx = (double)done / secs / 75.0 * 100.0;
                cx_band[b] = cx > 65535.0 ? 65535 : (uint16_t)(cx + 0.5);
            }
        }

        /* measured_cx is the MIDDLE band — one band means band 0, three
         * means band 1. Deliberately not the mean of the three: the
         * `measured` token is declared stable in
         * docs/reference/cli-machine-interface.md, and quietly turning it
         * into a different quantity would leave every existing parser
         * working while reading something else. */
        r->measured_cx = cx_band[points / 2];

        if (points > 1) {
            uint16_t lo = cx_band[0], hi = cx_band[0];
            int complete = 1;

            for (uint8_t b = 0; b < points; b++) {
                if (cx_band[b] == 0)
                    complete = 0;
                if (cx_band[b] < lo)
                    lo = cx_band[b];
                if (cx_band[b] > hi)
                    hi = cx_band[b];
            }
            /* A range over a SUBSET of the bands is a narrower claim
             * wearing the same shape, so one failed band withdraws the
             * gradient entirely rather than reporting a spread the disc
             * never showed us. */
            if (complete) {
                r->min_cx = lo;
                r->max_cx = hi;
            }
        }

        unsigned max_kbps, cur_kbps;
        if (accudisc_get_speed(dev, &max_kbps, &cur_kbps) == ACCUDISC_OK)
            r->reported_x = (uint16_t)(cur_kbps / 176);
    }

    free(buf);
    return ACCUDISC_OK;
}
