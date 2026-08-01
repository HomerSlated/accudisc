/* SPDX-License-Identifier: MIT */
/* Known-answer tests for the GF(2^16) field and the RS decoder.
 *
 * WHY THIS FILE EXISTS, separately from test_rs16.c.
 *
 * test_rs16.c is a round trip. It builds every error syndrome by calling
 * adsc_rs16_syndromes() and hands the result to adsc_rs16_decode(). Those two
 * functions share a position convention, so a convention that is wrong in
 * BOTH of them cancels exactly and every assertion there still passes — the
 * planted position comes back, the word is restored, 133,000 checks report
 * green. It is a large suite that cannot see the one defect most likely to be
 * present, because its inputs cannot distinguish the case.
 *
 * So nothing here uses the module's own arithmetic to produce an expected
 * value. The field is reimplemented below by a different method — carry-less
 * multiply with polynomial reduction, no logarithm tables — and the syndromes
 * are computed straight from the definition with it. That makes this file
 * ground truth for the other one: agreement between two methods that share no
 * code is evidence; a function agreeing with itself is not.
 *
 * The reimplementation is deliberately the slow, obvious one. It is not a
 * second candidate for shipping and does not need to be fast; its only job is
 * to be independently wrong-able.
 */

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "repair/gf16.h"
#include "repair/rs16.h"

#define KAT_POLY  0x1100Bu /* x^16 + x^12 + x^3 + x + 1 */
#define KAT_ALPHA 2u
#define MAX_N     256u
#define OUT_MAX   (ADSC_RS16_MAX_NPAR + 1)

static unsigned failures;
static unsigned checks;

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

/* ---- the independent field ------------------------------------------------
 *
 * Russian-peasant multiply in GF(2)[x], reducing by KAT_POLY whenever the
 * product reaches degree 16. This shares no line and no idea with the
 * log/antilog tables in gf16.c beyond the field itself.
 */
static uint16_t kat_mul(uint32_t a, uint32_t b)
{
    uint32_t r = 0;

    while (b) {
        if (b & 1u)
            r ^= a;
        b >>= 1;
        a <<= 1;
        if (a & 0x10000u)
            a ^= KAT_POLY;
    }
    return (uint16_t)r;
}

/* alpha^e by square-and-multiply. */
static uint16_t kat_pow(unsigned e)
{
    uint16_t r = 1, base = KAT_ALPHA;

    e %= ADSC_GF16_ORDER;
    while (e) {
        if (e & 1u)
            r = kat_mul(r, base);
        base = kat_mul(base, base);
        e >>= 1;
    }
    return r;
}

/* S_r = XOR_j v[j] * alpha^(r * (n - 1 - j)), straight from the definition
 * in rs16.h. `n` is a parameter rather than being taken from the word so the
 * basis probe below can vary it. */
static void kat_syndromes(const uint16_t *v, unsigned nv, unsigned n,
                          unsigned npar, uint16_t *syn)
{
    for (unsigned r = 0; r < npar; r++) {
        uint16_t acc = 0;

        for (unsigned j = 0; j < nv; j++)
            acc ^= kat_mul(v[j], kat_pow(r * (n - 1 - j)));
        syn[r] = acc;
    }
}

static uint32_t rnd_state = 0x9E3779B9u;

static uint32_t rnd(void)
{
    rnd_state ^= rnd_state << 13;
    rnd_state ^= rnd_state >> 17;
    rnd_state ^= rnd_state << 5;
    return rnd_state;
}

static uint16_t rnd16(void)        { return (uint16_t)(rnd() >> 8); }
static uint16_t rnd16_nonzero(void)
{
    uint16_t v;

    do { v = rnd16(); } while (!v);
    return v;
}

/* ---- 1. the field, two ways ----------------------------------------------- */

/* The cycle length settles both constants at once and needs no outside
 * source: if KAT_POLY were not primitive, or KAT_ALPHA not a generator, the
 * orbit of alpha would close in fewer than 65535 steps. Computed here with
 * the table-free multiply, so it is a statement about the field rather than
 * about gf16.c's tables. */
static void test_field_is_primitive(void)
{
    uint16_t x = 1;
    unsigned k = 0;

    do {
        x = kat_mul(x, KAT_ALPHA);
        k++;
    } while (x != 1 && k <= ADSC_GF16_ORDER + 1);
    expect(k == ADSC_GF16_ORDER,
           "alpha's orbit closes after %u steps, expected %u — 0x%05X is not "
           "primitive or %u is not a generator", k, ADSC_GF16_ORDER,
           KAT_POLY, KAT_ALPHA);
}

static void test_mul_matches_independent(void)
{
    /* Structured pairs first: the ones where a reduction bug hides. */
    static const uint16_t edge[] = {
        0, 1, 2, 3, 0x8000, 0x8001, 0xFFFF, 0x100B, 0x0FFF, 0x1000,
    };

    for (unsigned i = 0; i < sizeof edge / sizeof *edge; i++)
        for (unsigned j = 0; j < sizeof edge / sizeof *edge; j++) {
            uint16_t want = kat_mul(edge[i], edge[j]);
            uint16_t got = adsc_gf16_mul(edge[i], edge[j]);

            expect(want == got, "mul(%04x,%04x): tables %04x, definition %04x",
                   edge[i], edge[j], got, want);
        }

    for (unsigned t = 0; t < 200000; t++) {
        uint16_t a = rnd16(), b = rnd16();

        if (kat_mul(a, b) != adsc_gf16_mul(a, b)) {
            expect(0, "mul(%04x,%04x): tables %04x, definition %04x", a, b,
                   adsc_gf16_mul(a, b), kat_mul(a, b));
            return; /* one report is enough; the rest would be noise */
        }
        checks++;
    }
}

static void test_pow_matches_independent(void)
{
    for (unsigned e = 0; e < 4096; e++) {
        uint16_t want = kat_pow(e), got = adsc_gf16_pow(e);

        expect(want == got, "alpha^%u: tables %04x, definition %04x", e, got,
               want);
    }
    /* Past one full cycle, where a missing reduction shows up. */
    for (unsigned e = ADSC_GF16_ORDER - 4; e < ADSC_GF16_ORDER + 8; e++)
        expect(kat_pow(e) == adsc_gf16_pow(e), "alpha^%u past the cycle", e);
}

/* ---- 2. frozen vectors ----------------------------------------------------
 *
 * Regression pins, NOT external ground truth: they were produced by the
 * independent implementation above, so they cannot vouch for the field. What
 * they catch is a later edit that changes behaviour on a fixed input — the
 * one thing a generative suite with a moving seed will not notice.
 */
static void test_frozen_vectors(void)
{
    static const struct { unsigned e; uint16_t want; } powers[] = {
        { 1, 0x0002 }, { 2, 0x0004 }, { 15, 0x8000 }, { 16, 0x100B },
        { 255, 0x8D92 }, { 256, 0x0B2F }, { 4095, 0x8596 },
        { 32768, 0xF0C6 }, { 65534, 0x8805 },
    };
    static const uint16_t want_syn[8] = {
        0x4000, 0x9079, 0x5565, 0x6582, 0xA08F, 0xEB66, 0x030C, 0x8905,
    };
    uint16_t v[12], syn[8];

    for (unsigned i = 0; i < sizeof powers / sizeof *powers; i++)
        expect(adsc_gf16_pow(powers[i].e) == powers[i].want,
               "frozen alpha^%u: got %04x want %04x", powers[i].e,
               adsc_gf16_pow(powers[i].e), powers[i].want);

    for (unsigned j = 0; j < 12; j++)
        v[j] = (uint16_t)(0x1234u ^ (j * 0x0741u));
    expect(adsc_rs16_syndromes(v, 12, 8, syn) == ACCUDISC_OK,
           "frozen: syndrome helper rejected a valid call");
    for (unsigned r = 0; r < 8; r++)
        expect(syn[r] == want_syn[r], "frozen syn[%u]: got %04x want %04x", r,
               syn[r], want_syn[r]);
}

/* ---- 3. the syndrome function against its own definition ------------------ */
static void test_syndromes_match_definition(unsigned n, unsigned npar)
{
    uint16_t v[MAX_N], mine[ADSC_RS16_MAX_NPAR], theirs[ADSC_RS16_MAX_NPAR];

    for (unsigned trial = 0; trial < 64; trial++) {
        for (unsigned j = 0; j < n; j++)
            v[j] = rnd16();
        kat_syndromes(v, n, n, npar, mine);
        expect(adsc_rs16_syndromes(v, n, npar, theirs) == ACCUDISC_OK,
               "syndromes n=%u npar=%u: rejected a valid call", n, npar);
        for (unsigned r = 0; r < npar; r++)
            if (mine[r] != theirs[r]) {
                expect(0, "syndromes n=%u npar=%u r=%u: module %04x, "
                          "definition %04x", n, npar, r, theirs[r], mine[r]);
                return;
            }
        checks++;
    }
}

/* ---- 4. THE known-answer decode -------------------------------------------
 *
 * The error syndromes handed to the decoder are built by kat_syndromes()
 * alone. adsc_rs16_syndromes() is not called, so a position convention that
 * differs from the one rs16.h documents has nothing to cancel against and
 * shows up as a wrong position — which is exactly what test_rs16.c cannot
 * see.
 */
static int kat_attempt(unsigned npar, unsigned n_data, unsigned nerr,
                       const unsigned *at, const uint16_t *mask,
                       const unsigned *erasures, unsigned nerasures,
                       int expect_ok, const char *label)
{
    uint16_t orig[MAX_N], recv[MAX_N];
    uint16_t syn_ref[ADSC_RS16_MAX_NPAR], syn_ours[ADSC_RS16_MAX_NPAR];
    uint16_t err_syn[ADSC_RS16_MAX_NPAR];
    unsigned positions[OUT_MAX];
    uint16_t values[OUT_MAX];
    int rc;

    for (unsigned j = 0; j < n_data; j++)
        orig[j] = rnd16();
    memcpy(recv, orig, n_data * sizeof(*orig));
    for (unsigned i = 0; i < nerr; i++)
        recv[at[i]] ^= mask[i];

    kat_syndromes(orig, n_data, n_data, npar, syn_ref);
    kat_syndromes(recv, n_data, n_data, npar, syn_ours);
    for (unsigned r = 0; r < npar; r++)
        err_syn[r] = (uint16_t)(syn_ours[r] ^ syn_ref[r]);

    rc = adsc_rs16_decode(npar, err_syn, n_data, nerasures ? erasures : NULL,
                          nerasures, positions, values, OUT_MAX);
    checks++;

    /* expect_ok: 1 must succeed, 0 must decline, -1 either is acceptable but
     * an answer must still be the RIGHT answer. The third state exists for
     * the over-capacity sweep, where a refusal and a correct low-weight
     * decode are both legitimate and only a wrong answer is a failure. */
    if (expect_ok == 0) {
        expect(rc < 0, "%s: returned %d where it had to decline", label, rc);
        return rc;
    }
    if (rc < 0) {
        if (expect_ok > 0)
            expect(0, "%s: declined (%d) a correctable column", label, rc);
        return rc;
    }
    if (expect_ok > 0)
        expect((unsigned)rc == nerr, "%s: reported %d errata, %u planted",
               label, rc, nerr);

    for (int i = 0; i < rc; i++) {
        expect(positions[i] < n_data, "%s: position %u out of range", label,
               positions[i]);
        if (i && positions[i] <= positions[i - 1])
            expect(0, "%s: positions not ascending", label);
        expect(values[i] != 0, "%s: zero magnitude at position %u should have "
                               "been dropped", label, positions[i]);
    }

    if (expect_ok < 0) {
        /* Beyond capacity a syndrome set can be genuinely consistent with a
         * lower-weight pattern that is NOT the one planted, and returning it
         * is correct behaviour, not a miscorrection. So the planted word is
         * the wrong yardstick here. What must hold is that the errata handed
         * back actually reproduce E — checked with this file's arithmetic,
         * not the module's re-verification, which would only confirm that the
         * decoder agrees with itself. */
        for (unsigned r = 0; r < npar; r++) {
            uint16_t acc = 0;

            for (int i = 0; i < rc; i++)
                acc ^= kat_mul(values[i],
                               kat_pow(r * (n_data - 1 - positions[i])));
            expect(acc == err_syn[r],
                   "%s: returned errata do not reproduce E[%u] (%04x vs %04x)",
                   label, r, acc, err_syn[r]);
        }
        return rc;
    }

    /* Position and magnitude checked against what was PLANTED, not against a
     * second decode. */
    for (int i = 0; i < rc; i++) {
        int found = -1;

        for (unsigned k = 0; k < nerr; k++)
            if (at[k] == positions[i])
                found = (int)k;
        if (found < 0) {
            expect(0, "%s: reported position %u, nothing was planted there",
                   label, positions[i]);
            continue;
        }
        expect(values[i] == mask[found],
               "%s: position %u magnitude %04x, planted %04x", label,
               positions[i], values[i], mask[found]);
    }
    for (int i = 0; i < rc; i++)
        recv[positions[i]] ^= values[i];
    expect(memcmp(recv, orig, n_data * sizeof(*orig)) == 0,
           "%s: corrections applied, word still differs", label);
    return rc;
}

static void test_kat_error_only(unsigned npar, unsigned n_data)
{
    unsigned at[ADSC_RS16_MAX_NPAR];
    uint16_t mask[ADSC_RS16_MAX_NPAR];
    char label[96];

    for (unsigned k = 1; k <= npar / 2; k++) {
        snprintf(label, sizeof label, "KAT error-only npar=%u n=%u t=%u", npar,
                 n_data, k);
        for (unsigned trial = 0; trial < 32; trial++) {
            /* Distinct positions, chosen without reference to the module. */
            for (unsigned i = 0; i < k; i++) {
                int dup;

                do {
                    at[i] = rnd() % n_data;
                    dup = 0;
                    for (unsigned j = 0; j < i; j++)
                        dup |= (at[j] == at[i]);
                } while (dup);
                mask[i] = rnd16_nonzero();
            }
            kat_attempt(npar, n_data, k, at, mask, NULL, 0, 1, label);
        }
    }
}

/* A single error at a KNOWN index, swept across the whole word. If the
 * decoder counts positions from the other end, or off by npar, this reports
 * the mirrored or shifted index and fails on every one of them. */
static void test_kat_position_basis(unsigned npar, unsigned n_data)
{
    char label[96];

    for (unsigned j = 0; j < n_data; j++) {
        unsigned at = j;
        uint16_t mask = rnd16_nonzero();

        snprintf(label, sizeof label, "KAT basis npar=%u n=%u j=%u", npar,
                 n_data, j);
        kat_attempt(npar, n_data, 1, &at, &mask, NULL, 0, 1, label);
    }
}

static void test_kat_erasures(unsigned npar, unsigned n_data)
{
    unsigned at[ADSC_RS16_MAX_NPAR], erasures[ADSC_RS16_MAX_NPAR];
    uint16_t mask[ADSC_RS16_MAX_NPAR];
    char label[96];

    /* e erasures and t errors, at the e + 2t = npar boundary. */
    for (unsigned e = 1; e <= npar && e <= 8; e++) {
        unsigned t = (npar - e) / 2;
        unsigned nerr = e + t;

        if (nerr > ADSC_RS16_MAX_NPAR)
            continue;
        snprintf(label, sizeof label, "KAT erasures npar=%u n=%u e=%u t=%u",
                 npar, n_data, e, t);
        for (unsigned trial = 0; trial < 16; trial++) {
            for (unsigned i = 0; i < nerr; i++) {
                int dup;

                do {
                    at[i] = rnd() % n_data;
                    dup = 0;
                    for (unsigned j = 0; j < i; j++)
                        dup |= (at[j] == at[i]);
                } while (dup);
                mask[i] = rnd16_nonzero();
            }
            for (unsigned i = 0; i < e; i++)
                erasures[i] = at[i]; /* the first e are the flagged ones */
            kat_attempt(npar, n_data, nerr, at, mask, erasures, e, 1, label);
        }
    }
}

static void test_kat_declines_past_capacity(unsigned npar, unsigned n_data)
{
    unsigned at[ADSC_RS16_MAX_NPAR + 2];
    uint16_t mask[ADSC_RS16_MAX_NPAR + 2];
    unsigned k = npar / 2 + 1;
    char label[96];

    snprintf(label, sizeof label, "KAT over-capacity npar=%u n=%u t=%u", npar,
             n_data, k);
    for (unsigned trial = 0; trial < 64; trial++) {
        for (unsigned i = 0; i < k; i++) {
            int dup;

            do {
                at[i] = rnd() % n_data;
                dup = 0;
                for (unsigned j = 0; j < i; j++)
                    dup |= (at[j] == at[i]);
            } while (dup);
            mask[i] = rnd16_nonzero();
        }
        /* Not "must decline": t+1 errors can occasionally land on a syndrome
         * set that is genuinely decodable to a lower weight. What must never
         * happen is a WRONG answer presented as right — so accept a refusal,
         * accept a correction that restores the word, reject anything else.
         * kat_attempt already fails on a correction that does not restore. */
        kat_attempt(npar, n_data, k, at, mask, NULL, 0,
                    /* expect_ok */ -1, label);
    }
}

/* ---- 5. the basis probe, which is Phase 4's instrument ---------------------
 *
 * The integration question is not whether our decoder is self-consistent — it
 * is — but which `n` to hand it: the data length, or the data length plus
 * npar. Those differ by an exponent offset, so
 *
 *     S_r(n + npar) = alpha^(r * npar) * S_r(n)
 *
 * and the r = 0 syndrome is IDENTICAL under both, because alpha^0 = 1. A
 * check that looked only at S_0 would see nothing.
 *
 * The consequence is the useful part: against a fixed set of published
 * syndromes, a CLEAN column gives an all-zero E under the right basis and a
 * generally non-zero E under the wrong one. So the basis can be settled from
 * a fixture with syndromes, an XOR and a zero test — no Chien search, no
 * Forney, no correction, no CRC. This test proves that discrimination has
 * power before any 383 MB image is involved.
 */
static void test_basis_probe_discriminates(unsigned npar, unsigned n_data)
{
    uint16_t ref[MAX_N];
    uint16_t published[ADSC_RS16_MAX_NPAR];
    uint16_t same[ADSC_RS16_MAX_NPAR], other[ADSC_RS16_MAX_NPAR];
    unsigned wide = n_data + npar;
    unsigned nonzero_terms = 0;

    for (unsigned j = 0; j < n_data; j++)
        ref[j] = rnd16();

    /* A hypothetical publisher that counts from the end of the CODEWORD. */
    kat_syndromes(ref, n_data, wide, npar, published);

    kat_syndromes(ref, n_data, wide, npar, same);   /* matching basis */
    kat_syndromes(ref, n_data, n_data, npar, other); /* mismatched basis */

    for (unsigned r = 0; r < npar; r++) {
        expect((published[r] ^ same[r]) == 0,
               "basis probe npar=%u: matching basis left E[%u] = %04x", npar,
               r, published[r] ^ same[r]);
        nonzero_terms += ((published[r] ^ other[r]) != 0);

        /* The scaling relation the whole probe rests on. */
        expect(published[r] == kat_mul(other[r], kat_pow(r * npar)),
               "basis probe npar=%u r=%u: S_r(n+npar) != alpha^(r*npar)*S_r(n)",
               npar, r);
    }
    expect(nonzero_terms > 0,
           "basis probe npar=%u n=%u: a MISMATCHED basis produced an all-zero "
           "E on a clean column — the probe cannot discriminate", npar,
           n_data);
    /* r = 0 always cancels; everything above it should not. */
    expect((published[0] ^ other[0]) == 0,
           "basis probe: E[0] differed, but alpha^0 = 1 makes it identical");
    expect(nonzero_terms >= npar - 1,
           "basis probe npar=%u: only %u of %u syndromes differed under a "
           "wrong basis", npar, nonzero_terms, npar);
}

int main(void)
{
    static const unsigned npars[] = { 2, 4, 8, 16 };

    adsc_gf16_init();

    test_field_is_primitive();
    test_mul_matches_independent();
    test_pow_matches_independent();
    test_frozen_vectors();

    for (unsigned i = 0; i < sizeof npars / sizeof *npars; i++) {
        unsigned npar = npars[i];

        test_syndromes_match_definition(37, npar);
        test_syndromes_match_definition(196, npar);
        test_kat_error_only(npar, 64);
        test_kat_position_basis(npar, 40);
        test_kat_erasures(npar, 64);
        test_kat_declines_past_capacity(npar, 64);
        test_basis_probe_discriminates(npar, 64);
    }

    printf("test_rs16_kat: %u checks, %u failures\n", checks, failures);
    return failures ? 1 : 0;
}
