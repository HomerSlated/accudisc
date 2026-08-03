/* SPDX-License-Identifier: MIT */
/* accudisc_ctdb_repair — interface guards and a synthetic end-to-end.
 *
 * Fixture-free on purpose. test_ctdb_ab covers the real CTDB parity, but its
 * fixtures are 1.6 GB in /var/tmp and exist on one machine, so it skips
 * everywhere else and the API would otherwise be untested there. Here the
 * parity is BUILT from a synthetic image with the library's own syndrome
 * function, which is self-consistent and therefore proves the plumbing — the
 * domains, the window, the refusal path, the aliasing contract — rather than
 * the wire format. The wire format is what the fixtures are for; the two tests
 * answer different questions and neither substitutes for the other.
 */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <accudisc/accudisc.h>

#include "repair/rs16.h"

/* Geometry chosen so the test can fail, which took two attempts.
 *
 * S must NOT be a whole number of frames, and `base` must NOT be a multiple of
 * S. With S = 1 frame every image offset is an exact number of rows, so a
 * bitmap shifted between the PCM and image domains lands on the SAME column at
 * a shifted row and rescues the column anyway — the domain test passed while
 * proving nothing. The real stride (5880 pairs = 10 frames) against a
 * first_frame of 3 makes the domain shift a genuine reordering.
 *
 * stridecount must also be >= NPAR, or an all-erasure column cannot be built:
 * at S = 1 frame and 8 image frames a column held 6 symbols, so 8 planted
 * errors could not all land inside it. */
#define WIRE_STRIDE 5880u
#define S           (WIRE_STRIDE * 2u) /* 11760 words = 10 frames */
#define NPAR        8u
#define FIRST_FRAME 3u                 /* NOT 0, and not a multiple of S/1176 */
#define FRAMES      120u               /* W/S = 12, so stridecount = 10 >= NPAR */
#define PCM_FRAMES  130u

static unsigned failures, checks;

static void expect(int ok, const char *fmt, ...)
{
    va_list ap;

    checks++;
    if (ok)
        return;
    failures++;
    fputs("FAIL: ", stderr);
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
}

static uint32_t rnd_state = 0x243F6A88u;

static uint16_t rnd16(void)
{
    rnd_state ^= rnd_state << 13;
    rnd_state ^= rnd_state >> 17;
    rnd_state ^= rnd_state << 5;
    return (uint16_t)(rnd_state >> 8);
}

static uint16_t *pcm;      /* PCM domain: PCM_FRAMES frames */
static uint16_t *parity;   /* S columns x NPAR planes, syndrome-major */
static uint64_t pcm_words, base, W;
static unsigned sc;

/* Build parity for the clean image, using the same grid the library uses:
 * column c is base + S + c + j*S for j = 0..sc-1. */
static void build_parity(void)
{
    uint16_t col[ADSC_RS16_MAX_NPAR * 4];

    for (unsigned c = 0; c < S; c++) {
        uint16_t syn[ADSC_RS16_MAX_NPAR];

        for (unsigned j = 0; j < sc; j++)
            col[j] = pcm[base + S + c + (uint64_t)j * S];
        adsc_rs16_syndromes(col, sc, NPAR, syn);
        for (unsigned r = 0; r < NPAR; r++)
            parity[(size_t)r * S + c] = syn[r];
    }
}

static void req_init(accudisc_ctdb_req *q)
{
    memset(q, 0, sizeof *q);
    q->size = sizeof *q;
    q->npar = NPAR;
    q->wire_stride = WIRE_STRIDE;
    q->image_first_frame = FIRST_FRAME;
    q->image_frames = FRAMES;
    q->offset_pairs = 0;
    q->pcm = (const uint8_t *)pcm;
    q->pcm_bytes = pcm_words * 2u;
    q->parity = (const uint8_t *)parity;
    q->parity_bytes = (uint64_t)S * NPAR * 2u;
}

/* ---- interface guards ----------------------------------------------------- */
static void test_guards(void)
{
    accudisc_ctdb_req q;
    accudisc_ctdb_report rep = ACCUDISC_CTDB_REPORT_INIT;
    uint8_t *out = malloc((size_t)pcm_words * 2u);

    req_init(&q);
    expect(accudisc_ctdb_repair(NULL, out, &rep) == ACCUDISC_ERR_INVAL,
           "NULL req accepted");
    expect(accudisc_ctdb_repair(&q, NULL, &rep) == ACCUDISC_ERR_INVAL,
           "NULL out accepted");

    /* The size guard exists so a caller built against a different header fails
     * loudly instead of having a shorter struct read as a longer one. */
    req_init(&q); q.size = 0;
    expect(accudisc_ctdb_repair(&q, out, &rep) == ACCUDISC_ERR_ABI,
           "size 0 not refused");
    req_init(&q); q.size = sizeof(q) - 4;
    expect(accudisc_ctdb_repair(&q, out, &rep) == ACCUDISC_ERR_ABI,
           "short size not refused");
    {
        accudisc_ctdb_report bad = { .size = 1 };

        req_init(&q);
        expect(accudisc_ctdb_repair(&q, out, &bad) == ACCUDISC_ERR_ABI,
               "bad report size not refused");
    }

    req_init(&q); q.npar = 0;
    expect(accudisc_ctdb_repair(&q, out, &rep) == ACCUDISC_ERR_INVAL,
           "npar 0 accepted");
    req_init(&q); q.npar = ADSC_RS16_MAX_NPAR + 1;
    expect(accudisc_ctdb_repair(&q, out, &rep) == ACCUDISC_ERR_INVAL,
           "npar over the ceiling accepted");

    /* The parity-size cross-check. This is the ONLY structural defence against
     * a caller pairing an entry's blob with another entry's npar: a
     * syndrome-major blob's first npar planes are a VALID narrower code, so
     * decoding a 16-parity blob at npar 8 or 4 succeeds silently and returns
     * identical corrections (measured against real CTDB data). Nothing
     * downstream can notice. Refusing on the byte count is what makes the
     * mismatch loud. */
    req_init(&q); q.npar = NPAR / 2;
    expect(accudisc_ctdb_repair(&q, out, &rep) == ACCUDISC_ERR_INVAL,
           "npar halved but parity_bytes unchanged: not refused");
    req_init(&q); q.wire_stride = WIRE_STRIDE / 2;
    expect(accudisc_ctdb_repair(&q, out, &rep) == ACCUDISC_ERR_INVAL,
           "wire_stride halved: not refused");

    /* Domain guards: the image window must lie inside the PCM. */
    req_init(&q); q.image_frames = PCM_FRAMES;
    expect(accudisc_ctdb_repair(&q, out, &rep) == ACCUDISC_ERR_INVAL,
           "image window overruns the PCM: not refused");
    req_init(&q); q.image_first_frame = PCM_FRAMES;
    expect(accudisc_ctdb_repair(&q, out, &rep) == ACCUDISC_ERR_INVAL,
           "image starts past the PCM: not refused");
    req_init(&q); q.image_frames = 0;
    expect(accudisc_ctdb_repair(&q, out, &rep) == ACCUDISC_ERR_INVAL,
           "zero-length image accepted");

    /* An offset large enough to walk the codeword region off either end. */
    req_init(&q); q.offset_pairs = -100000;
    expect(accudisc_ctdb_repair(&q, out, &rep) == ACCUDISC_ERR_INVAL,
           "offset running off the front accepted");
    req_init(&q); q.offset_pairs = 100000;
    expect(accudisc_ctdb_repair(&q, out, &rep) == ACCUDISC_ERR_INVAL,
           "offset running off the back accepted");

    /* A bitmap too short to cover the PCM would be read out of bounds. */
    {
        uint8_t small[4] = { 0 };

        req_init(&q);
        q.pcm_erasures = small;
        q.pcm_erasures_bytes = sizeof small;
        expect(accudisc_ctdb_repair(&q, out, &rep) == ACCUDISC_ERR_INVAL,
               "short erasure bitmap accepted");
    }

    /* Odd addresses. Everything here is accessed as uint16_t, so an odd
     * pointer is undefined behaviour on any target and a fault on some; it has
     * to be refused rather than documented. Each of the three is checked
     * separately, because one combined test would pass with two of the three
     * guards missing. */
    req_init(&q); q.pcm = (const uint8_t *)pcm + 1;
    expect(accudisc_ctdb_repair(&q, out, &rep) == ACCUDISC_ERR_INVAL,
           "misaligned pcm accepted");
    req_init(&q); q.parity = (const uint8_t *)parity + 1;
    expect(accudisc_ctdb_repair(&q, out, &rep) == ACCUDISC_ERR_INVAL,
           "misaligned parity accepted");
    req_init(&q);
    expect(accudisc_ctdb_repair(&q, out + 1, &rep) == ACCUDISC_ERR_INVAL,
           "misaligned out_pcm accepted");
    free(out);
}

/* ---- end to end ----------------------------------------------------------- */
static void test_clean_image_is_clean(void)
{
    accudisc_ctdb_req q;
    accudisc_ctdb_report rep = ACCUDISC_CTDB_REPORT_INIT;
    uint8_t *out = calloc((size_t)pcm_words, 2u);
    int rc;

    req_init(&q);
    rc = accudisc_ctdb_repair(&q, out, &rep);
    expect(rc == ACCUDISC_OK, "clean image: rc %d", rc);
    expect(rep.dirty_columns == 0, "clean image: %u dirty columns",
           rep.dirty_columns);
    expect(rep.corrections == 0, "clean image: %u corrections",
           rep.corrections);
    expect(rep.crc32_before == rep.crc32_after,
           "clean image: crc moved %08x -> %08x", rep.crc32_before,
           rep.crc32_after);
    expect(memcmp(out, pcm, (size_t)pcm_words * 2u) == 0,
           "clean image: output differs from input");
    free(out);
}

static void test_repairs_exactly(unsigned nerr, const char *what)
{
    accudisc_ctdb_req q;
    accudisc_ctdb_report rep = ACCUDISC_CTDB_REPORT_INIT;
    uint16_t *damaged = malloc((size_t)pcm_words * 2u);
    uint8_t *out = calloc((size_t)pcm_words, 2u);
    uint64_t hit[16];
    int rc;

    memcpy(damaged, pcm, (size_t)pcm_words * 2u);
    /* Damage distinct symbols of ONE column, so capacity is what is being
     * tested rather than the spread. */
    for (unsigned i = 0; i < nerr; i++) {
        hit[i] = base + S + 7u + (uint64_t)i * S;
        damaged[hit[i]] ^= (uint16_t)(0x1234u + i);
    }

    req_init(&q);
    q.pcm = (const uint8_t *)damaged;
    rc = accudisc_ctdb_repair(&q, out, &rep);

    if (nerr <= NPAR / 2) {
        expect(rc == ACCUDISC_OK, "%s: rc %d", what, rc);
        expect(rep.dirty_columns == 1, "%s: %u dirty columns", what,
               rep.dirty_columns);
        expect(rep.corrections == nerr, "%s: %u corrections, %u planted", what,
               rep.corrections, nerr);
        expect(memcmp(out, pcm, (size_t)pcm_words * 2u) == 0,
               "%s: repaired image is not the original", what);
        expect(rep.crc32_before != rep.crc32_after || nerr == 0,
               "%s: crc did not move despite %u corrections", what, nerr);
    } else {
        /* Beyond capacity the decoder must decline, and — the property that
         * matters — must not have written anything. */
        expect(rc == ACCUDISC_ERR_NOTFOUND, "%s: rc %d, expected NOTFOUND",
               what, rc);
        expect(rep.refused_columns > 0, "%s: refused nothing", what);
        expect(rep.corrections == 0, "%s: reported %u corrections on a refusal",
               what, rep.corrections);
        expect(rep.crc32_after == rep.crc32_before,
               "%s: crc32_after moved on a refusal", what);
    }
    free(damaged);
    free(out);
}

/* out_pcm may alias req->pcm. Documented, so it must be tested: an in-place
 * run has to give the same bytes as an out-of-place one, and an out-of-place
 * run must not touch the input. */
static void test_in_place_matches(void)
{
    accudisc_ctdb_req q;
    accudisc_ctdb_report r1 = ACCUDISC_CTDB_REPORT_INIT;
    accudisc_ctdb_report r2 = ACCUDISC_CTDB_REPORT_INIT;
    uint16_t *a = malloc((size_t)pcm_words * 2u);
    uint16_t *b = malloc((size_t)pcm_words * 2u);
    uint8_t *out = calloc((size_t)pcm_words, 2u);

    memcpy(a, pcm, (size_t)pcm_words * 2u);
    a[base + S + 3u] ^= 0xBEEFu;
    a[base + S + 3u + S] ^= 0x0FACu;
    memcpy(b, a, (size_t)pcm_words * 2u);

    req_init(&q);
    q.pcm = (const uint8_t *)a;
    expect(accudisc_ctdb_repair(&q, out, &r1) == ACCUDISC_OK,
           "aliasing: out-of-place run failed");
    expect(memcmp(a, b, (size_t)pcm_words * 2u) == 0,
           "aliasing: out-of-place run MODIFIED its input");

    q.pcm = (const uint8_t *)b;
    expect(accudisc_ctdb_repair(&q, (uint8_t *)b, &r2) == ACCUDISC_OK,
           "aliasing: in-place run failed");
    expect(memcmp(out, b, (size_t)pcm_words * 2u) == 0,
           "aliasing: in-place result differs from out-of-place");
    expect(r1.crc32_after == r2.crc32_after,
           "aliasing: crc differs, %08x vs %08x", r1.crc32_after,
           r2.crc32_after);
    free(a); free(b); free(out);
}

/* The erasure bitmap is indexed by PCM word, not image word. A caller that
 * pre-shifts it into the image domain would flag the wrong samples; this
 * pins which domain the library expects by flagging a position that is only
 * correct in one of them. */
static void test_erasure_bitmap_is_pcm_absolute(void)
{
    accudisc_ctdb_req q;
    accudisc_ctdb_report rep = ACCUDISC_CTDB_REPORT_INIT;
    uint16_t *damaged = malloc((size_t)pcm_words * 2u);
    uint8_t *out = calloc((size_t)pcm_words, 2u);
    uint8_t *bits = calloc((size_t)(pcm_words + 7u) / 8u, 1u);
    uint64_t w[NPAR];
    int rc;

    memcpy(damaged, pcm, (size_t)pcm_words * 2u);
    /* NPAR errors in one column: beyond the error-only capacity of NPAR/2,
     * correctable only if every position is supplied as an erasure. */
    for (unsigned i = 0; i < NPAR; i++) {
        w[i] = base + S + 11u + (uint64_t)i * S;
        damaged[w[i]] ^= (uint16_t)(0xA000u + i);
        bits[w[i] >> 3] |= (uint8_t)(1u << (w[i] & 7u));
    }

    req_init(&q);
    q.pcm = (const uint8_t *)damaged;
    rc = accudisc_ctdb_repair(&q, out, &rep);
    expect(rc == ACCUDISC_ERR_NOTFOUND,
           "no erasures: %u errors should exceed npar/2 = %u (rc %d)", NPAR,
           NPAR / 2, rc);

    q.pcm_erasures = bits;
    q.pcm_erasures_bytes = (pcm_words + 7u) / 8u;
    rc = accudisc_ctdb_repair(&q, out, &rep);
    /* NPAR erasures is exactly full capacity, so this is the DETERMINED case:
     * the audio below is restored byte-exactly and the library still declines
     * to say it verified that. Both halves matter — the weaker return code
     * must not be a refusal in disguise. */
    expect(rc == ACCUDISC_CTDB_UNVERIFIED,
           "with PCM-absolute erasures at full capacity: rc %d", rc);
    expect(rep.unverified_columns == 1, "unverified_columns %u, expected 1",
           rep.unverified_columns);
    expect(rep.erasure_columns == 1, "erasure_columns %u, expected 1",
           rep.erasure_columns);
    expect(rep.corrections == NPAR, "%u corrections, %u planted",
           rep.corrections, NPAR);
    expect(memcmp(out, pcm, (size_t)pcm_words * 2u) == 0,
           "erasure repair did not restore the original");

    /* Now the same bitmap shifted into the IMAGE domain, which is what a
     * caller doing the shift itself would pass. It must NOT rescue the
     * column — if it did, the two domains would be interchangeable and the
     * contract would be meaningless. */
    memset(bits, 0, (size_t)(pcm_words + 7u) / 8u);
    for (unsigned i = 0; i < NPAR; i++) {
        uint64_t shifted = w[i] - base;

        bits[shifted >> 3] |= (uint8_t)(1u << (shifted & 7u));
    }
    rc = accudisc_ctdb_repair(&q, out, &rep);
    expect(rc == ACCUDISC_ERR_NOTFOUND,
           "an image-domain bitmap rescued the column: the domains are not "
           "distinguished (rc %d)", rc);

    free(damaged); free(out); free(bits);
}

/* The case the suite could not see before, and the reason ACCUDISC_CTDB_UNVERIFIED
 * exists. An erasure list that is FULL but WRONG — npar flags, one of them
 * pointing at undamaged audio while a real error goes unflagged — is exactly
 * determined, so the decoder's re-verification is an identity and succeeds.
 * The audio it produces is wrong. Nothing inside this library can tell, which
 * is precisely why the return code has to.
 *
 * The assertion that matters is the LAST one: out differs from the original.
 * Without it this test would pass against a decoder that quietly got the right
 * answer, and would therefore be asserting nothing about the contract. */
static void test_full_but_wrong_erasures_are_unverified(void)
{
    accudisc_ctdb_req q;
    accudisc_ctdb_report rep = ACCUDISC_CTDB_REPORT_INIT;
    uint16_t *damaged = malloc((size_t)pcm_words * 2u);
    uint8_t *out = calloc((size_t)pcm_words, 2u);
    uint8_t *bits = calloc((size_t)(pcm_words + 7u) / 8u, 1u);
    uint64_t w[NPAR], bogus;
    int rc;

    memcpy(damaged, pcm, (size_t)pcm_words * 2u);
    for (unsigned i = 0; i < NPAR; i++) {
        w[i] = base + S + 11u + (uint64_t)i * S;
        damaged[w[i]] ^= (uint16_t)(0xA000u + i);
    }
    /* Flag the first NPAR-1 truthfully; the last error stays unflagged and a
     * clean row is flagged in its place. Still exactly NPAR erasures. */
    for (unsigned i = 0; i < NPAR - 1u; i++)
        bits[w[i] >> 3] |= (uint8_t)(1u << (w[i] & 7u));
    /* Row NPAR is clean and, with sc == 10, is still INSIDE the column. A
     * first attempt used row NPAR+2, which is past the end: the erasure scan
     * never reached it, nera came out NPAR-1, and the call refused for want of
     * capacity — a red test that had nothing to do with the contract. */
    bogus = base + S + 11u + (uint64_t)NPAR * S;
    bits[bogus >> 3] |= (uint8_t)(1u << (bogus & 7u));

    req_init(&q);
    q.pcm = (const uint8_t *)damaged;
    q.pcm_erasures = bits;
    q.pcm_erasures_bytes = (pcm_words + 7u) / 8u;

    rc = accudisc_ctdb_repair(&q, out, &rep);
    expect(rc == ACCUDISC_CTDB_UNVERIFIED,
           "a full-but-wrong erasure list must report UNVERIFIED, not OK "
           "(rc %d)", rc);
    expect(rep.unverified_columns == 1, "unverified_columns %u, expected 1",
           rep.unverified_columns);
    expect(memcmp(out, pcm, (size_t)pcm_words * 2u) != 0,
           "the repair happened to be correct, so this test proved nothing "
           "about the unverified contract");

    free(damaged); free(out); free(bits);
}

/* A positive offset against an image whose frame count is a multiple of ten,
 * i.e. W mod S == 0. The bound at ctdb.c:125 was over-strict by one row S,
 * which refused every positive offset for such an image. No A/B arm could
 * reach it: the fixtures' negative offsets bind on `first < 0` instead. */
static void test_positive_offset_when_w_divides_s(void)
{
    accudisc_ctdb_req q;
    accudisc_ctdb_report rep = ACCUDISC_CTDB_REPORT_INIT;
    uint8_t *out = calloc((size_t)pcm_words, 2u);
    int rc;

    expect((uint64_t)FRAMES * 1176u % S == 0u,
           "fixture geometry no longer has W mod S == 0, so this test cannot "
           "exercise the bound it was written for");

    /* Declare the PCM as exactly the image window. The buffer really is
     * PCM_FRAMES long, so nothing reads out of bounds — but without this the
     * fixture's ten frames of slack absorb the spurious row and the old bound
     * passes too, which would make this test unable to fail. */
    req_init(&q);
    q.pcm_bytes = (base + W) * 2u;
    q.offset_pairs = 1;
    rc = accudisc_ctdb_repair(&q, out, &rep);
    expect(rc != ACCUDISC_ERR_INVAL,
           "a positive offset was refused as invalid geometry (rc %d)", rc);

    free(out);
}

/* ---- the offset window, pinned on BOTH sides ------------------------------
 *
 * test_positive_offset_when_w_divides_s above proves the catastrophic class is
 * no longer categorically refused, and that is all it proves: at offset +1 it
 * cannot distinguish the real ceiling S + (W mod S) from (W mod S) + 1 or from
 * 2S + (W mod S). An arm that looks like it varies the parameter and does not.
 *
 * So pin the boundary itself — largest accepted and smallest refused, on each
 * side — across geometries that vary what the formula depends on:
 *
 *     max offset_pairs = (S + (W mod S)) / 2      (pcm declared as base + W)
 *     min offset_pairs = -(base + S) / 2
 *
 * The `+ S` is structural rather than slack: the codeword region begins one
 * stride into the image and ends one stride short of its end, so there is a
 * full stride of headroom above it whatever W mod S is. That is why the
 * multiple-of-ten class floors at S/2 instead of at zero.
 *
 * Parity and PCM are all-zero here, which is VALID: zero samples give zero
 * syndromes, so an accepted geometry returns ACCUDISC_OK rather than merely
 * "not INVAL", and nothing about the decoder is under test. */
static int offset_accepted(unsigned stride, unsigned first_frame,
                           unsigned frames, long off)
{
    accudisc_ctdb_req q;
    accudisc_ctdb_report rep = ACCUDISC_CTDB_REPORT_INIT;
    unsigned s = stride * 2u;
    uint64_t words = ((uint64_t)first_frame + frames) * 1176u;
    uint8_t *zpcm = calloc((size_t)words, 2u);
    uint8_t *zpar = calloc((size_t)s * NPAR, 2u);
    uint8_t *zout = calloc((size_t)words, 2u);
    int rc;

    memset(&q, 0, sizeof q);
    q.size = sizeof q;
    q.npar = NPAR;
    q.wire_stride = stride;
    q.image_first_frame = first_frame;
    q.image_frames = frames;
    q.offset_pairs = (int32_t)off;
    q.pcm = zpcm;
    q.pcm_bytes = words * 2u;          /* exactly the image window: no slack */
    q.parity = zpar;
    q.parity_bytes = (uint64_t)s * NPAR * 2u;

    rc = accudisc_ctdb_repair(&q, zout, &rep);
    free(zpcm); free(zpar); free(zout);
    return rc != ACCUDISC_ERR_INVAL;
}

static void test_offset_window_boundaries(void)
{
    static const struct { unsigned stride, ff, frames; } geom[] = {
        { 5880u,  0u,  42u }, /* W mod S = 2352 words                        */
        { 5880u,  0u,  40u }, /* W mod S = 0: the class that refused ALL      */
        { 5880u,  3u,  42u }, /* base != 0, so the negative arm moves         */
        { 5880u,  0u,  45u }, /* W mod S = S/2                                */
        {   17u,  0u, 300u }, /* stride co-prime to 1176: no alignment luck   */
        {   17u,  7u, 301u },
    };

    for (unsigned i = 0; i < sizeof geom / sizeof geom[0]; i++) {
        unsigned stride = geom[i].stride, ff = geom[i].ff, fr = geom[i].frames;
        unsigned s = stride * 2u;
        uint64_t w = (uint64_t)fr * 1176u, b = (uint64_t)ff * 1176u;
        long hi = (long)((s + w % s) / 2u);
        long lo = -(long)((b + s) / 2u);

        expect(offset_accepted(stride, ff, fr, hi),
               "geom %u (stride %u, ff %u, frames %u): +%ld is the ceiling and "
               "was refused", i, stride, ff, fr, hi);
        expect(!offset_accepted(stride, ff, fr, hi + 1),
               "geom %u: +%ld is one past the ceiling and was accepted",
               i, hi + 1);
        expect(offset_accepted(stride, ff, fr, lo),
               "geom %u: %ld is the floor and was refused", i, lo);
        expect(!offset_accepted(stride, ff, fr, lo - 1),
               "geom %u: %ld is one past the floor and was accepted",
               i, lo - 1);
    }
}

/* ---- which erasure_columns is this? ---------------------------------------
 *
 * Two definitions are live in this tree and they agree on every arm we hold:
 * the library counts dirty columns that CARRIED erasures (ctdb.c:208-209,
 * incremented before the decode and never withdrawn), while tests/ctdb_ab.c
 * deliberately reproduces the reference tool's "erasures were used AND helped"
 * so the A/B compares like with like. cdda2img could not separate them by
 * measurement (their §147.3) because both definitions predict every number
 * either project had measured.
 *
 * Separating them needs a column that carries erasures and decodes error-only
 * anyway. Build one: flag npar+1 positions so the errata decode is refused for
 * want of capacity, leave the single real error UNflagged so the error-only
 * retry (ctdb.c:237-239) succeeds on it.
 *
 *     "column HAD erasures"  -> 1        "erasures HELPED" -> 0 */
static void test_erasure_columns_counts_columns_that_had_erasures(void)
{
    accudisc_ctdb_req q;
    accudisc_ctdb_report rep = ACCUDISC_CTDB_REPORT_INIT;
    uint16_t *damaged = malloc((size_t)pcm_words * 2u);
    uint8_t *out = calloc((size_t)pcm_words, 2u);
    uint8_t *bits = calloc((size_t)(pcm_words + 7u) / 8u, 1u);
    const unsigned col = 11u, bad_row = 4u;
    uint64_t bad = base + S + col + (uint64_t)bad_row * S;
    unsigned flagged = 0;
    int rc;

    expect(sc >= NPAR + 2u,
           "geometry gives only %u rows; need npar+1 flags plus an unflagged "
           "damaged row for this test to mean anything", sc);

    memcpy(damaged, pcm, (size_t)pcm_words * 2u);
    damaged[bad] ^= 0xBEEFu;

    /* npar+1 erasures, none of them the damaged word. The scan stops at
     * npar+1 by design, so flagging more would change nothing. */
    for (unsigned p = 0; p < sc && flagged < NPAR + 1u; p++) {
        uint64_t w = base + S + col + (uint64_t)p * S;

        if (p == bad_row)
            continue;
        bits[w >> 3] |= (uint8_t)(1u << (w & 7u));
        flagged++;
    }

    req_init(&q);
    q.pcm = (const uint8_t *)damaged;
    q.pcm_erasures = bits;
    q.pcm_erasures_bytes = (pcm_words + 7u) / 8u;
    rc = accudisc_ctdb_repair(&q, out, &rep);

    expect(rc == ACCUDISC_OK,
           "over-capacity erasures should fall back to error-only and verify "
           "(rc %d)", rc);
    expect(rep.repaired_columns == 1, "repaired_columns %u, expected 1",
           rep.repaired_columns);
    expect(rep.corrections == 1, "corrections %u, expected 1 (the erasures are "
           "no-ops and must not be counted)", rep.corrections);
    /* The retry passes no erasures, so nothing it returns is at capacity. */
    expect(rep.unverified_columns == 0,
           "unverified_columns %u, expected 0 on the error-only retry",
           rep.unverified_columns);
    expect(rep.erasure_columns == 1,
           "erasure_columns %u: 1 means \"column HAD erasures\" (the documented "
           "contract), 0 means \"erasures helped\" (ctdb_ab's shim). The two "
           "have diverged", rep.erasure_columns);
    expect(memcmp(out, pcm, (size_t)pcm_words * 2u) == 0,
           "error-only retry did not restore the original");

    free(damaged); free(out); free(bits);
}

int main(void)
{
    pcm_words = (uint64_t)PCM_FRAMES * 1176u;
    base = (uint64_t)FIRST_FRAME * 1176u;
    W = (uint64_t)FRAMES * 1176u;
    sc = (unsigned)(W / S) - 2u;

    pcm = malloc((size_t)pcm_words * 2u);
    parity = malloc((size_t)S * NPAR * 2u);
    for (uint64_t i = 0; i < pcm_words; i++)
        pcm[i] = rnd16();
    build_parity();

    test_guards();
    test_clean_image_is_clean();
    test_repairs_exactly(1, "1 error");
    test_repairs_exactly(NPAR / 2, "npar/2 errors (at capacity)");
    test_repairs_exactly(NPAR / 2 + 1, "npar/2 + 1 errors (past capacity)");
    test_in_place_matches();
    test_erasure_bitmap_is_pcm_absolute();
    test_full_but_wrong_erasures_are_unverified();
    test_positive_offset_when_w_divides_s();
    test_offset_window_boundaries();
    test_erasure_columns_counts_columns_that_had_erasures();

    printf("test_ctdb_api: %u checks, %u failures\n", checks, failures);
    free(pcm);
    free(parity);
    return failures ? 1 : 0;
}
