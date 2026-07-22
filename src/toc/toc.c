#include <stdlib.h>
#include <string.h>

#include "../mmc/mmc.h"
#include "toc.h"

static uint32_t be32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | p[3];
}

/* This session's lead-out, falling back to the disc lead-out when session
 * structure is unknown (format-0 path: every track carries session 0). */
static uint32_t session_leadout(const accudisc_toc *t, uint8_t session)
{
    for (uint8_t i = 0; i < t->session_count; i++)
        if (t->sessions[i].number == session)
            return t->sessions[i].leadout_lba;
    return t->leadout_lba;
}

/* Track extents: to the next track in the SAME session, and the session's last
 * track to that session's lead-out. Bounding by the session rather than by the
 * next track on the disc is load-bearing — across a session seam the next
 * track start is ~11,400 sectors further out than the payload actually runs,
 * with a lead-out, a lead-in and a pregap in between. Shared by both parse
 * paths so the two sources cannot disagree on geometry; with no session table
 * (session_count 0) every track is session 0 and this reduces exactly to the
 * old next-track-start behaviour. */
static void toc_fill_extents(accudisc_toc *out)
{
    for (uint8_t i = 0; i < out->track_count; i++) {
        accudisc_track *t = &out->tracks[i];
        uint32_t next;

        if (i + 1 < out->track_count &&
            out->tracks[i + 1].session == t->session)
            next = out->tracks[i + 1].lba;
        else
            next = session_leadout(out, t->session);

        t->sectors = next > t->lba ? next - t->lba : 0;
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
    /* Session accumulators indexed by session number (1..99). */
    accudisc_session sess[100];
    uint8_t sess_seen[100];
    uint8_t first_track = 0, last_track = 0, disc_type = 0;
    uint8_t first_session = 0, last_session = 0;
    int have_leadout = 0;

    if (!ft || !out)
        return ACCUDISC_ERR_INVAL;
    memset(out, 0, sizeof(*out));
    memset(slot, 0, sizeof(slot));
    memset(have, 0, sizeof(have));
    memset(sess, 0, sizeof(sess));
    memset(sess_seen, 0, sizeof(sess_seen));

    for (uint16_t i = 0; i < ft->entry_count; i++) {
        const accudisc_fulltoc_entry *e = &ft->entries[i];
        int32_t lba = accudisc_msf_to_lba(e->pmin, e->psec, e->pframe);

        if (!first_session || e->session < first_session)
            first_session = e->session;
        if (e->session > last_session)
            last_session = e->session;
        if (e->session >= 1 && e->session <= 99 && !sess_seen[e->session]) {
            sess_seen[e->session] = 1;
            sess[e->session].number = e->session;
        }

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
                slot[e->point].session = e->session;
                slot[e->point].lba = (uint32_t)lba;
            }
        } else if (e->point == 0xA0) {
            /* A0/A1/A2 are PER SESSION. The disc-wide first track is the
             * lowest session's A0 and the disc-wide last track the highest
             * session's A1 — taking whichever entry happened to come last
             * would report session 2's first track as the disc's. */
            if (e->session >= 1 && e->session <= 99) {
                sess[e->session].first_track = e->pmin;
                if (!first_track || e->session <= first_session)
                    first_track = e->pmin;
                if (!disc_type || e->session <= first_session)
                    disc_type = e->psec;
            }
        } else if (e->point == 0xA1) {
            if (e->session >= 1 && e->session <= 99) {
                sess[e->session].last_track = e->pmin;
                if (!last_track || e->session >= last_session)
                    last_track = e->pmin;
            }
        } else if (e->point == 0xA2 && lba >= 0) {
            if (e->session >= 1 && e->session <= 99)
                sess[e->session].leadout_lba = (uint32_t)lba;
            /* The disc lead-out is the HIGHEST session's: it bounds the last
             * track. Per-session lead-outs live in the session table, which is
             * what bounds each session's last track. */
            if (!have_leadout || e->session >= last_session) {
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

    /* Session table, ascending. A session is only usable if we know where it
     * ends, so one without an A2 is dropped rather than published with a
     * lead-out of 0 — a caller trusting that would compute a zero-length or
     * wildly wrong extent. Its tracks then fall back to the disc lead-out via
     * session_leadout(), which is the same guess the format-0 path makes. */
    for (unsigned n = 1; n <= 99; n++) {
        if (!sess_seen[n] || !sess[n].leadout_lba)
            continue;
        uint8_t obs_first = 0, obs_last = 0;

        for (uint8_t i = 0; i < out->track_count; i++) {
            if (out->tracks[i].session != n)
                continue;
            if (!obs_first)
                obs_first = out->tracks[i].number;
            obs_last = out->tracks[i].number;
            if (ACCUDISC_TRACK_IS_AUDIO(&out->tracks[i]))
                sess[n].audio_tracks++;
            else
                sess[n].data_tracks++;
        }
        /* Prefer A0/A1; fall back to what we actually placed, so a session
         * whose pointers are missing is still usable. */
        if (!sess[n].first_track)
            sess[n].first_track = obs_first;
        if (!sess[n].last_track)
            sess[n].last_track = obs_last;

        out->sessions[out->session_count++] = sess[n];
    }

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
