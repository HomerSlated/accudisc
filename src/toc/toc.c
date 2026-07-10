#include <stdlib.h>
#include <string.h>

#include "../mmc/mmc.h"
#include "toc.h"

static uint32_t be32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | p[3];
}

int adsc_toc_parse(const uint8_t *buf, uint32_t len, accudisc_toc *out)
{
    memset(out, 0, sizeof(*out));
    if (len < 4)
        return ACCUDISC_ERR_SHORT;

    uint32_t end = (uint32_t)(((unsigned)buf[0] << 8) | buf[1]) + 2;
    if (end > len)
        end = len;
    out->first_track = buf[2];
    out->last_track = buf[3];

    int have_leadout = 0;
    for (uint32_t off = 4; off + 8 <= end; off += 8) {
        uint8_t track = buf[off + 2];
        uint32_t lba = be32(buf + off + 4);
        if (track == 0xAA) { /* lead-out pseudo-track */
            out->leadout_lba = lba;
            have_leadout = 1;
        } else if (track >= 1 && track <= 99 &&
                   out->track_count < 99) {
            accudisc_track *t = &out->tracks[out->track_count++];
            t->number = track;
            t->adr_ctrl = buf[off + 1];
            t->lba = lba;
        }
    }
    if (!have_leadout)
        return ACCUDISC_ERR_SHORT;

    /* Track extents: to the next track's start; last track to lead-out. */
    for (uint8_t i = 0; i < out->track_count; i++) {
        uint32_t next = (i + 1 < out->track_count) ? out->tracks[i + 1].lba
                                                   : out->leadout_lba;
        out->tracks[i].sectors = next > out->tracks[i].lba
                                     ? next - out->tracks[i].lba : 0;
    }
    return ACCUDISC_OK;
}

int accudisc_read_toc(accudisc_device *dev, accudisc_toc *out)
{
    uint8_t *buf = NULL;
    uint32_t len = 0;
    int rc;

    if (!dev || !out)
        return ACCUDISC_ERR_INVAL;
    rc = adsc_mmc_read_toc_raw(dev, ADSC_TOC_FMT_TOC, 0, 1, &buf, &len);
    if (rc != ACCUDISC_OK)
        return rc;
    rc = adsc_toc_parse(buf, len, out);
    free(buf);
    return rc;
}

int accudisc_read_full_toc(accudisc_device *dev, uint8_t **out, uint32_t *len)
{
    if (!dev || !out || !len)
        return ACCUDISC_ERR_INVAL;
    /* time=1, track = session 1, per redumper cd_common.ixx. */
    return adsc_mmc_read_toc_raw(dev, ADSC_TOC_FMT_FULL, 1, 1, out, len);
}

int accudisc_read_cdtext(accudisc_device *dev, uint8_t **out, uint32_t *len)
{
    if (!dev || !out || !len)
        return ACCUDISC_ERR_INVAL;
    return adsc_mmc_read_toc_raw(dev, ADSC_TOC_FMT_CDTEXT, 0, 0, out, len);
}
