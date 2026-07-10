#ifndef ADSC_ENGINE_H
#define ADSC_ENGINE_H

#include <stdint.h>

/* Status-map byte for a C2-flagged sector: state ACCUDISC_MAP_C2 with a
 * ~log2(bits) severity in the high nibble. Pure — unit-tested. */
uint8_t adsc_map_c2_byte(uint32_t c2_bits);

#endif /* ADSC_ENGINE_H */
