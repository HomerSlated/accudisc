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

    /* Non-zero suspends the NOT-READY cooldown in adsc_dev_exec. Set for the
     * duration of a burn and nowhere else.
     *
     * The reason is specific rather than precautionary. READ BUFFER CAPACITY
     * (0x5C) is on the repeatable allowlist AND is issued inside the write
     * loop; a 250 ms sleep there is a quarter-second in which the drive's
     * buffer is not being fed, which is the one thing a burn cannot afford. A
     * readiness probe that causes an underrun is worse than the bug it fixes.
     * Unit-attention retries are NOT suspended — they cost no time at all, and
     * a UA mid-burn is precisely when re-issuing the read matters. */
    int ready_wait_off;

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

    /* Per-command trace (0.36.0), from the open flags: ADSC_TRACE_OFF / _CTRL
     * / _DATA (src/trace.h). trace_seq numbers every command issued, traced or
     * not; trace_t0_ns is the open time the "+T" column counts from. */
    int trace;
    uint32_t trace_seq;
    uint64_t trace_t0_ns;

    /* Cause of the most recent ACCUDISC_ERR_IO ("" if none yet). The companion
     * to last_sense: ERR_SENSE carries the drive's own explanation, ERR_IO
     * carries the transport's.
     *
     * 96 held every adsc_io_detail() line, which is a field dump. Widened to
     * 256 in 0.37.0 for the eject verification, whose whole value is a
     * sentence a user can act on ("unmount it and try again"); truncating that
     * at 96 would leave a diagnostic that stops mid-instruction. */
    char last_io[256];

    /* READ CD transfer shaping (0.39.0), owned by adsc_mmc_read_cd.
     *
     * `xfer_bounce` backs any READ CD whose nsec * sector_len is not a multiple
     * of 16: libata refuses ATAPI DMA for such a transfer and falls back to
     * PIO, and on PIO a LITE-ON LH-20A1S pads every record to 16 bytes (2742
     * -> 2752), displacing all but the first. The transfer is rounded up — but
     * the kernel writes the WHOLE dxfer_len, and every caller allocates exactly
     * nsec * sector_len, so the rounded transfer lands here and is copied out.
     * Rounding against the caller's own buffer would be a silent heap overrun
     * that ASan cannot see (copy_to_user writes past its redzones).
     * `xfer_exact`: 0 = untested, -1 = a rounded READ CD has succeeded on this
     * handle, 1 = a rounded transfer failed at the transport where the exact
     * one succeeded, so rounding is off for the handle's lifetime.
     *
     * `layout[c2]` is the combined C2 + raw P-W record order for that C2 mode
     * (ADSC_LAYOUT_*, src/cdda/layout.h), latched on positive evidence and
     * never on its absence. */
    uint8_t *xfer_bounce;
    uint32_t xfer_bounce_cap;
    int xfer_exact;
    uint8_t layout[3];

    /* Write health (0.34.0). Counts and times LIVE burns only — a simulate run
     * skips SEND OPC and never fires the laser, so it costs the medium nothing
     * and must not appear here. See accudisc_write_health in the public header
     * for why the PASS COUNT rather than the elapsed interval is the quantity
     * being bounded. */
    uint32_t wr_live;        /* live write operations through this handle */
    uint32_t wr_budget;      /* 0 = unlimited (the default) */
    uint32_t wr_base_settle; /* first live burn: lead-in settle, ms */
    uint32_t wr_base_pay;    /* first live burn: payload time, ms */
    uint32_t wr_base_sect;   /* first live burn: payload sectors */
    uint32_t wr_last_settle;
    uint32_t wr_last_pay;
    uint32_t wr_last_sect;
    uint32_t wr_anomaly;     /* ACCUDISC_WRITE_ANOMALY_* bitmask */
};

/* Record one completed LIVE burn against the handle's write health, and
 * recompute the anomaly mask. `sectors` is the payload sector count (the
 * lead-in gap excluded, so the per-sector rate is comparable across burns of
 * different lengths — comparing raw payload_ms would flag every short burn).
 * Called only from the burn path, only when the laser actually ran. */
void adsc_write_health_record(struct accudisc_device *dev, uint32_t settle_ms,
                              uint32_t payload_ms, uint32_t sectors);

/* Run a command on the device, recording decoded sense in the handle on any
 * failure (cleared on success). Returns ACCUDISC_OK / _ERR_IO / _ERR_SENSE. */
int adsc_dev_exec(struct accudisc_device *dev, adsc_cmd *cmd);

/* A trace-only narrative line ("trace: -- ..."), emitted only when the handle
 * was opened with a trace flag. For the steps of a multi-command operation
 * (the burn's phases), so the per-command lines around it can be read as a
 * sequence rather than a list. Silent — and free — with tracing off. */
/* ---- drive readiness (0.38.0, src/drive/ready.c) --------------------------
 *
 * See that file's header for the measurements that shaped this; the short
 * version is that TEST UNIT READY is the only indicator on the PX-716A that
 * changes state when a freshly loaded disc becomes usable, so it is the only
 * one consulted. */
enum {
    ADSC_READY_YES,       /* GOOD — proceed */
    ADSC_READY_WAIT,      /* 2/04/xx — becoming ready; cool down and re-ask */
    ADSC_READY_RETRY_NOW, /* 6/28|29 — unit attention; the command did not
                           * execute, so re-issue it at once */
    ADSC_READY_NO_MEDIUM, /* 2/3A/xx — terminal, no disc. Never retried */
    ADSC_READY_OTHER      /* not a readiness condition; hand it back unchanged */
};

/* Poll interval and cap. MEASURED, not chosen, and the spread is wide: on the
 * PX-716A the window after a software reload was ~1.0 s in one run
 * (tools/readyprobe.c, 2026-09-12) and ~18 s in another on the same disc the
 * same afternoon, with a hand-loaded disc needing 8 s once and a call blocking
 * 20 s another time. One drive, one afternoon, a factor of eighteen.
 *
 * The cap is 60 s because the two failure directions are not symmetric. Too
 * high costs waiting longer before reporting a drive that is genuinely stuck —
 * annoying, and bounded. Too low returns "not ready" for a drive that WOULD
 * have become ready, which is a false failure of exactly the kind this file
 * exists to remove; the first draft's 30 s left barely 12 s of headroom over
 * an observed case, which is not headroom. */
#define ADSC_READY_POLL_MS 250u
#define ADSC_READY_TIMEOUT_MS 60000u
/* Below this, a wait is not worth a line of output. */
#define ADSC_READY_NOTE_MS 1000u
/* Unit attentions can queue (a media change and a reset can both be pending),
 * so one retry is not always enough — but an unbounded loop against a drive
 * stuck in UA is a hang. These retries cost nothing: no sleep, the command
 * simply did not execute. */
#define ADSC_UA_RETRIES 3u

void adsc_sleep_ms(long ms);

/* Pure, and therefore testable without a drive — tests/test_ready.c. */
int adsc_ready_classify(int rc, const accudisc_sense *s);
int adsc_op_repeatable(uint8_t op);

/* Poll TEST UNIT READY until the drive is ready. ACCUDISC_OK when it is;
 * ACCUDISC_ERR_SENSE for a terminal condition (no medium, tray open — the
 * drive's own sense is in dev->last_sense and says which); ACCUDISC_ERR_IO on
 * timeout, with an explanation in dev->last_io. */
int adsc_dev_wait_ready(struct accudisc_device *dev, unsigned timeout_ms);

void adsc_dev_trace_note(struct accudisc_device *dev, const char *fmt, ...)
#if defined(__GNUC__)
    __attribute__((format(printf, 2, 3)))
#endif
    ;

/* Identify once and cache (INQUIRY). */
int adsc_dev_identify(struct accudisc_device *dev);

/* Collapse whitespace runs in an INQUIRY field to single spaces and trim the
 * ends, so table lookups match regardless of how a drive pads its fixed-width
 * fields ("DVDR   PX-716A" vs "DVDR PX-716A"). Always NUL-terminates. */
void adsc_inquiry_normalize(const char *src, char *dst, size_t cap);

/* `adsc_uncap_classify` was declared here until 0.8.0: the driver-free
 * inference from page 2A's advertised maximum against a per-model stock
 * ceiling. Removed with the table — see the note at the top of
 * src/drive/uncap.c for why the quantity it compared could not answer the
 * question it was asked. */

/* The uncap's state. ON or OFF only when this handle set it or an attached
 * driver answers; UNKNOWN otherwise, and UNKNOWN is not "off". Costs no MODE
 * SENSE. Since 0.8.0 these are the only sources there are, so
 * accudisc_speed_uncap_probe returns the same verdict and differs only in also
 * reporting max_x. */
accudisc_uncap_state adsc_uncap_authoritative(accudisc_device *dev);

/* The device-free half of accudisc_probe_speed_ladder: the window layout.
 * Rung i's window in band b begins at lba + b*(*band) + i*(*slot), and all
 * points*ncand of them are disjoint — which is the whole cache-freshness
 * guarantee. Returns ACCUDISC_ERR_INVAL when they would not all fit in
 * `count`, or when `points` is not 1 or 3.
 *
 * Split out because overlapping windows do not fail loudly: they would be
 * served from the drive cache and report the rung as FLAT across radii,
 * which is also what a genuinely CLV-clamped rung looks like. The guard
 * has to be testable without a disc. See src/drive/speeds.c. */
int adsc_speeds_layout(uint32_t count, uint8_t ncand, uint8_t points,
                       uint32_t *slot, uint32_t *band);

/* Fill in `verdict`/`equiv_x` for a measured ladder (see accudisc.h for what
 * the verdicts mean). Pure — no device, no I/O — so the whole admission rule
 * is testable against recorded measurements. `rungs` must be in the order
 * probed; `points` is what the probe ran with.
 *
 * `k` is the margin, in radius-steps, a rate gap must beat to count as a real
 * difference. The public entry point passes SPEEDS_ADMIT_K; it is a parameter
 * ONLY so tests can sweep it and show the ladder does not depend on the exact
 * value — a rule tuned until it matches one disc is a census, not a rule.
 * See src/drive/speeds.c. */
void adsc_speeds_admit(accudisc_speed_rung *rungs, uint8_t n, uint8_t points,
                       uint32_t k);

void adsc_dev_log(struct accudisc_device *dev, const char *fmt, ...);

/* ---- caller-declared struct size (API_PLAN §7.1) ---------------------------
 * Both are pure — no device, no I/O — so the whole negotiation is testable
 * without hardware. See src/abi.c and tests/test_abi.c.
 *
 * adsc_abi_import: copy a caller's INPUT struct into a local of this build's
 * layout. src_size is what the caller declared. A short struct is zero-extended
 * (an older caller gets older behaviour); a long one is accepted only when
 * every byte past our end is zero. dst's own leading uint32_t size is set to
 * dst_size on success, so the rest of the library sees a normalised struct.
 * PRECONDITION: both layouts begin with `uint32_t size`.
 *
 * adsc_abi_export: how many bytes of an OUTPUT struct we may write into the
 * caller's allocation. A short struct is honoured; a long one is refused,
 * because leaving a counter unfilled would hand back a zero the caller cannot
 * distinguish from a measured zero.
 *
 * Both return ACCUDISC_OK or ACCUDISC_ERR_ABI. A declared size of 0 — what a
 * caller that forgot ACCUDISC_*_INIT produces — is always refused. */
/* How far past this build's layout a caller's declared size may reach before it
 * is refused outright. Not a compatibility limit — it is the bound on how far
 * adsc_abi_import will read looking for set fields it cannot honour, and it
 * exists because that size is a *claim* that may be uninitialised stack. For
 * scale: accudisc_read_req grew 24 bytes in total across its whole history. */
#define ADSC_ABI_GROWTH_MAX 256u

int adsc_abi_import(void *dst, size_t dst_size,
                    const void *src, uint32_t src_size);
int adsc_abi_export(uint32_t want, size_t have, size_t *n_out);


/* CD Mastering (002Eh) alone — no smoke reads, so the write path can ask
 * "does this drive claim BURN-Proof?" without the cost of a full feature
 * probe. Fills only the mastering_* / *_claimed fields. Returns 0 when the
 * descriptor came back, -1 when it did not (which is not an error: CDEmu
 * reports no CD Mastering descriptor and burns through it fine). */
int adsc_probe_cd_mastering(struct accudisc_device *dev, accudisc_features *f);

/* Current WRITE speed in kB/s from mode page 2A. Device property (device.c),
 * needed by the write path AND by accudisc_set_speed — the SET STREAMING
 * descriptor cannot leave the write speed alone, so a read-speed call has to
 * carry it through by hand. */
int adsc_dev_cur_write_kbps(struct accudisc_device *dev, unsigned *kbps);

#endif /* ADSC_INTERNAL_H */