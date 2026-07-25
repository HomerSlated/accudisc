/* Internal shared definitions. Never installed, never included by bindings. */

#ifndef ADSC_INTERNAL_H
#define ADSC_INTERNAL_H

#include <stddef.h> /* size_t — the public header deals only in stdint types */

#include <accudisc/accudisc.h>
#include <accudisc/driver.h>

#include "transport/transport.h"

struct accudisc_device {
    adsc_transport t;
    accudisc_sense last_sense;

    /* cached INQUIRY (needed for driver match and offset lookup) */
    accudisc_drive_id id;
    int id_valid;

    /* SET STREAMING speed control: 0 = untried, 1 = works, -1 = unusable
     * (unsupported/illegal/unprivileged) — fall back to CDROM_SELECT_SPEED. */
    int streaming;

    /* Vendor read-speed uncap, as set THROUGH THIS HANDLE: 0 = we never set it,
     * 1 = we set it on, -1 = we set it off. The uncap is persistent drive
     * state, so this is not the whole story — a prior session can have left it
     * on before we existed, which is what accudisc_speed_uncap_probe's page-2A
     * path is for. But when it is non-zero it is the one source that needs no
     * driver and no inference, so the probe consults it first. */
    int uncap_set;

    /* attached vendor driver (NULL = generic MMC/SG) */
    void *drv_handle; /* dlopen handle */
    const accudisc_driver *drv;
    accudisc_host host;
    char access[160]; /* accudisc_access_method buffer */

    void (*log_fn)(void *user, const char *msg);
    void *log_user;

    /* Cause of the most recent ACCUDISC_ERR_IO ("" if none yet). The companion
     * to last_sense: ERR_SENSE carries the drive's own explanation, ERR_IO
     * carries the transport's. */
    char last_io[96];
};

/* Run a command on the device, recording decoded sense in the handle on any
 * failure (cleared on success). Returns ACCUDISC_OK / _ERR_IO / _ERR_SENSE. */
int adsc_dev_exec(struct accudisc_device *dev, adsc_cmd *cmd);

/* Identify once and cache (INQUIRY). */
int adsc_dev_identify(struct accudisc_device *dev);

/* Collapse whitespace runs in an INQUIRY field to single spaces and trim the
 * ends, so table lookups match regardless of how a drive pads its fixed-width
 * fields ("DVDR   PX-716A" vs "DVDR PX-716A"). Always NUL-terminates. */
void adsc_inquiry_normalize(const char *src, char *dst, size_t cap);

/* The driver-free half of accudisc_speed_uncap_probe: given INQUIRY strings and
 * the drive's reported maximum read speed in Nx, decide whether the vendor
 * read-speed uncap looks enabled. Pure — no device, no I/O — so the whole table
 * is testable without hardware. See src/drive/uncap.c. */
accudisc_uncap_state adsc_uncap_classify(const char *vendor,
                                         const char *product, unsigned max_x);

/* The uncap's state from authoritative sources only (this handle set it, or an
 * attached driver says so) — ON, OFF or UNKNOWN, never LIKELY_ON. Costs no MODE
 * SENSE, so the read engine can consult it per read; the full probe cannot. */
accudisc_uncap_state adsc_uncap_authoritative(accudisc_device *dev);

void adsc_dev_log(struct accudisc_device *dev, const char *fmt, ...);

#endif /* ADSC_INTERNAL_H */
