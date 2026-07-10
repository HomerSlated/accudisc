#ifndef ADSC_CRC16_H
#define ADSC_CRC16_H

#include <stddef.h>
#include <stdint.h>

/* CRC-16/X.25 as used by both the Q subchannel and CD-Text packs: poly
 * 0x1021, init 0, no reflection; the value stored on disc is the one's
 * complement of this. */
uint16_t adsc_crc16(const uint8_t *data, size_t len);

#endif /* ADSC_CRC16_H */
