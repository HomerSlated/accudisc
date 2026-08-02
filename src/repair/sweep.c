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

#include <string.h>

#include "gf16.h"
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

void adsc_ctdb_sweep(const uint16_t *pcm, uint64_t first, unsigned S,
                     unsigned sc, unsigned npar, uint16_t *acc,
                     uint32_t *crc_out)
{
    uint32_t crc = 0xFFFFFFFFu;

    crc_init();
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
