/* Cancellation lands within ONE READ CD (0.52.0, cdda2img correspondence 262).
 *
 * Until 0.52.0 accudisc_read_req.cancel was polled once per chunk, so a chunk
 * whose per-sector retries, rescues and verify passes were all failing ran to
 * the end before Ctrl-C took effect: minutes on a damaged disc. The claim now
 * is a BOUND: once the flag is up, the engine issues no further READ CD.
 *
 * So the flag is raised by the fake drive itself, DURING the Nth READ CD,
 * exactly as a SIGINT watcher thread would raise it while the engine is
 * blocked in a command, and every case asserts on the command stream: no READ
 * CD after the one the flag arrived in. A return code alone would pass a build
 * that keeps reading for a minute and then says CANCELLED.
 *
 * Also asserted, because a cancel that lands mid-chunk leaves that chunk
 * partial: nothing from it reaches the sink, no sector is booked as a hard
 * error, and the speed a rescue ladder set is put back (0.52.0 moved
 * ladder_restore onto every exit path).
 *
 * adsc_dev_exec is replaced with -Wl,--wrap. */

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "internal.h"
#include "mmc/cdb.h"
#include "mmc/mmc.h"

#define MAXCMD 65536

static volatile int flag;   /* the caller's cancel flag, per-call or handle */

static struct {
    int fail_cdda;          /* every CD-DA-type READ CD fails MEDIUM ERROR */
    int noisy;              /* every READ CD returns different audio */
    int c2fire;             /* every READ CD carries fired C2 bits */
    unsigned trip_at;       /* raise `flag` during this READ CD (1-based);
                             * 0 = never */
    unsigned reads;         /* READ CDs seen */
    int trip_idx;           /* command index the flag went up in; -1 = not */
    unsigned n;             /* commands recorded */
    struct {
        uint8_t op;
        uint8_t desc[28];   /* SET STREAMING descriptor */
    } cmd[MAXCMD];
} fk;

int __real_adsc_dev_exec(struct accudisc_device *dev, adsc_cmd *cmd);
int __wrap_adsc_dev_exec(struct accudisc_device *dev, adsc_cmd *cmd)
{
    unsigned i = fk.n < MAXCMD ? fk.n++ : MAXCMD - 1;

    cmd->resid = 0;
    cmd->driver_status = 0;
    memset(&fk.cmd[i], 0, sizeof(fk.cmd[i]));
    fk.cmd[i].op = cmd->cdb[0];
    if (cmd->cdb[0] == 0xB6 && cmd->buf && cmd->buf_len == 28)
        memcpy(fk.cmd[i].desc, cmd->buf, 28);
    if (cmd->cdb[0] != 0xBE) {
        if (cmd->dir == ADSC_XFER_IN && cmd->buf)
            memset(cmd->buf, 0, cmd->buf_len);
        return ACCUDISC_OK;
    }

    fk.reads++;
    if (fk.trip_at && fk.reads == fk.trip_at) {
        flag = 1;
        fk.trip_idx = (int)i;
    }

    unsigned type = (cmd->cdb[1] >> 2) & 7u;
    if (fk.fail_cdda && type != 0) {
        dev->last_sense.valid = 1;
        dev->last_sense.key = 0x03;
        dev->last_sense.asc = 0x11;
        dev->last_sense.ascq = 0x00;
        return ACCUDISC_ERR_SENSE;
    }

    uint32_t lba = (uint32_t)cmd->cdb[2] << 24 | (uint32_t)cmd->cdb[3] << 16 |
                   (uint32_t)cmd->cdb[4] << 8 | cmd->cdb[5];
    uint32_t nsec = (uint32_t)cmd->cdb[6] << 16 | (uint32_t)cmd->cdb[7] << 8 |
                    cmd->cdb[8];
    unsigned c2 = (cmd->cdb[9] >> 1) & 3u, sub = cmd->cdb[10] & 7u;
    uint32_t slen = adsc_read_cd_sector_len(c2, sub);

    for (uint32_t s = 0; s < nsec && (size_t)(s + 1) * slen <= cmd->buf_len;
         s++) {
        uint8_t *p = (uint8_t *)cmd->buf + (size_t)s * slen;
        /* Audio that carries alignment signal (not constant), different per
         * sector and, when noisy, per command. HASHED, not affine: an affine
         * pattern (b*7 + k*101) turns one command's copy into a pure sample
         * shift of another's, consensus takes the slip branch, and that branch
         * restores the ladder itself, which hid a missing restore on exit. */
        for (uint32_t b = 0; b < ACCUDISC_BYTES_AUDIO; b++) {
            uint32_t h = b * 2654435761u ^ (lba + s) * 40503u ^
                         (fk.noisy ? fk.reads * 2246822519u : 0u);
            h ^= h >> 15;
            h *= 2246822519u;
            h ^= h >> 13;
            p[b] = (uint8_t)h;
        }
        memset(p + ACCUDISC_BYTES_AUDIO, fk.c2fire ? 0xFF : 0x00,
               slen - ACCUDISC_BYTES_AUDIO);
    }
    return ACCUDISC_OK;
}

/* ---- running ------------------------------------------------------------- */

static uint64_t delivered;
static struct accudisc_device dev;

static int sink(void *user, const accudisc_chunk *c)
{
    (void)user;
    delivered += c->nsec;
    return 0;
}

static void reset(void)
{
    free(dev.xfer_bounce);
    memset(&dev, 0, sizeof(dev));
    memset(&fk, 0, sizeof(fk));
    fk.trip_idx = -1;
    flag = 0;
    delivered = 0;
}

/* 0.54.0: the read cases run twice, the second time in rung order
 * (rescue_by_rung), whose interleaved loops must honour the same bound. */
static uint8_t by_rung;

static int run(accudisc_read_req *req, accudisc_read_stats *st)
{
    req->buffer_bytes = ACCUDISC_BUFFER_NONE;
    req->rescue_by_rung = by_rung;
    *st = (accudisc_read_stats)ACCUDISC_READ_STATS_INIT;
    return accudisc_read_cdda(&dev, req, sink, NULL, st);
}

/* THE BOUND: no READ CD after the one the flag went up in. */
static unsigned reads_after_trip(void)
{
    unsigned k = 0;

    assert(fk.trip_idx >= 0); /* the case must actually have tripped */
    for (unsigned i = (unsigned)fk.trip_idx + 1; i < fk.n; i++)
        k += fk.cmd[i].op == 0xBE;
    return k;
}

/* The descriptor of the LAST speed set, against the one speed_x would send. */
static int last_speed_is(unsigned speed_x)
{
    uint8_t want[28];

    adsc_cdb_set_streaming_desc(want, speed_x, 0, 0xFFFFFFFFu, 0, 0);
    for (unsigned i = fk.n; i-- > 0;)
        if (fk.cmd[i].op == 0xB6)
            return memcmp(fk.cmd[i].desc, want, sizeof(want)) == 0;
    return 0;
}

/* ---- the cases ----------------------------------------------------------- */

/* Per-sector fallback: the chunk read fails, then every sector is retried.
 * Uncancelled, 24 sectors x 6 attempts plus a far read before each retry is
 * ~260 commands. The flag goes up during sector 0's second attempt. */
static void test_per_sector_retries(void)
{
    accudisc_read_req req = ACCUDISC_READ_REQ_INIT;
    accudisc_read_stats st;

    /* Baseline: the same read uncancelled really does run long, so a bound
     * below is a measurement and not a read that was short anyway. */
    reset();
    fk.fail_cdda = 1;
    req.lba = 20000;
    req.count = 24;
    req.retries = 6;
    assert(run(&req, &st) == ACCUDISC_OK);
    assert(fk.reads > 200);
    assert(st.hard_errors == 24);

    reset();
    fk.fail_cdda = 1;
    fk.trip_at = 3; /* chunk read, sector 0 attempt 1, far read: the far */
    req.cancel = &flag;
    assert(run(&req, &st) == ACCUDISC_ERR_CANCELLED);
    assert(reads_after_trip() == 0);
    assert(delivered == 0);
    assert(st.hard_errors == 0); /* sector 0 was abandoned, not exhausted */
    assert(st.sense_medium == 0);
    /* The flag arrives during the sector's LAST attempt, so the attempt
     * loop ends on its own: the sector must still not be booked hard. */
    reset();
    fk.fail_cdda = 1;
    req.retries = 2;
    fk.trip_at = 4; /* chunk, attempt 1, far, attempt 2 (the last) */
    assert(run(&req, &st) == ACCUDISC_ERR_CANCELLED);
    assert(reads_after_trip() == 0);
    assert(st.hard_errors == 0);
    assert(st.sense_medium == 0);
    printf("ok - per-sector retries stop within one READ CD\n");
}

/* Verify pass + consensus: every reread disagrees, so each sector runs the
 * full consensus loop with the rescue ladder. Trip in the middle of it. The
 * ladder rung must not survive the cancel. */
static void test_consensus_and_ladder_restore(void)
{
    static const uint16_t ladder[] = { 4 };
    accudisc_read_req req = ACCUDISC_READ_REQ_INIT;
    accudisc_read_stats st;

    reset();
    fk.noisy = 1;
    req.lba = 20000;
    req.count = 24;
    req.verify_passes = 2;
    req.speed_x = 8;
    req.speed_ladder = ladder;
    req.ladder_len = 1;
    assert(run(&req, &st) == ACCUDISC_OK);
    unsigned full = fk.reads;
    assert(full > 100);

    reset();
    fk.noisy = 1;
    fk.trip_at = 12; /* chunk, far, witness read, then inside consensus */
    req.cancel = &flag;
    assert(run(&req, &st) == ACCUDISC_ERR_CANCELLED);
    assert(reads_after_trip() == 0);
    assert(fk.reads < full);
    assert(delivered == 0);
    assert(last_speed_is(8)); /* the rung was set, and put back */
    printf("ok - consensus stops within one READ CD; ladder speed restored\n");
}

/* C2 rescue: every copy carries fired C2, so each sector runs c2_retries
 * context reads. */
static void test_c2_rescue(void)
{
    accudisc_read_req req = ACCUDISC_READ_REQ_INIT;
    accudisc_read_stats st;

    reset();
    fk.c2fire = 1;
    req.lba = 20000;
    req.count = 24;
    req.c2 = ACCUDISC_C2_PTRS;
    req.verify_passes = 2;
    req.c2_retries = 8;
    req.cancel = &flag;
    fk.trip_at = 6;
    assert(run(&req, &st) == ACCUDISC_ERR_CANCELLED);
    assert(reads_after_trip() == 0);
    assert(delivered == 0);
    printf("ok - C2 rescue stops within one READ CD\n");
}

/* The HANDLE-level flag (accudisc_set_cancel) stops a read that carries no
 * flag of its own, and a flag already up refuses the read before any command
 * at all. The library never clears it. */
static void test_handle_flag_on_a_read(void)
{
    accudisc_read_req req = ACCUDISC_READ_REQ_INIT;
    accudisc_read_stats st;

    reset();
    fk.fail_cdda = 1;
    assert(accudisc_set_cancel(&dev, &flag) == ACCUDISC_OK);
    fk.trip_at = 5;
    req.lba = 20000;
    req.count = 24;
    req.retries = 6;
    assert(run(&req, &st) == ACCUDISC_ERR_CANCELLED);
    assert(reads_after_trip() == 0);
    assert(st.hard_errors == 0);

    /* Still set: the next call is refused with no command issued. */
    unsigned before = fk.n;
    assert(run(&req, &st) == ACCUDISC_ERR_CANCELLED);
    assert(fk.n == before);
    assert(flag == 1);

    /* Cleared by the caller, the handle reads again. */
    flag = 0;
    fk.fail_cdda = 0;
    fk.trip_at = 0;
    assert(run(&req, &st) == ACCUDISC_OK);
    assert(delivered == 24);

    assert(accudisc_set_cancel(NULL, &flag) == ACCUDISC_ERR_INVAL);
    assert(accudisc_set_cancel(&dev, NULL) == ACCUDISC_OK);
    printf("ok - handle-level flag: bound, sticky, refusal before I/O\n");
}

/* The speed-ladder probe takes no flag; the handle's flag must reach it. */
static void test_speed_ladder_probe(void)
{
    static const uint16_t cand[] = { 4, 8, 16 };
    accudisc_speed_rung out[3];

    reset();
    assert(accudisc_probe_speed_ladder(&dev, 1000, 6000, cand, 3, 1, out) ==
           ACCUDISC_OK);
    unsigned full = fk.reads;
    assert(full > 30);

    reset();
    assert(accudisc_set_cancel(&dev, &flag) == ACCUDISC_OK);
    fk.trip_at = 4;
    assert(accudisc_probe_speed_ladder(&dev, 1000, 6000, cand, 3, 1, out) ==
           ACCUDISC_ERR_CANCELLED);
    assert(reads_after_trip() == 0);
    printf("ok - speed-ladder probe honours the handle flag\n");
}

/* Likewise the C2-lag probe: pass 1 streams the whole span. */
static void test_c2_lag_probe(void)
{
    accudisc_c2_lag out;

    reset();
    fk.c2fire = 1;
    (void)accudisc_probe_c2_lag(&dev, 20000, 400, &out);
    unsigned full = fk.reads;
    assert(full > 50);

    /* Pass 1 streams until it holds C2LAG_MAX_WINDOWS (8) active chunks, so
     * read 3 is inside it; read 12 is in pass 2's window rereads. */
    static const unsigned trips[] = { 3, 12 };
    for (unsigned t = 0; t < 2; t++) {
        reset();
        fk.c2fire = 1;
        assert(accudisc_set_cancel(&dev, &flag) == ACCUDISC_OK);
        fk.trip_at = trips[t];
        assert(accudisc_probe_c2_lag(&dev, 20000, 400, &out) ==
               ACCUDISC_ERR_CANCELLED);
        assert(reads_after_trip() == 0);
    }
    printf("ok - C2-lag probe honours the handle flag\n");
}

int main(void)
{
    for (by_rung = 0; by_rung < 2; by_rung++) {
        if (by_rung)
            printf("-- again, in rung order (rescue_by_rung)\n");
        test_per_sector_retries();
        test_consensus_and_ladder_restore();
        test_c2_rescue();
        test_handle_flag_on_a_read();
    }
    test_speed_ladder_probe();
    test_c2_lag_probe();
    free(dev.xfer_bounce);
    return 0;
}
