/* Disc-state check for the recording engine (READ DISC INFORMATION).
 *
 * A DAO burn requires a blank (or erased) disc; this reports the status so the
 * engine can refuse to overwrite a recorded disc. Read-only and safe.
 */

#include <string.h>

#include "../mmc/mmc.h"
#include "write.h"

int adsc_write_read_disc_info(struct accudisc_device *dev,
                              struct adsc_disc_info *out)
{
    uint8_t buf[34];
    uint32_t len = 0;
    int rc;

    if (!dev || !out)
        return ACCUDISC_ERR_INVAL;

    rc = adsc_mmc_read_disc_info(dev, buf, sizeof(buf), &len);
    if (rc != ACCUDISC_OK)
        return rc;
    if (len < 7)
        return ACCUDISC_ERR_SHORT;

    memset(out, 0, sizeof(*out));
    out->status      = buf[2] & 0x03;         /* 0 blank .. 2 complete */
    out->erasable    = (buf[2] >> 4) & 0x01;  /* CD-RW */
    out->first_track = buf[3];
    out->sessions    = buf[4];                /* LSB (enough for CD) */
    out->last_track  = buf[6];                /* last track, last session LSB */
    return ACCUDISC_OK;
}
