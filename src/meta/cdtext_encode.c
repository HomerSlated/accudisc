/* CD-Text pack -> R-W subchannel block encoding (Phase 3 Step B3, §11.5).
 *
 * The CD-Text lead-in carries 18-byte packs in the R-W subchannel as 6-bit
 * symbols: three pack bytes become four symbols (MSB first), so 4 packs (72
 * bytes) = 96 symbols = one sector's 96-byte R-W block. This is a PLAIN bit
 * packing -- unlike CD+G program-area R-W (src/cdda/rw.c), CD-Text uses NO
 * Reed-Solomon and NO convolutional interleave; its error protection is the
 * per-pack 16-bit CRC that Step B2 validates. Confirmed against cdrdao
 * PWSubChannel96::setRawRWdata (the "used for CD-TEXT" path) and its inverse
 * getRawRWdata.
 *
 * Ring fill: a pack count not divisible by 4 leaves the last block of a single
 * pass short. Rather than pad with invented packs, treat the pack stream as a
 * ring; block b carries packs (4b..4b+3) mod npacks. The minimal set that tiles
 * seamlessly is lcm(npacks,4)/4 blocks, after which the pattern repeats -- so
 * the burn path (B4/B5) cycles this set to fill the lead-in. This matches
 * cdrdao CdTextEncoder::buildSubChannels exactly (its packCount%4 switch yields
 * the same counts); it is not a divergence.
 */

#include <string.h>

#include <accudisc/accudisc.h>

#include "cdtext_encode.h"

#define PACK ADSC_CDTEXT_PACK_BYTES

/* gcd(a, 4) for a > 0, without a general gcd loop. */
static uint32_t gcd4(uint32_t a)
{
    if (a % 4u == 0u)
        return 4u;
    if (a % 2u == 0u)
        return 2u;
    return 1u;
}

uint32_t adsc_cdtext_rw_block_count(uint32_t npacks)
{
    if (npacks == 0u)
        return 0u;
    return npacks / gcd4(npacks); /* == lcm(npacks, 4) / 4 */
}

/* Pack three data bytes into four six-bit R-W symbols (MSB first). Mirrors
 * cdrdao PWSubChannel96::setRawRWdata; the top two bits (P, Q) stay clear. */
static void pack3(const uint8_t *in, uint8_t *out)
{
    out[0] = (uint8_t)((in[0] >> 2) & 0x3f);
    out[1] = (uint8_t)(((in[0] << 4) & 0x30) | ((in[1] >> 4) & 0x0f));
    out[2] = (uint8_t)(((in[1] << 2) & 0x3c) | ((in[2] >> 6) & 0x03));
    out[3] = (uint8_t)(in[2] & 0x3f);
}

int adsc_cdtext_encode_rw(const uint8_t *packs, uint32_t npacks, uint8_t *out)
{
    if (!packs || !out || npacks == 0u)
        return ACCUDISC_ERR_INVAL;

    uint32_t nblocks = adsc_cdtext_rw_block_count(npacks);

    for (uint32_t b = 0; b < nblocks; b++) {
        uint8_t buf[4u * PACK]; /* 72: four packs gathered from the ring */

        for (uint32_t j = 0; j < 4u; j++) {
            uint32_t idx = (4u * b + j) % npacks;
            memcpy(buf + j * PACK, packs + idx * PACK, PACK);
        }

        uint8_t *blk = out + b * ADSC_RW_BLOCK_BYTES;
        /* 72 bytes -> 96 symbols, in 24 groups of three-bytes-to-four-symbols.
         * 18 is a multiple of 3, so no group ever straddles a pack boundary. */
        for (uint32_t g = 0; g < 24u; g++)
            pack3(buf + g * 3u, blk + g * 4u);
    }
    return ACCUDISC_OK;
}
