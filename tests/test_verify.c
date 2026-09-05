/* Post-burn verification against a fake drive and a real source file.
 *
 * WHY THIS FILE EXISTS. accudisc_verify's alignment search is the part that
 * can be confidently wrong. It answers "at what displacement does the
 * read-back match the source?", and a displacement is a NUMBER — well-formed
 * whatever it is, with nothing downstream able to tell a right one from a
 * wrong one. That is this project's named failure mode, so every guard here
 * is made to FAIL before it is trusted: a silent anchor must refuse to align,
 * an ambiguous one must refuse, and a known displacement must come back
 * exactly, in the documented sign.
 *
 * The equation under test, stated once so the sign cannot drift:
 *     disc[j] == source[j + shift]
 * Positive shift = the read-back runs EARLY.
 *
 * It compiles the REAL src/write/verify.c against a stub drive, in the
 * test_burn_flow pattern. src/abi.c is linked for real, not stubbed: the
 * size-field rules are part of what is being asserted.
 */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "accudisc/accudisc.h"
#include "internal.h"

#define SPS 588u   /* samples per sector */

/* ---- the fake drive ------------------------------------------------------ */

static struct {
    uint32_t *disc;        /* read-back audio, one uint32 per sample */
    uint64_t  disc_samples;
    uint32_t  lba0;        /* disc[0] is this LBA's first sample */
    int       c2_verdict;
    int       counters_ok; /* 0 = no vendor counters at all */
    int       counter_read_fail; /* arm succeeds, READOUT fails — a separate
                                  * knob because arming is not evidence the
                                  * counters can be read, and one flag driving
                                  * both could never show the difference */
    int       census_ok;
    int       read_fail;   /* make every read fail */
    unsigned  reads;
} fake;

int accudisc_read_cdda(accudisc_device *dev, const accudisc_read_req *req,
                       accudisc_sink_fn sink, void *user,
                       accudisc_read_stats *stats)
{
    static uint8_t buf[SPS * 4 * 8];
    (void)dev;

    fake.reads++;
    if (fake.read_fail)
        return ACCUDISC_ERR_SENSE;
    if (stats) {
        stats->c2_bits = 7;
        stats->sectors_flagged = 3;
        stats->hard_errors = 0;
    }
    if (!sink)
        return ACCUDISC_OK;

    for (uint32_t done = 0; done < req->count; ) {
        uint32_t n = req->count - done > 8 ? 8 : req->count - done;
        accudisc_chunk ch;

        for (uint32_t k = 0; k < n; k++) {
            uint32_t lba = req->lba + done + k;
            int64_t j = (int64_t)(lba - fake.lba0) * SPS;

            for (uint32_t i = 0; i < SPS; i++) {
                uint32_t v = 0;
                if (j + i >= 0 && (uint64_t)(j + i) < fake.disc_samples)
                    v = fake.disc[j + i];
                memcpy(buf + (size_t)k * SPS * 4 + (size_t)i * 4, &v, 4);
            }
        }
        memset(&ch, 0, sizeof ch);
        ch.lba = req->lba + done;
        ch.nsec = n;
        ch.data = buf;
        ch.sector_len = SPS * 4;
        ch.audio_len = SPS * 4;
        if (sink(user, &ch) != 0)
            return ACCUDISC_ERR_CANCELLED;
        done += n;
    }
    return ACCUDISC_OK;
}

int accudisc_probe_features(accudisc_device *dev, accudisc_features *out)
{
    (void)dev;
    memset(out, 0, sizeof *out);
    out->c2_verdict = (uint8_t)fake.c2_verdict;
    return ACCUDISC_OK;
}

int accudisc_counter_scan_begin(accudisc_device *dev)
{
    (void)dev;
    return fake.counters_ok ? ACCUDISC_OK : ACCUDISC_ERR_UNSUPPORTED;
}

int accudisc_counter_scan_read(accudisc_device *dev, accudisc_counters *out)
{
    (void)dev;
    if (!fake.counters_ok || fake.counter_read_fail)
        return ACCUDISC_ERR_UNSUPPORTED;
    memset(out, 0, sizeof *out);
    out->have_detail = 1;
    return ACCUDISC_OK;
}

int accudisc_counter_scan_end(accudisc_device *dev) { (void)dev; return ACCUDISC_OK; }

int accudisc_counter_census(accudisc_device *dev, const accudisc_census_opts *o,
                            accudisc_census_fn fn, void *user,
                            accudisc_census_stats *stats)
{
    (void)dev; (void)o; (void)fn; (void)user;
    if (!fake.census_ok)
        return ACCUDISC_ERR_UNSUPPORTED;
    if (stats) {
        memset(stats, 0, sizeof *stats);
        stats->samples = 42;
        stats->c1 = 1234;
    }
    return ACCUDISC_OK;
}

/* ---- fixtures ------------------------------------------------------------ */

static char binpath[] = "/tmp/adsc_verify_XXXXXX";

/* Source of `n` samples with enough distinct values to be alignable, written
 * to the temp file. Returns the array so the disc can be built from it. */
static uint32_t *make_source(uint64_t n, int silent)
{
    uint32_t *src = malloc((size_t)n * 4);
    FILE *f;

    assert(src);
    for (uint64_t i = 0; i < n; i++) {
        if (silent == 1)
            src[i] = 0u;                        /* digital silence */
        else if (silent == 2)
            src[i] = (uint32_t)((i % 1000u) * 2654435761u + 12345u); /* periodic */
        else
            src[i] = (uint32_t)(i * 2654435761u + 12345u);
    }
    f = fopen(binpath, "wb");
    assert(f);
    assert(fwrite(src, 4, (size_t)n, f) == n);
    fclose(f);
    return src;
}

/* disc[j] = source[j + shift], zero where the source does not reach. */
static uint32_t *make_disc(const uint32_t *src, uint64_t n, int32_t shift)
{
    uint32_t *d = calloc((size_t)n, 4);

    assert(d);
    for (uint64_t j = 0; j < n; j++) {
        int64_t k = (int64_t)j + shift;
        if (k >= 0 && (uint64_t)k < n)
            d[j] = src[k];
    }
    return d;
}

static void reset_fake(uint32_t *disc, uint64_t n)
{
    memset(&fake, 0, sizeof fake);
    fake.disc = disc;
    fake.disc_samples = n;
    fake.lba0 = 0;
    fake.c2_verdict = ACCUDISC_C2_SUPPORTED;
    fake.counters_ok = 1;
    fake.census_ok = 1;
}

static int run(accudisc_verify_result *r, uint8_t want, uint8_t require,
               uint32_t count)
{
    accudisc_verify_opts o = ACCUDISC_VERIFY_OPTS_INIT;

    o.want_tier = want;
    o.require_tier = require;
    o.count = count;
    memset(r, 0, sizeof *r);
    r->size = sizeof *r;
    return accudisc_verify((accudisc_device *)&fake, binpath, &o, r, NULL, NULL);
}

int main(void)
{
    const uint32_t sectors = 400;
    const uint64_t n = (uint64_t)sectors * SPS;
    accudisc_verify_result r;
    uint32_t *src, *disc;
    int fd = mkstemp(binpath);

    assert(fd >= 0);
    close(fd);

    /* ---- 1. a perfect disc at zero displacement ------------------------- */
    src = make_source(n, 0);
    disc = make_disc(src, n, 0);
    reset_fake(disc, n);
    assert(run(&r, ACCUDISC_VERIFY_COUNTERS, ACCUDISC_VERIFY_COMPARE, sectors)
           == ACCUDISC_OK);
    assert(r.aligned == 1);
    assert(r.shift_samples == 0);
    assert(r.samples_differing == 0);
    assert(r.samples_compared == n);
    assert(r.tier == ACCUDISC_VERIFY_COUNTERS);
    assert(r.degraded == 0);
    assert(r.census.samples == 42);
    free(disc);

    /* ---- 2. a known POSITIVE displacement comes back exactly ------------ */
    /* This is the assertion the sign convention lives or dies on. If the
     * search reported -667 here the number would still be well-formed, and
     * only this test would ever say so. */
    disc = make_disc(src, n, 667);
    reset_fake(disc, n);
    assert(run(&r, ACCUDISC_VERIFY_COMPARE, ACCUDISC_VERIFY_COMPARE, sectors)
           == ACCUDISC_OK);
    assert(r.aligned == 1);
    assert(r.shift_samples == 667);
    assert(r.samples_differing == 0);
    /* The displacement costs exactly `shift` samples off the compared span:
     * the source does not reach past its own end. Not counted as compared,
     * because a sample never looked at must never sit in the denominator. */
    assert(r.samples_compared == n - 667);
    free(disc);

    /* ---- 3. a known NEGATIVE displacement, same exactness ---------------- */
    disc = make_disc(src, n, -1234);
    reset_fake(disc, n);
    assert(run(&r, ACCUDISC_VERIFY_COMPARE, ACCUDISC_VERIFY_COMPARE, sectors)
           == ACCUDISC_OK);
    assert(r.aligned == 1);
    assert(r.shift_samples == -1234);
    assert(r.samples_differing == 0);
    assert(r.samples_compared == n - 1234);
    free(disc);

    /* ---- 4. real differences are counted, and located -------------------- */
    disc = make_disc(src, n, 0);
    disc[SPS * 100 + 5] ^= 0xFFFFu;   /* sector 100 */
    disc[SPS * 250 + 9] ^= 0xFFFFu;   /* sector 250 */
    reset_fake(disc, n);
    assert(run(&r, ACCUDISC_VERIFY_COMPARE, ACCUDISC_VERIFY_COMPARE, sectors)
           == ACCUDISC_OK);
    assert(r.aligned == 1);
    assert(r.samples_differing == 2);
    assert(r.first_diff_lba == 100);
    free(disc);

    /* ---- 5. SILENCE MUST REFUSE TO ALIGN --------------------------------- */
    /* Digital silence matches at every displacement. A search that returned
     * one anyway would be the purest form of the failure this project is
     * named after: a confident number from evidence that distinguishes
     * nothing. It must come back unaligned, and it must NOT report a
     * comparison — an unaligned compare is not a failed compare. */
    free(src);
    src = make_source(n, 1);
    disc = make_disc(src, n, 0);
    reset_fake(disc, n);
    assert(run(&r, ACCUDISC_VERIFY_COMPARE, ACCUDISC_VERIFY_COMPARE, sectors)
           == ACCUDISC_OK);
    assert(r.aligned == 0);
    assert(r.samples_compared == 0);
    assert(r.samples_differing == 0);
    free(disc);
    free(src);

    /* ---- 6. tier degrades on evidence, not on a claim -------------------- */
    src = make_source(n, 0);
    disc = make_disc(src, n, 0);

    reset_fake(disc, n);
    fake.counters_ok = 0;                       /* no driver */
    assert(run(&r, ACCUDISC_VERIFY_COUNTERS, ACCUDISC_VERIFY_COMPARE, sectors)
           == ACCUDISC_OK);
    assert(r.tier == ACCUDISC_VERIFY_C2);
    assert(r.degraded == 1);
    assert(r.c2_bits == 7);                     /* tier 1 still measured */

    reset_fake(disc, n);
    fake.counters_ok = 0;
    fake.c2_verdict = ACCUDISC_C2_UNVERIFIED;   /* claimed, not proven */
    assert(run(&r, ACCUDISC_VERIFY_COUNTERS, ACCUDISC_VERIFY_COMPARE, sectors)
           == ACCUDISC_OK);
    assert(r.tier == ACCUDISC_VERIFY_COMPARE);
    assert(r.degraded == 1);
    /* Tier 0 leaves the C2 fields ZERO, and that zero means "never asked".
     * The result carries `tier` so a reader can tell it from "none seen". */
    assert(r.c2_bits == 0);
    assert(r.samples_differing == 0);           /* tier 0 still works alone */

    /* UNVERIFIED must not be promoted to SUPPORTED: a drive that claims C2
     * without the probe agreeing is exactly the case the verdict exists for. */
    reset_fake(disc, n);
    fake.c2_verdict = ACCUDISC_C2_UNSUPPORTED;
    fake.counters_ok = 0;
    assert(run(&r, ACCUDISC_VERIFY_C2, ACCUDISC_VERIFY_COMPARE, sectors)
           == ACCUDISC_OK);
    assert(r.tier == ACCUDISC_VERIFY_COMPARE);

    /* ---- 7. require_tier refuses rather than degrading silently ---------- */
    reset_fake(disc, n);
    fake.counters_ok = 0;
    assert(run(&r, ACCUDISC_VERIFY_COUNTERS, ACCUDISC_VERIFY_COUNTERS, sectors)
           == ACCUDISC_ERR_UNSUPPORTED);
    /* and require > want is a caller error, not a silent clamp */
    reset_fake(disc, n);
    assert(run(&r, ACCUDISC_VERIFY_COMPARE, ACCUDISC_VERIFY_COUNTERS, sectors)
           == ACCUDISC_ERR_INVAL);

    /* ---- 8. a census failure does not discard the compare ---------------- */
    reset_fake(disc, n);
    fake.census_ok = 0;
    assert(run(&r, ACCUDISC_VERIFY_COUNTERS, ACCUDISC_VERIFY_COMPARE, sectors)
           == ACCUDISC_OK);
    assert(r.tier == ACCUDISC_VERIFY_C2);
    assert(r.degraded == 1);
    assert(r.samples_compared == n);            /* tier 0 result survives */
    assert(r.census.samples == 0);

    /* ---- 8b. AMBIGUOUS audio must refuse — and this is the case that
     * isolates the margin from the entropy guard. A 1000-sample repeating
     * pattern has far more than ANCHOR_MIN_DISTINCT distinct values, so the
     * entropy early-out lets it through; every shift differing by a multiple
     * of the period then matches EXACTLY, and only the runner-up margin can
     * tell that there is no unique answer. Without it the search would return
     * whichever of nine equally-perfect shifts it saw first. */
    free(disc);
    free(src);
    src = make_source(n, 2);
    disc = make_disc(src, n, 0);
    reset_fake(disc, n);
    assert(run(&r, ACCUDISC_VERIFY_COMPARE, ACCUDISC_VERIFY_COMPARE, sectors)
           == ACCUDISC_OK);
    assert(r.aligned == 0);
    assert(r.samples_compared == 0);
    free(disc);
    free(src);

    /* ---- 8c. arming the counters is NOT evidence they can be read -------- */
    /* A drive that accepts the arm command and then refuses the readout must
     * degrade, not promise tier 2 and fill the census with zeros. */
    src = make_source(n, 0);
    disc = make_disc(src, n, 0);
    reset_fake(disc, n);
    fake.counter_read_fail = 1;
    assert(run(&r, ACCUDISC_VERIFY_COUNTERS, ACCUDISC_VERIFY_COMPARE, sectors)
           == ACCUDISC_OK);
    assert(r.tier == ACCUDISC_VERIFY_C2);
    assert(r.degraded == 1);
    assert(r.census.samples == 0);
    /* and require_tier 2 must refuse outright on the same drive */
    reset_fake(disc, n);
    fake.counter_read_fail = 1;
    assert(run(&r, ACCUDISC_VERIFY_COUNTERS, ACCUDISC_VERIFY_COUNTERS, sectors)
           == ACCUDISC_ERR_UNSUPPORTED);

    /* ---- 8d. A HEAVILY DAMAGED disc must still align and be counted ----- */
    /* The case that killed the first margin rule. A ratio test ("the runner-up
     * must be ten times worse") is unsatisfiable once the best shift is itself
     * bad, so a disc with 30% of its samples wrong would have come back
     * "cannot align" — silent about the very failure it was run to find. */
    free(disc);
    free(src);
    src = make_source(n, 0);
    disc = make_disc(src, n, 0);
    {
        uint64_t damaged = 0;
        for (uint64_t j = 0; j < n; j++)
            if (j % 10u < 3u) { disc[j] ^= 0xA5A5A5A5u; damaged++; }
        reset_fake(disc, n);
        assert(run(&r, ACCUDISC_VERIFY_COMPARE, ACCUDISC_VERIFY_COMPARE, sectors)
               == ACCUDISC_OK);
        assert(r.aligned == 1);
        assert(r.shift_samples == 0);
        assert(r.samples_differing == damaged);
        assert(r.samples_compared == n);
    }
    free(disc);

    /* ---- 8e. The WRONG audio must refuse, and this isolates the absolute
     * limit from the separation rule. 70% differing at the best shift clears
     * the separation test comfortably (the runner-up is ~100% differing), so
     * only the "are these even the same audio" limit can reject it. */
    disc = make_disc(src, n, 0);
    for (uint64_t j = 0; j < n; j++)
        if (j % 10u < 7u) disc[j] ^= 0x5A5A5A5Au;
    reset_fake(disc, n);
    assert(run(&r, ACCUDISC_VERIFY_COMPARE, ACCUDISC_VERIFY_COMPARE, sectors)
           == ACCUDISC_OK);
    assert(r.aligned == 0);
    assert(r.samples_compared == 0);
    free(disc);
    free(src);
    src = make_source(n, 0);
    disc = make_disc(src, n, 0);

    /* ---- 9. ABI guards ---------------------------------------------------- */
    {
        accudisc_verify_opts o = ACCUDISC_VERIFY_OPTS_INIT;
        accudisc_verify_result rr;

        reset_fake(disc, n);
        memset(&rr, 0, sizeof rr);
        rr.size = sizeof rr;
        o.size = 0;                             /* forgot the INIT macro */
        assert(accudisc_verify((accudisc_device *)&fake, binpath, &o, &rr,
                               NULL, NULL) == ACCUDISC_ERR_ABI);

        o.size = sizeof o;
        rr.size = 0;
        assert(accudisc_verify((accudisc_device *)&fake, binpath, &o, &rr,
                               NULL, NULL) == ACCUDISC_ERR_ABI);

        rr.size = sizeof rr + 64;               /* newer caller, older library */
        assert(accudisc_verify((accudisc_device *)&fake, binpath, &o, &rr,
                               NULL, NULL) == ACCUDISC_ERR_ABI);
    }

    /* ---- 10. a missing source file is an error, not a verdict ------------ */
    {
        accudisc_verify_opts o = ACCUDISC_VERIFY_OPTS_INIT;
        accudisc_verify_result rr;

        memset(&rr, 0, sizeof rr);
        rr.size = sizeof rr;
        assert(accudisc_verify((accudisc_device *)&fake, "/nonexistent/x.bin",
                               &o, &rr, NULL, NULL) == ACCUDISC_ERR_IO);
    }

    /* ---- 11. null arguments ---------------------------------------------- */
    {
        accudisc_verify_opts o = ACCUDISC_VERIFY_OPTS_INIT;
        accudisc_verify_result rr;

        memset(&rr, 0, sizeof rr);
        rr.size = sizeof rr;
        assert(accudisc_verify(NULL, binpath, &o, &rr, NULL, NULL)
               == ACCUDISC_ERR_INVAL);
        assert(accudisc_verify((accudisc_device *)&fake, NULL, &o, &rr,
                               NULL, NULL) == ACCUDISC_ERR_INVAL);
        assert(accudisc_verify((accudisc_device *)&fake, binpath, NULL, &rr,
                               NULL, NULL) == ACCUDISC_ERR_INVAL);
        assert(accudisc_verify((accudisc_device *)&fake, binpath, &o, NULL,
                               NULL, NULL) == ACCUDISC_ERR_INVAL);
    }

    free(disc);
    free(src);
    unlink(binpath);
    printf("test_verify: OK\n");
    return 0;
}
