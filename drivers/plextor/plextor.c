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
 * SETTINGS REPORT (0.46.0, ABI 6) — px_settings_get, READ-ONLY. The query set
 * is QPxTool's `cdvdcontrol -c` (console/cdvdcontrol/cdvdcontrol.cpp), in its
 * order, with its decode tables (lib/qpxplextor/plextor_features.{h,cpp});
 * credited in docs/reference/ATTRIBUTION.md. Chosen that way on purpose: that
 * exact set had already been sent to the drive this was built for, through
 * the same USB bridge, without incident. Two things from the wider QPxTool
 * surface are deliberately NOT sent, because `-c` does not send them:
 *   - Silent-mode pages 0x06/0x07: measured 2026-09-09 to answer 4/00/00 +
 *     DID_ERROR (FEATURES.md row 6) — a hardware-error path to provoke through
 *     a bridge with a history of wedging, for nothing `-c` shows.
 *   - 0xEB's "last actual speed": never checked against elapsed burn time, so
 *     it would be a tidy number whose meaning is assumed (the page-2A trap).
 * And one QPxTool form is replaced: EEPROM is read ONLY in the block form
 * F1 01 <idx> <size>. The F1 00 form is the one the PX-716 rejects.
 *
 * Built as an external accudisc-drv-plextor.so; never linked into
 * libaccudisc. See accudisc/driver.h for the contract.
 */

#include <stdarg.h>
#include <stdio.h>
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

static uint32_t be16(const uint8_t *p)
{
    return (uint32_t)(((unsigned)p[0] << 8) | p[1]);
}

/* Decode all eight fields of the 26-byte CD readout — QPxTool parity, 0.35.0.
 *
 * We used to take three: [12..17] summed as C1, [20..21] as CU, [22..23] as
 * C2.  Those three were not WRONG — byte 20 is the uncorrectable-at-C2 count
 * on both decodes, and byte 22 the correctable-at-C2 count on both — but they
 * discarded five fields the drive had already measured, including the one
 * (byte 10) that lets the C1 decode check itself.
 *
 * The portable triple is still filled, because the generic verification path
 * speaks it and has no e-fields to offer.  `have_detail` is what stops a
 * consumer reading an unfilled e-field's zero as an observation of zero. */
static int px_read(const accudisc_host *host, accudisc_counters *out)
{
    uint8_t d[CX_COUNTERS_LEN] = {0};
    int rc = px_cx_cmd(host, 0x16, d, sizeof(d));

    if (rc != ACCUDISC_OK)
        return rc;

    out->bler = be16(d + 10);
    out->e31  = be16(d + 12);
    out->e21  = be16(d + 14);
    out->e11  = be16(d + 16);
    out->uncr = be16(d + 18);   /* meaning OPEN — decoded, never relied on */
    out->e32  = be16(d + 20);
    out->e22  = be16(d + 22);
    out->e12  = be16(d + 24);
    out->have_detail = 1;

    /* The portable triple.  C1 stays the SUM rather than switching to the
     * drive's bler field: the sum is what every previously-reported figure
     * from this driver was, and changing which quantity feeds it would move
     * users' numbers silently.  The census checks the two against each other
     * instead, which is the honest way to learn they ever differ. */
    out->c1 = out->e11 + out->e21 + out->e31;
    out->c2 = out->e22;
    out->cu = out->e32;
    return ACCUDISC_OK;
}

static int px_end(const accudisc_host *host)
{
    return px_cx_cmd(host, 0x17, NULL, 0);
}

/* The counter-scan proof, kept as the FALLBACK: arm scan mode (set), read
 * the counters back (the 0x16 read only succeeds once armed — reading it IS
 * the re-read of the state we set), then disarm and confirm that too.
 *
 * It needs a READABLE DISC. Measured 2026-09-28 on a PX-716A with an empty
 * tray: the arm is refused, so the whole driver stayed unattached — while
 * every feature it gates (settings, POWEREC, SpeedRead) works without one. */
static int px_selftest_cx(const accudisc_host *host)
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

/* ---- settings report (READ-ONLY) ------------------------------------------
 *
 * Every command below is a GET. There is no SET form anywhere in this section,
 * and tests/test_plextor_settings.c fails the build if one is ever issued: it
 * rejects any 0xE9 whose CDB[1] is not 0x00, any 0xED with a non-zero CDB[1],
 * any 0xE4 with a non-zero CDB[1] (0xE4 CDB[1]=0x01 STARTS a Media Quality
 * Check), and 0xEE (drive reset), 0xE3 (PlexEraser), 0xE5 and 0xD5 outright. */

#define OP_PLEXTOR_AS_RD  0xE4
#define OP_PLEXTOR_EEPROM 0xF1
#define PX_EEPROM_BLOCK   256

#define PX_PAGE_SS_HIDE   0x01
#define PX_PAGE_VARIREC   0x02
#define PX_PAGE_GIGAREC   0x04
#define PX_PAGE_SILENT    0x08
#define PX_PAGE_TESTWRITE 0x21 /* DVD+R(W) test write */
#define PX_PAGE_BITSET    0x22
#define PX_PAGE_SECUREC   0xD5
#define PX_VARIREC_CD     0x00
#define PX_VARIREC_DVD    0x10
#define PX_BITSET_R       0x0A
#define PX_BITSET_RDL     0x0E

struct px_code { uint8_t val; const char *name; };

/* QPxTool plextor_features.h. GigaREC 0x00 is 1.0x, which the drive calls
 * off. */
static const struct px_code px_gigarec[] = {
    {0x83, "0.6"}, {0x82, "0.7"}, {0x81, "0.8"}, {0x84, "0.9"},
    {0x00, "OFF"}, {0x04, "1.1"}, {0x01, "1.2"}, {0x02, "1.3"},
    {0x03, "1.4"}, {0, NULL}};

/* VariREC power: magnitude in the low bits, bit 7 = negative. */
static const struct px_code px_varirec_pwr[] = {
    {0x84, "-4"}, {0x83, "-3"}, {0x82, "-2"}, {0x81, "-1"}, {0x00, "0"},
    {0x01, "+1"}, {0x02, "+2"}, {0x03, "+3"}, {0x04, "+4"}, {0, NULL}};

static const char *const px_varirec_str_cd[] = {
    "Default", "Azo", "Cyanine", "PhtaloCyanine A", "PhtaloCyanine B",
    "PhtaloCyanine C", "PhtaloCyanine D"};
static const char *const px_varirec_str_dvd[] = {
    "Default", "Strategy0", "Strategy1", "Strategy2", "Strategy3",
    "Strategy4", "Strategy5", "Strategy6", "Strategy7"};

/* Silent mode, CD caps (QPxTool silent_cd_{rd,wr}_tbl). */
static const struct px_code px_silent_cd_rd[] = {
    {0x05, "48X"}, {0x04, "40X"}, {0x03, "32X"}, {0x02, "24X"},
    {0x01, "8X"}, {0x00, "4X"}, {0, NULL}};
static const struct px_code px_silent_cd_wr[] = {
    {0x08, "48X"}, {0x06, "32X"}, {0x05, "24X"}, {0x03, "16X"},
    {0x01, "8X"}, {0x00, "4X"}, {0, NULL}};

/* AutoStrategy mode, resp[2] & 0x0F. On the PX-716 only OFF and AUTO exist;
 * FORCED and ON are PX-755/760 extensions. Note 1 is AUTO, not ON — this
 * repo's FEATURES.md said "ON" for it until 0.46.0. */
static const struct px_code px_as_mode[] = {
    {0x00, "OFF"}, {0x01, "AUTO"}, {0x04, "FORCED"}, {0x08, "ON"},
    {0, NULL}};

static const char *px_name(const struct px_code *t, uint8_t v)
{
    for (; t->name; t++)
        if (t->val == v)
            return t->name;
    return NULL;
}

struct px_vs {
    const accudisc_host *host;
    accudisc_vendor_setting *out;
    uint32_t cap, n;
    int dead; /* a transport failure: ask nothing more */
};

static void vs_put(struct px_vs *v, const char *key, const char *label,
                   uint32_t flags, int64_t num, const char *fmt, ...)
    __attribute__((format(printf, 6, 7)));

static void vs_put(struct px_vs *v, const char *key, const char *label,
                   uint32_t flags, int64_t num, const char *fmt, ...)
{
    if (v->n < v->cap) {
        accudisc_vendor_setting *e = &v->out[v->n];
        va_list ap;

        memset(e, 0, sizeof(*e));
        snprintf(e->key, sizeof(e->key), "%s", key);
        snprintf(e->label, sizeof(e->label), "%s", label);
        va_start(ap, fmt);
        vsnprintf(e->value, sizeof(e->value), fmt, ap);
        va_end(ap);
        e->num = num;
        e->flags = flags;
    }
    v->n++;
}

/* vs_exec's answer when the latch is set and it did NOT send the command. A
 * distinct code rather than a test of v->dead, because the latch is set BY the
 * failing command: testing the flag afterwards would report the one query that
 * actually wedged the bridge as never having been sent. */
#define PX_NOT_SENT ACCUDISC_ERR_CANCELLED

/* Record why a key has no answer. Three different facts, kept apart: never
 * sent says nothing about the drive; refused is the drive answering; a
 * transport failure is the bridge or host, and names the query it hit. */
static void vs_fail(struct px_vs *v, const char *key, const char *label,
                    int rc)
{
    if (rc == PX_NOT_SENT)
        vs_put(v, key, label, 0, 0,
               "not asked (an earlier command failed in transport)");
    else if (rc == ACCUDISC_ERR_SENSE)
        vs_put(v, key, label, 0, 0, "refused by the drive (CHECK CONDITION)");
    else
        vs_put(v, key, label, 0, 0,
               "failed in transport (rc %d); nothing further was sent", rc);
}

/* One GET, with the transport-failure latch. A CHECK CONDITION is the drive
 * answering "no" and the next query is still safe; anything else (host,
 * transport, timeout) may be a wedged bridge, and sending a dozen more
 * commands into one is how a probe becomes a reset storm. */
static int vs_exec(struct px_vs *v, const uint8_t *cdb, uint8_t *resp,
                   uint32_t len, uint32_t timeout_ms)
{
    int rc;

    if (v->dead)
        return PX_NOT_SENT;
    rc = v->host->exec(v->host->dev, cdb, 12, ACCUDISC_HOST_IN, resp, len,
                       timeout_ms);
    if (rc != ACCUDISC_OK && rc != ACCUDISC_ERR_SENSE) {
        v->dead = 1;
        v->host->log(v->host->dev, "plextor: settings query hit a transport "
                                   "failure; the remaining queries are not "
                                   "sent");
    }
    return rc;
}

static int vs_mode_get(struct px_vs *v, uint8_t page, uint8_t val,
                       uint8_t *resp)
{
    uint8_t cdb[12] = {0};

    cdb[0] = OP_PLEXTOR_MODE;
    cdb[1] = PX_MODE_GET;
    cdb[2] = page;
    cdb[3] = val;
    cdb[10] = PX_MODE_LEN;
    return vs_exec(v, cdb, resp, PX_MODE_LEN, 10000);
}

static int vs_eeprom_block(struct px_vs *v, uint8_t idx, uint8_t *resp)
{
    uint8_t cdb[12] = {0};

    cdb[0] = OP_PLEXTOR_EEPROM;
    cdb[1] = 0x01; /* the block form; F1 00 is refused by the PX-716 */
    cdb[7] = idx;
    cdb[8] = (uint8_t)(PX_EEPROM_BLOCK >> 8);
    cdb[9] = (uint8_t)(PX_EEPROM_BLOCK & 0xff);
    return vs_exec(v, cdb, resp, PX_EEPROM_BLOCK, 10000);
}

static uint32_t be32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | p[3];
}

static void vs_hms(struct px_vs *v, const char *key, const char *label,
                   uint32_t secs)
{
    vs_put(v, key, label, ACCUDISC_VSET_OK | ACCUDISC_VSET_HAS_NUM,
           (int64_t)secs, "%u:%02u:%02u", secs / 3600u, (secs / 60u) % 60u,
           secs % 60u);
}

/* The EEPROM layout QPxTool reads the life counters from is the one it uses
 * for the PX-714/716/716AL/755/760 group (4 x 256-byte blocks); the PX-708/712
 * and the CD-R-only models keep them elsewhere. Anything we cannot place is
 * reported as unknown rather than decoded from the wrong offsets. */
static int px_eeprom_layout_known(const accudisc_drive_id *id)
{
    static const char *const models[] = {
        "PX-714", "PX-716", "PX-755", "PX-760", NULL};

    for (int i = 0; id && models[i]; i++)
        if (strstr(id->product, models[i]))
            return 1;
    return 0;
}

/* A byte decoded through a table. `num` is the raw code: the tables are not
 * ordered, so no arithmetic on it would mean anything. An unknown code is
 * shown raw, still OK — the drive answered, we just cannot name it. */
static void vs_code(struct px_vs *v, const char *key, const char *label,
                    const struct px_code *t, uint8_t b)
{
    const char *nm = px_name(t, b);

    if (nm)
        vs_put(v, key, label, ACCUDISC_VSET_OK | ACCUDISC_VSET_HAS_NUM, b,
               "%s", nm);
    else
        vs_put(v, key, label, ACCUDISC_VSET_OK | ACCUDISC_VSET_HAS_NUM, b,
               "0x%02X (unknown code)", (unsigned)b);
}

static void vs_switch(struct px_vs *v, const char *key, const char *label,
                      int rc, int on)
{
    if (rc != ACCUDISC_OK)
        vs_fail(v, key, label, rc);
    else
        vs_put(v, key, label, ACCUDISC_VSET_OK | ACCUDISC_VSET_HAS_NUM,
               on ? 1 : 0, "%s", on ? "ON" : "OFF");
}

static void vs_varirec(struct px_vs *v, const char *key, const char *label,
                       uint8_t disc)
{
    uint8_t r[PX_MODE_LEN] = {0};
    int rc = vs_mode_get(v, PX_PAGE_VARIREC, (uint8_t)(0x02 | disc), r);
    const char *const *strs = disc == PX_VARIREC_DVD ? px_varirec_str_dvd
                                                     : px_varirec_str_cd;
    unsigned nstr = disc == PX_VARIREC_DVD
        ? (unsigned)(sizeof(px_varirec_str_dvd) / sizeof(*px_varirec_str_dvd))
        : (unsigned)(sizeof(px_varirec_str_cd) / sizeof(*px_varirec_str_cd));
    const char *pwr, *str;
    int64_t step;

    if (rc != ACCUDISC_OK) {
        vs_fail(v, key, label, rc);
        return;
    }
    if (!r[2]) {
        vs_put(v, key, label, ACCUDISC_VSET_OK | ACCUDISC_VSET_HAS_NUM, 0,
               "OFF");
        return;
    }
    /* ON. `num` is the signed power step, so a caller can test "is the laser
     * power offset" without parsing text. An undecodable power is shown raw
     * and gets no number — a guessed step is worse than none. */
    pwr = px_name(px_varirec_pwr, r[3]);
    str = r[5] < nstr ? strs[r[5]] : "?";
    step = (r[3] & 0x80) ? -(int64_t)(r[3] & 0x7f) : (int64_t)r[3];
    if (pwr)
        vs_put(v, key, label, ACCUDISC_VSET_OK | ACCUDISC_VSET_HAS_NUM, step,
               "ON, power %s, strategy %s [%u]", pwr, str, (unsigned)r[5]);
    else
        vs_put(v, key, label, ACCUDISC_VSET_OK, 0,
               "ON, power 0x%02X (unknown), strategy %s [%u]",
               (unsigned)r[3], str, (unsigned)r[5]);
}

/* A value describing the LOADED DISC, with none loaded. Not a failure and not
 * unanswered: the drive may well have returned bytes, but they describe
 * nothing, and printing them as a figure is how a stale number gets read as a
 * measurement. */
static void vs_nodisc(struct px_vs *v, const char *key, const char *label)
{
    vs_put(v, key, label, ACCUDISC_VSET_NO_DISC, 0, "no disc loaded");
}

static int px_settings_get(const accudisc_host *host,
                           const accudisc_drive_id *id,
                           const accudisc_disc_probe *disc,
                           accudisc_vendor_setting *out, uint32_t cap,
                           uint32_t *n)
{
    struct px_vs v = {host, out, cap, 0, 0};
    /* NULL probe = unknown: report the raw figures, without a unit claim. */
    int no_disc = disc && disc->reason == ACCUDISC_DISC_WHY_NO_MEDIUM;
    int is_dvd = disc && disc->profile >= 0x10 && disc->profile < 0x40;
    int is_cd = disc && disc->profile >= 0x08 && disc->profile <= 0x0A;
    uint8_t blk[PX_EEPROM_BLOCK];
    uint8_t r[PX_MODE_LEN];
    int rc;

    /* 1. EEPROM: TLA (block 0) and the life counters (block 1). QPxTool's
     * offsets are into the concatenated blocks — 0x29 for the TLA, 0x120.. for
     * the counters — so within block 1 they are 0x20.. */
    if (!px_eeprom_layout_known(id)) {
        static const char *const keys[][2] = {
            {"tla", "TLA"}, {"life.discs_loaded", "Discs loaded"},
            {"life.cd_read", "CD Rd time"}, {"life.cd_write", "CD Wr time"},
            {"life.dvd_read", "DVD Rd time"}, {"life.dvd_write", "DVD Wr time"}};
        for (unsigned i = 0; i < sizeof(keys) / sizeof(*keys); i++)
            vs_put(&v, keys[i][0], keys[i][1], 0, 0,
                   "not asked (EEPROM layout unknown for this model)");
    } else {
        memset(blk, 0, sizeof(blk));
        rc = vs_eeprom_block(&v, 0, blk);
        if (rc != ACCUDISC_OK) {
            vs_fail(&v, "tla", "TLA", rc);
        } else {
            char tla[5];
            for (int i = 0; i < 4; i++) {
                uint8_t c = blk[0x29 + i];
                tla[i] = (c >= 0x20 && c < 0x7f) ? (char)c : '?';
            }
            tla[4] = 0;
            vs_put(&v, "tla", "TLA", ACCUDISC_VSET_OK, 0, "%s", tla);
        }

        memset(blk, 0, sizeof(blk));
        rc = vs_eeprom_block(&v, 1, blk);
        if (rc != ACCUDISC_OK) {
            vs_fail(&v, "life.discs_loaded", "Discs loaded", rc);
            vs_fail(&v, "life.cd_read", "CD Rd time", rc);
            vs_fail(&v, "life.cd_write", "CD Wr time", rc);
            vs_fail(&v, "life.dvd_read", "DVD Rd time", rc);
            vs_fail(&v, "life.dvd_write", "DVD Wr time", rc);
        } else {
            uint32_t dn = be16(blk + 0x20);
            vs_put(&v, "life.discs_loaded", "Discs loaded",
                   ACCUDISC_VSET_OK | ACCUDISC_VSET_HAS_NUM, (int64_t)dn,
                   "%u", dn);
            vs_hms(&v, "life.cd_read", "CD Rd time", be32(blk + 0x22));
            vs_hms(&v, "life.cd_write", "CD Wr time", be32(blk + 0x26));
            vs_hms(&v, "life.dvd_read", "DVD Rd time", be32(blk + 0x2A));
            vs_hms(&v, "life.dvd_write", "DVD Wr time", be32(blk + 0x2E));
        }
    }

    /* 2. The feature pages, in cdvdcontrol -c's order. */
    memset(r, 0, sizeof(r));
    rc = vs_mode_get(&v, PX_PAGE_SS_HIDE, 0, r);
    vs_switch(&v, "hide_cdr", "Hide-CDR", rc, r[2] & 0x02);
    vs_switch(&v, "single_session", "SingleSession", rc, r[2] & 0x01);

    memset(r, 0, sizeof(r));
    rc = vs_mode_get(&v, PX_PAGE_SPEEDREAD, 0, r);
    vs_switch(&v, "speedread", "SpeedRead", rc, r[2]);

    memset(r, 0, sizeof(r));
    rc = v.dead ? PX_NOT_SENT : px_mode2(host, PX_MODE2_CODE_POWEREC,
                                         NULL, r);
    if (!v.dead && rc != ACCUDISC_OK && rc != ACCUDISC_ERR_SENSE)
        v.dead = 1;
    vs_switch(&v, "powerec", "PoweRec", rc, r[2] & PX_POWEREC_BIT);
    if (rc != ACCUDISC_OK) {
        vs_fail(&v, "powerec.recommended", "PoweRec speed", rc);
    } else if (no_disc) {
        vs_nodisc(&v, "powerec.recommended", "PoweRec speed");
    } else {
        uint32_t kbps = be16(r + 4);
        /* The drive's INTENTION for the loaded disc, not a delivered rate:
         * measured recommending 48x where the medium then took 25x. The X
         * factor follows the loaded profile, as cdvdcontrol's does (176 CD,
         * 1385 DVD); with the profile unknown no X is claimed. */
        if (is_cd || is_dvd)
            vs_put(&v, "powerec.recommended", "PoweRec speed",
                   ACCUDISC_VSET_OK | ACCUDISC_VSET_HAS_NUM, (int64_t)kbps,
                   "%u kB/s (%uX %s; an intention, not a rate)", kbps,
                   kbps / (is_dvd ? 1385u : 176u), is_dvd ? "DVD" : "CD");
        else
            vs_put(&v, "powerec.recommended", "PoweRec speed",
                   ACCUDISC_VSET_OK | ACCUDISC_VSET_HAS_NUM, (int64_t)kbps,
                   "%u kB/s (media type unknown; an intention, not a rate)",
                   kbps);
    }

    memset(r, 0, sizeof(r));
    rc = vs_mode_get(&v, PX_PAGE_GIGAREC, 0, r);
    if (rc != ACCUDISC_OK) {
        vs_fail(&v, "gigarec", "GigaRec", rc);
        vs_fail(&v, "gigarec.disc", "Disc GigaRec rate", rc);
    } else {
        vs_code(&v, "gigarec", "GigaRec", px_gigarec, r[3]);
        if (no_disc)
            vs_nodisc(&v, "gigarec.disc", "Disc GigaRec rate");
        else
            vs_code(&v, "gigarec.disc", "Disc GigaRec rate", px_gigarec, r[4]);
    }

    vs_varirec(&v, "varirec.cd", "VariRec CD", PX_VARIREC_CD);

    memset(r, 0, sizeof(r));
    rc = vs_mode_get(&v, PX_PAGE_SILENT, 0x04, r);
    if (rc != ACCUDISC_OK) {
        vs_fail(&v, "silent", "Silent mode", rc);
    } else if (!r[2]) {
        vs_put(&v, "silent", "Silent mode",
               ACCUDISC_VSET_OK | ACCUDISC_VSET_HAS_NUM, 0, "OFF");
    } else {
        /* ON matters to a burn: the CD write cap limits the rate whatever the
         * host asks for. [3]=read cap, [4]=write cap, [5]=access. */
        const char *rd = px_name(px_silent_cd_rd, r[3]);
        const char *wr = px_name(px_silent_cd_wr, r[4]);
        vs_put(&v, "silent", "Silent mode",
               ACCUDISC_VSET_OK | ACCUDISC_VSET_HAS_NUM, 1,
               "ON, CD read cap %s, CD write cap %s, access %s",
               rd ? rd : "?", wr ? wr : "?", r[5] ? "SLOW" : "FAST");
    }

    memset(r, 0, sizeof(r));
    rc = vs_mode_get(&v, PX_PAGE_SECUREC, 0, r);
    vs_switch(&v, "securec", "SecuRec", rc, r[3]);

    /* 3. The DVD-capable block. cdvdcontrol gates it on the drive writing
     * DVD; every model this driver can place in EEPROM does. Reported, not
     * acted on: DVD is outside AccuDisc's scope, a drive setting is not. */
    vs_varirec(&v, "varirec.dvd", "VariRec DVD", PX_VARIREC_DVD);

    memset(r, 0, sizeof(r));
    rc = vs_mode_get(&v, PX_PAGE_BITSET, PX_BITSET_R, r);
    vs_switch(&v, "bitset.dvd+r", "DVD+R bitsetting", rc, r[2] & 0x02);

    memset(r, 0, sizeof(r));
    rc = vs_mode_get(&v, PX_PAGE_BITSET, PX_BITSET_RDL, r);
    vs_switch(&v, "bitset.dvd+r_dl", "DVD+R DL bitsetting", rc, r[2] & 0x01);

    {
        uint8_t cdb[12] = {0};

        memset(r, 0, sizeof(r));
        cdb[0] = OP_PLEXTOR_AS_RD; /* CDB[1] stays 0: 0x01 = Media Check */
        cdb[10] = PX_MODE_LEN;
        rc = vs_exec(&v, cdb, r, PX_MODE_LEN, 10000);
        if (rc != ACCUDISC_OK) {
            vs_fail(&v, "autostrategy", "AutoStrategy", rc);
        } else {
            uint8_t m = (uint8_t)(r[2] & 0x0F);
            const char *nm = px_name(px_as_mode, m);
            vs_put(&v, "autostrategy", "AutoStrategy",
                   ACCUDISC_VSET_OK | ACCUDISC_VSET_HAS_NUM, (int64_t)m,
                   "%s [%u]", nm ? nm : "unknown", (unsigned)m);
        }
    }

    memset(r, 0, sizeof(r));
    rc = vs_mode_get(&v, PX_PAGE_TESTWRITE, 0, r);
    vs_switch(&v, "testwrite.dvd+", "DVD+R(W) testwrite", rc, r[2]);

    *n = v.n;
    return ACCUDISC_OK;
}

/* The disc-independent proof, tried first: read SpeedRead, SET it to the
 * value just read — the SET echoes the resulting page, which is the check —
 * and read it once more. Read/set/re-read of real device state, as driver.h
 * requires, with NO net change: the value written is the value found.
 *
 * Only a drive REFUSING the page (CHECK CONDITION) falls back to the counter
 * scan; a Plextor without SpeedRead may still have Q-Check. Any other failure
 * is the path itself failing, and a second vendor opcode would only repeat
 * the question into a transport that has already answered it. */
static int px_selftest(const accudisc_host *host)
{
    uint8_t r[PX_MODE_LEN] = {0};
    uint8_t cur;
    int rc = px_mode(host, PX_MODE_GET, PX_PAGE_SPEEDREAD, 0, r);

    if (rc == ACCUDISC_ERR_SENSE) {
        host->log(host->dev, "plextor: SpeedRead page refused; falling back "
                             "to the counter-scan selftest (needs a disc)");
        return px_selftest_cx(host);
    }
    if (rc != ACCUDISC_OK) {
        host->log(host->dev, "plextor: SpeedRead read failed in transport");
        return ACCUDISC_ERR_UNSUPPORTED;
    }
    cur = r[2] ? 1 : 0;

    memset(r, 0, sizeof(r));
    rc = px_mode(host, PX_MODE_SET, PX_PAGE_SPEEDREAD, cur, r);
    if (rc != ACCUDISC_OK || (r[2] ? 1 : 0) != cur) {
        host->log(host->dev, "plextor: SpeedRead set-to-current did not echo "
                             "back");
        return ACCUDISC_ERR_UNSUPPORTED;
    }

    memset(r, 0, sizeof(r));
    rc = px_mode(host, PX_MODE_GET, PX_PAGE_SPEEDREAD, 0, r);
    if (rc != ACCUDISC_OK || (r[2] ? 1 : 0) != cur) {
        host->log(host->dev, "plextor: SpeedRead re-read disagrees");
        return ACCUDISC_ERR_UNSUPPORTED;
    }
    return ACCUDISC_OK;
}

static const accudisc_driver plextor_driver = {
    .abi = ACCUDISC_DRIVER_ABI,
    .name = "plextor",
    .description = "Plextor extensions: C1/C2/CU error-counter scan (0xEA), "
                   "SpeedRead read-speed uncap (0xE9), POWEREC write-speed "
                   "governor (0xED), settings report (read-only)",
    .match = px_match,
    .selftest = px_selftest,
    .counter_scan_begin = px_begin,
    .counter_scan_read = px_read,
    .counter_scan_end = px_end,
    .speed_uncap_get = px_speed_uncap_get,
    .speed_uncap_set = px_speed_uncap_set,
    .write_governor_get = px_governor_get,
    .write_governor_set = px_governor_set,
    .settings_get = px_settings_get,
};

const accudisc_driver *accudisc_driver_entry(void)
{
    return &plextor_driver;
}
