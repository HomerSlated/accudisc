/* SPDX-License-Identifier: MIT */
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

/* Bumped on any change to this file; the library refuses drivers built
 * against an ABI outside [ACCUDISC_DRIVER_ABI_MIN, ACCUDISC_DRIVER_ABI].
 *
 * ABI 6 (0.46.0) appended settings_get and changed nothing before it, so an
 * ABI-4 driver's descriptor is a valid PREFIX of an ABI-6 one: the library
 * accepts it and simply never reads the slot it does not have. That matters
 * in practice — a development build with no ACCUDISC_DRIVER_DIR falls back to
 * the INSTALLED driver directory, and refusing an older installed driver
 * would drop the device to generic MMC with nothing but a log line to say so.
 * A change that is not a pure append must raise ACCUDISC_DRIVER_ABI_MIN. */
#define ACCUDISC_DRIVER_ABI 6
/* ABI 5 IS SKIPPED, never to be reused. It existed only in an uncommitted
 * local install on 2026-09-28, whose settings_get lacked the `disc` argument.
 * A library that called that driver with today's signature would hand it the
 * probe struct as its output array — silent stack corruption, not an error. So
 * the slot is read only from abi >= 6, and a stray ABI-5 driver is treated
 * like an ABI-4 one: attached, with no settings report. */
#define ACCUDISC_DRIVER_ABI_MIN 4

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

    /* Capability: lift the drive's CD read-speed cap (e.g. Plextor
     * SpeedRead). Firmware limits read speed on some media; where the vendor
     * offers an override, this toggles it. The setting is drive state and
     * persists until changed or the drive is power-cycled — set() must
     * verify the change took. Speed itself is still commanded through the
     * generic MMC path (accudisc_set_speed); this only raises the ceiling. */
    int (*speed_uncap_get)(const accudisc_host *host, int *on);
    int (*speed_uncap_set)(const accudisc_host *host, int on);

    /* Capability: the drive's automatic WRITE-speed governor (Plextor
     * POWEREC). Appended in ABI 3.
     *
     * Distinct from speed_uncap above in both direction and kind: that one
     * raises a READ ceiling, this one decides whether the DRIVE overrides the
     * write speed the host asked for. With it on, the drive picks a rate from
     * its own running assessment of the medium; the host's request becomes an
     * upper bound at best.
     *
     * `recommended_kbps` (get, may be NULL) is the rate the governor currently
     * recommends, in kB/s — READ-ONLY STATUS, and it is what the drive intends
     * rather than what it will achieve. Measured on a PX-716A 2026-08-28: with
     * POWEREC on it recommended 48x and cdrecord's dummy run then delivered
     * 25x. Never present it as a rate.
     *
     * set() must verify by re-reading, like speed_uncap_set. The setting is
     * persistent drive state.
     *
     * NOT wired into the burn path. cdrecord turns POWEREC off when it is
     * forcing a speed (drv_mmc.c speed_select_mmc), and whether we should do
     * the same is an open question that needs a live burn to answer — see
     * docs/reference/LIVE_BURN_QUEUE.md. Until then this is a probe and an
     * explicit caller action, never something a burn does behind the caller. */
    int (*write_governor_get)(const accudisc_host *host, int *on,
                              uint32_t *recommended_kbps);
    int (*write_governor_set)(const accudisc_host *host, int on);

    /* Capability: report the drive's persisted vendor settings and life
     * counters (see accudisc_vendor_settings). Appended in ABI 6 (5 is skipped,
     * see above); the library reads it only from a driver declaring abi >= 6.
     *
     * STRICTLY READ-ONLY: GETs only. Fill out[0..cap), set *n to the total
     * produced (may exceed cap). A failed query is an entry without
     * ACCUDISC_VSET_OK, not a failed call; return non-OK only when nothing
     * could be asked at all. `id` is the identified drive, for model-gated
     * layouts (e.g. where a counter lives in EEPROM). `disc` is the library's
     * probe of what is loaded, or NULL when it could not be obtained: values
     * that describe the loaded disc are reported ACCUDISC_VSET_NO_DISC when
     * its reason is ACCUDISC_DISC_WHY_NO_MEDIUM, and the driver may use its
     * profile to pick units (CD vs DVD speed factors). */
    int (*settings_get)(const accudisc_host *host, const accudisc_drive_id *id,
                        const accudisc_disc_probe *disc,
                        accudisc_vendor_setting *out, uint32_t cap,
                        uint32_t *n);
} accudisc_driver;

/* The symbol every driver .so must export. */
#define ACCUDISC_DRIVER_ENTRY_SYMBOL "accudisc_driver_entry"
typedef const accudisc_driver *(*accudisc_driver_entry_fn)(void);

#ifdef __cplusplus
}
#endif

#endif /* ACCUDISC_DRIVER_H */
