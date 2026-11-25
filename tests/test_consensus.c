/* Consensus against a SLIP that reproduces (0.41.0).
 *
 * Found 2026-09-16 on a LITE-ON LH-20A1S: a --verify 3 read delivered
 * 113069-113071 96 bytes (24 samples) late, marked RECOVERED, while the engine
 * counted slips on the span. consensus() accepted any reread that matched any
 * earlier copy, and single-sector rereads of that address landed 96 bytes late
 * EVERY time, so two of them agreed and the slip confirmed itself. Q cannot see
 * it: 24 samples stays inside one Q frame.
 *
 * The fake drive has to be able to be wrong that way, or it proves nothing. The
 * disc is one byte stream built from a position hash, so no two windows agree
 * by accident, and a READ CD returns the stream from lba * 2352 + shift. What
 * decides the shift is set per test, separately for each kind of transfer the
 * engine issues:
 *   - the chunk transfer (nsec > ANCHOR span), by its read index;
 *   - single-sector rereads (consensus), by address;
 *   - three-sector anchoring reads, by a rule of their own;
 * and a sector can be made silent, which matches itself at every offset.
 * adsc_dev_exec is replaced with -Wl,--wrap. */

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "internal.h"
#include "mmc/mmc.h"

#define S 2352u
/* 96 bytes = 24 samples = 4 CIRC frames, as measured (2026-09-16). fk.late
 * overrides it, for an off-frame slip (0.56.0's slips_off_frame). */
#define LATE (fk.late ? fk.late : 96)

static struct {
    int chunk_late_on;       /* 1-based chunk-transfer index read LATE; 0 none */
    int chunk_all_late;      /* every chunk transfer LATE */
    uint32_t chunk_late_from;/* with chunk_late_on: only sectors at index >= this
                              * WITHIN that transfer are late. The measured fault
                              * is a step function inside a transfer, not a
                              * uniform shift of the whole of it (18g). */
    uint32_t single_late_lo; /* single-sector reads of [lo, hi) LATE, always */
    uint32_t single_late_hi;
    int anchor_mode;         /* 0 exact, 1 always late, 2 alternate exact/late */
    uint32_t signal_only;    /* nonzero: every other sector is digital silence */
    uint32_t noise_lba;      /* chunk transfer #2 flips one byte here (no shift) */
    int noise_single;        /* single rereads of noise_lba each flip a DIFFERENT
                              * byte, so no two ever agree */
    int periodic_bg;         /* with signal_only: the silence is instead a
                              * pattern repeating every LATE bytes */
    uint32_t c2_hot_lba;     /* C2 fires on this sector in CHUNK transfers only;
                              * every reread of it decodes clean */
    uint32_t single_fail_lo; /* single-sector reads of [lo, hi) fail */
    uint32_t single_fail_hi;
    uint32_t late;           /* bytes; 0 = 96 */
    uint32_t damage_lba;     /* EVERY chunk transfer damages this sector in
                              * place, DIFFERENTLY each time (0.56.0): a copy
                              * displaced and damaged, which the exact slip test
                              * cannot see (AccuDisc 2026-11-25d) */
    unsigned singles;
    unsigned chunk_reads, anchor_reads;
} fk;

static uint8_t disc_byte(uint64_t p)
{
    uint64_t lba = p / S;

    /* signal_only: the one sector with signal keeps its first and last 200 bytes
     * silent too, so a copy shifted by LATE cannot carry any of it into a
     * neighbour — otherwise the "silent" neighbour is not silent in the shifted
     * transfer, and the test measures something else. */
    if (fk.signal_only &&
        (lba != fk.signal_only || p % S < 200 || p % S >= S - 200))
        return fk.periodic_bg ? (uint8_t)(1 + (p % LATE) * 7) : 0;
    uint64_t v = p * 0x9E3779B97F4A7C15ull;
    v ^= v >> 29;
    return (uint8_t)(v * 0xBF58476D1CE4E5B9ull >> 56);
}

int __real_adsc_dev_exec(struct accudisc_device *dev, adsc_cmd *cmd);
int __wrap_adsc_dev_exec(struct accudisc_device *dev, adsc_cmd *cmd)
{
    (void)dev;
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
    unsigned c2 = (cmd->cdb[9] >> 1) & 3u, sub = cmd->cdb[10] & 7u;
    uint32_t rec = S + (c2 == 1 ? 294u : c2 == 2 ? 296u : 0) +
                   (sub == 1 ? 96u : sub == 2 ? 16u : 0);
    int shift = 0, noise = 0;

    if (nsec == 1) {
        /* a cache-defeat read is audio-only and far away; the rest are rereads */
        if (lba >= fk.single_fail_lo && lba < fk.single_fail_hi)
            return ACCUDISC_ERR_IO;
        if (lba >= fk.single_late_lo && lba < fk.single_late_hi)
            shift = LATE;
        if (fk.noise_single && lba == fk.noise_lba)
            noise = 2 + (int)fk.singles++;
    } else if (nsec == 3) {
        fk.anchor_reads++;
        shift = fk.anchor_mode == 1   ? LATE
                : fk.anchor_mode == 2 ? (fk.anchor_reads % 2 ? 0 : LATE)
                                      : 0;
    } else {
        fk.chunk_reads++;
        if (fk.chunk_all_late || (int)fk.chunk_reads == fk.chunk_late_on)
            shift = LATE;
        noise = fk.chunk_reads == 2;
    }
    memset(cmd->buf, 0, cmd->buf_len);
    for (uint32_t s = 0; s < nsec; s++) {
        uint8_t *o = (uint8_t *)cmd->buf + (size_t)s * rec;
        int sh = shift;
        if (sh && nsec != 1 && nsec != 3 && s < fk.chunk_late_from)
            sh = 0; /* the step has not happened yet in this transfer */
        uint64_t base = (uint64_t)(lba + s) * S + (uint64_t)sh;

        for (uint32_t i = 0; i < S; i++)
            o[i] = disc_byte(base + i);
        if (noise == 1 && fk.noise_lba && lba + s == fk.noise_lba)
            o[1000] ^= 0x5A;
        if (noise >= 2)
            o[100 + 13 * noise] ^= 0xA5;
        if (fk.damage_lba && nsec != 1 && nsec != 3 && lba + s == fk.damage_lba)
            for (uint32_t j = 0; j < 40; j++) /* no two transfers share a byte */
                o[(j * 37 + fk.chunk_reads * 211) % S] ^=
                    (uint8_t)(0x11 * fk.chunk_reads | 1);
        if (c2 == 1 && nsec != 1 && nsec != 3 && lba + s == fk.c2_hot_lba)
            memset(o + S, 0xFF, 4); /* 32 fired bits */
    }
    return ACCUDISC_OK;
}

/* ---- what came out ------------------------------------------------------- */

#define MAXN 16

static struct {
    uint32_t lba0, n;
    char verdict[MAXN]; /* 'E' exact, 'L' late by LATE, '?' anything else */
} out;

static int sink(void *user, const accudisc_chunk *c)
{
    (void)user;
    for (uint32_t s = 0; s < c->nsec; s++) {
        const uint8_t *a = c->data + (size_t)s * c->sector_len;
        uint32_t lba = c->lba + s;
        int exact = 1, late = 1;

        for (uint32_t i = 0; i < S; i++) {
            exact &= a[i] == disc_byte((uint64_t)lba * S + i);
            late &= a[i] == disc_byte((uint64_t)lba * S + LATE + i);
        }
        assert(lba - out.lba0 < MAXN);
        out.verdict[lba - out.lba0] = exact ? 'E' : late ? 'L' : '?';
    }
    return 0;
}

/* 0.53.0: the origin lane of the last run(), so the anchoring cases can say
 * WHICH read's bytes went out, not only that the sector was recovered. */
static uint8_t omap[MAXN];

static accudisc_read_stats run_once(accudisc_read_req *req, uint8_t *map,
                                    uint8_t by_rung)
{
    static struct accudisc_device dev;
    accudisc_read_stats st = ACCUDISC_READ_STATS_INIT;

    free(dev.xfer_bounce);
    memset(&dev, 0, sizeof(dev));
    memset(&out, 0, sizeof(out));
    out.lba0 = req->lba;
    out.n = req->count;
    req->c2 = ACCUDISC_C2_PTRS;
    req->sub = ACCUDISC_SUB_NONE;
    req->status_map = map;
    req->buffer_bytes = ACCUDISC_BUFFER_NONE;
    req->rescue_by_rung = by_rung;
    memset(omap, 0, sizeof(omap));
    req->origin_map = req->count <= MAXN ? omap : NULL;
    assert(accudisc_read_cdda(&dev, req, sink, NULL, &st) == ACCUDISC_OK);
    return st;
}

/* EVERY CASE IS ALSO AN EQUIVALENCE TEST (0.54.0). The same read runs twice
 * from the same fake state, in sector order and then in rung order
 * (rescue_by_rung), and everything a caller can see must match: the bytes
 * delivered, the status map, the origin lane, the fake's own read counters
 * (so the same reads were issued), and every stat except the ones the
 * ordering exists to change: speed operations, speed changes and time. Then
 * each test's own assertions run on the rung-order result. */
static void zero_order_dependent(accudisc_read_stats *st)
{
    memset(st->us, 0, sizeof(st->us));
    st->us_total = 0;
    st->cmds[ACCUDISC_STAGE_SPEED] = 0;
    st->cmds_failed[ACCUDISC_STAGE_SPEED] = 0;
    st->speed_changes = 0;
}

static unsigned equivalence_runs;

static accudisc_read_stats run_orders(accudisc_read_req *req, uint8_t *map)
{
    __typeof__(fk) before = fk;
    uint8_t map0[MAXN], omap0[MAXN];
    char verdict0[MAXN];
    uint32_t n = req->count < MAXN ? req->count : MAXN;

    if (map)
        memset(map, 0, n);
    accudisc_read_stats a = run_once(req, map, 0);
    __typeof__(fk) after0 = fk;
    if (map)
        memcpy(map0, map, n);
    memcpy(omap0, omap, sizeof(omap0));
    memcpy(verdict0, out.verdict, sizeof(verdict0));

    fk = before;
    if (map)
        memset(map, 0, n);
    accudisc_read_stats b = run_once(req, map, 1);

    assert(memcmp(verdict0, out.verdict, sizeof(verdict0)) == 0);
    if (map)
        assert(memcmp(map0, map, n) == 0);
    assert(memcmp(omap0, omap, sizeof(omap0)) == 0);
    assert(after0.singles == fk.singles);
    assert(after0.chunk_reads == fk.chunk_reads);
    assert(after0.anchor_reads == fk.anchor_reads);
    accudisc_read_stats za = a, zb = b;
    zero_order_dependent(&za);
    zero_order_dependent(&zb);
    assert(memcmp(&za, &zb, sizeof(za)) == 0);
    equivalence_runs++;
    return b;
}

/* AND EVERY CASE IS RUN WITH shift_search OFF AND ON (0.56.0), and must come
 * out the same: these cases' slips are exact or absent, and the flag may only
 * ADD detections of damaged ones. The cases built to differ call run_orders()
 * with the flag set themselves. Returns the flag-on result. */
static unsigned shift_runs;

static accudisc_read_stats run(accudisc_read_req *req, uint8_t *map)
{
    __typeof__(fk) before = fk;
    uint8_t map0[MAXN], omap0[MAXN];
    char verdict0[MAXN];
    uint32_t n = req->count < MAXN ? req->count : MAXN;

    req->shift_search = 0;
    accudisc_read_stats a = run_orders(req, map);
    __typeof__(fk) after0 = fk;
    if (map)
        memcpy(map0, map, n);
    memcpy(omap0, omap, sizeof(omap0));
    memcpy(verdict0, out.verdict, sizeof(verdict0));

    fk = before;
    req->shift_search = 1;
    accudisc_read_stats b = run_orders(req, map);
    req->shift_search = 0;

    if (memcmp(verdict0, out.verdict, sizeof(verdict0)) != 0)
        fprintf(stderr, "shift_search off %.*s, on %.*s (lba %u)\n", (int)n,
                verdict0, (int)n, out.verdict, req->lba);
    assert(memcmp(verdict0, out.verdict, sizeof(verdict0)) == 0);
    if (map)
        assert(memcmp(map0, map, n) == 0);
    assert(memcmp(omap0, omap, sizeof(omap0)) == 0);
    assert(after0.singles == fk.singles);
    assert(after0.chunk_reads == fk.chunk_reads);
    assert(after0.anchor_reads == fk.anchor_reads);
    zero_order_dependent(&a);
    accudisc_read_stats zb = b;
    zero_order_dependent(&zb);
    assert(memcmp(&a, &zb, sizeof(a)) == 0);
    shift_runs++;
    return b;
}

static void show(const char *name, const accudisc_read_stats *st,
                 const uint8_t *map, uint32_t n)
{
    printf("%-22s delivered ", name);
    for (uint32_t i = 0; i < n; i++)
        putchar(out.verdict[i]);
    printf("  map ");
    for (uint32_t i = 0; i < n; i++)
        printf("%x", map[i] & 15);
    printf("  recovered=%llu suspect=%llu slips=%llu anchor_reads=%u\n",
           (unsigned long long)st->sectors_recovered,
           (unsigned long long)st->sectors_suspect,
           (unsigned long long)st->slips, fk.anchor_reads);
}

/* ---- the cases ----------------------------------------------------------- */

/* THE REGRESSION. Verify pass 2's chunk transfer lands late, and single-sector
 * rereads of 20001-20003 land late every time; anchoring reads land true. Before
 * 0.41.0 the engine delivered those three LATE, marked RECOVERED. Now every
 * sector must be exact, and every disagreement anchored — RECOVERED, not
 * SUSPECT, because the drive gave a consistent true alignment to settle on. */
static void test_reproducing_slip_is_anchored(void)
{
    accudisc_read_req req = ACCUDISC_READ_REQ_INIT;
    uint8_t map[MAXN] = {0};

    memset(&fk, 0, sizeof(fk));
    fk.chunk_late_on = 2;
    fk.single_late_lo = 20001;
    fk.single_late_hi = 20004;
    req.lba = 20000;
    req.count = 8;
    req.verify_passes = 3;
    accudisc_read_stats st = run(&req, map);

    show("reproducing slip", &st, map, 8);
    assert(memcmp(out.verdict, "EEEEEEEE", 8) == 0);
    assert(st.sectors_suspect == 0 && st.sectors_recovered == 8);
    for (unsigned i = 0; i < 8; i++)
        assert((map[i] & 15) == ACCUDISC_MAP_RECOVERED);
    assert(st.slips >= 8 && fk.anchor_reads > 0);
    assert(st.slips_off_frame == 0); /* 24 samples: 4 whole frames */
    /* 0.53.0: the slipped sectors' bytes came from anchoring, at the pass
     * speed: the reread that reproduced the slip is never the winner. */
    for (unsigned i = 1; i < 4; i++)
        assert(omap[i] == ACCUDISC_ORIGIN_ANCHOR);
    assert(st.won_by[ACCUDISC_ORIGIN_ANCHOR] >= 3);
    assert(st.cmds[ACCUDISC_STAGE_ANCHOR] == fk.anchor_reads);

    /* With a ladder, the consensus rereads run at rung 1, and the restore
     * before anchoring must put the RUNG back too: the anchored bytes were
     * read at the pass speed, so their origin carries rung 0. */
    static const uint16_t rung8[] = { 8 };
    memset(&fk, 0, sizeof(fk));
    memset(map, 0, sizeof(map));
    fk.chunk_late_on = 2;
    fk.single_late_lo = 20001;
    fk.single_late_hi = 20004;
    req.speed_ladder = rung8;
    req.ladder_len = 1;
    st = run(&req, map);
    assert(memcmp(out.verdict, "EEEEEEEE", 8) == 0);
    for (unsigned i = 1; i < 4; i++)
        assert(omap[i] == ACCUDISC_ORIGIN_ANCHOR); /* rung nibble 0 */
    assert(st.rung_cmds[1] > 0);                   /* the ladder was used */
    req.speed_ladder = NULL;
    req.ladder_len = 0;

    /* The same with every single-sector reread of 20001-20003 FAILING: the
     * verify comparison's own shift verdict is the only evidence, so it must be
     * carried into consensus rather than rediscovered among rereads. */
    memset(&fk, 0, sizeof(fk));
    memset(map, 0, sizeof(map));
    fk.chunk_late_on = 2;
    fk.single_fail_lo = 20001;
    fk.single_fail_hi = 20004;
    st = run(&req, map);

    show("slip, no rereads", &st, map, 8);
    assert(memcmp(out.verdict, "EEEEEEEE", 8) == 0);
    for (unsigned i = 1; i < 4; i++)
        assert((map[i] & 15) == ACCUDISC_MAP_RECOVERED);
}

/* Anchoring reads that land true and late in turn corroborate BOTH alignments.
 * Nothing can choose between them: SUSPECT, never RECOVERED, and the copy
 * delivered is never the late one.
 *
 * Sector 20000 is excluded, and why matters. Its second anchoring window starts
 * at 19998, and neither of that window's neighbours lies in any transfer the
 * read made, so the late read corroborates NOTHING rather than the other side,
 * and the two true reads anchor it. That is the rule working (an uncorroborated
 * read is not a vote), not a conflict the fake failed to create. Every other
 * sector's windows are covered, so both alternations land. */
static void test_conflicting_anchors_are_suspect(void)
{
    accudisc_read_req req = ACCUDISC_READ_REQ_INIT;
    uint8_t map[MAXN] = {0};

    memset(&fk, 0, sizeof(fk));
    fk.chunk_late_on = 2;
    fk.single_late_lo = 20001;
    fk.single_late_hi = 20004;
    fk.anchor_mode = 2;
    req.lba = 20000;
    req.count = 8;
    req.verify_passes = 2;
    accudisc_read_stats st = run(&req, map);

    show("conflicting anchors", &st, map, 8);
    assert(memcmp(out.verdict, "EEEEEEEE", 8) == 0);
    for (unsigned i = 1; i < 8; i++)
        assert((map[i] & 15) == ACCUDISC_MAP_SUSPECT);
    assert(st.sectors_suspect == 7);
}

/* A neighbour that is digital silence matches a late copy of itself as well as
 * a true one, so it cannot fix an alignment. Only the middle of 20002 carries
 * signal, and the anchoring reads ALL land late: if silence counted as corroboration, 20002
 * would be "anchored" late and delivered late. It must be SUSPECT, delivered as
 * the primary transfer had it. */
static void test_silent_neighbours_do_not_anchor(void)
{
    accudisc_read_req req = ACCUDISC_READ_REQ_INIT;
    uint8_t map[MAXN] = {0};

    memset(&fk, 0, sizeof(fk));
    fk.chunk_late_on = 2;
    fk.single_late_lo = 20002;
    fk.single_late_hi = 20003;
    fk.anchor_mode = 1;
    fk.signal_only = 20002;
    req.lba = 20000;
    req.count = 5;
    req.verify_passes = 2;
    accudisc_read_stats st = run(&req, map);

    show("silent neighbours", &st, map, 5);
    assert(memcmp(out.verdict, "EEEEE", 5) == 0);
    assert((map[2] & 15) == ACCUDISC_MAP_SUSPECT);
    assert(st.sectors_suspect == 1 && st.sectors_recovered == 0);
    assert(fk.anchor_reads > 0); /* it tried, and silence did not count */
}

/* The same, where the neighbours are not silent but REPEAT every 96 bytes: a
 * copy shifted by exactly that period matches itself, so it fixes no alignment
 * either, though no byte of it is constant. */
static void test_periodic_neighbours_do_not_anchor(void)
{
    accudisc_read_req req = ACCUDISC_READ_REQ_INIT;
    uint8_t map[MAXN] = {0};

    memset(&fk, 0, sizeof(fk));
    fk.chunk_late_on = 2;
    fk.single_late_lo = 20002;
    fk.single_late_hi = 20003;
    fk.anchor_mode = 1;
    fk.signal_only = 20002;
    fk.periodic_bg = 1;
    req.lba = 20000;
    req.count = 5;
    req.verify_passes = 2;
    accudisc_read_stats st = run(&req, map);

    show("periodic neighbours", &st, map, 5);
    assert(out.verdict[2] == 'E');
    assert((map[2] & 15) == ACCUDISC_MAP_SUSPECT);
    assert(fk.anchor_reads > 0);
}

/* Ordinary instability, no shift anywhere: pass 2 differs by one byte on one
 * sector. Consensus settles it exactly as before 0.41.0, and not one anchoring
 * read is spent — the anchor is for slips only. */
static void test_plain_disagreement_needs_no_anchor(void)
{
    accudisc_read_req req = ACCUDISC_READ_REQ_INIT;
    uint8_t map[MAXN] = {0};

    memset(&fk, 0, sizeof(fk));
    fk.noise_lba = 20004;
    req.lba = 20000;
    req.count = 8;
    req.verify_passes = 2;
    accudisc_read_stats st = run(&req, map);

    show("plain disagreement", &st, map, 8);
    assert(memcmp(out.verdict, "EEEEEEEE", 8) == 0);
    assert(st.sectors_recovered == 1 && st.sectors_suspect == 0);
    assert((map[4] & 15) == ACCUDISC_MAP_RECOVERED);
    assert(st.slips == 0 && fk.anchor_reads == 0);
}

/* Instability whose rereads never agree, and no shift anywhere: SUSPECT, as
 * before 0.41.0, and no anchoring read spent. Anchoring is not a second chance
 * for damage; a damaged sector read with its neighbours is still damaged. */
static void test_unsettled_damage_is_not_anchored(void)
{
    accudisc_read_req req = ACCUDISC_READ_REQ_INIT;
    uint8_t map[MAXN] = {0};

    memset(&fk, 0, sizeof(fk));
    fk.noise_lba = 20004;
    fk.noise_single = 1;
    req.lba = 20000;
    req.count = 8;
    req.verify_passes = 2;
    accudisc_read_stats st = run(&req, map);

    show("unsettled damage", &st, map, 8);
    assert((map[4] & 15) == ACCUDISC_MAP_SUSPECT);
    assert(st.sectors_suspect == 1 && st.slips == 0 && fk.anchor_reads == 0);
}

/* No sector may come out displaced with a map byte that says it is fine. That
 * is the defect overlap_sectors had until the seam check widened: it found a
 * displaced transfer, repaired the two sectors it compared, and delivered the
 * rest of that transfer as OK. */
static void assert_no_false_ok(const uint8_t *map, uint32_t n)
{
    for (uint32_t i = 0; i < n; i++)
        assert(out.verdict[i] == 'E' ||
               (map[i] & 15) == ACCUDISC_MAP_SUSPECT);
}

/* The seam path, BACKWARD. Chunks of 4 with 2 sectors of overlap, no verify:
 * the first chunk's transfer (with its extension) lands late, the second lands
 * true, and single-sector rereads of the seam land late every time. Before
 * 0.41.0 the seam check "confirmed" the late extension into the second chunk's
 * head. Before the seam widened, the seam sectors came out right and the first
 * chunk's own four came out LATE marked OK: the seam check had seen that
 * transfer displaced, and the chunk had already been delivered. */
static void test_seam_slip_is_anchored(void)
{
    accudisc_read_req req = ACCUDISC_READ_REQ_INIT;
    uint8_t map[MAXN] = {0};

    memset(&fk, 0, sizeof(fk));
    fk.chunk_late_on = 1;
    fk.single_late_lo = 20004;
    fk.single_late_hi = 20006;
    req.lba = 20000;
    req.count = 12;
    req.chunk_sectors = 4;
    req.overlap_sectors = 2;
    accudisc_read_stats st = run(&req, map);

    show("seam slip", &st, map, 12);
    assert(st.slips > 0 && st.slips_off_frame == 0);
    assert(out.verdict[4] == 'E' && out.verdict[5] == 'E');
    assert((map[4] & 15) == ACCUDISC_MAP_RECOVERED);
    assert((map[5] & 15) == ACCUDISC_MAP_RECOVERED);
    assert(fk.anchor_reads > 0);
    for (uint32_t i = 0; i < 4; i++) /* the chunk BEHIND the seam */
        assert(out.verdict[i] == 'E' &&
               (map[i] & 15) == ACCUDISC_MAP_RECOVERED);
    assert_no_false_ok(map, 12);

    /* Every single-sector reread of the seam FAILS: the only evidence of the
     * slip is the pair the seam check itself compared, so it must be carried
     * into consensus rather than rediscovered there. */
    memset(&fk, 0, sizeof(fk));
    memset(map, 0, sizeof(map));
    fk.chunk_late_on = 1;
    fk.single_fail_lo = 20004;
    fk.single_fail_hi = 20006;
    st = run(&req, map);

    show("seam slip, no rereads", &st, map, 12);
    assert(out.verdict[4] == 'E' && out.verdict[5] == 'E');
    assert((map[4] & 15) == ACCUDISC_MAP_RECOVERED);
    assert((map[5] & 15) == ACCUDISC_MAP_RECOVERED);
    assert_no_false_ok(map, 12);
}

/* The seam path, FORWARD. Now the SECOND chunk's transfer lands late: its head
 * (20004-20005) mismatches the first chunk's extension, and 20006-20007 are late
 * in the same transfer but were never compared with anything. Before the seam
 * widened they came out LATE marked OK — at the real chunk of 23 with overlap
 * 4, nineteen sectors per displaced transfer. */
static void test_seam_slip_widens_forward(void)
{
    accudisc_read_req req = ACCUDISC_READ_REQ_INIT;
    uint8_t map[MAXN] = {0};

    memset(&fk, 0, sizeof(fk));
    fk.chunk_late_on = 2;
    req.lba = 20000;
    req.count = 12;
    req.chunk_sectors = 4;
    req.overlap_sectors = 2;
    accudisc_read_stats st = run(&req, map);

    show("seam slip, forward", &st, map, 12);
    for (uint32_t i = 0; i < 12; i++)
        assert(out.verdict[i] == 'E');
    for (uint32_t i = 4; i < 8; i++) /* the displaced transfer, all of it */
        assert((map[i] & 15) == ACCUDISC_MAP_RECOVERED);
    assert_no_false_ok(map, 12);
    /* Two bad seams (the late transfer's own extension disagrees with the
     * chunk after it), and a chunk is witnessed ONCE: three chunk transfers,
     * then chunks 1 and 2 at the first seam and only chunk 3 at the second. */
    assert(fk.chunk_reads == 6);

    /* The displaced transfer is the LAST chunk: no later seam can reach back
     * and repair it, so only the witness of the chunk AHEAD of the seam does.
     * At the end of every read, this is the only protection there is. */
    memset(&fk, 0, sizeof(fk));
    memset(map, 0, sizeof(map));
    fk.chunk_late_on = 2;
    req.count = 8;
    st = run(&req, map);

    show("seam slip, last chunk", &st, map, 8);
    for (uint32_t i = 0; i < 8; i++)
        assert(out.verdict[i] == 'E');
    assert_no_false_ok(map, 8);
}

/* The seam's only evidence is DAMAGED as well as displaced. One sector of
 * overlap, and the second transfer's copy of it is late AND has a flipped byte,
 * so it is not an exact shift of the first transfer's copy — which is what a
 * displaced read of a damaged area looks like (18k: "best alignment, with
 * damage"). The widening must still fire: a trigger that waits for a clean
 * shift would miss exactly the transfers it exists for, and deliver 20005-20007
 * late marked OK. */
static void test_seam_widens_on_damaged_evidence(void)
{
    accudisc_read_req req = ACCUDISC_READ_REQ_INIT;
    uint8_t map[MAXN] = {0};

    memset(&fk, 0, sizeof(fk));
    fk.chunk_late_on = 2;
    fk.noise_lba = 20004; /* chunk transfer #2 flips a byte here */
    req.lba = 20000;
    req.count = 8; /* the late transfer is LAST: no later seam to rescue it */
    req.chunk_sectors = 4;
    req.overlap_sectors = 1;
    accudisc_read_stats st = run_orders(&req, map);

    show("seam, damaged evidence", &st, map, 8);
    for (uint32_t i = 0; i < 8; i++)
        assert(out.verdict[i] == 'E');
    for (uint32_t i = 5; i < 8; i++)
        assert((map[i] & 15) == ACCUDISC_MAP_RECOVERED);
    assert_no_false_ok(map, 8);
    assert(st.slips == 3 && omap[4] == ACCUDISC_ORIGIN_CONSENSUS);

    /* THE ONE CASE HERE THAT shift_search CHANGES, and why it should (0.56.0).
     * 20004's seam pair is late AND damaged, so the exact test missed the slip
     * and single re-reads, landing true, settled it by agreement. With the
     * flag the slip is seen, so agreement is no longer evidence: anchoring
     * decides, and it cannot, since the true extension has no neighbour to
     * corroborate. SUSPECT, and the copy held goes out. That is exactly what
     * the same transfer WITHOUT the damage gets with the flag off: the flag
     * makes a damaged slip count as a slip, no more. */
    uint8_t map_on[MAXN] = {0}, omap_on[MAXN];
    memset(&fk, 0, sizeof(fk));
    fk.chunk_late_on = 2;
    fk.noise_lba = 20004;
    req.shift_search = 1;
    accudisc_read_stats on = run_orders(&req, map_on);
    memcpy(omap_on, omap, sizeof(omap_on));
    show("  ...shift_search on", &on, map_on, 8);
    assert(out.verdict[4] == '?' && (map_on[4] & 15) == ACCUDISC_MAP_SUSPECT);
    assert(on.slips == st.slips + 1);
    assert_no_false_ok(map_on, 8);

    memset(&fk, 0, sizeof(fk));
    fk.chunk_late_on = 2; /* the same slip, undamaged, flag off */
    req.shift_search = 0;
    accudisc_read_stats exact = run_orders(&req, map);
    assert(out.verdict[4] == 'L');
    assert(memcmp(map, map_on, 8) == 0 && memcmp(omap, omap_on, 8) == 0);
    assert(exact.slips == on.slips &&
           exact.sectors_suspect == on.sectors_suspect &&
           exact.sectors_recovered == on.sectors_recovered);
}

/* The step inside a transfer, which is the shape of the fault actually
 * measured: chunk 1's transfer is exact for its first two sectors and late from
 * the third on, through its extension. The seam fires, and the witness must
 * settle PER SECTOR: 20002-20003 repaired, 20000-20001 confirmed as they were.
 * A widening that condemned the transfer wholesale would mark the good two
 * RECOVERED (or worse, SUSPECT) — the item-2 C2 trigger leans on this too. */
static void test_seam_step_inside_transfer(void)
{
    accudisc_read_req req = ACCUDISC_READ_REQ_INIT;
    uint8_t map[MAXN] = {0};

    memset(&fk, 0, sizeof(fk));
    fk.chunk_late_on = 1;
    fk.chunk_late_from = 2;
    req.lba = 20000;
    req.count = 12;
    req.chunk_sectors = 4;
    req.overlap_sectors = 2;
    accudisc_read_stats st = run(&req, map);

    show("seam, step in transfer", &st, map, 12);
    for (uint32_t i = 0; i < 12; i++)
        assert(out.verdict[i] == 'E');
    assert((map[0] & 15) == ACCUDISC_MAP_OK && (map[1] & 15) == ACCUDISC_MAP_OK);
    for (uint32_t i = 2; i < 6; i++)
        assert((map[i] & 15) == ACCUDISC_MAP_RECOVERED);
    assert_no_false_ok(map, 12);
}

/* A RUNT last chunk with overlap on: count 10 in chunks of 4 leaves 2. That
 * exercises the extension at the tail (chunk 2 extends by only what remains),
 * the final publish with n < chunk, and the hold-back's last-chunk path at
 * once — the shape 113114 had (a 3-sector runt from --count 49). The runt's own
 * transfer lands late. */
static void test_seam_runt_last_chunk(void)
{
    accudisc_read_req req = ACCUDISC_READ_REQ_INIT;
    uint8_t map[MAXN] = {0};

    memset(&fk, 0, sizeof(fk));
    fk.chunk_late_on = 3;
    req.lba = 20000;
    req.count = 10;
    req.chunk_sectors = 4;
    req.overlap_sectors = 2;
    accudisc_read_stats st = run(&req, map);

    show("seam, runt last chunk", &st, map, 10);
    for (uint32_t i = 0; i < 10; i++)
        assert(out.verdict[i] == 'E');
    assert((map[8] & 15) == ACCUDISC_MAP_RECOVERED &&
           (map[9] & 15) == ACCUDISC_MAP_RECOVERED);
    assert_no_false_ok(map, 10);
}

/* The widening is a COST: it re-reads only where a seam disagreed. A clean read
 * with overlap must issue one transfer per chunk and nothing more, or the
 * trigger has quietly become a second pass over everything. */
static void test_clean_seams_cost_nothing(void)
{
    accudisc_read_req req = ACCUDISC_READ_REQ_INIT;
    uint8_t map[MAXN] = {0};

    memset(&fk, 0, sizeof(fk));
    req.lba = 20000;
    req.count = 12;
    req.chunk_sectors = 4;
    req.overlap_sectors = 2;
    accudisc_read_stats st = run(&req, map);

    show("clean seams", &st, map, 12);
    assert(fk.chunk_reads == 3 && fk.anchor_reads == 0 && fk.singles == 0);
    assert(st.rereads == 0);
    for (uint32_t i = 0; i < 12; i++)
        assert(out.verdict[i] == 'E' && (map[i] & 15) == ACCUDISC_MAP_OK);
}

/* ---- c2_witness (0.45.0) ------------------------------------------------- */

static accudisc_read_stats run_c2w(uint32_t count, int c2_witness, uint8_t *map)
{
    accudisc_read_req req = ACCUDISC_READ_REQ_INIT;

    req.lba = 20000;
    req.count = count;
    req.chunk_sectors = 4;
    req.c2_witness = (uint8_t)c2_witness;
    return run(&req, map);
}

/* THE CASE IT EXISTS FOR. Chunk 2's transfer lands late, and C2 fires on ONE of
 * its sectors (20005). The other three late sectors are clean decodes of the
 * wrong samples: without the trigger they go out LATE marked OK. With it, the
 * flag condemns chunk 2 and both neighbours, and every sector comes out exact.
 * Transfers: 3 primaries plus ONE witness per chunk = 6. */
static void test_c2_witness_catches_the_unflagged(void)
{
    uint8_t map[MAXN] = {0};

    memset(&fk, 0, sizeof(fk));
    fk.chunk_late_on = 2;
    fk.c2_hot_lba = 20005;
    accudisc_read_stats st = run_c2w(12, 1, map);

    show("c2 witness", &st, map, 12);
    for (uint32_t i = 0; i < 12; i++)
        assert(out.verdict[i] == 'E');
    for (uint32_t i = 4; i < 8; i++)
        assert((map[i] & 15) == ACCUDISC_MAP_RECOVERED);
    assert_no_false_ok(map, 12);
    assert(fk.chunk_reads == 6);
}

/* THE PROVEN PATH IS UNTOUCHED WITHOUT IT (Keith, 2026-09-18). Same damage,
 * c2_witness off: one transfer per chunk and not one reread, exactly as
 * before — and the three unflagged late sectors go out marked OK, which is
 * the defect a caller opts in to fix. Asserted so that nobody turns this on
 * by default without meaning to. */
static void test_c2_witness_off_is_the_old_path(void)
{
    uint8_t map[MAXN] = {0};

    memset(&fk, 0, sizeof(fk));
    fk.chunk_late_on = 2;
    fk.c2_hot_lba = 20005;
    accudisc_read_stats st = run_c2w(12, 0, map);

    show("c2 witness OFF", &st, map, 12);
    assert(fk.chunk_reads == 3 && fk.anchor_reads == 0 && st.rereads == 0);
    assert(out.verdict[4] == 'L' && (map[4] & 15) == ACCUDISC_MAP_OK);
}

/* REACH, BACKWARD: the displaced transfer carries NO flag; the flag is in the
 * chunk after it. Chunk 1 lands late and clean; C2 fires in chunk 2. Only the
 * witness of the chunk held back behind the flag can catch chunk 1. */
static void test_c2_witness_reaches_back(void)
{
    uint8_t map[MAXN] = {0};

    memset(&fk, 0, sizeof(fk));
    fk.chunk_late_on = 1;
    fk.c2_hot_lba = 20005;
    accudisc_read_stats st = run_c2w(12, 1, map);

    show("c2 witness, back", &st, map, 12);
    for (uint32_t i = 0; i < 4; i++)
        assert(out.verdict[i] == 'E' &&
               (map[i] & 15) == ACCUDISC_MAP_RECOVERED);
    assert_no_false_ok(map, 12);
}

/* REACH, FORWARD: the flag is in chunk 2 and the displaced transfer is chunk
 * 3's, read AFTER it. Transfer order: #1 chunk 1, #2 chunk 2 (flag), #3 and #4
 * the witnesses of chunks 1 and 2, #5 chunk 3 — which lands late. Only the
 * carried trigger (witness_next) can catch it. */
static void test_c2_witness_reaches_forward(void)
{
    uint8_t map[MAXN] = {0};

    memset(&fk, 0, sizeof(fk));
    fk.chunk_late_on = 5;
    fk.c2_hot_lba = 20005;
    accudisc_read_stats st = run_c2w(12, 1, map);

    show("c2 witness, forward", &st, map, 12);
    for (uint32_t i = 8; i < 12; i++)
        assert(out.verdict[i] == 'E' &&
               (map[i] & 15) == ACCUDISC_MAP_RECOVERED);
    assert_no_false_ok(map, 12);
}

/* Free on a clean disc: no C2, no trigger, one transfer per chunk. */
static void test_c2_witness_clean_costs_nothing(void)
{
    uint8_t map[MAXN] = {0};

    memset(&fk, 0, sizeof(fk));
    accudisc_read_stats st = run_c2w(12, 1, map);

    show("c2 witness, clean", &st, map, 12);
    assert(fk.chunk_reads == 3 && fk.anchor_reads == 0 && st.rereads == 0);
    for (uint32_t i = 0; i < 12; i++)
        assert(out.verdict[i] == 'E' && (map[i] & 15) == ACCUDISC_MAP_OK);
}

/* Triggered BY C2, so without C2 capture it could never fire: refused before
 * any read rather than silently inert. */
static void test_c2_witness_requires_c2(void)
{
    static struct accudisc_device dev;
    accudisc_read_req req = ACCUDISC_READ_REQ_INIT;

    memset(&fk, 0, sizeof(fk));
    req.lba = 20000;
    req.count = 8;
    req.buffer_bytes = ACCUDISC_BUFFER_NONE;
    req.c2_witness = 1;
    req.c2 = ACCUDISC_C2_NONE;
    assert(accudisc_read_cdda(&dev, &req, NULL, NULL, NULL) ==
           ACCUDISC_ERR_INVAL);
    assert(fk.chunk_reads == 0);
    req.c2 = ACCUDISC_C2_PTRS;
    assert(accudisc_read_cdda(&dev, &req, NULL, NULL, NULL) == ACCUDISC_OK);
    free(dev.xfer_bounce);
}

/* ---- c2_retries ---------------------------------------------------------- */

/* THE SECOND PATH. The chunk copy of 20003 is C2-flagged; every reread of it
 * decodes clean. Single-sector rereads land late, and so do the context reads,
 * so no clean copy lines up with anything. Before this fix c2_rescue kept the
 * late clean reread: RECOVERED, zero bits, slips not counted. Now nothing
 * eligible exists, and the flagged copy is delivered as flagged. */
static void test_c2_rescue_refuses_unaligned_copy(void)
{
    accudisc_read_req req = ACCUDISC_READ_REQ_INIT;
    uint8_t map[MAXN] = {0};

    memset(&fk, 0, sizeof(fk));
    fk.c2_hot_lba = 20003;
    fk.single_late_lo = 20003;
    fk.single_late_hi = 20004;
    fk.anchor_mode = 1;
    req.lba = 20000;
    req.count = 8;
    req.c2_retries = 3;
    req.verify_passes = 2; /* required since 0.43.0 */
    accudisc_read_stats st = run(&req, map);

    show("c2 rescue, late", &st, map, 8);
    assert(memcmp(out.verdict, "EEEEEEEE", 8) == 0);
    assert((map[3] & 15) == ACCUDISC_MAP_C2);
    assert(st.sectors_recovered == 0 && st.sectors_flagged == 1);
    assert(fk.anchor_reads > 0);
}

/* ... and the rescue still WORKS when a clean copy lines up: context reads land
 * true, so the clean reread replaces the flagged one, RECOVERED, zero bits. */
static void test_c2_rescue_accepts_aligned_copy(void)
{
    accudisc_read_req req = ACCUDISC_READ_REQ_INIT;
    uint8_t map[MAXN] = {0};

    memset(&fk, 0, sizeof(fk));
    fk.c2_hot_lba = 20003;
    fk.single_late_lo = 20003;
    fk.single_late_hi = 20004;
    req.lba = 20000;
    req.count = 8;
    req.c2_retries = 3;
    req.verify_passes = 2; /* required since 0.43.0 */
    accudisc_read_stats st = run(&req, map);

    show("c2 rescue, aligned", &st, map, 8);
    assert(memcmp(out.verdict, "EEEEEEEE", 8) == 0);
    assert((map[3] & 15) == ACCUDISC_MAP_RECOVERED);
    assert(st.sectors_recovered == 1 && st.sectors_flagged == 0);
}

/* A clean copy whose neighbours are silent cannot be lined up, whichever way
 * the context reads land: here they land late, and a late clean copy must not
 * be spliced on the strength of silence. */
static void test_c2_rescue_silent_neighbours(void)
{
    accudisc_read_req req = ACCUDISC_READ_REQ_INIT;
    uint8_t map[MAXN] = {0};

    memset(&fk, 0, sizeof(fk));
    fk.c2_hot_lba = 20002;
    fk.signal_only = 20002;
    fk.anchor_mode = 1;
    req.lba = 20000;
    req.count = 5;
    req.c2_retries = 3;
    req.verify_passes = 2; /* required since 0.43.0 */
    accudisc_read_stats st = run(&req, map);

    show("c2 rescue, silent", &st, map, 5);
    assert(out.verdict[2] == 'E');
    assert((map[2] & 15) == ACCUDISC_MAP_C2 && st.sectors_recovered == 0);
}

/* No position witness, no rescue: c2_retries without verify_passes >= 2 is
 * refused before any read (0.43.0). */
static void test_c2_rescue_requires_verify(void)
{
    static struct accudisc_device dev;
    accudisc_read_req req = ACCUDISC_READ_REQ_INIT;

    memset(&fk, 0, sizeof(fk));
    req.lba = 20000;
    req.count = 8;
    req.c2 = ACCUDISC_C2_PTRS;
    req.buffer_bytes = ACCUDISC_BUFFER_NONE;
    req.c2_retries = 3;
    for (unsigned v = 0; v < 2; v++) {
        req.verify_passes = (uint8_t)v;
        assert(accudisc_read_cdda(&dev, &req, NULL, NULL, NULL) ==
               ACCUDISC_ERR_INVAL);
    }
    assert(fk.chunk_reads == 0 && fk.anchor_reads == 0);
    req.verify_passes = 2;
    assert(accudisc_read_cdda(&dev, &req, NULL, NULL, NULL) == ACCUDISC_OK);
    free(dev.xfer_bounce);
}

/* ---- shift_search (0.56.0) ---------------------------------------------- */

/* 25d's S1, THE CASE IT EXISTS FOR. Both chunk transfers damage 20002 in
 * place, differently, so they disagree; single re-reads of 20002 land late and
 * clean; anchoring reads land true. A late re-read against a damaged true copy
 * fails the exact test, so with the flag off two late re-reads agree and go
 * out LATE, RECOVERED: today's weakness, pinned. With it on, the first late
 * re-read is seen as displaced, agreement stops counting, and anchoring
 * delivers the true copy. */
static void test_damaged_slip_is_anchored_with_shift_search(void)
{
    accudisc_read_req req = ACCUDISC_READ_REQ_INIT;
    uint8_t map[MAXN] = {0};

    for (int flag = 0; flag <= 1; flag++) {
        memset(&fk, 0, sizeof(fk));
        memset(map, 0, sizeof(map));
        fk.damage_lba = 20002;
        fk.single_late_lo = 20002;
        fk.single_late_hi = 20003;
        req.lba = 20000;
        req.count = 5;
        req.verify_passes = 2;
        req.shift_search = (uint8_t)flag;
        accudisc_read_stats st = run_orders(&req, map);

        show(flag ? "damaged slip, on" : "damaged slip, off", &st, map, 5);
        assert(memcmp(out.verdict, flag ? "EEEEE" : "EELEE", 5) == 0);
        assert((map[2] & 15) == ACCUDISC_MAP_RECOVERED);
        assert(st.sectors_recovered == 1 && st.sectors_suspect == 0);
        assert(st.slips == 0); /* the transfers' pair is damage, not a slip */
        if (flag) {
            assert(omap[2] == ACCUDISC_ORIGIN_ANCHOR && fk.anchor_reads > 0);
        } else {
            assert(ACCUDISC_ORIGIN_CLASS(omap[2]) == ACCUDISC_ORIGIN_CONSENSUS);
            assert(fk.anchor_reads == 0);
        }
    }
}

/* Damage in place and NO shift anywhere: the flag must find nothing. The two
 * transfers' copies differ, true re-reads agree, consensus settles it, and not
 * one anchoring read is spent, with the flag off and on alike (run()). */
static void test_damage_in_place_is_not_a_slip(void)
{
    accudisc_read_req req = ACCUDISC_READ_REQ_INIT;
    uint8_t map[MAXN] = {0};

    memset(&fk, 0, sizeof(fk));
    fk.damage_lba = 20002;
    req.lba = 20000;
    req.count = 5;
    req.verify_passes = 2;
    accudisc_read_stats st = run(&req, map);

    show("damage in place", &st, map, 5);
    assert(memcmp(out.verdict, "EEEEE", 5) == 0);
    assert((map[2] & 15) == ACCUDISC_MAP_RECOVERED);
    assert(st.slips == 0 && fk.anchor_reads == 0);
    assert(ACCUDISC_ORIGIN_CLASS(omap[2]) == ACCUDISC_ORIGIN_CONSENSUS);
}

/* The WITNESS pass's own comparison. The witness transfer lands late and
 * damages 20003, so that pair is displaced AND damaged: the exact test counts
 * the other five slips, the flag counts all six. Every sector comes out true
 * and RECOVERED either way (true re-reads agree on 20003 with the flag off;
 * anchoring settles it with the flag on). Six sectors, not five: a last
 * sector whose only neighbour in the read is the damaged one has nothing to
 * corroborate it, and would be SUSPECT both ways. */
static void test_witness_sees_damaged_slip(void)
{
    accudisc_read_req req = ACCUDISC_READ_REQ_INIT;
    uint8_t map[MAXN] = {0};
    uint64_t slips[2];

    for (int flag = 0; flag <= 1; flag++) {
        memset(&fk, 0, sizeof(fk));
        memset(map, 0, sizeof(map));
        fk.chunk_late_on = 2;
        fk.damage_lba = 20003;
        req.lba = 20000;
        req.count = 6;
        req.verify_passes = 2;
        req.shift_search = (uint8_t)flag;
        accudisc_read_stats st = run_orders(&req, map);

        show(flag ? "witness damaged, on" : "witness damaged, off", &st, map, 6);
        assert(memcmp(out.verdict, "EEEEEE", 6) == 0);
        assert(st.sectors_recovered == 6 && st.sectors_suspect == 0);
        slips[flag] = st.slips;
        assert(omap[3] == (flag ? ACCUDISC_ORIGIN_ANCHOR
                                : ACCUDISC_ORIGIN_CONSENSUS)); /* rung 0 */
    }
    assert(slips[0] == 5 && slips[1] == 6);
}

/* slips_off_frame, at the witness pass and at the seam. Counted with the flag
 * off: it is about the slip, not about which test found it. The shifts are
 * chosen so that only "a multiple of 6" fits them all: 8 and 9 samples are
 * off-frame (8 is even, 9 a multiple of 3), and 6 is on-frame (not a
 * multiple of 4, 12 or 24); the measured 24 is asserted above. */
static void off_frame_witness(uint32_t late_bytes, uint64_t want_off)
{
    accudisc_read_req req = ACCUDISC_READ_REQ_INIT;
    uint8_t map[MAXN] = {0};

    memset(&fk, 0, sizeof(fk));
    fk.late = late_bytes;
    fk.chunk_late_on = 2;
    fk.single_late_lo = 20001;
    fk.single_late_hi = 20004;
    req.lba = 20000;
    req.count = 8;
    req.verify_passes = 2;
    accudisc_read_stats st = run(&req, map);
    show("slip at the witness", &st, map, 8);
    assert(memcmp(out.verdict, "EEEEEEEE", 8) == 0);
    assert(st.slips == 8 && st.slips_off_frame == want_off);
}

static void off_frame_seam(uint32_t late_bytes, uint64_t want_off)
{
    accudisc_read_req req = ACCUDISC_READ_REQ_INIT;
    uint8_t map[MAXN] = {0};

    memset(&fk, 0, sizeof(fk));
    fk.late = late_bytes;
    fk.chunk_late_on = 1;
    fk.single_late_lo = 20004;
    fk.single_late_hi = 20006;
    req.lba = 20000;
    req.count = 12;
    req.chunk_sectors = 4;
    req.overlap_sectors = 2;
    accudisc_read_stats st = run(&req, map);
    show("slip at the seam", &st, map, 12);
    assert(out.verdict[4] == 'E' && out.verdict[5] == 'E');
    assert(st.slips == 6 && st.slips_off_frame == want_off);
}

static void test_off_frame_slips_are_counted(void)
{
    off_frame_witness(32, 8); /* 8 samples */
    off_frame_witness(36, 8); /* 9 */
    off_frame_witness(24, 0); /* 6: one frame */
    off_frame_seam(32, 6);
    off_frame_seam(36, 6);
    off_frame_seam(24, 0);
}

int main(void)
{
    test_reproducing_slip_is_anchored();
    test_conflicting_anchors_are_suspect();
    test_silent_neighbours_do_not_anchor();
    test_periodic_neighbours_do_not_anchor();
    test_plain_disagreement_needs_no_anchor();
    test_unsettled_damage_is_not_anchored();
    test_seam_slip_is_anchored();
    test_seam_slip_widens_forward();
    test_seam_widens_on_damaged_evidence();
    test_seam_step_inside_transfer();
    test_seam_runt_last_chunk();
    test_clean_seams_cost_nothing();
    test_c2_witness_catches_the_unflagged();
    test_c2_witness_off_is_the_old_path();
    test_c2_witness_reaches_back();
    test_c2_witness_reaches_forward();
    test_c2_witness_clean_costs_nothing();
    test_c2_witness_requires_c2();
    test_c2_rescue_refuses_unaligned_copy();
    test_c2_rescue_accepts_aligned_copy();
    test_c2_rescue_silent_neighbours();
    test_c2_rescue_requires_verify();
    test_damaged_slip_is_anchored_with_shift_search();
    test_damage_in_place_is_not_a_slip();
    test_witness_sees_damaged_slip();
    test_off_frame_slips_are_counted();
    printf("ok - and with shift_search off and on: %u identical pairs\n",
           shift_runs);
    assert(shift_runs > 20);
    printf("ok - every case above also ran in rung order: %u equivalent pairs\n",
           equivalence_runs);
    assert(equivalence_runs > 20);
    return 0;
}
