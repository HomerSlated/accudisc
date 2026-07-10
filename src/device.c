/* Public device lifecycle and error surface. */

#include <stdlib.h>
#include <string.h>

#include "internal.h"
#include "mmc/mmc.h"

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

int accudisc_drive_identify(accudisc_device *dev, accudisc_drive_id *out)
{
    if (!dev || !out)
        return ACCUDISC_ERR_INVAL;
    return adsc_mmc_inquiry(dev, out);
}
