#include <string.h>

#include "transport.h"

void adsc_sense_decode(const uint8_t *sense, unsigned len, accudisc_sense *out)
{
    memset(out, 0, sizeof(*out));
    if (len < 1)
        return;

    unsigned code = sense[0] & 0x7f;
    if ((code == 0x70 || code == 0x71) && len >= 14) {
        /* Fixed format: key at byte 2, ASC/ASCQ at 12/13. */
        out->valid = 1;
        out->key = sense[2] & 0x0f;
        out->asc = sense[12];
        out->ascq = sense[13];
    } else if ((code == 0x72 || code == 0x73) && len >= 4) {
        /* Descriptor format: key/ASC/ASCQ at bytes 1/2/3. */
        out->valid = 1;
        out->key = sense[1] & 0x0f;
        out->asc = sense[2];
        out->ascq = sense[3];
    }
}
