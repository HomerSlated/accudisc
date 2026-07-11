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
    return adsc_transport_select_speed(&dev->t, speed_x);
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

int accudisc_spindle_stop(accudisc_device *dev)
{
    if (!dev)
        return ACCUDISC_ERR_INVAL;
    return adsc_mmc_start_stop(dev, 0, 0);
}
