#ifndef ADSC_ENGINE_H
#define ADSC_ENGINE_H

#include <stdint.h>

#include <accudisc/accudisc.h>

/* Q health byte for one parsed frame: ACCUDISC_SUBQ_*, severity always 0.
 * Exposed for unit testing rather than for use — the classification order is
 * load-bearing (crc_ok must be consulted before adr) and a reimplementation in
 * the test would be free to get it right while the engine got it wrong. Pure. */
uint8_t adsc_subq_byte(const accudisc_q *q, uint32_t expect_lba);
/* Absolute disc position a CRC-valid ADR=1 frame reports, as an LBA.
 * Meaningless for any other frame — check crc_ok and adr first. */
int32_t adsc_q_position_lba(const accudisc_q *q);

/* Status-map byte builders (state + severity nibble). Pure — unit-tested. */
uint8_t adsc_map_c2_byte(uint32_t c2_bits);        /* sev ~log2(bits) */
uint8_t adsc_map_recovered_byte(unsigned attempts); /* sev = attempts */
uint8_t adsc_map_suspect_byte(uint32_t diff_bytes); /* sev ~log2(diff) */

/* Differing-byte count between two 2352-byte audio payloads. Pure. */
uint32_t adsc_audio_diff(const uint8_t *a, const uint8_t *b);

/* Is b a purely shifted copy of a (a positioning slip, not data damage)?
 * Searches ±half a sector in whole samples and demands the full overlap
 * match at the found shift. 1 = yes (*shift_samples set, nonzero), 0 = no.
 * Pure. */
int adsc_shift_find(const uint8_t *a, const uint8_t *b,
                    int32_t *shift_samples);

#endif /* ADSC_ENGINE_H */
