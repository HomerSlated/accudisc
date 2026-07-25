/* Pregap / index scan: read each track boundary's neighbourhood and decode it.
 *
 * Moved out of cli/main.c per API_PLAN §5.1. What was actually private was not
 * the loop but the POLICY — the 400/4 window, and the fact that boundaries are
 * read separately so the seek between them defeats the drive cache. Both are
 * now stated in the header, because both change what the reported numbers mean.
 */

#include <stdlib.h>
#include <string.h>

#include "../internal.h"
#include "accudisc/accudisc.h"

/* Collects the 96-byte raw subchannel of each delivered sector into a flat
 * per-boundary buffer, indexed by LBA. Sectors outside the window are ignored
 * rather than trusted: the engine may deliver an overlap. */
struct sub_collector {
    uint8_t *buf;
    uint32_t base;
    uint32_t count;
};

static int sub_collect_sink(void *user, const accudisc_chunk *c)
{
    struct sub_collector *sc = user;

    if (!c->sub_len)
        return 0;
    for (uint32_t s = 0; s < c->nsec; s++) {
        uint32_t lba = c->lba + s;
        if (lba < sc->base || lba - sc->base >= sc->count)
            continue;
        const uint8_t *sec = c->data + (size_t)s * c->sector_len;
        memcpy(sc->buf + (size_t)(lba - sc->base) * 96,
               sec + c->audio_len + c->c2_len, 96);
    }
    return 0;
}

/* Page 2A reports kB/s and accudisc_set_speed takes Nx, so a restore has to
 * cross a unit boundary that does not divide evenly in general. We therefore
 * restore ONLY when the prior figure converts exactly — 7056/176 = 40 does,
 * an odd figure would not — and otherwise leave the speed we were given rather
 * than write back a rounded value the drive never reported. Leaving a known
 * speed beats silently substituting a nearby one. */
#define KBPS_PER_X 176u

static int speed_prior_x(accudisc_device *dev)
{
    unsigned max_kbps = 0, cur_kbps = 0;

    if (accudisc_get_speed(dev, &max_kbps, &cur_kbps) != ACCUDISC_OK)
        return -1;
    if (cur_kbps == 0 || cur_kbps % KBPS_PER_X != 0)
        return -1; /* not exactly representable as Nx: do not guess */
    return (int)(cur_kbps / KBPS_PER_X);
}

int accudisc_scan_pregaps(accudisc_device *dev, const accudisc_toc *toc,
                          const accudisc_pregap_scan_opts *opts,
                          accudisc_index_map *out, uint8_t max, uint8_t *n_out)
{
    accudisc_pregap_scan_opts o = {0};
    uint8_t *buf = NULL;
    uint32_t window, tail;
    int prior_x = -1, rc = ACCUDISC_OK;
    uint8_t n = 0;

    if (n_out)
        *n_out = 0;
    if (!dev || !toc || !out || max == 0 || !n_out)
        return ACCUDISC_ERR_INVAL;
    if (opts)
        o = *opts;

    window = o.window ? o.window : ACCUDISC_PREGAP_WINDOW;
    tail = o.tail ? o.tail : ACCUDISC_PREGAP_TAIL;

    buf = malloc((size_t)(window + tail) * 96);
    if (!buf)
        return ACCUDISC_ERR_NOMEM;

    /* Speed is drive state: record the prior BEFORE setting, so a set that
     * partially applied still has something to go back to. Same discipline as
     * accudisc_speed_uncap_push. */
    if (o.speed_x) {
        prior_x = speed_prior_x(dev);
        accudisc_set_speed(dev, o.speed_x);
    }

    for (uint8_t i = 0; i < toc->track_count && n < max; i++) {
        const accudisc_track *t = &toc->tracks[i];
        uint32_t L = t->lba;
        uint32_t start = L > window ? L - window : 0;
        uint32_t count = L - start + tail;
        struct sub_collector sc = { buf, start, count };
        accudisc_toc one;
        accudisc_read_req req;
        uint32_t got;

        if (o.cancel && *o.cancel) {
            rc = ACCUDISC_ERR_CANCELLED;
            goto out;
        }

        /* The window is sized from the defaults, but a caller may pass a
         * larger one; the buffer was sized for window+tail, and start is
         * clamped at 0, so count can only ever be <= window+tail. */
        memset(buf, 0, (size_t)count * 96);

        memset(&req, 0, sizeof req);
        req.lba = start;
        req.count = count;
        req.sub = ACCUDISC_SUB_RAW;
        rc = accudisc_read_cdda(dev, &req, sub_collect_sink, &sc, NULL);
        if (rc != ACCUDISC_OK)
            goto out; /* first failure ends the scan — see the header */

        /* A single-track TOC, so the decoder reports just this boundary. */
        memset(&one, 0, sizeof one);
        one.first_track = t->number;
        one.last_track = t->number;
        one.track_count = 1;
        one.tracks[0] = *t;

        /* The count is checked, not assumed. It is 1 for a one-track TOC
         * today, but n_out is this function's only honest statement about how
         * many entries the caller may read, and deriving it from the loop
         * counter instead would make it a promise nothing verifies. */
        got = accudisc_index_map_decode(buf, (int32_t)start, count, &one,
                                        &out[n], 1);
        if (got == 1)
            n++;
    }

out:
    /* Restore on EVERY path — success, read failure, cancel. Whoever changes
     * drive state owns putting it back. */
    if (o.speed_x && prior_x > 0)
        accudisc_set_speed(dev, (unsigned)prior_x);
    free(buf);
    *n_out = n;
    return rc;
}
