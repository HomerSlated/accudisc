/* Malformed and deliberately hostile TOCs. Pure: no device, no hardware.
 *
 * Copy-protection schemes do not corrupt discs by accident — they malform the
 * lead-in on purpose, and a CD-DA tool will meet them in the wild. The
 * taxonomy exercised here is from Kris Kaspersky, "CD Cracking Uncovered"
 * (ch. 6 "Anti-Copying Mechanisms", ch. 7 "Preventing Playback in PC CD-ROM").
 *
 * The design rule under test is that a malformed TOC must fail INFORMATIVELY.
 * Two ways of failing are unacceptable and both are checked for:
 *
 *   - crashing or reading out of bounds on hostile input;
 *   - silently "helpfully" normalising a contradictory TOC into a plausible
 *     one, because the audio-range guard then vets a map that does not
 *     describe the disc and hands back an authoritative-looking wrong answer.
 *
 * The second is the dangerous one, and the reason for accudisc_toc.anomalies.
 */

#include <assert.h>
#include <string.h>

#include "toc/toc.h"

static void ent(accudisc_fulltoc *f, uint8_t sess, uint8_t ctrl, uint8_t point,
                int32_t lba)
{
    accudisc_fulltoc_entry *e = &f->entries[f->entry_count++];
    int32_t m = lba + 150; /* MSF is offset by the 2-second pregap */

    memset(e, 0, sizeof(*e));
    e->session = sess;
    e->adr_ctrl = ctrl;
    e->point = point;
    assert(m >= 0); /* negative addresses are built by hand — see msf() */
    e->pmin = (uint8_t)(m / (60 * 75));
    e->psec = (uint8_t)((m / 75) % 60);
    e->pframe = (uint8_t)(m % 75);
}

/* A raw MSF entry. Needed for addresses BELOW LBA 0: MSF is unsigned and the
 * origin sits at 00:02:00, so lba = (m*60+s)*75 + f - 150 is negative exactly
 * when the address is under two seconds. 00:00:00 is LBA -150. */
static void msf(accudisc_fulltoc *f, uint8_t sess, uint8_t ctrl, uint8_t point,
                uint8_t mm, uint8_t ss, uint8_t ff)
{
    accudisc_fulltoc_entry *e = &f->entries[f->entry_count++];

    memset(e, 0, sizeof(*e));
    e->session = sess;
    e->adr_ctrl = ctrl;
    e->point = point;
    e->pmin = mm;
    e->psec = ss;
    e->pframe = ff;
}

/* A0/A1 name track numbers in pmin rather than an address. */
static void ptr(accudisc_fulltoc *f, uint8_t sess, uint8_t point, uint8_t num)
{
    accudisc_fulltoc_entry *e = &f->entries[f->entry_count++];

    memset(e, 0, sizeof(*e));
    e->session = sess;
    e->adr_ctrl = 0x10;
    e->point = point;
    e->pmin = num;
}

static void base(accudisc_fulltoc *f, uint8_t first, uint8_t last,
                 uint32_t leadout)
{
    memset(f, 0, sizeof(*f));
    f->first_session = 1;
    f->last_session = 1;
    ptr(f, 1, 0xA0, first);
    ptr(f, 1, 0xA1, last);
    ent(f, 1, 0x10, 0xA2, (int32_t)leadout);
}

int main(void)
{
    /* --- baseline: a well-formed disc reports NO anomalies ----------------
     * The whole scheme is worthless if it cries wolf on honest media, so this
     * assertion guards every one below. Geometry mirrors the measured Mixed
     * Mode CD-R: data track 1, audio 2-3, one session. */
    {
        accudisc_fulltoc f;
        accudisc_toc t;
        accudisc_range_check c;

        base(&f, 1, 3, 30000);
        ent(&f, 1, 0x14, 1, 0);
        ent(&f, 1, 0x10, 2, 10000);
        ent(&f, 1, 0x10, 3, 20000);
        assert(adsc_toc_from_fulltoc(&f, &t, NULL) == ACCUDISC_OK);
        assert(t.anomalies == 0);
        assert(t.tracks[0].sectors == 10000);
        assert(t.tracks[2].sectors == 10000); /* last track to the lead-out */
        /* The audio half is rippable and the data track still refused. */
        assert(accudisc_check_audio_range(&t, 10000, 20000, &c) == ACCUDISC_OK);
        assert(accudisc_check_audio_range(&t, 0, 30000, &c) != ACCUDISC_OK);
        assert(c.reason == ACCUDISC_RANGE_DATA_TRACK);
    }

    /* --- ch. 6, "Incorrect Starting Address for the Track" -----------------
     * Track numbers ascend; addresses do not. This is the attack that used to
     * DEFEAT the data-track guard: computing extents in track-number order
     * collapsed the out-of-order data track's extent to zero (making it
     * invisible to the map) while its neighbour's extent stretched across the
     * region it vacated, so a span covering data was reported "ok".
     *
     * Two independent defences are asserted, because either alone would do and
     * the pair is deliberate: the geometry must now be CORRECT, and the guard
     * must ALSO refuse a TOC that contradicts itself. */
    {
        accudisc_fulltoc f;
        accudisc_toc t;
        accudisc_range_check c;

        base(&f, 1, 3, 30000);
        ent(&f, 1, 0x10, 1, 0);
        ent(&f, 1, 0x14, 2, 1000); /* DATA */
        ent(&f, 1, 0x10, 3, 500);  /* audio, addressed BELOW track 2 */
        assert(adsc_toc_from_fulltoc(&f, &t, NULL) == ACCUDISC_OK);

        assert(t.anomalies & ACCUDISC_TOC_ANOM_LBA_ORDER);
        assert(t.anomalies & ACCUDISC_TOC_ANOM_UNTRUSTED_GEOMETRY);

        /* Defence 1: extents follow ADDRESS order, so nothing is hidden and
         * nothing overlaps. Track 2 is data and owns [1000,30000). */
        assert(t.tracks[0].lba == 0 && t.tracks[0].sectors == 500);
        assert(t.tracks[2].lba == 500 && t.tracks[2].sectors == 500);
        assert(t.tracks[1].lba == 1000 && t.tracks[1].sectors == 29000);
        assert(!ACCUDISC_TRACK_IS_AUDIO(&t.tracks[1]));

        /* Defence 2: the guard refuses outright rather than vetting against a
         * map built from a self-contradicting TOC. */
        assert(accudisc_check_audio_range(&t, 500, 29500, &c) != ACCUDISC_OK);
        assert(c.reason == ACCUDISC_RANGE_TOC_UNTRUSTED);
    }

    /* --- ch. 6, "Fictitious Track Coinciding with the Genuine Track" ------
     * Two tracks claiming the same sectors. No ordering reconciles it, so it
     * is flagged rather than guessed at. */
    {
        accudisc_fulltoc f;
        accudisc_toc t;
        accudisc_range_check c;

        base(&f, 1, 2, 30000);
        ent(&f, 1, 0x10, 1, 0);
        ent(&f, 1, 0x14, 2, 0); /* same address, opposite type */
        assert(adsc_toc_from_fulltoc(&f, &t, NULL) == ACCUDISC_OK);
        assert(t.anomalies & ACCUDISC_TOC_ANOM_UNTRUSTED_GEOMETRY);
        assert(accudisc_check_audio_range(&t, 0, 30000, &c) != ACCUDISC_OK);
    }

    /* --- ch. 7, "Castrated Lead-Out" --------------------------------------
     * The lead-out pointer aimed back toward the start of the disc. Every
     * extent derived from it is suspect, so this is untrusted rather than
     * merely noted. */
    {
        accudisc_fulltoc f;
        accudisc_toc t;
        accudisc_range_check c;

        base(&f, 1, 2, 2100); /* ~28 s, Kaspersky's worked example */
        ent(&f, 1, 0x10, 1, 5000);
        ent(&f, 1, 0x10, 2, 15000);
        assert(adsc_toc_from_fulltoc(&f, &t, NULL) == ACCUDISC_OK);
        assert(t.anomalies & ACCUDISC_TOC_ANOM_LEADOUT_BEFORE);
        assert(t.anomalies & ACCUDISC_TOC_ANOM_UNTRUSTED_GEOMETRY);
        assert(accudisc_check_audio_range(&t, 5000, 10000, &c) != ACCUDISC_OK);
    }

    /* --- ch. 6, "Fictitious Track in the Lead-Out Area" -------------------
     * A track parked past the lead-out. Reported, but the map still describes
     * the disc: the stray track simply owns no sector, and the real audio
     * stays rippable. Over-refusing here would break a disc that is otherwise
     * completely readable. */
    {
        accudisc_fulltoc f;
        accudisc_toc t;
        accudisc_range_check c;

        base(&f, 1, 3, 20000);
        ent(&f, 1, 0x10, 1, 0);
        ent(&f, 1, 0x10, 2, 10000);
        ent(&f, 1, 0x10, 3, 25000); /* beyond the lead-out */
        assert(adsc_toc_from_fulltoc(&f, &t, NULL) == ACCUDISC_OK);
        assert(t.anomalies & ACCUDISC_TOC_ANOM_PAST_LEADOUT);
        assert(t.anomalies & ACCUDISC_TOC_ANOM_EMPTY_TRACK);
        assert(!(t.anomalies & ACCUDISC_TOC_ANOM_UNTRUSTED_GEOMETRY));
        assert(t.tracks[2].sectors == 0); /* owns nothing */
        assert(accudisc_check_audio_range(&t, 0, 20000, &c) == ACCUDISC_OK);
    }

    /* --- ch. 7, "Negative Starting Address of the First Audio Track" ------
     * A track addressed before LBA 0. The point is dropped rather than allowed
     * to wrap: lba is unsigned downstream, so a wrap would yield a colossal
     * bogus address that passes every plausibility check. */
    {
        accudisc_fulltoc f;
        accudisc_toc t;

        base(&f, 1, 2, 30000);
        msf(&f, 1, 0x10, 1, 0, 0, 0); /* 00:00:00 = LBA -150 */
        ent(&f, 1, 0x10, 2, 10000);
        assert(adsc_toc_from_fulltoc(&f, &t, NULL) == ACCUDISC_OK);
        assert(t.anomalies & ACCUDISC_TOC_ANOM_NEGATIVE_LBA);
        for (uint8_t i = 0; i < t.track_count; i++)
            assert(t.tracks[i].lba < 30000); /* nothing wrapped */
    }

    /* --- ch. 6, "Invalidating Track Numbering" ----------------------------
     * A1 claims a last track that is not present. Reported; the observed list
     * is used, because we cannot know which of the two accounts is honest. */
    {
        accudisc_fulltoc f;
        accudisc_toc t;

        base(&f, 1, 1, 30000);
        f.entries[1].pmin = 9; /* A1 says last track 9 ... */
        ent(&f, 1, 0x10, 1, 0);
        ent(&f, 1, 0x10, 2, 10000); /* ... but only 1 and 2 exist */
        assert(adsc_toc_from_fulltoc(&f, &t, NULL) == ACCUDISC_OK);
        assert(t.anomalies & ACCUDISC_TOC_ANOM_RANGE_MISMATCH);
        assert(t.track_count == 2);
    }

    /* --- ch. 6, "Track with Non-Standard Number" --------------------------
     * A0/A1 naming a track outside 1..99. Flagged, and the observed track list
     * is used instead of the bogus pointer. */
    {
        accudisc_fulltoc f;
        accudisc_toc t;

        base(&f, 1, 1, 30000);
        f.entries[1].pmin = 0xAB; /* A1: not a track number at all */
        ent(&f, 1, 0x10, 1, 0);
        assert(adsc_toc_from_fulltoc(&f, &t, NULL) == ACCUDISC_OK);
        assert(t.anomalies & ACCUDISC_TOC_ANOM_BAD_TRACK_NUM);
        assert(t.last_track == 1); /* fell back to what is actually there */
    }

    /* --- out-of-range session numbers -------------------------------------
     * sess[] is indexed by session number, so this is the bounds case. The
     * entry is dropped, not indexed with. */
    {
        accudisc_fulltoc f;
        accudisc_toc t;

        base(&f, 1, 1, 30000);
        ent(&f, 1, 0x10, 1, 0);
        ent(&f, 200, 0x10, 2, 10000); /* session 200 */
        assert(adsc_toc_from_fulltoc(&f, &t, NULL) == ACCUDISC_OK);
        assert(t.anomalies & ACCUDISC_TOC_ANOM_BAD_SESSION);
    }

    /* --- real-scheme shapes, from the 2026-07-22 protection survey ---------
     * Two shapes reported for commercial schemes were flagged as possible gaps
     * in this taxonomy. Both turn out to be covered; these pin that down so a
     * later change cannot quietly reopen either, and so neither has to wait on
     * acquiring a physical disc to be answered. */

    /* Cactus Data Shield 200, as reported: a second (data) session whose track
     * duplicates an address already claimed in session 1. Distinct in shape
     * from same-session overlap — and caught, because the overlap check
     * compares every pair of tracks without filtering by session. Session-
     * bounded extents mean a legitimate multi-session disc never overlaps, so
     * the broader check costs no false positives. */
    {
        accudisc_fulltoc f;
        accudisc_toc t;
        accudisc_range_check c;

        memset(&f, 0, sizeof(f));
        f.first_session = 1;
        f.last_session = 2;
        ptr(&f, 1, 0xA0, 1);
        ptr(&f, 1, 0xA1, 2);
        ent(&f, 1, 0x10, 0xA2, 50000);
        ent(&f, 1, 0x10, 1, 0);
        ent(&f, 1, 0x10, 2, 25000);
        ptr(&f, 2, 0xA0, 3);
        ptr(&f, 2, 0xA1, 3);
        ent(&f, 2, 0x14, 0xA2, 90000);
        ent(&f, 2, 0x14, 3, 25000); /* claims session 1's address */

        assert(adsc_toc_from_fulltoc(&f, &t, NULL) == ACCUDISC_OK);
        assert(t.anomalies & ACCUDISC_TOC_ANOM_OVERLAP);
        assert(accudisc_check_audio_range(&t, 0, 50000, &c) != ACCUDISC_OK);
        assert(c.reason == ACCUDISC_RANGE_TOC_UNTRUSTED);
    }

    /* Sony key2audio, as reported: THREE sessions, two small data sessions
     * bracketing the audio. Nothing here is malformed — the lead-in does not
     * contradict itself, there is simply more of it than a typical disc — so
     * no anomaly should fire. What must hold is that the model picks the
     * session CONTAINING AUDIO rather than assuming the last (or first)
     * session is the interesting one. */
    {
        accudisc_fulltoc f;
        accudisc_toc t;
        accudisc_range_check c;
        uint32_t lba = 0, count = 0;

        memset(&f, 0, sizeof(f));
        f.first_session = 1;
        f.last_session = 3;
        ptr(&f, 1, 0xA0, 1);
        ptr(&f, 1, 0xA1, 1);
        ent(&f, 1, 0x14, 0xA2, 5000);
        ent(&f, 1, 0x14, 1, 0); /* small data session */
        ptr(&f, 2, 0xA0, 2);
        ptr(&f, 2, 0xA1, 3);
        ent(&f, 2, 0x10, 0xA2, 200000);
        ent(&f, 2, 0x10, 2, 20000); /* the audio, in the MIDDLE session */
        ent(&f, 2, 0x10, 3, 100000);
        ptr(&f, 3, 0xA0, 4);
        ptr(&f, 3, 0xA1, 4);
        ent(&f, 3, 0x14, 0xA2, 260000);
        ent(&f, 3, 0x14, 4, 220000); /* small data session */

        assert(adsc_toc_from_fulltoc(&f, &t, NULL) == ACCUDISC_OK);
        assert(t.anomalies == 0); /* three valid sessions is not malformation */
        assert(t.session_count == 3);
        assert(accudisc_toc_default_audio_session(&t) == 2); /* not 1, not 3 */
        assert(accudisc_toc_session_audio_range(&t, 2, &lba, &count) ==
               ACCUDISC_OK);
        assert(lba == 20000 && count == 180000); /* to session 2's lead-out */
        assert(accudisc_check_audio_range(&t, lba, count, &c) == ACCUDISC_OK);
    }

    /* SunnComm MediaCloQ, as observed in the comp.publish.cdrom FAQ: a PC
     * drive reports "two sessions and 16 data tracks" where a CD player sees
     * 15 AUDIO tracks. So the audio is presented to a computer as DATA.
     *
     * Note what does NOT happen: no anomaly fires, and correctly so — this TOC
     * is entirely self-consistent. It is not malformed, it is LYING, and those
     * are different things. CTRL is the only statement the TOC makes about
     * track type, so nothing in a consistency check can catch it.
     *
     * The guard refuses, which is right — we report what the disc claims — and
     * the CLI is expected to say specifically that every track is marked data
     * and that --force exists. This is the calibration case: a disc that plays
     * in a hi-fi and that we nonetheless decline by default. */
    {
        accudisc_fulltoc f;
        accudisc_toc t;
        accudisc_range_check c;

        memset(&f, 0, sizeof(f));
        f.first_session = 1;
        f.last_session = 2;
        ptr(&f, 1, 0xA0, 1);
        ptr(&f, 1, 0xA1, 15);
        ent(&f, 1, 0x14, 0xA2, 270000);
        for (uint8_t i = 0; i < 15; i++) /* 0x14: CTRL bit 2 set = data */
            ent(&f, 1, 0x14, (uint8_t)(i + 1), (int32_t)i * 18000);
        ptr(&f, 2, 0xA0, 16);
        ptr(&f, 2, 0xA1, 16);
        ent(&f, 2, 0x14, 0xA2, 300000);
        ent(&f, 2, 0x14, 16, 280000);

        assert(adsc_toc_from_fulltoc(&f, &t, NULL) == ACCUDISC_OK);
        assert(t.track_count == 16);
        assert(t.anomalies == 0); /* consistent, just untruthful */
        assert(t.sessions[0].audio_tracks == 0);
        assert(t.sessions[0].data_tracks == 15);
        assert(accudisc_toc_default_audio_session(&t) == ACCUDISC_ERR_NOTFOUND);
        assert(accudisc_check_audio_range(&t, 0, 270000, &c) != ACCUDISC_OK);
        assert(c.reason == ACCUDISC_RANGE_DATA_TRACK);
    }

    /* --- track 1's program-area pregap belongs to track 1 ------------------
     * ECMA-130 §20: a Pause is "a part of an Information Track", so the
     * sectors before INDEX 01 are owned by the track that follows, not by
     * nobody. The program area starts at LBA 0, so a first track whose INDEX 01
     * is at LBA 33 has a 33-sector pregap — ordinary audio that every other
     * ripper captures, and where hidden-track-one audio lives.
     *
     * Reported by cdda2img 2026-07-23: building extents from INDEX 01 alone
     * made the default read start at 33, shifting every LBA against the
     * delivered stream and producing a wrong disc ID. */
    {
        accudisc_fulltoc f;
        accudisc_toc t;
        accudisc_range_check c;
        uint32_t lba = 0, count = 0;

        base(&f, 1, 2, 300000);
        ent(&f, 1, 0x10, 1, 33); /* INDEX 01 at 33, not 0 */
        ent(&f, 1, 0x10, 2, 100000);

        assert(adsc_toc_from_fulltoc(&f, &t, NULL) == ACCUDISC_OK);
        assert(t.anomalies == 0); /* a pregap is not a defect */

        /* lba keeps its meaning — INDEX 01 never moves. */
        assert(t.tracks[0].lba == 33);
        assert(t.tracks[0].pregap == 33);
        assert(t.tracks[1].pregap == 0); /* only the first track qualifies */

        /* The default range now starts at 0, and still ends at the lead-out. */
        assert(accudisc_toc_session_audio_range(&t, 1, &lba, &count) ==
               ACCUDISC_OK);
        assert(lba == 0);
        assert(lba + count == 300000);

        /* Sector 0 is owned, and the whole span passes the guard. */
        assert(accudisc_check_audio_range(&t, 0, 300000, &c) == ACCUDISC_OK);
        assert(accudisc_check_audio_range(&t, 0, 1, &c) == ACCUDISC_OK);

        /* --tracks 1-2 agrees with the default rather than differing by 33. */
        {
            uint32_t tl = 0, tc = 0;

            assert(accudisc_toc_track_range(&t, 1, 2, &tl, &tc) == ACCUDISC_OK);
            assert(tl == lba && tc == count);
        }
    }

    /* A first track at LBA 0 — the ordinary case — gains no pregap, and a
     * Mixed Mode disc's audio range must NOT reach back over the data track. */
    {
        accudisc_fulltoc f;
        accudisc_toc t;
        uint32_t lba = 0, count = 0;

        base(&f, 1, 3, 30000);
        ent(&f, 1, 0x14, 1, 0); /* data at 0 */
        ent(&f, 1, 0x10, 2, 10000);
        ent(&f, 1, 0x10, 3, 20000);

        assert(adsc_toc_from_fulltoc(&f, &t, NULL) == ACCUDISC_OK);
        assert(t.tracks[0].pregap == 0); /* nothing before LBA 0 */
        assert(accudisc_toc_session_audio_range(&t, 1, &lba, &count) ==
               ACCUDISC_OK);
        assert(lba == 10000); /* the data track keeps its sectors */
    }

    /* A LATER session's first track gets no pregap: the space before it is the
     * inter-session lead-out/lead-in/pregap gap, which is not readable as CD-DA
     * and must stay owned by nobody. */
    {
        accudisc_fulltoc f;
        accudisc_toc t;

        memset(&f, 0, sizeof(f));
        f.first_session = 1;
        f.last_session = 2;
        ptr(&f, 1, 0xA0, 1);
        ptr(&f, 1, 0xA1, 1);
        ent(&f, 1, 0x10, 0xA2, 195656);
        ent(&f, 1, 0x10, 1, 33); /* session 1: real pregap */
        ptr(&f, 2, 0xA0, 2);
        ptr(&f, 2, 0xA1, 2);
        ent(&f, 2, 0x14, 0xA2, 211185);
        ent(&f, 2, 0x14, 2, 207056); /* session 2, across the 11,400 gap */

        assert(adsc_toc_from_fulltoc(&f, &t, NULL) == ACCUDISC_OK);
        assert(t.tracks[0].pregap == 33);
        assert(t.tracks[1].pregap == 0); /* NOT 207056 */
    }

    /* --- degenerate input: must not crash ---------------------------------
     * No tracks, no lead-out, and a lead-in whose entries are all control
     * points. Short-circuits rather than indexing an empty track list. */
    {
        accudisc_fulltoc f;
        accudisc_toc t;

        memset(&f, 0, sizeof(f));
        assert(adsc_toc_from_fulltoc(&f, &t, NULL) != ACCUDISC_OK);

        base(&f, 1, 1, 30000); /* pointers but no track points */
        assert(adsc_toc_from_fulltoc(&f, &t, NULL) != ACCUDISC_OK);

        assert(adsc_toc_from_fulltoc(NULL, &t, NULL) == ACCUDISC_ERR_INVAL);
        assert(adsc_toc_from_fulltoc(&f, NULL, NULL) == ACCUDISC_ERR_INVAL);
    }

    /* --- every entry slot filled with hostile data ------------------------
     * Saturates entry_count with points at every value, to shake out indexing
     * assumptions. The only requirement is that it terminates and stays in
     * bounds; whether it parses is beside the point. */
    {
        accudisc_fulltoc f;
        accudisc_toc t;

        memset(&f, 0, sizeof(f));
        f.first_session = 0;
        f.last_session = 255;
        f.entry_count = (uint16_t)(sizeof(f.entries) / sizeof(f.entries[0]));
        for (uint16_t i = 0; i < f.entry_count; i++) {
            f.entries[i].session = (uint8_t)(i * 7);
            f.entries[i].adr_ctrl = (uint8_t)(i * 13);
            f.entries[i].point = (uint8_t)i;
            f.entries[i].pmin = (uint8_t)(i * 3);
            f.entries[i].psec = (uint8_t)(i * 5);
            f.entries[i].pframe = (uint8_t)(i % 75);
        }
        (void)adsc_toc_from_fulltoc(&f, &t, NULL);
    }

    return 0;
}
