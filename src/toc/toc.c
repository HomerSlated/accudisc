#include <stdlib.h>
#include <string.h>

#include "../mmc/mmc.h"
#include "toc.h"

static uint32_t be32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | p[3];
}

/* Track extents: to the next track's start; last track to lead-out. Shared by
 * both parse paths so the two sources cannot disagree on geometry. */
static void toc_fill_extents(accudisc_toc *out)
{
    for (uint8_t i = 0; i < out->track_count; i++) {
        uint32_t next = (i + 1 < out->track_count) ? out->tracks[i + 1].lba
                                                   : out->leadout_lba;
        out->tracks[i].sectors = next > out->tracks[i].lba
                                     ? next - out->tracks[i].lba : 0;
    }
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

    toc_fill_extents(out);
    return ACCUDISC_OK;
}

int adsc_toc_from_fulltoc(const accudisc_fulltoc *ft, accudisc_toc *out,
                          accudisc_toc_info *info)
{
    /* Slot by track number rather than appending: the lead-in may list points
     * in any order, and a multi-session disc repeats structure per session, so
     * appending would duplicate and mis-order tracks. */
    accudisc_track slot[100];
    uint8_t have[100];
    uint8_t first_track = 0, last_track = 0, disc_type = 0;
    uint8_t first_session = 0, last_session = 0;
    int have_leadout = 0;

    if (!ft || !out)
        return ACCUDISC_ERR_INVAL;
    memset(out, 0, sizeof(*out));
    memset(slot, 0, sizeof(slot));
    memset(have, 0, sizeof(have));

    for (uint16_t i = 0; i < ft->entry_count; i++) {
        const accudisc_fulltoc_entry *e = &ft->entries[i];
        int32_t lba = accudisc_msf_to_lba(e->pmin, e->psec, e->pframe);

        if (!first_session || e->session < first_session)
            first_session = e->session;
        if (e->session > last_session)
            last_session = e->session;

        if (e->point >= 0x01 && e->point <= 0x63) {
            /* A track start before LBA 0 is not a legal address; the lead-in's
             * own running time (min/sec/frame) is a different field and must
             * not be confused with the payload address. */
            if (lba < 0)
                continue;
            if (!have[e->point]) {
                have[e->point] = 1;
                slot[e->point].number = e->point;
                slot[e->point].adr_ctrl = e->adr_ctrl;
                slot[e->point].lba = (uint32_t)lba;
            }
        } else if (e->point == 0xA0) {
            first_track = e->pmin;
            disc_type = e->psec;
        } else if (e->point == 0xA1) {
            last_track = e->pmin;
        } else if (e->point == 0xA2) {
            /* Highest session's lead-out wins: on a multi-session disc that is
             * the one bounding the last track. */
            if (lba >= 0 && (!have_leadout || e->session >= last_session)) {
                out->leadout_lba = (uint32_t)lba;
                have_leadout = 1;
            }
        }
    }

    for (unsigned n = 1; n <= 99; n++) {
        if (have[n] && out->track_count < 99)
            out->tracks[out->track_count++] = slot[n];
    }

    if (!have_leadout || !out->track_count)
        return ACCUDISC_ERR_SHORT;

    /* Prefer A0/A1 when present; fall back to the observed track points, so a
     * lead-in missing those pointers still yields a usable TOC. */
    out->first_track = first_track ? first_track : out->tracks[0].number;
    out->last_track =
        last_track ? last_track : out->tracks[out->track_count - 1].number;

    toc_fill_extents(out);

    if (info) {
        info->first_session = first_session;
        info->last_session = last_session;
        info->disc_type = disc_type;
    }
    return ACCUDISC_OK;
}

const char *accudisc_toc_source_str(unsigned source)
{
    switch (source) {
    case ACCUDISC_TOC_SRC_FULLTOC: return "fulltoc";
    case ACCUDISC_TOC_SRC_TOC:     return "toc";
    default:                       return "unknown";
    }
}

const char *accudisc_toc_degrade_str(unsigned degrade)
{
    switch (degrade) {
    case ACCUDISC_TOC_DEGRADE_NONE:              return "none";
    case ACCUDISC_TOC_DEGRADE_LEADIN_UNREADABLE: return "leadin_unreadable";
    case ACCUDISC_TOC_DEGRADE_LEADIN_ABSENT:     return "leadin_absent";
    case ACCUDISC_TOC_DEGRADE_LEADIN_MALFORMED:  return "leadin_malformed";
    default:                                     return "unknown";
    }
}

int accudisc_read_toc_src(accudisc_device *dev, accudisc_toc *out,
                          accudisc_toc_info *info)
{
    accudisc_toc_info local = {0};
    uint8_t *raw = NULL;
    uint32_t len = 0;
    int rc;

    if (!dev || !out)
        return ACCUDISC_ERR_INVAL;

    /* Rung 1: the full TOC, for session structure. */
    rc = accudisc_read_full_toc(dev, &raw, &len);
    if (rc == ACCUDISC_OK) {
        accudisc_fulltoc ft;
        int prc = accudisc_fulltoc_parse(raw, len, &ft);

        if (prc == ACCUDISC_OK)
            prc = adsc_toc_from_fulltoc(&ft, out, &local);
        free(raw);
        if (prc == ACCUDISC_OK) {
            local.source = ACCUDISC_TOC_SRC_FULLTOC;
            local.degrade = ACCUDISC_TOC_DEGRADE_NONE;
            local.degrade_err = 0;
            if (info)
                *info = local;
            return ACCUDISC_OK;
        }
        /* The drive answered but the lead-in did not yield a usable TOC. */
        memset(&local, 0, sizeof(local));
        local.degrade = ACCUDISC_TOC_DEGRADE_LEADIN_MALFORMED;
        local.degrade_err = prc;
    } else {
        /* NOTFOUND means the drive answered "no data of this format"; anything
         * else means the lead-in could not be read at all. The distinction is
         * the disc-health signal, so it is preserved rather than flattened. */
        local.degrade = (rc == ACCUDISC_ERR_NOTFOUND)
                            ? ACCUDISC_TOC_DEGRADE_LEADIN_ABSENT
                            : ACCUDISC_TOC_DEGRADE_LEADIN_UNREADABLE;
        local.degrade_err = rc;
    }

    /* Rung 2: the cooked TOC. Boundaries and lead-out — everything a checksum
     * needs, and all that offset rescue requires. */
    rc = accudisc_read_toc(dev, out);
    if (rc != ACCUDISC_OK)
        return rc; /* both paths failed: the caller has nothing */

    local.source = ACCUDISC_TOC_SRC_TOC;
    local.first_session = local.last_session = 0;
    local.disc_type = 0;
    if (info)
        *info = local;
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
