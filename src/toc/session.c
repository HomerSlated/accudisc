/* Session selection and the audio-range guard: pure functions over a parsed
 * TOC. Nothing here touches hardware.
 *
 * These exist because a multi-session disc is not a contiguous program area.
 * The seam between sessions holds a lead-out, a lead-in and a pregap — around
 * 11,400 sectors on a typical Enhanced CD — none of which is track payload and
 * none of which can be read as CD-DA. A data track cannot be read as CD-DA
 * either, and the drive's refusal is CATEGORICAL: rereading never helps, so a
 * range covering one is not a hard disc to rip, it is an impossible request.
 * Catching that here costs one pass over at most 99 descriptors; discovering
 * it at the drive costs a per-sector retry storm with a seek between attempts.
 */

#include <stddef.h>

#include <accudisc/accudisc.h>

#include "toc.h"

static const accudisc_session *find_session(const accudisc_toc *toc, uint8_t n)
{
    for (uint8_t i = 0; i < toc->session_count; i++)
        if (toc->sessions[i].number == n)
            return &toc->sessions[i];
    return NULL;
}

int accudisc_toc_default_audio_session(const accudisc_toc *toc)
{
    int found = 0;
    uint8_t which = 0;

    if (!toc)
        return ACCUDISC_ERR_INVAL;
    if (!toc->session_count)
        return ACCUDISC_ERR_INVAL; /* structure unknown; caller must not guess */

    for (uint8_t i = 0; i < toc->session_count; i++) {
        if (!toc->sessions[i].audio_tracks)
            continue;
        found++;
        if (found == 1)
            which = toc->sessions[i].number;
    }
    if (!found)
        return ACCUDISC_ERR_NOTFOUND;
    /* Two or more audio sessions have no defensible default — picking one
     * would silently discard the other. The caller chooses. */
    if (found > 1)
        return ACCUDISC_ERR_UNSUPPORTED;
    return (int)which;
}

int accudisc_toc_session_range(const accudisc_toc *toc, uint8_t session,
                               uint32_t *lba, uint32_t *count)
{
    const accudisc_session *s;
    uint32_t start = 0;
    int have_start = 0;

    if (!toc || !lba || !count)
        return ACCUDISC_ERR_INVAL;
    s = find_session(toc, session);
    if (!s)
        return ACCUDISC_ERR_NOTFOUND;

    for (uint8_t i = 0; i < toc->track_count; i++) {
        if (toc->tracks[i].session != session)
            continue;
        if (!have_start || toc->tracks[i].lba < start) {
            start = toc->tracks[i].lba;
            have_start = 1;
        }
    }
    if (!have_start)
        return ACCUDISC_ERR_NOTFOUND;
    if (s->leadout_lba <= start)
        return ACCUDISC_ERR_SHORT; /* lead-out at or before the first track */

    *lba = start;
    *count = s->leadout_lba - start;
    return ACCUDISC_OK;
}

int accudisc_toc_session_audio_range(const accudisc_toc *toc, uint8_t session,
                                     uint32_t *lba, uint32_t *count)
{
    const accudisc_track *first = NULL, *last = NULL;

    if (!toc || !lba || !count)
        return ACCUDISC_ERR_INVAL;
    if (!find_session(toc, session))
        return ACCUDISC_ERR_NOTFOUND;

    for (uint8_t i = 0; i < toc->track_count; i++) {
        const accudisc_track *t = &toc->tracks[i];

        if (t->session != session)
            continue;
        if (!ACCUDISC_TRACK_IS_AUDIO(t)) {
            /* A data track AFTER audio has already begun means the audio is
             * split. Mixed Mode puts its data track first, so this does not
             * fire there — but the wire permits it and a silently truncated
             * rip would be worse than a refusal. */
            if (first)
                return ACCUDISC_ERR_UNSUPPORTED;
            continue;
        }
        if (!first)
            first = t;
        last = t;
    }
    if (!first)
        return ACCUDISC_ERR_NOTFOUND;
    if (!last->sectors)
        return ACCUDISC_ERR_SHORT;

    *lba = first->lba;
    *count = last->lba + last->sectors - first->lba;
    return ACCUDISC_OK;
}

int accudisc_toc_track_range(const accudisc_toc *toc, uint8_t first,
                             uint8_t last, uint32_t *lba, uint32_t *count)
{
    const accudisc_track *a = NULL, *b = NULL;

    if (!toc || !lba || !count || last < first)
        return ACCUDISC_ERR_INVAL;

    for (uint8_t i = 0; i < toc->track_count; i++) {
        if (toc->tracks[i].number == first)
            a = &toc->tracks[i];
        if (toc->tracks[i].number == last)
            b = &toc->tracks[i];
    }
    if (!a || !b)
        return ACCUDISC_ERR_NOTFOUND;
    /* Extents are session-bounded, so a span crossing a session seam would
     * silently include the lead-out/lead-in gap between them. */
    if (a->session != b->session)
        return ACCUDISC_ERR_UNSUPPORTED;
    if (!b->sectors || b->lba + b->sectors <= a->lba)
        return ACCUDISC_ERR_SHORT;

    *lba = a->lba;
    *count = b->lba + b->sectors - a->lba;
    return ACCUDISC_OK;
}

const char *accudisc_range_reason_str(unsigned reason)
{
    switch (reason) {
    case ACCUDISC_RANGE_OK:              return "ok";
    case ACCUDISC_RANGE_DATA_TRACK:      return "data_track";
    case ACCUDISC_RANGE_NOT_IN_TRACK:    return "not_in_track";
    case ACCUDISC_RANGE_CROSSES_SESSION: return "crosses_session";
    case ACCUDISC_RANGE_BEYOND_LEADOUT:  return "beyond_leadout";
    case ACCUDISC_RANGE_NO_SESSION_INFO: return "no_session_info";
    case ACCUDISC_RANGE_SESSION_UNMAPPED: return "session_unmapped";
    case ACCUDISC_RANGE_EMPTY:           return "empty";
    case ACCUDISC_RANGE_TOC_UNTRUSTED:   return "toc_untrusted";
    default:                             return "unknown";
    }
}

const char *accudisc_toc_anomaly_str(unsigned bit)
{
    switch (bit) {
    case ACCUDISC_TOC_ANOM_LBA_ORDER:      return "lba_order";
    case ACCUDISC_TOC_ANOM_OVERLAP:        return "overlap";
    case ACCUDISC_TOC_ANOM_LEADOUT_BEFORE: return "leadout_before";
    case ACCUDISC_TOC_ANOM_PAST_LEADOUT:   return "past_leadout";
    case ACCUDISC_TOC_ANOM_EMPTY_TRACK:    return "empty_track";
    case ACCUDISC_TOC_ANOM_NEGATIVE_LBA:   return "negative_lba";
    case ACCUDISC_TOC_ANOM_BAD_TRACK_NUM:  return "bad_track_num";
    case ACCUDISC_TOC_ANOM_RANGE_MISMATCH: return "range_mismatch";
    case ACCUDISC_TOC_ANOM_BAD_SESSION:    return "bad_session";
    default:                               return "unknown";
    }
}

/* The track owning lba, or NULL. Extents are already session-bounded by
 * toc_fill_extents(), so a sector in a session's lead-out or in the seam
 * between sessions belongs to no track — which is exactly the answer we want.
 */
static const accudisc_track *track_at(const accudisc_toc *toc, uint32_t lba)
{
    for (uint8_t i = 0; i < toc->track_count; i++) {
        const accudisc_track *t = &toc->tracks[i];
        if (lba >= t->lba && lba < t->lba + t->sectors)
            return t;
    }
    return NULL;
}

static int reject(accudisc_range_check *out, uint8_t reason, uint8_t session,
                  uint8_t track, uint32_t bad)
{
    out->ok = 0;
    out->reason = reason;
    out->session = session;
    out->track = track;
    out->first_bad_lba = bad;
    return ACCUDISC_ERR_UNSUPPORTED;
}

int accudisc_check_audio_range(const accudisc_toc *toc, uint32_t lba,
                               uint32_t count, accudisc_range_check *out)
{
    accudisc_range_check local;
    uint8_t session = 0;

    if (!toc)
        return ACCUDISC_ERR_INVAL;
    if (!out)
        out = &local;
    out->ok = 0;
    out->reason = ACCUDISC_RANGE_OK;
    out->session = 0;
    out->track = 0;
    out->first_bad_lba = 0;

    if (!count)
        return reject(out, ACCUDISC_RANGE_EMPTY, 0, 0, lba);

    /* Before anything is checked AGAINST the map, ask whether the map can be
     * believed. Copy protection works by malforming the lead-in (Kaspersky,
     * "CD Cracking Uncovered", ch. 6-7), and a track map derived from a TOC
     * that contradicts itself may report a span as audio when it is not —
     * verified on a synthetic TOC: with tracks numbered in ascending order but
     * addressed out of order, a data track's extent collapsed to zero, hiding
     * it, while its neighbour's extent stretched across the region it left.
     * Vetting a range against such a map is worse than not vetting it, because
     * the answer looks authoritative. So it is refused, and --force remains the
     * caller's deliberate way past. */
    if (toc->anomalies & ACCUDISC_TOC_ANOM_UNTRUSTED_GEOMETRY)
        return reject(out, ACCUDISC_RANGE_TOC_UNTRUSTED, 0, 0, lba);
    if (lba >= toc->leadout_lba || count > toc->leadout_lba - lba)
        return reject(out, ACCUDISC_RANGE_BEYOND_LEADOUT, 0, 0,
                      lba < toc->leadout_lba ? toc->leadout_lba : lba);

    /* No MAPPED sessions: the format-0 degrade path. What is safe here turns
     * entirely on whether anything could tell us how many sessions exist —
     * READ DISC INFORMATION answers that from the drive's disc model, so it
     * survives a lead-in that will not read.
     *
     *   total > 1  the seams are KNOWN to exist and their positions are not.
     *              Format 0 hands back the last session's lead-out, so the
     *              final track's extent is wrong and every seam is invisible.
     *              This is the multi-session all-audio case that a track
     *              census provably cannot detect. Refuse outright.
     *   total == 0 nobody could say. A flat all-audio list is still safe to
     *              walk (no seam to fall into), which keeps a single-session
     *              disc with a dead lead-in rippable; a data track implies
     *              multi-session strongly enough to refuse.
     *
     * total == 1 never reaches here: a single session is fully reconstructible
     * and accudisc_read_toc_src() maps it. */
    if (!toc->session_count) {
        if (toc->sessions_total > 1)
            return reject(out, ACCUDISC_RANGE_SESSION_UNMAPPED, 0, 0, lba);
        for (uint8_t i = 0; i < toc->track_count; i++)
            if (!ACCUDISC_TRACK_IS_AUDIO(&toc->tracks[i]))
                return reject(out, ACCUDISC_RANGE_NO_SESSION_INFO, 0,
                              toc->tracks[i].number, lba);
    }

    for (uint32_t p = lba; p < lba + count; p++) {
        const accudisc_track *t = track_at(toc, p);

        if (!t)
            return reject(out, ACCUDISC_RANGE_NOT_IN_TRACK, session, 0, p);
        if (!ACCUDISC_TRACK_IS_AUDIO(t))
            return reject(out, ACCUDISC_RANGE_DATA_TRACK, t->session,
                          t->number, p);
        if (!session)
            session = t->session;
        else if (t->session != session)
            return reject(out, ACCUDISC_RANGE_CROSSES_SESSION, session,
                          t->number, p);

        /* Whole tracks at a time: every sector of a track shares its type and
         * session, so there is nothing to learn from the other 500,000. */
        p = t->lba + t->sectors - 1;
    }

    out->ok = 1;
    out->reason = ACCUDISC_RANGE_OK;
    out->session = session;
    return ACCUDISC_OK;
}
