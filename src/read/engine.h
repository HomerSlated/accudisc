#ifndef ADSC_ENGINE_H
#define ADSC_ENGINE_H

#include <stdint.h>

/* Status-map byte builders (state + severity nibble). Pure — unit-tested. */
uint8_t adsc_map_c2_byte(uint32_t c2_bits);        /* sev ~log2(bits) */
uint8_t adsc_map_recovered_byte(unsigned attempts); /* sev = attempts */
uint8_t adsc_map_suspect_byte(uint32_t diff_bytes); /* sev ~log2(diff) */

/* Differing-byte count between two 2352-byte audio payloads. Pure. */
uint32_t adsc_audio_diff(const uint8_t *a, const uint8_t *b);

#endif /* ADSC_ENGINE_H */
