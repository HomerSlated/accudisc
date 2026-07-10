/* accudisc.h — public API for libaccudisc.
 *
 * This header is the ABI contract: the CLI and all language bindings are
 * built against it exclusively. Keep it C-only, self-contained, and free of
 * internal types.
 */

#ifndef ACCUDISC_H
#define ACCUDISC_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define ACCUDISC_VERSION_MAJOR 0
#define ACCUDISC_VERSION_MINOR 0
#define ACCUDISC_VERSION_PATCH 1

#if defined(_WIN32)
#  define ACCUDISC_API __declspec(dllexport)
#else
#  define ACCUDISC_API __attribute__((visibility("default")))
#endif

/* ---- version ------------------------------------------------------------- */

/* Version of the library actually linked (compare against the macros
 * above to detect header/library skew). */
ACCUDISC_API const char *accudisc_version_string(void);
ACCUDISC_API void accudisc_version(int *major, int *minor, int *patch);

/* ---- CD-DA sizes ----------------------------------------------------------
 * One CD-DA sector = 1/75 s of audio = 2352 bytes. The optional per-sector
 * companions a drive can return alongside it in the same READ CD transfer: */
#define ACCUDISC_BYTES_AUDIO   2352 /* raw s16le PCM user data */
#define ACCUDISC_BYTES_C2      294  /* C2 pointers: 2352 bits, 1/byte, MSB-first */
#define ACCUDISC_BYTES_C2_BEB  296  /* C2 + block-error-bits variant */
#define ACCUDISC_BYTES_SUB_RAW 96   /* raw P-W subcode, interleaved */
#define ACCUDISC_BYTES_SUB_Q   16   /* formatted Q subchannel block */

/* ---- errors ---------------------------------------------------------------
 * All fallible functions return ACCUDISC_OK (0) or a negative accudisc_err.
 * ACCUDISC_ERR_SENSE means the drive itself rejected the command
 * (CHECK CONDITION) — the decoded sense is available via
 * accudisc_last_sense() on the device the call was made against. */
typedef enum accudisc_err {
    ACCUDISC_OK              = 0,
    ACCUDISC_ERR_INVAL       = -1, /* invalid argument */
    ACCUDISC_ERR_NOMEM       = -2, /* allocation failure */
    ACCUDISC_ERR_OPEN        = -3, /* device could not be opened */
    ACCUDISC_ERR_IO          = -4, /* transport/host/driver failure */
    ACCUDISC_ERR_SENSE       = -5, /* drive returned CHECK CONDITION */
    ACCUDISC_ERR_SHORT       = -6, /* response shorter than required */
    ACCUDISC_ERR_UNSUPPORTED = -7  /* not supported by drive or build */
} accudisc_err;

/* Static human-readable name for an accudisc_err value. */
ACCUDISC_API const char *accudisc_strerror(int err);

/* Decoded SCSI sense from the most recent failed command on a device.
 * valid is 0 when the failure produced no usable sense data. */
typedef struct accudisc_sense {
    uint8_t valid;
    uint8_t key;  /* sense key, e.g. 3 = MEDIUM ERROR, 4 = HARDWARE ERROR */
    uint8_t asc;  /* additional sense code */
    uint8_t ascq; /* additional sense code qualifier */
} accudisc_sense;

/* ---- device ---------------------------------------------------------------
 * A handle to one optical drive. Handles are not thread-safe; use one per
 * thread or serialize externally. */
typedef struct accudisc_device accudisc_device;

/* Open read-write. Required for vendor opcodes, MODE SELECT, and writing:
 * the kernel's unprivileged SG_IO command filter blocks those on read-only
 * fds. Plain reading should not set this (least privilege). */
#define ACCUDISC_OPEN_RDWR 0x1u

/* Open the drive at path (e.g. "/dev/sr0"). Returns NULL on failure; if err
 * is non-NULL it receives the accudisc_err cause. */
ACCUDISC_API accudisc_device *accudisc_open(const char *path, unsigned flags,
                                            int *err);
ACCUDISC_API void accudisc_close(accudisc_device *dev);

/* Sense from the most recent ACCUDISC_ERR_SENSE/_IO failure on dev. */
ACCUDISC_API void accudisc_last_sense(const accudisc_device *dev,
                                      accudisc_sense *out);

/* INQUIRY identification strings, space-trimmed and NUL-terminated. */
typedef struct accudisc_drive_id {
    char vendor[9];
    char product[17];
    char revision[5];
} accudisc_drive_id;

ACCUDISC_API int accudisc_drive_identify(accudisc_device *dev,
                                         accudisc_drive_id *out);

#ifdef __cplusplus
}
#endif

#endif /* ACCUDISC_H */
