#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "mmc.h"
#include "../cdda/layout.h"

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
    /* Clamp to the largest EVEN allocation length: 0xffff would round up to
     * 0x10000 and truncate to 0 in the 16-bit CDB field — asking for nothing. */
    if (len > 0xfffe)
        len = 0xfffe;

    /* Ask for an even count (see adsc_alloc_even: odd => DID_ERROR), but keep
     * `len` as the TRUE response length the drive declared. Reporting the
     * padded figure would hand callers a trailing byte of drive buffer residue
     * that is not disc data and need not be reproducible across drives or
     * reads — measured by cdda2img on a 12-track disc: 169 real, 170 padded,
     * pad 0x2b. A raw dump compared by file size would then differ from an
     * identical one, so the pad must not escape this function. */
    uint32_t xfer = adsc_alloc_even(len);

    uint8_t *buf = malloc(xfer);
    if (!buf)
        return ACCUDISC_ERR_NOMEM;

    adsc_cdb_read_toc(cmd.cdb, format, time_bit, track, (uint16_t)xfer);
    cmd.buf = buf;
    cmd.buf_len = xfer;
    rc = adsc_dev_exec(dev, &cmd);
    if (rc != ACCUDISC_OK) {
        free(buf);
        return rc;
    }
    *out = buf;
    *out_len = len; /* true length, never the padded transfer */
    return ACCUDISC_OK;
}

static int read_cd_once(struct accudisc_device *dev, uint32_t lba,
                        uint32_t nsec, unsigned sector_type, unsigned c2,
                        unsigned sub, uint8_t *dst, uint32_t exact,
                        uint32_t xfer)
{
    adsc_cmd cmd = {0};

    adsc_cdb_read_cd(cmd.cdb, lba, nsec, sector_type, c2, sub);
    cmd.cdb_len = 12;
    cmd.dir = ADSC_XFER_IN;
    cmd.buf = dst;
    cmd.buf_len = xfer;
    cmd.timeout_ms = ADSC_TIMEOUT_READ_MS;
    /* READ CD is exact-length: a GOOD-status short transfer leaves stale bytes
     * in the buffer tail. Promote it to ACCUDISC_ERR_SHORT so callers fall
     * back to the per-sector path (or fail the sector) instead of streaming
     * stale contents as valid audio. The rounding slack is not data. */
    return adsc_exec_check_short(adsc_dev_exec(dev, &cmd), cmd.resid,
                                 xfer - exact);
}

/* Put a combined C2 + raw P-W buffer into the standard's record order, or
 * refuse it. See src/cdda/layout.h for what the evidence is and why. */
static int read_cd_layout(struct accudisc_device *dev, unsigned c2,
                          uint8_t *buf, uint32_t nsec, uint32_t sector_len)
{
    uint32_t c2_len = sector_len - ACCUDISC_BYTES_AUDIO - ACCUDISC_BYTES_SUB_RAW;
    adsc_layout_verdict v;

    adsc_layout_inspect(buf, nsec, sector_len, c2_len, &v);

    /* Every record after the first is displaced. The audio reads GOOD and the
     * drive reported nothing, so this is the only place it can be caught — and
     * a caller that falls back to single-sector reads (the engine does) gets
     * correct records, because one record cannot be displaced. */
    if (v.misframed) {
        snprintf(dev->last_io, sizeof(dev->last_io),
                 "READ CD delivered records at a %u-byte stride, not %u; "
                 "refused rather than sliced wrong", v.misframed, sector_len);
        adsc_dev_trace_note(dev, "%s", dev->last_io);
        return ACCUDISC_ERR_IO;
    }

    /* Latch only on more than one agreeing record: a CRC-16 collision in one
     * record is ~1 in 65 536, and a latched layout is what an evidence-free
     * buffer (damaged P-W) is sorted by later. One record still decides for
     * itself. */
    uint32_t votes = v.hits_mmc > v.hits_sub ? v.hits_mmc : v.hits_sub;

    if (v.layout && votes >= 2 && dev->layout[c2] != v.layout) {
        if (v.layout == ADSC_LAYOUT_SUB_FIRST || dev->layout[c2])
            adsc_dev_log(dev, "READ CD C2+subchannel: drive delivers %s; "
                              "records are returned in MMC order "
                              "(audio, C2, subchannel)",
                         v.layout == ADSC_LAYOUT_SUB_FIRST
                             ? "subchannel BEFORE C2 (not MMC order)"
                             : "MMC order");
        dev->layout[c2] = (uint8_t)v.layout;
    }

    int use = v.layout ? v.layout : dev->layout[c2];

    if (use == ADSC_LAYOUT_SUB_FIRST)
        adsc_layout_to_mmc(buf, nsec, sector_len, c2_len);
    return ACCUDISC_OK;
}

int adsc_mmc_read_cd(struct accudisc_device *dev, uint32_t lba, uint32_t nsec,
                     unsigned sector_type, unsigned c2, unsigned sub,
                     void *buf, uint32_t sector_len)
{
    if (sector_len != adsc_read_cd_sector_len(c2, sub) || nsec == 0 ||
        nsec > UINT32_MAX / sector_len)
        return ACCUDISC_ERR_INVAL;

    /* TRANSFER LENGTH IS ROUNDED UP TO A MULTIPLE OF 16 (0.39.0).
     *
     * libata's atapi_check_dma() refuses DMA for an ATAPI transfer whose byte
     * count is not a multiple of 16 ("Quite a few ATAPI devices choke on such
     * DMA requests") and sends it by PIO instead. On PIO a LITE-ON LH-20A1S
     * pads every READ CD record to 16 bytes — 2742 -> 2752, 2646 -> 2656 — with
     * GOOD status, so every record after the first lands 10 bytes further on
     * and the tail is truncated. A whole Tracy Chapman rip verified 0/11 that
     * way. Measured 2026-09-14: 16- and 8-sector C2+sub reads (multiples of 16)
     * clean on fresh ranges, 15- and 23-sector reads displaced; and the same 23,
     * 15, 3 and 1-sector reads with the transfer rounded up came back correct,
     * GOOD, resid 0. Audio alone (2352 = 16 x 147) was never affected.
     *
     * Rounding the TRANSFER, not choosing sector counts, is what covers every
     * READ CD at once: sized chunks, the short last chunk, one-sector rescue
     * re-reads, probes, and any caller's chunk_sectors.
     *
     * The rounded transfer never touches the caller's buffer. The kernel hands
     * back all of dxfer_len (measured: a sentinel-filled buffer came back
     * overwritten to the end), and callers allocate exactly nsec * sector_len. */
    uint32_t exact = nsec * sector_len;
    uint32_t xfer = exact;
    uint8_t *dst = buf;

    /* xfer_exact: 0 = untested, -1 = a rounded read has succeeded, 1 = the
     * transport refused rounding on this handle. */
    if (dev->xfer_exact != 1 && (exact & 15u) && exact <= UINT32_MAX - 15u) {
        uint32_t want = (exact + 15u) & ~15u;

        if (dev->xfer_bounce_cap < want) {
            uint8_t *nb = realloc(dev->xfer_bounce, want);

            if (nb) {
                dev->xfer_bounce = nb;
                dev->xfer_bounce_cap = want;
            }
        }
        /* No bounce buffer: read exactly, as before 0.39.0. It is a lost
         * optimisation on most hosts and a misframing the layout check below
         * will refuse on this one — never a silent overrun. */
        if (dev->xfer_bounce_cap >= want) {
            xfer = want;
            dst = dev->xfer_bounce;
        }
    }

    int rc = read_cd_once(dev, lba, nsec, sector_type, c2, sub, dst, exact, xfer);

    /* A transport that rejects a transfer larger than the data (none measured)
     * gets the exact length back, permanently for this handle. Only until a
     * rounded read has once succeeded: after that an ERR_IO is the disc or the
     * drive, and a second attempt would only double a timeout. */
    if (rc == ACCUDISC_ERR_IO && xfer != exact && dev->xfer_exact == 0) {
        int rc2 = read_cd_once(dev, lba, nsec, sector_type, c2, sub, buf,
                               exact, exact);
        if (rc2 == ACCUDISC_OK) {
            dev->xfer_exact = 1;
            adsc_dev_log(dev, "READ CD: a transfer rounded to 16 bytes failed "
                              "where the exact length succeeded; using exact "
                              "lengths on this device");
        }
        rc = rc2;
        dst = buf;
        xfer = exact;
    }
    if (rc != ACCUDISC_OK)
        return rc;
    if (xfer != exact) {
        memcpy(buf, dst, exact);
        dev->xfer_exact = -1; /* proven: never fall back on this handle */
    }

    if (c2 != ADSC_C2_NONE && sub == ADSC_SUB_RAW)
        return read_cd_layout(dev, c2, buf, nsec, sector_len);
    return ACCUDISC_OK;
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
    /* Same hazard as the full TOC: a mode-page length is drive data and may be
     * odd. Round before it becomes a transfer, then re-clamp to the buffer. */
    total = adsc_alloc_even(total);
    if (total > cap)
        total = cap & ~1u; /* clamp WITHOUT undoing the rounding: an odd cap
                            * would otherwise hand an odd length straight back,
                            * and *len feeds mode_select10's OUT transfer too */

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

    /* nblocks > 0xFFFF would truncate into the CDB's 16-bit transfer length
     * while cmd.buf_len below kept the untruncated size: the drive would be
     * told "zero blocks" and handed a multi-megabyte buffer, both well-formed,
     * neither rejectable downstream. burn.c's CHUNK is 27 so this is not
     * reachable today; reject it rather than leave it to stay unreachable. */
    if (nblocks == 0 || nblocks > 0xFFFFu || block_bytes == 0)
        return ACCUDISC_ERR_INVAL;

    adsc_cdb_write10(cmd.cdb, (uint32_t)lba, (uint16_t)nblocks);
    cmd.cdb_len = 10;
    cmd.dir = ADSC_XFER_OUT;
    cmd.buf = (void *)buf;      /* OUT: exec reads, does not modify */
    cmd.buf_len = nblocks * block_bytes;
    cmd.timeout_ms = ADSC_TIMEOUT_WRITE_MS;
    return adsc_dev_exec(dev, &cmd);
}

int adsc_mmc_read_buffer_capacity(struct accudisc_device *dev,
                                  uint32_t *total, uint32_t *blank)
{
    adsc_cmd cmd = {0};
    uint8_t buf[12] = {0};
    int rc;

    if (!total || !blank)
        return ACCUDISC_ERR_INVAL;

    adsc_cdb_read_buffer_capacity(cmd.cdb);
    cmd.cdb_len = 10;
    cmd.dir = ADSC_XFER_IN;
    cmd.buf = buf;
    cmd.buf_len = sizeof buf;
    cmd.timeout_ms = ADSC_TIMEOUT_CTRL_MS;

    rc = adsc_dev_exec(dev, &cmd);
    if (rc != ACCUDISC_OK)
        return rc;

    /* Data Length is 16 bits at bytes 0-1; bytes 2-3 are RESERVED (MMC-5
     * Table 353). Reading 0-3 as one 32-bit field is an easy mistake that
     * happens to look right — 0x000A0000 for a valid answer — because the
     * reserved bytes are zero. It is wrong, and the first probe here made it. */
    if (((unsigned)buf[0] << 8 | buf[1]) < 10)
        return ACCUDISC_ERR_SHORT;

    *total = (uint32_t)buf[4] << 24 | (uint32_t)buf[5] << 16 |
             (uint32_t)buf[6] << 8  | buf[7];
    *blank = (uint32_t)buf[8] << 24 | (uint32_t)buf[9] << 16 |
             (uint32_t)buf[10] << 8 | buf[11];
    /* A zero-capacity buffer is not a full one. The caller must read this as
     * "unknown"; returning OK with total 0 and letting it divide would report
     * a starving drive forever. */
    if (*total == 0)
        return ACCUDISC_ERR_SHORT;
    if (*blank > *total)
        return ACCUDISC_ERR_SHORT;   /* free space exceeding the buffer */
    return ACCUDISC_OK;
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

int adsc_mmc_set_streaming(struct accudisc_device *dev, unsigned speed_x,
                           uint32_t start_lba, uint32_t end_lba, unsigned exact,
                           uint32_t write_kbps)
{
    uint8_t desc[28];
    adsc_cmd cmd = {0};

    adsc_cdb_set_streaming_desc(desc, speed_x, start_lba, end_lba, exact,
                                write_kbps);
    adsc_cdb_set_streaming(cmd.cdb, (uint16_t)sizeof(desc));
    cmd.cdb_len = 12;
    cmd.dir = ADSC_XFER_OUT;
    cmd.buf = desc;
    cmd.buf_len = sizeof(desc);
    cmd.timeout_ms = ADSC_TIMEOUT_CTRL_MS;
    return adsc_dev_exec(dev, &cmd);
}

int adsc_mmc_set_cd_speed(struct accudisc_device *dev, uint16_t read_kbps,
                          uint16_t write_kbps, unsigned rotctl)
{
    adsc_cmd cmd = {0};

    adsc_cdb_set_cd_speed(cmd.cdb, read_kbps, write_kbps, rotctl);
    cmd.cdb_len = 12;
    cmd.dir = ADSC_XFER_NONE;
    cmd.timeout_ms = ADSC_TIMEOUT_CTRL_MS;
    return adsc_dev_exec(dev, &cmd);
}

int adsc_mmc_get_performance(struct accudisc_device *dev, uint32_t start_lba,
                             uint16_t max_desc, uint8_t *buf, uint32_t cap,
                             uint32_t *len)
{
    adsc_cmd cmd = {0};
    int rc;

    if (cap < 8)
        return ACCUDISC_ERR_INVAL;

    adsc_cdb_get_performance(cmd.cdb, start_lba, max_desc,
                             ADSC_PERF_TYPE_NOMINAL);
    cmd.cdb_len = 12;
    cmd.dir = ADSC_XFER_IN;
    cmd.buf = buf;
    cmd.buf_len = cap;
    cmd.timeout_ms = ADSC_TIMEOUT_CTRL_MS;

    rc = adsc_dev_exec(dev, &cmd);
    if (rc != ACCUDISC_OK)
        return rc;

    /* Bytes 0-3 = Performance Data Length: the count of bytes that FOLLOW the
     * length field, so the whole response is that + 4. Clamp to what we asked
     * for (the drive may report more than the buffer holds). */
    uint32_t total = ((uint32_t)buf[0] << 24 | (uint32_t)buf[1] << 16 |
                      (uint32_t)buf[2] << 8 | buf[3]) + 4;
    *len = total < cap ? total : cap;
    return ACCUDISC_OK;
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

int adsc_mmc_test_unit_ready(struct accudisc_device *dev, adsc_cmd *cmd)
{
    adsc_cmd local = {0};
    adsc_cmd *c = cmd ? cmd : &local;

    memset(c, 0, sizeof(*c));
    c->cdb[0] = 0x00;
    c->cdb_len = 6;
    c->dir = ADSC_XFER_NONE;
    c->timeout_ms = ADSC_TIMEOUT_CTRL_MS;
    return adsc_dev_exec(dev, c);
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
