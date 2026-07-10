/* AccuDisc vendor driver: Plextor.
 *
 * Vendor opcode 0xEA error-counter scan (the mechanism behind PlexTools
 * Q-Check), sub-commands pinned from cdrtools readcd plextor_*_cx_scan and
 * validated on a PX-716A:
 *   0x15 = arm scan mode, 0x16 = read interval counters (26 B), 0x17 = end.
 *
 * Counter block layout (validated against readcd -cxscan):
 *   [12..17] three big-endian words summing to the interval's C1 count
 *   [20..21] CU (uncorrectable)
 *   [22..23] C2
 *
 * Built as an external accudisc-drv-plextor.so; never linked into
 * libaccudisc. See accudisc/driver.h for the contract.
 */

#include <string.h>

#include <accudisc/driver.h>

#define OP_PLEXTOR_CX 0xEA
#define CX_COUNTERS_LEN 26

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

static int px_match(const accudisc_drive_id *id)
{
    return strcmp(id->vendor, "PLEXTOR") == 0;
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
    .description = "Plextor 0xEA extensions: C1/C2/CU error-counter scan",
    .match = px_match,
    .selftest = px_selftest,
    .counter_scan_begin = px_begin,
    .counter_scan_read = px_read,
    .counter_scan_end = px_end,
};

const accudisc_driver *accudisc_driver_entry(void)
{
    return &plextor_driver;
}
