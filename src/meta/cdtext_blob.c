/* CD-Text pass-through blob validation (Phase 3 Step B2, RECORDING_PLAN §11.4).
 *
 * Validates a raw READ TOC format-05 blob WITHOUT interpreting the payload:
 *   [0..1] TOC data length (big-endian, = len-2)
 *   [2..3] reserved
 *   [4..]  N x 18-byte CD-Text packs; each pack's [16..17] is the X.25 CRC,
 *          stored complemented (so stored == ~crc16(pack[0..15])).
 *
 * Structural checks, then a three-way per-pack CRC rule (see header). The
 * payload bytes are never read, interpreted or transcoded -- only the 16 bits
 * that exist to validate them, and those only when the drive dropped them
 * (all-zero). Mechanism for the zero-CRC case is a Plextor drive behaviour, not
 * a tool quirk: redumper's cd/toc.ixx:422 hard-codes an exemption for a Plextor
 * whose final pack CRC "is always zeroed", while its own dump path writes the
 * blob verbatim and never zeroes anything (cd_common.ixx:190). Keith decided
 * 2026-07-24 to recompute zero-CRC packs (payload untouched); non-zero-but-wrong
 * stays damage and refuses.
 */

#include <string.h>

#include <accudisc/accudisc.h>

#include "../cdda/crc16.h"
#include "cdtext_blob.h"

#define HDR_LEN  4u
#define PACK_LEN 18u

int adsc_cdtext_blob_validate(uint8_t *blob, uint32_t len,
                              struct adsc_cdtext_info *info)
{
    struct adsc_cdtext_info scratch;
    if (!info)
        info = &scratch;
    memset(info, 0, sizeof(*info));

    if (!blob)
        return ACCUDISC_ERR_INVAL;
    if (len < HDR_LEN + PACK_LEN)
        return ACCUDISC_ERR_SHORT;
    if ((len - HDR_LEN) % PACK_LEN != 0)
        return ACCUDISC_ERR_INVAL;

    /* Header length field [0..1] is defined as len-2. Some drives pad the
     * transfer; refuse on inconsistency rather than trust a padded blob. */
    uint32_t hdr_len = ((uint32_t)blob[0] << 8) | blob[1];
    if (hdr_len != len - 2u)
        return ACCUDISC_ERR_INVAL;

    uint32_t npacks = (len - HDR_LEN) / PACK_LEN;
    info->npacks = npacks;

    for (uint32_t i = 0; i < npacks; i++) {
        uint8_t *p = blob + HDR_LEN + i * PACK_LEN;
        uint16_t stored = (uint16_t)(((uint16_t)p[16] << 8) | p[17]);
        uint16_t want = (uint16_t)~adsc_crc16(p, 16); /* stored is complemented */

        if (stored != want) {
            if (stored == 0x0000) {
                /* Transport dropped the check field; regenerate it from the
                 * payload we never touched. Not a content change. */
                p[16] = (uint8_t)(want >> 8);
                p[17] = (uint8_t)(want & 0xff);
                info->crc_recomputed++;
            } else {
                info->bad_pack = i;
                return ACCUDISC_ERR_CRC; /* non-zero and wrong = damage */
            }
        }

        /* Capture the first SIZE_INFO (0x8f) pack's first/last track for the
         * caller's optional .toc-consistency check (§11.4). The reconstructed
         * size-info block begins charset,[first],[last] at payload bytes 0,1,2,
         * i.e. p[4],p[5],p[6]. v0: non-extension, block 0 only. We report it;
         * acting on a mismatch (warn, exit 3) is the caller's, not a refusal. */
        if (!info->have_size_info && p[0] == 0x8f &&
            !(p[1] & 0x80) && ((p[3] >> 4) & 0x07) == 0) {
            info->have_size_info = 1;
            info->si_first_track = p[5];
            info->si_last_track = p[6];
        }
    }
    return ACCUDISC_OK;
}
