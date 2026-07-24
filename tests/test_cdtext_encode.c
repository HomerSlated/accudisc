/* Phase 3 Step B3: CD-Text 18-byte packs -> 96-byte R-W subchannel blocks,
 * with ring fill for non-multiple-of-4 pack counts (RECORDING_PLAN §11.5).
 *
 * Hardware-free. The oracle is the inverse extraction (cdrdao's getRawRWdata),
 * so a block decoded back to 72 bytes must reproduce the four ring packs that
 * built it. Byte-exact because 72*8 == 96*6 (the 6-bit packing loses nothing).
 */

#include <assert.h>
#include <stdint.h>
#include <string.h>

#include <accudisc/accudisc.h>

#include "meta/cdtext_encode.h"

#define PACK 18u

/* Inverse of pack3 / adsc_cdtext_encode_rw: four six-bit symbols -> three
 * bytes. Mirrors cdrdao PWSubChannel96::getRawRWdata exactly. */
static void unpack4(const uint8_t *sym, uint8_t *out3)
{
    out3[0] = (uint8_t)(((sym[0] << 2) & 0xfc) | ((sym[1] >> 4) & 0x03));
    out3[1] = (uint8_t)(((sym[1] << 4) & 0xf0) | ((sym[2] >> 2) & 0x0f));
    out3[2] = (uint8_t)(((sym[2] << 6) & 0xc0) | (sym[3] & 0x3f));
}

/* Decode one 96-byte block back into its four 18-byte packs (72 bytes). */
static void decode_block(const uint8_t *blk, uint8_t *packs72)
{
    for (uint32_t g = 0; g < 24u; g++)
        unpack4(blk + g * 4u, packs72 + g * 3u);
}

/* Fill n packs with distinct, full-8-bit content so the round-trip is exact
 * only if every bit survives. */
static void make_packs(uint8_t *packs, uint32_t n)
{
    for (uint32_t k = 0; k < n; k++)
        for (uint32_t i = 0; i < PACK; i++)
            packs[k * PACK + i] = (uint8_t)(k * 31u + i * 7u + 1u);
}

/* Encode n packs, then assert each block round-trips to the ring packs and no
 * output byte uses the top two (P/Q) bits. */
static void check_roundtrip(uint32_t n)
{
    uint8_t packs[64 * PACK];
    uint8_t out[64 * PACK * 4]; /* >= nblocks*96 for n <= 64 */
    assert(n <= 64);

    make_packs(packs, n);
    uint32_t nblocks = adsc_cdtext_rw_block_count(n);
    assert(adsc_cdtext_encode_rw(packs, n, out) == ACCUDISC_OK);

    for (uint32_t b = 0; b < nblocks; b++) {
        const uint8_t *blk = out + b * 96u;
        for (uint32_t i = 0; i < 96u; i++)
            assert((blk[i] & 0xc0) == 0); /* only the low 6 bits are R-W */

        uint8_t got[72];
        decode_block(blk, got);
        for (uint32_t j = 0; j < 4u; j++) {
            uint32_t idx = (4u * b + j) % n; /* the ring pack for this slot */
            assert(memcmp(got + j * PACK, packs + idx * PACK, PACK) == 0);
        }
    }
}

int main(void)
{
    /* --- block count = lcm(npacks,4)/4 = npacks/gcd(npacks,4) --------------- */
    assert(adsc_cdtext_rw_block_count(0) == 0);
    assert(adsc_cdtext_rw_block_count(1) == 1);  /* pack repeated x4 */
    assert(adsc_cdtext_rw_block_count(2) == 1);  /* 0,1,0,1 */
    assert(adsc_cdtext_rw_block_count(3) == 3);  /* odd -> npacks */
    assert(adsc_cdtext_rw_block_count(4) == 1);
    assert(adsc_cdtext_rw_block_count(6) == 3);  /* %4==2 -> npacks/2 */
    assert(adsc_cdtext_rw_block_count(8) == 2);
    assert(adsc_cdtext_rw_block_count(33) == 33); /* real cdemu capture */
    assert(adsc_cdtext_rw_block_count(35) == 35); /* real px716a capture */
    assert(adsc_cdtext_rw_block_count(42) == 21); /* real libmirage capture */

    /* --- exact bit-packing vector (matches cdrdao setRawRWdata) ------------- */
    {
        /* One pack whose first three bytes are FF,00,FF; symbols 0..3 of the
         * block must be 3f,30,03,3f. */
        uint8_t packs[PACK];
        uint8_t out[96];
        memset(packs, 0, sizeof packs);
        packs[0] = 0xff;
        packs[1] = 0x00;
        packs[2] = 0xff;
        assert(adsc_cdtext_encode_rw(packs, 1, out) == ACCUDISC_OK);
        assert(out[0] == 0x3f && out[1] == 0x30 && out[2] == 0x03 &&
               out[3] == 0x3f);
    }

    /* --- round-trips, multiples of 4 and the wild non-multiples ------------- */
    check_roundtrip(4);   /* aligned */
    check_roundtrip(8);   /* aligned, multiple blocks */
    check_roundtrip(1);   /* degenerate ring */
    check_roundtrip(2);   /* %4==2, single block */
    check_roundtrip(3);   /* odd */
    check_roundtrip(6);   /* %4==2, three blocks */
    check_roundtrip(33);  /* the captures that killed the multiple-of-4 rule */
    check_roundtrip(35);
    check_roundtrip(42);

    /* --- argument checks --------------------------------------------------- */
    {
        uint8_t packs[PACK] = {0};
        uint8_t out[96];
        assert(adsc_cdtext_encode_rw(NULL, 1, out) == ACCUDISC_ERR_INVAL);
        assert(adsc_cdtext_encode_rw(packs, 1, NULL) == ACCUDISC_ERR_INVAL);
        assert(adsc_cdtext_encode_rw(packs, 0, out) == ACCUDISC_ERR_INVAL);
    }

    return 0;
}
