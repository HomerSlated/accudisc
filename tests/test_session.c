/* Session selection and the audio-range guard. Pure: no device, no hardware.
 *
 * The geometry under test is the real one measured on a PX-716A, 2026-07-22
 * (an Enhanced CD): session 1 holds audio tracks 1-13 and ends at lead-out
 * 195656; session 2 holds data track 14 at 207056 with lead-out 211185. The
 * 11,400 sectors between 195656 and 207056 are session 1's lead-out, session
 * 2's lead-in and track 14's pregap — no payload, unreadable as CD-DA. A model
 * that reports track 13 running all the way to 207056 sends the ripper into
 * them, which is the defect these tests pin down. */

#include <assert.h>
#include <string.h>

#include "toc/toc.h"

static void add(accudisc_toc *t, uint8_t num, uint8_t adr_ctrl, uint8_t session,
                uint32_t lba, uint32_t sectors)
{
    accudisc_track *k = &t->tracks[t->track_count++];

    k->number = num;
    k->adr_ctrl = adr_ctrl;
    k->session = session;
    k->lba = lba;
    k->sectors = sectors;
}

static void add_session(accudisc_toc *t, uint8_t num, uint8_t first,
                        uint8_t last, uint8_t audio, uint8_t data,
                        uint32_t leadout)
{
    accudisc_session *s = &t->sessions[t->session_count++];

    s->number = num;
    s->first_track = first;
    s->last_track = last;
    s->audio_tracks = audio;
    s->data_tracks = data;
    s->leadout_lba = leadout;
}

/* The measured Enhanced CD. */
static void enhanced_cd(accudisc_toc *t)
{
    memset(t, 0, sizeof(*t));
    t->first_track = 1;
    t->last_track = 14;
    t->leadout_lba = 211185;
    /* Tracks 2-12 collapsed into one contiguous span: session 1 must be gap
     * free for the guard's "first impossible sector" answers to mean anything.
     */
    add(t, 1, 0x10, 1, 0, 13320);
    add(t, 2, 0x10, 1, 13320, 170980);
    add(t, 13, 0x10, 1, 184300, 11356); /* to session 1's lead-out, not to 14 */
    add(t, 14, 0x14, 2, 207056, 4129);  /* CTRL bit 2 set: data */
    add_session(t, 1, 1, 13, 3, 0, 195656);
    add_session(t, 2, 14, 14, 0, 1, 211185);
}

int main(void)
{
    /* --- default session: exactly one session has audio -------------------- */
    {
        accudisc_toc t;
        uint32_t lba = 0, count = 0;

        enhanced_cd(&t);
        assert(accudisc_toc_default_audio_session(&t) == 1);

        assert(accudisc_toc_session_range(&t, 1, &lba, &count) == ACCUDISC_OK);
        assert(lba == 0);
        assert(count == 195656); /* through session 1's lead-out, NOT 211185 */

        assert(accudisc_toc_session_range(&t, 2, &lba, &count) == ACCUDISC_OK);
        assert(lba == 207056 && count == 4129);

        assert(accudisc_toc_session_range(&t, 3, &lba, &count) ==
               ACCUDISC_ERR_NOTFOUND);
    }

    /* --- two audio sessions: no defensible default, caller must choose ----- */
    {
        accudisc_toc t;

        memset(&t, 0, sizeof(t));
        t.leadout_lba = 200000;
        add(&t, 1, 0x10, 1, 0, 50000);
        add(&t, 2, 0x10, 2, 80000, 60000);
        add_session(&t, 1, 1, 1, 1, 0, 50000);
        add_session(&t, 2, 2, 2, 1, 0, 140000);
        assert(accudisc_toc_default_audio_session(&t) ==
               ACCUDISC_ERR_UNSUPPORTED);
    }

    /* --- no audio anywhere, and no session structure ----------------------- */
    {
        accudisc_toc t;

        memset(&t, 0, sizeof(t));
        t.leadout_lba = 100000;
        add(&t, 1, 0x14, 1, 0, 100000);
        add_session(&t, 1, 1, 1, 0, 1, 100000);
        assert(accudisc_toc_default_audio_session(&t) == ACCUDISC_ERR_NOTFOUND);

        t.session_count = 0; /* format-0 degrade: structure unknown */
        assert(accudisc_toc_default_audio_session(&t) == ACCUDISC_ERR_INVAL);
        assert(accudisc_toc_default_audio_session(NULL) == ACCUDISC_ERR_INVAL);
    }

    /* --- the range guard --------------------------------------------------- */
    {
        accudisc_toc t;
        accudisc_range_check c;

        enhanced_cd(&t);

        /* Session 1's audio, exactly. */
        assert(accudisc_check_audio_range(&t, 0, 13320, &c) == ACCUDISC_OK);
        assert(c.ok && c.reason == ACCUDISC_RANGE_OK && c.session == 1);

        /* Track 13 at its corrected length. */
        assert(accudisc_check_audio_range(&t, 184300, 11356, &c) ==
               ACCUDISC_OK);

        /* Track 13 at its OLD, wrong length runs into session 1's lead-out.
         * This is the case that hung a real rip. */
        assert(accudisc_check_audio_range(&t, 184300, 22756, &c) !=
               ACCUDISC_OK);
        assert(c.reason == ACCUDISC_RANGE_NOT_IN_TRACK);
        assert(c.first_bad_lba == 195656);

        /* The whole disc, as a caller would naively ask for it. Refused at the
         * first impossible sector — the lead-out, before the data track. */
        assert(accudisc_check_audio_range(&t, 0, 211185, &c) != ACCUDISC_OK);
        assert(c.reason == ACCUDISC_RANGE_NOT_IN_TRACK);
        assert(c.first_bad_lba == 195656);

        /* The data track itself, named exactly. */
        assert(accudisc_check_audio_range(&t, 207056, 4129, &c) != ACCUDISC_OK);
        assert(c.reason == ACCUDISC_RANGE_DATA_TRACK);
        assert(c.track == 14 && c.session == 2);
        assert(c.first_bad_lba == 207056);

        /* Past the disc, and empty. */
        assert(accudisc_check_audio_range(&t, 211185, 1, &c) != ACCUDISC_OK);
        assert(c.reason == ACCUDISC_RANGE_BEYOND_LEADOUT);
        assert(accudisc_check_audio_range(&t, 0, 211186, &c) != ACCUDISC_OK);
        assert(c.reason == ACCUDISC_RANGE_BEYOND_LEADOUT);
        assert(accudisc_check_audio_range(&t, 0, 0, &c) != ACCUDISC_OK);
        assert(c.reason == ACCUDISC_RANGE_EMPTY);

        assert(accudisc_check_audio_range(NULL, 0, 1, &c) == ACCUDISC_ERR_INVAL);
        /* out may be NULL: the return code alone is a valid use. */
        assert(accudisc_check_audio_range(&t, 0, 13320, NULL) == ACCUDISC_OK);
    }

    /* --- crossing a session seam where the tracks happen to abut ----------- */
    {
        accudisc_toc t;
        accudisc_range_check c;

        /* Contrived: two audio sessions with no gap between them, so the only
         * thing separating the tracks is the session number itself. Without
         * the session check this range would look perfectly readable. */
        memset(&t, 0, sizeof(t));
        t.leadout_lba = 100000;
        add(&t, 1, 0x10, 1, 0, 50000);
        add(&t, 2, 0x10, 2, 50000, 50000);
        add_session(&t, 1, 1, 1, 1, 0, 50000);
        add_session(&t, 2, 2, 2, 1, 0, 100000);

        assert(accudisc_check_audio_range(&t, 0, 100000, &c) != ACCUDISC_OK);
        assert(c.reason == ACCUDISC_RANGE_CROSSES_SESSION);
        assert(c.first_bad_lba == 50000 && c.session == 1 && c.track == 2);
    }

    /* --- the format-0 degrade path ----------------------------------------- */
    {
        accudisc_toc t;
        accudisc_range_check c;

        /* All audio, no session structure: safe to walk. There is no seam to
         * fall into, so this keeps a disc with a dead lead-in rippable. */
        memset(&t, 0, sizeof(t));
        t.leadout_lba = 100000;
        add(&t, 1, 0x10, 0, 0, 50000);
        add(&t, 2, 0x10, 0, 50000, 50000);
        assert(accudisc_check_audio_range(&t, 0, 100000, &c) == ACCUDISC_OK);

        /* A data track with no session structure means the disc is almost
         * certainly multi-session and the audio extents cannot be trusted:
         * the last audio track's length runs through a lead-out we cannot
         * see. Refuse rather than guess. */
        t.tracks[1].adr_ctrl = 0x14;
        assert(accudisc_check_audio_range(&t, 0, 50000, &c) != ACCUDISC_OK);
        assert(c.reason == ACCUDISC_RANGE_NO_SESSION_INFO);
        assert(c.track == 2);
    }

    /* --- machine-interface tokens are stable ------------------------------- */
    assert(!strcmp(accudisc_range_reason_str(ACCUDISC_RANGE_OK), "ok"));
    assert(!strcmp(accudisc_range_reason_str(ACCUDISC_RANGE_DATA_TRACK),
                   "data_track"));
    assert(!strcmp(accudisc_range_reason_str(ACCUDISC_RANGE_NOT_IN_TRACK),
                   "not_in_track"));
    assert(!strcmp(accudisc_range_reason_str(ACCUDISC_RANGE_CROSSES_SESSION),
                   "crosses_session"));
    assert(!strcmp(accudisc_range_reason_str(ACCUDISC_RANGE_BEYOND_LEADOUT),
                   "beyond_leadout"));
    assert(!strcmp(accudisc_range_reason_str(ACCUDISC_RANGE_NO_SESSION_INFO),
                   "no_session_info"));
    assert(!strcmp(accudisc_range_reason_str(ACCUDISC_RANGE_EMPTY), "empty"));
    assert(!strcmp(accudisc_range_reason_str(99), "unknown"));

    return 0;
}
