/* accudisc_toc_from_fulltoc_raw (0.50.0): the TOC from a SAVED format-2
 * response, and the promise that makes it worth having — it is the TOC a live
 * accudisc_read_toc_src builds from the same lead-in.
 *
 * That promise is tested against the live path, not restated: a fake drive
 * (the adsc_dev_exec seam) answers READ TOC format 2 with the very bytes the
 * pure call is given, and every field of both results must agree. Asked by
 * cdda2img (correspondence 247): their offline tools hold saved dumps, and
 * a dump that folded differently from a live read would move track 1's pregap
 * — and with it every LBA and the disc ID — only for files read from disk. */

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "internal.h"

/* ---- a lead-in, as bytes ---------------------------------------------------
 * 4-byte header (length excluding itself, first/last session), then 11-byte
 * descriptors: session, adr/ctrl, tno, point, min/sec/frame, zero,
 * pmin/psec/pframe. */

static uint8_t raw[4 + 11 * 40];
static uint32_t raw_len;

static void hdr(uint8_t first, uint8_t last)
{
    memset(raw, 0, sizeof(raw));
    raw[2] = first;
    raw[3] = last;
    raw_len = 4;
}

static void desc(uint8_t session, uint8_t adr_ctrl, uint8_t point, uint8_t pm,
                 uint8_t ps, uint8_t pf)
{
    uint8_t *d = raw + raw_len;

    d[0] = session;
    d[1] = adr_ctrl;
    d[3] = point;
    d[8] = pm;
    d[9] = ps;
    d[10] = pf;
    raw_len += 11;
    raw[0] = (uint8_t)((raw_len - 2) >> 8);
    raw[1] = (uint8_t)(raw_len - 2);
}

/* A track or lead-out address. MSF 00:02:00 is LBA 0. */
static void at(uint8_t session, uint8_t adr_ctrl, uint8_t point, uint32_t lba)
{
    uint32_t a = lba + 150;

    desc(session, adr_ctrl, point, (uint8_t)(a / 4500),
         (uint8_t)((a / 75) % 60), (uint8_t)(a % 75));
}

/* ABBA Gold's shape: one audio session whose track 1 starts at LBA 33. */
static void abba(void)
{
    hdr(1, 1);
    desc(1, 0x10, 0xA0, 1, 0, 0);
    desc(1, 0x10, 0xA1, 2, 0, 0);
    at(1, 0x10, 0xA2, 300000);
    at(1, 0x10, 1, 33);
    at(1, 0x10, 2, 100000);
}

/* The same session 1, plus a data session 2 — Enhanced CD. Only session 1's
 * first track may own a pregap; session_count must come out as 2. */
static void enhanced(void)
{
    hdr(1, 2);
    desc(1, 0x10, 0xA0, 1, 0, 0);
    desc(1, 0x10, 0xA1, 2, 0, 0);
    at(1, 0x10, 0xA2, 195656);
    at(1, 0x10, 1, 33);
    at(1, 0x10, 2, 100000);
    desc(2, 0x14, 0xA0, 3, 0x20, 0);
    desc(2, 0x14, 0xA1, 3, 0, 0);
    at(2, 0x14, 0xA2, 211185);
    at(2, 0x14, 3, 207056);
}

/* ---- the fake drive --------------------------------------------------------
 * READ TOC format 2 answers with raw[]; format 0 with fmt0[] (used only when a
 * test makes format 2 fail to fold); READ DISC INFORMATION and anything else
 * with zeros. */

static uint8_t fmt0[4 + 8 * 4];
static uint32_t fmt0_len;

int __real_adsc_dev_exec(struct accudisc_device *dev, adsc_cmd *cmd);
int __wrap_adsc_dev_exec(struct accudisc_device *dev, adsc_cmd *cmd)
{
    const uint8_t *src = NULL;
    uint32_t n = 0;

    (void)dev;
    cmd->resid = 0;
    cmd->driver_status = 0;
    if (cmd->dir == ADSC_XFER_IN && cmd->buf)
        memset(cmd->buf, 0, cmd->buf_len);
    if (cmd->cdb[0] == 0x43) {
        if ((cmd->cdb[2] & 0x0F) == 2) {
            src = raw;
            n = raw_len;
        } else if ((cmd->cdb[2] & 0x0F) == 0) {
            src = fmt0;
            n = fmt0_len;
        }
    }
    if (src && cmd->buf)
        memcpy(cmd->buf, src, n < cmd->buf_len ? n : cmd->buf_len);
    return ACCUDISC_OK;
}

static int live(accudisc_toc *t, accudisc_toc_info *i)
{
    static struct accudisc_device dev;

    memset(&dev, 0, sizeof(dev));
    return accudisc_read_toc_src(&dev, t, i);
}

/* ---- comparison, field by field -------------------------------------------
 * Not memcmp: struct padding is not part of the contract, and a byte compare
 * that passed or failed on it would test the compiler, not the fold. */

static void same(const accudisc_toc *a, const accudisc_toc *b,
                 const accudisc_toc_info *ia, const accudisc_toc_info *ib)
{
    assert(a->first_track == b->first_track);
    assert(a->last_track == b->last_track);
    assert(a->track_count == b->track_count);
    assert(a->leadout_lba == b->leadout_lba);
    for (unsigned k = 0; k < a->track_count; k++) {
        const accudisc_track *x = &a->tracks[k], *y = &b->tracks[k];

        assert(x->number == y->number);
        assert(x->adr_ctrl == y->adr_ctrl);
        assert(x->session == y->session);
        assert(x->lba == y->lba);
        assert(x->sectors == y->sectors);
        assert(x->pregap == y->pregap);
    }
    assert(a->session_count == b->session_count);
    for (unsigned k = 0; k < a->session_count; k++) {
        const accudisc_session *x = &a->sessions[k], *y = &b->sessions[k];

        assert(x->number == y->number);
        assert(x->first_track == y->first_track);
        assert(x->last_track == y->last_track);
        assert(x->audio_tracks == y->audio_tracks);
        assert(x->data_tracks == y->data_tracks);
        assert(x->leadout_lba == y->leadout_lba);
    }
    assert(a->sessions_total == b->sessions_total);
    assert(a->anomalies == b->anomalies);

    assert(ia->source == ib->source);
    assert(ia->degrade == ib->degrade);
    assert(ia->degrade_err == ib->degrade_err);
    assert(ia->first_session == ib->first_session);
    assert(ia->last_session == ib->last_session);
    assert(ia->session_count == ib->session_count);
    assert(ia->disc_type == ib->disc_type);
}

/* Every byte of a failed call's outputs is zero, whatever was there before. */
static int all_zero(const void *p, size_t n)
{
    const uint8_t *b = p;

    for (size_t k = 0; k < n; k++)
        if (b[k])
            return 0;
    return 1;
}

int main(void)
{
    static accudisc_toc t, lt;
    accudisc_toc_info i, li;

    /* 1. ABBA Gold's geometry, from bytes: track 1 owns LBA 0..32. */
    abba();
    assert(accudisc_toc_from_fulltoc_raw(raw, raw_len, &t, &i) == ACCUDISC_OK);
    assert(t.track_count == 2);
    assert(t.tracks[0].lba == 33 && t.tracks[0].pregap == 33);
    assert(t.tracks[1].pregap == 0);
    assert(t.leadout_lba == 300000);
    assert(i.source == ACCUDISC_TOC_SRC_FULLTOC);
    assert(i.degrade == ACCUDISC_TOC_DEGRADE_NONE && i.degrade_err == 0);
    assert(i.first_session == 1 && i.last_session == 1);
    assert(i.session_count == 1 && t.sessions_total == 1);
    printf("ok   abba from bytes: pregap 33, fulltoc, 1 session\n");

    /* 2. The live path on the same lead-in gives the same TOC. */
    assert(live(&lt, &li) == ACCUDISC_OK);
    same(&t, &lt, &i, &li);
    printf("ok   abba: bytes == live read_toc_src, every field\n");

    /* 3. Two sessions: the count comes from the lead-in, and only session 1's
     *    first track gets a pregap — on both paths. */
    enhanced();
    assert(accudisc_toc_from_fulltoc_raw(raw, raw_len, &t, &i) == ACCUDISC_OK);
    assert(i.session_count == 2 && t.sessions_total == 2);
    assert(t.tracks[0].pregap == 33);
    assert(t.tracks[2].number == 3 && t.tracks[2].pregap == 0);
    assert(live(&lt, &li) == ACCUDISC_OK);
    same(&t, &lt, &i, &li);
    printf("ok   enhanced: 2 sessions, s2 no pregap, bytes == live\n");

    /* 4. Info is optional. */
    abba();
    assert(accudisc_toc_from_fulltoc_raw(raw, raw_len, &t, NULL) ==
           ACCUDISC_OK);
    assert(t.tracks[0].pregap == 33 && t.sessions_total == 1);

    /* 5. Failures: the return says why, and nothing half-written survives. */
    memset(&t, 0xA5, sizeof(t));
    memset(&i, 0xA5, sizeof(i));
    assert(accudisc_toc_from_fulltoc_raw(NULL, 10, &t, &i) ==
           ACCUDISC_ERR_INVAL);
    assert(all_zero(&t, sizeof(t)) && all_zero(&i, sizeof(i)));
    assert(accudisc_toc_from_fulltoc_raw(raw, raw_len, NULL, &i) ==
           ACCUDISC_ERR_INVAL);

    memset(&t, 0xA5, sizeof(t));
    memset(&i, 0xA5, sizeof(i));
    assert(accudisc_toc_from_fulltoc_raw(raw, 3, &t, &i) == ACCUDISC_ERR_SHORT);
    assert(all_zero(&t, sizeof(t)) && all_zero(&i, sizeof(i)));

    /* A lead-in with tracks but no lead-out: the fold gets far enough to
     * write tracks before it refuses, which is the case the zeroing is for. */
    hdr(1, 1);
    desc(1, 0x10, 0xA0, 1, 0, 0);
    desc(1, 0x10, 0xA1, 2, 0, 0);
    at(1, 0x10, 1, 33);
    at(1, 0x10, 2, 100000);
    memset(&t, 0xA5, sizeof(t));
    memset(&i, 0xA5, sizeof(i));
    int no_a2 = accudisc_toc_from_fulltoc_raw(raw, raw_len, &t, &i);
    assert(no_a2 == ACCUDISC_ERR_SHORT);
    assert(all_zero(&t, sizeof(t)) && all_zero(&i, sizeof(i)));
    printf("ok   failures: INVAL, SHORT, outputs zeroed\n");

    /* 6. The live path still degrades on the same bytes, and records the very
     *    error the pure call returns as its degrade_err — the refactor must
     *    not have changed what a malformed lead-in reports. Format 0 answers
     *    with a plain one-track TOC. */
    memset(fmt0, 0, sizeof(fmt0));
    fmt0[1] = 2 + 8 * 2;
    fmt0[2] = 1;
    fmt0[3] = 1;
    fmt0[5] = 0x10; fmt0[6] = 1;     /* track 1 at LBA 0 */
    fmt0[13] = 0x10; fmt0[14] = 0xAA; /* lead-out at 300000 */
    fmt0[18] = (uint8_t)(300000 >> 8);
    fmt0[17] = (uint8_t)(300000 >> 16);
    fmt0[19] = (uint8_t)300000;
    fmt0_len = 4 + 8 * 2;
    assert(live(&lt, &li) == ACCUDISC_OK);
    assert(li.source == ACCUDISC_TOC_SRC_TOC);
    assert(li.degrade == ACCUDISC_TOC_DEGRADE_LEADIN_MALFORMED);
    assert(li.degrade_err == no_a2);
    printf("ok   live degrade: malformed, degrade_err == pure call's error\n");

    return 0;
}
