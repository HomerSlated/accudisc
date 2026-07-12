#include <stdlib.h>
#include <string.h>

#include "mmc.h"

/* Copy a fixed-width INQUIRY string field, right-trimming spaces. */
static void copy_trim(char *dst, const uint8_t *src, unsigned n)
{
    while (n > 0 && (src[n - 1] == ' ' || src[n - 1] == '\0'))
        n--;
    memcpy(dst, src, n);
    dst[n] = '\0';
}

int adsc_mmc_inquiry(struct accudisc_device *dev, accudisc_drive_id *out)
{
    uint8_t buf[36] = {0};
    adsc_cmd cmd = {0};
    int rc;

    adsc_cdb_inquiry(cmd.cdb, sizeof(buf));
    cmd.cdb_len = 6;
    cmd.dir = ADSC_XFER_IN;
    cmd.buf = buf;
    cmd.buf_len = sizeof(buf);
    cmd.timeout_ms = ADSC_TIMEOUT_CTRL_MS;

    rc = adsc_dev_exec(dev, &cmd);
    if (rc != ACCUDISC_OK)
        return rc;

    copy_trim(out->vendor, buf + 8, 8);
    copy_trim(out->product, buf + 16, 16);
    copy_trim(out->revision, buf + 32, 4);
    return ACCUDISC_OK;
}

int adsc_mmc_read_toc_raw(struct accudisc_device *dev, unsigned format,
                          unsigned time_bit, unsigned track,
                          uint8_t **out, uint32_t *out_len)
{
    uint8_t hdr[4] = {0};
    adsc_cmd cmd = {0};
    int rc;

    adsc_cdb_read_toc(cmd.cdb, format, time_bit, track, sizeof(hdr));
    cmd.cdb_len = 10;
    cmd.dir = ADSC_XFER_IN;
    cmd.buf = hdr;
    cmd.buf_len = sizeof(hdr);
    cmd.timeout_ms = ADSC_TIMEOUT_CTRL_MS;

    rc = adsc_dev_exec(dev, &cmd);
    if (rc != ACCUDISC_OK)
        return rc;

    uint32_t len = (uint32_t)(((unsigned)hdr[0] << 8) | hdr[1]) + 2;
    if (len <= 4) /* header only: the drive answered, the disc simply has
                   * no data of this format (e.g. no CD-Text) */
        return ACCUDISC_ERR_NOTFOUND;
    if (len > 0xffff)
        len = 0xffff;

    uint8_t *buf = malloc(len);
    if (!buf)
        return ACCUDISC_ERR_NOMEM;

    adsc_cdb_read_toc(cmd.cdb, format, time_bit, track, (uint16_t)len);
    cmd.buf = buf;
    cmd.buf_len = len;
    rc = adsc_dev_exec(dev, &cmd);
    if (rc != ACCUDISC_OK) {
        free(buf);
        return rc;
    }
    *out = buf;
    *out_len = len;
    return ACCUDISC_OK;
}

int adsc_mmc_read_cd(struct accudisc_device *dev, uint32_t lba, uint32_t nsec,
                     unsigned sector_type, unsigned c2, unsigned sub,
                     void *buf, uint32_t sector_len)
{
    adsc_cmd cmd = {0};

    if (sector_len != adsc_read_cd_sector_len(c2, sub))
        return ACCUDISC_ERR_INVAL;

    adsc_cdb_read_cd(cmd.cdb, lba, nsec, sector_type, c2, sub);
    cmd.cdb_len = 12;
    cmd.dir = ADSC_XFER_IN;
    cmd.buf = buf;
    cmd.buf_len = nsec * sector_len;
    cmd.timeout_ms = ADSC_TIMEOUT_READ_MS;
    return adsc_dev_exec(dev, &cmd);
}

int adsc_mmc_mode_sense10(struct accudisc_device *dev, unsigned page,
                          uint8_t *buf, uint32_t cap,
                          uint32_t *len, uint32_t *page_off)
{
    adsc_cmd cmd = {0};
    int rc;

    if (cap < 16)
        return ACCUDISC_ERR_INVAL;

    adsc_cdb_mode_sense10(cmd.cdb, page, 8);
    cmd.cdb_len = 10;
    cmd.dir = ADSC_XFER_IN;
    cmd.buf = buf;
    cmd.buf_len = 8;
    cmd.timeout_ms = ADSC_TIMEOUT_CTRL_MS;

    rc = adsc_dev_exec(dev, &cmd);
    if (rc != ACCUDISC_OK)
        return rc;

    uint32_t total = (uint32_t)(((unsigned)buf[0] << 8) | buf[1]) + 2;
    if (total > cap)
        total = cap;

    adsc_cdb_mode_sense10(cmd.cdb, page, (uint16_t)total);
    cmd.buf_len = total;
    rc = adsc_dev_exec(dev, &cmd);
    if (rc != ACCUDISC_OK)
        return rc;

    uint32_t off = 8 + (uint32_t)(((unsigned)buf[6] << 8) | buf[7]);
    if (off + 2 > total)
        return ACCUDISC_ERR_SHORT;
    *len = total;
    *page_off = off;
    return ACCUDISC_OK;
}

int adsc_mmc_mode_select10(struct accudisc_device *dev, uint8_t *buf,
                           uint32_t len, uint32_t page_off)
{
    adsc_cmd cmd = {0};

    if (page_off >= len)
        return ACCUDISC_ERR_INVAL;

    buf[0] = buf[1] = 0;       /* mode data length: reserved on select */
    buf[page_off] &= 0x3f;     /* clear PS (and reserved bit 6) */

    adsc_cdb_mode_select10(cmd.cdb, (uint16_t)len);
    cmd.cdb_len = 10;
    cmd.dir = ADSC_XFER_OUT;
    cmd.buf = buf;
    cmd.buf_len = len;
    cmd.timeout_ms = ADSC_TIMEOUT_CTRL_MS;
    return adsc_dev_exec(dev, &cmd);
}

int adsc_mmc_write10(struct accudisc_device *dev, int32_t lba,
                     uint32_t nblocks, const void *buf, uint32_t block_bytes)
{
    adsc_cmd cmd = {0};

    if (nblocks == 0 || block_bytes == 0)
        return ACCUDISC_ERR_INVAL;

    adsc_cdb_write10(cmd.cdb, (uint32_t)lba, (uint16_t)nblocks);
    cmd.cdb_len = 10;
    cmd.dir = ADSC_XFER_OUT;
    cmd.buf = (void *)buf;      /* OUT: exec reads, does not modify */
    cmd.buf_len = nblocks * block_bytes;
    cmd.timeout_ms = ADSC_TIMEOUT_WRITE_MS;
    return adsc_dev_exec(dev, &cmd);
}

int adsc_mmc_sync_cache(struct accudisc_device *dev)
{
    adsc_cmd cmd = {0};

    adsc_cdb_sync_cache(cmd.cdb);
    cmd.cdb_len = 10;
    cmd.dir = ADSC_XFER_NONE;
    cmd.timeout_ms = ADSC_TIMEOUT_WRITE_MS;
    return adsc_dev_exec(dev, &cmd);
}

int adsc_mmc_send_cue_sheet(struct accudisc_device *dev, const uint8_t *cue,
                            uint32_t len)
{
    adsc_cmd cmd = {0};

    if (!cue || len == 0)
        return ACCUDISC_ERR_INVAL;

    adsc_cdb_send_cue(cmd.cdb, len);
    cmd.cdb_len = 10;
    cmd.dir = ADSC_XFER_OUT;
    cmd.buf = (void *)cue;
    cmd.buf_len = len;
    cmd.timeout_ms = ADSC_TIMEOUT_CTRL_MS;
    return adsc_dev_exec(dev, &cmd);
}

int adsc_mmc_send_opc(struct accudisc_device *dev)
{
    adsc_cmd cmd = {0};

    adsc_cdb_send_opc(cmd.cdb);
    cmd.cdb_len = 10;
    cmd.dir = ADSC_XFER_NONE;
    cmd.timeout_ms = ADSC_TIMEOUT_WRITE_MS;
    return adsc_dev_exec(dev, &cmd);
}

int adsc_mmc_read_disc_info(struct accudisc_device *dev, uint8_t *buf,
                            uint32_t cap, uint32_t *len)
{
    adsc_cmd cmd = {0};
    int rc;

    if (cap < 4)
        return ACCUDISC_ERR_INVAL;

    /* One pass: the standard disc-information block is 34 bytes; ask for what
     * the caller can hold and report what came back. */
    uint32_t want = cap > 34 ? 34 : cap;
    adsc_cdb_read_disc_info(cmd.cdb, (uint16_t)want);
    cmd.cdb_len = 10;
    cmd.dir = ADSC_XFER_IN;
    cmd.buf = buf;
    cmd.buf_len = want;
    cmd.timeout_ms = ADSC_TIMEOUT_CTRL_MS;

    rc = adsc_dev_exec(dev, &cmd);
    if (rc != ACCUDISC_OK)
        return rc;

    uint32_t total = (uint32_t)(((unsigned)buf[0] << 8) | buf[1]) + 2;
    *len = total < want ? total : want;
    return ACCUDISC_OK;
}

int adsc_mmc_get_configuration(struct accudisc_device *dev, uint16_t feature,
                               uint8_t *out, uint32_t cap)
{
    adsc_cmd cmd = {0};

    adsc_cdb_get_configuration(cmd.cdb, 0x02, feature, (uint16_t)cap);
    cmd.cdb_len = 10;
    cmd.dir = ADSC_XFER_IN;
    cmd.buf = out;
    cmd.buf_len = cap;
    cmd.timeout_ms = ADSC_TIMEOUT_CTRL_MS;
    return adsc_dev_exec(dev, &cmd);
}

int adsc_mmc_start_stop(struct accudisc_device *dev, unsigned start,
                        unsigned loej)
{
    adsc_cmd cmd = {0};

    adsc_cdb_start_stop(cmd.cdb, start, loej);
    cmd.cdb_len = 6;
    cmd.dir = ADSC_XFER_NONE;
    cmd.timeout_ms = ADSC_TIMEOUT_CTRL_MS;
    return adsc_dev_exec(dev, &cmd);
}
