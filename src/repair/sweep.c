/* SPDX-License-Identifier: MIT */
/* Pass 1 of CTDB parity repair: the syndrome sweep, and the CRC fused into it.
 *
 * This is 80% of the wall time of a repair (6.3 s of 7.9 s on a 383 MB image,
 * measured), so it is the one place in src/repair/ where the shape of the code
 * is chosen for the machine rather than for the reader. Everything here is
 * bit-identical to the straightforward loop it replaces; correctness is pinned
 * by tests/ctdb_ab, which now routes through this file — see ctdb_internal.h
 * for why that mattered.
 *
 * The recurrence is Horner over the column, oldest symbol first:
 *
 *     S_r  <-  v ^ S_r * alpha^r
 *
 * With the plane-major accumulator (ctdb_internal.h), r is fixed for a whole
 * plane, so alpha^r is a constant and the inner loop is "multiply a run of
 * field elements by one constant, then XOR in a run of samples".
 */

#include <stdlib.h>
#include <string.h>

#include "gf16.h"
#include "rs16.h"
#include "ctdb_internal.h"

/* CRC-32, reflected, polynomial 0xEDB88320, init and final xor 0xFFFFFFFF.
 * Measured against the reference tool's published crc_before/crc_after; it is
 * a change detector, not a gate (CTDB publishes per-track CRCs, which are a
 * different quantity over different bytes). */
static uint32_t crc_tab[8][256];
static int crc_ready;

static void crc_init(void)
{
    if (crc_ready)
        return;
    for (unsigned i = 0; i < 256; i++) {
        uint32_t c = i;

        for (unsigned k = 0; k < 8; k++)
            c = (c & 1u) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
        crc_tab[0][i] = c;
    }
    /* Slicing-by-8: table[s][i] is table[0] applied s+1 times, which lets one
     * step consume eight bytes. */
    for (unsigned i = 0; i < 256; i++)
        for (unsigned s = 1; s < 8; s++)
            crc_tab[s][i] = (crc_tab[s - 1][i] >> 8)
                            ^ crc_tab[0][crc_tab[s - 1][i] & 0xFFu];
    crc_ready = 1; /* idempotent and value-identical if raced; see gf16_init */
}

/* Little-endian byte order is asserted rather than assumed: the eight-byte
 * step below reads the stream as two uint32_t, which only matches the
 * byte-at-a-time order on a little-endian host. The tail handles any host. */
#if defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
#define CRC_SLICE_BY_8 1
#else
#define CRC_SLICE_BY_8 0
#endif

static uint32_t crc_update(uint32_t crc, const uint8_t *p, uint64_t n)
{
#if CRC_SLICE_BY_8
    while (n >= 8) {
        uint32_t a, b;

        memcpy(&a, p, 4);
        memcpy(&b, p + 4, 4);
        a ^= crc;
        crc = crc_tab[7][a & 0xFFu]
            ^ crc_tab[6][(a >> 8) & 0xFFu]
            ^ crc_tab[5][(a >> 16) & 0xFFu]
            ^ crc_tab[4][(a >> 24) & 0xFFu]
            ^ crc_tab[3][b & 0xFFu]
            ^ crc_tab[2][(b >> 8) & 0xFFu]
            ^ crc_tab[1][(b >> 16) & 0xFFu]
            ^ crc_tab[0][(b >> 24) & 0xFFu];
        p += 8;
        n -= 8;
    }
#endif
    while (n--)
        crc = crc_tab[0][(crc ^ *p++) & 0xFFu] ^ (crc >> 8);
    return crc;
}

uint32_t adsc_ctdb_crc32_words(const uint16_t *v, uint64_t n)
{
    crc_init();
    return crc_update(0xFFFFFFFFu, (const uint8_t *)v, n * 2u) ^ 0xFFFFFFFFu;
}

/* One plane, one row: acc[c] <- v[c] ^ acc[c] * alpha^e, for c in [0, S).
 * `e` is constant across the run, which is the whole point of plane-major. */
static void plane_step(uint16_t *acc, const uint16_t *v, unsigned S,
                       unsigned e)
{
    for (unsigned c = 0; c < S; c++)
        acc[c] = (uint16_t)(adsc_gf16_mul_pow_fast(acc[c], e) ^ v[c]);
}

static void sweep_scalar(const uint16_t *pcm, uint64_t first, unsigned S,
                         unsigned sc, unsigned npar, uint16_t *acc,
                         uint32_t *crc_out)
{
    uint32_t crc = 0xFFFFFFFFu;

    for (unsigned j = 0; j < sc; j++) {
        const uint16_t *v = pcm + first + (uint64_t)j * S;

        if (crc_out)
            crc = crc_update(crc, (const uint8_t *)v, (uint64_t)S * 2u);
        /* alpha^0 == 1, so plane 0 is a plain XOR and needs no multiply. */
        for (unsigned c = 0; c < S; c++)
            acc[c] = (uint16_t)(acc[c] ^ v[c]);
        for (unsigned r = 1; r < npar; r++)
            plane_step(acc + (size_t)r * S, v, S, r);
    }
    if (crc_out)
        *crc_out = crc ^ 0xFFFFFFFFu;
}

/* ---- AVX2 --------------------------------------------------------------
 *
 * Multiplying a run of field elements by ONE constant is the only vectorisable
 * shape GF(2^16) offers without carry-less multiply, and plane-major is what
 * produces it. The kernel is the split-nibble table method: write a = a3a2a1a0
 * in nibbles, then a*k = XOR over i of ((a_i << 4i) * k), so four 16-entry
 * tables indexed by nibble replace the multiply. vpshufb is exactly a 16-entry
 * byte table lookup, 32 lanes at a time.
 *
 * That needs the operands as SEPARATE low and high byte streams — a nibble
 * table returns bytes, and interleaved uint16 would need a shuffle per lookup.
 * So the accumulator is held split ("ALTMAP": plane r is S low bytes followed
 * by S high bytes) FOR THE DURATION OF THE SWEEP ONLY, and converted back
 * before returning. That keeps the split form out of ctdb_internal.h and out
 * of both callers: the conversion is one pass over 376 KB against sc passes
 * over 383 MB, so it does not show up.
 *
 * Correctness is not argued from any of the above. tests/test_sweep.c requires
 * this kernel and the scalar one to agree bit-for-bit, and tests/ctdb_ab
 * compares the result against real CTDB parity on eight arms.
 */
#if defined(__x86_64__) && (defined(__GNUC__) || defined(__clang__))
#define SWEEP_HAVE_AVX2 1
#include <immintrin.h>

#define AVX2_FN __attribute__((target("avx2")))

/* T[e] holds, for the constant alpha^e, eight vpshufb tables: for each of the
 * four nibble positions, the low and high byte of that nibble's contribution.
 * Built once per sweep, not once per row. */
struct nib_tab {
    __m256i lo[4];
    __m256i hi[4];
};

AVX2_FN static void build_nib_tab(struct nib_tab *t, unsigned e)
{
    uint16_t k = adsc_gf16_pow(e);

    for (unsigned i = 0; i < 4; i++) {
        uint8_t lo[16], hi[16];

        for (unsigned n = 0; n < 16; n++) {
            uint16_t p = adsc_gf16_mul((uint16_t)(n << (4u * i)), k);

            lo[n] = (uint8_t)(p & 0xFFu);
            hi[n] = (uint8_t)(p >> 8);
        }
        /* Broadcast to both 128-bit lanes: vpshufb indexes within a lane. */
        t->lo[i] = _mm256_broadcastsi128_si256(
            _mm_loadu_si128((const __m128i *)(const void *)lo));
        t->hi[i] = _mm256_broadcastsi128_si256(
            _mm_loadu_si128((const __m128i *)(const void *)hi));
    }
}

/* 32 elements, given as separate low/high byte vectors, multiplied by the
 * constant the table was built for. */
AVX2_FN static inline void mul_const32(const struct nib_tab *t, __m256i *alo,
                                       __m256i *ahi)
{
    const __m256i mask = _mm256_set1_epi8(0x0F);
    __m256i n0 = _mm256_and_si256(*alo, mask);
    __m256i n1 = _mm256_and_si256(_mm256_srli_epi16(*alo, 4), mask);
    __m256i n2 = _mm256_and_si256(*ahi, mask);
    __m256i n3 = _mm256_and_si256(_mm256_srli_epi16(*ahi, 4), mask);

    *alo = _mm256_xor_si256(
        _mm256_xor_si256(_mm256_shuffle_epi8(t->lo[0], n0),
                         _mm256_shuffle_epi8(t->lo[1], n1)),
        _mm256_xor_si256(_mm256_shuffle_epi8(t->lo[2], n2),
                         _mm256_shuffle_epi8(t->lo[3], n3)));
    *ahi = _mm256_xor_si256(
        _mm256_xor_si256(_mm256_shuffle_epi8(t->hi[0], n0),
                         _mm256_shuffle_epi8(t->hi[1], n1)),
        _mm256_xor_si256(_mm256_shuffle_epi8(t->hi[2], n2),
                         _mm256_shuffle_epi8(t->hi[3], n3)));
}

/* Split 32 interleaved uint16 into 32 low bytes and 32 high bytes. */
AVX2_FN static inline void split32(const uint16_t *v, __m256i *lo, __m256i *hi)
{
    const __m256i sh = _mm256_setr_epi8(0, 2, 4, 6, 8, 10, 12, 14,
                                        1, 3, 5, 7, 9, 11, 13, 15,
                                        0, 2, 4, 6, 8, 10, 12, 14,
                                        1, 3, 5, 7, 9, 11, 13, 15);
    __m256i a = _mm256_shuffle_epi8(
        _mm256_loadu_si256((const __m256i *)(const void *)v), sh);
    __m256i b = _mm256_shuffle_epi8(
        _mm256_loadu_si256((const __m256i *)(const void *)(v + 16)), sh);

    /* Each now holds, per 128-bit lane, 8 low bytes then 8 high bytes. Gather
     * the quadwords, then reorder lanes: vpunpck works within lanes, so the
     * permute is what makes the 32 bytes contiguous in element order. */
    *lo = _mm256_permute4x64_epi64(_mm256_unpacklo_epi64(a, b), 0xD8);
    *hi = _mm256_permute4x64_epi64(_mm256_unpackhi_epi64(a, b), 0xD8);
}

AVX2_FN static void sweep_avx2(const uint16_t *pcm, uint64_t first, unsigned S,
                               unsigned sc, unsigned npar, uint16_t *acc,
                               uint32_t *crc_out, uint8_t *scratch)
{
    struct nib_tab tab[ADSC_RS16_MAX_NPAR];
    uint32_t crc = 0xFFFFFFFFu;
    uint8_t *alt = (uint8_t *)(void *)acc; /* plane r: [r*2S, r*2S+S) lo, then hi */
    unsigned vec = S & ~31u;               /* elements handled 32 at a time */

    for (unsigned r = 1; r < npar; r++)
        build_nib_tab(&tab[r], r);

    /* acc arrives zeroed, and zero is zero in either representation, so no
     * conversion is needed on the way in — only on the way out. */
    for (unsigned j = 0; j < sc; j++) {
        const uint16_t *v = pcm + first + (uint64_t)j * S;

        if (crc_out)
            crc = crc_update(crc, (const uint8_t *)v, (uint64_t)S * 2u);

        /* Split the row once and reuse it for every plane, rather than once
         * per plane. 23 KB, so it stays in L1 across the plane loop. */
        for (unsigned c = 0; c < vec; c += 32) {
            __m256i lo, hi;

            split32(v + c, &lo, &hi);
            _mm256_storeu_si256((__m256i *)(void *)(scratch + c), lo);
            _mm256_storeu_si256((__m256i *)(void *)(scratch + S + c), hi);
        }
        for (unsigned c = vec; c < S; c++) {
            scratch[c] = (uint8_t)(v[c] & 0xFFu);
            scratch[S + c] = (uint8_t)(v[c] >> 8);
        }

        for (unsigned r = 0; r < npar; r++) {
            uint8_t *p = alt + (size_t)r * 2u * S;

            for (unsigned c = 0; c < vec; c += 32) {
                __m256i lo = _mm256_loadu_si256(
                    (const __m256i *)(const void *)(p + c));
                __m256i hi = _mm256_loadu_si256(
                    (const __m256i *)(const void *)(p + S + c));

                /* alpha^0 == 1: plane 0 is a plain XOR, no table needed. */
                if (r)
                    mul_const32(&tab[r], &lo, &hi);
                lo = _mm256_xor_si256(lo, _mm256_loadu_si256(
                    (const __m256i *)(const void *)(scratch + c)));
                hi = _mm256_xor_si256(hi, _mm256_loadu_si256(
                    (const __m256i *)(const void *)(scratch + S + c)));
                _mm256_storeu_si256((__m256i *)(void *)(p + c), lo);
                _mm256_storeu_si256((__m256i *)(void *)(p + S + c), hi);
            }
            for (unsigned c = vec; c < S; c++) {
                uint16_t a = (uint16_t)(p[c] | ((uint16_t)p[S + c] << 8));

                a = (uint16_t)(adsc_gf16_mul_pow_fast(a, r)
                               ^ (uint16_t)(scratch[c]
                                            | ((uint16_t)scratch[S + c] << 8)));
                p[c] = (uint8_t)(a & 0xFFu);
                p[S + c] = (uint8_t)(a >> 8);
            }
        }
    }

    /* Split form back to uint16, plane by plane, through the row scratch. */
    for (unsigned r = 0; r < npar; r++) {
        uint8_t *p = alt + (size_t)r * 2u * S;
        uint16_t *out = acc + (size_t)r * S;

        memcpy(scratch, p, (size_t)S * 2u);
        for (unsigned c = 0; c < S; c++)
            out[c] = (uint16_t)(scratch[c] | ((uint16_t)scratch[S + c] << 8));
    }
    if (crc_out)
        *crc_out = crc ^ 0xFFFFFFFFu;
}
#endif /* SWEEP_HAVE_AVX2 */

/* Selection is one-shot and observable. ACCUDISC_REPAIR_KERNEL=scalar forces
 * the portable path, which is how the test suite exercises BOTH on a machine
 * that has AVX2 — otherwise the scalar kernel would ship untested here and
 * be discovered broken on someone else's hardware. */
/* Deliberately NOT cached in a static. The sweep runs once per repair, so this
 * costs one getenv per multi-second operation — and caching it would make the
 * two kernels uncomparable within a process, which is exactly what
 * tests/test_sweep.c has to do. A micro-optimisation that defeats the test
 * proving the optimisation is correct would be a poor trade. */
static int kernel_choice(void)
{
    const char *env = getenv("ACCUDISC_REPAIR_KERNEL");
    int choice = 0;

#ifdef SWEEP_HAVE_AVX2
    if (__builtin_cpu_supports("avx2"))
        choice = 1;
#endif
    if (env && !strcmp(env, "scalar"))
        choice = 0;
    return choice;
}

const char *adsc_ctdb_sweep_kernel(void)
{
    return kernel_choice() ? "avx2" : "scalar";
}

void adsc_ctdb_sweep(const uint16_t *pcm, uint64_t first, unsigned S,
                     unsigned sc, unsigned npar, uint16_t *acc,
                     uint32_t *crc_out)
{
    crc_init();
    adsc_gf16_init();
#ifdef SWEEP_HAVE_AVX2
    if (kernel_choice()) {
        uint8_t *scratch = malloc((size_t)S * 2u);

        if (scratch) {
            sweep_avx2(pcm, first, S, sc, npar, acc, crc_out, scratch);
            free(scratch);
            return;
        }
        /* Falling through on an allocation failure is deliberate: the scalar
         * path needs nothing, so a 23 KB shortage degrades speed rather than
         * failing a repair. */
    }
#endif
    sweep_scalar(pcm, first, S, sc, npar, acc, crc_out);
}
