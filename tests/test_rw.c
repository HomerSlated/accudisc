/* R-W subchannel decode: de-interleave and Reed-Solomon correction.
 *
 * There is no CD+G disc in the lab, so correctness is established by ROUND
 * TRIP: this file contains an independent encoder, and the decoder must invert
 * it exactly. The two are deliberately built by different methods — the
 * encoder solves a linear system by Gaussian elimination, the decoder works
 * from syndromes — so a mistake in one is unlikely to be mirrored in the
 * other. A shared-bug round trip that passes by agreeing on the same error is
 * the failure mode to design against, and this is the defence against it.
 *
 * What this DOES prove: the permutation, the 8-pack convolutional interleave,
 * the GF(2^6) arithmetic, both parity matrices, and the error-correction
 * capability the spec claims.
 *
 * What it does NOT prove: that a real drive hands back the 96 subcode bytes in
 * the order assumed. That rests on the Q path in subq.c, which reads bit 6 of
 * the same bytes and is hardware-verified. Worth restating rather than
 * forgetting: end-to-end verification needs a CD+G disc.
 */

#include <assert.h>
#include <string.h>

#include <accudisc/accudisc.h>

/* ---- an independent GF(2^6) ---------------------------------------------- */

static uint8_t E[128], L[64];

static void gf(void)
{
    unsigned x = 1;

    for (unsigned i = 0; i < 63; i++) {
        E[i] = (uint8_t)x;
        E[i + 63] = (uint8_t)x;
        L[x] = (uint8_t)i;
        x <<= 1;
        if (x & 0x40)
            x ^= 0x43;
    }
}

static uint8_t mul(uint8_t a, uint8_t b)
{
    return (!a || !b) ? 0 : E[L[a] + L[b]];
}

static uint8_t dvd(uint8_t a, uint8_t b)
{
    return !a ? 0 : E[L[a] + 63 - L[b]];
}

static uint8_t pw(unsigned e)
{
    return E[e % 63];
}

/* ---- encoder: solve H * V = 0 for the parity symbols ---------------------
 * H[r][j] = a^(r * (n-1-j)), parity in the last `np` positions. Gaussian
 * elimination over GF(2^6) — a different route to the same code than the
 * decoder's syndrome arithmetic. */
static void encode(uint8_t *v, unsigned n, unsigned np)
{
    uint8_t m[4][5]; /* [np][np+1] augmented */

    for (unsigned r = 0; r < np; r++) {
        uint8_t rhs = 0;

        for (unsigned j = 0; j < n - np; j++) /* known data terms */
            rhs ^= mul(v[j], pw(r * (n - 1 - j)));
        for (unsigned c = 0; c < np; c++) /* unknown parity coefficients */
            m[r][c] = pw(r * (n - 1 - (n - np + c)));
        m[r][np] = rhs; /* H*V = 0, and -x == x in characteristic 2 */
    }
    for (unsigned c = 0; c < np; c++) { /* forward elimination */
        unsigned p = c;

        while (p < np && !m[p][c])
            p++;
        assert(p < np); /* the parity submatrix is invertible by construction */
        if (p != c)
            for (unsigned k = 0; k <= np; k++) {
                uint8_t t = m[c][k];
                m[c][k] = m[p][k];
                m[p][k] = t;
            }
        for (unsigned r = 0; r < np; r++) {
            if (r == c || !m[r][c])
                continue;
            uint8_t f = dvd(m[r][c], m[c][c]);
            for (unsigned k = 0; k <= np; k++)
                m[r][k] ^= mul(f, m[c][k]);
        }
    }
    for (unsigned c = 0; c < np; c++)
        v[n - np + c] = dvd(m[c][np], m[c][c]);
}

/* A logical pack: symbols 0,1 and 4..19 are payload; 2,3 are Q parity;
 * 20..23 are P parity. Q must be computed FIRST — it lands inside the region
 * P covers, so computing P first would leave P checking stale symbols. */
static void make_pack(uint8_t *v, unsigned seed)
{
    memset(v, 0, 24);
    v[0] = (1 << 3) | 1; /* MODE 1 / ITEM 1 = TV-GRAPHICS, i.e. 0x09 */
    v[1] = (uint8_t)(seed % 64);
    for (unsigned j = 4; j < 20; j++)
        v[j] = (uint8_t)((seed * 7 + j * 13) % 64);
    encode(v, 4, 2);  /* Q parity into 2,3 */
    encode(v, 24, 4); /* P parity into 20..23 */
}

/* The interleaver, from the spec's Figure 5.3: the symbol at logical position
 * perm[i] goes to channel position i, delayed by (i mod 8) packs. */
static const uint8_t perm[24] = {
     0, 18,  5, 23,  4,  2,  6,  7,
     8,  9, 10, 11, 12, 13, 14, 15,
    16, 17,  1, 19, 20, 21, 22,  3
};

#define NPACK 64

static void interleave(uint8_t logical[NPACK][24], uint8_t chan[NPACK][24])
{
    memset(chan, 0, NPACK * 24);
    for (unsigned c = 0; c < NPACK; c++)
        for (unsigned i = 0; i < 24; i++) {
            unsigned d = i % 8;

            if (c >= d)
                chan[c][i] = logical[c - d][perm[i]];
        }
}

/* Channel packs -> sectors of 96 subcode bytes. R-W occupies the low 6 bits;
 * the P and Q channels in bits 7 and 6 are filled with noise to prove they are
 * masked off rather than accidentally read. */
static void to_sectors(uint8_t chan[NPACK][24], uint8_t sec[NPACK / 4][96])
{
    for (unsigned c = 0; c < NPACK; c++)
        for (unsigned i = 0; i < 24; i++)
            sec[c / 4][(c % 4) * 24 + i] =
                (uint8_t)(chan[c][i] | ((c * 24 + i) % 4) << 6);
}

/* Decode every sector, collecting emitted packs. */
static unsigned run(uint8_t sec[NPACK / 4][96], accudisc_rw_pack *got,
                    accudisc_rw_stats *st)
{
    accudisc_rw *rw = accudisc_rw_open();
    unsigned total = 0;

    assert(rw);
    for (unsigned s = 0; s < NPACK / 4; s++) {
        unsigned n = 0;

        assert(accudisc_rw_feed(rw, sec[s], got + total, 4, &n) == ACCUDISC_OK);
        total += n;
    }
    if (st)
        accudisc_rw_get_stats(rw, st);
    accudisc_rw_close(rw);
    return total;
}

int main(void)
{
    uint8_t logical[NPACK][24], chan[NPACK][24], sec[NPACK / 4][96];
    accudisc_rw_pack got[NPACK];
    accudisc_rw_stats st;

    gf();

    /* --- GF(2^6) sanity, before anything depends on it -------------------- */
    {
        assert(E[0] == 1);
        assert(E[1] == 2);         /* the primitive element a = 0b000010 */
        assert(mul(E[5], E[7]) == E[12]);
        assert(dvd(E[12], E[5]) == E[7]);
        for (unsigned i = 1; i < 64; i++)
            assert(mul((uint8_t)i, dvd(1, (uint8_t)i)) == 1);
    }

    /* --- clean round trip -------------------------------------------------
     * The decoder must reproduce every logical pack exactly, and report no
     * corrections, because there is nothing to correct. */
    {
        unsigned n;

        for (unsigned c = 0; c < NPACK; c++)
            make_pack(logical[c], c);
        interleave(logical, chan);
        to_sectors(chan, sec);
        n = run(sec, got, &st);

        /* The de-interleave spans 8 packs, so the first 7 emit nothing. */
        assert(n == NPACK - ACCUDISC_RW_PRIME_PACKS);
        assert(st.packs == n);
        assert(st.p_fixed == 0 && st.q_fixed == 0);
        assert(st.p_failed == 0 && st.q_failed == 0);

        for (unsigned k = 0; k < n; k++) {
            /* Emitted pack k IS logical pack k: the 7-pack priming latency costs
             * the packs at the TAIL, which need channel packs past the end. */
            assert(memcmp(got[k].symbol, logical[k],
                          24) == 0);
            assert(ACCUDISC_RW_MODE(&got[k]) == ACCUDISC_RW_MODE_GRAPHICS);
            assert(ACCUDISC_RW_ITEM(&got[k]) == ACCUDISC_RW_ITEM_TV_GRAPHICS);
            assert(got[k].symbol[0] == 0x09); /* the .cdg command byte */
        }
    }

    /* --- single symbol errors, well separated: correctable -----------------
     * Note what "one error" has to mean here. A logical pack draws from EIGHT
     * consecutive channel packs, so corrupting one symbol in every channel
     * pack would pile several errors into each logical pack and blow past
     * t=2 — which is a test of the interleave, not of single-error
     * correction. Corrupting channel position 7 (i mod 8 == 7) sends the error
     * to exactly one logical pack, c - 7, and spacing the hits keeps them from
     * colliding. */
    {
        unsigned n, hits = 0;

        for (unsigned c = 0; c < NPACK; c++)
            make_pack(logical[c], c + 100);
        interleave(logical, chan);
        for (unsigned c = 16; c < NPACK; c += 16) {
            chan[c][7] ^= 0x2a;
            hits++;
        }
        to_sectors(chan, sec);
        n = run(sec, got, &st);

        for (unsigned k = 0; k < n; k++)
            assert(memcmp(got[k].symbol, logical[k], 24) == 0);
        assert(st.p_failed == 0 && st.q_failed == 0);
        /* Exactly one repair per injected error — no more, no fewer. */
        assert(st.p_fixed == hits);
    }

    /* --- two symbol errors in one logical pack: still correctable ----------
     * The (24,20) P code has four parity symbols, so t=2. Because of the
     * interleave, two errors in ONE LOGICAL pack means touching two different
     * channel packs — which is exactly the property being tested. */
    {
        unsigned n;

        for (unsigned c = 0; c < NPACK; c++)
            make_pack(logical[c], c + 200);
        interleave(logical, chan);
        /* Logical pack 20, symbols at logical positions 8 and 12: channel
         * position i carries logical perm[i], delayed (i mod 8). */
        chan[20 + (8 % 8)][8] ^= 0x15;
        chan[20 + (12 % 8)][12] ^= 0x31;
        to_sectors(chan, sec);
        n = run(sec, got, &st);

        assert(st.p_failed == 0 && st.q_failed == 0);
        for (unsigned k = 0; k < n; k++)
            assert(memcmp(got[k].symbol, logical[k],
                          24) == 0);
        assert(st.p_fixed == 2);
    }

    /* --- an 8-symbol burst on the disc ------------------------------------
     * The spec claims 8-symbol burst correction with a single-symbol strategy.
     * A burst is consecutive symbols in the CHANNEL stream, which the
     * interleave scatters across logical packs — that scattering is the entire
     * point of interleaving, and this asserts it works. */
    {
        unsigned n;

        for (unsigned c = 0; c < NPACK; c++)
            make_pack(logical[c], c + 300);
        interleave(logical, chan);
        for (unsigned b = 0; b < 8; b++) {
            unsigned pos = 30 * 24 + 4 + b; /* 8 consecutive channel symbols */

            chan[pos / 24][pos % 24] ^= 0x3f;
        }
        to_sectors(chan, sec);
        n = run(sec, got, &st);

        assert(st.p_failed == 0 && st.q_failed == 0);
        for (unsigned k = 0; k < n; k++)
            assert(memcmp(got[k].symbol, logical[k],
                          24) == 0);
    }

    /* --- beyond capacity: must REPORT failure, never silently corrupt ------
     * This is the assertion that matters most. Handing back a plausible pack
     * that is not what was recorded would be worse than admitting defeat,
     * because a renderer cannot tell the difference. */
    {
        unsigned n, bad = 0;

        for (unsigned c = 0; c < NPACK; c++)
            make_pack(logical[c], c + 400);
        interleave(logical, chan);
        /* Five errors into one logical pack: well past t=2 for P. */
        for (unsigned j = 0; j < 5; j++) {
            unsigned lp = 25, i = j * 3 + 5;

            chan[lp + (i % 8)][i] ^= 0x3f;
        }
        to_sectors(chan, sec);
        n = run(sec, got, &st);

        for (unsigned k = 0; k < n; k++)
            if (memcmp(got[k].symbol, logical[k],
                       24) != 0)
                bad++;
        /* Every pack that came back wrong must have SAID it came back wrong. */
        for (unsigned k = 0; k < n; k++)
            if (memcmp(got[k].symbol, logical[k],
                       24) != 0)
                assert(got[k].p_failed || got[k].q_failed);
        assert(bad > 0);        /* the damage really did exceed capacity */
        assert(st.p_failed > 0);
    }

    /* --- an all-zero disc reads as MODE ZERO, not as garbage ---------------
     * Most CDs carry nothing in R-W. Zero symbols are a valid codeword (all
     * syndromes vanish), so this must come back clean and be reported as
     * "no data", never as a stream of corrections. */
    {
        unsigned n;

        memset(sec, 0, sizeof(sec));
        n = run(sec, got, &st);
        assert(n == NPACK - ACCUDISC_RW_PRIME_PACKS);
        assert(st.p_fixed == 0 && st.q_fixed == 0);
        assert(st.p_failed == 0 && st.q_failed == 0);
        assert(st.mode_zero == n);
        assert(st.mode_graphics == 0);
    }

    /* --- argument handling ------------------------------------------------ */
    {
        accudisc_rw *rw = accudisc_rw_open();
        accudisc_rw_pack p;
        uint8_t raw[96] = {0};
        unsigned n = 99;

        assert(rw);
        assert(accudisc_rw_feed(NULL, raw, &p, 1, &n) == ACCUDISC_ERR_INVAL);
        assert(accudisc_rw_feed(rw, NULL, &p, 1, &n) == ACCUDISC_ERR_INVAL);
        assert(accudisc_rw_feed(rw, raw, &p, 1, NULL) == ACCUDISC_ERR_INVAL);
        /* max = 0 with a NULL sink is legal: it primes without emitting. */
        assert(accudisc_rw_feed(rw, raw, NULL, 0, &n) == ACCUDISC_OK);
        assert(n == 0);
        accudisc_rw_close(rw);
        accudisc_rw_close(NULL); /* must tolerate NULL */
    }

    return 0;
}
