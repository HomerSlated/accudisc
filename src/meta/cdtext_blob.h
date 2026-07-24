/* CD-Text pass-through blob validation (Phase 3 Step B2, RECORDING_PLAN §11.4).
 * Internal — not part of the public ABI. */
#ifndef ADSC_CDTEXT_BLOB_H
#define ADSC_CDTEXT_BLOB_H

#include <stdint.h>

/* What validation observed about a format-05 CD-Text blob. Filled even on the
 * error paths as far as validation got (npacks/crc_recomputed are meaningful;
 * bad_pack is set only on ACCUDISC_ERR_CRC). */
struct adsc_cdtext_info {
    uint32_t npacks;          /* number of 18-byte packs (0 if structure bad) */
    uint32_t crc_recomputed;  /* zero-CRC packs whose check field we regenerated */
    int      have_size_info;  /* a usable 0x8f SIZE_INFO pack was seen */
    uint8_t  si_first_track;  /* SIZE_INFO first track (iff have_size_info) */
    uint8_t  si_last_track;   /* SIZE_INFO last track  (iff have_size_info) */
    uint32_t bad_pack;        /* on ACCUDISC_ERR_CRC: index of the damaged pack */
};

/* Validate (and minimally repair) a raw READ TOC format-05 CD-Text blob IN
 * PLACE, without interpreting the payload. Structural checks first (length,
 * header cross-check), then a three-way per-pack CRC rule:
 *   - CRC valid            -> keep.
 *   - CRC field all-zero    -> regenerate it from the untouched payload (a
 *                              Plextor transport artifact, §11.4) and count it.
 *   - CRC non-zero + wrong  -> ACCUDISC_ERR_CRC (damage; refuse the burn).
 * Fills *info (may be NULL). Returns ACCUDISC_OK, ACCUDISC_ERR_SHORT (blob too
 * short to hold one pack), ACCUDISC_ERR_INVAL (malformed structure), or
 * ACCUDISC_ERR_CRC. blob is mutable so zero-CRC packs can be repaired. */
int adsc_cdtext_blob_validate(uint8_t *blob, uint32_t len,
                              struct adsc_cdtext_info *info);

#endif /* ADSC_CDTEXT_BLOB_H */
