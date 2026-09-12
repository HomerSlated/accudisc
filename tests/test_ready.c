/* The readiness classifier and the repeat allowlist.
 *
 * Both are pure functions, and both are tested here rather than on hardware
 * because the interesting cases are the ones a drive will not produce on
 * demand. A drive answers 2/04/01 for about a second after a load and 2/3A/02
 * only with the tray open; getting it to emit a queued unit attention, or an
 * unrelated 5/64/00, at the moment a test is watching is not a thing that can
 * be arranged. So the sense goes in by hand.
 *
 * What must not be got wrong, in order of how expensive the mistake is:
 *
 * 1. A WRITE must never be repeated. The allowlist is the only thing standing
 *    between a unit attention mid-burn and a re-issued WRITE(10), so the test
 *    that matters most is the NEGATIVE one — and it is written as an explicit
 *    roll-call of the dangerous opcodes rather than as "some opcode is
 *    excluded", because the failure mode is one specific opcode leaking in.
 * 2. "No medium" must never be treated as "wait". Waiting 30 s to conclude
 *    there is no disc is the difference between a graceful failure and a
 *    useless one.
 * 3. An unrelated sense must pass through untouched. A readiness layer that
 *    swallows 5/64/00 would have hidden this week's `speeds` bug.
 */

#include <stdio.h>
#include <string.h>

#include "internal.h"

static int fails;

static void check(int cond, const char *what)
{
    if (!cond) {
        printf("FAIL: %s\n", what);
        fails++;
    }
}

static accudisc_sense mk(uint8_t key, uint8_t asc, uint8_t ascq)
{
    accudisc_sense s;

    memset(&s, 0, sizeof(s));
    s.valid = 1;
    s.key = key;
    s.asc = asc;
    s.ascq = ascq;
    return s;
}

static void classify(void)
{
    accudisc_sense s = mk(0, 0, 0);

    /* GOOD is ready whatever the (stale) sense buffer holds — the rc leads. */
    s = mk(0x02, 0x04, 0x01);
    check(adsc_ready_classify(ACCUDISC_OK, &s) == ADSC_READY_YES,
          "GOOD is ready even with a stale not-ready sense behind it");

    /* The measured case: 2/04/01 for ~1.0 s after a load. */
    s = mk(0x02, 0x04, 0x01);
    check(adsc_ready_classify(ACCUDISC_ERR_SENSE, &s) == ADSC_READY_WAIT,
          "2/04/01 becoming ready -> WAIT");
    s = mk(0x02, 0x04, 0x00);
    check(adsc_ready_classify(ACCUDISC_ERR_SENSE, &s) == ADSC_READY_WAIT,
          "2/04/00 cause not reportable -> WAIT");
    s = mk(0x02, 0x04, 0x02);
    check(adsc_ready_classify(ACCUDISC_ERR_SENSE, &s) == ADSC_READY_WAIT,
          "2/04/02 initializing command required -> WAIT (documented choice: "
          "we have never seen a drive ask, so the spin-up is not built and the "
          "wait times out honestly instead)");

    /* Terminal. Waiting cannot conjure a disc, and this is the whole of
     * "failing gracefully" — the message must be "no disc", not "gave up". */
    s = mk(0x02, 0x3A, 0x01);
    check(adsc_ready_classify(ACCUDISC_ERR_SENSE, &s) == ADSC_READY_NO_MEDIUM,
          "2/3A/01 no medium, tray closed -> NO_MEDIUM, never WAIT");
    s = mk(0x02, 0x3A, 0x02);
    check(adsc_ready_classify(ACCUDISC_ERR_SENSE, &s) == ADSC_READY_NO_MEDIUM,
          "2/3A/02 no medium, tray open -> NO_MEDIUM, never WAIT");
    s = mk(0x02, 0x3A, 0x00);
    check(adsc_ready_classify(ACCUDISC_ERR_SENSE, &s) == ADSC_READY_NO_MEDIUM,
          "2/3A/00 unspecified -> NO_MEDIUM (the ASCQ refines, it does not "
          "decide)");

    /* Unit attention: the command did not execute, so a repeat is a repeat. */
    s = mk(0x06, 0x28, 0x00);
    check(adsc_ready_classify(ACCUDISC_ERR_SENSE, &s) == ADSC_READY_RETRY_NOW,
          "6/28/00 not-ready-to-ready change -> RETRY_NOW");
    s = mk(0x06, 0x29, 0x00);
    check(adsc_ready_classify(ACCUDISC_ERR_SENSE, &s) == ADSC_READY_RETRY_NOW,
          "6/29/00 power on / reset -> RETRY_NOW");

    /* Everything else passes through. These are the cases where a retry layer
     * does damage by being helpful. */
    s = mk(0x05, 0x64, 0x00);
    check(adsc_ready_classify(ACCUDISC_ERR_SENSE, &s) == ADSC_READY_OTHER,
          "5/64/00 illegal mode for this track -> OTHER (the sense that the "
          "speeds bug turned on; swallowing it would re-create that bug)");
    s = mk(0x03, 0x02, 0x00);
    check(adsc_ready_classify(ACCUDISC_ERR_SENSE, &s) == ADSC_READY_OTHER,
          "3/02/00 no seek complete -> OTHER (a medium/mechanical error is "
          "not a readiness condition and must reach the caller)");
    s = mk(0x02, 0x53, 0x02);
    check(adsc_ready_classify(ACCUDISC_ERR_SENSE, &s) == ADSC_READY_OTHER,
          "key 2 with an unrelated ASC -> OTHER (the KEY alone does not mean "
          "wait)");
    s = mk(0x06, 0x2A, 0x01);
    check(adsc_ready_classify(ACCUDISC_ERR_SENSE, &s) == ADSC_READY_OTHER,
          "6/2A/01 mode parameters changed -> OTHER (not every unit attention "
          "is a media transition)");

    /* An ERR_IO has no sense to read, and an invalid sense must not be
     * interpreted — both would otherwise decode as key 0 = ready. */
    s = mk(0x02, 0x04, 0x01);
    check(adsc_ready_classify(ACCUDISC_ERR_IO, &s) == ADSC_READY_OTHER,
          "ERR_IO is not a readiness verdict whatever sense is lying around");
    memset(&s, 0, sizeof(s)); /* valid = 0 */
    check(adsc_ready_classify(ACCUDISC_ERR_SENSE, &s) == ADSC_READY_OTHER,
          "an unparsed sense is OTHER, not YES");
    check(adsc_ready_classify(ACCUDISC_ERR_SENSE, NULL) == ADSC_READY_OTHER,
          "NULL sense is OTHER, not a crash");
}

static void allowlist(void)
{
    /* THE test. Every one of these changes the medium or the drive's
     * persistent state, and a repeat of any of them is a second action rather
     * than a retry. Named individually on purpose: the failure this guards
     * against is one opcode leaking into the allowlist, which a spot check of
     * "some opcode returns 0" would not catch. */
    static const struct { uint8_t op; const char *name; } forbidden[] = {
        { 0x2A, "WRITE(10)" },
        { 0xAA, "WRITE(12)" },
        { 0x53, "RESERVE TRACK" },
        { 0x5B, "CLOSE TRACK/SESSION" },
        { 0xA1, "BLANK" },
        { 0x54, "SEND OPC" },
        { 0x5D, "SEND CUE SHEET" },
        { 0x15, "MODE SELECT(6)" },
        { 0x55, "MODE SELECT(10)" },
        { 0x35, "SYNCHRONIZE CACHE" },
        { 0x1B, "START STOP UNIT" },
        { 0x1E, "PREVENT ALLOW MEDIUM REMOVAL" },
        { 0xB6, "SET STREAMING" },
        { 0xBB, "SET CD SPEED" },
        { 0x04, "FORMAT UNIT" },
        { 0xE9, "Plextor vendor 0xE9" },
        { 0xED, "Plextor vendor 0xED (PoweRec)" },
        { 0xEA, "Plextor vendor 0xEA (Q-Check)" },
        { 0xF1, "Plextor vendor 0xF1 (EEPROM)" },
    };
    char buf[96];

    for (size_t i = 0; i < sizeof(forbidden) / sizeof(forbidden[0]); i++) {
        snprintf(buf, sizeof buf, "%s (0x%02X) is NOT repeatable",
                 forbidden[i].name, forbidden[i].op);
        check(!adsc_op_repeatable(forbidden[i].op), buf);
    }

    /* The positive side. Thinner on purpose: a read wrongly excluded costs a
     * retry that does not happen, which is the old behaviour and merely
     * unhelpful. */
    check(adsc_op_repeatable(0x00), "TEST UNIT READY is repeatable");
    check(adsc_op_repeatable(0xBE), "READ CD is repeatable");
    check(adsc_op_repeatable(0x43), "READ TOC/PMA/ATIP is repeatable");
    check(adsc_op_repeatable(0x51), "READ DISC INFORMATION is repeatable");
    check(adsc_op_repeatable(0x5A), "MODE SENSE(10) is repeatable");
    check(adsc_op_repeatable(0x4A), "GET EVENT STATUS NOTIFICATION is "
                                    "repeatable");

    /* The allowlist's direction is itself the safety property: an opcode
     * nobody has considered must default to NOT repeatable. */
    check(!adsc_op_repeatable(0x7F), "an unknown opcode defaults to NOT "
                                     "repeatable");
    check(!adsc_op_repeatable(0xFF), "0xFF defaults to NOT repeatable");
}

int main(void)
{
    classify();
    allowlist();
    if (fails) {
        printf("test_ready: %d FAILED\n", fails);
        return 1;
    }
    printf("test_ready: ok\n");
    return 0;
}
