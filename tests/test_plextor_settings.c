/* The Plextor settings report (0.46.0): decode, safety, and failure shape.
 *
 * WHAT THIS CAN AND CANNOT PROVE. The fixtures below encode QPxTool's reading
 * of the PX-716A's responses, and so does the driver — both came from the same
 * source. A pass therefore proves the code is SELF-CONSISTENT with that
 * reading, not that the reading is right. The real gate is a back-to-back run
 * against `cdvdcontrol -c` on the drive itself, field by field. What this file
 * does own outright is the safety and failure behaviour, which no hardware run
 * should ever be the first to exercise:
 *
 *   - no command it sends is a SET, a reset, an erase or a Media Check
 *     (and the guard that says so is shown to FIRE before it is trusted);
 *   - a transport failure stops all further commands;
 *   - a refused query leaves its entry unanswered, never OFF;
 *   - a model whose EEPROM layout is unknown gets no EEPROM read at all;
 *   - a short caller array is never overrun;
 *   - an ABI-4 driver's descriptor is never read past its end.
 *
 * The expected values are Keith's `cdvdcontrol -d /dev/sr0 -c` of the new
 * PX-716A on 2026-09-28, compared by exact equality: 1349 loads, CD Rd
 * 30:15:46, CD Wr 9:45:21, DVD Rd 72:44:39, DVD Wr 36:22:03, TLA 0309, PoweRec
 * ON at 48X, DVD+R DL bitsetting ON, AutoStrategy AUTO [1], all else OFF. */

#include <stdio.h>
#include <string.h>

#include "internal.h"

const accudisc_driver *accudisc_driver_entry(void);

static int fails;

#define CHECK(cond, ...)                                                      \
    do {                                                                      \
        if (!(cond)) {                                                        \
            printf("FAIL %s:%d: ", __FILE__, __LINE__);                       \
            printf(__VA_ARGS__);                                              \
            printf("\n");                                                     \
            fails++;                                                          \
        }                                                                     \
    } while (0)

/* ---- the forbidden-command guard ---------------------------------------- */

/* Anything a READ-ONLY report must never send. Returns a reason, or NULL. */
static const char *forbidden(const uint8_t *cdb)
{
    switch (cdb[0]) {
    case 0xE9: return cdb[1] != 0x00 ? "0xE9 with a non-GET direction" : NULL;
    case 0xED: return cdb[1] != 0x00 ? "0xED SET form" : NULL;
    case 0xE4: return cdb[1] != 0x00 ? "0xE4 sub-command (Media Check)" : NULL;
    case 0xF1: return cdb[1] != 0x01 ? "0xF1 other than the block form" : NULL;
    case 0xEE: return "0xEE drive reset";
    case 0xE3: return "0xE3 PlexEraser";
    case 0xE5: return "0xE5 AutoStrategy write";
    case 0xD5: return "0xD5 SecuRec SEND_AUTH";
    case 0xEA: return "0xEA counter scan (not part of this report)";
    default:   return "opcode outside the report's query set";
    }
}

/* ---- mock host ----------------------------------------------------------- */

struct mock {
    int calls;
    int io_fail_at;      /* 1-based call that fails in transport; 0 = never */
    uint8_t sense_op;    /* opcode+page that answers CHECK CONDITION */
    uint8_t sense_page;
    int varirec_cd_on;   /* answer VariREC CD as ON, -2, Cyanine */
    int allow_set;       /* the selftest's SpeedRead set-to-current */
    uint8_t speedread;   /* the drive's SpeedRead state */
    uint8_t sets[4];     /* values the selftest SET, in order */
    int nsets;
    uint8_t ops[16];     /* opcodes seen, in order (first 16) */
    const char *bad;     /* first forbidden command seen */
    uint8_t bad_cdb[12]; /* ...and the command itself */
};

static struct mock M;

static void put32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
}

static int mock_exec(void *dev, const uint8_t *cdb, uint8_t cdb_len,
                     accudisc_host_dir dir, void *buf, uint32_t len,
                     uint32_t timeout_ms)
{
    uint8_t *r = buf;
    const char *why = forbidden(cdb);

    (void)dev;
    (void)timeout_ms;
    M.calls++;
    if (M.calls <= 16)
        M.ops[M.calls - 1] = cdb[0];
    if (M.allow_set && cdb[0] == 0xE9 && cdb[1] == 0x10 && cdb[2] == 0xBB) {
        why = NULL;
        if (M.nsets < 4)
            M.sets[M.nsets++] = cdb[3];
    }
    if (M.allow_set && cdb[0] == 0xEA)
        why = NULL; /* the fallback selftest's counter scan */
    if (why && !M.bad) {
        M.bad = why;
        memcpy(M.bad_cdb, cdb, 12);
    }
    if (cdb_len != 12 || dir != ACCUDISC_HOST_IN)
        M.bad = M.bad ? M.bad : "not a 12-byte data-IN command";
    if (M.io_fail_at && M.calls == M.io_fail_at)
        return ACCUDISC_ERR_IO;
    if (M.sense_op && cdb[0] == M.sense_op &&
        (cdb[0] != 0xE9 || cdb[2] == M.sense_page))
        return ACCUDISC_ERR_SENSE;
    if (r && len)
        memset(r, 0, len);

    switch (cdb[0]) {
    case 0xF1:
        if (len != 256 || cdb[8] != 0x01 || cdb[9] != 0x00)
            M.bad = M.bad ? M.bad : "EEPROM block not 256 bytes";
        if (cdb[7] == 0)
            memcpy(r + 0x29, "0309", 4);
        if (cdb[7] == 1) {
            r[0x20] = 1349 >> 8;
            r[0x21] = 1349 & 0xff;
            put32(r + 0x22, 108946); /* 30:15:46 */
            put32(r + 0x26, 35121);  /*  9:45:21 */
            put32(r + 0x2A, 261879); /* 72:44:39 */
            put32(r + 0x2E, 130923); /* 36:22:03 */
        }
        break;
    case 0xED: /* PoweRec ON, 8467 kB/s = 48X */
        r[2] = 0x01;
        r[4] = 8467 >> 8;
        r[5] = 8467 & 0xff;
        break;
    case 0xE4: /* AutoStrategy AUTO */
        r[2] = 0x01;
        break;
    case 0xE9:
        r[0] = cdb[2];
        r[1] = 0x06;
        if (cdb[2] == 0x08) { /* measured 2026-09-09: 08 06 00 04 08 00 19 0d */
            static const uint8_t sil[8] = {0x08, 0x06, 0x00, 0x04,
                                           0x08, 0x00, 0x19, 0x0d};
            memcpy(r, sil, 8);
        }
        if (cdb[2] == 0xBB) {
            if (cdb[1] == 0x10)
                M.speedread = cdb[3]; /* SET echoes the resulting page */
            r[2] = M.speedread;
        }
        if (cdb[2] == 0x22 && cdb[3] == 0x0E) /* DVD+R DL bitsetting ON */
            r[2] = 0x01;
        if (cdb[2] == 0x02 && cdb[3] == 0x02 && M.varirec_cd_on) {
            r[2] = 0x01;
            r[3] = 0x82; /* -2 */
            r[5] = 2;    /* Cyanine */
        }
        break;
    }
    return ACCUDISC_OK;
}

static void mock_log(void *dev, const char *msg)
{
    (void)dev;
    (void)msg;
}

static const accudisc_host HOST = {NULL, mock_exec, mock_log};

static const accudisc_drive_id PX716 = {"PLEXTOR", "DVDR   PX-716A", "1.11"};
static const accudisc_drive_id PX712 = {"PLEXTOR", "DVDR   PX-712A", "1.09"};

static const accudisc_vendor_setting *find(const accudisc_vendor_setting *v,
                                           uint32_t n, const char *key)
{
    for (uint32_t i = 0; i < n; i++)
        if (!strcmp(v[i].key, key))
            return &v[i];
    return NULL;
}

static void expect(const accudisc_vendor_setting *v, uint32_t n,
                   const char *key, const char *value)
{
    const accudisc_vendor_setting *e = find(v, n, key);

    CHECK(e, "key %s missing", key);
    if (!e)
        return;
    CHECK(e->flags & ACCUDISC_VSET_OK, "%s not OK: %s", key, e->value);
    CHECK(!strcmp(e->value, value), "%s = '%s', want '%s'", key, e->value,
          value);
}

static void expect_num(const accudisc_vendor_setting *v, uint32_t n,
                       const char *key, int64_t num)
{
    const accudisc_vendor_setting *e = find(v, n, key);

    CHECK(e && (e->flags & ACCUDISC_VSET_HAS_NUM) && e->num == num,
          "%s num = %lld, want %lld", key, e ? (long long)e->num : -1LL,
          (long long)num);
}

/* The disc in the tray for Keith's cdvdcontrol run: burn 2's CD-R. */
static const accudisc_disc_probe CDR_AUDIO = {
    .profile = 0x0009, .erasable = 0, .disc_status = 2, .audio_tracks = 11,
    .kind = ACCUDISC_DISC_AUDIO, .reason = ACCUDISC_DISC_WHY_AUDIO};
static const accudisc_disc_probe EMPTY = {
    .profile = 0, .erasable = ACCUDISC_DISC_STATUS_UNKNOWN,
    .disc_status = ACCUDISC_DISC_STATUS_UNKNOWN,
    .kind = ACCUDISC_DISC_NEITHER, .reason = ACCUDISC_DISC_WHY_NO_MEDIUM,
    .tray = ACCUDISC_TRAY_CLOSED};
static const accudisc_disc_probe DVD = {
    .profile = 0x0011, .kind = ACCUDISC_DISC_NEITHER,
    .reason = ACCUDISC_DISC_WHY_NOT_CD_PROFILE};

static const accudisc_disc_probe *TRAY = &CDR_AUDIO;

static uint32_t run(const accudisc_drive_id *id, accudisc_vendor_setting *v,
                    uint32_t cap)
{
    const accudisc_driver *d = accudisc_driver_entry();
    uint32_t n = 0;
    int rc = d->settings_get(&HOST, id, TRAY, v, cap, &n);

    CHECK(rc == ACCUDISC_OK, "settings_get rc %d", rc);
    return n;
}

/* ---- ABI-4 gating ------------------------------------------------------- */

static int abi4_slot_called;

static int abi4_settings(const accudisc_host *h, const accudisc_drive_id *id,
                         const accudisc_disc_probe *dp,
                         accudisc_vendor_setting *o, uint32_t c, uint32_t *n)
{
    (void)h, (void)id, (void)dp, (void)o, (void)c, (void)n;
    abi4_slot_called = 1;
    return ACCUDISC_OK;
}

int main(void)
{
    accudisc_vendor_setting v[ACCUDISC_VENDOR_SETTINGS_MAX];
    uint32_t n;

    /* 0. THE GUARD FIRES. Before any result below is trusted, each command a
     * read-only report must never send is shown to be caught. Remove a case
     * from forbidden() and this block fails, not the rest silently passing. */
    {
        static const uint8_t bad[][12] = {
            {0xE9, 0x10, 0xBB, 0x01}, /* SpeedRead SET */
            {0xED, 0x11, 0x00},       /* PoweRec SET on */
            {0xE4, 0x01, 0x00},       /* Media Quality Check start */
            {0xF1, 0x00},             /* EEPROM non-block form */
            {0xEE},                   /* reset */
            {0xE3},                   /* PlexEraser */
            {0xE5},                   /* AutoStrategy write */
            {0xD5},                   /* SecuRec password */
        };
        for (unsigned i = 0; i < sizeof(bad) / sizeof(*bad); i++)
            CHECK(forbidden(bad[i]) != NULL,
                  "guard missed forbidden CDB %02X %02X", bad[i][0],
                  bad[i][1]);
    }

    /* 1. The full report against Keith's cdvdcontrol output. */
    memset(&M, 0, sizeof(M));
    n = run(&PX716, v, ACCUDISC_VENDOR_SETTINGS_MAX);
    CHECK(!M.bad, "forbidden command sent: %s (CDB %02X %02X %02X)",
          M.bad ? M.bad : "", M.bad_cdb[0], M.bad_cdb[1], M.bad_cdb[2]);
    CHECK(n == 21, "entry count %u, want 21", n);
    CHECK(M.calls == 14, "command count %d, want 14", M.calls);
    expect(v, n, "tla", "0309");
    expect(v, n, "life.discs_loaded", "1349");
    expect_num(v, n, "life.discs_loaded", 1349);
    expect(v, n, "life.cd_read", "30:15:46");
    expect(v, n, "life.cd_write", "9:45:21");
    expect_num(v, n, "life.cd_write", 35121);
    expect(v, n, "life.dvd_read", "72:44:39");
    expect(v, n, "life.dvd_write", "36:22:03");
    expect(v, n, "hide_cdr", "OFF");
    expect(v, n, "single_session", "OFF");
    expect(v, n, "speedread", "OFF");
    expect(v, n, "powerec", "ON");
    expect_num(v, n, "powerec.recommended", 8467);
    {
        const accudisc_vendor_setting *e = find(v, n, "powerec.recommended");
        CHECK(e && strstr(e->value, "(48X CD"), "PoweRec speed '%s' not 48X",
              e ? e->value : "");
    }
    expect(v, n, "gigarec", "OFF");
    expect(v, n, "gigarec.disc", "OFF");
    expect(v, n, "varirec.cd", "OFF");
    expect(v, n, "varirec.dvd", "OFF");
    expect(v, n, "silent", "OFF");
    expect(v, n, "securec", "OFF");
    expect(v, n, "bitset.dvd+r", "OFF");
    expect(v, n, "bitset.dvd+r_dl", "ON");
    expect(v, n, "autostrategy", "AUTO [1]");
    expect(v, n, "testwrite.dvd+", "OFF");

    /* 2. VariREC ON decodes power and strategy, with the signed step. */
    memset(&M, 0, sizeof(M));
    M.varirec_cd_on = 1;
    n = run(&PX716, v, ACCUDISC_VENDOR_SETTINGS_MAX);
    expect(v, n, "varirec.cd", "ON, power -2, strategy Cyanine [2]");
    expect_num(v, n, "varirec.cd", -2);

    /* 3. A refused query is unanswered — never OFF — and the rest go on. */
    memset(&M, 0, sizeof(M));
    M.sense_op = 0xE9;
    M.sense_page = 0xD5;
    n = run(&PX716, v, ACCUDISC_VENDOR_SETTINGS_MAX);
    {
        const accudisc_vendor_setting *e = find(v, n, "securec");
        CHECK(e && !(e->flags & ACCUDISC_VSET_OK) &&
                  strstr(e->value, "refused"),
              "refused SecuRec shown as '%s'", e ? e->value : "(missing)");
    }
    expect(v, n, "testwrite.dvd+", "OFF"); /* later queries still asked */
    CHECK(M.calls == 14, "a CHECK CONDITION stopped the report (%d calls)",
          M.calls);

    /* 4. A transport failure stops every further command. Call 3 is the first
     * mode page (Hide-CDR/SingleSession), after the two EEPROM blocks. */
    memset(&M, 0, sizeof(M));
    M.io_fail_at = 3;
    n = run(&PX716, v, ACCUDISC_VENDOR_SETTINGS_MAX);
    CHECK(M.calls == 3, "commands sent after a transport failure: %d total",
          M.calls);
    CHECK(n == 21, "entries after a transport failure %u, want all 21", n);
    {
        const accudisc_vendor_setting *e = find(v, n, "autostrategy");
        CHECK(e && !(e->flags & ACCUDISC_VSET_OK) &&
                  strstr(e->value, "not asked"),
              "post-failure AutoStrategy shown as '%s'",
              e ? e->value : "(missing)");
    }
    expect(v, n, "life.cd_write", "9:45:21"); /* before the failure: kept */
    {
        /* The query that FAILED is not "not asked": which command wedged the
         * bridge is the most useful thing a flaky-bridge diagnosis can see. */
        const accudisc_vendor_setting *e = find(v, n, "hide_cdr");
        CHECK(e && !(e->flags & ACCUDISC_VSET_OK) &&
                  strstr(e->value, "transport") &&
                  !strstr(e->value, "not asked"),
              "the failing query shown as '%s'", e ? e->value : "(missing)");
    }

    /* 5. Unknown EEPROM layout: no 0xF1 at all, life keys unanswered. */
    memset(&M, 0, sizeof(M));
    n = run(&PX712, v, ACCUDISC_VENDOR_SETTINGS_MAX);
    CHECK(M.calls == 12, "PX-712 command count %d, want 12 (no EEPROM)",
          M.calls);
    {
        const accudisc_vendor_setting *e = find(v, n, "life.cd_write");
        CHECK(e && !(e->flags & ACCUDISC_VSET_OK),
              "PX-712 life counter answered from a guessed layout");
    }

    /* 6. A short array is filled to cap and no further; n is the total. */
    {
        accudisc_vendor_setting small[4 + 1];
        memset(small, 0xA5, sizeof(small));
        memset(&M, 0, sizeof(M));
        n = run(&PX716, small, 4);
        CHECK(n == 21, "truncated call reported %u, want 21", n);
        CHECK(((uint8_t *)&small[4])[0] == 0xA5 &&
                  ((uint8_t *)&small[4])[sizeof(small[4]) - 1] == 0xA5,
              "wrote past cap");
    }

    /* 7a. EMPTY TRAY (Keith, 2026-09-28: "with and without a disc"). The
     * drive's settings are all still answered; the two values that describe
     * the loaded disc say so instead of printing a figure. Same 14 commands. */
    memset(&M, 0, sizeof(M));
    TRAY = &EMPTY;
    n = run(&PX716, v, ACCUDISC_VENDOR_SETTINGS_MAX);
    CHECK(!M.bad, "empty tray: forbidden command sent: %s", M.bad ? M.bad : "");
    CHECK(n == 21 && M.calls == 14, "empty tray: %u entries, %d commands", n,
          M.calls);
    expect(v, n, "powerec", "ON");
    expect(v, n, "gigarec", "OFF");
    expect(v, n, "life.cd_write", "9:45:21");
    {
        static const char *const disc_keys[] = {"powerec.recommended",
                                                "gigarec.disc"};
        for (unsigned i = 0; i < 2; i++) {
            const accudisc_vendor_setting *e = find(v, n, disc_keys[i]);
            CHECK(e && e->flags == ACCUDISC_VSET_NO_DISC &&
                      !strcmp(e->value, "no disc loaded"),
                  "empty tray: %s flags %u value '%s'", disc_keys[i],
                  e ? e->flags : 0u, e ? e->value : "(missing)");
        }
        uint32_t nd = 0;
        for (uint32_t i = 0; i < n; i++)
            nd += (v[i].flags & ACCUDISC_VSET_NO_DISC) ? 1u : 0u;
        CHECK(nd == 2, "empty tray: %u no-disc entries, want exactly 2", nd);
    }

    /* 7b. The PoweRec X factor follows the loaded profile; unknown claims none. */
    memset(&M, 0, sizeof(M));
    TRAY = &DVD;
    n = run(&PX716, v, ACCUDISC_VENDOR_SETTINGS_MAX);
    {
        const accudisc_vendor_setting *e = find(v, n, "powerec.recommended");
        CHECK(e && strstr(e->value, "(6X DVD"), "DVD PoweRec shown as '%s'",
              e ? e->value : "(missing)");
    }
    memset(&M, 0, sizeof(M));
    TRAY = NULL;
    n = run(&PX716, v, ACCUDISC_VENDOR_SETTINGS_MAX);
    {
        const accudisc_vendor_setting *e = find(v, n, "powerec.recommended");
        CHECK(e && (e->flags & ACCUDISC_VSET_OK) && strstr(e->value, "unknown")
                  && !strstr(e->value, "X "),
              "unknown-media PoweRec shown as '%s'", e ? e->value : "");
    }
    TRAY = &CDR_AUDIO;

    /* 8. SELFTEST, disc-independent: GET, SET-to-current, GET — on SpeedRead,
     * with the value written equal to the value found, for both states. */
    {
        const accudisc_driver *d = accudisc_driver_entry();
        for (uint8_t st = 0; st < 2; st++) {
            memset(&M, 0, sizeof(M));
            M.allow_set = 1;
            M.speedread = st;
            int rc = d->selftest(&HOST);
            CHECK(rc == ACCUDISC_OK && M.calls == 3 && M.nsets == 1 &&
                      M.sets[0] == st && M.speedread == st && !M.bad,
                  "selftest SpeedRead=%u: rc %d, %d cmds, %d sets, wrote %u",
                  st, rc, M.calls, M.nsets, M.sets[0]);
            CHECK(M.ops[0] == 0xE9 && M.ops[1] == 0xE9 && M.ops[2] == 0xE9,
                  "selftest used opcodes %02X %02X %02X", M.ops[0], M.ops[1],
                  M.ops[2]);
        }

        /* Refused page -> the counter-scan fallback (0xEA arm/read/end). */
        memset(&M, 0, sizeof(M));
        M.allow_set = 1;
        M.sense_op = 0xE9;
        M.sense_page = 0xBB;
        int rc = d->selftest(&HOST);
        CHECK(rc == ACCUDISC_OK && M.calls == 4 && M.ops[1] == 0xEA &&
                  M.ops[3] == 0xEA,
              "fallback selftest: rc %d, %d cmds, ops %02X..%02X", rc,
              M.calls, M.ops[1], M.ops[3]);

        /* A transport failure is NOT a refusal: no fallback, no more opcodes. */
        memset(&M, 0, sizeof(M));
        M.allow_set = 1;
        M.io_fail_at = 1;
        rc = d->selftest(&HOST);
        CHECK(rc == ACCUDISC_ERR_UNSUPPORTED && M.calls == 1,
              "selftest after transport failure: rc %d, %d cmds", rc,
              M.calls);
    }

    /* 9. The library never reads settings_get from an ABI-4 descriptor, and
     * refuses a caller whose element size differs from this build's. */
    {
        struct accudisc_device dev;
        accudisc_driver fake;
        uint32_t cnt = 99;
        int rc;

        memset(&dev, 0, sizeof(dev));
        memset(&fake, 0, sizeof(fake));
        fake.abi = 4;
        fake.settings_get = abi4_settings; /* beyond an ABI-4 descriptor */
        dev.drv = &fake;
        rc = accudisc_vendor_settings(&dev, v, sizeof(v[0]),
                                      ACCUDISC_VENDOR_SETTINGS_MAX, &cnt);
        CHECK(rc == ACCUDISC_ERR_UNSUPPORTED && !abi4_slot_called &&
                  cnt == 0,
              "ABI-4 driver: rc %d, slot called %d, n %u", rc,
              abi4_slot_called, cnt);

        /* ABI 5 is SKIPPED (driver.h): the only ABI-5 drivers that ever
         * existed have a settings_get WITHOUT the disc argument. Calling one
         * would hand it the probe struct as its output array. */
        fake.abi = 5;
        rc = accudisc_vendor_settings(&dev, v, sizeof(v[0]),
                                      ACCUDISC_VENDOR_SETTINGS_MAX, &cnt);
        CHECK(rc == ACCUDISC_ERR_UNSUPPORTED && !abi4_slot_called,
              "stale ABI-5 driver: rc %d, slot called %d", rc,
              abi4_slot_called);

        fake.abi = 6;
        rc = accudisc_vendor_settings(&dev, v, sizeof(v[0]) - 8,
                                      ACCUDISC_VENDOR_SETTINGS_MAX, &cnt);
        CHECK(rc == ACCUDISC_ERR_ABI && !abi4_slot_called,
              "wrong elem_size: rc %d", rc);
    }

    if (fails) {
        printf("test_plextor_settings: %d failure(s)\n", fails);
        return 1;
    }
    printf("test_plextor_settings: ok\n");
    return 0;
}
