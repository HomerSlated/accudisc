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

#define FAR 5000u /* the engine's ADSC_FLUSH_DISTANCE */
#define MAXCMD 4096

static struct {
    uint32_t refuse_lba;      /* a READ CD of this one sector is refused */
    int refuse;               /* refuse_lba is armed */
    int fail_multi;           /* every multi-sector READ CD fails (MEDIUM) */
    unsigned n;               /* commands recorded */
    struct {
        uint8_t op;
        uint32_t lba, nsec;   /* READ CD only */
        unsigned c2, sub;     /* READ CD only */
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
    printf("test_cache_defeat: 11/11\n");
    return 0;
}
