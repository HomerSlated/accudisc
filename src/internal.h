/* Internal shared definitions. Never installed, never included by bindings. */

#ifndef ADSC_INTERNAL_H
#define ADSC_INTERNAL_H

#include <accudisc/accudisc.h>

#include "transport/transport.h"

struct accudisc_device {
    adsc_transport t;
    accudisc_sense last_sense;
};

/* Run a command on the device, recording decoded sense in the handle on any
 * failure (cleared on success). Returns ACCUDISC_OK / _ERR_IO / _ERR_SENSE. */
int adsc_dev_exec(struct accudisc_device *dev, adsc_cmd *cmd);

#endif /* ADSC_INTERNAL_H */
