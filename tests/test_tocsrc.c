/* Full-TOC (READ TOC format 2) -> flat track model, the fallback's rung 1.
 * Pure: no device, no hardware. */

#include <assert.h>
#include <string.h>

#include "toc/toc.h"

/* One 11-byte lead-in descriptor: session, adr/ctrl, tno, point, min/sec/frame,
 * zero, pmin/psec/pframe. Only the p* triple carries the address. */
static void entry(accudisc_fulltoc *ft, uint8_t session, uint8_t adr_ctrl,
                  uint8_t point, uint8_t pmin, uint8_t psec, uint8_t pframe)
{
    accudisc_fulltoc_entry *e = &ft->entries[ft->entry_count++];

    memset(e, 0, sizeof(*e));
    e->session = session;
    e->adr_ctrl = adr_ctrl;
    e->point = point;
    e->pmin = pmin;
    e->psec = psec;
    e->pframe = pframe;
}

/* MSF 00:02:00 is LBA 0 — the 150-sector offset. */
static void msf(uint32_t lba, uint8_t *m, uint8_t *s, uint8_t *f)
{
    uint32_t a = lba + 150;

    *m = (uint8_t)(a / (60 * 75));
    *s = (uint8_t)((a / 75) % 60);
    *f = (uint8_t)(a % 75);
}

static void add_track(accudisc_fulltoc *ft, uint8_t session, uint8_t adr_ctrl,
                      uint8_t point, uint32_t lba)
{
    uint8_t m, s, f;

    msf(lba, &m, &s, &f);
    entry(ft, session, adr_ctrl, point, m, s, f);
}

int main(void)
{
    /* --- single session, three tracks, one of them data ------------------- */
    {
        accudisc_fulltoc ft = {0};
        accudisc_toc toc;
        accudisc_toc_info info;

        ft.first_session = ft.last_session = 1;
        entry(&ft, 1, 0x10, 0xA0, 1, 0x00, 0); /* first track 1, CD-DA */
        entry(&ft, 1, 0x10, 0xA1, 3, 0, 0);    /* last track 3 */
        add_track(&ft, 1, 0x10, 0xA2, 200000); /* lead-out */
        add_track(&ft, 1, 0x10, 1, 0);
        add_track(&ft, 1, 0x10, 2, 50000);
        add_track(&ft, 1, 0x14, 3, 120000); /* data: CTRL bit 2 set */

        assert(adsc_toc_from_fulltoc(&ft, &toc, &info) == ACCUDISC_OK);
        assert(toc.first_track == 1);
        assert(toc.last_track == 3);
        assert(toc.track_count == 3);
        assert(toc.leadout_lba == 200000);

        assert(toc.tracks[0].lba == 0);
        assert(toc.tracks[1].lba == 50000);
        assert(toc.tracks[2].lba == 120000);

        /* Extents run to the next start, last track to lead-out. */
        assert(toc.tracks[0].sectors == 50000);
        assert(toc.tracks[1].sectors == 70000);
        assert(toc.tracks[2].sectors == 80000);

        assert(ACCUDISC_TRACK_IS_AUDIO(&toc.tracks[0]));
        assert(!ACCUDISC_TRACK_IS_AUDIO(&toc.tracks[2]));

        assert(info.first_session == 1 && info.last_session == 1);
        assert(info.disc_type == 0x00);
    }

    /* --- points out of order, and repeated across sessions ---------------- */
    {
        accudisc_fulltoc ft = {0};
        accudisc_toc toc;

        /* The lead-in may list points in any order; a multi-session disc
         * repeats structure per session. Appending blindly would duplicate
         * and mis-order, so slotting by track number is the invariant here. */
        add_track(&ft, 1, 0x10, 2, 50000);
        entry(&ft, 1, 0x10, 0xA0, 1, 0x00, 0);
        add_track(&ft, 1, 0x10, 1, 0);
        add_track(&ft, 1, 0x10, 2, 50000); /* duplicate point */
        entry(&ft, 1, 0x10, 0xA1, 2, 0, 0);
        add_track(&ft, 1, 0x10, 0xA2, 100000);

        assert(adsc_toc_from_fulltoc(&ft, &toc, NULL) == ACCUDISC_OK);
        assert(toc.track_count == 2);
        assert(toc.tracks[0].number == 1 && toc.tracks[0].lba == 0);
        assert(toc.tracks[1].number == 2 && toc.tracks[1].lba == 50000);
    }

    /* --- multi-session: the highest session's lead-out bounds the last track */
    {
        accudisc_fulltoc ft = {0};
        accudisc_toc toc;
        accudisc_toc_info info;

        entry(&ft, 1, 0x10, 0xA0, 1, 0x20, 0); /* CD-ROM XA */
        entry(&ft, 1, 0x10, 0xA1, 1, 0, 0);
        add_track(&ft, 1, 0x10, 0xA2, 60000); /* session 1 lead-out */
        add_track(&ft, 1, 0x10, 1, 0);
        entry(&ft, 2, 0x10, 0xA0, 2, 0x20, 0);
        entry(&ft, 2, 0x10, 0xA1, 2, 0, 0);
        add_track(&ft, 2, 0x10, 0xA2, 150000); /* session 2 lead-out wins */
        add_track(&ft, 2, 0x10, 2, 80000);

        assert(adsc_toc_from_fulltoc(&ft, &toc, &info) == ACCUDISC_OK);
        assert(toc.leadout_lba == 150000);
        assert(toc.track_count == 2);
        assert(toc.tracks[1].sectors == 70000); /* 150000 - 80000 */
        assert(info.first_session == 1 && info.last_session == 2);
        assert(info.disc_type == 0x20);

        /* Session 1's last track is bounded by SESSION 1's lead-out, not by
         * the next track on the disc. Track 2 starts at 80000, but 60000
         * onward is lead-out, lead-in and pregap — 20000 sectors of nothing.
         * Reporting 80000 here is what walks a ripper off the end. */
        assert(toc.tracks[0].sectors == 60000);
        assert(toc.tracks[0].session == 1);
        assert(toc.tracks[1].session == 2);

        assert(toc.session_count == 2);
        assert(toc.sessions[0].number == 1);
        assert(toc.sessions[0].leadout_lba == 60000);
        assert(toc.sessions[0].first_track == 1);
        assert(toc.sessions[0].last_track == 1);
        assert(toc.sessions[0].audio_tracks == 1);
        assert(toc.sessions[0].data_tracks == 0);
        assert(toc.sessions[1].number == 2);
        assert(toc.sessions[1].leadout_lba == 150000);

        /* The disc-wide pointers come from the right sessions: first track
         * from the LOWEST session's A0, last from the HIGHEST session's A1. */
        assert(toc.first_track == 1);
        assert(toc.last_track == 2);
    }

    /* --- session structure with a data second session (Enhanced CD shape) -- */
    {
        accudisc_fulltoc ft = {0};
        accudisc_toc toc;

        /* Sessions deliberately listed highest-first: the disc-wide first
         * track must still come from session 1, not from whichever A0 the
         * drive happened to report first. */
        entry(&ft, 2, 0x14, 0xA0, 14, 0x00, 0);
        entry(&ft, 2, 0x14, 0xA1, 14, 0, 0);
        add_track(&ft, 2, 0x14, 0xA2, 211185);
        add_track(&ft, 2, 0x14, 14, 207056);
        entry(&ft, 1, 0x10, 0xA0, 1, 0x00, 0);
        entry(&ft, 1, 0x10, 0xA1, 13, 0, 0);
        add_track(&ft, 1, 0x10, 0xA2, 195656);
        add_track(&ft, 1, 0x10, 13, 184300);

        assert(adsc_toc_from_fulltoc(&ft, &toc, NULL) == ACCUDISC_OK);
        assert(toc.first_track == 1);
        assert(toc.last_track == 14);
        assert(toc.tracks[0].number == 13);
        assert(toc.tracks[0].sectors == 11356); /* 195656 - 184300 */
        assert(toc.tracks[1].number == 14);
        assert(toc.tracks[1].sectors == 4129);
        assert(toc.sessions[0].audio_tracks == 1);
        assert(toc.sessions[1].data_tracks == 1);
    }

    /* --- a session with no A2 is dropped, not published with lead-out 0 ---- */
    {
        accudisc_fulltoc ft = {0};
        accudisc_toc toc;

        add_track(&ft, 1, 0x10, 1, 0); /* session 1: tracks but no A2 */
        add_track(&ft, 2, 0x10, 2, 80000);
        add_track(&ft, 2, 0x10, 0xA2, 150000);

        assert(adsc_toc_from_fulltoc(&ft, &toc, NULL) == ACCUDISC_OK);
        assert(toc.session_count == 1); /* only session 2 is usable */
        assert(toc.sessions[0].number == 2);
        /* Session 1's track falls back to the disc lead-out, the same guess
         * the format-0 path makes — never to a bogus 0-length extent. */
        assert(toc.tracks[0].sectors == 150000);
    }

    /* --- A0/A1 absent: fall back to the observed track points ------------- */
    {
        accudisc_fulltoc ft = {0};
        accudisc_toc toc;

        add_track(&ft, 1, 0x10, 4, 0);
        add_track(&ft, 1, 0x10, 5, 30000);
        add_track(&ft, 1, 0x10, 0xA2, 90000);

        assert(adsc_toc_from_fulltoc(&ft, &toc, NULL) == ACCUDISC_OK);
        assert(toc.first_track == 4);
        assert(toc.last_track == 5);
    }

    /* --- rejected: no lead-out, and no tracks ----------------------------- */
    {
        accudisc_fulltoc ft = {0};
        accudisc_toc toc;

        add_track(&ft, 1, 0x10, 1, 0); /* tracks but no A2 */
        assert(adsc_toc_from_fulltoc(&ft, &toc, NULL) == ACCUDISC_ERR_SHORT);
    }
    {
        accudisc_fulltoc ft = {0};
        accudisc_toc toc;

        add_track(&ft, 1, 0x10, 0xA2, 90000); /* A2 but no tracks */
        assert(adsc_toc_from_fulltoc(&ft, &toc, NULL) == ACCUDISC_ERR_SHORT);
    }
    {
        accudisc_fulltoc ft = {0};
        accudisc_toc toc;

        assert(adsc_toc_from_fulltoc(&ft, &toc, NULL) == ACCUDISC_ERR_SHORT);
        assert(adsc_toc_from_fulltoc(NULL, &toc, NULL) == ACCUDISC_ERR_INVAL);
        assert(adsc_toc_from_fulltoc(&ft, NULL, NULL) == ACCUDISC_ERR_INVAL);
    }

    /* --- machine-interface tokens are stable ------------------------------ */
    assert(!strcmp(accudisc_toc_source_str(ACCUDISC_TOC_SRC_FULLTOC),
                   "fulltoc"));
    assert(!strcmp(accudisc_toc_source_str(ACCUDISC_TOC_SRC_TOC), "toc"));
    assert(!strcmp(accudisc_toc_degrade_str(ACCUDISC_TOC_DEGRADE_NONE),
                   "none"));
    assert(!strcmp(accudisc_toc_degrade_str(
                       ACCUDISC_TOC_DEGRADE_LEADIN_UNREADABLE),
                   "leadin_unreadable"));
    assert(!strcmp(accudisc_toc_degrade_str(ACCUDISC_TOC_DEGRADE_LEADIN_ABSENT),
                   "leadin_absent"));
    assert(!strcmp(accudisc_toc_degrade_str(
                       ACCUDISC_TOC_DEGRADE_LEADIN_MALFORMED),
                   "leadin_malformed"));
    assert(!strcmp(accudisc_toc_source_str(99), "unknown"));
    assert(!strcmp(accudisc_toc_degrade_str(99), "unknown"));

    return 0;
}
