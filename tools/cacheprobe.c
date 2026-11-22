/* How does the PX-716A's read cache give way? Keith, 2026-11-22 ("Queue up
 * testing of the drive cache and READ(10)/(12) for next session"), test 1:
 * the cache model. TODO.md, "QUEUED ... 2026-11-22 02:34".
 *
 * Question. 0.49.0's cache_defeat reads one sector ADSC_FLUSH_DISTANCE = 5000
 * away before a re-read, and on the PX-716A that works (2026-11-21, backward,
 * 3/3). Is 5000 needed? Either any out-of-sequence read invalidates the cache
 * (as on the LITE-ON LH-20A1S, 2026-09-18), so distance hardly matters, or it
 * evicts sector by sector, so only a read past the buffer (~2040 audio
 * sectors) does it. Plus: may the far target be a DATA sector read with
 * expected sector type ANY? That is what a far read bounded to a session's
 * readable range (0.51.0, not approved) would rely on.
 *
 * Method. Clean media gives identical bytes cached or fresh, so the
 * discriminator is TIMING: host CLOCK_MONOTONIC around the one READ CD that
 * re-reads the span. One process, one read-only handle, raw READ CD only (no
 * speed command, no mode pages, nothing that changes drive state).
 *
 * An instance: READ CD of a 24-sector span S (t1), one 1-sector READ CD
 * elsewhere (the far read; none for a control), READ CD of S again (t2).
 * 24 sectors = one transfer, under the 64 KiB cap.
 *
 *   A       control, no far read
 *   Bd      far read at S - d                       (behind the span)
 *   Fd      far read at S + 23 + d                  (d past its last sector)
 *   X       far read into the data track, type ANY  (flags 0xF8, 2352 bytes)
 *   d in {1, 24, 100, 500, 1000, 2040, 3000, 5000}
 *
 * Per repetition: 2 x A, 8 x B, 8 x F, 1 x X = 19 instances, in an order
 * shuffled with a fixed seed so that distance is not tied to disc radius.
 * 3 repetitions = 57 instances.
 *
 * Layout. Instances are laid out in ascending LBA, in shuffled order, so
 * that nothing an instance touches can already be cached. Read-ahead runs
 * FORWARD and cannot exceed the 4,802,784-byte buffer (2042 sectors), so after
 * each instance everything up to (its highest sector read + GUARD) counts as
 * possibly cached, and the next instance's LOWEST sector (its far target for B,
 * its span otherwise) starts above that. So every t1 is a cold read (the fresh
 * reference), and no far target was read by an EARLIER instance. But an F
 * target with d up to ~2040 lies inside the read-ahead of the instance's own
 * t1, so it may be served from the buffer without touching the disc: see the
 * rule. The X targets are placed in the data track at descending LBAs, so no X
 * target is in an earlier one's read-ahead. One discarded READ CD at LBA 500
 * (read-ahead ends below 3000, the first span) takes spin-up out of instance 0.
 * The tool refuses to run if the layout overruns session 1's audio.
 *
 * Reading rule (fixed before any read)
 * ------------------------------------
 * Validity: every command completes GOOD with no residual; else the instance
 *   is INVALID. An INVALID A, B or F instance makes the verdict UNRESOLVED.
 *   An INVALID X voids only the X arm, and its sense data is the X result
 *   (a refused type-ANY far read is itself the answer to the X question).
 * Control: every A instance must have t2 <= t1 / 4 (clearly cached), else
 *   MOOT: back-to-back re-reads are not cached here, so there is nothing to
 *   defeat. Stop.
 * Let c = the largest A t2. A test instance is
 *   FRESH if t2 >= 4c, CACHED if t2 <= 2c, else AMBIGUOUS.
 * Per arm (3 instances): FLUSHES if 3/3 FRESH, KEEPS if 3/3 CACHED, else MIXED.
 * B5000 is the known-working control (2026-11-21), printed FIRST: if it is not
 *   FLUSHES, the run disagrees with that result.
 * What each arm can and cannot show:
 *   - B100..B1000 and B2040..B5000 discriminate the models. A refill by
 *     read-ahead from S-d back into S would take tens to hundreds of ms, so
 *     FRESH means the far read cost the span its cache, and CACHED means the
 *     span survived it.
 *   - B1, B24: if the far read DOES invalidate, read-ahead from S-d+1 streams
 *     straight back into S, so a t2 near c does NOT prove survival. Only a
 *     FRESH result is evidence there (of invalidation).
 *   - F arms: a far read with tf <= 2c was served from the buffer (read-ahead
 *     hit), not from the disc. Such an instance is marked "rahit": it tested
 *     "a read from the buffer", not "an out-of-sequence media read", and says
 *     nothing about the LITE-ON-versus-eviction question. F3000 and F5000 are
 *     real forward seeks past the buffer.
 * No further reads follow any outcome without Keith.
 *
 * Mode "fua" (test 2, Keith 12:29): does a READ with FUA flush the cache?
 * ---------------------------------------------------------------------
 * Sources (TODO.md): DiscImageCreator's FlushDriveCache sends READ(12), FUA=1,
 * LBA = n+1, transfer length 0, no data, before re-reading sector n. EAC's
 * -usefua is reported to copy PlexTools' flush command; spath: every drive
 * ACCEPTS it, so only the timing counts. Test 1 showed that any read reaching
 * the disc flushes this drive, so the question is whether a command that
 * moves NO data (or is refused) does so.
 *
 * Instance: READ CD span S (t1), the middle command (tf, status and sense
 * recorded), READ CD S (t2). Arms, 3 each, plus 2 controls per repetition,
 * shuffled with the fixed seed, laid out ascending with the same guard:
 *   A    nothing
 *   U    READ(12) A8h FUA=1 LBA S+1 length 0, no data   (DIC's flush)
 *   N    READ(12) A8h FUA=0 LBA S+1 length 0, no data   (is it FUA or the CDB?)
 *   R    READ(10) 28h FUA=1 LBA S+1 length 0, no data   (CacheExplorer's 28h)
 *   D    READ(12) A8h FUA=1 LBA S+1 length 1, 2048-byte buffer
 *   D0   READ(12) A8h FUA=0 LBA S+1 length 1, 2048-byte buffer
 *        (D/D0: expected CHECK CONDITION on an audio sector; the sense is
 *         the "does it answer at all" result, t2 says whether the refusal
 *         flushed anyway)
 * Rule: as above. Only the two span READ CDs must be GOOD (else UNRESOLVED);
 *   the middle command's status is a RESULT, never a validity failure.
 *   Same MOOT control, same c / FRESH / CACHED / per-arm verdicts.
 * Reading: U FLUSHES with N KEEPS = the FUA bit does it (the flush works on
 *   this drive, through this bridge). U and N both FLUSH = the zero-length
 *   read itself does it; FUA is irrelevant here. A USB bridge may mishandle a
 *   no-data READ, so every result is "through the bridge".
 * Status splits the verdict, for U, N and R alike:
 *   KEEPS + GOOD          = accepted and ignored: a real negative (spath's
 *                           caveat), for this drive through this bridge.
 *   KEEPS + CHECK COND.   = refused (sense recorded). The drive cannot be told
 *                           apart from the bridge here: "not delivered or
 *                           refused", NOT "does not work on the PX-716A".
 *   st = -1               = transport failure: no drive result; that arm is
 *                           void.
 *   FLUSHES + CHECK COND. = refused but flushed anyway; not a clean success.
 * What tf (the middle command's time) means if U FLUSHES: tens of ms = it
 *   forced a disc access, i.e. test 1's mechanism, no cheaper than a 1-sector
 *   far read. About 1 ms or less = the drive dropped its cache WITHOUT a disc
 *   access, a genuinely cheaper flush. That is the only outcome that adds
 *   something practical beyond test 1.
 * D/D0 ask for 1 block into a 2352-byte buffer, so an over-length answer
 *   shows as a smaller residual rather than an over-transfer on the bridge.
 *
 * Build and run (CAP_SYS_RAWIO is not needed for READ CD, but the library's
 * open path is the same as the CLI's):
 *   gcc -O2 -Wall -Wextra -o build/cacheprobe tools/cacheprobe.c \
 *       -I include -I src build/src/libaccudisc.a -ldl
 *   flock /var/tmp/sr0.lock timeout 300 ./build/cacheprobe /dev/sr0 \
 *       /var/tmp/cacheprobe-<stamp>.tsv
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <accudisc/accudisc.h>

#include "internal.h"

#define SPAN 24u
#define GUARD 2100u /* > 2042-sector buffer */
#define REPS 3
#define SEED 20261122u

static const unsigned DIST[] = {1, 24, 100, 500, 1000, 2040, 3000, 5000};
#define ND (sizeof DIST / sizeof DIST[0])
#define NARM (2 + 2 * ND + 1)

enum kind { K_A, K_B, K_F, K_X };

typedef struct {
    enum kind k;
    unsigned d;
    int rep;
    uint32_t s, far;      /* span start, far target (unused for A) */
    double t1, tf, t2;    /* ms */
    int ok;
    char why[96];
} inst;

static double now_ms(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec * 1e3 + t.tv_nsec / 1e6;
}

static uint8_t buf[SPAN * 2352];

/* Raw READ CD. type_cdda: expected sector type CD-DA + user data (2352);
 * otherwise type ANY with sync+headers+user data+EDC/ECC (2352 for every
 * sector type). Returns ms, or -1 with why filled. */
static double read_cd(accudisc_device *dev, uint32_t lba, uint32_t n,
                      int type_cdda, char *why, size_t wlen)
{
    adsc_cmd c;
    memset(&c, 0, sizeof c);
    c.cdb[0] = 0xBE;
    c.cdb[1] = type_cdda ? (1u << 2) : 0;
    c.cdb[2] = (uint8_t)(lba >> 24); c.cdb[3] = (uint8_t)(lba >> 16);
    c.cdb[4] = (uint8_t)(lba >> 8);  c.cdb[5] = (uint8_t)lba;
    c.cdb[6] = (uint8_t)(n >> 16); c.cdb[7] = (uint8_t)(n >> 8);
    c.cdb[8] = (uint8_t)n;
    c.cdb[9] = type_cdda ? 0x10 : 0xF8;
    c.cdb_len = 12;
    c.dir = ADSC_XFER_IN;
    c.buf = buf;
    c.buf_len = n * 2352;
    c.timeout_ms = 20000;
    double a = now_ms();
    int rc = adsc_dev_exec(dev, &c);
    double b = now_ms();
    if (rc != ACCUDISC_OK || c.scsi_status != 0 || c.resid != 0) {
        int key = c.sense_len > 2 ? c.sense[2] & 0x0f : -1;
        int asc = c.sense_len > 13 ? c.sense[12] : -1;
        int ascq = c.sense_len > 13 ? c.sense[13] : -1;
        snprintf(why, wlen, "READ CD %u+%u rc=%d st=0x%02x resid=%u sense=%x/%02x/%02x",
                 lba, n, rc, c.scsi_status, c.resid, key, asc, ascq);
        return -1;
    }
    return b - a;
}

static const char *name(const inst *x, char *o, size_t n)
{
    switch (x->k) {
    case K_A: snprintf(o, n, "A"); break;
    case K_B: snprintf(o, n, "B%u", x->d); break;
    case K_F: snprintf(o, n, "F%u", x->d); break;
    case K_X: snprintf(o, n, "X"); break;
    }
    return o;
}

/* ---- mode "fua" ------------------------------------------------------- */

enum farm { M_A, M_U, M_N, M_R, M_D, M_D0, M_COUNT };
static const char *FNAME[] = {"A", "U", "N", "R", "D", "D0"};

typedef struct {
    enum farm k;
    int rep;
    uint32_t s;
    double t1, tf, t2;
    int ok;                /* both span reads GOOD */
    int st;                /* middle command: rc==OK ? scsi_status : -1 */
    int key, asc, ascq;    /* middle command sense, -1 if none */
    uint32_t resid;
    char why[96];
} finst;

/* The middle command. Returns ms; status and sense go into x. */
static double middle(accudisc_device *dev, finst *x)
{
    static uint8_t blk[2352];
    adsc_cmd c;
    memset(&c, 0, sizeof c);
    uint32_t lba = x->s + 1;
    int r10 = x->k == M_R;
    int fua = x->k == M_U || x->k == M_R || x->k == M_D;
    uint32_t len = (x->k == M_D || x->k == M_D0) ? 1 : 0;
    c.cdb[0] = r10 ? 0x28 : 0xA8;
    c.cdb[1] = fua ? 0x08 : 0;   /* FUA = byte 1 bit 3 in READ(10)/(12) */
    c.cdb[2] = (uint8_t)(lba >> 24); c.cdb[3] = (uint8_t)(lba >> 16);
    c.cdb[4] = (uint8_t)(lba >> 8);  c.cdb[5] = (uint8_t)lba;
    if (r10) {
        c.cdb[7] = (uint8_t)(len >> 8); c.cdb[8] = (uint8_t)len;
        c.cdb_len = 10;
    } else {
        c.cdb[6] = (uint8_t)(len >> 24); c.cdb[7] = (uint8_t)(len >> 16);
        c.cdb[8] = (uint8_t)(len >> 8);  c.cdb[9] = (uint8_t)len;
        c.cdb_len = 12;
    }
    c.dir = len ? ADSC_XFER_IN : ADSC_XFER_NONE;
    c.buf = len ? blk : NULL;
    c.buf_len = len ? sizeof blk : 0;
    c.timeout_ms = 20000;
    double a = now_ms();
    int rc = adsc_dev_exec(dev, &c);
    double b = now_ms();
    /* -1 = no SCSI status at all (transport/host error); detail kept in why */
    x->st = (rc != ACCUDISC_OK && !c.scsi_status) ? -1 : c.scsi_status;
    if (x->st < 0) adsc_io_detail(&c, x->why, sizeof x->why);
    x->key = c.sense_len > 2 ? c.sense[2] & 0x0f : -1;
    x->asc = c.sense_len > 13 ? c.sense[12] : -1;
    x->ascq = c.sense_len > 13 ? c.sense[13] : -1;
    x->resid = c.resid;
    return b - a;
}

static int run_fua(accudisc_device *dev, uint32_t audio_end, const char *out)
{
    const enum farm per[] = {M_A, M_A, M_U, M_N, M_R, M_D, M_D0};
    const unsigned NP = sizeof per / sizeof per[0];
    finst v[REPS * 7];
    unsigned nv = 0;
    srand(SEED);
    for (int r = 0; r < REPS; r++) {
        enum farm arm[7];
        memcpy(arm, per, sizeof arm);
        for (unsigned i = NP - 1; i > 0; i--) {
            unsigned j = (unsigned)rand() % (i + 1);
            enum farm t = arm[i]; arm[i] = arm[j]; arm[j] = t;
        }
        for (unsigned i = 0; i < NP; i++) {
            memset(&v[nv], 0, sizeof v[nv]);
            v[nv].k = arm[i]; v[nv].rep = r; nv++;
        }
    }
    uint32_t cur = 3000;
    for (unsigned i = 0; i < nv; i++) { v[i].s = cur; cur += SPAN + GUARD; }
    if (cur > audio_end) { fprintf(stderr, "layout overruns audio\n"); return 2; }
    printf("fua mode: %u instances, layout %u..%u\n", nv, 3000u, cur - GUARD);
    fflush(stdout);

    {
        char w[96];
        if (read_cd(dev, 500, SPAN, 1, w, sizeof w) < 0)
            printf("warm-up failed (continuing): %s\n", w);
    }
    for (unsigned i = 0; i < nv; i++) {
        finst *x = &v[i];
        x->ok = 1; x->st = 0; x->key = x->asc = x->ascq = -1;
        x->t1 = read_cd(dev, x->s, SPAN, 1, x->why, sizeof x->why);
        if (x->t1 < 0) { x->ok = 0; continue; }
        if (x->k != M_A) x->tf = middle(dev, x);
        x->t2 = read_cd(dev, x->s, SPAN, 1, x->why, sizeof x->why);
        if (x->t2 < 0) x->ok = 0;
    }

    FILE *f = fopen(out, "w");
    if (!f) { perror(out); return 2; }
    fprintf(f, "# cacheprobe fua seed=%u guard=%u span=%u\n", SEED, GUARD, SPAN);
    fprintf(f, "i\trep\tarm\tspan\tt1_ms\tmid_ms\tmid_status\tsense\tresid\tt2_ms\tok\twhy\n");
    for (unsigned i = 0; i < nv; i++) {
        finst *x = &v[i];
        fprintf(f, "%u\t%d\t%s\t%u\t%.3f\t%.3f\t%d\t%x/%02x/%02x\t%u\t%.3f\t%d\t%s\n",
                i, x->rep, FNAME[x->k], x->s, x->t1, x->tf, x->st,
                x->key & 0xff, x->asc & 0xff, x->ascq & 0xff, x->resid, x->t2,
                x->ok, (!x->ok || x->st < 0) ? x->why : "");
    }
    fclose(f);

    int bad = 0;
    for (unsigned i = 0; i < nv; i++)
        if (!v[i].ok) { printf("INVALID %s @%u: %s\n", FNAME[v[i].k], v[i].s, v[i].why); bad = 1; }
    if (bad) { printf("VERDICT: UNRESOLVED (a span READ CD failed)\n"); return 1; }

    double c = 0;
    int moot = 0;
    for (unsigned i = 0; i < nv; i++) {
        if (v[i].k != M_A) continue;
        printf("A     rep%d  t1=%8.3f  t2=%8.3f\n", v[i].rep, v[i].t1, v[i].t2);
        if (v[i].t2 > v[i].t1 / 4) moot = 1;
        if (v[i].t2 > c) c = v[i].t2;
    }
    if (moot) { printf("VERDICT: MOOT (a control re-read was not clearly cached)\n"); return 0; }
    printf("c = %.3f ms: FRESH >= %.3f, CACHED <= %.3f\n\n", c, 4 * c, 2 * c);

    for (int k = M_U; k < M_COUNT; k++) {
        int fr = 0, ca = 0, n = 0;
        printf("%-3s", FNAME[k]);
        for (unsigned i = 0; i < nv; i++) {
            finst *x = &v[i];
            if ((int)x->k != k) continue;
            const char *cl = x->t2 >= 4 * c ? "FRESH" : x->t2 <= 2 * c ? "CACHED" : "AMBIG";
            fr += cl[0] == 'F'; ca += cl[0] == 'C'; n++;
            if (x->key >= 0)
                printf("  t2=%7.2f mid=%6.2f st=%d %x/%02x/%02x %-6s", x->t2, x->tf,
                       x->st, x->key, x->asc, x->ascq, cl);
            else
                printf("  t2=%7.2f mid=%6.2f st=%d%s%s %-6s", x->t2, x->tf, x->st,
                       x->st < 0 ? " " : "", x->st < 0 ? x->why : "", cl);
        }
        printf("  -> %s\n", fr == n ? "FLUSHES" : ca == n ? "KEEPS" : "MIXED");
    }
    printf("\n(mid = the middle command's ms; st = its SCSI status, -1 = transport error)\n");
    return 0;
}

int main(int argc, char **argv)
{
    if (argc != 3 && !(argc == 4 && !strcmp(argv[3], "fua"))) {
        fprintf(stderr, "usage: %s DEVICE OUT.tsv [fua]\n", argv[0]);
        return 2;
    }
    int err = 0;
    accudisc_device *dev = accudisc_open(argv[1], 0, &err);
    if (!dev) { fprintf(stderr, "open: %s\n", accudisc_strerror(err)); return 2; }

    accudisc_toc toc;
    if (accudisc_read_toc_src(dev, &toc, NULL) != ACCUDISC_OK ||
        toc.session_count < 2) {
        fprintf(stderr, "need a mapped multi-session TOC (Under My Skin)\n");
        return 2;
    }
    uint32_t audio_end = toc.sessions[0].leadout_lba, data_lo = 0, data_end = 0;
    for (unsigned i = 0; i < toc.track_count; i++) {
        const accudisc_track *t = &toc.tracks[i];
        if ((t->adr_ctrl & 0x04) && !data_lo) {
            data_lo = t->lba;
            data_end = t->lba + t->sectors;
        }
    }
    if (!data_lo || data_end - data_lo < 4000) {
        fprintf(stderr, "no data track of >= 4000 sectors\n");
        return 2;
    }

    if (argc == 4) {
        int rc = run_fua(dev, audio_end, argv[2]);
        accudisc_close(dev);
        return rc;
    }

    /* Build and shuffle. */
    inst v[REPS * NARM];
    unsigned nv = 0;
    srand(SEED);
    for (int r = 0; r < REPS; r++) {
        inst arm[NARM];
        unsigned k = 0;
        memset(arm, 0, sizeof arm);
        arm[k++].k = K_A;
        arm[k++].k = K_A;
        for (unsigned i = 0; i < ND; i++) { arm[k].k = K_B; arm[k++].d = DIST[i]; }
        for (unsigned i = 0; i < ND; i++) { arm[k].k = K_F; arm[k++].d = DIST[i]; }
        arm[k++].k = K_X;
        for (unsigned i = NARM - 1; i > 0; i--) {
            unsigned j = (unsigned)rand() % (i + 1);
            inst t = arm[i]; arm[i] = arm[j]; arm[j] = t;
        }
        for (unsigned i = 0; i < NARM; i++) { arm[i].rep = r; v[nv++] = arm[i]; }
    }

    /* Lay out. */
    uint32_t cur = 3000, xt = data_end - 400;
    for (unsigned i = 0; i < nv; i++) {
        inst *x = &v[i];
        uint32_t hi;
        switch (x->k) {
        case K_B: x->s = cur + x->d; x->far = x->s - x->d; hi = x->s + SPAN; break;
        case K_F: x->s = cur; x->far = x->s + SPAN - 1 + x->d; hi = x->far + 1; break;
        case K_X: x->s = cur; x->far = xt; xt -= 1400; hi = x->s + SPAN; break;
        default:  x->s = cur; hi = x->s + SPAN; break;
        }
        cur = hi + GUARD;
    }
    if (cur - GUARD > audio_end || xt + 1400 < data_lo) {
        fprintf(stderr, "layout overruns: ends %u, audio ends %u; X low %u, data %u\n",
                cur - GUARD, audio_end, xt + 1400, data_lo);
        return 2;
    }
    printf("audio ends %u, data %u..%u; layout ends %u; %u instances\n",
           audio_end, data_lo, data_end, cur - GUARD, nv);
    fflush(stdout);

    /* Run. One discarded warm-up read first. */
    {
        char w[96];
        if (read_cd(dev, 500, SPAN, 1, w, sizeof w) < 0)
            printf("warm-up failed (continuing): %s\n", w);
    }
    for (unsigned i = 0; i < nv; i++) {
        inst *x = &v[i];
        x->ok = 1;
        x->tf = 0;
        x->t1 = read_cd(dev, x->s, SPAN, 1, x->why, sizeof x->why);
        if (x->t1 < 0) { x->ok = 0; continue; }
        if (x->k != K_A) {
            x->tf = read_cd(dev, x->far, 1, x->k != K_X, x->why, sizeof x->why);
            if (x->tf < 0) { x->ok = 0; continue; }
        }
        x->t2 = read_cd(dev, x->s, SPAN, 1, x->why, sizeof x->why);
        if (x->t2 < 0) x->ok = 0;
    }
    accudisc_close(dev);

    /* Record raw rows before judging anything. */
    FILE *f = fopen(argv[2], "w");
    if (!f) { perror(argv[2]); return 2; }
    fprintf(f, "# cacheprobe seed=%u guard=%u span=%u\n", SEED, GUARD, SPAN);
    fprintf(f, "i\trep\tarm\tspan\tfar\tt1_ms\tfar_ms\tt2_ms\tok\twhy\n");
    char nm[16];
    for (unsigned i = 0; i < nv; i++) {
        inst *x = &v[i];
        fprintf(f, "%u\t%d\t%s\t%u\t%u\t%.3f\t%.3f\t%.3f\t%d\t%s\n", i, x->rep,
                name(x, nm, sizeof nm), x->s, x->k == K_A ? 0 : x->far,
                x->t1, x->tf, x->t2, x->ok, x->ok ? "" : x->why);
    }
    fclose(f);

    /* Apply the rule. */
    int bad = 0, xbad = 0;
    for (unsigned i = 0; i < nv; i++)
        if (!v[i].ok) {
            printf("INVALID %s @%u: %s\n", name(&v[i], nm, sizeof nm), v[i].s, v[i].why);
            if (v[i].k == K_X) xbad = 1; else bad = 1;
        }
    if (bad) { printf("VERDICT: UNRESOLVED (invalid A/B/F instance)\n"); return 1; }
    if (xbad) printf("X arm VOID: the sense data above is the X result\n");

    double c = 0;
    int moot = 0;
    for (unsigned i = 0; i < nv; i++) {
        if (v[i].k != K_A) continue;
        printf("A     rep%d  t1=%8.3f  t2=%8.3f\n", v[i].rep, v[i].t1, v[i].t2);
        if (v[i].t2 > v[i].t1 / 4) moot = 1;
        if (v[i].t2 > c) c = v[i].t2;
    }
    if (moot) { printf("VERDICT: MOOT (a control re-read was not clearly cached)\n"); return 0; }
    printf("c = %.3f ms: FRESH >= %.3f, CACHED <= %.3f\n\n", c, 4 * c, 2 * c);

    /* One line per arm: B5000 first (the agreement check), then the rest. */
    for (int pass = 0; pass < 2; pass++)
    for (int kk = K_B; kk <= K_X; kk++) {
        if (kk == K_X && xbad) continue;
        unsigned nd = kk == K_X ? 1 : ND;
        for (unsigned di = 0; di < nd; di++) {
            unsigned d = kk == K_X ? 0 : DIST[di];
            int first = kk == K_B && d == 5000;
            if (pass == 0 ? !first : first) continue;
            int fr = 0, ca = 0, n = 0, ra = 0;
            char line[320];
            int off = 0;
            for (unsigned i = 0; i < nv; i++) {
                inst *x = &v[i];
                if ((int)x->k != kk || (kk != K_X && x->d != d)) continue;
                const char *cl = x->t2 >= 4 * c ? "FRESH" : x->t2 <= 2 * c ? "CACHED" : "AMBIG";
                int rh = kk == K_F && x->tf <= 2 * c;
                fr += cl[0] == 'F'; ca += cl[0] == 'C'; ra += rh; n++;
                off += snprintf(line + off, sizeof line - (size_t)off,
                                "  %7.2f/%6.2f %-6s%s", x->t2, x->tf, cl, rh ? "(rahit)" : "");
            }
            inst probe = { .k = (enum kind)kk, .d = d };
            printf("%-6s %s  -> %s%s\n", name(&probe, nm, sizeof nm), line,
                   fr == n ? "FLUSHES" : ca == n ? "KEEPS" : "MIXED",
                   ra ? " [far read served from buffer: not a media read]" : "");
        }
        if (pass == 0) { printf("\n"); break; }
    }
    printf("\n(columns: t2 ms / far-read ms, class)\n");
    return 0;
}
