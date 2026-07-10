/* Internal shared definitions. Never installed, never included by bindings. */

#ifndef ADSC_INTERNAL_H
#define ADSC_INTERNAL_H

#include <accudisc/accudisc.h>
#include <accudisc/driver.h>

#include "transport/transport.h"

struct accudisc_device {
    adsc_transport t;
    accudisc_sense last_sense;

    /* cached INQUIRY (needed for driver match and offset lookup) */
    accudisc_drive_id id;
    int id_valid;

    /* attached vendor driver (NULL = generic MMC/SG) */
    void *drv_handle; /* dlopen handle */
    const accudisc_driver *drv;
    accudisc_host host;
    char access[160]; /* accudisc_access_method buffer */

    void (*log_fn)(void *user, const char *msg);
    void *log_user;
};

/* Run a command on the device, recording decoded sense in the handle on any
 * failure (cleared on success). Returns ACCUDISC_OK / _ERR_IO / _ERR_SENSE. */
int adsc_dev_exec(struct accudisc_device *dev, adsc_cmd *cmd);

/* Identify once and cache (INQUIRY). */
int adsc_dev_identify(struct accudisc_device *dev);

void adsc_dev_log(struct accudisc_device *dev, const char *fmt, ...);

#endif /* ADSC_INTERNAL_H */
