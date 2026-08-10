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

/* How many radius-steps a rate gap must exceed before it counts as a real
 * difference rather than the cross-rung radius term.
 *
 * Its job is narrower than it looks, and worth stating exactly because the
 * obvious reading is wrong. K does NOT manufacture duplicates: a rung that
 * buys nothing usually fails the "faster than the rung below" test outright
 * — on the validating run (PX-716A, 2026-07-28) req=48 measured SLOWER than
 * req=40 (23.01 vs 23.73), the radius bias made visible, and lands on
 * DUPLICATE without the margin being consulted at all. K's only load-bearing
 * job is the other direction: not collapsing rungs that are genuinely
 * distinct. On that run the closest genuine pair is 40-vs-32, a gap of 4.18x
 * against a radius-step of 0.72x, so any K below 5.8 keeps it. K from 1 to 5
 * gives an identical ladder and 6 starts eating real rungs; 3 sits in the
 * middle with room either side. tests/test_speeds.c sweeps it. */
#define SPEEDS_ADMIT_K 3
/* Floor, in centi-x, below which a gap is noise whatever the geometry says.
 * Needed because a CLV-clamped rung has ZERO spread, which would otherwise
 * make its radius-step 0 and admit any difference at all. 50 comes from the
 * measured run-to-run spread on that same run: the largest difference
 * between two identical repeats was 0.40x. */
#define SPEEDS_ADMIT_NOISE_CX 50

/* Rate difference attributable to radius alone between two ADJACENT rungs.
 *
 * A rung's max_cx - min_cx is the change across the whole probed span,
 * which is (points - 1) band-widths; a band is ncand windows; adjacent
 * rungs are one window apart. So the per-neighbour radius effect is the
 * full spread divided by (points - 1) * ncand. Self-calibrating — it is
 * measured from this disc and this drive rather than modelled. */
static uint32_t radius_step_cx(const accudisc_speed_rung *r, uint8_t n,
                               uint8_t points)
{
    if (points < 2 || n == 0 || r->max_cx <= r->min_cx)
        return 0;
    return (uint32_t)(r->max_cx - r->min_cx) / ((uint32_t)(points - 1) * n);
}

void adsc_speeds_admit(accudisc_speed_rung *rungs, uint8_t n, uint8_t points,
                       uint32_t k)
{
    if (!rungs || n == 0)
        return;

    for (uint8_t i = 0; i < n; i++) {
        rungs[i].verdict = ACCUDISC_RUNG_UNKNOWN;
        rungs[i].equiv_x = 0;
    }
    /* Without the sweep there is no interval, and a verdict off point
     * samples is exactly the confident-but-wrong answer this item exists
     * to avoid. Leave every rung UNKNOWN. */
    if (points < 2)
        return;

    /* Walk slowest first, so the LOWEST setting achieving a given rate is
     * the one admitted and the faster setting that buys nothing is the one
     * marked. The caller's order is arbitrary, so find each next-slowest
     * by scan rather than assuming the array is sorted. */
    uint8_t done = 0;
    const accudisc_speed_rung *best = NULL; /* fastest admitted so far */

    while (done < n) {
        int pick = -1;

        for (uint8_t i = 0; i < n; i++) {
            if (rungs[i].verdict != ACCUDISC_RUNG_UNKNOWN || rungs[i].equiv_x)
                continue; /* already settled this pass */
            if (pick < 0 || rungs[i].requested_x < rungs[pick].requested_x)
                pick = i;
        }
        if (pick < 0)
            break;

        accudisc_speed_rung *r = &rungs[pick];

        /* Mark it settled for the scan above by giving it a verdict; the
         * branches below decide which. */
        if (r->measured_cx == 0) {
            r->verdict = ACCUDISC_RUNG_UNKNOWN;
            r->equiv_x = 0xffff; /* scan sentinel, cleared below */
        } else if (r->reported_x && r->reported_x < r->requested_x) {
            /* Clause 1, and the only exact one: the drive reported a lower
             * speed than asked for. No measurement, no comparison, no
             * radius term — it told us. */
            r->verdict = ACCUDISC_RUNG_QUANTIZED;
            r->equiv_x = r->reported_x;
        } else if (!best) {
            r->verdict = ACCUDISC_RUNG_ADMITTED;
        } else {
            uint32_t step = radius_step_cx(r, n, points);
            uint32_t other = radius_step_cx(best, n, points);
            uint32_t margin;

            if (other > step)
                step = other; /* the more radius-sensitive of the pair */
            margin = step * k;
            if (margin < SPEEDS_ADMIT_NOISE_CX)
                margin = SPEEDS_ADMIT_NOISE_CX;

            if (r->measured_cx > best->measured_cx
                && (uint32_t)(r->measured_cx - best->measured_cx) > margin) {
                r->verdict = ACCUDISC_RUNG_ADMITTED;
            } else {
                r->verdict = ACCUDISC_RUNG_DUPLICATE;
                r->equiv_x = best->requested_x;
            }
        }

        if (r->verdict == ACCUDISC_RUNG_ADMITTED)
            best = r;
        done++;
    }

    for (uint8_t i = 0; i < n; i++)
        if (rungs[i].equiv_x == 0xffff)
            rungs[i].equiv_x = 0;
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
        r->band_cx[0] = r->band_cx[1] = r->band_cx[2] = 0;

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

        /* The bands themselves, in span order — reported before they are
         * summarised, because the summaries below are lossy in a way that
         * matters: min/max record how far the rate moved but not where it
         * was fastest, and the two only look equivalent while the curve
         * rises monotonically. Copied one by one rather than by memcpy so
         * this loop is bounded by `points` and never publishes a band that
         * was not measured this pass. */
        for (uint8_t b = 0; b < points; b++)
            r->band_cx[b] = cx_band[b];

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
    adsc_speeds_admit(out, ncand, points, SPEEDS_ADMIT_K);
    return ACCUDISC_OK;
}
