/* accudisc_compare_read (0.59.0; AccuDisc correspondence 2026-11-25aa,
 * cdda2img 282).
 *
 * The fake disc is one byte stream from a position hash, so no two windows
 * agree by accident. THIS pass can be made wrong every way the comparison has
 * to tell apart: damaged in place, displaced on-frame and off-frame, beyond
 * 15 frames, unreadable, and C2-flagged with correct audio. The REFERENCE is
 * the true stream, with sectors the callback marks absent. Every case runs
 * with the AccuBuffer on (the default: the callback on the buffer thread) and
 * off, and the two must agree exactly. adsc_dev_exec is replaced with
 * -Wl,--wrap. */

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "internal.h"
#include "mmc/mmc.h"

#define S 2352u
#define N 48 /* sectors per read: two chunks of 24 at C2 PTRS */

static struct {
    uint32_t damage_lba;           /* this pass flips 30 bytes here */
    uint32_t late_lo, late_hi;     /* chunk sectors in [lo, hi) read late... */
    uint32_t late;                 /* ... by this many bytes */
    uint32_t hard_lba;             /* every read of this sector fails */
    uint32_t c2_lba;               /* C2 bits set here, audio correct */
    uint32_t c2_last_lba;          /* ... only in the LAST C2 byte */
    uint32_t periodic_lo, periodic_hi; /* the disc repeats every 400 bytes
                                    * (100 samples) here: no signal for
                                    * accudisc_shift_test, an anchor for the
                                    * exact test */
    unsigned far_reads, chunk_reads;
} fk;

static uint8_t disc(uint64_t p)
{
    uint64_t sec = p / S;

    if (fk.periodic_hi && sec >= fk.periodic_lo && sec < fk.periodic_hi)
        p = p % 400;
    uint64_t v = p * 0x9E3779B97F4A7C15ull;
    v ^= v >> 29;
    return (uint8_t)(v * 0xBF58476D1CE4E5B9ull >> 56);
}

int __real_adsc_dev_exec(struct accudisc_device *dev, adsc_cmd *cmd);
int __wrap_adsc_dev_exec(struct accudisc_device *dev, adsc_cmd *cmd)
{
    cmd->resid = 0;
    cmd->driver_status = 0;
    if (cmd->cdb[0] != 0xBE) {
        if (cmd->dir == ADSC_XFER_IN && cmd->buf)
            memset(cmd->buf, 0, cmd->buf_len);
        return ACCUDISC_OK;
    }
    uint32_t lba = (uint32_t)cmd->cdb[2] << 24 | (uint32_t)cmd->cdb[3] << 16 |
                   (uint32_t)cmd->cdb[4] << 8 | cmd->cdb[5];
    uint32_t nsec = (uint32_t)cmd->cdb[6] << 16 | (uint32_t)cmd->cdb[7] << 8 |
                    cmd->cdb[8];
    unsigned c2 = (cmd->cdb[9] >> 1) & 3u;
    uint32_t c2len = c2 == 1 ? 294u : c2 == 2 ? 296u : 0;
    uint32_t rec = S + c2len;
    unsigned type = (cmd->cdb[1] >> 2) & 7u;

    if (type == 0 && nsec == 1) { /* SECTOR TYPE ANY: a far read */
        fk.far_reads++;
        memset(cmd->buf, 0, cmd->buf_len);
        return ACCUDISC_OK;
    }
    if (nsec > 1)
        fk.chunk_reads++;
    if (fk.hard_lba && fk.hard_lba >= lba && fk.hard_lba < lba + nsec) {
        dev->last_sense.valid = 1;
        dev->last_sense.key = 0x03;
        dev->last_sense.asc = 0x11;
        return ACCUDISC_ERR_SENSE;
    }
    for (uint32_t s = 0; s < nsec; s++) {
        uint8_t *o = (uint8_t *)cmd->buf + (size_t)s * rec;
        uint32_t cur = lba + s;
        uint64_t base = (uint64_t)cur * S;

        if (nsec > 1 && cur >= fk.late_lo && cur < fk.late_hi)
            base += fk.late;
        for (uint32_t i = 0; i < S; i++)
            o[i] = disc(base + i);
        if (cur == fk.damage_lba)
            for (uint32_t j = 0; j < 30; j++)
                o[100 + j * 61] ^= 0x5A;
        memset(o + S, 0, c2len);
        if (c2len && cur == fk.c2_lba)
            o[S + 7] = 0x40;
        if (c2len && cur == fk.c2_last_lba)
            o[S + c2len - 1] = 0x01;
    }
    return ACCUDISC_OK;
}

/* ---- the reference --------------------------------------------------------- */

static struct {
    uint32_t absent_lba;  /* marked absent (and zero-filled, as a HARD
                           * sector of the caller's capture would be) */
    uint8_t absent_value; /* what to write there; 1 normally */
    uint32_t cancel_at;   /* return nonzero on the call for this lba */
    uint32_t next_lba;    /* address-order check */
    unsigned calls, sink_calls;
    uint32_t sink_next;
} rf;

static int ref(void *user, uint32_t lba, uint32_t n, uint8_t *audio,
               uint8_t *absent)
{
    (void)user;
    assert(lba == rf.next_lba); /* address order, contiguous, once each */
    rf.next_lba = lba + n;
    rf.calls++;
    for (uint32_t s = 0; s < n; s++)
        assert(absent[s] == 0); /* arrives zeroed */
    if (rf.cancel_at && lba == rf.cancel_at)
        return 1;
    for (uint32_t s = 0; s < n; s++) {
        uint32_t cur = lba + s;

        for (uint32_t i = 0; i < S; i++)
            audio[(size_t)s * S + i] = disc((uint64_t)cur * S + i);
        if (cur == rf.absent_lba) {
            memset(audio + (size_t)s * S, 0, S);
            absent[s] = rf.absent_value;
        }
    }
    return 0;
}

/* The caller's own sink: after the comparison, same chunk, same order. */
static int sink(void *user, const accudisc_chunk *c)
{
    (void)user;
    assert(c->lba == rf.sink_next);
    assert(rf.next_lba == c->lba + c->nsec); /* the callback saw it first */
    rf.sink_next = c->lba + c->nsec;
    rf.sink_calls++;
    return 0;
}

/* ---- running ------------------------------------------------------------- */

static uint8_t amap[N];

static int run1(accudisc_read_req *req, accudisc_compare_stats *cs,
                accudisc_read_stats *st, uint32_t buffer)
{
    static struct accudisc_device dev;
    __typeof__(fk) f0 = fk;
    __typeof__(rf) r0 = rf;

    free(dev.xfer_bounce);
    memset(&dev, 0, sizeof(dev));
    memset(amap, 0xEE, sizeof(amap));
    rf.next_lba = rf.sink_next = req->lba;
    req->buffer_bytes = buffer;
    *cs = (accudisc_compare_stats)ACCUDISC_COMPARE_STATS_INIT;
    *st = (accudisc_read_stats)ACCUDISC_READ_STATS_INIT;
    int rc = accudisc_compare_read(&dev, req, ref, NULL, amap, sink, NULL, st,
                                   cs);
    (void)f0;
    (void)r0;
    return rc;
}

/* Both buffer modes; the maps and the compare stats must be identical. */
static int run(accudisc_read_req *req, accudisc_compare_stats *cs,
               accudisc_read_stats *st)
{
    __typeof__(fk) f0 = fk;
    __typeof__(rf) r0 = rf;
    accudisc_compare_stats cs0;
    accudisc_read_stats st0;
    uint8_t m0[N];

    int rc0 = run1(req, &cs0, &st0, ACCUDISC_BUFFER_NONE);
    memcpy(m0, amap, N);
    fk = f0;
    rf = r0;
    int rc = run1(req, cs, st, 0); /* the default AccuBuffer */
    assert(rc == rc0);
    assert(memcmp(m0, amap, N) == 0);
    assert(memcmp(&cs0, cs, sizeof(cs0)) == 0);
    if (rc == ACCUDISC_OK) {
        uint64_t sum = cs->agree + cs->agree_c2 + cs->differ + cs->shifted +
                       cs->hard + cs->ref_missing;
        assert(sum == req->count);
        assert(cs->shifted_off_frame <= cs->shifted);
        assert(rf.sink_calls == rf.calls);
    }
    return rc;
}

static accudisc_read_req base_req(void)
{
    accudisc_read_req r = ACCUDISC_READ_REQ_INIT;

    r.lba = 20000;
    r.count = N;
    r.c2 = ACCUDISC_C2_PTRS;
    return r;
}

static unsigned count_state(unsigned state)
{
    unsigned k = 0;

    for (unsigned i = 0; i < N; i++)
        k += ACCUDISC_AGREE_STATE(amap[i]) == state;
    return k;
}

/* ---- the cases ----------------------------------------------------------- */

/* The policy's REFERENCE_VERIFY request, against an identical reference: all
 * AGREE, and the pass makes exactly ONE far read (note 5's promise: no
 * per-chunk defeat, no Q rescue). */
static void test_identical_via_policy(void)
{
    accudisc_read_intent in = ACCUDISC_READ_INTENT_INIT;
    accudisc_read_req req = ACCUDISC_READ_REQ_INIT;
    accudisc_read_policy_info info = ACCUDISC_READ_POLICY_INFO_INIT;
    accudisc_compare_stats cs;
    accudisc_read_stats st;

    in.purpose = ACCUDISC_PURPOSE_REFERENCE_VERIFY;
    in.c2_usable = 1;
    in.n_admitted = 3;
    in.admitted_x[0] = 8;
    in.admitted_x[1] = 24;
    in.admitted_x[2] = 40;
    in.ref_speed_x = 40;
    in.lba = 20000;
    in.count = N;
    in.readable_hi = 200000;
    assert(accudisc_read_policy(&in, &req, &info) == ACCUDISC_OK);
    assert(req.speed_x == 24 && req.cache_defeat == 1);

    memset(&fk, 0, sizeof(fk));
    memset(&rf, 0, sizeof(rf));
    assert(run(&req, &cs, &st) == ACCUDISC_OK);
    assert(cs.agree == N && count_state(ACCUDISC_AGREE) == N);
    assert(st.cmds[ACCUDISC_STAGE_FAR] == 1 && fk.far_reads == 1);
    assert(st.cmds[ACCUDISC_STAGE_QPOS] == 0);
    assert(rf.calls == 2 && rf.sink_calls == 2);
    printf("ok - REFERENCE_VERIFY vs an identical reference: %u AGREE, one "
           "far read\n", N);
}

/* Each way this pass can be wrong, one sector or one run at a time. */
static void test_states(void)
{
    accudisc_read_req req = base_req();
    accudisc_compare_stats cs;
    accudisc_read_stats st;

    /* damaged in place: DIFFER, not a shift */
    memset(&fk, 0, sizeof(fk));
    memset(&rf, 0, sizeof(rf));
    fk.damage_lba = 20005;
    assert(run(&req, &cs, &st) == ACCUDISC_OK);
    assert(amap[5] == ACCUDISC_AGREE_DIFFER && cs.differ == 1 &&
           cs.agree == N - 1);

    /* 24 samples late (4 frames): SHIFTED, nibble 4 */
    memset(&fk, 0, sizeof(fk));
    fk.late_lo = 20010;
    fk.late_hi = 20013;
    fk.late = 96;
    assert(run(&req, &cs, &st) == ACCUDISC_OK);
    for (unsigned i = 10; i < 13; i++)
        assert(amap[i] == (ACCUDISC_AGREE_SHIFTED | 4 << 4));
    assert(cs.shifted == 3 && cs.shifted_off_frame == 0);

    /* 7 samples (off-frame): SHIFTED, nibble 0, counted off-frame */
    fk.late = 28;
    assert(run(&req, &cs, &st) == ACCUDISC_OK);
    for (unsigned i = 10; i < 13; i++)
        assert(amap[i] == ACCUDISC_AGREE_SHIFTED);
    assert(cs.shifted == 3 && cs.shifted_off_frame == 3);

    /* 20 frames (120 samples): the nibble caps at 15 */
    fk.late = 480;
    assert(run(&req, &cs, &st) == ACCUDISC_OK);
    for (unsigned i = 10; i < 13; i++)
        assert(amap[i] == (ACCUDISC_AGREE_SHIFTED | 15 << 4));

    /* 15 frames exactly: 15, and 1 frame: 1 (the cap is not off by one) */
    fk.late = 15 * 24;
    assert(run(&req, &cs, &st) == ACCUDISC_OK);
    assert(amap[10] == (ACCUDISC_AGREE_SHIFTED | 15 << 4));
    fk.late = 24;
    assert(run(&req, &cs, &st) == ACCUDISC_OK);
    assert(amap[10] == (ACCUDISC_AGREE_SHIFTED | 1 << 4));

    /* displaced AND damaged: still SHIFTED (the tolerant half of the union) */
    fk.late = 96;
    fk.damage_lba = 20011;
    assert(run(&req, &cs, &st) == ACCUDISC_OK);
    assert(amap[11] == (ACCUDISC_AGREE_SHIFTED | 4 << 4));

    /* C2 bits on a correct copy: AGREE_C2 */
    memset(&fk, 0, sizeof(fk));
    fk.c2_lba = 20030;
    assert(run(&req, &cs, &st) == ACCUDISC_OK);
    assert(amap[30] == ACCUDISC_AGREE_C2 && cs.agree_c2 == 1);
    fk.c2_lba = 0; /* a bit in the LAST C2 byte counts too */
    fk.c2_last_lba = 20031;
    assert(run(&req, &cs, &st) == ACCUDISC_OK);
    assert(amap[31] == ACCUDISC_AGREE_C2);

    /* The EXACT half of the union: audio repeating every 100 samples has no
     * signal for accudisc_shift_test (it equals itself at k = 100), but a
     * late copy still matches the reference exactly at some offset. SHIFTED,
     * not DIFFER: the union is why a periodic passage is not lost. */
    memset(&fk, 0, sizeof(fk));
    fk.periodic_lo = 20008;
    fk.periodic_hi = 20016;
    fk.late_lo = 20010;
    fk.late_hi = 20013;
    fk.late = 96;
    assert(run(&req, &cs, &st) == ACCUDISC_OK);
    for (unsigned i = 10; i < 13; i++)
        assert(ACCUDISC_AGREE_STATE(amap[i]) == ACCUDISC_AGREE_SHIFTED);
    assert(cs.differ == 0);

    /* this pass cannot read it: HARD */
    memset(&fk, 0, sizeof(fk));
    fk.hard_lba = 20040;
    assert(run(&req, &cs, &st) == ACCUDISC_OK);
    assert(amap[40] == ACCUDISC_AGREE_HARD && cs.hard == 1);
    assert(st.hard_errors == 1);

    /* the reference has no copy: REF_MISSING, never DIFFER against zeros */
    memset(&fk, 0, sizeof(fk));
    rf.absent_lba = 20020;
    rf.absent_value = 1;
    assert(run(&req, &cs, &st) == ACCUDISC_OK);
    assert(amap[20] == ACCUDISC_AGREE_REF_MISSING && cs.ref_missing == 1);
    assert(cs.differ == 0);

    /* HARD outranks REF_MISSING */
    fk.hard_lba = 20020;
    assert(run(&req, &cs, &st) == ACCUDISC_OK);
    assert(amap[20] == ACCUDISC_AGREE_HARD && cs.ref_missing == 0);
    printf("ok - DIFFER, SHIFTED (on/off-frame, capped, damaged), AGREE_C2, "
           "HARD, REF_MISSING, and their precedence\n");
}

/* Stopping: the callback cancels at the second chunk (its bytes and every
 * later one stay NOT_COMPARED); a reserved absent value stops the read as
 * ERR_INVAL (cdda2img 282); a refusing sink stops it too. */
static int refuse_sink(void *user, const accudisc_chunk *c)
{
    (void)user;
    (void)c;
    return 1;
}

static void test_stopping(void)
{
    accudisc_read_req req = base_req();
    accudisc_compare_stats cs;
    accudisc_read_stats st;

    memset(&fk, 0, sizeof(fk));
    memset(&rf, 0, sizeof(rf));
    rf.cancel_at = 20024;
    assert(run(&req, &cs, &st) == ACCUDISC_ERR_CANCELLED);
    for (unsigned i = 0; i < 24; i++)
        assert(amap[i] == ACCUDISC_AGREE);
    for (unsigned i = 24; i < N; i++)
        assert(amap[i] == 0xEE || amap[i] == ACCUDISC_AGREE_NOT_COMPARED);
    assert(cs.agree == 24);

    memset(&rf, 0, sizeof(rf));
    rf.absent_lba = 20003;
    rf.absent_value = 2; /* reserved */
    assert(run(&req, &cs, &st) == ACCUDISC_ERR_INVAL);
    rf.absent_value = 0xFF;
    assert(run(&req, &cs, &st) == ACCUDISC_ERR_INVAL);

    static struct accudisc_device dev;
    memset(&dev, 0, sizeof(dev));
    memset(&rf, 0, sizeof(rf));
    rf.next_lba = req.lba;
    req.buffer_bytes = ACCUDISC_BUFFER_NONE;
    cs = (accudisc_compare_stats)ACCUDISC_COMPARE_STATS_INIT;
    assert(accudisc_compare_read(&dev, &req, ref, NULL, amap, refuse_sink,
                                 NULL, &st, &cs) == ACCUDISC_ERR_CANCELLED);
    assert(rf.calls == 1 && cs.agree == 24);
    printf("ok - a cancelling callback, a reserved absent value (ERR_INVAL) and "
           "a refusing sink each stop the read\n");
}

/* The plumbing: NULL ref refused; NULL agree_map and NULL sink fine; a caller
 * status_map is used, not replaced; cstats under the size rule. */
static void test_plumbing(void)
{
    static struct accudisc_device dev;
    accudisc_read_req req = base_req();
    accudisc_compare_stats cs = ACCUDISC_COMPARE_STATS_INIT;
    accudisc_read_stats st = ACCUDISC_READ_STATS_INIT;
    uint8_t status[N];

    memset(&fk, 0, sizeof(fk));
    memset(&rf, 0, sizeof(rf));
    memset(&dev, 0, sizeof(dev));
    assert(accudisc_compare_read(&dev, &req, NULL, NULL, amap, NULL, NULL, &st,
                                 &cs) == ACCUDISC_ERR_INVAL);
    cs.size = 0;
    assert(accudisc_compare_read(&dev, &req, ref, NULL, amap, NULL, NULL, &st,
                                 &cs) == ACCUDISC_ERR_ABI);
    cs.size = sizeof(cs) + 8;
    assert(accudisc_compare_read(&dev, &req, ref, NULL, amap, NULL, NULL, &st,
                                 &cs) == ACCUDISC_ERR_ABI);

    fk.hard_lba = 20001;
    memset(status, 0, sizeof(status));
    req.status_map = status;
    req.buffer_bytes = ACCUDISC_BUFFER_NONE;
    rf.next_lba = req.lba;
    cs.size = sizeof(cs);
    assert(accudisc_compare_read(&dev, &req, ref, NULL, NULL, NULL, NULL, &st,
                                 &cs) == ACCUDISC_OK);
    assert((status[1] & 15) == ACCUDISC_MAP_HARD); /* the caller's map */
    assert(cs.hard == 1 && cs.agree == N - 1);
    assert(cs.size == sizeof(cs)); /* written back, as read_stats.size is */
    assert(sizeof(accudisc_compare_stats) == 64);
    printf("ok - NULL ref refused; NULL agree_map and sink; the caller's "
           "status map; compare_stats 64 bytes under the OUT rule\n");
}

int main(void)
{
    test_identical_via_policy();
    test_states();
    test_stopping();
    test_plumbing();
    return 0;
}
