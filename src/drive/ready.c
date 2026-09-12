/* "Are you ready?" — drive readiness, classified and waited for.
 *
 * AccuDisc had no readiness handling at all until 0.38.0: TEST UNIT READY
 * existed in this tree only as a string in the trace decoder. The cost, in
 * Keith's words, was "contention between the drive evaluating the disc that's
 * just been inserted, and an MMC command issued to the drive" — measured
 * 2026-09-12 as the first `accudisc disc` after a load reporting
 * profile=0x0000 reason=not_cd_profile, and `accudisc media` drawing 2/04/01
 * NOT READY - BECOMING READY, for a perfectly good blank. Our own docs already
 * carried the human workaround ("retry once before believing it"), which is
 * the tell: a rule written for a person to follow is a missing feature.
 *
 * MMC-5 §4.1.6.2 names the safe set — while Busy a drive shall still accept
 * REQUEST SENSE, INQUIRY, GET CONFIGURATION, GET EVENT STATUS NOTIFICATION and
 * TEST UNIT READY — but the design here comes from measurement rather than
 * from the spec, via tools/readyprobe.c on the PX-716A, because the spec
 * allows several things this drive does not do:
 *
 *   - TEST UNIT READY is the ONLY trustworthy oracle. It alone changed state
 *     at the moment the disc became usable (~1.0 s after a software reload).
 *   - GESN Media Present was 1 from t=0. It answers "is there a disc", not "is
 *     the disc understood", so it cannot gate readiness.
 *   - GET CONFIGURATION answered the CORRECT profile 0x0009 from t=0 while
 *     READ DISC INFORMATION was still refusing — and had answered 0x0000
 *     during the same kind of window earlier the same day. Sometimes right
 *     during the window is worse than always wrong: no gate can use it.
 *   - The Device Busy event class is ADVERTISED (supported classes 0x5E) and
 *     never fires — busy=0 on every tick of the not-ready window. The
 *     predicted time-to-ready the design first leaned on does not exist here.
 *
 * So no GESN is issued at all, which also sidesteps the event-draining hazard
 * (MMC-5 §6.7.1.4: an allocation length above 4 CONSUMES an event, and the
 * Linux sr driver polls GESN for media change on its own). TEST UNIT READY's
 * own sense carries every case, including the terminal ones.
 */

#include <stdio.h>
#include <time.h>

#include "../internal.h"
#include "../mmc/mmc.h"

void adsc_sleep_ms(long ms)
{
    struct timespec ts = { ms / 1000, (ms % 1000) * 1000000L };

    nanosleep(&ts, NULL);
}

int adsc_ready_classify(int rc, const accudisc_sense *s)
{
    if (rc == ACCUDISC_OK)
        return ADSC_READY_YES;
    if (rc != ACCUDISC_ERR_SENSE || !s || !s->valid)
        return ADSC_READY_OTHER;

    if (s->key == 0x06) {
        /* UNIT ATTENTION. 0x28 NOT READY TO READY CHANGE (the drive announcing
         * a new medium) and 0x29 POWER ON / RESET. Both mean THE COMMAND DID
         * NOT EXECUTE, which is what makes re-issuing it a repeat rather than
         * a second action. Measured: 6/28/00 fires exactly once, on the tick
         * the disc becomes readable, and lands on whatever command is in
         * flight. */
        if (s->asc == 0x28 || s->asc == 0x29)
            return ADSC_READY_RETRY_NOW;
        return ADSC_READY_OTHER;
    }

    if (s->key == 0x02) {
        /* 0x3A MEDIUM NOT PRESENT is TERMINAL, and saying so at once is the
         * whole of "fail gracefully": no amount of waiting produces a disc, so
         * the message must be "there is no disc" and not "gave up after N
         * tries". ASCQ 0x01 = tray closed, 0x02 = tray open. */
        if (s->asc == 0x3A)
            return ADSC_READY_NO_MEDIUM;
        /* 0x04 LOGICAL UNIT NOT READY — the spin-up/evaluation window this
         * whole file exists for. ASCQ 0x01 is "in process of becoming ready",
         * the one actually observed.
         *
         * ASCQ 0x02 ("initializing command required") is deliberately treated
         * the same, as a WAIT, even though the spec-correct response is to
         * issue START STOP UNIT with start=1. We have never seen this drive
         * ask for it, and a recovery path nobody has watched fire is not a
         * recovery path — it is untested code on a rarely-taken branch. The
         * cost of getting it wrong is bounded and honest: the wait times out
         * and reports the drive's own sense, which names the condition. Build
         * the spin-up when a drive is seen to need it. */
        if (s->asc == 0x04)
            return ADSC_READY_WAIT;
        return ADSC_READY_OTHER;
    }
    return ADSC_READY_OTHER;
}

int adsc_op_repeatable(uint8_t op)
{
    /* An ALLOWLIST, deliberately, and the direction matters more than the
     * contents: with a denylist, every opcode added later — a vendor command,
     * a new MMC one — would be silently retryable by default, and the first
     * time that is wrong it costs a disc. Here the default is "do not repeat"
     * and each entry is a decision.
     *
     * Everything below is a pure interrogation: it returns information and
     * changes nothing on the medium or in the drive's persistent state. Absent
     * on purpose, and not by oversight: WRITE(10)/(12), SEND CUE SHEET, CLOSE
     * TRACK/SESSION, BLANK, SEND OPC, FORMAT UNIT, MODE SELECT, SET STREAMING,
     * SET CD SPEED, START STOP UNIT, PREVENT ALLOW MEDIUM REMOVAL,
     * SYNCHRONIZE CACHE, and every vendor opcode. */
    switch (op) {
    case 0x00: /* TEST UNIT READY */
    case 0x03: /* REQUEST SENSE */
    case 0x12: /* INQUIRY */
    case 0x1A: /* MODE SENSE(6) */
    case 0x23: /* READ FORMAT CAPACITIES */
    case 0x25: /* READ CAPACITY */
    case 0x28: /* READ(10) */
    case 0x42: /* READ SUB-CHANNEL */
    case 0x43: /* READ TOC/PMA/ATIP */
    case 0x46: /* GET CONFIGURATION */
    case 0x4A: /* GET EVENT STATUS NOTIFICATION */
    case 0x51: /* READ DISC INFORMATION */
    case 0x52: /* READ TRACK INFORMATION */
    case 0x5A: /* MODE SENSE(10) */
    case 0x5C: /* READ BUFFER CAPACITY */
    case 0xA8: /* READ(12) */
    case 0xAC: /* GET PERFORMANCE */
    case 0xAD: /* READ DISC STRUCTURE */
    case 0xB9: /* READ CD MSF */
    case 0xBD: /* MECHANISM STATUS */
    case 0xBE: /* READ CD */
        return 1;
    default:
        return 0;
    }
}

int adsc_dev_wait_ready(struct accudisc_device *dev, unsigned timeout_ms)
{
    unsigned waited = 0;

    if (!dev)
        return ACCUDISC_ERR_INVAL;

    for (;;) {
        adsc_cmd cmd = {0};
        int rc = adsc_mmc_test_unit_ready(dev, &cmd);
        int what = adsc_ready_classify(rc, &dev->last_sense);

        if (what == ADSC_READY_YES) {
            /* One line, on the ordinary log rather than the trace, when the
             * wait was long enough for a human to have noticed the pause.
             * Under the threshold it says nothing: the common case is a drive
             * that was ready on the first ask, and a message then would train
             * the reader to ignore this one. */
            if (waited >= ADSC_READY_NOTE_MS)
                adsc_dev_log(dev, "drive became ready after %u.%us",
                             waited / 1000, (waited % 1000) / 100);
            return ACCUDISC_OK;
        }
        /* A unit attention IS the transition — the drive announcing the medium
         * it has just finished reading. Re-ask at once rather than sleeping
         * through the readiness we were waiting for. */
        if (what == ADSC_READY_RETRY_NOW)
            continue;
        if (what == ADSC_READY_NO_MEDIUM || what == ADSC_READY_OTHER)
            return rc;

        if (waited >= timeout_ms) {
            snprintf(dev->last_io, sizeof(dev->last_io),
                     "drive still not ready after %u.%us (last: %u/%02X/%02X)",
                     waited / 1000, (waited % 1000) / 100, dev->last_sense.key,
                     dev->last_sense.asc, dev->last_sense.ascq);
            return ACCUDISC_ERR_IO;
        }
        /* Traced, always. A retry layer that silently absorbs "not ready" is
         * the same hazard in miniature as the bugs it was built to fix: this
         * project has just spent days on a fault whose signature was a drive
         * answering oddly, and a layer that quietly waited until the answer
         * looked normal would have hidden it. */
        adsc_dev_trace_note(dev, "drive not ready (%u/%02X/%02X), waiting "
                                 "%u ms (%u ms of %u elapsed)",
                            dev->last_sense.key, dev->last_sense.asc,
                            dev->last_sense.ascq, ADSC_READY_POLL_MS, waited,
                            timeout_ms);
        adsc_sleep_ms(ADSC_READY_POLL_MS);
        waited += ADSC_READY_POLL_MS;
    }
}
