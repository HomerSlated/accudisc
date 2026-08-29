/* accudisc_plan_read_range — the read-range resolution that used to live in
 * cli/main.c. Pure: no device, no hardware, no disc.
 *
 * That is the whole point of API_PLAN §5.2. Before this function existed, every
 * branch below (Mixed Mode split, multi-session ambiguity, a degraded lead-in
 * with no session table, a start past the lead-out) was reachable ONLY by
 * putting the right physical disc in the drive, so in practice none of them
 * were tested at all.
 *
 * Coverage rule for this file: EVERY accudisc_plan_reason value must have a
 * synthetic TOC here that produces it. A reason code with no test is either a
 * hole in the planner or dead code, and the only way to find out which is to
 * try to reach it. */

#include <stdio.h>
#include <string.h>

#include "accudisc/accudisc.h"

static int fails;

static void ck(int cond, const char *what)
{
    printf("%-4s %s\n", cond ? "ok" : "FAIL", what);
    if (!cond)
        fails++;
}

/* Assert the refusal reason, and that the return code is the SAME for every
 * refusal — a caller must not be able to branch on it, or the reason codes
 * stop being the only channel and §5.2's guarantee quietly rots. */
static void ck_refused(int rc, const accudisc_range_plan *p,
                       accudisc_plan_reason want, const char *what)
{
    int ok = rc == ACCUDISC_ERR_UNSUPPORTED && p->plan_reason == want;

    printf("%-4s %-38s %s\n", ok ? "ok" : "FAIL", what,
           accudisc_plan_reason_str(p->plan_reason));
    if (!ok)
        fails++;
}

static void add(accudisc_toc *t, uint8_t num, uint8_t adr_ctrl, uint8_t session,
                uint32_t lba, uint32_t sectors)
{
    accudisc_track *k = &t->tracks[t->track_count++];

    k->number = num;
    k->adr_ctrl = adr_ctrl;
    k->session = session;
    k->lba = lba;
    k->sectors = sectors;
    t->first_track = t->tracks[0].number;
    t->last_track = num;
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
    t->sessions_total = t->session_count;
    t->leadout_lba = leadout;
}

#define AUDIO 0x00
#define DATA  0x44

/* Plain Red Book: one session, three audio tracks. */
static void plain(accudisc_toc *t)
{
    memset(t, 0, sizeof *t);
    add(t, 1, AUDIO, 1, 0, 1000);
    add(t, 2, AUDIO, 1, 1000, 2000);
    add(t, 3, AUDIO, 1, 3000, 2000);
    add_session(t, 1, 1, 3, 3, 0, 5000);
}

/* Mixed Mode: data track 1, then audio 2 and 3 in the same session. */
static void mixed_mode(accudisc_toc *t)
{
    memset(t, 0, sizeof *t);
    add(t, 1, DATA, 1, 0, 1000);
    add(t, 2, AUDIO, 1, 1000, 2000);
    add(t, 3, AUDIO, 1, 3000, 2000);
    add_session(t, 1, 1, 3, 2, 1, 5000);
}

/* Two sessions, audio in BOTH: no defensible default. */
static void two_audio_sessions(accudisc_toc *t)
{
    memset(t, 0, sizeof *t);
    add(t, 1, AUDIO, 1, 0, 1000);
    add(t, 2, AUDIO, 1, 1000, 1000);
    add(t, 3, AUDIO, 2, 13500, 1000);
    add_session(t, 1, 1, 2, 2, 0, 2000);
    add_session(t, 2, 3, 3, 1, 0, 14500);
}

/* Audio, data, audio inside one session — legal on the wire, no single range
 * expresses the audio. */
static void split_by_data(accudisc_toc *t)
{
    memset(t, 0, sizeof *t);
    add(t, 1, AUDIO, 1, 0, 1000);
    add(t, 2, DATA, 1, 1000, 1000);
    add(t, 3, AUDIO, 1, 2000, 1000);
    add_session(t, 1, 1, 3, 2, 1, 3000);
}

int main(void)
{
    accudisc_toc t;
    accudisc_range_spec s;
    accudisc_range_plan p;
    int rc;

    /* Every field explicit at every call: a spec built by memset alone means
     * "session 0, tracks 0, start 0, count 0", which is not the unspecified
     * state and would quietly test something else. */
#define SPEC(se, ft, lt, st, cn, fo) \
    (s.size = (uint32_t)sizeof s, \
     s.session = (se), s.first_track = (ft), s.last_track = (lt), \
     s.start = (st), s.count = (cn), s.force = (fo))

    /* ---- the happy paths -------------------------------------------- */

    plain(&t);
    SPEC(-1, -1, -1, -1, -1, 0);
    rc = accudisc_plan_read_range(&t, &s, &p);
    ck(rc == ACCUDISC_OK && p.lba == 0 && p.count == 5000 && p.session == 1,
       "bare read takes the sole audio session, whole disc");

    mixed_mode(&t);
    SPEC(-1, -1, -1, -1, -1, 0);
    rc = accudisc_plan_read_range(&t, &s, &p);
    ck(rc == ACCUDISC_OK && p.lba == 1000 && p.count == 4000,
       "Mixed Mode: the AUDIO span, not the whole session");

    plain(&t);
    SPEC(-1, 2, 3, -1, -1, 0);
    rc = accudisc_plan_read_range(&t, &s, &p);
    ck(rc == ACCUDISC_OK && p.lba == 1000 && p.count == 4000,
       "--tracks 2-3 spans both tracks");

    /* ---- the regression this refactor exists to fix ------------------
     * Measured against the OLD cli/main.c ordering on this exact TOC: the
     * track branch resolved lba 3000 count 2000, then the session branch
     * overwrote start with the session's audio start (1000), leaving count at
     * the track-derived 2000. Result: lba 1000 count 2000 — track 2 and half
     * of track 3, for a request naming track 3.
     *
     * Nothing downstream could catch it. The extent is contiguous audio inside
     * one session, so accudisc_check_audio_range PASSES it. Wrong audio,
     * silently, with a clean exit. */
    mixed_mode(&t);
    SPEC(1, 3, 3, -1, -1, 0);
    rc = accudisc_plan_read_range(&t, &s, &p);
    ck(rc == ACCUDISC_OK && p.lba == 3000 && p.count == 2000,
       "tracks BEAT session: --tracks 3-3 --session 1 is track 3");

    /* ---- one test per reason code ------------------------------------ */

    plain(&t);
    SPEC(-1, 4, 4, -1, -1, 0);
    rc = accudisc_plan_read_range(&t, &s, &p);
    ck_refused(rc, &p, ACCUDISC_PLAN_TRACKS_NOT_FOUND, "track 4 on a 3-track disc");

    two_audio_sessions(&t);
    SPEC(-1, 1, 3, -1, -1, 0);
    rc = accudisc_plan_read_range(&t, &s, &p);
    ck_refused(rc, &p, ACCUDISC_PLAN_TRACKS_CROSS_SESSION, "tracks 1-3 over a seam");

    plain(&t);
    SPEC(-1, 3, 2, -1, -1, 0);
    rc = accudisc_plan_read_range(&t, &s, &p);
    ck_refused(rc, &p, ACCUDISC_PLAN_TRACKS_NO_EXTENT, "last < first");

    two_audio_sessions(&t);
    SPEC(-1, -1, -1, -1, -1, 0);
    rc = accudisc_plan_read_range(&t, &s, &p);
    ck_refused(rc, &p, ACCUDISC_PLAN_MULTIPLE_AUDIO_SESSIONS, "audio in two sessions");

    memset(&t, 0, sizeof t); /* every track marked data — MediaCloQ's shape */
    add(&t, 1, DATA, 1, 0, 1000);
    add_session(&t, 1, 1, 1, 0, 1, 1000);
    SPEC(-1, -1, -1, -1, -1, 0);
    rc = accudisc_plan_read_range(&t, &s, &p);
    ck_refused(rc, &p, ACCUDISC_PLAN_NO_AUDIO_SESSION, "all tracks marked data");

    split_by_data(&t);
    SPEC(1, -1, -1, -1, -1, 0);
    rc = accudisc_plan_read_range(&t, &s, &p);
    ck_refused(rc, &p, ACCUDISC_PLAN_SESSION_SPLIT_BY_DATA, "audio either side of data");

    plain(&t);
    SPEC(7, -1, -1, -1, -1, 0);
    rc = accudisc_plan_read_range(&t, &s, &p);
    ck_refused(rc, &p, ACCUDISC_PLAN_SESSION_NOT_FOUND, "session 7 on a 1-session disc");

    /* Degraded lead-in: tracks, but no session table at all. The default-audio
     * lookup returns ERR_INVAL and the planner falls through to the flat
     * whole-disc default — which is the only route to START_PAST_LEADOUT. */
    plain(&t);
    t.session_count = 0;
    SPEC(-1, -1, -1, 9000, -1, 0);
    rc = accudisc_plan_read_range(&t, &s, &p);
    ck_refused(rc, &p, ACCUDISC_PLAN_START_PAST_LEADOUT, "start past lead-out, no sessions");

    plain(&t);
    t.session_count = 0;
    SPEC(-1, -1, -1, 100, -1, 0);
    rc = accudisc_plan_read_range(&t, &s, &p);
    ck(rc == ACCUDISC_OK && p.lba == 100 && p.count == 4900 && p.session == 0,
       "no session table: flat read to lead-out, session reported 0");

    /* A start beyond the named extent yields a NEGATIVE count. resolved_count
     * is the only field that can still say so; p.count would render it as a
     * huge positive number, which is how a refusal turns into a 4-billion
     * sector read in a caller that trusts the wrong field. */
    plain(&t);
    SPEC(-1, 1, 1, 4000, -1, 0);
    rc = accudisc_plan_read_range(&t, &s, &p);
    ck_refused(rc, &p, ACCUDISC_PLAN_EMPTY_RANGE, "start past the named track");
    ck(p.resolved_count == -3000, "EMPTY_RANGE keeps the negative count visible");

    /* The guard: a range that reaches into the data track. */
    mixed_mode(&t);
    SPEC(-1, -1, -1, 0, 2000, 0);
    rc = accudisc_plan_read_range(&t, &s, &p);
    ck_refused(rc, &p, ACCUDISC_PLAN_GUARD_REFUSED, "range covering a data track");
    ck(p.check.reason == ACCUDISC_RANGE_DATA_TRACK && p.lba == 0 && p.count == 2000,
       "GUARD_REFUSED carries .check AND what was refused");

    /* force skips the guard but NOT the resolution — the two are separate
     * questions and conflating them would mean you could not ask for a CD-DA
     * read of a data track even to watch the drive reject it. */
    mixed_mode(&t);
    SPEC(-1, -1, -1, 0, 2000, 1);
    rc = accudisc_plan_read_range(&t, &s, &p);
    ck(rc == ACCUDISC_OK && p.lba == 0 && p.count == 2000,
       "force reaches into the data track");

    plain(&t);
    SPEC(-1, -1, -1, -1, -1, 0);
    rc = accudisc_plan_read_range(&t, NULL, &p);
    ck(rc == ACCUDISC_ERR_INVAL && p.plan_reason == ACCUDISC_PLAN_BAD_ARGUMENT,
       "NULL spec is INVAL, and *out is still written");
    ck(accudisc_plan_read_range(&t, &s, NULL) == ACCUDISC_ERR_INVAL,
       "NULL out is INVAL, not a crash");

    /* ---- only -1 means unspecified ----------------------------------
     * Mapping every negative to "unspecified" is the tempting shortcut and it
     * turns `--start -5` from a refusal into a silent whole-disc read. */
    plain(&t);
    SPEC(-1, -1, -1, -5, -1, 0);
    ck(accudisc_plan_read_range(&t, &s, &p) == ACCUDISC_ERR_INVAL,
       "start -5 is a bad argument, not 'unspecified'");
    SPEC(-1, -1, -1, -1, -5, 0);
    ck(accudisc_plan_read_range(&t, &s, &p) == ACCUDISC_ERR_INVAL,
       "count -5 is a bad argument, not 'unspecified'");

    /* Narrowing to uint32_t must refuse rather than truncate. The CLI used to
     * cast a too-large --count silently. */
    plain(&t);
    SPEC(-1, -1, -1, 0, 5000000000LL, 1);
    ck(accudisc_plan_read_range(&t, &s, &p) == ACCUDISC_ERR_INVAL,
       "count beyond uint32 is refused, not truncated");

    /* ---- the token table --------------------------------------------
     * Every reason must have its own token: two codes sharing one, or a real
     * code falling through to "unknown", makes the machine interface lie. */
    {
        int i, dup = 0, unk = 0;
        for (i = 0; i <= ACCUDISC_PLAN_BAD_ARGUMENT; i++) {
            const char *a = accudisc_plan_reason_str((unsigned)i);
            if (!strcmp(a, "unknown"))
                unk++;
            for (int j = i + 1; j <= ACCUDISC_PLAN_BAD_ARGUMENT; j++)
                if (!strcmp(a, accudisc_plan_reason_str((unsigned)j)))
                    dup++;
        }
        ck(!unk && !dup, "every plan reason has its own token");
        ck(!strcmp(accudisc_plan_reason_str(9999), "unknown"),
           "an out-of-range reason is 'unknown', never NULL");
    }

    if (fails)
        printf("\n%d failure(s)\n", fails);
    return fails ? 1 : 0;
}
