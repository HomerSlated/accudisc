/* Read-range planning: TOC in, plan out.
 *
 * This is the ordering and the defaults that used to live in cli/main.c, moved
 * behind the public ABI so the CLI, Python and Rust all resolve a range the
 * same way instead of each reconstructing the precedence from the man page.
 *
 * Everything here is pure. No device handle reaches this file deliberately —
 * that is what turns "reachable only with the right disc in the drive" into a
 * unit test against a synthetic TOC. */

#include <stdint.h>
#include <string.h>

#include "accudisc/accudisc.h"
#include "internal.h"

/* Every refusal returns the same code. The reason travels in the struct, never
 * in the return value: a caller that branches on ERR_UNSUPPORTED vs ERR_NOTFOUND
 * has to re-derive which of the two session failures it hit, and that is exactly
 * the information loss this function exists to prevent. */
static int refuse(accudisc_range_plan *out, accudisc_plan_reason why)
{
    out->plan_reason = (uint8_t)why;
    return ACCUDISC_ERR_UNSUPPORTED;
}

int accudisc_plan_read_range(const accudisc_toc *toc,
                             const accudisc_range_spec *spec,
                             accudisc_range_plan *out)
{
    /* Signed and wide for the whole computation. Narrowing to uint32_t happens
     * once, at the end, after the count is known to be positive: a count
     * computed in uint32_t wraps when the start lies past the named extent,
     * and a wrapped count is a huge valid-looking span rather than a refusal. */
    int64_t start, count;
    int32_t session;
    int have_start, err;

    if (!out)
        return ACCUDISC_ERR_INVAL;
    memset(out, 0, sizeof *out);
    out->plan_reason = ACCUDISC_PLAN_BAD_ARGUMENT;
    if (!toc || !spec)
        return ACCUDISC_ERR_INVAL;

    /* EXACTLY -1 means "unspecified". Anything below it is a caller error, not
     * a second way of saying the same thing: mapping every negative to
     * "unspecified" turns `--start -5` from a refusal into a silent whole-disc
     * read, which is the failure this whole function is meant to end. */
    if (spec->start < -1 || spec->count < -1)
        return ACCUDISC_ERR_INVAL;

    have_start = spec->start >= 0;
    start = have_start ? spec->start : 0; /* -1 must never survive into the
                                           * lead-out comparison below */
    count = spec->count;                  /* < 0 = unspecified, through the end */
    session = spec->session;

    if (spec->first_track > 0) {
        uint32_t tlba = 0, tcount = 0;

        if (spec->first_track > 99 || spec->last_track < 1 ||
            spec->last_track > 99)
            return ACCUDISC_ERR_INVAL;

        err = accudisc_toc_track_range(toc, (uint8_t)spec->first_track,
                                       (uint8_t)spec->last_track, &tlba,
                                       &tcount);
        if (err == ACCUDISC_ERR_NOTFOUND)
            return refuse(out, ACCUDISC_PLAN_TRACKS_NOT_FOUND);
        if (err == ACCUDISC_ERR_UNSUPPORTED)
            return refuse(out, ACCUDISC_PLAN_TRACKS_CROSS_SESSION);
        if (err != ACCUDISC_OK)
            return refuse(out, ACCUDISC_PLAN_TRACKS_NO_EXTENT);

        if (!have_start)
            start = (int64_t)tlba;
        if (count < 0)
            count = (int64_t)tlba + tcount - start;

        /* Report which session the tracks turned out to be in — track_range
         * has already established they share one. Informational only: a caller
         * choosing how to describe the range should key off what it ASKED for,
         * not off this, or a "tracks A-B" message silently becomes a
         * "session N" one. */
        for (uint8_t i = 0; i < toc->track_count; i++)
            if (toc->tracks[i].number == (uint8_t)spec->first_track) {
                out->session = toc->tracks[i].session;
                break;
            }
        /* and the session branch is skipped entirely — see below */
    } else {
        if (session < 0 && !have_start && count < 0) {
            /* Neither a range nor a session named: take the audio session, if
             * there is exactly one. */
            int s = accudisc_toc_default_audio_session(toc);

            if (s == ACCUDISC_ERR_UNSUPPORTED)
                return refuse(out, ACCUDISC_PLAN_MULTIPLE_AUDIO_SESSIONS);
            if (s == ACCUDISC_ERR_NOTFOUND)
                return refuse(out, ACCUDISC_PLAN_NO_AUDIO_SESSION);
            if (s > 0)
                session = s;
            /* ACCUDISC_ERR_INVAL = no session structure at all (the format-0
             * degrade path): fall through to the flat whole-disc default,
             * which the guard below still vets. */
        }

        if (session > 0) {
            uint32_t slba = 0, scount = 0;

            /* The AUDIO span, not the whole session. On a Mixed Mode CD the
             * session also holds a data track; the whole-session range would
             * include it and the guard would (correctly) refuse the lot. */
            err = accudisc_toc_session_audio_range(toc, (uint8_t)session, &slba,
                                                   &scount);
            if (err == ACCUDISC_ERR_UNSUPPORTED)
                return refuse(out, ACCUDISC_PLAN_SESSION_SPLIT_BY_DATA);
            if (err != ACCUDISC_OK)
                return refuse(out, ACCUDISC_PLAN_SESSION_NOT_FOUND);

            if (!have_start)
                start = (int64_t)slba;
            if (count < 0)
                count = (int64_t)slba + scount - start;
            out->session = (uint8_t)session;
        } else if (count < 0) {
            /* No session structure to work from: run to the lead-out. */
            if (start >= (int64_t)toc->leadout_lba) {
                out->lba = (uint32_t)start;
                return refuse(out, ACCUDISC_PLAN_START_PAST_LEADOUT);
            }
            count = (int64_t)toc->leadout_lba - start;
        }
    }

    out->resolved_count = count;
    if (count <= 0) {
        out->lba = (uint32_t)start;
        return refuse(out, ACCUDISC_PLAN_EMPTY_RANGE);
    }
    /* Only now is the narrowing safe, and only if it fits. The CLI used to
     * truncate a too-large --count silently. */
    if (start > UINT32_MAX || count > UINT32_MAX ||
        start + count > (int64_t)UINT32_MAX + 1)
        return ACCUDISC_ERR_INVAL;

    out->lba = (uint32_t)start;
    out->count = (uint32_t)count;

    if (!spec->force &&
        accudisc_check_audio_range(toc, out->lba, out->count, &out->check) !=
            ACCUDISC_OK)
        return refuse(out, ACCUDISC_PLAN_GUARD_REFUSED);

    out->plan_reason = ACCUDISC_PLAN_OK;
    return ACCUDISC_OK;
}

const char *accudisc_plan_reason_str(unsigned plan_reason)
{
    switch (plan_reason) {
    case ACCUDISC_PLAN_OK:                    return "ok";
    case ACCUDISC_PLAN_TRACKS_NOT_FOUND:      return "tracks_not_found";
    case ACCUDISC_PLAN_TRACKS_CROSS_SESSION:  return "tracks_cross_session";
    case ACCUDISC_PLAN_TRACKS_NO_EXTENT:      return "tracks_no_extent";
    case ACCUDISC_PLAN_MULTIPLE_AUDIO_SESSIONS: return "multiple_audio_sessions";
    case ACCUDISC_PLAN_NO_AUDIO_SESSION:      return "no_audio_session";
    case ACCUDISC_PLAN_SESSION_SPLIT_BY_DATA: return "session_split_by_data";
    case ACCUDISC_PLAN_SESSION_NOT_FOUND:     return "session_not_found";
    case ACCUDISC_PLAN_START_PAST_LEADOUT:    return "start_past_leadout";
    case ACCUDISC_PLAN_EMPTY_RANGE:           return "empty_range";
    case ACCUDISC_PLAN_GUARD_REFUSED:         return "guard_refused";
    case ACCUDISC_PLAN_BAD_ARGUMENT:          return "bad_argument";
    default:                                  return "unknown";
    }
}
