/* Phase 3 Step B2: CD-Text pass-through blob validation (RECORDING_PLAN §11.4).
 *
 * Hardware-free: blobs are synthesised in-code (the real captures are private
 * and git-ignored, §11.1), so the CRCs are ours and the mutations exact.
 * Exercises the three-way CRC rule and the structural gate.
 */

#include <assert.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <accudisc/accudisc.h>

#include "cdda/crc16.h"
#include "meta/cdtext_blob.h"

/* Each pack spec is its 16 covered bytes ([0]type [1]track [2]seq
 * [3]block/charpos [4..15]payload); build() appends the complemented CRC. */
static uint8_t *build(const uint8_t packs[][16], uint32_t n, uint32_t *len_out)
{
    uint32_t L = 4u + n * 18u;
    uint8_t *b = malloc(L);
    assert(b);
    uint32_t hdr = L - 2u;
    b[0] = (uint8_t)(hdr >> 8);
    b[1] = (uint8_t)hdr;
    b[2] = 0;
    b[3] = 0;
    for (uint32_t i = 0; i < n; i++) {
        uint8_t *p = b + 4 + i * 18;
        memcpy(p, packs[i], 16);
        uint16_t stored = (uint16_t)~adsc_crc16(p, 16);
        p[16] = (uint8_t)(stored >> 8);
        p[17] = (uint8_t)stored;
    }
    *len_out = L;
    return b;
}

/* Three well-formed packs: title, performer, and a SIZE_INFO declaring
 * first track 1, last track 3. Three packs is deliberately not a multiple of
 * four (see the non-mult-of-4 assertion below). */
static const uint8_t PACKS[3][16] = {
    {0x80, 0x01, 0x00, 0x00, 'A', 'L', 'B', 'U', 'M', 0, 0, 0, 0, 0, 0, 0},
    {0x81, 0x01, 0x01, 0x00, 'B', 'A', 'N', 'D', 0, 0, 0, 0, 0, 0, 0, 0},
    {0x8f, 0x00, 0x02, 0x00, 0x00, 0x01, 0x03, 0x00, 0, 0, 0, 0, 0, 0, 0, 0},
};

int main(void)
{
    struct adsc_cdtext_info info;
    uint32_t len;

    /* --- a valid blob passes; SIZE_INFO first/last are read out -------------
     * 3 packs, not a multiple of 4: this must NOT be refused (§11.4 killed the
     * multiple-of-4 rule with real 33- and 35-pack captures). */
    {
        uint8_t *b = build(PACKS, 3, &len);
        assert(adsc_cdtext_blob_validate(b, len, &info) == ACCUDISC_OK);
        assert(info.npacks == 3);
        assert(info.crc_recomputed == 0);
        assert(info.have_size_info == 1);
        assert(info.si_first_track == 1 && info.si_last_track == 3);
        free(b);
    }

    /* --- zero-CRC pack is repaired, not refused ----------------------------
     * The Plextor transport artifact: a pack whose CRC field is all-zero. We
     * regenerate it from the untouched payload and report the count; a second
     * pass then sees a fully-valid blob (the repair persisted). */
    {
        uint8_t *b = build(PACKS, 3, &len);
        uint8_t *last = b + 4 + 2 * 18;
        last[16] = 0x00; /* drop the check field, as the drive does */
        last[17] = 0x00;
        assert(adsc_cdtext_blob_validate(b, len, &info) == ACCUDISC_OK);
        assert(info.crc_recomputed == 1);
        /* payload untouched */
        assert(last[0] == 0x8f && last[6] == 0x03);
        /* repair persisted: re-validate finds nothing to fix */
        assert(adsc_cdtext_blob_validate(b, len, &info) == ACCUDISC_OK);
        assert(info.crc_recomputed == 0);
        free(b);
    }

    /* --- a non-zero but wrong CRC is damage: refuse, name the pack ---------- */
    {
        uint8_t *b = build(PACKS, 3, &len);
        uint8_t *p = b + 4 + 1 * 18; /* pack index 1 */
        uint16_t want = (uint16_t)~adsc_crc16(p, 16);
        uint16_t bad = (uint16_t)(want ^ 0x00ff);
        if (bad == 0)
            bad = 0x1234; /* guarantee non-zero and != want */
        p[16] = (uint8_t)(bad >> 8);
        p[17] = (uint8_t)bad;
        assert(adsc_cdtext_blob_validate(b, len, &info) == ACCUDISC_ERR_CRC);
        assert(info.bad_pack == 1);
        free(b);
    }

    /* --- structural failures ----------------------------------------------- */
    {
        uint8_t *b = build(PACKS, 1, &len); /* a valid 22-byte, 1-pack blob */

        /* too short to hold even one pack */
        assert(adsc_cdtext_blob_validate(b, 10, &info) == ACCUDISC_ERR_SHORT);

        /* length not 4 + 18k */
        assert(adsc_cdtext_blob_validate(b, len + 5, &info) ==
               ACCUDISC_ERR_INVAL);

        /* header length field disagrees with the buffer (drive padding, etc.) */
        uint8_t save0 = b[0], save1 = b[1];
        b[0] = 0xff;
        b[1] = 0xff;
        assert(adsc_cdtext_blob_validate(b, len, &info) == ACCUDISC_ERR_INVAL);
        b[0] = save0;
        b[1] = save1;
        /* restored header validates again */
        assert(adsc_cdtext_blob_validate(b, len, &info) == ACCUDISC_OK);
        assert(info.npacks == 1);
        free(b);
    }

    /* --- NULL blob rejected ------------------------------------------------ */
    assert(adsc_cdtext_blob_validate(NULL, 100, &info) == ACCUDISC_ERR_INVAL);

    /* --- info may be NULL --------------------------------------------------- */
    {
        uint8_t *b = build(PACKS, 3, &len);
        assert(adsc_cdtext_blob_validate(b, len, NULL) == ACCUDISC_OK);
        free(b);
    }

    return 0;
}
