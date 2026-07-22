/* R-W subchannel decode: de-interleave and Reed-Solomon correction, producing
 * the 24-byte pack stream that IS the .cdg format.
 *
 * Normative source: Philips/Sony, "System Description Compact Disc Digital
 * Audio System — Subcode/Control and Display System, Channels R-W", November
 * 1991, §5.1. Licensed document; the constants below are facts drawn from it,
 * the document itself is not redistributed.
 *
 * Three stages, in order:
 *
 *   1. Extract  Each of the 96 subcode frames in a sector contributes one
 *               6-bit symbol. The drive hands back one byte per frame with the
 *               eight subchannels stacked P,Q,R,S,T,U,V,W from bit 7 down, so
 *               R-W is simply the low six bits. (Cross-checked against the Q
 *               path in subq.c, which takes bit 6 and is hardware-verified.)
 *
 *   2. De-interleave  §5.1.4-5.1.6. A convolutional interleave spanning 8
 *               packs, combined with a fixed permutation of symbol positions.
 *               Both are needed; neither alone yields a readable pack.
 *
 *   3. Correct  §5.1.3 and §5.1.7. Two Reed-Solomon codes over GF(2^6).
 */

#include <stdlib.h>
#include <string.h>

#include <accudisc/accudisc.h>

/* ---- GF(2^6), P(X) = X^6 + X + 1, primitive element a = 0b000010 ---------- */

#define GF_ORDER 63 /* 2^6 - 1: a^63 == 1 */

static uint8_t gf_exp[128]; /* doubled, so exponents up to 125 need no modulo */
static uint8_t gf_log[64];
static int gf_ready;

static void gf_init(void)
{
    unsigned x = 1;

    if (gf_ready)
        return;
    for (unsigned i = 0; i < GF_ORDER; i++) {
        gf_exp[i] = (uint8_t)x;
        gf_exp[i + GF_ORDER] = (uint8_t)x;
        gf_log[x] = (uint8_t)i;
        x <<= 1;
        if (x & 0x40)
            x ^= 0x43; /* X^6 + X + 1 */
    }
    gf_log[0] = 0; /* never consulted; guards a stray index */
    gf_ready = 1;
}

static uint8_t gf_mul(uint8_t a, uint8_t b)
{
    if (!a || !b)
        return 0;
    return gf_exp[gf_log[a] + gf_log[b]];
}


static uint8_t gf_div(uint8_t a, uint8_t b)
{
    if (!a)
        return 0;
    return gf_exp[gf_log[a] + GF_ORDER - gf_log[b]];
}

/* a^e for any non-negative e. */
static uint8_t gf_pow(unsigned e)
{
    return gf_exp[e % GF_ORDER];
}

/* ---- Reed-Solomon ---------------------------------------------------------
 * Both codes share a shape. The spec gives the parity matrices as
 *
 *     Hp[r][j] = a^(r * (23 - j))   r = 0..3, j = 0..23   (24,20)
 *     Hq[r][j] = a^(r * (3  - j))   r = 0..1, j = 0..3    (4,2)
 *
 * which is a conventional RS parity check with consecutive roots a^0, a^1, ...
 * once positions are counted from the END of the block: with k = n-1-j,
 * H[r][k] = a^(r*k). One routine therefore serves both, parameterised by block
 * length and parity count.
 *
 * Correction capability is floor(nsyn/2): two symbols for P, one for Q.
 */

/* Syndromes S_r = sum_j v[j] * a^(r * (n-1-j)). */
static void rs_syndromes(const uint8_t *v, unsigned n, unsigned nsyn,
                         uint8_t *s)
{
    for (unsigned r = 0; r < nsyn; r++) {
        uint8_t acc = 0;

        for (unsigned j = 0; j < n; j++) {
            if (!v[j])
                continue;
            acc ^= r ? gf_mul(v[j], gf_pow(r * (n - 1 - j))) : v[j];
        }
        s[r] = acc;
    }
}

/* Apply a correction and confirm every syndrome clears. A syndrome set can be
 * consistent with a low-weight error pattern that is not the one that actually
 * occurred; re-checking costs almost nothing and turns a silent miscorrection
 * into an honest failure. */
static int rs_verify(const uint8_t *v, unsigned n, unsigned nsyn)
{
    uint8_t s[4];

    rs_syndromes(v, n, nsyn, s);
    for (unsigned r = 0; r < nsyn; r++)
        if (s[r])
            return 0;
    return 1;
}

/* Returns the number of symbols corrected, or -1 if the block is in error and
 * could not be repaired. 0 means it was already clean. */
static int rs_correct(uint8_t *v, unsigned n, unsigned nsyn)
{
    uint8_t s[4];
    int any = 0;

    rs_syndromes(v, n, nsyn, s);
    for (unsigned r = 0; r < nsyn; r++)
        if (s[r])
            any = 1;
    if (!any)
        return 0;

    /* --- single error -----------------------------------------------------
     * S_0 = e, S_1 = e * a^k. A single error always has S_0 != 0, so an all-
     * zero S_0 with other syndromes set rules this case out immediately. */
    if (s[0]) {
        uint8_t e = s[0];
        unsigned k = gf_log[gf_div(s[1], s[0])];

        if (s[1] && k < n) {
            unsigned j = n - 1 - k;
            uint8_t save = v[j];

            v[j] ^= e;
            if (rs_verify(v, n, nsyn))
                return 1;
            v[j] = save;
        }
    }

    /* --- two errors (P code only; Q has no room for it) --------------------
     * Solve for the error locator L(x) = 1 + l1*x + l2*x^2 from
     *     l1*S_1 + l2*S_0 = S_2
     *     l1*S_2 + l2*S_1 = S_3
     * then find its roots by exhaustive search over the block's positions
     * (Chien search; n <= 24, so this is trivial), and recover the two
     * magnitudes from S_0 and S_1. */
    if (nsyn >= 4) {
        uint8_t det = gf_mul(s[1], s[1]) ^ gf_mul(s[0], s[2]);

        if (det) {
            uint8_t l1 = gf_div(gf_mul(s[2], s[1]) ^ gf_mul(s[0], s[3]), det);
            uint8_t l2 = gf_div(gf_mul(s[1], s[3]) ^ gf_mul(s[2], s[2]), det);
            unsigned pos[2];
            unsigned found = 0;

            for (unsigned k = 0; k < n && found < 2; k++) {
                /* x = a^-k is a root of 1 + l1*x + l2*x^2 */
                uint8_t x = gf_pow(GF_ORDER - (k % GF_ORDER));
                uint8_t sum = 1 ^ gf_mul(l1, x) ^ gf_mul(l2, gf_mul(x, x));

                if (!sum)
                    pos[found++] = k;
            }
            if (found == 2) {
                uint8_t x1 = gf_pow(pos[0]), x2 = gf_pow(pos[1]);
                uint8_t e1 = gf_div(s[1] ^ gf_mul(s[0], x2), x1 ^ x2);
                uint8_t e2 = s[0] ^ e1;
                unsigned j1 = n - 1 - pos[0], j2 = n - 1 - pos[1];
                uint8_t save1 = v[j1], save2 = v[j2];

                v[j1] ^= e1;
                v[j2] ^= e2;
                if (rs_verify(v, n, nsyn))
                    return 2;
                v[j1] = save1;
                v[j2] = save2;
            }
        }
    }
    return -1;
}

/* ---- de-interleave --------------------------------------------------------
 * §5.1.5-5.1.6. On write, the symbol destined for logical position perm[i] is
 * placed at channel position i and delayed by (i mod 8) packs. On read the
 * complementary delay is 7 - (i mod 8), so every branch totals 7 packs.
 *
 * The permutation is three transpositions — (1,18), (2,5), (3,23) — and the
 * identity everywhere else. Both figures were read directly and agree.
 *
 * Note the P-parity symbols land at channel positions 20..22 but logical
 * position 23 holds DATA (D-S_24n+3), not parity: the fourth parity symbol
 * rides at channel position 3. Getting this backwards would corrupt exactly
 * the symbols the code is supposed to protect. */
static const uint8_t rw_perm[ACCUDISC_RW_PACK_SYMBOLS] = {
     0, 18,  5, 23,  4,  2,  6,  7,
     8,  9, 10, 11, 12, 13, 14, 15,
    16, 17,  1, 19, 20, 21, 22,  3
};

struct accudisc_rw {
    /* Ring of the last 8 channel packs; age 0 is the most recent. */
    uint8_t hist[8][ACCUDISC_RW_PACK_SYMBOLS];
    unsigned head;  /* index of the most recently written slot */
    unsigned seen;  /* channel packs fed, saturating at 8 */
    accudisc_rw_stats st;
};

accudisc_rw *accudisc_rw_open(void)
{
    accudisc_rw *rw = calloc(1, sizeof(*rw));

    gf_init();
    return rw;
}

void accudisc_rw_close(accudisc_rw *rw)
{
    free(rw);
}

static void push_channel_pack(accudisc_rw *rw, const uint8_t *sym)
{
    rw->head = (rw->head + 1) % 8;
    memcpy(rw->hist[rw->head], sym, ACCUDISC_RW_PACK_SYMBOLS);
    if (rw->seen < 8)
        rw->seen++;
}

/* Assemble one logical pack from the ring, then correct it. */
static void emit_pack(accudisc_rw *rw, accudisc_rw_pack *out)
{
    int p, q;

    memset(out, 0, sizeof(*out));
    for (unsigned i = 0; i < ACCUDISC_RW_PACK_SYMBOLS; i++) {
        unsigned age = 7 - (i % 8);
        unsigned slot = (rw->head + 8 - age) % 8;

        out->symbol[rw_perm[i]] = rw->hist[slot][i];
    }

    /* P first: it spans the whole pack and can repair symbols that Q also
     * covers, so running it first gives Q a cleaner block to check. */
    p = rs_correct(out->symbol, ACCUDISC_RW_PACK_SYMBOLS, 4);
    if (p < 0)
        out->p_failed = 1;
    else
        out->p_fixed = (uint8_t)p;

    q = rs_correct(out->symbol, 4, 2);
    if (q < 0)
        out->q_failed = 1;
    else
        out->q_fixed = (uint8_t)q;

    rw->st.packs++;
    rw->st.p_fixed += out->p_fixed;
    rw->st.q_fixed += out->q_fixed;
    rw->st.p_failed += out->p_failed;
    rw->st.q_failed += out->q_failed;
    if (ACCUDISC_RW_MODE(out) == ACCUDISC_RW_MODE_ZERO)
        rw->st.mode_zero++;
    else if (ACCUDISC_RW_MODE(out) == ACCUDISC_RW_MODE_GRAPHICS)
        rw->st.mode_graphics++;
}

int accudisc_rw_feed(accudisc_rw *rw, const uint8_t raw[96],
                     accudisc_rw_pack *out, unsigned max, unsigned *emitted)
{
    unsigned n = 0;

    if (!rw || !raw || (!out && max) || !emitted)
        return ACCUDISC_ERR_INVAL;
    *emitted = 0;

    for (unsigned pk = 0; pk < ACCUDISC_RW_PACKS_PER_SEC; pk++) {
        uint8_t sym[ACCUDISC_RW_PACK_SYMBOLS];

        /* R-W is the low 6 bits of each subcode byte (P is bit 7, Q bit 6). */
        for (unsigned i = 0; i < ACCUDISC_RW_PACK_SYMBOLS; i++)
            sym[i] = raw[pk * ACCUDISC_RW_PACK_SYMBOLS + i] & 0x3f;

        push_channel_pack(rw, sym);
        if (rw->seen < 8) /* still priming: the de-interleave spans 8 packs */
            continue;
        if (n >= max)
            break;
        emit_pack(rw, &out[n++]);
    }
    *emitted = n;
    return ACCUDISC_OK;
}

void accudisc_rw_get_stats(const accudisc_rw *rw, accudisc_rw_stats *out)
{
    if (!rw || !out)
        return;
    *out = rw->st;
}
