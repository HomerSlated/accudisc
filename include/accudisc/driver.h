/* driver.h — the AccuDisc vendor-driver SDK.
 *
 * libaccudisc itself is pure MMC/SG: no proprietary opcode is ever baked
 * into the core. Hardware-specific features live in external drivers —
 * shared objects named accudisc-drv-<name>.so — loaded at runtime only when
 * the calling application permits it, and only after the driver proves the
 * vendor path genuinely works on the attached drive (selftest).
 *
 * A driver never links against libaccudisc. It receives an accudisc_host —
 * callbacks for raw command execution and logging — and returns a static
 * accudisc_driver descriptor from its single exported entry point:
 *
 *     const accudisc_driver *accudisc_driver_entry(void);
 *
 * Attach order (enforced by the library):
 *   1. the drive is identified (INQUIRY);
 *   2. a driver is located (by explicit name, or by matching the ID);
 *   3. the calling application's permission is implied by the attach call
 *      itself — no attach, no vendor opcodes, ever;
 *   4. selftest() must demonstrate the opcode path works by reading,
 *      setting, and re-reading real device state (run once per attach —
 *      re-attach per command invocation to re-verify);
 *   5. on any failure the device silently remains generic MMC/SG.
 */

#ifndef ACCUDISC_DRIVER_H
#define ACCUDISC_DRIVER_H

#include <accudisc/accudisc.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Bumped on any incompatible change to this file; the library refuses
 * drivers built against a different ABI. */
#define ACCUDISC_DRIVER_ABI 1

typedef enum accudisc_host_dir {
    ACCUDISC_HOST_NONE = 0,
    ACCUDISC_HOST_IN   = 1, /* device -> host */
    ACCUDISC_HOST_OUT  = 2  /* host -> device */
} accudisc_host_dir;

/* The library-provided execution context. dev is opaque to the driver and
 * must be passed back verbatim. exec returns accudisc_err values; on
 * ACCUDISC_ERR_SENSE the decoded sense is available to the calling
 * application via accudisc_last_sense as usual. */
typedef struct accudisc_host {
    void *dev;
    int (*exec)(void *dev, const uint8_t *cdb, uint8_t cdb_len,
                accudisc_host_dir dir, void *buf, uint32_t buf_len,
                uint32_t timeout_ms);
    void (*log)(void *dev, const char *msg);
} accudisc_host;

/* The driver descriptor. Capability slots may be NULL (= not offered);
 * abi, name, match and selftest are mandatory. */
typedef struct accudisc_driver {
    uint32_t abi;            /* ACCUDISC_DRIVER_ABI */
    const char *name;        /* short id, e.g. "plextor" */
    const char *description; /* one line for access-method reporting */

    /* Does this driver support the identified drive? 1 = yes. */
    int (*match)(const accudisc_drive_id *id);

    /* Prove the vendor path works: read device state, change it, re-read
     * to confirm the change took, restore. ACCUDISC_OK = trustworthy. */
    int (*selftest)(const accudisc_host *host);

    /* Capability: hardware error-counter scan (e.g. Plextor Q-Check
     * C1/C2/CU). begin arms the drive's counters; read returns and resets
     * the interval counts; end disarms. */
    int (*counter_scan_begin)(const accudisc_host *host);
    int (*counter_scan_read)(const accudisc_host *host,
                             accudisc_counters *out);
    int (*counter_scan_end)(const accudisc_host *host);
} accudisc_driver;

/* The symbol every driver .so must export. */
#define ACCUDISC_DRIVER_ENTRY_SYMBOL "accudisc_driver_entry"
typedef const accudisc_driver *(*accudisc_driver_entry_fn)(void);

#ifdef __cplusplus
}
#endif

#endif /* ACCUDISC_DRIVER_H */
