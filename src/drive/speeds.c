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

#include "../mmc/mmc.h"

/* Audio-only streaming keeps the measurement about the drive, not the
 * C2/sub plumbing. 27 sectors is the largest transfer under 64 KiB. */
#define SPEEDS_CHUNK 27
/* Per-rung measurement: one second of audio at the requested speed,
 * clamped so slow rungs stay quick and fast rungs stay timeable. */
#define SPEEDS_MIN_SECTORS 150
#define SPEEDS_MAX_SECTORS 2250

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

int accudisc_probe_speed_ladder(accudisc_device *dev, uint32_t lba,
                                uint32_t count, const uint16_t *candidates,
                                uint8_t ncand, accudisc_speed_rung *out)
{
    if (!dev || !candidates || !out || ncand == 0)
        return ACCUDISC_ERR_INVAL;

    uint32_t stride = count / ncand;
    if (stride < SPEEDS_MIN_SECTORS + SPEEDS_CHUNK)
        return ACCUDISC_ERR_INVAL; /* span too small to give each rung a
                                    * cache-fresh window */

    uint8_t *buf = malloc((size_t)SPEEDS_CHUNK * ACCUDISC_BYTES_AUDIO);
    if (!buf)
        return ACCUDISC_ERR_NOMEM;

    for (uint8_t i = 0; i < ncand; i++) {
        uint32_t wlba = lba + (uint32_t)i * stride;
        uint32_t want = (uint32_t)candidates[i] * 75;
        accudisc_speed_rung *r = &out[i];

        if (want < SPEEDS_MIN_SECTORS)
            want = SPEEDS_MIN_SECTORS;
        if (want > SPEEDS_MAX_SECTORS)
            want = SPEEDS_MAX_SECTORS;
        if (want > stride - SPEEDS_CHUNK)
            want = stride - SPEEDS_CHUNK;

        r->requested_x = candidates[i];
        r->reported_x = 0;
        r->measured_cx = 0;

        accudisc_set_speed(dev, candidates[i]); /* best-effort by design */

        /* Warm-up: let the drive recalibrate/spin at the new setting and
         * position the head at the window before the clock starts. */
        stream_span(dev, wlba, SPEEDS_CHUNK, buf);

        double t0 = mono_now();
        uint32_t done = stream_span(dev, wlba + SPEEDS_CHUNK, want, buf);
        double secs = mono_now() - t0;

        if (done && secs > 0) {
            double cx = (double)done / secs / 75.0 * 100.0;
            r->measured_cx = cx > 65535.0 ? 65535 : (uint16_t)(cx + 0.5);
        }

        unsigned max_kbps, cur_kbps;
        if (accudisc_get_speed(dev, &max_kbps, &cur_kbps) == ACCUDISC_OK)
            r->reported_x = (uint16_t)(cur_kbps / 176);
    }

    free(buf);
    return ACCUDISC_OK;
}
