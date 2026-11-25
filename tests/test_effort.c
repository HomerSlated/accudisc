/* Effort stats and the origin lane (0.53.0, cdda2img correspondence 268-274;
 * AccuDisc design notes 2026-11-25g/j).
 *
 * The claim is EXACT ACCOUNTING: every operation the read engine issues is
 * booked to exactly one stage, and every delivered sector's origin byte names
 * the read whose bytes went out. So each case below scripts a fake drive,
 * derives the counts by hand from the engine's documented behaviour, and
 * asserts them exactly. A count that is merely "plausible" passes nothing.
 *
 * The fake answers MODE SENSE page 2A with the speed it was last set to, so
 * the read-back paths (pass speed and each rung) succeed instead of failing,
 * and rung_honoured_x can be checked against the rung that was asked for.
 *
 * adsc_dev_exec is replaced with -Wl,--wrap. */

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "internal.h"
#include "mmc/cdb.h"
#include "mmc/mmc.h"
#include "cdda/crc16.h"

#define S 2352u

static struct {
    unsigned speed;          /* last SET STREAMING speed, decoded */
    int fail_chunk_at;       /* chunk transfers starting at this LBA fail */
    uint32_t bad_lba;        /* single-sector reads of this LBA fail (0 = none) */
    unsigned bad_skip;       /* ... except the first bad_skip of them (0.57.0) */
    unsigned bad_seen;
    uint32_t c2_lo, c2_hi;   /* C2 fires on [lo, hi), every read */
    uint32_t c2_chunk_only;  /* C2 fires here in CHUNK transfers only */
    uint32_t mispos_lba;     /* its Q names lba - 2048 on its FIRST read only */
    unsigned mispos_seen;
    uint32_t noise_lba;      /* the 2nd chunk transfer flips a byte here */
    uint32_t nz_lo, nz_hi;   /* chunk transfer #nz_chunk flips a byte in
                              * every sector of [nz_lo, nz_hi) ... */
    unsigned nz_chunk;
    uint8_t nz_single[256];  /* ... and each such sector's FIRST single-sector
                              * read flips a different byte, so consensus
                              * needs a second attempt (index lba - 10000) */
    int mispos_reads;        /* reads of mispos_lba that name lba - 2048
                              * (0 means 1, the original behaviour) */
    unsigned chunk_reads;
    unsigned delay_us;       /* every READ CD takes at least this long */
} fk;

static uint8_t to_bcd(unsigned v) { return (uint8_t)((v / 10) << 4 | (v % 10)); }

static void make_sub(uint32_t qlba, uint8_t raw[96])
{
    uint8_t q[12];
    uint32_t a = qlba + 150;

    memset(q, 0, 12);
    q[0] = 0x01; q[1] = 0x01; q[2] = 0x01;
    q[7] = to_bcd(a / 4500); q[8] = to_bcd((a / 75) % 60); q[9] = to_bcd(a % 75);
    uint16_t crc = (uint16_t)~adsc_crc16(q, 10);
    q[10] = (uint8_t)(crc >> 8); q[11] = (uint8_t)crc;
    for (unsigned i = 0; i < 96; i++)
        raw[i] = (uint8_t)(0x3F | (((q[i >> 3] >> (7 - (i & 7))) & 1) << 6));
}

static unsigned decode_speed(const uint8_t *desc)
{
    static const unsigned cand[] = { 0, 4, 8, 16, 24, 32, 40, 48 };
    uint8_t want[28];

    for (unsigned i = 0; i < sizeof(cand) / sizeof(cand[0]); i++) {
        adsc_cdb_set_streaming_desc(want, cand[i], 0, 0xFFFFFFFFu, 0, 0);
        if (memcmp(desc, want, 28) == 0)
            return cand[i];
    }
    assert(!"undecodable SET STREAMING descriptor");
    return 0;
}

static uint8_t audio(uint32_t lba, uint32_t b)
{
    uint32_t h = b * 2654435761u ^ lba * 40503u;
    h ^= h >> 15;
    h *= 2246822519u;
    h ^= h >> 13;
    return (uint8_t)h;
}

int __real_adsc_dev_exec(struct accudisc_device *dev, adsc_cmd *cmd);
int __wrap_adsc_dev_exec(struct accudisc_device *dev, adsc_cmd *cmd)
{
    cmd->resid = 0;
    cmd->driver_status = 0;
    if (cmd->cdb[0] == 0xB6 && cmd->buf && cmd->buf_len == 28) {
        fk.speed = decode_speed(cmd->buf);
        return ACCUDISC_OK;
    }
    if (cmd->cdb[0] == 0x5A && cmd->dir == ADSC_XFER_IN && cmd->buf) {
        /* MODE SENSE(10) page 2A: header (no block descriptors), then a
         * 20-byte page whose current read speed is the last speed set. */
        uint8_t *b = cmd->buf;
        unsigned kbps = (fk.speed ? fk.speed : 40) * 176u;

        memset(b, 0, cmd->buf_len);
        b[1] = 22; /* mode data length: 24 bytes in all */
        if (cmd->buf_len >= 24) {
            b[8] = 0x2a;
            b[9] = 20;
            b[8 + 14] = (uint8_t)(kbps >> 8);
            b[8 + 15] = (uint8_t)kbps;
        }
        return ACCUDISC_OK;
    }
    if (cmd->cdb[0] != 0xBE) {
        if (cmd->dir == ADSC_XFER_IN && cmd->buf)
            memset(cmd->buf, 0, cmd->buf_len);
        return ACCUDISC_OK;
    }

    uint32_t lba = (uint32_t)cmd->cdb[2] << 24 | (uint32_t)cmd->cdb[3] << 16 |
                   (uint32_t)cmd->cdb[4] << 8 | cmd->cdb[5];
    uint32_t nsec = (uint32_t)cmd->cdb[6] << 16 | (uint32_t)cmd->cdb[7] << 8 |
                    cmd->cdb[8];
    unsigned type = (cmd->cdb[1] >> 2) & 7u;
    unsigned c2 = (cmd->cdb[9] >> 1) & 3u, sub = cmd->cdb[10] & 7u;
    uint32_t slen = adsc_read_cd_sector_len(c2, sub);
    uint32_t c2len = slen - S - (sub == 1 ? 96u : sub == 2 ? 16u : 0u);
    int chunk = nsec > 3;

    if (fk.delay_us) {
        struct timespec ts = { 0, (long)fk.delay_us * 1000L };
        nanosleep(&ts, NULL);
    }

    if (chunk) {
        fk.chunk_reads++;
        if (fk.fail_chunk_at >= 0 && lba == (uint32_t)fk.fail_chunk_at) {
            dev->last_sense.valid = 1;
            dev->last_sense.key = 0x03;
            dev->last_sense.asc = 0x11;
            return ACCUDISC_ERR_SENSE;
        }
    }
    if (type != 0 && nsec == 1 && fk.bad_lba && lba == fk.bad_lba &&
        fk.bad_seen++ >= fk.bad_skip) {
        dev->last_sense.valid = 1;
        dev->last_sense.key = 0x03;
        dev->last_sense.asc = 0x11;
        return ACCUDISC_ERR_SENSE;
    }
    for (uint32_t s = 0; s < nsec && (size_t)(s + 1) * slen <= cmd->buf_len;
         s++) {
        uint8_t *p = (uint8_t *)cmd->buf + (size_t)s * slen;
        uint32_t cur = lba + s;

        for (uint32_t b = 0; b < S; b++)
            p[b] = audio(cur, b);
        if (chunk && fk.chunk_reads == 2 && cur == fk.noise_lba)
            p[1000] ^= 0x5A;
        if (chunk && fk.nz_chunk && fk.chunk_reads == fk.nz_chunk &&
            cur >= fk.nz_lo && cur < fk.nz_hi)
            p[500] ^= 0x33;
        if (!chunk && nsec == 1 && type != 0 && cur >= fk.nz_lo &&
            cur < fk.nz_hi && cur - 10000 < 256 && !fk.nz_single[cur - 10000]++)
            p[700] ^= 0x44;
        memset(p + S, (cur >= fk.c2_lo && cur < fk.c2_hi) ? 0x01 : 0x00,
               c2len);
        if (chunk && fk.c2_chunk_only && cur == fk.c2_chunk_only)
            memset(p + S, 0xFF, 4);
        if (sub == 1) {
            uint32_t q = cur;
            if (fk.mispos_lba && cur == fk.mispos_lba &&
                fk.mispos_seen++ < (unsigned)(fk.mispos_reads ? fk.mispos_reads : 1))
                q = cur - 2048;
            make_sub(q, p + S + c2len);
        }
    }
    return ACCUDISC_OK;
}

/* ---- running ------------------------------------------------------------- */

static struct accudisc_device dev;
static uint8_t origin[256], status[256];

static void reset(void)
{
    free(dev.xfer_bounce);
    memset(&dev, 0, sizeof(dev));
    memset(&fk, 0, sizeof(fk));
    fk.fail_chunk_at = -1;
}

static accudisc_read_stats run(accudisc_read_req *req)
{
    accudisc_read_stats st = ACCUDISC_READ_STATS_INIT;

    assert(req->count <= sizeof(origin));
    memset(origin, 0xEE, sizeof(origin));
    memset(status, 0, sizeof(status));
    req->origin_map = origin;
    req->status_map = status;
    req->buffer_bytes = ACCUDISC_BUFFER_NONE;
    int was_valid = dev.speed_req_valid;
    unsigned was_last = dev.speed_req_last;
    assert(accudisc_read_cdda(&dev, req, NULL, NULL, &st) == ACCUDISC_OK);

    /* 0.57.0, on EVERY read: what the budget charges is exactly the stages it
     * names, less the head far read, and the head set is not a charged
     * change. A gate in the wrong helper, or a missing exemption, fails here
     * without a case of its own. */
    uint64_t charged = 0;
    static const unsigned ch[] = {
        ACCUDISC_STAGE_FALLBACK, ACCUDISC_STAGE_QPOS, ACCUDISC_STAGE_C2_RESCUE,
        ACCUDISC_STAGE_CONSENSUS, ACCUDISC_STAGE_ANCHOR, ACCUDISC_STAGE_WITNESS,
        ACCUDISC_STAGE_FAR };
    for (unsigned i = 0; i < sizeof(ch) / sizeof(ch[0]); i++)
        charged += st.cmds[ch[i]];
    if (st.cache_defeat == ACCUDISC_CACHE_DEFEAT_DONE ||
        st.cache_defeat == ACCUDISC_CACHE_DEFEAT_NEAR)
        charged--;
    assert(st.budget_spent_cmds == charged);
    unsigned head_change = req->speed_x &&
                           (!was_valid || was_last != req->speed_x);
    assert(st.budget_spent_changes == st.speed_changes - head_change);
    if (req->budget_cmds)
        assert(st.budget_spent_cmds <= req->budget_cmds);
    if (req->budget_speed_changes)
        assert(st.budget_spent_changes <= req->budget_speed_changes);
    if (!req->budget_cmds && !req->budget_speed_changes)
        assert(st.sectors_budget_cut == 0 && st.budget_cut_lba == -1);
    assert((st.sectors_budget_cut == 0) == (st.budget_cut_lba == -1));

    /* Invariants that hold for every read. */
    uint64_t won = 0, wins = 0, us = 0;
    for (unsigned i = 0; i < 16; i++) {
        won += st.won_by[i];
        wins += st.rung_wins[i];
        us += st.us[i];
        assert(st.cmds_failed[i] <= st.cmds[i]);
    }
    assert(won == req->count);                    /* every sector, once */
    assert(wins == st.sectors_read);              /* non-HARD ones, once */
    assert(st.us_total >= us);
    for (uint32_t i = 0; i < req->count; i++)
        assert(origin[i] != 0xEE);                /* every byte written */
    return st;
}

static void only(const accudisc_read_stats *st, unsigned stage, uint32_t n)
{
    for (unsigned i = 0; i < 16; i++)
        if (i != stage)
            assert(st->cmds[i] == 0);
    assert(st->cmds[stage] == n);
}

/* ---- the cases ----------------------------------------------------------- */

/* A clean single pass: C2 only, 92 sectors, chunks of 24 (65535 / 2646). Four
 * STREAM transfers and nothing else; every sector FIRST at rung 0. */
static void test_clean_single_pass(void)
{
    accudisc_read_req req = ACCUDISC_READ_REQ_INIT;

    reset();
    req.lba = 10000;
    req.count = 92;
    req.c2 = ACCUDISC_C2_PTRS;
    accudisc_read_stats st = run(&req);
    only(&st, ACCUDISC_STAGE_STREAM, 4);
    assert(st.won_by[ACCUDISC_ORIGIN_FIRST] == 92);
    assert(st.rung_wins[0] == 92);
    assert(st.chunks_witnessed == 0 && st.speed_changes == 0);
    for (unsigned i = 0; i < 92; i++)
        assert(origin[i] == ACCUDISC_ORIGIN_FIRST);
    printf("ok - clean single pass: 4 STREAM, 92 FIRST, nothing else\n");
}

/* verify 2 over the same clean span: each chunk is witnessed once, with one
 * far read before its second transfer. Every sector CONFIRMED. rereads stays 0,
 * which is exactly the blind spot (2026-11-25a B.4) these counters close. */
static void test_verify_clean(void)
{
    accudisc_read_req req = ACCUDISC_READ_REQ_INIT;

    reset();
    req.lba = 10000;
    req.count = 92;
    req.c2 = ACCUDISC_C2_PTRS;
    req.verify_passes = 2;
    accudisc_read_stats st = run(&req);
    assert(st.cmds[ACCUDISC_STAGE_STREAM] == 4);
    assert(st.cmds[ACCUDISC_STAGE_WITNESS] == 4);
    assert(st.cmds[ACCUDISC_STAGE_FAR] == 4);
    assert(st.cmds[ACCUDISC_STAGE_CONSENSUS] == 0);
    assert(st.rereads == 0);
    assert(st.won_by[ACCUDISC_ORIGIN_CONFIRMED] == 92);
    assert(st.chunks_witnessed == 4);

    /* Time is booked to the stage that spent it: with every READ CD taking at
     * least 2 ms, each stage holds at least its own commands' share, and the
     * speed stage, which issued nothing, holds nothing. */
    reset();
    fk.delay_us = 2000;
    st = run(&req);
    assert(st.us[ACCUDISC_STAGE_STREAM] >= 4 * 2000);
    assert(st.us[ACCUDISC_STAGE_WITNESS] >= 4 * 2000);
    assert(st.us[ACCUDISC_STAGE_FAR] >= 4 * 2000);
    assert(st.us[ACCUDISC_STAGE_SPEED] == 0);
    assert(st.us_total >= 12 * 2000);
    printf("ok - verify 2: 4 STREAM + 4 WITNESS + 4 FAR, 92 CONFIRMED, "
           "rereads 0, time booked per stage\n");
}

/* One Q misposition on the first read of 10050 (C2 + raw P-W, chunks of 23):
 * 10046-10054 are suspect (the 4-sector margin, inside chunk 10046), and each
 * gets one QPOS re-read after one far read. Those 9 are QPOS; the rest FIRST. */
static void test_qpos(void)
{
    accudisc_read_req req = ACCUDISC_READ_REQ_INIT;

    reset();
    fk.mispos_lba = 10050;
    req.lba = 10000;
    req.count = 120;
    req.c2 = ACCUDISC_C2_PTRS;
    req.sub = ACCUDISC_SUB_RAW;
    accudisc_read_stats st = run(&req);
    assert(st.cmds[ACCUDISC_STAGE_STREAM] == 6); /* 5 x 23 + 5 */
    assert(st.cmds[ACCUDISC_STAGE_QPOS] == 9);
    assert(st.cmds[ACCUDISC_STAGE_FAR] == 9);
    assert(st.rung_cmds[0] == 9);
    assert(st.won_by[ACCUDISC_ORIGIN_QPOS] == 9);
    assert(st.won_by[ACCUDISC_ORIGIN_FIRST] == 111);
    for (uint32_t l = 10046; l <= 10054; l++)
        assert(origin[l - 10000] == ACCUDISC_ORIGIN_QPOS);
    assert(origin[10045 - 10000] == ACCUDISC_ORIGIN_FIRST);
    assert(origin[10055 - 10000] == ACCUDISC_ORIGIN_FIRST);
    printf("ok - Q misposition: 9 QPOS reads + 9 FAR, 9 QPOS origins\n");
}

/* A failed chunk: the transfer at 10024 fails, so each of its 24 sectors is
 * read singly. 10030 never reads: attempt 1, then a far read and attempt 2
 * (retries default 2). So FALLBACK = 24 + 1 = 25, two of them failed, FAR = 1,
 * and the sectors are 23 FALLBACK + 1 NONE (HARD). */
static void test_fallback(void)
{
    accudisc_read_req req = ACCUDISC_READ_REQ_INIT;

    reset();
    fk.fail_chunk_at = 10024;
    fk.bad_lba = 10030;
    req.lba = 10000;
    req.count = 72;
    req.c2 = ACCUDISC_C2_PTRS;
    accudisc_read_stats st = run(&req);
    assert(st.cmds[ACCUDISC_STAGE_STREAM] == 3);
    assert(st.cmds_failed[ACCUDISC_STAGE_STREAM] == 1);
    assert(st.cmds[ACCUDISC_STAGE_FALLBACK] == 25);
    assert(st.cmds_failed[ACCUDISC_STAGE_FALLBACK] == 2);
    assert(st.cmds[ACCUDISC_STAGE_FAR] == 1);
    assert(st.hard_errors == 1);
    assert(st.won_by[ACCUDISC_ORIGIN_FALLBACK] == 23);
    assert(st.won_by[ACCUDISC_ORIGIN_NONE] == 1);
    assert(origin[10030 - 10000] == ACCUDISC_ORIGIN_NONE);
    assert(origin[10031 - 10000] == ACCUDISC_ORIGIN_FALLBACK);
    printf("ok - failed chunk: 25 FALLBACK (2 failed) + 1 FAR, 23 FALLBACK + "
           "1 NONE\n");
}

/* The C.7 case (2026-11-25a), now counted: c2_retries 3, verify 2, ladder
 * {24,8,4}, pass speed 32, five persistently flagged sectors in one chunk.
 * Every rescue attempt is used (C2 never clears), walking the ladder per
 * sector. Speed operations, by hand:
 *   head set 32 + its read-back                        2
 *   rung sets 24 8 4 for each of 5 sectors             15
 *   one read-back per rung, after its first set        3
 *   restore to 32 before the witness pass              1   => SPEED = 21
 * speed_changes = head (fresh handle) 1 + 15 + restore 1 = 17, the 17 that
 * the rung-ordered rescue (0.54.0) is to bring down to 5. */
static void test_ladder_rescue(void)
{
    static const uint16_t ladder[] = { 24, 8, 4 };
    accudisc_read_req req = ACCUDISC_READ_REQ_INIT;

    reset();
    fk.c2_lo = 10005;
    fk.c2_hi = 10010;
    req.lba = 10000;
    req.count = 46;
    req.c2 = ACCUDISC_C2_PTRS;
    req.speed_x = 32;
    req.c2_retries = 3;
    req.verify_passes = 2;
    req.speed_ladder = ladder;
    req.ladder_len = 3;
    accudisc_read_stats st = run(&req);
    assert(st.cmds[ACCUDISC_STAGE_C2_RESCUE] == 15);
    assert(st.rung_cmds[1] == 5 && st.rung_cmds[2] == 5 && st.rung_cmds[3] == 5);
    assert(st.rung_cmds[0] == 0);
    assert(st.cmds[ACCUDISC_STAGE_SPEED] == 21);
    assert(st.cmds_failed[ACCUDISC_STAGE_SPEED] == 0);
    assert(st.speed_changes == 17);
    assert(st.rung_honoured_x[0] == 32 && st.speed_honoured_x == 32);
    assert(st.rung_honoured_x[1] == 24);
    assert(st.rung_honoured_x[2] == 8);
    assert(st.rung_honoured_x[3] == 4);
    assert(st.rung_honoured_x[4] == 0);
    /* 2 chunks (24 + 22), each witnessed; 15 rescue reads + 2 witnesses = 17
     * far reads. No rescue replaced anything (bits never fell), so the five
     * flagged sectors are CONFIRMED like the rest. */
    assert(st.cmds[ACCUDISC_STAGE_FAR] == 17);
    assert(st.chunks_witnessed == 2);
    assert(st.won_by[ACCUDISC_ORIGIN_CONFIRMED] == 46);
    printf("ok - ladder rescue: 15 C2_RESCUE (5 per rung), SPEED 21, "
           "17 changes, rungs read back 24/8/4\n");
}

/* 0.54.0: the C.7 case in RUNG ORDER. The same 15 rescue reads at the same
 * rungs, but the ladder is walked once per chunk instead of once per sector:
 *   head set 32 + its read-back                        2
 *   rung sets 24 8 4, once each                        3
 *   one read-back per rung                             3
 *   restore to 32                                      1   => SPEED = 9
 *   speed_changes = 1 + 3 + 1 = 5  (17 in sector order)
 * Everything else, bytes, map, lane and every non-speed stat, equals the
 * sector-order run. */
static void test_ladder_rescue_by_rung(void)
{
    static const uint16_t ladder[] = { 24, 8, 4 };
    accudisc_read_req req = ACCUDISC_READ_REQ_INIT;
    uint8_t origin0[46], status0[46];

    reset();
    fk.c2_lo = 10005;
    fk.c2_hi = 10010;
    req.lba = 10000;
    req.count = 46;
    req.c2 = ACCUDISC_C2_PTRS;
    req.speed_x = 32;
    req.c2_retries = 3;
    req.verify_passes = 2;
    req.speed_ladder = ladder;
    req.ladder_len = 3;
    accudisc_read_stats a = run(&req);
    memcpy(origin0, origin, 46);
    memcpy(status0, status, 46);

    reset();
    fk.c2_lo = 10005;
    fk.c2_hi = 10010;
    req.rescue_by_rung = 1;
    accudisc_read_stats b = run(&req);
    assert(b.cmds[ACCUDISC_STAGE_SPEED] == 9);
    assert(b.speed_changes == 5);
    assert(b.rung_honoured_x[1] == 24 && b.rung_honoured_x[2] == 8 &&
           b.rung_honoured_x[3] == 4);
    assert(memcmp(origin0, origin, 46) == 0 && memcmp(status0, status, 46) == 0);
    for (unsigned i = 0; i < 16; i++) {
        if (i == ACCUDISC_STAGE_SPEED)
            continue;
        assert(a.cmds[i] == b.cmds[i] && a.cmds_failed[i] == b.cmds_failed[i]);
        assert(a.rung_cmds[i] == b.rung_cmds[i]);
        assert(a.won_by[i] == b.won_by[i] && a.rung_wins[i] == b.rung_wins[i]);
    }
    assert(a.rereads == b.rereads && a.sectors_flagged == b.sectors_flagged);
    printf("ok - ladder rescue in rung order: SPEED 21 -> 9, changes 17 -> 5, "
           "the same 15 reads\n");
}

/* Q-position rescue in rung order: one misposition (10050, first read only)
 * makes 9 suspects (the 4-sector margin), a 2-rung ladder. The first attempt
 * fixes every one, so all 9 are re-read at rung 1 in one round with ONE rung
 * set. */
static void test_qpos_by_rung(void)
{
    static const uint16_t ladder[] = { 8, 4 };
    accudisc_read_req req = ACCUDISC_READ_REQ_INIT;

    reset();
    fk.mispos_lba = 10050;
    req.lba = 10046;
    req.count = 23;
    req.c2 = ACCUDISC_C2_PTRS;
    req.sub = ACCUDISC_SUB_RAW;
    req.speed_x = 32;
    req.speed_ladder = ladder;
    req.ladder_len = 2;
    req.rescue_by_rung = 1;
    accudisc_read_stats st = run(&req);
    assert(st.cmds[ACCUDISC_STAGE_QPOS] == 9);
    assert(st.rung_cmds[1] == 9);
    for (uint32_t i = 0; i <= 8; i++)
        assert(origin[i] == (ACCUDISC_ORIGIN_QPOS | 1u << 4));
    /* head 32 + read-back, rung 8 + read-back, restore 32 = 5 operations */
    assert(st.cmds[ACCUDISC_STAGE_SPEED] == 5 && st.speed_changes == 3);
    printf("ok - Q rescue in rung order: 9 re-reads at rung 1, one rung set\n");
}

/* Run the same scripted read in both orders and require everything but speed
 * and time to match; return the two stats for the caller's speed checks. */
static void both_orders(void (*setup)(accudisc_read_req *), uint32_t count,
                        accudisc_read_stats *a, accudisc_read_stats *b)
{
    accudisc_read_req req = ACCUDISC_READ_REQ_INIT;
    uint8_t origin0[256], status0[256];

    reset();
    setup(&req);
    *a = run(&req);
    memcpy(origin0, origin, count);
    memcpy(status0, status, count);

    accudisc_read_req req2 = ACCUDISC_READ_REQ_INIT;
    reset();
    setup(&req2);
    req2.rescue_by_rung = 1;
    *b = run(&req2);
    assert(memcmp(origin0, origin, count) == 0);
    assert(memcmp(status0, status, count) == 0);
    for (unsigned i = 0; i < 16; i++) {
        if (i == ACCUDISC_STAGE_SPEED)
            continue;
        assert(a->cmds[i] == b->cmds[i]);
        assert(a->cmds_failed[i] == b->cmds_failed[i]);
        assert(a->rung_cmds[i] == b->rung_cmds[i]);
        assert(a->won_by[i] == b->won_by[i]);
        assert(a->rung_wins[i] == b->rung_wins[i]);
    }
    assert(a->rereads == b->rereads && a->slips == b->slips);
    assert(a->sectors_recovered == b->sectors_recovered);
    assert(a->sectors_suspect == b->sectors_suspect);
}

static const uint16_t ladder84[] = { 8, 4 };

/* Consensus, three sectors, each needing attempt 2: the witness transfer
 * (chunk transfer #2) flips a byte in 10003-10005, and each one's first
 * re-read flips a different byte, so only its second re-read (rung 2, 4x)
 * matches the first copy.
 *   sector order: head 32 + rb, then 8 + rb, 4 + rb, 8, 4, 8, 4, restore = 11
 *                 changes 1 + 6 + 1 = 8
 *   rung order:   head 32 + rb, 8 + rb, 4 + rb, restore             = 7
 *                 changes 1 + 2 + 1 = 4 */
static void setup_cons(accudisc_read_req *req)
{
    fk.nz_lo = 10003;
    fk.nz_hi = 10006;
    fk.nz_chunk = 2;
    req->lba = 10000;
    req->count = 24;
    req->c2 = ACCUDISC_C2_PTRS;
    req->speed_x = 32;
    req->verify_passes = 2;
    req->speed_ladder = ladder84;
    req->ladder_len = 2;
}

static void test_consensus_by_rung(void)
{
    accudisc_read_stats a, b;

    both_orders(setup_cons, 24, &a, &b);
    assert(a.cmds[ACCUDISC_STAGE_CONSENSUS] == 6);
    assert(a.cmds[ACCUDISC_STAGE_SPEED] == 11 && a.speed_changes == 8);
    assert(b.cmds[ACCUDISC_STAGE_SPEED] == 7 && b.speed_changes == 4);
    for (uint32_t i = 3; i <= 5; i++)
        assert(origin[i] == (ACCUDISC_ORIGIN_CONSENSUS | 2u << 4));
    printf("ok - consensus, 3 sectors x 2 attempts: changes 8 -> 4, same "
           "bytes, map and lane\n");
}

/* The same through a SEAM: overlap 2, chunk transfer #2 flips 10022-10023
 * (the head of chunk 2, which the previous transfer's extension covers clean),
 * and each sector's first re-read is noisy. The seam's consensus needs attempt
 * 2 on both sectors; sector order walks 8, 4 twice, rung order once. */
static void setup_seam(accudisc_read_req *req)
{
    fk.nz_lo = 10022;
    fk.nz_hi = 10024;
    fk.nz_chunk = 2;
    req->lba = 10000;
    req->count = 46;
    req->c2 = ACCUDISC_C2_PTRS;
    req->speed_x = 32;
    req->overlap_sectors = 2;
    req->speed_ladder = ladder84;
    req->ladder_len = 2;
}

static void test_seam_by_rung(void)
{
    accudisc_read_stats a, b;

    both_orders(setup_seam, 46, &a, &b);
    assert(a.cmds[ACCUDISC_STAGE_CONSENSUS] == 4);
    assert(b.speed_changes == a.speed_changes - 2);
    assert(b.cmds[ACCUDISC_STAGE_SPEED] == a.cmds[ACCUDISC_STAGE_SPEED] - 2);
    printf("ok - seam consensus, 2 sectors x 2 attempts: 2 fewer speed "
           "changes, same everything else\n");
}

/* Q rescue where 10050 is mispositioned on its first TWO reads (the chunk
 * transfer and its first re-read): the other 8 suspects resolve at rung 1,
 * 10050 needs rung 2. Sector order: 8 for 10046-10049, 8 then 4 for 10050,
 * 8 again for 10051 = changes 8,4,8; rung order: 8, 4. */
static void setup_qpos2(accudisc_read_req *req)
{
    fk.mispos_lba = 10050;
    fk.mispos_reads = 2;
    req->lba = 10046;
    req->count = 23;
    req->c2 = ACCUDISC_C2_PTRS;
    req->sub = ACCUDISC_SUB_RAW;
    req->speed_x = 32;
    req->speed_ladder = ladder84;
    req->ladder_len = 2;
}

static void test_qpos_two_rungs(void)
{
    accudisc_read_stats a, b;

    both_orders(setup_qpos2, 23, &a, &b);
    assert(a.cmds[ACCUDISC_STAGE_QPOS] == 10);
    assert(origin[4] == (ACCUDISC_ORIGIN_QPOS | 2u << 4));
    /* head 1 + 8,4,8 + restore 1 = 5 ; head 1 + 8,4 + restore 1 = 4 */
    assert(a.speed_changes == 5 && b.speed_changes == 4);
    printf("ok - Q rescue needing rung 2 on one sector: changes 5 -> 4\n");
}

/* rescue_by_rung and shift_search are flags. (0.54-0.56 also refused any
 * non-zero byte in reserved3, 106-111; 0.57.0 gives those to the budget.) */
static void test_flags_are_flags(void)
{
    accudisc_read_req req = ACCUDISC_READ_REQ_INIT;
    accudisc_read_stats st = ACCUDISC_READ_STATS_INIT;

    reset();
    req.lba = 10000;
    req.count = 24;
    req.rescue_by_rung = 2;
    assert(accudisc_read_cdda(&dev, &req, NULL, NULL, &st) == ACCUDISC_ERR_INVAL);
    req.rescue_by_rung = 0;
    req.shift_search = 2;
    assert(accudisc_read_cdda(&dev, &req, NULL, NULL, &st) == ACCUDISC_ERR_INVAL);
    req.shift_search = 1;
    req.budget_cmds = 0xFFFFFFFFu; /* any value is a budget now */
    req.budget_speed_changes = 0xFFFF;
    assert(accudisc_read_cdda(&dev, &req, NULL, NULL, &st) == ACCUDISC_OK);
    printf("ok - rescue_by_rung > 1 and shift_search > 1 are refused; any "
           "budget is accepted\n");
}

/* The rung nibble: the witness transfer carries a flipped byte at 10003, the
 * consensus re-read at rung 1 (8x) matches the first copy, so 10003's bytes
 * came from CONSENSUS at rung 1. */
static void test_consensus_rung(void)
{
    static const uint16_t ladder[] = { 8 };
    accudisc_read_req req = ACCUDISC_READ_REQ_INIT;

    reset();
    fk.noise_lba = 10003;
    req.lba = 10000;
    req.count = 24;
    req.c2 = ACCUDISC_C2_PTRS;
    req.speed_x = 32;
    req.verify_passes = 2;
    req.speed_ladder = ladder;
    req.ladder_len = 1;
    accudisc_read_stats st = run(&req);
    assert(origin[3] == (ACCUDISC_ORIGIN_CONSENSUS | 1u << 4));
    assert(ACCUDISC_ORIGIN_RUNG(origin[3]) == 1);
    assert(st.cmds[ACCUDISC_STAGE_CONSENSUS] == 1);
    assert(st.rung_cmds[1] == 1);
    assert(st.rung_wins[1] == 1 && st.rung_wins[0] == 23);
    assert(st.won_by[ACCUDISC_ORIGIN_CONSENSUS] == 1);
    assert(st.won_by[ACCUDISC_ORIGIN_CONFIRMED] == 23);
    assert(st.rung_honoured_x[1] == 8);
    printf("ok - consensus at rung 1: origin 0x%02x, rung_wins[1] = 1\n",
           origin[3]);
}

/* A rescue that WINS: C2 fires on 10005 in chunk transfers only, so the first
 * context read (rung 1, 8x) decodes it clean, replaces it and stops. Its bytes
 * are C2_RESCUE at rung 1; the witness agrees with them, and a rescued copy
 * keeps its class rather than becoming CONFIRMED. */
static void test_c2_rescue_wins(void)
{
    static const uint16_t ladder[] = { 8, 4 };
    accudisc_read_req req = ACCUDISC_READ_REQ_INIT;

    reset();
    fk.c2_chunk_only = 10005;
    req.lba = 10000;
    req.count = 24;
    req.c2 = ACCUDISC_C2_PTRS;
    req.speed_x = 32;
    req.c2_retries = 3;
    req.verify_passes = 2;
    req.speed_ladder = ladder;
    req.ladder_len = 2;
    accudisc_read_stats st = run(&req);
    assert(st.cmds[ACCUDISC_STAGE_C2_RESCUE] == 1);
    assert(origin[5] == (ACCUDISC_ORIGIN_C2_RESCUE | 1u << 4));
    assert(st.won_by[ACCUDISC_ORIGIN_C2_RESCUE] == 1);
    assert(st.sectors_recovered == 1);
    printf("ok - C2 rescue wins at rung 1: origin 0x%02x\n", origin[5]);
}

/* The seam extension agreeing is a second transfer: overlap 2, chunks of 22
 * (24 - 2): 22 + 22 + 2. The two sectors after each seam are CONFIRMED. */
static void test_seam_confirms(void)
{
    accudisc_read_req req = ACCUDISC_READ_REQ_INIT;

    reset();
    req.lba = 10000;
    req.count = 46;
    req.c2 = ACCUDISC_C2_PTRS;
    req.overlap_sectors = 2;
    accudisc_read_stats st = run(&req);
    only(&st, ACCUDISC_STAGE_STREAM, 3);
    assert(st.won_by[ACCUDISC_ORIGIN_CONFIRMED] == 4);
    assert(st.won_by[ACCUDISC_ORIGIN_FIRST] == 42);
    assert(origin[22] == ACCUDISC_ORIGIN_CONFIRMED);
    assert(origin[23] == ACCUDISC_ORIGIN_CONFIRMED);
    assert(origin[24] == ACCUDISC_ORIGIN_FIRST);
    assert(origin[44] == ACCUDISC_ORIGIN_CONFIRMED);
    printf("ok - seam: 4 CONFIRMED by the previous transfer's extension\n");
}

/* A speed CHANGE is judged against the handle, not the read: a second read at
 * the same speed on the same handle issues the set (SPEED counts it) but it is
 * not a change. */
static void test_speed_change_is_per_handle(void)
{
    accudisc_read_req req = ACCUDISC_READ_REQ_INIT;

    reset();
    req.lba = 10000;
    req.count = 24;
    req.c2 = ACCUDISC_C2_PTRS;
    req.speed_x = 24;
    accudisc_read_stats a = run(&req);
    assert(a.cmds[ACCUDISC_STAGE_SPEED] == 2 && a.speed_changes == 1);
    accudisc_read_stats b = run(&req); /* same handle: dev is not reset */
    assert(b.cmds[ACCUDISC_STAGE_SPEED] == 2 && b.speed_changes == 0);
    req.speed_x = 40;
    accudisc_read_stats c = run(&req);
    assert(c.speed_changes == 1 && c.speed_honoured_x == 40);
    printf("ok - speed changes judged per handle: 1, then 0, then 1\n");
}

/* An old caller: a 96-byte request (0.52.0) cannot carry origin_map, and gets
 * stats up to its own declared size. A 160-byte stats struct still works. */
static void test_old_caller(void)
{
    accudisc_read_req req = ACCUDISC_READ_REQ_INIT;
    struct { accudisc_read_stats st; } big;
    accudisc_read_stats *st = &big.st;

    reset();
    req.size = 96;
    req.lba = 10000;
    req.count = 24;
    req.c2 = ACCUDISC_C2_PTRS;
    req.buffer_bytes = ACCUDISC_BUFFER_NONE;
    memset(st, 0xAB, sizeof(*st));
    st->size = 160;
    assert(accudisc_read_cdda(&dev, &req, NULL, NULL, st) == ACCUDISC_OK);
    assert(st->size == 160 && st->sectors_read == 24);
    assert(st->us_total == 0xABABABABABABABABull); /* past 160: untouched */
    printf("ok - a 0.52 caller (96-byte request, 160-byte stats) is unaffected\n");

    /* 0.55 caller: 656-byte stats, nothing written past them (0.56.0) */
    reset();
    req.size = 112;
    memset(st, 0xAB, sizeof(*st));
    st->size = 656;
    assert(accudisc_read_cdda(&dev, &req, NULL, NULL, st) == ACCUDISC_OK);
    assert(st->size == 656 && st->sectors_read == 24);
    assert(st->slips_off_frame == 0xABABABABABABABABull);
    printf("ok - a 0.55 caller (656-byte stats) gets 656 bytes back\n");
}

/* ---- the per-read budget (0.57.0) ---------------------------------------- */

static unsigned count_map(uint32_t n, unsigned state)
{
    unsigned k = 0;

    for (uint32_t i = 0; i < n; i++)
        k += (status[i] & 15) == state;
    return k;
}

/* CONTRACT POINT 1 (notice 25w): a ROUTINE verify pass the budget cuts leaves
 * each sector as it was. A clean verify-2 read of 92 sectors (chunks of 24)
 * costs, per chunk, one far read + one witness transfer = 2 charged commands.
 *   budget 1: no witness fits; 0 spent; all 92 OK and cut, from 10000
 *   budget 3: chunk 1 (2), then 2 more would be 4 > 3: 1 left unused;
 *             chunks 2-4 cut = 68 from 10024
 *   budget 4: chunks 1-2; 44 cut from 10048
 *   budget 8: exactly enough, nothing cut
 * Never SUSPECT: a missing witness is not evidence. */
static void budget_verify(uint32_t budget, uint32_t spent, uint64_t cut,
                          int64_t cut_lba, uint32_t witnessed)
{
    accudisc_read_req req = ACCUDISC_READ_REQ_INIT;

    reset();
    req.lba = 10000;
    req.count = 92;
    req.c2 = ACCUDISC_C2_PTRS;
    req.verify_passes = 2;
    req.budget_cmds = budget;
    accudisc_read_stats st = run(&req);
    assert(st.budget_spent_cmds == spent);
    assert(st.sectors_budget_cut == cut && st.budget_cut_lba == cut_lba);
    assert(st.chunks_witnessed == witnessed);
    assert(st.cmds[ACCUDISC_STAGE_WITNESS] == witnessed);
    assert(st.sectors_suspect == 0 && st.sectors_recovered == 0);
    assert(count_map(92, ACCUDISC_MAP_OK) == 92);
    /* chunks of 24, 24, 24, 20 */
    assert(st.won_by[ACCUDISC_ORIGIN_CONFIRMED] ==
           (witnessed == 4 ? 92u : 24u * witnessed));
}

static void test_budget_routine_witness(void)
{
    budget_verify(1, 0, 92, 10000, 0);
    budget_verify(3, 2, 68, 10024, 1);
    budget_verify(4, 4, 44, 10048, 2);
    budget_verify(8, 8, 0, -1, 4);
    printf("ok - budget, routine verify cut: OK kept, 92/68/44/0 cut, never "
           "SUSPECT\n");
}

/* A failed chunk's fallback: test_fallback's read costs 26 (24 single reads,
 * and for 10030 a far read + a second read). Exactly enough: as unlimited.
 * 96 sectors, so chunk 4 reuses chunk 2's slot: a cut flag left over from
 * chunk 2 would show as a second cut sector.
 *   25: the last sector, 10047, cannot be read: HARD, zero-filled, a hard
 *       error, cut; its sense is NOT counted (10030's is).
 *   7:  10024-10029 (6) + 10030's first read (7); its second attempt needs
 *       2: refused, and so is everything after. 10030-10047 = 18 HARD, all
 *       cut, no sense counted for any of them. */
static void budget_fallback(uint32_t budget, uint32_t spent, uint64_t hard,
                            uint64_t cut, int64_t cut_lba, uint64_t sensed)
{
    accudisc_read_req req = ACCUDISC_READ_REQ_INIT;

    reset();
    fk.fail_chunk_at = 10024;
    fk.bad_lba = 10030;
    req.lba = 10000;
    req.count = 96;
    req.c2 = ACCUDISC_C2_PTRS;
    req.budget_cmds = budget;
    accudisc_read_stats st = run(&req);
    assert(st.budget_spent_cmds == spent);
    assert(st.hard_errors == hard && count_map(96, ACCUDISC_MAP_HARD) == hard);
    assert(st.sectors_budget_cut == cut && st.budget_cut_lba == cut_lba);
    assert(st.sense_medium + st.sense_hardware + st.sense_other == sensed);
    assert(st.cmds[ACCUDISC_STAGE_STREAM] == 4); /* streaming never stops */
}

static void test_budget_fallback(void)
{
    budget_fallback(26, 26, 1, 0, -1, 1);
    budget_fallback(25, 25, 2, 1, 10047, 1);
    budget_fallback(7, 7, 18, 18, 10030, 0);
    /* 8: 10030's second attempt needs 2 with 1 left: refused, and the LATCH
     * refuses 10031's 1-command attempt too, though it would fit. */
    budget_fallback(8, 7, 18, 18, 10030, 0);
    printf("ok - budget, fallback: 26 exact; 25 cuts 10047; 7 cuts "
           "10030-10047, HARD and in hard_errors, sense not counted\n");
}

/* Q rescue: test_qpos costs 9 x (far + read) = 18.
 *   17: 10046-10053 (16); 10054 needs 2: SUSPECT, cut
 *   1:  nothing fits: all 9 SUSPECT, cut from 10046 */
static void budget_qpos(uint32_t budget, uint32_t spent, uint64_t susp,
                        int64_t cut_lba, uint8_t by_rung)
{
    accudisc_read_req req = ACCUDISC_READ_REQ_INIT;

    reset();
    req.rescue_by_rung = by_rung;
    fk.mispos_lba = 10050;
    req.lba = 10000;
    req.count = 120;
    req.c2 = ACCUDISC_C2_PTRS;
    req.sub = ACCUDISC_SUB_RAW;
    req.budget_cmds = budget;
    accudisc_read_stats st = run(&req);
    assert(st.budget_spent_cmds == spent);
    assert(st.sectors_suspect == susp && st.sectors_budget_cut == susp);
    assert(st.budget_cut_lba == cut_lba);
    assert(st.sectors_recovered == 9 - susp);
    if (susp)
        assert((status[cut_lba - 10000] & 15) == ACCUDISC_MAP_SUSPECT);
}

static void test_budget_qpos(void)
{
    for (uint8_t by_rung = 0; by_rung <= 1; by_rung++) {
        budget_qpos(18, 18, 0, -1, by_rung);
        budget_qpos(17, 16, 1, 10054, by_rung); /* no ladder: one round */
        budget_qpos(1, 0, 9, 10046, by_rung);
    }
    printf("ok - budget, Q rescue (both orders): 18 exact; 17 leaves 10054 "
           "SUSPECT; 1 leaves all 9\n");
}

/* C2 rescue under a SPEED-CHANGE budget: the C.7 case (pass 32, ladder
 * {24,8,4}, five flagged sectors, c2_retries 3, verify 2). A rung change is
 * admitted only if the restore to 32 also fits, so with 3:
 *   sector order: 10005 at 24 (spent 1; 1+1 restore <= 3), at 8 (spent 2;
 *     2+1 <= 3), at 4 refused (3+1 > 3). 2 rescue reads.
 *   rung order:   round 24 (spent 1), round 8 (spent 2), round 4 refused.
 *     10 rescue reads.
 * Either way the restore spends the 3rd change, every witness is cut (the
 * latch), all 46 sectors are cut, the five stay C2 and nothing is SUSPECT.
 * With 4 in rung order the whole ladder fits: spent 4 (= 5 changes - head).
 * With 1 no rung change can ever fit: 0 rescue reads, 0 spent. */
static accudisc_read_stats budget_c2(uint16_t changes, int by_rung)
{
    static const uint16_t ladder[] = { 24, 8, 4 };
    accudisc_read_req req = ACCUDISC_READ_REQ_INIT;

    reset();
    fk.c2_lo = 10005;
    fk.c2_hi = 10010;
    req.lba = 10000;
    req.count = 46;
    req.c2 = ACCUDISC_C2_PTRS;
    req.speed_x = 32;
    req.c2_retries = 3;
    req.verify_passes = 2;
    req.speed_ladder = ladder;
    req.ladder_len = 3;
    req.rescue_by_rung = (uint8_t)by_rung;
    req.budget_speed_changes = changes;
    return run(&req);
}

static void test_budget_c2_changes(void)
{
    for (int by_rung = 0; by_rung <= 1; by_rung++) {
        accudisc_read_stats st = budget_c2(3, by_rung);
        assert(st.budget_spent_changes == 3);
        assert(st.cmds[ACCUDISC_STAGE_C2_RESCUE] == (by_rung ? 10u : 2u));
        assert(st.cmds[ACCUDISC_STAGE_WITNESS] == 0);
        assert(st.sectors_budget_cut == 46 && st.budget_cut_lba == 10000);
        assert(count_map(46, ACCUDISC_MAP_C2) == 5);
        assert(count_map(46, ACCUDISC_MAP_OK) == 41);
        assert(st.sectors_suspect == 0 && st.sectors_recovered == 0);
    }
    accudisc_read_stats full = budget_c2(4, 1);
    assert(full.budget_spent_changes == 4 && full.sectors_budget_cut == 0);
    assert(full.cmds[ACCUDISC_STAGE_C2_RESCUE] == 15);
    accudisc_read_stats none = budget_c2(1, 0);
    assert(none.budget_spent_changes == 0);
    assert(none.cmds[ACCUDISC_STAGE_C2_RESCUE] == 0);
    assert(none.sectors_budget_cut == 46);
    printf("ok - budget, C2 rescue on 3 changes: 2 reads (sector order), 10 "
           "(rung order), restore reserved; 4 fits the ladder; 1 never\n");
}

/* Consensus: setup_cons costs 1 far + 1 witness + 3 sectors x 2 attempts x
 * (far + read) = 14. With 13, 10005's second attempt (needing 2 at 12 spent)
 * is refused in both orders: SUSPECT (its copies disagreed), cut; 10003 and
 * 10004 RECOVERED as unlimited. */
static void test_budget_consensus(void)
{
    for (int by_rung = 0; by_rung <= 1; by_rung++) {
        for (uint32_t budget = 13; budget <= 14; budget++) {
            accudisc_read_req req = ACCUDISC_READ_REQ_INIT;

            reset();
            setup_cons(&req);
            req.rescue_by_rung = (uint8_t)by_rung;
            req.budget_cmds = budget;
            accudisc_read_stats st = run(&req);
            int short_one = budget == 13;
            assert(st.budget_spent_cmds == (short_one ? 12u : 14u));
            assert(st.sectors_recovered == (short_one ? 2u : 3u));
            assert(st.sectors_suspect == (short_one ? 1u : 0u));
            assert(st.sectors_budget_cut == (short_one ? 1u : 0u));
            assert(st.budget_cut_lba == (short_one ? 10005 : -1));
            assert((status[5] & 15) == (short_one ? ACCUDISC_MAP_SUSPECT
                                                  : ACCUDISC_MAP_RECOVERED));
        }
    }
    /* On 2 SPEED CHANGES (pass 32, ladder {8, 4}): a rung change needs room
     * for itself and the restore. Rung 8 fits (spent 1, +1 reserved); rung 4
     * would need 3. Sector order: 10003 at 8, then 10003 at 4 is refused;
     * rung order: all three at 8, then the round at 4 is refused. Every
     * sector's copies disagree: 3 SUSPECT, 3 cut; the restore spends the 2nd
     * change. */
    for (int by_rung = 0; by_rung <= 1; by_rung++) {
        accudisc_read_req req = ACCUDISC_READ_REQ_INIT;

        reset();
        setup_cons(&req);
        req.rescue_by_rung = (uint8_t)by_rung;
        req.budget_speed_changes = 2;
        accudisc_read_stats st = run(&req);
        assert(st.budget_spent_changes == 2);
        assert(st.cmds[ACCUDISC_STAGE_CONSENSUS] == (by_rung ? 3u : 1u));
        assert(st.sectors_suspect == 3 && st.sectors_recovered == 0);
        assert(st.sectors_budget_cut == 3 && st.budget_cut_lba == 10003);
    }
    printf("ok - budget, consensus: 14 exact; 13 leaves 10005 SUSPECT; 2 "
           "changes stop at the second rung; both orders\n");
}

/* A MISSING witness copy is not a disagreement. Chunk 2 (10024) fails as a
 * transfer every time, so its sectors are read singly, and 10030 reads only
 * the FIRST time: in the stream's fallback, not in the witness pass's. So the
 * witness has no copy of 10030, and consensus re-reads it (alt NULL). Costs:
 *   chunk 1 witness: far + read                                   2
 *   chunk 2 stream fallback: 24 single reads                      24
 *   chunk 2 witness: far + failed transfer                        2
 *   its fallback: 24 + 10030's second attempt (far + read)        26
 *   consensus on 10030: 4 attempts x (far + failed read)          8   = 62
 * 62: every re-read fails: SUSPECT, as before 0.57.0, nothing cut.
 * 54: consensus's first attempt is refused. The job saw no disagreement (one
 *     copy, nothing differing): 10030 KEEPS its state (OK), cut. */
static void test_budget_missing_witness(void)
{
    for (uint32_t budget = 54; budget <= 62; budget += 8) {
        accudisc_read_req req = ACCUDISC_READ_REQ_INIT;

        reset();
        fk.fail_chunk_at = 10024;
        fk.bad_lba = 10030;
        fk.bad_skip = 1;
        req.lba = 10000;
        req.count = 48;
        req.c2 = ACCUDISC_C2_PTRS;
        req.verify_passes = 2;
        req.budget_cmds = budget;
        accudisc_read_stats st = run(&req);
        int cut = budget == 54;
        assert(st.budget_spent_cmds == budget);
        assert(st.cmds[ACCUDISC_STAGE_CONSENSUS] == (cut ? 0u : 4u));
        assert((status[30] & 15) ==
               (cut ? ACCUDISC_MAP_OK : ACCUDISC_MAP_SUSPECT));
        assert(st.sectors_budget_cut == (cut ? 1u : 0u));
        assert(st.budget_cut_lba == (cut ? 10030 : -1));
        assert(st.sectors_suspect == (cut ? 0u : 1u));
    }
    printf("ok - budget: a missing witness copy whose re-reads are cut keeps "
           "its state; uncut it is SUSPECT as before\n");
}

/* The same missing witness copy under an EVIDENCE-triggered pass: C2 fires
 * on 10010 in chunk transfers only, so c2_witness witnesses chunk 1 and,
 * through witness_next, chunk 2. Costs exactly as above (chunk 1's witness
 * replaces the verify pass's): 62. With 54 the re-reads of 10030 are cut,
 * and since evidence condemned the transfer, 10030 is SUSPECT, not kept. */
static void test_budget_missing_witness_evidence(void)
{
    accudisc_read_req req = ACCUDISC_READ_REQ_INIT;

    reset();
    fk.fail_chunk_at = 10024;
    fk.bad_lba = 10030;
    fk.bad_skip = 1;
    fk.c2_chunk_only = 10010;
    req.lba = 10000;
    req.count = 48;
    req.c2 = ACCUDISC_C2_PTRS;
    req.c2_witness = 1;
    req.budget_cmds = 54;
    accudisc_read_stats st = run(&req);
    assert(st.budget_spent_cmds == 54);
    assert(st.chunks_witnessed == 2);
    assert((status[30] & 15) == ACCUDISC_MAP_SUSPECT);
    assert(st.sectors_suspect == 1);
    assert(st.sectors_budget_cut == 1 && st.budget_cut_lba == 10030);
    printf("ok - budget: under evidence, a missing witness copy whose re-reads "
           "are cut is SUSPECT\n");
}

/* An evidence-triggered pass cut over a chunk holding RECOVERED sectors. A Q
 * misposition at 10010 (first read) makes 10006-10014 suspect; their 9 QPOS
 * attempts (far + read) cost 18. C2 fires on 10020 in the chunk transfer, so
 * c2_witness condemns the chunk (23 sectors, C2 + raw P-W). With 18 the
 * witness is cut: the 9 Q-rescued sectors keep RECOVERED (their own stage
 * finished, and Q validated each), the other 14 are SUSPECT; all 23 cut. */
static void test_budget_evidence_keeps_recovered(void)
{
    accudisc_read_req req = ACCUDISC_READ_REQ_INIT;

    reset();
    fk.mispos_lba = 10010;
    fk.c2_chunk_only = 10020;
    req.lba = 10000;
    req.count = 23;
    req.c2 = ACCUDISC_C2_PTRS;
    req.sub = ACCUDISC_SUB_RAW;
    req.c2_witness = 1;
    req.budget_cmds = 18;
    accudisc_read_stats st = run(&req);
    assert(st.budget_spent_cmds == 18);
    assert(st.cmds[ACCUDISC_STAGE_WITNESS] == 0);
    assert(st.sectors_recovered == 9 && st.sectors_suspect == 14);
    assert(st.sectors_budget_cut == 23 && st.budget_cut_lba == 10000);
    for (uint32_t i = 6; i <= 14; i++)
        assert((status[i] & 15) == ACCUDISC_MAP_RECOVERED);
    printf("ok - budget: an evidence cut leaves Q-rescued sectors RECOVERED, "
           "the rest SUSPECT\n");
}

/* accudisc_read_policy (0.58.0) INTO THE ENGINE. Every output the policy can
 * produce, for each purpose and edge, run as a real read on the fake: the
 * engine must accept it (a policy emitting something the engine's own
 * validation refuses is caught here, not by the pure tests), and wherever the
 * policy asked for the head far read, the engine must have made one, DONE or
 * NEAR, never failed for want of a target. */
static void policy_read(uint8_t purpose, int c2_usable, uint16_t speed,
                        const uint16_t *adm, uint8_t n, uint32_t lba,
                        uint32_t count, uint32_t lo, uint32_t hi)
{
    accudisc_read_intent in = ACCUDISC_READ_INTENT_INIT;
    accudisc_read_req req = ACCUDISC_READ_REQ_INIT;
    accudisc_read_policy_info info = ACCUDISC_READ_POLICY_INFO_INIT;

    in.purpose = purpose;
    in.c2_usable = (uint8_t)c2_usable;
    in.speed_x = speed;
    in.n_admitted = n;
    memcpy(in.admitted_x, adm, n * sizeof(*adm));
    in.lba = lba;
    in.count = count;
    in.readable_lo = lo;
    in.readable_hi = hi;
    assert(accudisc_read_policy(&in, &req, &info) == ACCUDISC_OK);

    reset();
    fk.c2_lo = lba + 5; /* a flagged sector, so the rescue path has work */
    fk.c2_hi = lba + 6;
    accudisc_read_stats st = run(&req);
    assert(st.sectors_read + st.hard_errors == count);
    if (req.cache_defeat)
        assert(st.cache_defeat == ACCUDISC_CACHE_DEFEAT_DONE ||
               st.cache_defeat == ACCUDISC_CACHE_DEFEAT_NEAR);
    else
        assert(st.cache_defeat == ACCUDISC_CACHE_DEFEAT_OFF);
    /* the ladder's contents are pinned exactly in test_policy.c */
}

static void test_policy_into_the_engine(void)
{
    static const uint16_t adm[] = { 32, 8, 24, 16 }, one[] = { 24 };
    const uint8_t PC = ACCUDISC_PURPOSE_CAPTURE,
                  PG = ACCUDISC_PURPOSE_GATED_REREAD,
                  PS = ACCUDISC_PURPOSE_SPAN_RESCUE;

    policy_read(PC, 1, 32, adm, 4, 10000, 46, 0, 200000);
    policy_read(PC, 0, 0, adm, 4, 10000, 46, 0, 0);
    policy_read(PG, 1, 24, adm, 4, 10000, 46, 0, 200000); /* FULL target */
    policy_read(PG, 1, 24, adm, 4, 2000, 46, 0, 6000);    /* NEAR target */
    policy_read(PG, 1, 24, adm, 4, 10000, 46, 10000, 10046); /* no target */
    policy_read(PG, 0, 0, adm, 4, 10000, 46, 0, 0);       /* unknown range */
    policy_read(PS, 1, 24, adm, 4, 10000, 46, 0, 200000); /* ladder 16, 8 */
    policy_read(PS, 1, 8, adm, 4, 10000, 46, 0, 200000);  /* next up: 16 */
    policy_read(PS, 1, 24, one, 1, 10000, 46, 0, 200000); /* no ladder */
    printf("ok - every policy output reads on the fake; the head far read "
           "exactly where the policy asked\n");
}

/* The head is never charged: cache_defeat's far read and the pass-speed set,
 * on budgets of 1 each, spend nothing and cut nothing. */
static void test_budget_spares_the_head(void)
{
    accudisc_read_req req = ACCUDISC_READ_REQ_INIT;

    reset();
    req.lba = 10000;
    req.count = 24;
    req.c2 = ACCUDISC_C2_PTRS;
    req.speed_x = 32;
    req.cache_defeat = 1;
    req.budget_cmds = 1;
    req.budget_speed_changes = 1;
    accudisc_read_stats st = run(&req);
    assert(st.cache_defeat == ACCUDISC_CACHE_DEFEAT_DONE);
    assert(st.cmds[ACCUDISC_STAGE_FAR] == 1 && st.speed_changes == 1);
    assert(st.budget_spent_cmds == 0 && st.budget_spent_changes == 0);
    assert(st.sectors_budget_cut == 0);
    printf("ok - budget: the head far read and pass-speed set are free\n");
}

int main(void)
{
    test_clean_single_pass();
    test_verify_clean();
    test_qpos();
    test_fallback();
    test_ladder_rescue();
    test_ladder_rescue_by_rung();
    test_qpos_by_rung();
    test_flags_are_flags();
    test_consensus_by_rung();
    test_seam_by_rung();
    test_qpos_two_rungs();
    test_consensus_rung();
    test_c2_rescue_wins();
    test_seam_confirms();
    test_speed_change_is_per_handle();
    test_old_caller();
    test_budget_routine_witness();
    test_budget_fallback();
    test_budget_qpos();
    test_budget_c2_changes();
    test_budget_consensus();
    test_budget_spares_the_head();
    test_budget_missing_witness();
    test_budget_missing_witness_evidence();
    test_budget_evidence_keeps_recovered();
    test_policy_into_the_engine();
    return 0;
}
