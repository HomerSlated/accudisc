/* Feature probing: the drive's claims (GET CONFIGURATION) cross-checked with
 * functional smoke reads. Port of c2read probe_features(); the claim-vs-
 * function split exists because drives are known to advertise C2 they don't
 * honour, and to honour C2 they don't advertise. */

#include <stdlib.h>
#include <string.h>

#include "../mmc/mmc.h"

#define ADSC_FEATURE_CD_READ 0x001E

static int cd_read_feature(struct accudisc_device *dev, accudisc_features *f)
{
    uint8_t buf[64] = {0};

    if (adsc_mmc_get_configuration(dev, ADSC_FEATURE_CD_READ, buf,
                                   sizeof(buf)) != ACCUDISC_OK)
        return -1;
    /* 8-byte feature header, then the first feature descriptor. */
    unsigned code = ((unsigned)buf[8] << 8) | buf[9];
    if (code != ADSC_FEATURE_CD_READ)
        return -1;
    f->feature_present = 1;
    f->current = buf[10] & 0x01;
    f->dap = (buf[12] >> 7) & 1;
    f->c2_claimed = (buf[12] >> 1) & 1;
    f->cdtext_claimed = buf[12] & 1;
    return 0;
}

/* Does READ CD with this C2/sub combination return data (not CHECK
 * CONDITION)? Three CD-DA sectors from LBA 0. */
static int combo_smoke(struct accudisc_device *dev, unsigned c2, unsigned sub)
{
    uint32_t sector_len = adsc_read_cd_sector_len(c2, sub);
    uint8_t *buf = malloc((size_t)3 * sector_len);
    int rc;

    if (!buf)
        return 0;
    rc = adsc_mmc_read_cd(dev, 0, 3, ADSC_SECTOR_CDDA, c2, sub, buf,
                          sector_len);
    free(buf);
    return rc == ACCUDISC_OK;
}

/* Accurate Stream probe: read [lba, lba+12), then re-read from staggered
 * start points with cache defeat in between; on an Accurate Stream drive
 * the overlapping sectors are byte-identical regardless of where the read
 * began. Any positional mismatch = the drive can slip. */
#define AS_SPAN 12

int accudisc_probe_accurate_stream(accudisc_device *dev, uint32_t lba,
                                   uint8_t *accurate)
{
    static const uint32_t starts[] = {1, 5, 9};
    uint32_t sec = ACCUDISC_BYTES_AUDIO;
    uint8_t *base, *shifted;
    int rc = ACCUDISC_OK;

    if (!dev || !accurate)
        return ACCUDISC_ERR_INVAL;
    base = malloc((size_t)AS_SPAN * sec);
    shifted = malloc((size_t)AS_SPAN * sec);
    if (!base || !shifted) {
        rc = ACCUDISC_ERR_NOMEM;
        goto out;
    }

    rc = adsc_mmc_read_cd(dev, lba, AS_SPAN, ADSC_SECTOR_CDDA,
                          ADSC_C2_NONE, ADSC_SUB_NONE, base,
                          sec);
    if (rc != ACCUDISC_OK)
        goto out;

    *accurate = 1;
    for (size_t t = 0; t < sizeof(starts) / sizeof(starts[0]); t++) {
        uint32_t k = starts[t];

        /* Cache defeat: a far throwaway read so the staggered read hits
         * the platter, not the drive's buffer of the base read. */
        adsc_mmc_read_cd(dev, lba + 5000, 1, ADSC_SECTOR_CDDA,
                         ADSC_C2_NONE, ADSC_SUB_NONE, shifted, sec);

        rc = adsc_mmc_read_cd(dev, lba + k, AS_SPAN, ADSC_SECTOR_CDDA,
                              ADSC_C2_NONE, ADSC_SUB_NONE, shifted, sec);
        if (rc != ACCUDISC_OK)
            goto out;
        if (memcmp(base + (size_t)k * sec, shifted,
                   (size_t)(AS_SPAN - k) * sec) != 0) {
            *accurate = 0;
            break;
        }
    }

out:
    free(base);
    free(shifted);
    return rc;
}

int accudisc_probe_features(accudisc_device *dev, accudisc_features *out)
{
    if (!dev || !out)
        return ACCUDISC_ERR_INVAL;
    memset(out, 0, sizeof(*out));

    int have_feat = cd_read_feature(dev, out);

    out->ok_c2 = (uint8_t)combo_smoke(dev, ADSC_C2_294, ADSC_SUB_NONE);
    out->ok_sub_raw = (uint8_t)combo_smoke(dev, ADSC_C2_NONE, ADSC_SUB_RAW);
    out->ok_sub_q = (uint8_t)combo_smoke(dev, ADSC_C2_NONE, ADSC_SUB_Q);
    out->ok_c2_sub_raw = (uint8_t)combo_smoke(dev, ADSC_C2_294, ADSC_SUB_RAW);
    out->ok_c2_sub_q = (uint8_t)combo_smoke(dev, ADSC_C2_294, ADSC_SUB_Q);

    if (!out->ok_c2)
        out->c2_verdict = ACCUDISC_C2_UNSUPPORTED;
    else if (have_feat == 0 && out->c2_claimed)
        out->c2_verdict = ACCUDISC_C2_SUPPORTED;
    else
        out->c2_verdict = ACCUDISC_C2_UNVERIFIED;
    return ACCUDISC_OK;
}
