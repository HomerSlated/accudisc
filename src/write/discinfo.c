/* Disc-state check for the recording engine (READ DISC INFORMATION).
 *
 * A DAO burn requires a blank (or erased) disc; this reports the status so the
 * engine can refuse to overwrite a recorded disc. Read-only and safe.
 */

#include <string.h>

#include "../mmc/mmc.h"
#include "write.h"

uint32_t adsc_leadin_len_from_msf(uint8_t m, uint8_t s, uint8_t f)
{
    uint32_t start = ((uint32_t)m * 60u + s) * 75u + f;

    /* 80:00:00 = LBA 360000, 100:00:00 = 450000. Outside that window the MSF
     * is not a usable ATIP lead-in start, so fall back rather than subtract. */
    if (start >= 360000u && start < 450000u)
        return 450000u - start;
    return 4500u; /* 1 minute */
}

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

    /* Lead-in start MSF (bytes 17-19) -> the writable lead-in extent. See the
     * derivation note on struct adsc_disc_info; mirrors cdrdao
     * GenericMMC.cc:414-432. A drive that returns a short response, or an MSF
     * that is not a plausible ATIP lead-in, gets the one-minute fallback rather
     * than an underflowing subtraction. */
    out->leadin_len = 4500u; /* 1 minute, the fallback when bytes 17-19 absent */
    if (len >= 20) {
        out->leadin_m = buf[17];
        out->leadin_s = buf[18];
        out->leadin_f = buf[19];
        out->leadin_len = adsc_leadin_len_from_msf(buf[17], buf[18], buf[19]);
    }
    return ACCUDISC_OK;
}
