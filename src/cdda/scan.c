/* MCN/ISRC scanners: read raw subchannel and hunt the Q stream for ADR 2/3
 * frames. The spec requires MCN/ISRC (when present) to recur at least once
 * per 100 sectors, so a bounded scan that finds nothing is a real absence,
 * not bad luck. CRC-gated: only frames that verify are believed. */

#include <stdlib.h>

#include "../mmc/mmc.h"

#define SCAN_CHUNK 16
#define SCAN_BUDGET 200 /* sectors; ~2x the spec's recurrence interval */

static int scan_adr(struct accudisc_device *dev, uint32_t lba, unsigned adr,
                    accudisc_q *hit)
{
    uint32_t sector_len =
        adsc_read_cd_sector_len(ADSC_C2_NONE, ADSC_SUB_RAW);
    uint8_t *buf = malloc((size_t)SCAN_CHUNK * sector_len);
    int rc = ACCUDISC_ERR_NOTFOUND;

    if (!buf)
        return ACCUDISC_ERR_NOMEM;

    for (uint32_t off = 0; off < SCAN_BUDGET; off += SCAN_CHUNK) {
        int crc = adsc_mmc_read_cd(dev, lba + off, SCAN_CHUNK,
                                   ADSC_SECTOR_CDDA, ADSC_C2_NONE,
                                   ADSC_SUB_RAW, buf, sector_len);
        if (crc != ACCUDISC_OK) {
            rc = crc;
            break;
        }
        for (unsigned s = 0; s < SCAN_CHUNK; s++) {
            const uint8_t *sub =
                buf + (size_t)s * sector_len + ACCUDISC_BYTES_AUDIO;
            uint8_t q[12];
            accudisc_q parsed;

            accudisc_sub_extract_q(sub, q);
            if (accudisc_q_parse(q, &parsed) == ACCUDISC_OK &&
                parsed.adr == adr) {
                *hit = parsed;
                rc = ACCUDISC_OK;
                goto out;
            }
        }
    }
out:
    free(buf);
    return rc;
}

/* All-zero MCN/ISRC frames are the disc's way of transmitting "none
 * encoded" (CRC-valid, but carrying no identity) — report them as absent. */
static int all_zero_digits(const char *s)
{
    for (; *s; s++)
        if (*s != '0')
            return 0;
    return 1;
}

int accudisc_scan_mcn(accudisc_device *dev, uint32_t lba, char mcn[14])
{
    accudisc_q hit;
    int rc;

    if (!dev || !mcn)
        return ACCUDISC_ERR_INVAL;
    rc = scan_adr(dev, lba, ACCUDISC_Q_MCN, &hit);
    if (rc != ACCUDISC_OK)
        return rc;
    if (all_zero_digits(hit.mcn))
        return ACCUDISC_ERR_NOTFOUND;
    for (unsigned i = 0; i < 14; i++)
        mcn[i] = hit.mcn[i];
    return ACCUDISC_OK;
}

int accudisc_scan_isrc(accudisc_device *dev, uint32_t lba, char isrc[13])
{
    accudisc_q hit;
    int rc;

    if (!dev || !isrc)
        return ACCUDISC_ERR_INVAL;
    rc = scan_adr(dev, lba, ACCUDISC_Q_ISRC, &hit);
    if (rc != ACCUDISC_OK)
        return rc;
    if (all_zero_digits(hit.isrc))
        return ACCUDISC_ERR_NOTFOUND;
    for (unsigned i = 0; i < 13; i++)
        isrc[i] = hit.isrc[i];
    return ACCUDISC_OK;
}
