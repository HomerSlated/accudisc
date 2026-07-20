/* ATIP disc decode + manufacturer lookup.
 *
 * The catalog is factual public data: ATIP codes (97:SS:FF) are pressed into
 * every CD-R pregroove. It is informed by PlexTools and cross-validated
 * against cdrecord's diskid.c and Nero 2026 (see docs/reference/ATTRIBUTION.md).
 * Regenerate with tools/gen_media_db.py.
 *
 * Matching: on (sec, frame-decade). A manufacturer owns a whole 97:SS:Fx
 * decade range (real discs carry frames like 97:24:01 for a 97:24:00 entry),
 * and different manufacturers share a `sec` across decades — so the frame
 * decade, not the exact frame, is the discriminator. cdrecord uses the same
 * rounding rule.
 */

#include <stdlib.h>

#include "../mmc/mmc.h"

struct atip_mfr {
    uint8_t min, sec, frame;
    const char *name;
};

static const struct atip_mfr atip_db[] = {
#include "media_atip_db.inc"
};

const char *accudisc_atip_manufacturer(uint8_t min, uint8_t sec, uint8_t frame)
{
    size_t n = sizeof(atip_db) / sizeof(atip_db[0]);

    for (size_t i = 0; i < n; i++) {
        if (atip_db[i].min == min && atip_db[i].sec == sec &&
            atip_db[i].frame / 10 == frame / 10)
            return atip_db[i].name;
    }
    return NULL; /* unlisted code — report the raw digits, not a wrong guess */
}

int accudisc_read_atip(accudisc_device *dev, accudisc_atip *out)
{
    uint8_t *buf = NULL;
    uint32_t len = 0;
    int rc;

    if (!dev || !out)
        return ACCUDISC_ERR_INVAL;

    /* READ TOC/PMA/ATIP, format 0x04. Absent ATIP (pressed disc) surfaces as
     * SHORT from the two-step reader; report it as NOTFOUND, not an error. */
    rc = adsc_mmc_read_toc_raw(dev, ADSC_TOC_FMT_ATIP, 0, 0, &buf, &len);
    if (rc != ACCUDISC_OK)
        return rc == ACCUDISC_ERR_SHORT ? ACCUDISC_ERR_NOTFOUND : rc;

    /* Full response: [len:2][res:2] then the ATIP descriptor. Lead-in start
     * (the manufacturer code) is at bytes 8..10, lead-out last-possible start
     * (capacity) at 12..14, disc-type bit at byte 6 bit 6. */
    if (len < 15) {
        free(buf);
        return ACCUDISC_ERR_NOTFOUND;
    }

    out->lead_in_min    = buf[8];
    out->lead_in_sec    = buf[9];
    out->lead_in_frame  = buf[10];
    out->lead_out_min   = buf[12];
    out->lead_out_sec   = buf[13];
    out->lead_out_frame = buf[14];
    out->erasable       = (buf[6] >> 6) & 1; /* ATIP disc type: 1 = CD-RW */
    out->manufacturer   = accudisc_atip_manufacturer(buf[8], buf[9], buf[10]);

    free(buf);
    return ACCUDISC_OK;
}
