/* Public device lifecycle and error surface. */

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "internal.h"
#include "mmc/mmc.h"

void adsc_dev_log(struct accudisc_device *dev, const char *fmt, ...)
{
    char msg[256];
    va_list ap;

    if (!dev->log_fn)
        return;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);
    dev->log_fn(dev->log_user, msg);
}

void accudisc_set_log(accudisc_device *dev,
                      void (*fn)(void *user, const char *msg), void *user)
{
    if (!dev)
        return;
    dev->log_fn = fn;
    dev->log_user = user;
}

/* ---- accudisc_host bridge (see accudisc/driver.h) ------------------------ */

static int host_exec(void *devp, const uint8_t *cdb, uint8_t cdb_len,
                     accudisc_host_dir dir, void *buf, uint32_t buf_len,
                     uint32_t timeout_ms)
{
    struct accudisc_device *dev = devp;
    adsc_cmd cmd = {0};

    if (!dev || !cdb || cdb_len > ADSC_CDB_MAX)
        return ACCUDISC_ERR_INVAL;
    memcpy(cmd.cdb, cdb, cdb_len);
    cmd.cdb_len = cdb_len;
    cmd.dir = dir == ACCUDISC_HOST_IN    ? ADSC_XFER_IN
              : dir == ACCUDISC_HOST_OUT ? ADSC_XFER_OUT
                                         : ADSC_XFER_NONE;
    cmd.buf = buf;
    cmd.buf_len = buf_len;
    cmd.timeout_ms = timeout_ms;
    return adsc_dev_exec(dev, &cmd);
}

static void host_log(void *devp, const char *msg)
{
    adsc_dev_log(devp, "%s", msg);
}

const char *accudisc_strerror(int err)
{
    switch (err) {
    case ACCUDISC_OK:              return "success";
    case ACCUDISC_ERR_INVAL:       return "invalid argument";
    case ACCUDISC_ERR_NOMEM:       return "out of memory";
    case ACCUDISC_ERR_OPEN:        return "cannot open device";
    case ACCUDISC_ERR_IO:          return "transport I/O failure";
    case ACCUDISC_ERR_SENSE:       return "drive rejected command (check sense)";
    case ACCUDISC_ERR_SHORT:       return "response too short";
    case ACCUDISC_ERR_UNSUPPORTED: return "not supported";
    case ACCUDISC_ERR_CANCELLED:   return "cancelled";
    case ACCUDISC_ERR_CRC:         return "checksum failed";
    case ACCUDISC_ERR_NOTFOUND:    return "data absent";
    default:                       return "unknown error";
    }
}

accudisc_device *accudisc_open(const char *path, unsigned flags, int *err)
{
    int e = ACCUDISC_OK;
    accudisc_device *dev = NULL;

    if (!path) {
        e = ACCUDISC_ERR_INVAL;
        goto fail;
    }
    dev = calloc(1, sizeof(*dev));
    if (!dev) {
        e = ACCUDISC_ERR_NOMEM;
        goto fail;
    }
    e = adsc_transport_open(&dev->t, path, (flags & ACCUDISC_OPEN_RDWR) != 0);
    if (e != ACCUDISC_OK)
        goto fail;
    dev->host.dev = dev;
    dev->host.exec = host_exec;
    dev->host.log = host_log;
    if (err)
        *err = ACCUDISC_OK;
    return dev;

fail:
    free(dev);
    if (err)
        *err = e;
    return NULL;
}

void accudisc_close(accudisc_device *dev)
{
    if (!dev)
        return;
    accudisc_driver_detach(dev);
    adsc_transport_close(&dev->t);
    free(dev);
}

void accudisc_last_sense(const accudisc_device *dev, accudisc_sense *out)
{
    if (!out)
        return;
    if (!dev) {
        memset(out, 0, sizeof(*out));
        return;
    }
    *out = dev->last_sense;
}

int adsc_dev_exec(struct accudisc_device *dev, adsc_cmd *cmd)
{
    int rc = adsc_transport_exec(&dev->t, cmd);

    if (rc == ACCUDISC_OK)
        memset(&dev->last_sense, 0, sizeof(dev->last_sense));
    else
        adsc_sense_decode(cmd->sense, cmd->sense_len, &dev->last_sense);
    return rc;
}

int adsc_dev_identify(struct accudisc_device *dev)
{
    int rc;

    if (dev->id_valid)
        return ACCUDISC_OK;
    rc = adsc_mmc_inquiry(dev, &dev->id);
    if (rc == ACCUDISC_OK)
        dev->id_valid = 1;
    return rc;
}

int accudisc_drive_identify(accudisc_device *dev, accudisc_drive_id *out)
{
    int rc;

    if (!dev || !out)
        return ACCUDISC_ERR_INVAL;
    rc = adsc_dev_identify(dev);
    if (rc == ACCUDISC_OK)
        *out = dev->id;
    return rc;
}

void accudisc_free(void *p)
{
    free(p);
}

int accudisc_set_speed(accudisc_device *dev, unsigned speed_x)
{
    if (!dev)
        return ACCUDISC_ERR_INVAL;

    /* Prefer SET STREAMING (0xB6): a performance-descriptor ceiling the drive
     * actually enforces. SET CD SPEED / CDROM_SELECT_SPEED is advisory on
     * CAV Plextors — kept as the fallback. Once streaming proves unusable on
     * this handle (unsupported opcode, or blocked because we lack
     * CAP_SYS_RAWIO), stop retrying it and use the block-layer path. */
    if (dev->streaming >= 0) {
        int rc = adsc_mmc_set_streaming(dev, speed_x, 0, 0xFFFFFFFFu, 0);
        if (rc == ACCUDISC_OK) {
            dev->streaming = 1;
            return ACCUDISC_OK;
        }
        /* Illegal request (bad opcode/param) or any hard IO/permission
         * failure means streaming will not work this run — latch it off.
         * A transient medium/not-ready sense should not, so fall through
         * to the block layer for this call without latching. (No CAP_SYS_RAWIO
         * surfaces as ERR_IO here: the kernel's SG filter blocks the data-OUT
         * before it reaches the drive.) */
        if (rc == ACCUDISC_ERR_IO ||
            (dev->last_sense.valid && dev->last_sense.key == 0x05)) {
            dev->streaming = -1;
            adsc_dev_log(dev, "set-speed: SET STREAMING unusable, "
                              "falling back to CDROM_SELECT_SPEED");
        }
    }
    return adsc_transport_select_speed(&dev->t, speed_x);
}

int accudisc_set_speed_range(accudisc_device *dev, unsigned speed_x,
                             int32_t start_lba, int32_t end_lba, unsigned flags)
{
    if (!dev)
        return ACCUDISC_ERR_INVAL;

    /* Ranged and Exact ceilings are SET STREAMING's alone — SET CD SPEED does
     * whole-disc CAV only — so there is no block-layer fallback here. A drive
     * that lacks SET STREAMING (or the handle that lacks CAP_SYS_RAWIO) gets
     * the command's error back for the caller to report, not a silent downgrade
     * that would ignore the requested range. */
    unsigned exact = (flags & ACCUDISC_SPEED_EXACT) ? 1u : 0u;
    return adsc_mmc_set_streaming(dev, speed_x, (uint32_t)start_lba,
                                  (uint32_t)end_lba, exact);
}

/* Page 2A read speeds, the fields cdrdao drive-info reports (max at page
 * offset 8, current at 14, kB/s). The "page 2A lies" folklore is naive
 * readers using the wrong offsets. */
int accudisc_get_speed(accudisc_device *dev, unsigned *max_kbps,
                       unsigned *cur_kbps)
{
    uint8_t buf[256];
    uint32_t len = 0, off = 0;
    int rc;

    if (!dev)
        return ACCUDISC_ERR_INVAL;
    rc = adsc_mmc_mode_sense10(dev, 0x2a, buf, sizeof(buf), &len, &off);
    if (rc != ACCUDISC_OK)
        return rc;
    if ((buf[off] & 0x3f) != 0x2a || off + 16 > len)
        return ACCUDISC_ERR_SHORT;
    if (max_kbps)
        *max_kbps = ((unsigned)buf[off + 8] << 8) | buf[off + 9];
    if (cur_kbps)
        *cur_kbps = ((unsigned)buf[off + 14] << 8) | buf[off + 15];
    return ACCUDISC_OK;
}

int accudisc_get_performance(accudisc_device *dev, accudisc_perf_desc *out,
                             uint32_t max_out, uint32_t *count)
{
    /* One descriptor per medium zone; 16 covers any real CD/DVD curve (the
     * PX-716A reports 1, a zoned drive a handful). */
    enum { MAX_DESC = 16 };
    uint8_t buf[8 + MAX_DESC * 16];
    uint32_t len = 0, n;
    int rc;

    if (!dev || !out || !count)
        return ACCUDISC_ERR_INVAL;
    *count = 0;

    uint16_t want = max_out < MAX_DESC ? (uint16_t)max_out : MAX_DESC;
    rc = adsc_mmc_get_performance(dev, 0, want, buf, sizeof(buf), &len);
    if (rc != ACCUDISC_OK)
        return rc;
    if (len < 8)
        return ACCUDISC_ERR_SHORT;

    n = (len - 8) / 16;
    if (n > max_out)
        n = max_out;
    for (uint32_t i = 0; i < n; i++) {
        const uint8_t *d = buf + 8 + i * 16;
        out[i].start_lba  = (uint32_t)d[0]  << 24 | (uint32_t)d[1]  << 16 |
                            (uint32_t)d[2]  << 8  | d[3];
        out[i].start_kbps = (uint32_t)d[4]  << 24 | (uint32_t)d[5]  << 16 |
                            (uint32_t)d[6]  << 8  | d[7];
        out[i].end_lba    = (uint32_t)d[8]  << 24 | (uint32_t)d[9]  << 16 |
                            (uint32_t)d[10] << 8  | d[11];
        out[i].end_kbps   = (uint32_t)d[12] << 24 | (uint32_t)d[13] << 16 |
                            (uint32_t)d[14] << 8  | d[15];
    }
    *count = n;
    return ACCUDISC_OK;
}

accudisc_rotation accudisc_classify_rotation(const accudisc_perf_desc *desc,
                                             uint32_t count)
{
    /* Below one CD-1x (176 kB/s), treat a rate difference as flat — drives
     * report tiny non-monotonic jitter that is not a real slope. */
    const uint32_t EPS = 176;

    if (!desc || count == 0)
        return ACCUDISC_ROTATION_UNKNOWN;

    /* The discriminator is the slope WITHIN a segment. CAV rises inside a zone
     * (start < end of the same descriptor); zoned CLV is flat inside each zone
     * but steps to a new flat level at the next one. A step-up between segments
     * is therefore NOT a "rise" — it is exactly what makes a curve Z-CLV. */
    int any_rise = 0;
    uint32_t vmin = UINT32_MAX, vmax = 0;
    for (uint32_t i = 0; i < count; i++) {
        uint32_t s = desc[i].start_kbps, e = desc[i].end_kbps;
        if (e > s + EPS)
            any_rise = 1;
        for (int k = 0; k < 2; k++) {
            uint32_t v = k ? e : s;
            if (v < vmin) vmin = v;
            if (v > vmax) vmax = v;
        }
    }

    if (count == 1)
        return any_rise ? ACCUDISC_ROTATION_CAV : ACCUDISC_ROTATION_CLV;

    if (any_rise) {
        /* Rising somewhere. If the outermost segment has flattened, the drive
         * hit its speed cap and holds CLV past it: partial CAV. Still rising at
         * the rim: plain CAV. */
        uint32_t s = desc[count - 1].start_kbps, e = desc[count - 1].end_kbps;
        return (e > s + EPS) ? ACCUDISC_ROTATION_CAV : ACCUDISC_ROTATION_PCAV;
    }
    /* Every segment flat: differing levels across zones = zoned CLV; a single
     * level = plain CLV. */
    return (vmax - vmin > EPS) ? ACCUDISC_ROTATION_ZCLV : ACCUDISC_ROTATION_CLV;
}

int accudisc_spindle_stop(accudisc_device *dev)
{
    if (!dev)
        return ACCUDISC_ERR_INVAL;
    return adsc_mmc_start_stop(dev, 0, 0);
}

int accudisc_eject(accudisc_device *dev)
{
    if (!dev)
        return ACCUDISC_ERR_INVAL;
    /* Block-layer CDROMEJECT: unprivileged for cdrom-group members, unlike
     * MMC START STOP UNIT over SG_IO (which the kernel filter can gate on
     * CAP_SYS_RAWIO). */
    return adsc_transport_eject(&dev->t);
}

int accudisc_load(accudisc_device *dev)
{
    if (!dev)
        return ACCUDISC_ERR_INVAL;
    return adsc_transport_load(&dev->t);
}
