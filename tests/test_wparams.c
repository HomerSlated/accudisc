/* Page 05 round trip (src/write/wparams.c) against a fake drive.
 *
 * The property under test is not "MODE SELECT is sent" -- the burn has always
 * done that -- but that a drive which does NOT hold the page is told apart from
 * one that does, when both answer every command GOOD. Each fake below returns
 * GOOD throughout; they differ only in what the page reads back as. A check
 * that passed on all of them would be the bug this exists to catch, so every
 * case asserts the verdict AND the reason.
 *
 * The fake's default page is the shape a PX-716A reports (page length 0x32,
 * 60 bytes with the header). adsc_dev_exec is replaced with -Wl,--wrap. */

#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "internal.h"
#include "mmc/cdb.h"
#include "write/write.h"

#define HDR 8u
#define PAGE 52u /* 2 + page length 0x32 */

enum select_mode {
    HONOUR,     /* stores the page as sent */
    IGNORE,     /* GOOD, and changes nothing */
    FORCE_TEST, /* stores it, with Test Write left set */
    CORRUPT,    /* stores it with one non-burn byte altered */
    REFUSE      /* CHECK CONDITION 5/26/00 */
};

static struct {
    uint8_t page[PAGE];
    enum select_mode mode;
    int ps;            /* report the PS bit on sense, as real drives do */
    int fail_sense_at; /* fail the Nth MODE SENSE data read (1-based), 0 = never */
    int senses, selects;
} fk;

static void fake_reset(enum select_mode mode)
{
    memset(&fk, 0, sizeof fk);
    fk.mode = mode;
    fk.ps = 1;
    fk.page[0] = 0x05;
    fk.page[1] = 0x32;
    fk.page[2] = 0x61; /* BUFE | LS_V | write type 1 (TAO): a drive default */
    fk.page[3] = 0xc4; /* multisession 3, track mode 4 */
    fk.page[4] = 0x08; /* data block type 8 (Mode 1) */
    fk.page[8] = 0x00;
    fk.page[14] = 0x00;
    fk.page[15] = 0x96; /* audio pause length 150 */
}

int __real_adsc_dev_exec(struct accudisc_device *dev, adsc_cmd *cmd);
int __wrap_adsc_dev_exec(struct accudisc_device *dev, adsc_cmd *cmd)
{
    cmd->resid = 0;
    if (cmd->cdb[0] == ADSC_OP_MODE_SENSE10) {
        uint8_t full[HDR + PAGE] = {0};

        assert(cmd->dir == ADSC_XFER_IN);
        assert((cmd->cdb[2] & 0x3f) == 0x05);
        full[1] = HDR + PAGE - 2;
        memcpy(full + HDR, fk.page, PAGE);
        if (fk.ps)
            full[HDR] |= 0x80;
        if (cmd->buf_len > 8 && fk.fail_sense_at &&
            ++fk.senses == fk.fail_sense_at)
            return ACCUDISC_ERR_IO;
        memcpy(cmd->buf, full,
               cmd->buf_len < sizeof full ? cmd->buf_len : sizeof full);
        return ACCUDISC_OK;
    }
    if (cmd->cdb[0] == ADSC_OP_MODE_SELECT10) {
        const uint8_t *b = cmd->buf;

        assert(cmd->dir == ADSC_XFER_OUT);
        assert(cmd->buf_len == HDR + PAGE);
        assert(b[0] == 0 && b[1] == 0);      /* header length reserved */
        assert((b[HDR] & 0xc0) == 0);        /* PS never sent back */
        fk.selects++;
        switch (fk.mode) {
        case REFUSE:
            dev->last_sense.key = 0x05;
            dev->last_sense.asc = 0x26;
            dev->last_sense.ascq = 0x00;
            return ACCUDISC_ERR_SENSE;
        case IGNORE:
            break;
        case HONOUR:
            memcpy(fk.page, b + HDR, PAGE);
            break;
        case FORCE_TEST:
            memcpy(fk.page, b + HDR, PAGE);
            fk.page[2] |= 0x10;
            break;
        case CORRUPT:
            memcpy(fk.page, b + HDR, PAGE);
            fk.page[15] ^= 0x01; /* audio pause length: no burn field */
            break;
        }
        return ACCUDISC_OK;
    }
    assert(0 && "the round trip sent a command other than MODE SENSE/SELECT");
    return ACCUDISC_ERR_IO;
}

static struct accudisc_device dev;
static const struct adsc_write_params LIVE = {.burnproof = 1};
static const struct adsc_write_params SIM = {.simulate = 1, .burnproof = 1};

static void t_honour(void)
{
    struct adsc_wparams_roundtrip rt;

    fake_reset(HONOUR);
    assert(adsc_write_params_roundtrip(&dev, &LIVE, &rt) == ACCUDISC_OK);
    assert(rt.stage == ADSC_WPRT_STAGE_DONE);
    assert(rt.before_len == PAGE && rt.sent_len == PAGE && rt.after_len == PAGE);
    assert(rt.before[2] == 0x61);            /* what the drive held */
    assert(rt.sent[2] == 0x62);              /* DAO, BUFE, LS_V kept, no test */
    assert((rt.sent[3] & 0xc0) == 0);
    assert((rt.sent[4] & 0x0f) == 0);
    assert((rt.sent[0] & 0xc0) == 0);        /* captured as it went out */
    assert(rt.after[0] == 0x85);             /* PS back on the read ... */
    assert(rt.page_ok && rt.fields_ok);      /* ... and not counted against it */
    assert(rt.diff_bytes == 0);
    assert(rt.changed && !rt.ignored);
    assert(fk.selects == 1);
}

/* GOOD status, nothing applied. This is the case the burn could not see. */
static void t_ignore(void)
{
    struct adsc_wparams_roundtrip rt;

    fake_reset(IGNORE);
    assert(adsc_write_params_roundtrip(&dev, &LIVE, &rt) == ACCUDISC_OK);
    assert(rt.stage == ADSC_WPRT_STAGE_DONE);
    assert(!rt.page_ok && !rt.fields_ok);
    assert(rt.changed && rt.ignored);
    assert(rt.after[2] == 0x61);
}

/* The hypothesis itself: the live page goes down, Test Write stays up. */
static void t_force_test(void)
{
    struct adsc_wparams_roundtrip rt;

    fake_reset(FORCE_TEST);
    assert(adsc_write_params_roundtrip(&dev, &LIVE, &rt) == ACCUDISC_OK);
    assert(!(rt.sent[2] & 0x10) && (rt.after[2] & 0x10));
    assert(!rt.fields_ok && !rt.page_ok);
    assert(rt.diff_bytes == 1);
    assert(!rt.ignored);                     /* it changed, just not to ours */
}

/* A byte the burn never sets comes back different: the fields are fine and
 * the whole-page comparison is the only thing that notices. */
static void t_corrupt(void)
{
    struct adsc_wparams_roundtrip rt;

    fake_reset(CORRUPT);
    assert(adsc_write_params_roundtrip(&dev, &LIVE, &rt) == ACCUDISC_OK);
    assert(rt.fields_ok);
    assert(!rt.page_ok && rt.diff_bytes == 1);
    assert(!rt.ignored);
}

/* The page already holds what we send. An honest drive and an ignoring drive
 * read back identically, and the report must say the match proves nothing. */
static void t_undiscriminating(void)
{
    struct adsc_wparams_roundtrip first, rt;

    for (int m = 0; m < 2; m++) {
        fake_reset(HONOUR);
        assert(adsc_write_params_roundtrip(&dev, &LIVE, &first) == ACCUDISC_OK);
        fk.mode = m ? IGNORE : HONOUR;       /* now ask for the same again */
        assert(adsc_write_params_roundtrip(&dev, &LIVE, &rt) == ACCUDISC_OK);
        assert(rt.page_ok && rt.fields_ok);
        assert(!rt.changed);                 /* same answer from both drives */
        assert(!rt.ignored);
    }
}

/* Test Write must be seen to move both ways: ON reads back ON, then OFF reads
 * back OFF, and both requests really did differ from the page before them. */
static void t_toggle(void)
{
    struct adsc_wparams_roundtrip a, b;

    fake_reset(HONOUR);
    assert(adsc_write_params_roundtrip(&dev, &SIM, &a) == ACCUDISC_OK);
    assert((a.sent[2] & 0x1f) == 0x12 && (a.after[2] & 0x10));
    assert(a.page_ok && a.changed);
    assert(adsc_write_params_roundtrip(&dev, &LIVE, &b) == ACCUDISC_OK);
    assert((b.before[2] & 0x10) && !(b.after[2] & 0x10));
    assert(b.page_ok && b.changed);
}

static void t_refuse(void)
{
    struct adsc_wparams_roundtrip rt;

    fake_reset(REFUSE);
    assert(adsc_write_params_roundtrip(&dev, &LIVE, &rt) == ACCUDISC_ERR_SENSE);
    assert(rt.stage == ADSC_WPRT_STAGE_SELECT);
    assert(rt.before_len == PAGE && rt.sent_len == PAGE && rt.after_len == 0);
    assert(!rt.page_ok && !rt.fields_ok);    /* never "ok" without a read-back */
}

/* The select went down and the read-back failed in transport: the page may or
 * may not hold, and the report must not claim either. */
static void t_readback_fails(void)
{
    struct adsc_wparams_roundtrip rt;

    fake_reset(HONOUR);
    fk.fail_sense_at = 2;                    /* 1 = before, 2 = the read-back */
    assert(adsc_write_params_roundtrip(&dev, &LIVE, &rt) == ACCUDISC_ERR_IO);
    assert(rt.stage == ADSC_WPRT_STAGE_READBACK);
    assert(rt.sent_len == PAGE && rt.after_len == 0);
    assert(!rt.page_ok && !rt.fields_ok && !rt.ignored);
    assert(fk.selects == 1);
}

/* CD-Text asks for data block type 3 and it must read back as 3. */
static void t_cdtext(void)
{
    struct adsc_wparams_roundtrip rt;
    struct adsc_write_params wp = {.burnproof = 1, .cdtext = 1};

    fake_reset(HONOUR);
    assert(adsc_write_params_roundtrip(&dev, &wp, &rt) == ACCUDISC_OK);
    assert((rt.sent[4] & 0x0f) == 3 && (rt.after[4] & 0x0f) == 3);
    assert(rt.page_ok);
}

/* The burn's own entry point is the same sequence with no capture: one sense
 * pair, one select, and the same bytes. Guards the refactor that made the
 * round trip share it. */
static void t_set_params_unchanged(void)
{
    struct adsc_wparams_roundtrip rt;
    uint8_t via_burn[PAGE];

    fake_reset(HONOUR);
    assert(adsc_write_set_params(&dev, &LIVE) == ACCUDISC_OK);
    assert(fk.selects == 1);
    memcpy(via_burn, fk.page, PAGE);

    fake_reset(HONOUR);
    assert(adsc_write_params_roundtrip(&dev, &LIVE, &rt) == ACCUDISC_OK);
    assert(memcmp(via_burn, fk.page, PAGE) == 0);
    assert(memcmp(via_burn, rt.sent, PAGE) == 0);
}

static void t_restore(void)
{
    struct adsc_wparams_roundtrip rt;
    uint8_t orig[PAGE];

    fake_reset(HONOUR);
    memcpy(orig, fk.page, PAGE);
    assert(adsc_write_params_roundtrip(&dev, &SIM, &rt) == ACCUDISC_OK);
    assert(memcmp(orig, fk.page, PAGE) != 0);
    assert(adsc_write_params_restore(&dev, rt.before, rt.before_len) ==
           ACCUDISC_OK);
    assert(memcmp(orig, fk.page, PAGE) == 0);
    /* A capture of the wrong shape is refused rather than selected. */
    assert(adsc_write_params_restore(&dev, rt.before, rt.before_len - 2) ==
           ACCUDISC_ERR_SHORT);
}

int main(void)
{
    t_honour();
    t_ignore();
    t_force_test();
    t_corrupt();
    t_undiscriminating();
    t_toggle();
    t_refuse();
    t_readback_fails();
    t_cdtext();
    t_set_params_unchanged();
    t_restore();
    printf("test_wparams: ok\n");
    return 0;
}
