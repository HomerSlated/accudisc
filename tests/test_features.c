/* accudisc_probe_features against a fake drive in each tray state (0.47.0).
 *
 * The defect this pins: the functional smoke reads are CD-DA reads, and until
 * 0.47.0 they were sent whatever was loaded. With an empty tray they failed
 * for want of a disc and were reported as five failed combos; with a blank or
 * a data disc the verdict was a confident C2_UNSUPPORTED about a drive that
 * had not been asked anything it could answer. Both are well-formed, so only
 * a fake that can be in each state tells the right answer from the wrong one.
 *
 * Every case asserts three things that must agree: what `medium` says, what
 * the verdict says, and HOW MANY READ CDs REACHED THE DRIVE. The last is the
 * one a report alone cannot fake — "not applicable" has to mean "not sent".
 * adsc_dev_exec is replaced with -Wl,--wrap. */

#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "internal.h"
#include "mmc/cdb.h"

enum tray {
    EMPTY,    /* no disc: READ DISC INFORMATION answers 2/3A/01 */
    BLANK,    /* CD-R, disc status 0, no TOC */
    DATA,     /* pressed CD-ROM, one data track */
    AUDIO,    /* three audio tracks from LBA 0 */
    MIXED,    /* data track at 0, audio from 15000 */
    DVD,      /* a DVD profile: not a CD at all */
    BUSY      /* audio disc still becoming ready: profile 0, 2/04/01 */
};

#define MIXED_AUDIO_LBA 15000u

static struct {
    enum tray tray;
    int c2_works;        /* READ CD with C2 succeeds on audio */
    int reads;           /* READ CD commands that reached the drive */
    uint32_t read_lba;   /* LBA of the first one */
    int governor_asked;
} fk;

static int sense(struct accudisc_device *dev, uint8_t key, uint8_t asc,
                 uint8_t ascq)
{
    dev->last_sense.valid = 1;
    dev->last_sense.key = key;
    dev->last_sense.asc = asc;
    dev->last_sense.ascq = ascq;
    return ACCUDISC_ERR_SENSE;
}

static void put32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
}

static void out(adsc_cmd *cmd, const uint8_t *src, size_t n)
{
    memset(cmd->buf, 0, cmd->buf_len);
    memcpy(cmd->buf, src, n < cmd->buf_len ? n : cmd->buf_len);
}

int __real_adsc_dev_exec(struct accudisc_device *dev, adsc_cmd *cmd);
int __wrap_adsc_dev_exec(struct accudisc_device *dev, adsc_cmd *cmd)
{
    uint8_t r[64] = {0};

    cmd->resid = 0;
    switch (cmd->cdb[0]) {
    case 0x46: { /* GET CONFIGURATION */
        unsigned feat = ((unsigned)cmd->cdb[2] << 8) | cmd->cdb[3];
        uint16_t profile = fk.tray == EMPTY || fk.tray == BUSY ? 0
                         : fk.tray == BLANK ? 0x0009
                         : fk.tray == DVD ? 0x0010 : 0x0008;

        r[6] = (uint8_t)(profile >> 8);
        r[7] = (uint8_t)profile;
        if (feat == 0x001E) {
            r[9] = 0x1E;
            r[10] = fk.tray == AUDIO || fk.tray == MIXED;
            r[12] = 0x03; /* C2 flags + CD-Text claimed */
        } else if (feat == 0x002E) {
            r[9] = 0x2E;
            r[10] = fk.tray == BLANK;
            r[12] = 0x64; /* BUF, SAO, Test Write */
        }
        out(cmd, r, sizeof r);
        return ACCUDISC_OK;
    }
    case 0x51: /* READ DISC INFORMATION */
        if (fk.tray == EMPTY)
            return sense(dev, 0x02, 0x3a, 0x01);
        if (fk.tray == BUSY)
            return sense(dev, 0x02, 0x04, 0x01);
        r[1] = 32;
        r[2] = fk.tray == BLANK ? 0x00 : 0x0e; /* complete, last session complete */
        out(cmd, r, 34);
        return ACCUDISC_OK;
    case 0x43: { /* READ TOC, format 0 */
        size_t n = 4;

        if (fk.tray == EMPTY)
            return sense(dev, 0x02, 0x3a, 0x01);
        if (fk.tray == BUSY)
            return sense(dev, 0x02, 0x04, 0x01);
        if (fk.tray == BLANK)
            return sense(dev, 0x05, 0x24, 0x00);
        if ((cmd->cdb[2] & 0x0f) != 0)
            return sense(dev, 0x05, 0x24, 0x00); /* only format 0 modelled */
#define TRK(num, ctrl, lba) do { r[n + 1] = (ctrl); r[n + 2] = (num); \
                                 put32(r + n + 4, (lba)); n += 8; } while (0)
        if (fk.tray == AUDIO) {
            TRK(1, 0x10, 0); TRK(2, 0x10, 20000); TRK(3, 0x10, 40000);
        } else if (fk.tray == MIXED) {
            TRK(1, 0x14, 0); TRK(2, 0x10, MIXED_AUDIO_LBA);
        } else { /* DATA, DVD */
            TRK(1, 0x14, 0);
        }
        TRK(0xAA, 0x10, 60000);
#undef TRK
        r[0] = 0;
        r[1] = (uint8_t)(n - 2);
        r[2] = 1;
        r[3] = (uint8_t)((n - 4) / 8 - 1);
        out(cmd, r, n);
        return ACCUDISC_OK;
    }
    case 0xBE: { /* READ CD */
        uint32_t lba = (uint32_t)cmd->cdb[2] << 24 | (uint32_t)cmd->cdb[3] << 16 |
                       (uint32_t)cmd->cdb[4] << 8 | cmd->cdb[5];
        int wants_c2 = (cmd->cdb[9] >> 1) & 3;

        if (fk.reads++ == 0)
            fk.read_lba = lba;
        if (fk.tray == EMPTY)
            return sense(dev, 0x02, 0x3a, 0x01);
        if (fk.tray == BUSY)
            return sense(dev, 0x02, 0x04, 0x01);
        if (fk.tray == BLANK)
            return sense(dev, 0x05, 0x21, 0x00);
        if (fk.tray == DATA || fk.tray == DVD ||
            (fk.tray == MIXED && lba < MIXED_AUDIO_LBA))
            return sense(dev, 0x05, 0x64, 0x00); /* ILLEGAL MODE FOR THIS TRACK */
        if (wants_c2 && !fk.c2_works)
            return sense(dev, 0x05, 0x24, 0x00);
        memset(cmd->buf, 0, cmd->buf_len);
        return ACCUDISC_OK;
    }
    default:
        if (cmd->dir == ADSC_XFER_IN && cmd->buf)
            memset(cmd->buf, 0, cmd->buf_len);
        return ACCUDISC_OK;
    }
}

static struct accudisc_device dev;

/* A vendor driver that answers the governor question the way the PX-716A
 * does: ON, and a recommended rate whatever is in the tray. */
static int fake_governor_get(const accudisc_host *host, int *on,
                             uint32_t *recommended_kbps)
{
    (void)host;
    *on = 1;
    *recommended_kbps = 8467;
    return ACCUDISC_OK;
}

static const accudisc_driver FAKE_DRIVER = {
    .write_governor_get = fake_governor_get,
};
static int with_driver;

static accudisc_features probe(enum tray tray, int c2_works)
{
    accudisc_features f;

    memset(&dev, 0, sizeof dev);
    if (with_driver)
        dev.drv = &FAKE_DRIVER;
    memset(&fk, 0, sizeof fk);
    fk.tray = tray;
    fk.c2_works = c2_works;
    memset(&f, 0xff, sizeof f); /* a field the probe forgets shows as 0xff */
    assert(accudisc_probe_features(&dev, &f) == ACCUDISC_OK);
    return f;
}

static int any_ok(const accudisc_features *f)
{
    return f->ok_c2 || f->ok_sub_raw || f->ok_sub_q || f->ok_c2_sub_raw ||
           f->ok_c2_sub_q;
}

/* The claims are read in every state; only the functional half depends on
 * what is loaded. */
static void claims_present(const accudisc_features *f)
{
    assert(f->feature_present == 1 && f->c2_claimed == 1);
    assert(f->mastering_present == 1 && f->buf_claimed == 1 &&
           f->sao_claimed == 1 && f->test_write_claimed == 1);
}

static void t_empty(void)
{
    accudisc_features f = probe(EMPTY, 1);

    assert(f.medium == ACCUDISC_FEATURES_MEDIUM_NONE);
    assert(fk.reads == 0);                    /* not applicable = not sent */
    assert(!any_ok(&f));
    assert(f.c2_verdict == ACCUDISC_C2_UNVERIFIED);
    assert(f.current == 0 && f.mastering_current == 0);
    claims_present(&f);
}

/* The case that used to be a confident false negative: a blank is loaded, the
 * CD-DA reads fail 5/21, and the drive was called C2_UNSUPPORTED. */
static void t_blank(void)
{
    accudisc_features f = probe(BLANK, 1);

    assert(f.medium == ACCUDISC_FEATURES_MEDIUM_NO_AUDIO);
    assert(fk.reads == 0);
    assert(!any_ok(&f));
    assert(f.c2_verdict == ACCUDISC_C2_UNVERIFIED);
    assert(f.mastering_current == 1);         /* a blank IS the write medium */
    claims_present(&f);
}

static void t_data_and_dvd(void)
{
    accudisc_features f = probe(DATA, 1);

    assert(f.medium == ACCUDISC_FEATURES_MEDIUM_NO_AUDIO);
    assert(fk.reads == 0);
    assert(f.c2_verdict == ACCUDISC_C2_UNVERIFIED);

    f = probe(DVD, 1);
    assert(f.medium == ACCUDISC_FEATURES_MEDIUM_NO_AUDIO);
    assert(fk.reads == 0);
    assert(f.c2_verdict == ACCUDISC_C2_UNVERIFIED);
}

static void t_audio(void)
{
    accudisc_features f = probe(AUDIO, 1);

    assert(f.medium == ACCUDISC_FEATURES_MEDIUM_AUDIO);
    assert(fk.reads >= 5 && fk.read_lba == 0);
    assert(f.ok_c2 && f.ok_sub_raw && f.ok_sub_q && f.ok_c2_sub_raw &&
           f.ok_c2_sub_q);
    assert(f.c2_verdict == ACCUDISC_C2_SUPPORTED);
    claims_present(&f);
}

/* With audio loaded a refused C2 read IS evidence, and must stay a verdict:
 * the fix must not have turned every failure into "unverified". */
static void t_audio_c2_refused(void)
{
    accudisc_features f = probe(AUDIO, 0);

    assert(f.medium == ACCUDISC_FEATURES_MEDIUM_AUDIO);
    assert(fk.reads >= 5);
    assert(!f.ok_c2 && !f.ok_c2_sub_raw && !f.ok_c2_sub_q);
    assert(f.ok_sub_raw && f.ok_sub_q);
    assert(f.c2_verdict == ACCUDISC_C2_UNSUPPORTED);
}

/* Mixed Mode: LBA 0 is a data track and refuses CD-DA reads. The smoke reads
 * must go to the first AUDIO track, or a working drive reads as five failures. */
static void t_mixed(void)
{
    accudisc_features f = probe(MIXED, 1);

    assert(f.medium == ACCUDISC_FEATURES_MEDIUM_AUDIO);
    assert(fk.read_lba == MIXED_AUDIO_LBA);
    assert(f.ok_c2 && f.ok_sub_raw && f.ok_c2_sub_q);
    assert(f.c2_verdict == ACCUDISC_C2_SUPPORTED);
}

/* A drive still evaluating its disc answers profile 0 and NOT READY. That is
 * an absence of classification, so the reads ARE sent (it is not "no audio"),
 * and their NOT READY refusal must not become a verdict about C2. */
static void t_busy(void)
{
    accudisc_features f = probe(BUSY, 1);

    assert(f.medium == ACCUDISC_FEATURES_MEDIUM_UNKNOWN);
    assert(fk.reads >= 1);
    assert(!any_ok(&f));
    assert(f.c2_verdict == ACCUDISC_C2_UNVERIFIED);
}

/* Whether the governor is ON is a drive setting and survives an empty tray.
 * The rate it recommends is for the loaded medium: the drive answers with a
 * figure regardless, and with no disc that figure must not be passed on. */
static void t_governor(void)
{
    accudisc_features f;

    f = probe(EMPTY, 1);
    assert(f.governor_known == 0);            /* no driver: not asked */

    with_driver = 1;
    f = probe(EMPTY, 1);
    assert(f.governor_known == 1 && f.governor_on == 1);
    assert(f.governor_recommended_kbps == 0);

    f = probe(BLANK, 1);                      /* the state it is FOR */
    assert(f.governor_known == 1 && f.governor_on == 1);
    assert(f.governor_recommended_kbps == 8467);

    f = probe(AUDIO, 1);
    assert(f.governor_recommended_kbps == 8467);
    with_driver = 0;
}

int main(void)
{
    t_empty();
    t_blank();
    t_data_and_dvd();
    t_audio();
    t_audio_c2_refused();
    t_mixed();
    t_busy();
    t_governor();
    printf("test_features: ok\n");
    return 0;
}
