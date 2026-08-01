/* SPDX-License-Identifier: MIT */
/* Self-test for the GF(2^16) field and the RS decoder, covering the nine
 * acceptance cases of the specification.
 *
 * Two rules shape what is asserted here. The field is checked by ALGEBRA,
 * not by stored vectors: a vector produced by the implementation under test
 * proves only that it is self-consistent. And every decode is checked by
 * recovering the ORIGINAL word, never by comparing against a second run of
 * the decoder.
 *
 * Standalone, so it can be built and run before the module is wired into
 * CMake:
 *
 *   gcc -std=c11 -Wall -Wextra -Iinclude -o /tmp/rs16_selftest \
 *       src/repair/gf16.c src/repair/rs16.c src/repair/selftest.c
 */

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "gf16.h"
#include "rs16.h"

#define MAX_N_DATA 1024
#define OUT_MAX    (ADSC_RS16_MAX_NPAR + 1)

static unsigned failures;
static unsigned checks;

static void fail(const char *fmt, ...)
{
    va_list ap;

    va_start(ap, fmt);
    fputs("FAIL: ", stderr);
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
    va_end(ap);
    failures++;
}

static void expect(int ok, const char *fmt, ...)
{
    va_list ap;

    checks++;
    if (ok)
        return;
    va_start(ap, fmt);
    fputs("FAIL: ", stderr);
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
    va_end(ap);
    failures++;
}

/* xorshift64*, seeded fixed so a failure is reproducible. */
static uint64_t rng = 0x2f6e2b1c4d7a9301ull;

static uint32_t rnd(void)
{
    rng ^= rng >> 12;
    rng ^= rng << 25;
    rng ^= rng >> 27;
    return (uint32_t)((rng * 0x2545f4914f6cdd1dull) >> 32);
}

static uint16_t rnd16(void)
{
    return (uint16_t)rnd();
}

static uint16_t rnd16_nonzero(void)
{
    uint16_t v;

    do {
        v = rnd16();
    } while (!v);
    return v;
}

static unsigned rnd_below(unsigned n)
{
    return rnd() % n;
}

/* `count` distinct values in [0, range), none of them in avoid[]. */
static void pick_distinct(unsigned *out, unsigned count, unsigned range,
                          const unsigned *avoid, unsigned navoid)
{
    unsigned n = 0;

    while (n < count) {
        unsigned v = rnd_below(range);
        unsigned dup = 0;

        for (unsigned i = 0; i < n; i++)
            dup |= out[i] == v;
        for (unsigned i = 0; i < navoid; i++)
            dup |= avoid[i] == v;
        if (!dup)
            out[n++] = v;
    }
}

/* ---- 7.1 field construction ----------------------------------------------
 * The antilog cycle length is a complete proof of both the field polynomial
 * and the generator. If 0x1100B were not primitive, or 2 not a generator,
 * the sequence would close on itself early and some non-zero element would
 * be missed (and another repeated). Nothing else needs to be trusted. */
static void test_field_cycle(void)
{
    static uint8_t seen[65536];
    unsigned repeats = 0, missing = 0, zeros = 0;

    for (unsigned i = 0; i < ADSC_GF16_ORDER; i++) {
        uint16_t v = adsc_gf16_pow(i);

        if (!v)
            zeros++;
        else if (seen[v])
            repeats++;
        else
            seen[v] = 1;
    }
    for (unsigned v = 1; v <= 0xffff; v++)
        if (!seen[v])
            missing++;

    expect(zeros == 0, "7.1: antilog produced 0 %u times", zeros);
    expect(repeats == 0, "7.1: antilog repeated an element %u times", repeats);
    expect(missing == 0, "7.1: %u non-zero elements never appear", missing);
    expect(adsc_gf16_pow(ADSC_GF16_ORDER) == 1, "7.1: alpha^65535 != 1");
    expect(adsc_gf16_pow(0) == 1, "7.1: alpha^0 != 1");
    /* Belt: the cycle cannot be shorter than the full order, since every
     * element appeared exactly once above. Spot-check the divisors of 65535
     * anyway — those are the only lengths a short cycle could have. */
    static const unsigned divisors[] = { 3, 5, 15, 17, 51, 85, 255,
                                         257, 771, 1285, 3855, 4369,
                                         13107, 21845 };
    for (unsigned i = 0; i < sizeof(divisors) / sizeof(*divisors); i++)
        expect(adsc_gf16_pow(divisors[i]) != 1,
               "7.1: alpha^%u == 1, cycle is short", divisors[i]);
}

/* ---- 7.2 GF arithmetic, as properties ------------------------------------ */
static void test_field_algebra(void)
{
    unsigned bad_identity = 0, bad_inverse = 0, bad_roundtrip = 0;
    unsigned bad_div = 0;

    for (unsigned a = 1; a <= 0xffff; a++) {
        uint16_t x = (uint16_t)a;

        if (adsc_gf16_mul(x, 1) != x || adsc_gf16_mul(1, x) != x)
            bad_identity++;
        if (adsc_gf16_mul(x, adsc_gf16_inv(x)) != 1)
            bad_inverse++;
        if (adsc_gf16_pow((unsigned)adsc_gf16_log(x)) != x)
            bad_roundtrip++;
        if (adsc_gf16_div(x, x) != 1)
            bad_div++;
    }
    expect(!bad_identity, "7.2: a*1 != a for %u elements", bad_identity);
    expect(!bad_inverse, "7.2: a*inv(a) != 1 for %u elements", bad_inverse);
    expect(!bad_roundtrip, "7.2: antilog(log(a)) != a for %u", bad_roundtrip);
    expect(!bad_div, "7.2: a/a != 1 for %u elements", bad_div);

    /* Zero, where a log/antilog table goes wrong silently. */
    expect(adsc_gf16_log(0) == ADSC_GF16_LOG_UNDEFINED,
           "7.2: log(0) returned a plausible index instead of a guard");
    for (unsigned i = 0; i < 4096; i++) {
        uint16_t a = rnd16();

        expect(adsc_gf16_mul(0, a) == 0, "7.2: 0*a != 0");
        expect(adsc_gf16_mul(a, 0) == 0, "7.2: a*0 != 0");
        expect(adsc_gf16_div(0, a ? a : 1) == 0, "7.2: 0/a != 0");
        expect(adsc_gf16_mul_pow(0, i) == 0, "7.2: 0*alpha^k != 0");
    }

    for (unsigned i = 0; i < 20000; i++) {
        uint16_t a = rnd16(), b = rnd16(), c = rnd16();
        uint16_t ab = adsc_gf16_mul(a, b);
        unsigned e = rnd() % (3 * ADSC_GF16_ORDER);

        expect(ab == adsc_gf16_mul(b, a), "7.2: multiplication does not commute");
        expect(adsc_gf16_mul(ab, c) == adsc_gf16_mul(a, adsc_gf16_mul(b, c)),
               "7.2: multiplication does not associate");
        expect(adsc_gf16_mul(a, (uint16_t)(b ^ c))
                   == (uint16_t)(ab ^ adsc_gf16_mul(a, c)),
               "7.2: multiplication does not distribute over XOR");
        if (b)
            expect(adsc_gf16_mul(adsc_gf16_div(a, b), b) == a,
                   "7.2: (a/b)*b != a");
        expect(adsc_gf16_mul_pow(a, e) == adsc_gf16_mul(a, adsc_gf16_pow(e)),
               "7.2: a*alpha^k disagrees with the general multiply");
    }
}

/* ---- decode harness -------------------------------------------------------
 * Plants `nerr` errors in a random word, flags `nflag_bad` of them and
 * `nflag_clean` undamaged positions as erasures, and hands the decoder only
 * the error syndromes E = S(ours) XOR S(published). Success must restore the
 * original word exactly.
 *
 * Returns the decoder's return value. */
static int attempt(unsigned npar, unsigned n_data, unsigned nerr,
                   unsigned nflag_bad, unsigned nflag_clean, unsigned out_cap,
                   int expect_ok, const char *label)
{
    uint16_t orig[MAX_N_DATA], recv[MAX_N_DATA];
    uint16_t syn_published[ADSC_RS16_MAX_NPAR];
    uint16_t syn_ours[ADSC_RS16_MAX_NPAR];
    uint16_t err_syn[ADSC_RS16_MAX_NPAR];
    unsigned bad[ADSC_RS16_MAX_NPAR + 1];
    unsigned clean[ADSC_RS16_MAX_NPAR + 1];
    unsigned erasures[2 * ADSC_RS16_MAX_NPAR + 2];
    unsigned positions[OUT_MAX];
    uint16_t values[OUT_MAX];
    unsigned nerasures = 0;
    int rc;

    for (unsigned j = 0; j < n_data; j++)
        orig[j] = rnd16();
    memcpy(recv, orig, n_data * sizeof(*orig));

    pick_distinct(bad, nerr, n_data, NULL, 0);
    for (unsigned i = 0; i < nerr; i++)
        recv[bad[i]] ^= rnd16_nonzero();
    pick_distinct(clean, nflag_clean, n_data, bad, nerr);

    for (unsigned i = 0; i < nflag_bad; i++)
        erasures[nerasures++] = bad[i];
    for (unsigned i = 0; i < nflag_clean; i++)
        erasures[nerasures++] = clean[i];

    if (adsc_rs16_syndromes(orig, n_data, npar, syn_published) != ACCUDISC_OK
        || adsc_rs16_syndromes(recv, n_data, npar, syn_ours) != ACCUDISC_OK) {
        fail("%s: syndrome helper rejected a valid call", label);
        return ACCUDISC_ERR_INVAL;
    }
    for (unsigned r = 0; r < npar; r++)
        err_syn[r] = (uint16_t)(syn_ours[r] ^ syn_published[r]);

    rc = adsc_rs16_decode(npar, err_syn, n_data, nerasures ? erasures : NULL,
                          nerasures, positions, values, out_cap);
    checks++;
    if (!expect_ok) {
        if (rc >= 0)
            fail("%s: decoder returned %d where it had to decline", label, rc);
        return rc;
    }
    if (rc < 0) {
        fail("%s: decoder declined (%d) a correctable column", label, rc);
        return rc;
    }
    if ((unsigned)rc != nerr) {
        fail("%s: decoder reported %d errata, %u were planted", label, rc,
             nerr);
        return rc;
    }
    for (int i = 0; i < rc; i++) {
        if (positions[i] >= n_data) {
            fail("%s: position %u out of range", label, positions[i]);
            return rc;
        }
        if (i && positions[i] <= positions[i - 1])
            fail("%s: positions are not ascending", label);
        recv[positions[i]] ^= values[i];
    }
    if (memcmp(recv, orig, n_data * sizeof(*orig)) != 0)
        fail("%s: correction applied, word still differs", label);
    return rc;
}

/* ---- 7.3 error-only round trips, and the single-error closed form -------- */
static void test_single_error_closed_form(unsigned npar, unsigned n_data)
{
    /* One error is the case that pins the Forney factor and the position
     * basis at once: the evaluator collapses to the error magnitude and
     * Lambda'(X^-1) collapses to X, so any stray power of alpha shows up
     * immediately and unambiguously. Run it before the random sweeps —
     * a failure here says which stage is wrong, a failure at t = 6 does
     * not. */
    uint16_t orig[MAX_N_DATA], recv[MAX_N_DATA];
    uint16_t a[ADSC_RS16_MAX_NPAR], b[ADSC_RS16_MAX_NPAR];
    uint16_t err_syn[ADSC_RS16_MAX_NPAR];
    unsigned positions[OUT_MAX];
    uint16_t values[OUT_MAX];

    for (unsigned trial = 0; trial < 256; trial++) {
        unsigned j = rnd_below(n_data);
        uint16_t mask = rnd16_nonzero();
        int rc;

        for (unsigned i = 0; i < n_data; i++)
            orig[i] = rnd16();
        memcpy(recv, orig, n_data * sizeof(*orig));
        recv[j] ^= mask;

        adsc_rs16_syndromes(orig, n_data, npar, a);
        adsc_rs16_syndromes(recv, n_data, npar, b);
        for (unsigned r = 0; r < npar; r++)
            err_syn[r] = (uint16_t)(a[r] ^ b[r]);

        rc = adsc_rs16_decode(npar, err_syn, n_data, NULL, 0, positions,
                              values, OUT_MAX);
        expect(rc == 1, "single error npar=%u: rc %d", npar, rc);
        if (rc != 1)
            continue;
        expect(positions[0] == j,
               "single error npar=%u: position %u, planted %u", npar,
               positions[0], j);
        expect(values[0] == mask,
               "single error npar=%u: magnitude %04x, planted %04x", npar,
               values[0], mask);
    }
}

static void test_error_only(unsigned npar, unsigned n_data)
{
    char label[96];

    for (unsigned k = 0; k <= npar / 2; k++) {
        snprintf(label, sizeof(label), "7.3 npar=%u k=%u", npar, k);
        for (unsigned trial = 0; trial < 64; trial++) {
            int rc = attempt(npar, n_data, k, 0, 0, npar, 1, label);

            if (k == 0)
                expect(rc == 0, "%s: clean column returned %d", label, rc);
        }
    }
}

/* ---- 7.4 refusal beyond capacity, no erasures ---------------------------- */
static void test_refusal(unsigned npar, unsigned n_data)
{
    char label[96];
    unsigned k = npar / 2 + 1;
    unsigned trials = 512;

    snprintf(label, sizeof(label), "7.4 npar=%u k=%u", npar, k);
    for (unsigned trial = 0; trial < trials; trial++)
        attempt(npar, n_data, k, 0, 0, npar, 0, label);
    printf("  7.4 npar=%2u: %u trials at k=%u, 0 corrections returned\n", npar,
           trials, k);
}

/* ---- 7.5 errors and erasures, e + 2t <= npar ----------------------------- */
static void test_errors_and_erasures(unsigned npar, unsigned n_data)
{
    char label[96];

    for (unsigned e = 0; e <= npar; e++) {
        for (unsigned t = 0; e + 2 * t <= npar; t++) {
            snprintf(label, sizeof(label), "7.5 npar=%u e=%u t=%u", npar, e, t);
            for (unsigned trial = 0; trial < 24; trial++)
                attempt(npar, n_data, e + t, e, 0, npar, 1, label);
        }
    }
}

/* ---- 7.6 over-capacity refusal with erasures ----------------------------- */
static void test_over_capacity_with_erasures(unsigned npar, unsigned n_data)
{
    char label[96];
    unsigned e = 2;
    unsigned t = npar / 2;
    unsigned trials = 256;

    /* e + 2t = npar + 2: one erasure pair past the limit. */
    snprintf(label, sizeof(label), "7.6 npar=%u e=%u t=%u", npar, e, t);
    for (unsigned trial = 0; trial < trials; trial++)
        attempt(npar, n_data, e + t, e, 0, npar, 0, label);
    printf("  7.6 npar=%2u: %u trials at e=%u t=%u (e+2t=%u), 0 corrections\n",
           npar, trials, e, t, e + 2 * t);
}

/* ---- 7.7 false-positive erasures ----------------------------------------
 * C2 pointers over-flag. A decoder that spends erasure budget on undamaged
 * words passes every other case here and fails in production. */
static void test_false_positive_erasures(unsigned npar, unsigned n_data)
{
    char label[96];

    /* Every flagged position clean, the column genuinely undamaged. */
    snprintf(label, sizeof(label), "7.7 npar=%u all-clean flags", npar);
    for (unsigned trial = 0; trial < 32; trial++) {
        int rc = attempt(npar, n_data, 0, 0, npar, npar, 1, label);

        expect(rc == 0, "%s: clean column with %u flags returned %d", label,
               npar, rc);
    }

    /* Real damage alongside false flags, still within capacity. */
    for (unsigned nclean = 1; nclean <= npar / 2; nclean++) {
        for (unsigned nbad = 0; nbad + nclean <= npar; nbad++) {
            unsigned e = nbad + nclean;
            unsigned t = (npar - e) / 2;

            snprintf(label, sizeof(label),
                     "7.7 npar=%u flagged-bad=%u flagged-clean=%u extra=%u",
                     npar, nbad, nclean, t);
            for (unsigned trial = 0; trial < 16; trial++)
                attempt(npar, n_data, nbad + t, nbad, nclean, npar, 1, label);
        }
    }
}

/* ---- random syndromes: the invariant behind every refusal ----------------
 * The nine cases all plant a known error pattern, and under them exactly one
 * of the decoder's three refusal guards ever fires — measured, not assumed.
 * Syndromes drawn at random are not the image of any low-weight pattern, so
 * they reach the guards the planted cases never do.
 *
 * The assertion is not "it declines": a random E may legitimately decode.
 * It is that anything returned must EXPLAIN E — recompute the syndromes of
 * the errata the decoder handed back, independently, and require them to
 * equal E. A decoder that returns a well-formed answer which does not
 * account for the syndromes it was given is the silent-corruption failure. */
static void test_random_syndromes(unsigned npar, unsigned n_data)
{
    uint16_t err_syn[ADSC_RS16_MAX_NPAR];
    uint16_t check[ADSC_RS16_MAX_NPAR];
    uint16_t word[MAX_N_DATA];
    unsigned erasures[ADSC_RS16_MAX_NPAR];
    unsigned positions[OUT_MAX];
    uint16_t values[OUT_MAX];
    unsigned decoded = 0, declined = 0;

    for (unsigned trial = 0; trial < 4000; trial++) {
        unsigned nerasures = rnd_below(npar + 1);
        int rc;

        for (unsigned r = 0; r < npar; r++)
            err_syn[r] = rnd16();
        pick_distinct(erasures, nerasures, n_data, NULL, 0);

        rc = adsc_rs16_decode(npar, err_syn, n_data,
                              nerasures ? erasures : NULL, nerasures,
                              positions, values, OUT_MAX);
        checks++;
        if (rc < 0) {
            declined++;
            continue;
        }
        decoded++;
        if ((unsigned)rc > npar) {
            fail("random npar=%u: returned %d errata, ceiling is %u", npar, rc,
                 npar);
            continue;
        }
        memset(word, 0, n_data * sizeof(*word));
        for (int i = 0; i < rc; i++) {
            if (positions[i] >= n_data) {
                fail("random npar=%u: position %u out of range", npar,
                     positions[i]);
                return;
            }
            word[positions[i]] ^= values[i];
        }
        adsc_rs16_syndromes(word, n_data, npar, check);
        if (memcmp(check, err_syn, npar * sizeof(*check)) != 0)
            fail("random npar=%u: %d errata returned that do not explain E",
                 npar, rc);
    }
    printf("  random npar=%2u: %u declined, %u decoded (all explained E)\n",
           npar, declined, decoded);
}

/* ---- 7.9 npar changing at runtime, one code path, no reinitialisation ---- */
static void test_npar_transition(void)
{
    /* An all-erasure column is the capacity maximum: e = npar, t = 0, and
     * it returns npar errata — the case that overruns any buffer sized from
     * the no-erasure bound of npar/2. Immediately followed by the same at
     * the other npar, in this process, with no init call between. */
    unsigned n_data = 300;

    attempt(8, n_data, 8, 8, 0, 8, 1, "7.9 all-erasure npar=8");
    attempt(16, n_data, 16, 16, 0, 16, 1, "7.9 all-erasure npar=16 (after 8)");
    attempt(8, n_data, 8, 8, 0, 8, 1, "7.9 all-erasure npar=8 (after 16)");

    /* The same transition through the syndrome helper, whose per-call
     * exponents also depend on npar. */
    for (unsigned i = 0; i < 8; i++) {
        unsigned npar = (i & 1) ? 16u : 8u;
        char label[96];

        snprintf(label, sizeof(label), "7.9 alternating npar=%u", npar);
        attempt(npar, n_data, npar / 2, 0, 0, npar, 1, label);
        attempt(npar, n_data, npar, npar, 0, npar, 1, label);
    }
}

/* ---- wide columns --------------------------------------------------------
 * Everything above runs on a 588-symbol column. The locator exponent is
 * r * (n_data - 1 - j), which at the field's limit reaches 31 * 65534 — well
 * past 16 bits. A column of ADSC_GF16_ORDER symbols is the widest the
 * position basis admits at all (one distinct locator per symbol), so it is
 * the boundary worth walking rather than a size we expect to meet. */
static void test_wide_column(unsigned n_data, const char *what)
{
    uint16_t *orig = malloc(n_data * sizeof(*orig));
    uint16_t *recv = malloc(n_data * sizeof(*recv));
    uint16_t published[ADSC_RS16_MAX_NPAR], ours[ADSC_RS16_MAX_NPAR];
    uint16_t err_syn[ADSC_RS16_MAX_NPAR];
    unsigned positions[OUT_MAX], erasures[ADSC_RS16_MAX_NPAR];
    uint16_t values[OUT_MAX];
    unsigned npar = 16, nerasures = 6, nerr = 11; /* e + 2t = 16 */
    int rc;

    if (!orig || !recv) {
        fail("%s: out of memory", what);
        free(orig);
        free(recv);
        return;
    }
    for (unsigned j = 0; j < n_data; j++)
        orig[j] = rnd16();
    memcpy(recv, orig, n_data * sizeof(*orig));

    /* Include both ends of the column: index 0 and index n_data-1 are the
     * two the basis maps to the extreme exponents. */
    erasures[0] = 0;
    erasures[1] = n_data - 1;
    erasures[2] = n_data / 2;
    for (unsigned i = 3; i < nerasures; i++)
        erasures[i] = 1 + i;
    for (unsigned i = 0; i < nerasures; i++)
        recv[erasures[i]] ^= rnd16_nonzero();
    for (unsigned i = nerasures; i < nerr; i++)
        recv[100 + 37 * i] ^= rnd16_nonzero();

    adsc_rs16_syndromes(orig, n_data, npar, published);
    adsc_rs16_syndromes(recv, n_data, npar, ours);
    for (unsigned r = 0; r < npar; r++)
        err_syn[r] = (uint16_t)(ours[r] ^ published[r]);

    rc = adsc_rs16_decode(npar, err_syn, n_data, erasures, nerasures,
                          positions, values, OUT_MAX);
    expect(rc == (int)nerr, "%s: rc %d, %u planted", what, rc, nerr);
    for (int i = 0; i < rc; i++)
        recv[positions[i]] ^= values[i];
    expect(rc == (int)nerr && !memcmp(recv, orig, n_data * sizeof(*orig)),
           "%s: word not restored", what);
    free(orig);
    free(recv);
}

/* ---- interface guards ---------------------------------------------------- */
static void test_interface_guards(void)
{
    uint16_t err_syn[ADSC_RS16_MAX_NPAR];
    unsigned positions[OUT_MAX];
    uint16_t values[OUT_MAX];
    unsigned erasures[ADSC_RS16_MAX_NPAR];
    int rc;

    for (unsigned r = 0; r < ADSC_RS16_MAX_NPAR; r++)
        err_syn[r] = (uint16_t)(r + 1);

    rc = adsc_rs16_decode(0, err_syn, 100, NULL, 0, positions, values, 32);
    expect(rc == ACCUDISC_ERR_INVAL, "guard: npar=0 accepted (%d)", rc);
    rc = adsc_rs16_decode(ADSC_RS16_MAX_NPAR + 1, err_syn, 100, NULL, 0,
                          positions, values, 32);
    expect(rc == ACCUDISC_ERR_INVAL, "guard: npar over ceiling accepted (%d)",
           rc);
    rc = adsc_rs16_decode(8, err_syn, 0, NULL, 0, positions, values, 32);
    expect(rc == ACCUDISC_ERR_INVAL, "guard: n_data=0 accepted (%d)", rc);
    rc = adsc_rs16_decode(8, NULL, 100, NULL, 0, positions, values, 32);
    expect(rc == ACCUDISC_ERR_INVAL, "guard: NULL syndromes accepted (%d)", rc);
    rc = adsc_rs16_decode(8, err_syn, 100, NULL, 3, positions, values, 32);
    expect(rc == ACCUDISC_ERR_INVAL, "guard: NULL erasure list accepted (%d)",
           rc);

    erasures[0] = 5;
    erasures[1] = 200;
    rc = adsc_rs16_decode(8, err_syn, 100, erasures, 2, positions, values, 32);
    expect(rc == ACCUDISC_ERR_INVAL, "guard: erasure past n_data accepted (%d)",
           rc);

    erasures[0] = 5;
    erasures[1] = 5;
    rc = adsc_rs16_decode(8, err_syn, 100, erasures, 2, positions, values, 32);
    expect(rc == ACCUDISC_ERR_INVAL, "guard: duplicate erasure accepted (%d)",
           rc);

    /* An all-erasure column needs npar slots. A caller who sized for the
     * no-erasure bound must be told, not overrun. */
    rc = attempt(8, 100, 8, 8, 0, 4, 0, "guard: out_cap npar/2");
    expect(rc == ACCUDISC_ERR_INVAL,
           "guard: short output buffer returned %d, not INVAL", rc);

    /* A clean column short-circuits before anything is written, so a zero
     * capacity is fine there. */
    memset(err_syn, 0, sizeof(err_syn));
    rc = adsc_rs16_decode(8, err_syn, 100, NULL, 0, positions, values, 0);
    expect(rc == 0, "guard: clean column returned %d", rc);
}

int main(void)
{
    static const unsigned npars[] = { 8, 16 };
    unsigned n_data = 588;

    printf("rs16 self-test\n");

    test_field_cycle();
    test_field_algebra();
    printf("  7.1/7.2 field: done\n");

    /* 7.8: everything below at both parity counts. */
    for (unsigned i = 0; i < sizeof(npars) / sizeof(*npars); i++) {
        unsigned npar = npars[i];

        test_single_error_closed_form(npar, n_data);
        test_error_only(npar, n_data);
        test_refusal(npar, n_data);
        test_errors_and_erasures(npar, n_data);
        test_over_capacity_with_erasures(npar, n_data);
        test_false_positive_erasures(npar, n_data);
        test_random_syndromes(npar, n_data);
        printf("  7.3/7.5/7.7 npar=%2u: done\n", npar);
    }

    test_npar_transition();
    test_wide_column(2940, "wide column n_data=2940");
    test_wide_column(ADSC_GF16_ORDER, "wide column n_data=65535 (field limit)");
    test_interface_guards();

    printf("%u checks, %u failures\n", checks, failures);
    return failures ? 1 : 0;
}
