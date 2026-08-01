/* SPDX-License-Identifier: MIT */
/* Reed-Solomon syndrome decoding over GF(2^16) — see rs16.h for the
 * contract, the buffer hazard and the position basis.
 *
 * The code has consecutive roots alpha^0 .. alpha^(npar-1) (b = 0), with
 * symbol positions counted from the end of the word: an erratum at data
 * index j has locator index k = n_data - 1 - j and locator X = alpha^k.
 *
 * Decoding, in the standard errors-and-erasures shape:
 *
 *   Gamma(x)  = product over erasures of (1 + X_i x)      erasure locator
 *   T(x)      = Gamma(x) S(x)  mod x^npar                 modified syndromes
 *   sigma(x)  = Berlekamp-Massey on T[e .. npar-1]        unknown errors
 *   Lambda(x) = sigma(x) Gamma(x)                         errata locator
 *   Omega(x)  = Lambda(x) S(x)  mod x^npar                errata evaluator
 *
 * Chien search gives the roots of Lambda; Forney gives the magnitudes. With
 * b = 0 the Forney magnitude carries an extra factor of X:
 *
 *   value_i = X_i * Omega(X_i^-1) / Lambda'(X_i^-1)
 *
 * which follows from value_i = X_i^(1-b) Omega / Lambda' — the single-error
 * case pins it, since there Omega is the constant `value` and
 * Lambda'(X^-1) = X, so the two factors of X must cancel.
 *
 * References are recorded in private/docs/rs16-implementation-notes.md. No
 * code or pseudocode was taken from any source; the algorithms are the
 * published ones, written here from their mathematical statement.
 */

#include <string.h>

#include "gf16.h"
#include "rs16.h"

/* Berlekamp-Massey's connection polynomial and its saved predecessor are
 * shifted by m each update, so an index can reach deg(B) + m. Both are
 * bounded by the syndrome count, hence twice the parity ceiling. Sizing to
 * the reachable index rather than the provable degree keeps the update loop
 * free of a truncating bounds test. */
#define BM_SCRATCH (2 * ADSC_RS16_MAX_NPAR + 2)

/* product = a * b, with terms in x^limit and above dropped. product must
 * hold `limit` coefficients and must not alias a or b. */
static void poly_mul(const uint16_t *a, unsigned da, const uint16_t *b,
                     unsigned db, uint16_t *product, unsigned limit)
{
    memset(product, 0, limit * sizeof(*product));
    for (unsigned i = 0; i <= da && i < limit; i++) {
        if (!a[i])
            continue;
        for (unsigned j = 0; j <= db && i + j < limit; j++)
            product[i + j] ^= adsc_gf16_mul(a[i], b[j]);
    }
}

/* Horner. Trailing zero coefficients are harmless, so `deg` may be an upper
 * bound on the true degree. */
static uint16_t poly_eval(const uint16_t *p, unsigned deg, uint16_t x)
{
    uint16_t acc = p[deg];

    for (unsigned i = deg; i-- > 0;)
        acc = (uint16_t)(adsc_gf16_mul(acc, x) ^ p[i]);
    return acc;
}

/* The formal derivative in characteristic 2: every even-degree term
 * differentiates away, so p'(x) = sum over odd d of p[d] x^(d-1). */
static uint16_t poly_deriv_eval(const uint16_t *p, unsigned deg, uint16_t x)
{
    uint16_t acc = 0;
    uint16_t xp = 1; /* x^(d-1), starting at d = 1 */

    for (unsigned d = 1; d <= deg; d++) {
        if (d & 1)
            acc ^= adsc_gf16_mul(p[d], xp);
        xp = adsc_gf16_mul(xp, x);
    }
    return acc;
}

/* Berlekamp-Massey: the shortest linear recurrence generating syn[0..nsyn-1].
 *
 * At step r the discrepancy is d = syn[r] + sum_{i=1..L} C_i syn[r-i] — what
 * the current recurrence gets wrong about the next syndrome. A non-zero
 * discrepancy is annulled by adding (d/b) x^m B(x), where B is the last
 * connection polynomial that itself failed, b its discrepancy and m the
 * number of steps since. The length grows only when 2L <= r.
 *
 * Writes the connection polynomial to locator[0..L] and returns L. This is
 * the classical form with a field division per non-zero discrepancy; the
 * inversionless variants exist to avoid that division in hardware, and here
 * a division is one table lookup, so they would buy nothing.
 */
static unsigned berlekamp_massey(const uint16_t *syn, unsigned nsyn,
                                 uint16_t *locator)
{
    uint16_t c[BM_SCRATCH] = { 1 };    /* connection polynomial */
    uint16_t prev[BM_SCRATCH] = { 1 }; /* last one that failed */
    uint16_t save[BM_SCRATCH];
    uint16_t b = 1; /* prev's discrepancy */
    unsigned L = 0; /* recurrence length */
    unsigned m = 1; /* steps since prev was taken */

    for (unsigned r = 0; r < nsyn; r++) {
        uint16_t d = syn[r];
        uint16_t coefficient;

        for (unsigned i = 1; i <= L; i++)
            d ^= adsc_gf16_mul(c[i], syn[r - i]);
        if (!d) {
            m++;
            continue;
        }
        coefficient = adsc_gf16_div(d, b);
        memcpy(save, c, sizeof(save));
        for (unsigned i = 0; i + m < BM_SCRATCH; i++)
            if (prev[i])
                c[i + m] ^= adsc_gf16_mul(coefficient, prev[i]);
        if (2 * L <= r) {
            L = r + 1 - L;
            memcpy(prev, save, sizeof(prev));
            b = d;
            m = 1;
        } else {
            m++;
        }
    }
    memcpy(locator, c, (L + 1) * sizeof(*locator));
    return L;
}

int adsc_rs16_syndromes(const uint16_t *v, unsigned n, unsigned npar,
                        uint16_t *syn)
{
    adsc_gf16_init();
    if (!v || !syn || n == 0 || n > ADSC_GF16_ORDER)
        return ACCUDISC_ERR_INVAL;
    if (npar == 0 || npar > ADSC_RS16_MAX_NPAR)
        return ACCUDISC_ERR_INVAL;

    for (unsigned r = 0; r < npar; r++) {
        uint16_t acc = 0;

        for (unsigned j = 0; j < n; j++) {
            if (!v[j])
                continue;
            acc ^= r ? adsc_gf16_mul_pow(v[j], r * (n - 1 - j)) : v[j];
        }
        syn[r] = acc;
    }
    return ACCUDISC_OK;
}

int adsc_rs16_decode(unsigned npar, const uint16_t *err_syn, unsigned n_data,
                     const unsigned *erasures, unsigned nerasures,
                     unsigned *positions, uint16_t *values, unsigned out_cap)
{
    uint16_t gamma[ADSC_RS16_MAX_NPAR + 1] = { 1 };
    uint16_t modified[ADSC_RS16_MAX_NPAR];
    uint16_t sigma[ADSC_RS16_MAX_NPAR + 1];
    uint16_t lambda[ADSC_RS16_MAX_NPAR + 1];
    uint16_t omega[ADSC_RS16_MAX_NPAR];
    uint16_t reg[ADSC_RS16_MAX_NPAR + 1];
    unsigned root[ADSC_RS16_MAX_NPAR + 1];
    uint16_t magnitude[ADSC_RS16_MAX_NPAR + 1];
    unsigned degree_gamma = 0, degree_lambda;
    unsigned length, nroots = 0, nout = 0, written = 0;
    int clean = 1;

    adsc_gf16_init();

    if (!err_syn || !positions || !values)
        return ACCUDISC_ERR_INVAL;
    if (npar == 0 || npar > ADSC_RS16_MAX_NPAR)
        return ACCUDISC_ERR_INVAL;
    if (n_data == 0 || n_data > ADSC_GF16_ORDER)
        return ACCUDISC_ERR_INVAL;
    if (nerasures && !erasures)
        return ACCUDISC_ERR_INVAL;
    for (unsigned i = 0; i < nerasures; i++)
        if (erasures[i] >= n_data)
            return ACCUDISC_ERR_INVAL;

    for (unsigned r = 0; r < npar; r++)
        if (err_syn[r])
            clean = 0;
    if (clean)
        return 0; /* nothing to do, whatever the erasure list claims */

    /* e + 2t <= npar with t >= 0 needs e <= npar. Bounding here also keeps
     * the duplicate scan below quadratic in npar rather than in the caller's
     * erasure count — which is why a duplicate inside an already
     * over-capacity list reports NOTFOUND rather than INVAL. */
    if (nerasures > npar)
        return ACCUDISC_ERR_NOTFOUND;
    for (unsigned i = 0; i < nerasures; i++)
        for (unsigned j = i + 1; j < nerasures; j++)
            if (erasures[i] == erasures[j])
                return ACCUDISC_ERR_INVAL;

    /* Gamma(x) = product of (1 + X_i x) over the erasure locators. */
    for (unsigned i = 0; i < nerasures; i++) {
        uint16_t x = adsc_gf16_pow(n_data - 1 - erasures[i]);

        for (unsigned d = degree_gamma + 1; d >= 1; d--)
            gamma[d] ^= adsc_gf16_mul(gamma[d - 1], x);
        degree_gamma++;
    }

    poly_mul(gamma, degree_gamma, err_syn, npar - 1, modified, npar);
    length = berlekamp_massey(modified + nerasures, npar - nerasures, sigma);

    /* Capacity: e + 2t <= npar. Nothing below can rescue a locator that
     * claims more errors than the parity can carry. */
    if (2 * length + nerasures > npar)
        return ACCUDISC_ERR_NOTFOUND;

    degree_lambda = length + degree_gamma;
    poly_mul(sigma, length, gamma, degree_gamma, lambda, degree_lambda + 1);
    /* A leading coefficient that cancelled means sigma and Gamma disagree
     * about how many symbols are in error, which no consistent errata
     * pattern does. */
    if (degree_lambda && !lambda[degree_lambda])
        return ACCUDISC_ERR_NOTFOUND;
    if (!degree_lambda)
        return ACCUDISC_ERR_NOTFOUND; /* non-zero syndromes, no locations */

    poly_mul(lambda, degree_lambda, err_syn, npar - 1, omega, npar);

    /* Chien search. reg[d] holds Lambda_d * alpha^(-k d), so the sum of the
     * registers is Lambda(alpha^-k) and each step costs one multiply per
     * coefficient. Roots come out in ascending k, i.e. descending data
     * index. */
    memcpy(reg, lambda, (degree_lambda + 1) * sizeof(*reg));
    for (unsigned k = 0; k < n_data; k++) {
        uint16_t sum = 0;

        for (unsigned d = 0; d <= degree_lambda; d++)
            sum ^= reg[d];
        if (!sum) {
            if (nroots == degree_lambda)
                return ACCUDISC_ERR_NOTFOUND; /* more roots than degree */
            root[nroots++] = k;
        }
        for (unsigned d = 1; d <= degree_lambda; d++)
            reg[d] = adsc_gf16_mul_pow(reg[d], ADSC_GF16_ORDER - d);
    }
    /* Roots outside the column are the ordinary signature of an
     * over-capacity pattern: the locator is well-formed but does not point
     * at symbols that exist. */
    if (nroots != degree_lambda)
        return ACCUDISC_ERR_NOTFOUND;

    /* Forney. */
    for (unsigned i = 0; i < nroots; i++) {
        uint16_t x = adsc_gf16_pow(root[i]);
        uint16_t xinv = adsc_gf16_pow(ADSC_GF16_ORDER - root[i]);
        uint16_t numerator = poly_eval(omega, npar - 1, xinv);
        uint16_t denominator = poly_deriv_eval(lambda, degree_lambda, xinv);

        if (!denominator)
            return ACCUDISC_ERR_NOTFOUND; /* repeated root */
        magnitude[i] = adsc_gf16_mul(x, adsc_gf16_div(numerator, denominator));
    }

    /* Re-verify: the errata we are about to hand back must reproduce E
     * exactly. A syndrome set can be consistent with a low-weight error
     * pattern that is not the one that actually occurred; this is what turns
     * a silent miscorrection into an honest failure, and it costs one pass
     * over npar. */
    for (unsigned r = 0; r < npar; r++) {
        uint16_t acc = 0;

        for (unsigned i = 0; i < nroots; i++)
            if (magnitude[i])
                acc ^= adsc_gf16_mul_pow(magnitude[i], r * root[i]);
        if (acc != err_syn[r])
            return ACCUDISC_ERR_NOTFOUND;
    }

    /* A zero magnitude is a position that was flagged but undamaged — the
     * ordinary result of C2 over-flagging. Drop it: XORing zero is a no-op
     * and the return value should count real corrections. */
    for (unsigned i = 0; i < nroots; i++)
        if (magnitude[i])
            nout++;
    if (nout > out_cap)
        return ACCUDISC_ERR_INVAL;

    /* Emit oldest-first: ascending data index is descending locator index. */
    for (unsigned i = nroots; i-- > 0;) {
        if (!magnitude[i])
            continue;
        positions[written] = n_data - 1 - root[i];
        values[written] = magnitude[i];
        written++;
    }
    return (int)nout;
}
