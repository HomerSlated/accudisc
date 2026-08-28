/* AccuDisc vendor driver: Plextor.
 *
 * Two vendor opcodes, both validated on a PX-716A (see PROTOCOL.md):
 *
 * 0xEA — error-counter scan (the mechanism behind PlexTools Q-Check),
 * sub-commands pinned from cdrtools readcd plextor_*_cx_scan:
 *   0x15 = arm scan mode, 0x16 = read interval counters (26 B), 0x17 = end.
 * Counter block layout (validated against readcd -cxscan):
 *   [12..17] three big-endian words summing to the interval's C1 count
 *   [20..21] CU (uncorrectable)
 *   [22..23] C2
 *
 * 0xE9 — vendor MODE get/set of small 8-byte feature pages. Page 0xBB is
 * SpeedRead, which lifts the firmware's CD read-speed cap (PX-716A: max read
 * 40x -> 48x in mode page 2A).
 *
 * 0xED — "drive mode 2", a SEPARATE 8-byte state block from 0xE9's pages and
 * reached by a different CDB shape. Mode code 0 carries POWEREC, the automatic
 * write-speed governor. Pinned from cdrtools cdrecord drv_mmc.c
 * (drivemode2_plextor / powerrec_plextor / check_powerrec_plextor); credited in
 * docs/reference/ATTRIBUTION.md.
 *   GET: data-IN, 8 bytes. resp[2] bit 0 = POWEREC on; resp[4..5] = the
 *        recommended write speed in kB/s, big-endian.
 *   SET: NO data transfer at all — the payload is one BIT, smuggled through
 *        CDB byte 1. cdrecord assigns two BITFIELDS of that byte, and both
 *        land inside it (libscg/scg/scsicdb.h, struct scsi_g5cdb, LE):
 *          g5_cdb.reladr = bit 0        <- the new POWEREC state
 *          g5_cdb.res    = bits 1..4    <- set to 0x08, i.e. byte1 |= 0x10
 *        so byte 1 is 0x10 for off and 0x11 for on. Mode code stays at byte 2.
 *        Getting this wrong is not silent: byte 1 = 0x08 with the value in
 *        byte 2 (my first attempt, reading `res = 0x08` as a whole byte) is
 *        refused 5/24 INVALID FIELD IN CDB by the drive.
 *        The state byte the get returns is NOT resent; only the bit is.
 *
 * Built as an external accudisc-drv-plextor.so; never linked into
 * libaccudisc. See accudisc/driver.h for the contract.
 */

#include <string.h>

#include <accudisc/driver.h>

#define OP_PLEXTOR_CX 0xEA
#define CX_COUNTERS_LEN 26

/* 0xE9 vendor MODE: CDB[1] direction, CDB[2] page, CDB[3] value, length at
 * CDB[10]. The transfer is data-IN even for a SET — the drive echoes the
 * resulting page, so a write reads itself back. resp[0] = page echo,
 * resp[1] = constant 0x06 header, resp[2..] = state. */
#define OP_PLEXTOR_MODE 0xE9
#define PX_MODE_GET 0x00
#define PX_MODE_SET 0x10
#define PX_MODE_LEN 8
#define PX_PAGE_SPEEDREAD 0xBB

/* 0xED drive-mode-2. CDB[2] carries the mode code; a SET is flagged by CDB[1]
 * = 0x08 with the value in CDB[2]... see px_mode2() for the exact placement,
 * which differs from 0xE9's and is the reason this is a separate helper. */
#define OP_PLEXTOR_MODE2 0xED
#define PX_MODE2_LEN 8
#define PX_MODE2_CODE_POWEREC 0x00
#define PX_POWEREC_BIT 0x01

static int px_cx_cmd(const accudisc_host *host, uint8_t sub, uint8_t *data,
                     uint32_t len)
{
    uint8_t cdb[12] = {0};

    cdb[0] = OP_PLEXTOR_CX;
    cdb[1] = sub;
    if (sub == 0x15)
        cdb[3] = 0x01;
    if (sub == 0x16) {
        cdb[2] = 0x01;
        cdb[10] = (uint8_t)len;
    }
    return host->exec(host->dev, cdb, sizeof(cdb),
                      len ? ACCUDISC_HOST_IN : ACCUDISC_HOST_NONE, data, len,
                      30000);
}

static int px_mode(const accudisc_host *host, uint8_t dir, uint8_t page,
                   uint8_t val, uint8_t *resp)
{
    uint8_t cdb[12] = {0};

    cdb[0] = OP_PLEXTOR_MODE;
    cdb[1] = dir;
    cdb[2] = page;
    cdb[3] = val;
    cdb[10] = PX_MODE_LEN;
    return host->exec(host->dev, cdb, sizeof(cdb), ACCUDISC_HOST_IN, resp,
                      PX_MODE_LEN, 10000);
}

/* 0xED drive mode 2. Two shapes in one command, following cdrecord:
 *
 *   GET (val == NULL): data-IN of PX_MODE2_LEN, length at CDB[8..9], mode code
 *                      at CDB[2] (g1_cdb.addr[0]).
 *   SET (val != NULL): NO data transfer. CDB[1] = 0x08 marks it a set and
 *                      CDB[2] carries the value's truthiness.
 *
 * The set form is genuinely odd — a one-BIT payload smuggled through CDB
 * fields rather than a data-out buffer — and it is transcribed from the
 * reference rather than derived, because there is no other source for it.
 */
static int px_mode2(const accudisc_host *host, uint8_t modecode,
                    const uint8_t *val, uint8_t *resp)
{
    uint8_t cdb[12] = {0};

    cdb[0] = OP_PLEXTOR_MODE2;
    if (val) {
        /* bits 1-4 = 0x08 (the "set" marker) and bit 0 = the new state.
         * 0x08 << 1 = 0x10. See the file header for why this is one byte and
         * not two. */
        cdb[1] = (uint8_t)(0x10 | (*val ? 0x01 : 0x00));
        cdb[2] = modecode;
        return host->exec(host->dev, cdb, sizeof(cdb), ACCUDISC_HOST_NONE,
                          NULL, 0, 10000);
    }
    cdb[2] = modecode;
    cdb[8] = (uint8_t)(PX_MODE2_LEN >> 8);
    cdb[9] = (uint8_t)(PX_MODE2_LEN & 0xff);
    return host->exec(host->dev, cdb, sizeof(cdb), ACCUDISC_HOST_IN, resp,
                      PX_MODE2_LEN, 10000);
}

/* POWEREC state, and the rate the governor recommends.
 *
 * The recommended rate is STATUS, not a promise: measured on a PX-716A
 * 2026-08-28 it recommended 48x while cdrecord's dummy run of the same medium
 * delivered 25x. Reported because the caller may want to see what the drive
 * intends; never to be presented as a rate. */
static int px_governor_get(const accudisc_host *host, int *on,
                           uint32_t *recommended_kbps)
{
    uint8_t r[PX_MODE2_LEN] = {0};
    int rc = px_mode2(host, PX_MODE2_CODE_POWEREC, NULL, r);

    if (rc != ACCUDISC_OK)
        return rc;
    if (on)
        *on = (r[2] & PX_POWEREC_BIT) ? 1 : 0;
    if (recommended_kbps)
        *recommended_kbps = (uint32_t)(((unsigned)r[4] << 8) | r[5]);
    return ACCUDISC_OK;
}

static int px_governor_set(const accudisc_host *host, int on)
{
    uint8_t want = (uint8_t)(on ? 1 : 0);
    int rc, now = 0;

    /* Only the BIT travels — the CDB has room for nothing else — so unlike
     * cdrecord's read-modify-write of a state byte there is nothing here to
     * preserve. */
    rc = px_mode2(host, PX_MODE2_CODE_POWEREC, &want, NULL);
    if (rc != ACCUDISC_OK)
        return rc;

    /* RE-READ. The 0xED set returns no data, so unlike SpeedRead's 0xE9 it
     * cannot verify itself — the confirmation has to be a second command. */
    rc = px_governor_get(host, &now, NULL);
    if (rc != ACCUDISC_OK)
        return rc;
    if (now != (on ? 1 : 0)) {
        host->log(host->dev, "plextor: POWEREC set did not take effect");
        return ACCUDISC_ERR_UNSUPPORTED;
    }
    return ACCUDISC_OK;
}

static int px_match(const accudisc_drive_id *id)
{
    return strcmp(id->vendor, "PLEXTOR") == 0;
}

/* SpeedRead (0xE9 page 0xBB): resp[2] is the on/off state. */
static int px_speed_uncap_get(const accudisc_host *host, int *on)
{
    uint8_t r[PX_MODE_LEN] = {0};
    int rc = px_mode(host, PX_MODE_GET, PX_PAGE_SPEEDREAD, 0, r);

    if (rc != ACCUDISC_OK)
        return rc;
    *on = r[2] ? 1 : 0;
    return ACCUDISC_OK;
}

static int px_speed_uncap_set(const accudisc_host *host, int on)
{
    uint8_t r[PX_MODE_LEN] = {0};
    int rc = px_mode(host, PX_MODE_SET, PX_PAGE_SPEEDREAD,
                     (uint8_t)(on ? 1 : 0), r);

    if (rc != ACCUDISC_OK)
        return rc;
    /* The SET echoes the resulting page: the write verifies itself. */
    if ((r[2] ? 1 : 0) != (on ? 1 : 0)) {
        host->log(host->dev, "plextor: SpeedRead set did not take effect");
        return ACCUDISC_ERR_UNSUPPORTED;
    }
    return ACCUDISC_OK;
}

static int px_begin(const accudisc_host *host)
{
    return px_cx_cmd(host, 0x15, NULL, 0);
}

static int px_read(const accudisc_host *host, accudisc_counters *out)
{
    uint8_t d[CX_COUNTERS_LEN] = {0};
    int rc = px_cx_cmd(host, 0x16, d, sizeof(d));

    if (rc != ACCUDISC_OK)
        return rc;
    out->c1 = (uint32_t)(((unsigned)d[12] << 8) | d[13]) +
              (uint32_t)(((unsigned)d[14] << 8) | d[15]) +
              (uint32_t)(((unsigned)d[16] << 8) | d[17]);
    out->cu = (uint32_t)(((unsigned)d[20] << 8) | d[21]);
    out->c2 = (uint32_t)(((unsigned)d[22] << 8) | d[23]);
    return ACCUDISC_OK;
}

static int px_end(const accudisc_host *host)
{
    return px_cx_cmd(host, 0x17, NULL, 0);
}

/* Read/set/re-read proof: arm scan mode (set), read the counters back (the
 * 0x16 read only succeeds once armed — reading it IS the re-read of the
 * state we set), then disarm and confirm the drive accepted that too. */
static int px_selftest(const accudisc_host *host)
{
    accudisc_counters c;
    int rc;

    rc = px_begin(host);
    if (rc != ACCUDISC_OK) {
        host->log(host->dev, "plextor: 0xEA arm refused");
        return ACCUDISC_ERR_UNSUPPORTED;
    }
    rc = px_read(host, &c);
    if (rc != ACCUDISC_OK) {
        host->log(host->dev, "plextor: counter read-back failed after arm");
        px_end(host);
        return ACCUDISC_ERR_UNSUPPORTED;
    }
    rc = px_end(host);
    if (rc != ACCUDISC_OK) {
        host->log(host->dev, "plextor: 0xEA disarm refused");
        return ACCUDISC_ERR_UNSUPPORTED;
    }
    return ACCUDISC_OK;
}

static const accudisc_driver plextor_driver = {
    .abi = ACCUDISC_DRIVER_ABI,
    .name = "plextor",
    .description = "Plextor extensions: C1/C2/CU error-counter scan (0xEA), "
                   "SpeedRead read-speed uncap (0xE9), POWEREC write-speed "
                   "governor (0xED)",
    .match = px_match,
    .selftest = px_selftest,
    .counter_scan_begin = px_begin,
    .counter_scan_read = px_read,
    .counter_scan_end = px_end,
    .speed_uncap_get = px_speed_uncap_get,
    .speed_uncap_set = px_speed_uncap_set,
    .write_governor_get = px_governor_get,
    .write_governor_set = px_governor_set,
};

const accudisc_driver *accudisc_driver_entry(void)
{
    return &plextor_driver;
}
