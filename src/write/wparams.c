/* DAO write-parameters setup (recording engine, phase 1 slice 1).
 *
 * Read the write-parameters mode page (0x05), configure it for Disc-At-Once
 * (Session-At-Once) audio recording, and select it back. This only programs
 * the drive's write registers — it does NOT touch the disc, so it is safe and
 * fully reversible (the drive resets the page on eject/power). Field layout
 * and the variant handling follow cdrdao's GenericMMC::setWriteParameters
 * (reference/cdrdao/dao/GenericMMC.cc); credited in docs/ATTRIBUTION.md.
 */

#include <string.h>

#include "../mmc/mmc.h"
#include "write.h"

int adsc_write_set_params(struct accudisc_device *dev,
                          const struct adsc_write_params *wp)
{
    uint8_t buf[64];
    uint32_t len = 0, po = 0;
    int rc;

    if (!dev || !wp)
        return ACCUDISC_ERR_INVAL;

    rc = adsc_mmc_mode_sense10(dev, ADSC_MODE_WRITE_PARAMS, buf,
                               sizeof(buf), &len, &po);
    if (rc != ACCUDISC_OK)
        return rc;
    if (po + 9 > len)             /* need bytes 0..8 of the page */
        return ACCUDISC_ERR_SHORT;

    uint8_t *p = buf + po;

    /* byte 2: [BUFE(0x40)][Test Write(0x10)][Write Type(low 4)]. */
    p[2] = (uint8_t)(p[2] & 0xe0);      /* keep BUFE/LS-V, clear write type */
    p[2] |= 0x02;                       /* write type = Session-at-once (DAO) */
    if (wp->simulate)
        p[2] |= 0x10;                   /* test write: exercise path, no laser */
    if (wp->burnproof)
        p[2] |= 0x40;                   /* BURN-Proof buffer-underrun protect */

    /* byte 3: multisession bits 7-6. Single session, next not allowed. */
    p[3] = (uint8_t)(p[3] & 0x3f);

    /* byte 4: data block type (low nibble). Audio DAO uses raw 2352 (0), or
     * raw + P-W subchannel 2448 (3) when CD-Text must go in the lead-in. */
    p[4] = (uint8_t)((p[4] & 0xf0) | (wp->cdtext ? 0x03 : 0x00));

    /* byte 8: session format. 0x00 = CD-DA / CD-ROM. */
    p[8] = 0x00;

    return adsc_mmc_mode_select10(dev, buf, len, po);
}

int adsc_write_get_params(struct accudisc_device *dev,
                          struct adsc_write_params *out)
{
    uint8_t buf[64];
    uint32_t len = 0, po = 0;
    int rc;

    if (!dev || !out)
        return ACCUDISC_ERR_INVAL;

    rc = adsc_mmc_mode_sense10(dev, ADSC_MODE_WRITE_PARAMS, buf,
                               sizeof(buf), &len, &po);
    if (rc != ACCUDISC_OK)
        return rc;
    if (po + 9 > len)
        return ACCUDISC_ERR_SHORT;

    const uint8_t *p = buf + po;
    memset(out, 0, sizeof(*out));
    out->write_type = (uint8_t)(p[2] & 0x0f);
    out->simulate   = (p[2] & 0x10) ? 1 : 0;
    out->burnproof  = (p[2] & 0x40) ? 1 : 0;
    out->cdtext     = ((p[4] & 0x0f) == 0x03) ? 1 : 0;
    return ACCUDISC_OK;
}
