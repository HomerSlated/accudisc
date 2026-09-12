/* Drive-readiness probe: which commands answer truthfully while the drive is
 * still evaluating a freshly inserted disc, and how long that window lasts.
 *
 * Purpose. AccuDisc has no readiness handling at all — TEST UNIT READY and
 * GET EVENT STATUS NOTIFICATION exist in this tree only as strings in the
 * trace decoder. The observable cost, measured 2026-09-12: the first
 * `accudisc disc` after a tray load answered `profile=0x0000
 * reason=not_cd_profile`, and `accudisc media` drew 2/04/01 NOT READY —
 * LOGICAL UNIT IS IN PROCESS OF BECOMING READY, for a perfectly good blank.
 *
 * MMC-5 §4.1.6.2 is normative about this and names the safe set:
 *
 *   "A Drive may become Busy ... While a Drive is Busy, it shall accept and
 *    process REQUEST SENSE, INQUIRY, GET CONFIGURATION, GET EVENT STATUS
 *    NOTIFICATION, and TEST UNIT READY. However, the Drive may not be able to
 *    process some commands and shall respond with CHECK CONDITION status with
 *    sense key set to NOT READY..."
 *
 * So this probe polls the five-command safe set alongside the two commands
 * `accudisc disc` actually uses, at a fixed cadence from the moment the tray
 * closes, and prints one row per tick. What we want out of it:
 *
 *   1. Which commands answer, and which answer CORRECTLY — those differ. On
 *      2026-09-12 GET CONFIGURATION was accepted during the busy window and
 *      returned profile 0x0000, i.e. a well-formed provisional answer. A
 *      readiness gate keyed on "did the command succeed?" sails straight
 *      through that, so the gate must key on CONTENT, not status.
 *   2. Whether this 2004 drive implements the Device Busy event class, whose
 *      descriptor carries a predicted time-to-ready in 100 ms units — the
 *      firmware telling us the cooldown instead of us guessing it. The event
 *      header's Supported Event Class byte answers this on the first tick.
 *   3. The real duration of the window, as a distribution, to set the
 *      cooldown and retry cap from measurement rather than from "sleep 8".
 *
 * On draining events. MMC-5 §6.7.1.4: "If Allocation Length is 4 or less, then
 * the Drive shall transfer Event Header only and shall not clear any event. An
 * event shall be considered reported for all Allocation Lengths greater than
 * 4." That matters because the Linux sr driver polls GESN for media change on
 * its own, and an event we consume is one the kernel does not see. So this
 * probe issues BOTH forms and labels them: a header-only read that is
 * guaranteed non-destructive, and the full read that carries Media Status and
 * the busy time. --no-drain omits the destructive pair entirely.
 *
 * Read-only: TEST UNIT READY, GESN, GET CONFIGURATION, MECHANISM STATUS and
 * READ DISC INFORMATION all leave drive state alone. The one state change is
 * the optional --load, which closes the tray so t=0 is well defined.
 *
 *   cmake --build build
 *   gcc -O2 -o build/readyprobe tools/readyprobe.c -I include -I src \
 *       build/src/libaccudisc.a -ldl
 *   doas /usr/bin/setcap cap_sys_rawio=ep build/readyprobe
 *   flock /var/tmp/sr0.lock ./build/readyprobe --load /dev/sr0
 */

#include <linux/cdrom.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>

#include <accudisc/accudisc.h>

#include "internal.h"
#include "transport/transport.h"

/* GESN Notification Class Request bits (MMC-5 Table 274). Bit 0 and bit 7 are
 * perpetually reserved; setting them is explicitly not an error. */
#define GESN_OP_CHANGE   0x02
#define GESN_POWER       0x04
#define GESN_EXTERNAL    0x08
#define GESN_MEDIA       0x10
#define GESN_MULTIHOST   0x20
#define GESN_DEVICE_BUSY 0x40
#define GESN_ALL         (GESN_OP_CHANGE | GESN_POWER | GESN_EXTERNAL \
                          | GESN_MEDIA | GESN_MULTIHOST | GESN_DEVICE_BUSY)

static double mono_now(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

/* Render a result as either a status word or the drive's own sense triple.
 * Both matter: "which command answered" and "what did it say when it did not"
 * are different questions and this probe exists to separate them. */
static void res(char *out, size_t cap, int rc, const adsc_cmd *c)
{
    if (rc == ACCUDISC_OK) {
        snprintf(out, cap, "GOOD");
    } else if (rc == ACCUDISC_ERR_SENSE && c->sense_len >= 14) {
        snprintf(out, cap, "%u/%02X/%02X", c->sense[2] & 0x0f, c->sense[12],
                 c->sense[13]);
    } else if (c->io_errno) {
        snprintf(out, cap, "errno=%d", c->io_errno);
    } else {
        snprintf(out, cap, "rc=%d", rc);
    }
}

static int exec(adsc_transport *t, adsc_cmd *c)
{
    /* A short timeout on purpose. The whole point is to see the drive answer
     * or refuse WHILE it is busy; a command that blocks for 20 s has already
     * destroyed the cadence this probe is measuring, and that fact is itself
     * a finding worth recording as a timeout rather than waited out. */
    c->timeout_ms = 5000;
    return adsc_transport_exec(t, c);
}

static int tur(adsc_transport *t, char *out, size_t cap)
{
    adsc_cmd c;
    int rc;

    memset(&c, 0, sizeof(c));
    c.cdb[0] = 0x00;
    c.cdb_len = 6;
    c.dir = ADSC_XFER_NONE;
    rc = exec(t, &c);
    res(out, cap, rc, &c);
    return rc;
}

/* GET EVENT STATUS NOTIFICATION. alloc <= 4 is the non-destructive form. */
static int gesn(adsc_transport *t, uint8_t classes, uint16_t alloc,
                uint8_t *buf, char *out, size_t cap)
{
    adsc_cmd c;
    int rc;

    memset(&c, 0, sizeof(c));
    memset(buf, 0, alloc);
    c.cdb[0] = 0x4A;
    c.cdb[1] = 0x01; /* Polled */
    c.cdb[4] = classes;
    c.cdb[7] = (uint8_t)(alloc >> 8);
    c.cdb[8] = (uint8_t)(alloc & 0xff);
    c.cdb_len = 10;
    c.dir = ADSC_XFER_IN;
    c.buf = buf;
    c.buf_len = alloc;
    rc = exec(t, &c);
    res(out, cap, rc, &c);
    return rc;
}

int main(int argc, char **argv)
{
    const char *path = "/dev/sr0";
    double secs = 60.0, interval = 0.25;
    int do_load = 0, drain = 1;
    adsc_transport t;
    uint8_t buf[64];
    char a[32], b[32], c[32], d[32], e[32], f[32];
    double t0;
    int tick = 0;

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--load"))
            do_load = 1;
        else if (!strcmp(argv[i], "--no-drain"))
            drain = 0;
        else if (!strcmp(argv[i], "--seconds") && i + 1 < argc)
            secs = atof(argv[++i]);
        else if (!strcmp(argv[i], "--interval-ms") && i + 1 < argc)
            interval = atof(argv[++i]) / 1000.0;
        else if (argv[i][0] != '-')
            path = argv[i];
        else {
            fprintf(stderr, "usage: readyprobe [--load] [--no-drain] "
                            "[--seconds N] [--interval-ms N] [DEVICE]\n");
            return 1;
        }
    }

    if (adsc_transport_open(&t, path, 0) != ACCUDISC_OK) {
        fprintf(stderr, "readyprobe: cannot open %s\n", path);
        return 1;
    }

    /* One GESN asking for EVERY class, before the clock starts: the response
     * header's Supported Event Class byte is the drive telling us which of
     * these classes exist on it at all. Without this the run cannot
     * distinguish "the drive is not busy" from "the drive does not implement
     * the Device Busy class", and those look identical downstream. */
    if (gesn(&t, GESN_ALL, 8, buf, a, sizeof a) == ACCUDISC_OK) {
        unsigned sup = buf[3], nea = (buf[2] >> 7) & 1, cls = buf[2] & 0x07;

        printf("# supported event classes 0x%02X ="
               "%s%s%s%s%s%s   NEA=%u  answering class=%u\n", sup,
               (sup & GESN_OP_CHANGE)   ? " opchange" : "",
               (sup & GESN_POWER)       ? " power" : "",
               (sup & GESN_EXTERNAL)    ? " external" : "",
               (sup & GESN_MEDIA)       ? " media" : "",
               (sup & GESN_MULTIHOST)   ? " multihost" : "",
               (sup & GESN_DEVICE_BUSY) ? " devicebusy" : "",
               nea, cls);
        if (!(sup & GESN_DEVICE_BUSY))
            printf("# NOTE: no Device Busy class — this drive will NOT report a"
                   " predicted time-to-ready;\n#       the cooldown has to come"
                   " from the measured window below.\n");
    } else {
        printf("# GESN(all classes) failed: %s — the drive may not implement"
               " 0x4A at all\n", a);
    }

    if (do_load) {
        printf("# closing the tray (CDROMCLOSETRAY); t=0 is the moment it"
               " returns\n");
        if (ioctl(t.fd, CDROMCLOSETRAY) < 0)
            printf("# CDROMCLOSETRAY failed — carrying on; t=0 is now\n");
    }

    printf("#\n# %7s  %-9s %-9s %-22s %-20s %-9s %-22s %-9s\n", "t_ms",
           "TUR", "GESNhdr", "GESN media (drains)", "GESN busy (drains)",
           "GETCONF", "MECHSTATUS", "DISCINFO");
    t0 = mono_now();

    while (mono_now() - t0 < secs) {
        double tms = (mono_now() - t0) * 1000.0;
        /* Wide enough for the longest rendering of each field AND for a
         * sense triple falling back into the same column. A truncated
         * diagnostic is a diagnostic that lies quietly, which is the whole
         * failure class this probe was built to expose. */
        char media[48] = "-", busy[48] = "-", prof[48] = "-", mech[48] = "-";
        adsc_cmd cmd;
        int rc;

        /* Order is least-invasive first, with the two commands `accudisc disc`
         * uses last, so a row shows what the safe set said BEFORE the
         * offending command had any chance to perturb the drive. */
        tur(&t, a, sizeof a);
        gesn(&t, GESN_ALL, 4, buf, b, sizeof b); /* header only: no event cleared */

        if (drain) {
            if (gesn(&t, GESN_MEDIA, 8, buf, c, sizeof c) == ACCUDISC_OK
                && buf[1] >= 4) {
                static const char *const ec[6] = {
                    "nochg", "ejectreq", "newmedia", "removal", "changed",
                    "bgdone" };
                unsigned code = buf[4] & 0x0f;

                snprintf(media, sizeof media, "%s pres=%u open=%u",
                         code < 6 ? ec[code] : "?", (buf[5] >> 1) & 1,
                         buf[5] & 1);
            } else {
                snprintf(media, sizeof media, "%s", c);
            }
            if (gesn(&t, GESN_DEVICE_BUSY, 8, buf, d, sizeof d) == ACCUDISC_OK
                && buf[1] >= 4) {
                snprintf(busy, sizeof busy, "%s busy=%u t=%.1fs",
                         (buf[4] & 0x0f) ? "change" : "nochg", buf[5],
                         ((unsigned)buf[6] << 8 | buf[7]) / 10.0);
            } else {
                snprintf(busy, sizeof busy, "%s", d);
            }
        }

        /* GET CONFIGURATION: in the safe set, so it ANSWERS while busy. The
         * question this row settles is whether what it answers is true yet. */
        memset(&cmd, 0, sizeof(cmd));
        memset(buf, 0, sizeof buf);
        cmd.cdb[0] = 0x46;
        cmd.cdb[1] = 0x00; /* RT 00b: all features from Starting Feature up */
        cmd.cdb[8] = 32;
        cmd.cdb_len = 10;
        cmd.dir = ADSC_XFER_IN;
        cmd.buf = buf;
        cmd.buf_len = 32;
        rc = exec(&t, &cmd);
        res(e, sizeof e, rc, &cmd);
        if (rc == ACCUDISC_OK)
            snprintf(prof, sizeof prof, "0x%04X",
                     (unsigned)buf[6] << 8 | buf[7]);
        else
            snprintf(prof, sizeof prof, "%s", e);

        /* MECHANISM STATUS: a pure state read with no event queue behind it,
         * so unlike GESN it cannot consume anything the kernel wanted. */
        memset(&cmd, 0, sizeof(cmd));
        memset(buf, 0, sizeof buf);
        cmd.cdb[0] = 0xBD;
        cmd.cdb[8] = 0;
        cmd.cdb[9] = 8;
        cmd.cdb_len = 12;
        cmd.dir = ADSC_XFER_IN;
        cmd.buf = buf;
        cmd.buf_len = 8;
        rc = exec(&t, &cmd);
        res(e, sizeof e, rc, &cmd);
        if (rc == ACCUDISC_OK) {
            static const char *const ms[8] = { "idle", "playing", "scanning",
                                               "active", "?", "?", "?",
                                               "nostate" };
            snprintf(mech, sizeof mech, "%s door=%u chg=%u",
                     ms[(buf[1] >> 5) & 0x07], (buf[1] >> 4) & 1,
                     (buf[0] >> 5) & 0x03);
        } else {
            snprintf(mech, sizeof mech, "%s", e);
        }

        /* READ DISC INFORMATION — NOT in the safe set, and the command that
         * actually drew 2/04/01 on 2026-09-12. Issued last, every tick. */
        memset(&cmd, 0, sizeof(cmd));
        memset(buf, 0, sizeof buf);
        cmd.cdb[0] = 0x51;
        cmd.cdb[7] = 0;
        cmd.cdb[8] = 34;
        cmd.cdb_len = 10;
        cmd.dir = ADSC_XFER_IN;
        cmd.buf = buf;
        cmd.buf_len = 34;
        rc = exec(&t, &cmd);
        res(f, sizeof f, rc, &cmd);

        printf("  %7.0f  %-9s %-9s %-22s %-20s %-9s %-22s %-9s\n", tms, a, b,
               media, busy, prof, mech, f);
        fflush(stdout);
        tick++;
        usleep((useconds_t)(interval * 1e6));
    }

    printf("# %d ticks over %.1f s\n", tick, mono_now() - t0);
    adsc_transport_close(&t);
    return 0;
}
