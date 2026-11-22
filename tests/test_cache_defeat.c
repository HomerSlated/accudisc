/* accudisc_read_req.cache_defeat (0.49.0): one far read before the first
 * transfer, so a re-read straight after another read is not answered from the
 * drive's cache.
 *
 * Asked for by cdda2img for its speed-ladder re-reads (correspondence 238),
 * under Keith's ruling of 2026-10-22: an option on the existing read rather than
 * a new sweep call. Their conditions are what this file checks: off by default,
 * one command with no retry, a failure that does not fail the read, a record of
 * whether it ran, and a defined answer when there is nowhere to put it.
 *
 * Nothing here can show that a far read empties a real drive's cache; that was
 * measured on a LITE-ON LH-20A1S and is unmeasured on the PX-716A. What a fake
 * CAN show is the command stream: where the far read went, when, how often,
 * and what the read did when it failed. So every assertion is on the commands
 * the engine sent, recorded in order, never on a return code alone.
 * adsc_dev_exec is replaced with -Wl,--wrap. */

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "internal.h"
#include "mmc/mmc.h"
#include "read/far.h"

#define FAR 5000u /* the engine's ADSC_FLUSH_DISTANCE */
#define MAXCMD 4096

static struct {
    uint32_t refuse_lba;      /* a READ CD of this one sector is refused */
    int refuse;               /* refuse_lba is armed */
    int fail_multi;           /* every multi-sector READ CD fails (MEDIUM) */
    uint32_t data_lo, data_hi;/* 0.51.0: a "data track". A CD-DA-type READ CD
                               * there is refused 5/64/00; an ANY-type one
                               * answers GOOD but SHORT (2048 of 2352), as a
                               * Mode 1 sector does with user data only */
    int short_multi;          /* 0.51.1: every multi-sector READ CD answers
                               * GOOD but one sector short */
    int short_lba_armed;      /* a one-sector READ CD of short_lba answers */
    uint32_t short_lba;       /* GOOD with 100 bytes missing */
    uint32_t pad_resid;       /* resid reported on every GOOD READ CD (the
                               * 0.39.0 rounding slack case) */
    int bridge;               /* 0.51.2: the PX-716A's USB bridge, as measured
                               * 2026-11-22: a READ CD whose allocation exceeds
                               * its data (rounded up to 16) moves only whole
                               * 4-byte words of the data, zeroes the rest and
                               * reports that residual; an exact one is whole */
    unsigned n;               /* commands recorded */
    struct {
        uint8_t op;
        uint32_t lba, nsec;   /* READ CD only */
        unsigned c2, sub;     /* READ CD only */
        unsigned type;        /* READ CD expected sector type: 0 ANY, 1 CDDA */
        uint32_t len;         /* READ CD allocation in bytes */
    } cmd[MAXCMD];
    unsigned refused;         /* READ CDs of refuse_lba seen */
} fk;

int __real_adsc_dev_exec(struct accudisc_device *dev, adsc_cmd *cmd);
int __wrap_adsc_dev_exec(struct accudisc_device *dev, adsc_cmd *cmd)
{
    unsigned i = fk.n < MAXCMD ? fk.n++ : MAXCMD - 1;

    cmd->resid = 0;
    cmd->driver_status = 0;
    memset(&fk.cmd[i], 0, sizeof(fk.cmd[i]));
    fk.cmd[i].op = cmd->cdb[0];
    if (cmd->cdb[0] != 0xBE) {
        /* SET STREAMING, MODE SENSE and the rest: GOOD, and an all-zero page
         * for anything that reads one. The engine then records no honoured
         * speed, which nothing here depends on. */
        if (cmd->dir == ADSC_XFER_IN && cmd->buf)
            memset(cmd->buf, 0, cmd->buf_len);
        return ACCUDISC_OK;
    }
    uint32_t lba = (uint32_t)cmd->cdb[2] << 24 | (uint32_t)cmd->cdb[3] << 16 |
                   (uint32_t)cmd->cdb[4] << 8 | cmd->cdb[5];
    uint32_t nsec = (uint32_t)cmd->cdb[6] << 16 | (uint32_t)cmd->cdb[7] << 8 |
                    cmd->cdb[8];

    fk.cmd[i].lba = lba;
    fk.cmd[i].nsec = nsec;
    fk.cmd[i].c2 = (cmd->cdb[9] >> 1) & 3u;
    fk.cmd[i].sub = cmd->cdb[10] & 7u;
    fk.cmd[i].type = (cmd->cdb[1] >> 2) & 7u;
    fk.cmd[i].len = cmd->buf_len;
    if (lba >= fk.data_lo && lba < fk.data_hi) {
        if (fk.cmd[i].type != 0) {
            dev->last_sense.valid = 1;
            dev->last_sense.key = 0x05; /* ILLEGAL MODE FOR THIS TRACK */
            dev->last_sense.asc = 0x64;
            dev->last_sense.ascq = 0x00;
            return ACCUDISC_ERR_SENSE;
        }
        memset(cmd->buf, 0x5A, cmd->buf_len);
        cmd->resid = cmd->buf_len - 2048u * nsec;
        cmd->scsi_status = 0;
        return ACCUDISC_OK;
    }
    if (fk.fail_multi && nsec > 1) {
        dev->last_sense.valid = 1;
        dev->last_sense.key = 0x03; /* MEDIUM ERROR, so it is retried */
        dev->last_sense.asc = 0x11;
        dev->last_sense.ascq = 0x00;
        return ACCUDISC_ERR_SENSE;
    }
    if (fk.refuse && nsec == 1 && lba == fk.refuse_lba) {
        /* What a drive says to a sector past the lead-out: ILLEGAL REQUEST,
         * LOGICAL BLOCK ADDRESS OUT OF RANGE. */
        fk.refused++;
        dev->last_sense.valid = 1;
        dev->last_sense.key = 0x05;
        dev->last_sense.asc = 0x21;
        dev->last_sense.ascq = 0x00;
        return ACCUDISC_ERR_SENSE;
    }
    memset(cmd->buf, (int)(lba & 0xFF), cmd->buf_len);
    if (fk.bridge) {
        uint32_t exact = nsec * adsc_read_cd_sector_len(fk.cmd[i].c2, fk.cmd[i].sub);
        uint32_t moved = cmd->buf_len > exact ? exact & ~3u : cmd->buf_len;
        memset((uint8_t *)cmd->buf + moved, 0, cmd->buf_len - moved);
        cmd->resid = cmd->buf_len - moved;
        return ACCUDISC_OK;
    }
    if (fk.short_multi && nsec > 1)
        cmd->resid = cmd->buf_len / nsec; /* one sector's worth missing */
    else if (fk.short_lba_armed && nsec == 1 && lba == fk.short_lba)
        cmd->resid = 100;
    else
        cmd->resid = fk.pad_resid;
    return ACCUDISC_OK;
}

/* ---- running a read ------------------------------------------------------ */

static uint64_t delivered;

static int sink(void *user, const accudisc_chunk *c)
{
    (void)user;
    delivered += c->nsec;
    return 0;
}

static int run(accudisc_read_req *req, accudisc_read_stats *st)
{
    static struct accudisc_device dev;

    free(dev.xfer_bounce);
    memset(&dev, 0, sizeof(dev));
    fk.n = 0;
    fk.refused = 0;
    delivered = 0;
    req->buffer_bytes = ACCUDISC_BUFFER_NONE;
    *st = (accudisc_read_stats)ACCUDISC_READ_STATS_INIT;
    return accudisc_read_cdda(&dev, req, sink, NULL, st);
}

/* Index of the first recorded command matching op (and, for READ CD, a
 * one-sector transfer if single, or a longer one if !single); -1 if none. */
static int first(uint8_t op, int read_cd_single)
{
    for (unsigned i = 0; i < fk.n; i++) {
        if (fk.cmd[i].op != op)
            continue;
        if (op != 0xBE || read_cd_single < 0)
            return (int)i;
        if ((fk.cmd[i].nsec == 1) == (read_cd_single == 1))
            return (int)i;
    }
    return -1;
}

static unsigned count_reads_outside(uint32_t lo, uint32_t hi)
{
    unsigned k = 0;

    for (unsigned i = 0; i < fk.n; i++)
        if (fk.cmd[i].op == 0xBE &&
            (fk.cmd[i].lba + fk.cmd[i].nsec <= lo || fk.cmd[i].lba >= hi))
            k++;
    return k;
}

/* ---- the cases ----------------------------------------------------------- */

/* Off by default: the command stream is the old one, and the first READ CD is
 * the span's own first sector. */
static void test_off_is_the_old_path(void)
{
    accudisc_read_req req = ACCUDISC_READ_REQ_INIT;
    accudisc_read_stats st;

    req.lba = 20000;
    req.count = 8;
    assert(run(&req, &st) == ACCUDISC_OK);
    int r = first(0xBE, -1);
    assert(r >= 0 && fk.cmd[r].lba == 20000 && fk.cmd[r].nsec == 8);
    assert(count_reads_outside(20000, 20008) == 0);
    assert(st.cache_defeat == ACCUDISC_CACHE_DEFEAT_OFF);
    assert(delivered == 8);
}

/* On, with room before the span: one sector at lba - 5000, audio only, before
 * the span's first transfer, and nothing else outside the span. */
static void test_far_read_before_the_span(void)
{
    accudisc_read_req req = ACCUDISC_READ_REQ_INIT;
    accudisc_read_stats st;

    req.lba = 20000;
    req.count = 8;
    req.c2 = ACCUDISC_C2_PTRS;
    req.sub = ACCUDISC_SUB_RAW;
    req.cache_defeat = 1;
    assert(run(&req, &st) == ACCUDISC_OK);
    int f = first(0xBE, 1), s = first(0xBE, 0);
    assert(f >= 0 && s >= 0 && f < s);
    assert(fk.cmd[f].lba == 20000 - FAR);
    assert(fk.cmd[f].c2 == 0 && fk.cmd[f].sub == 0); /* audio only */
    assert(fk.cmd[s].lba == 20000);
    assert(count_reads_outside(20000, 20008) == 1);
    assert(st.cache_defeat == ACCUDISC_CACHE_DEFEAT_DONE);
    assert(st.rereads == 0 && st.hard_errors == 0);
    assert(delivered == 8);
}

/* On, with no room before the span: the sector 5000 past its LAST. */
static void test_far_read_after_the_span(void)
{
    accudisc_read_req req = ACCUDISC_READ_REQ_INIT;
    accudisc_read_stats st;

    req.lba = 100;
    req.count = 8;
    req.cache_defeat = 1;
    assert(run(&req, &st) == ACCUDISC_OK);
    int f = first(0xBE, 1), s = first(0xBE, 0);
    assert(f >= 0 && s >= 0 && f < s);
    assert(fk.cmd[f].lba == 100 + 8 - 1 + FAR);
    assert(count_reads_outside(100, 108) == 1);
    assert(st.cache_defeat == ACCUDISC_CACHE_DEFEAT_DONE);
}

/* A span longer than the far distance, starting below it. Stepping forward
 * from the first sector, as the reread cache_defeat() does, would land INSIDE
 * the span and prime the cache with sectors the read is about to ask for. The
 * far read must fall outside it. */
static void test_far_read_never_lands_inside_a_long_span(void)
{
    accudisc_read_req req = ACCUDISC_READ_REQ_INIT;
    accudisc_read_stats st;

    req.lba = 0;
    req.count = 3 * FAR;
    req.cache_defeat = 1;
    assert(run(&req, &st) == ACCUDISC_OK);
    int f = first(0xBE, 1);
    assert(f >= 0);
    assert(fk.cmd[f].lba >= 3 * FAR);
    assert(count_reads_outside(0, 3 * FAR) == 1);
    assert(st.cache_defeat == ACCUDISC_CACHE_DEFEAT_DONE);
    assert(delivered == 3 * FAR);
}

/* Refused, as past the lead-out: one attempt, no retry, the read completes
 * with every sector, nothing is counted against the span, and the record says
 * FAILED. */
static void test_refused_far_read_does_not_fail_the_read(void)
{
    accudisc_read_req req = ACCUDISC_READ_REQ_INIT;
    accudisc_read_stats st;

    req.lba = 100;
    req.count = 40;
    req.c2 = ACCUDISC_C2_PTRS;
    req.cache_defeat = 1;
    fk.refuse = 1;
    fk.refuse_lba = 100 + 40 - 1 + FAR;
    assert(run(&req, &st) == ACCUDISC_OK);
    fk.refuse = 0;
    assert(fk.refused == 1);
    assert(count_reads_outside(100, 140) == 1);
    assert(st.cache_defeat == ACCUDISC_CACHE_DEFEAT_FAILED);
    assert(st.sense_medium == 0 && st.sense_hardware == 0 &&
           st.sense_other == 0);
    assert(st.hard_errors == 0 && st.rereads == 0);
    assert(st.sectors_read == 40 && delivered == 40);
}

/* After the pass speed is set, not before: the far read empties the cache the
 * read is about to use, at the speed it asked for. */
static void test_far_read_follows_the_speed_set(void)
{
    accudisc_read_req req = ACCUDISC_READ_REQ_INIT;
    accudisc_read_stats st;

    req.lba = 20000;
    req.count = 8;
    req.speed_x = 8;
    req.cache_defeat = 1;
    assert(run(&req, &st) == ACCUDISC_OK);
    int sp = first(0xB6, -1), f = first(0xBE, 1), s = first(0xBE, 0);
    assert(sp >= 0 && f >= 0 && s >= 0);
    assert(sp < f && f < s);
    assert(st.speed_requested_x == 8);
}

/* No address to try: a span starting below 5000 whose end plus 5000 does not
 * fit in an LBA. Nothing is sent and the record says FAILED. The read is
 * cancelled before its first chunk, since the fake cannot serve four billion
 * sectors; the stats are still exported. */
static void test_no_address_to_try(void)
{
    accudisc_read_req req = ACCUDISC_READ_REQ_INIT;
    accudisc_read_stats st;
    static const volatile int stop = 1;

    req.lba = 0;
    req.count = UINT32_MAX - FAR / 2;
    req.cache_defeat = 1;
    req.cancel = &stop;
    assert(run(&req, &st) == ACCUDISC_ERR_CANCELLED);
    assert(first(0xBE, -1) == -1);
    assert(st.cache_defeat == ACCUDISC_CACHE_DEFEAT_FAILED);
}

/* A caller built against 0.48.1 declares 80 bytes, so the library never sees
 * cache_defeat however the rest of the struct is filled: no far read. */
static void test_old_caller_size_never_defeats(void)
{
    accudisc_read_req req = ACCUDISC_READ_REQ_INIT;
    accudisc_read_stats st;

    req.size = 80;
    req.lba = 20000;
    req.count = 8;
    req.cache_defeat = 1;
    assert(run(&req, &st) == ACCUDISC_OK);
    assert(first(0xBE, 1) == -1);
    assert(st.cache_defeat == ACCUDISC_CACHE_DEFEAT_OFF);
}

/* ---- the engine's own far read (0.50.1) ----------------------------------
 * Separate from the option above: cache_defeat() runs before every verify-pass
 * chunk and every per-sector retry, whatever req.cache_defeat says. It used to
 * step FORWARD whenever the sector was less than 5000 into the span, so a short
 * re-read near the end of session 1 aimed past session 1's lead-out
 * (correspondence 251, *Under My Skin*: session 1 ends at 195656, session 2's
 * lead-in starts at 202406). The engine holds no TOC; backward from a session-1
 * sector is the one direction that stays inside session 1 without one. */

/* Every one-sector READ CD outside [lo, hi): all must be at or before lo. */
static void assert_far_reads_backward(uint32_t lo, uint32_t hi, unsigned want)
{
    unsigned k = 0;

    for (unsigned i = 0; i < fk.n; i++) {
        if (fk.cmd[i].op != 0xBE || fk.cmd[i].nsec != 1)
            continue;
        if (fk.cmd[i].lba >= lo && fk.cmd[i].lba < hi)
            continue;
        /* exactly 5000 behind some sector of the span */
        assert(fk.cmd[i].lba < lo);
        assert(fk.cmd[i].lba + FAR >= lo && fk.cmd[i].lba + FAR < hi);
        k++;
    }
    assert(k >= want);
}

/* A short span near the end of a session, read with a verify pass: the far
 * read before the second transfer goes 5000 BACK, not 5000 on. */
static void test_verify_pass_far_read_steps_back(void)
{
    accudisc_read_req req = ACCUDISC_READ_REQ_INIT;
    accudisc_read_stats st;

    req.lba = 195600; /* 56 sectors before session 1's lead-out */
    req.count = 8;
    req.verify_passes = 2;
    assert(run(&req, &st) == ACCUDISC_OK);
    assert(fk.n > 0);
    assert_far_reads_backward(195600, 195608, 1);
    assert(delivered == 8);
}

/* A per-sector retry: the chunk read fails, every sector is re-read alone, and
 * a failed sector's second attempt is preceded by a far read — backward. */
static void test_retry_far_read_steps_back(void)
{
    accudisc_read_req req = ACCUDISC_READ_REQ_INIT;
    accudisc_read_stats st;

    req.lba = 195600;
    req.count = 8;
    req.retries = 3;
    fk.fail_multi = 1;
    fk.refuse = 1;
    fk.refuse_lba = 195603; /* refused on every attempt */
    assert(run(&req, &st) == ACCUDISC_OK);
    fk.refuse = 0;
    fk.fail_multi = 0;
    assert(fk.refused == 3);
    assert_far_reads_backward(195600, 195608, 2);
    assert(st.hard_errors == 1);
}

/* With no room behind (a sector below 5000), forward is the only choice, and
 * is what it was. */
static void test_far_read_near_lba_zero_steps_forward(void)
{
    accudisc_read_req req = ACCUDISC_READ_REQ_INIT;
    accudisc_read_stats st;

    req.lba = 100;
    req.count = 8;
    req.verify_passes = 2;
    assert(run(&req, &st) == ACCUDISC_OK);
    int f = first(0xBE, 1);
    assert(f >= 0 && fk.cmd[f].lba == 100 + FAR);
}

/* ---- 0.51.0: the readable range ------------------------------------------ */

/* The picker alone, at every boundary the rule has. */
static void test_picker_boundaries(void)
{
    uint32_t t = 0;

    /* No range: exactly 0.50.1's rule. */
    assert(adsc_far_target(20000, 8, 0, 0, FAR, &t) == ADSC_FAR_FULL && t == 15000);
    assert(adsc_far_target(100, 8, 0, 0, FAR, &t) == ADSC_FAR_FULL && t == 107 + FAR);
    assert(adsc_far_target(0, UINT32_MAX, 0, 0, FAR, &t) == ADSC_FAR_NONE);

    /* a - D == lo: in range, full distance back. */
    assert(adsc_far_target(15000, 8, 10000, 30000, FAR, &t) == ADSC_FAR_FULL &&
           t == 10000);
    /* a - D == lo - 1: not in range, so forward from the span's end. */
    assert(adsc_far_target(14999, 8, 10000, 30000, FAR, &t) == ADSC_FAR_FULL &&
           t == 15006 + FAR);
    /* end + D == hi - 1: in range. */
    assert(adsc_far_target(14999, 8, 10000, 20007, FAR, &t) == ADSC_FAR_FULL &&
           t == 20006);
    /* end + D == hi: out, so the furthest in-range sector behind: lo. */
    assert(adsc_far_target(14999, 8, 10000, 20006, FAR, &t) == ADSC_FAR_NEAR &&
           t == 10000);
    /* One sector wider than the span, behind it, then ahead of it. */
    assert(adsc_far_target(500, 8, 499, 508, FAR, &t) == ADSC_FAR_NEAR && t == 499);
    assert(adsc_far_target(500, 8, 500, 509, FAR, &t) == ADSC_FAR_NEAR && t == 508);
    /* The range IS the span: nowhere to go, and never inside it. */
    assert(adsc_far_target(500, 8, 500, 508, FAR, &t) == ADSC_FAR_NONE);
    /* A forced span wholly past the range: the target stays in range. */
    assert(adsc_far_target(50000, 8, 10000, 20000, FAR, &t) == ADSC_FAR_NEAR &&
           t == 10000);
    /* A span starting before the range: forward lands in it. */
    assert(adsc_far_target(5000, 8, 10000, 30000, FAR, &t) == ADSC_FAR_FULL &&
           t == 10007);
}

static unsigned single_reads_outside(uint32_t lo, uint32_t hi)
{
    unsigned k = 0;

    for (unsigned i = 0; i < fk.n; i++)
        if (fk.cmd[i].op == 0xBE && fk.cmd[i].nsec == 1 &&
            (fk.cmd[i].lba < lo || fk.cmd[i].lba >= hi))
            k++;
    return k;
}

/* Audio in a later session (Under My Skin's shape, session 2 from 206906):
 * 5000 back lands between the sessions and 5000 forward past the disc, so
 * the far read goes to the furthest in-range sector behind the span, and says
 * NEAR. */
static void test_later_session_far_read_stays_in_it(void)
{
    accudisc_read_req req = ACCUDISC_READ_REQ_INIT;
    accudisc_read_stats st;

    req.lba = 207100;
    req.count = 8;
    req.cache_defeat = 1;
    req.readable_lo = 206906;
    req.readable_hi = 211185;
    assert(run(&req, &st) == ACCUDISC_OK);
    int f = first(0xBE, 1);
    assert(f >= 0 && fk.cmd[f].lba == 206906);
    assert(count_reads_outside(206906, 211185) == 0);
    assert(st.cache_defeat == ACCUDISC_CACHE_DEFEAT_NEAR);
    assert(delivered == 8);
}

/* The engine's OWN far reads (before every per-sector retry) obey the range
 * too, not only the opt-in one. */
static void test_retry_far_reads_stay_in_range(void)
{
    accudisc_read_req req = ACCUDISC_READ_REQ_INIT;
    accudisc_read_stats st;

    fk.fail_multi = 1;
    fk.refuse = 1;
    fk.refuse_lba = 21003; /* every attempt refused: 3 attempts, 2 far reads */
    req.lba = 21000;
    req.count = 8;
    req.retries = 3;
    req.readable_lo = 20000;
    req.readable_hi = 23000;
    run(&req, &st);
    fk.fail_multi = 0;
    fk.refuse = 0;
    assert(fk.refused == 3);
    /* the far reads happened: single reads outside the span, inside range */
    assert(single_reads_outside(21000, 21008) >= 2);
    assert(count_reads_outside(20000, 23000) == 0);
    /* and none of them inside the sector being retried */
    for (unsigned i = 1; i < fk.n; i++)
        if (fk.cmd[i].op == 0xBE && fk.cmd[i].nsec == 1 &&
            fk.cmd[i - 1].op == 0xBE && fk.cmd[i - 1].nsec == 1 &&
            fk.cmd[i].lba == fk.cmd[i - 1].lba)
            assert(0 && "a far read must not be the retried sector itself");
}

/* Mixed Mode with no range: 5000 back lands in the data track. Through 0.50.1
 * the far read used CD-DA and was refused 5/64/00. Now it is ANY, the sector
 * answers short, and that is a completed far read. */
static void test_data_track_target_is_read_as_any(void)
{
    accudisc_read_req req = ACCUDISC_READ_REQ_INIT;
    accudisc_read_stats st;

    fk.data_lo = 0;
    fk.data_hi = 30000;
    req.lba = 32000;
    req.count = 8;
    req.cache_defeat = 1;
    assert(run(&req, &st) == ACCUDISC_OK);
    fk.data_lo = fk.data_hi = 0;
    int f = first(0xBE, 1);
    assert(f >= 0 && fk.cmd[f].lba == 27000);
    assert(fk.cmd[f].type == 0); /* ANY */
    assert(st.cache_defeat == ACCUDISC_CACHE_DEFEAT_DONE);
    assert(delivered == 8);
}

/* A range that cannot be meant is refused before any command. */
static void test_impossible_range_is_refused(void)
{
    static const uint32_t bad[][2] = {{5, 0}, {10, 10}, {10, 5}};
    accudisc_read_req req = ACCUDISC_READ_REQ_INIT;
    accudisc_read_stats st;

    for (unsigned i = 0; i < 3; i++) {
        req.lba = 20000;
        req.count = 8;
        req.readable_lo = bad[i][0];
        req.readable_hi = bad[i][1];
        assert(run(&req, &st) == ACCUDISC_ERR_INVAL);
        assert(first(0xBE, -1) < 0);
    }
}

/* The range is exactly the span: no far read at all, never one inside it. */
static void test_range_equal_to_span_has_no_target(void)
{
    accudisc_read_req req = ACCUDISC_READ_REQ_INIT;
    accudisc_read_stats st;

    req.lba = 20000;
    req.count = 8;
    req.cache_defeat = 1;
    req.readable_lo = 20000;
    req.readable_hi = 20008;
    assert(run(&req, &st) == ACCUDISC_OK);
    assert(first(0xBE, 1) < 0);
    assert(single_reads_outside(20000, 20008) == 0);
    assert(st.cache_defeat == ACCUDISC_CACHE_DEFEAT_FAILED);
    assert(delivered == 8);
}

/* ---- 0.51.1: the short-READ-CD guard ------------------------------------- */

/* A GOOD transfer that arrives short is ERR_SHORT, never OK; a residual no
 * larger than the 16-byte rounding slack is not a short read. Through 0.51.0
 * the residual was read before the command ran, so the first assertion
 * failed: the transfer came back OK with a stale tail. */
static void test_short_read_cd_is_refused(void)
{
    static struct accudisc_device dev;
    static uint8_t b[2 * (2352 + 294 + 96)];

    memset(&dev, 0, sizeof dev);
    fk.n = 0;
    fk.short_lba_armed = 1;
    fk.short_lba = 100;
    assert(adsc_mmc_read_cd(&dev, 100, 1, ADSC_SECTOR_CDDA, ADSC_C2_NONE,
                            ADSC_SUB_NONE, b, 2352) == ACCUDISC_ERR_SHORT);
    assert(adsc_mmc_read_cd(&dev, 101, 1, ADSC_SECTOR_CDDA, ADSC_C2_NONE,
                            ADSC_SUB_NONE, b, 2352) == ACCUDISC_OK);
    fk.short_lba_armed = 0;

    /* C2 + raw P-W: 2742 bytes, rounded to 2752 (0.39.0). A residual of 10 is
     * the slack, not data. 11 is one byte of data missing. */
    free(dev.xfer_bounce);
    memset(&dev, 0, sizeof dev);
    fk.pad_resid = 10;
    assert(adsc_mmc_read_cd(&dev, 200, 1, ADSC_SECTOR_CDDA, ADSC_C2_294,
                            ADSC_SUB_RAW, b, 2742) == ACCUDISC_OK);
    fk.pad_resid = 11;
    assert(adsc_mmc_read_cd(&dev, 200, 1, ADSC_SECTOR_CDDA, ADSC_C2_294,
                            ADSC_SUB_RAW, b, 2742) == ACCUDISC_ERR_SHORT);
    fk.pad_resid = 0;
    free(dev.xfer_bounce);
    dev.xfer_bounce = NULL;
}

/* In the engine: a short CHUNK is not delivered; the span falls back to one
 * sector at a time, which here arrive whole, so the read is clean. */
static void test_short_chunk_falls_back_per_sector(void)
{
    accudisc_read_req req = ACCUDISC_READ_REQ_INIT;
    accudisc_read_stats st;

    fk.short_multi = 1;
    req.lba = 20000;
    req.count = 8;
    assert(run(&req, &st) == ACCUDISC_OK);
    fk.short_multi = 0;
    unsigned singles = 0;
    for (unsigned i = 0; i < fk.n; i++)
        if (fk.cmd[i].op == 0xBE && fk.cmd[i].nsec == 1)
            singles++;
    assert(singles == 8);
    assert(st.hard_errors == 0);
    assert(delivered == 8);
}

/* A sector that is short on every attempt is a hard error (zero-filled,
 * counted), not stale bytes delivered as audio. */
static void test_always_short_sector_is_a_hard_error(void)
{
    accudisc_read_req req = ACCUDISC_READ_REQ_INIT;
    accudisc_read_stats st;

    fk.short_multi = 1;
    fk.short_lba_armed = 1;
    fk.short_lba = 20003;
    req.lba = 20000;
    req.count = 8;
    req.retries = 3;
    run(&req, &st);
    fk.short_multi = 0;
    fk.short_lba_armed = 0;
    assert(st.hard_errors == 1);
    assert(delivered == 8);
}

/* ---- 0.51.2: a transport that truncates rounded transfers ---------------- */

static uint32_t c2sub_bytes(uint8_t *last2, const uint8_t *b, uint32_t n)
{
    uint32_t len = n * 2742u;
    last2[0] = b[len - 2];
    last2[1] = b[len - 1];
    return len;
}

/* The measured bridge, through adsc_mmc_read_cd. A 23-sector C2+sub read is
 * short when rounded; the exact retry is whole, its last two bytes are the
 * real data (not the bridge's zeros), and the handle switches to exact
 * lengths, so the next read is sent exact first time. */
static void test_rounded_short_retries_exact_and_latches(void)
{
    static struct accudisc_device dev;
    static uint8_t b[24 * 2742];
    uint8_t t[2];

    memset(&dev, 0, sizeof dev);
    fk.n = 0;
    fk.bridge = 1;
    assert(adsc_mmc_read_cd(&dev, 30000, 23, ADSC_SECTOR_CDDA, ADSC_C2_294,
                            ADSC_SUB_RAW, b, 2742) == ACCUDISC_OK);
    c2sub_bytes(t, b, 23);
    assert(t[0] == (30000 & 0xFF) && t[1] == (30000 & 0xFF));
    assert(fk.n == 2 && fk.cmd[0].len == 63072 && fk.cmd[1].len == 63066);
    assert(dev.xfer_exact == 1);
    fk.n = 0;
    assert(adsc_mmc_read_cd(&dev, 31000, 23, ADSC_SECTOR_CDDA, ADSC_C2_294,
                            ADSC_SUB_RAW, b, 2742) == ACCUDISC_OK);
    assert(fk.n == 1 && fk.cmd[0].len == 63066);
    fk.bridge = 0;
    free(dev.xfer_bounce);
    dev.xfer_bounce = NULL;
}

/* The trap the latch had: a rounded read whose data is already whole words
 * (2 x 2742 = 5484) arrives whole, which marks rounding as working (-1). The
 * next 23-sector chunk must still fall back, not fail forever. */
static void test_short_after_a_good_rounded_read_still_falls_back(void)
{
    static struct accudisc_device dev;
    static uint8_t b[24 * 2742];

    memset(&dev, 0, sizeof dev);
    fk.n = 0;
    fk.bridge = 1;
    assert(adsc_mmc_read_cd(&dev, 40000, 2, ADSC_SECTOR_CDDA, ADSC_C2_294,
                            ADSC_SUB_RAW, b, 2742) == ACCUDISC_OK);
    assert(dev.xfer_exact == -1);
    assert(adsc_mmc_read_cd(&dev, 41000, 23, ADSC_SECTOR_CDDA, ADSC_C2_294,
                            ADSC_SUB_RAW, b, 2742) == ACCUDISC_OK);
    assert(dev.xfer_exact == 1);
    fk.bridge = 0;
    free(dev.xfer_bounce);
    dev.xfer_bounce = NULL;
}

/* The whole engine on the bridge: a C2 + raw-subchannel read of 300 sectors
 * (276 here, twelve whole chunks) reads clean, with no per-sector fallback and no hard errors, and after the
 * first chunk every READ CD is sent at its exact length. 0.51.1 crawled here
 * (cdda2img 257): every rounded chunk short, every sector retried. */
static void test_c2sub_read_on_the_bridge_is_clean(void)
{
    accudisc_read_req req = ACCUDISC_READ_REQ_INIT;
    accudisc_read_stats st;

    fk.bridge = 1;
    req.lba = 50000;
    req.count = 276; /* 12 chunks of 23: any one-sector READ CD is a fallback */
    req.c2 = ACCUDISC_C2_PTRS;
    req.sub = ACCUDISC_SUB_RAW;
    assert(run(&req, &st) == ACCUDISC_OK);
    fk.bridge = 0;
    assert(st.hard_errors == 0);
    assert(delivered == 276);
    unsigned singles = 0, rounded_after_first = 0;
    for (unsigned i = 0; i < fk.n; i++) {
        if (fk.cmd[i].op != 0xBE)
            continue;
        if (fk.cmd[i].nsec == 1)
            singles++;
        if (i > 1 && fk.cmd[i].len != fk.cmd[i].nsec * 2742u)
            rounded_after_first++;
    }
    assert(singles == 0);
    assert(rounded_after_first == 0);
}

int main(void)
{
    test_off_is_the_old_path();
    test_far_read_before_the_span();
    test_far_read_after_the_span();
    test_far_read_never_lands_inside_a_long_span();
    test_refused_far_read_does_not_fail_the_read();
    test_far_read_follows_the_speed_set();
    test_no_address_to_try();
    test_old_caller_size_never_defeats();
    test_verify_pass_far_read_steps_back();
    test_retry_far_read_steps_back();
    test_far_read_near_lba_zero_steps_forward();
    test_picker_boundaries();
    test_later_session_far_read_stays_in_it();
    test_retry_far_reads_stay_in_range();
    test_data_track_target_is_read_as_any();
    test_impossible_range_is_refused();
    test_range_equal_to_span_has_no_target();
    test_short_read_cd_is_refused();
    test_short_chunk_falls_back_per_sector();
    test_always_short_sector_is_a_hard_error();
    test_rounded_short_retries_exact_and_latches();
    test_short_after_a_good_rounded_read_still_falls_back();
    test_c2sub_read_on_the_bridge_is_clean();
    printf("test_cache_defeat: 23/23\n");
    return 0;
}
