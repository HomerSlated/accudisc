/* CD-Text pack -> R-W subchannel block encoding (Phase 3 Step B3, §11.5).
 * Internal — not part of the public ABI. */
#ifndef ADSC_CDTEXT_ENCODE_H
#define ADSC_CDTEXT_ENCODE_H

#include <stdint.h>

#define ADSC_CDTEXT_PACK_BYTES 18u /* one CD-Text pack (incl. its 2-byte CRC) */
#define ADSC_RW_BLOCK_BYTES    96u /* one sector's R-W subchannel, 1 symbol/byte */

/* Minimal number of 96-byte R-W blocks the pack ring produces before the
 * pattern repeats: lcm(npacks, 4) / 4 == npacks / gcd(npacks, 4). Cycling this
 * many blocks tiles the lead-in seamlessly (the ring closes exactly). Returns 0
 * for npacks == 0. Matches cdrdao CdTextEncoder::buildSubChannels. */
uint32_t adsc_cdtext_rw_block_count(uint32_t npacks);

/* Encode `npacks` 18-byte CD-Text packs into the minimal repeating set of
 * 96-byte R-W subchannel blocks (RING FILL). Writes exactly
 * adsc_cdtext_rw_block_count(npacks) blocks of 96 bytes into `out`; the caller
 * sizes `out` from that count. Block b carries packs (4b .. 4b+3) mod npacks, so
 * a pack count not divisible by 4 still fills every block with real packs across
 * the wrap — nothing is invented. Each block holds 96 six-bit R-W symbols in the
 * low 6 bits of each byte (3 pack bytes -> 4 symbols, MSB first, matching cdrdao
 * PWSubChannel96::setRawRWdata); the top two bits (P, Q) are the burn path's.
 * `packs` points at the pack area of a validated blob (i.e. blob + 4). Returns
 * ACCUDISC_OK, or ACCUDISC_ERR_INVAL if packs/out is NULL or npacks == 0. */
int adsc_cdtext_encode_rw(const uint8_t *packs, uint32_t npacks, uint8_t *out);

#endif /* ADSC_CDTEXT_ENCODE_H */
