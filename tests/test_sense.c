#include <assert.h>

#include "transport/transport.h"

int main(void)
{
    accudisc_sense s;

    /* Fixed format (0x70): MEDIUM ERROR, unrecovered read (3/11/05). */
    uint8_t fixed[18] = {0};
    fixed[0] = 0x70;
    fixed[2] = 0x03;
    fixed[12] = 0x11;
    fixed[13] = 0x05;
    adsc_sense_decode(fixed, sizeof(fixed), &s);
    assert(s.valid && s.key == 0x3 && s.asc == 0x11 && s.ascq == 0x05);

    /* Deferred fixed format (0x71) decodes the same way. */
    fixed[0] = 0x71;
    adsc_sense_decode(fixed, sizeof(fixed), &s);
    assert(s.valid && s.key == 0x3);

    /* Descriptor format (0x72): key/ASC/ASCQ at bytes 1/2/3. */
    uint8_t desc[8] = {0x72, 0x04, 0x08, 0x03, 0, 0, 0, 0};
    adsc_sense_decode(desc, sizeof(desc), &s);
    assert(s.valid && s.key == 0x4 && s.asc == 0x08 && s.ascq == 0x03);

    /* Truncated / garbage input must come back invalid, not crash. */
    adsc_sense_decode(fixed, 5, &s);
    assert(!s.valid);
    uint8_t junk[4] = {0xff, 0xff, 0xff, 0xff};
    adsc_sense_decode(junk, sizeof(junk), &s);
    assert(!s.valid);
    adsc_sense_decode(junk, 0, &s);
    assert(!s.valid);

    return 0;
}
