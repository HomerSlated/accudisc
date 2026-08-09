/* The read-speed uncap's state, and the ONE property that now matters: that we
 * only ever report it when someone who can actually answer has.
 *
 * This file used to be mostly a table test. `adsc_uncap_classify` inferred the
 * uncap's state by comparing page 2A's advertised maximum against a per-model
 * stock ceiling, and most of the cases here pinned that table's UNKNOWN
 * behaviour for models it had never heard of. Both are gone as of 0.8.0 — see
 * the note at the top of src/drive/uncap.c.
 *
 * Worth recording what those deleted tests were good at, because it was not
 * nothing: they correctly guarded the previous implementation's real defect, a
 * bare `max_x > 40` that answered confidently about every drive in the world
 * from one drive's measurements. They enforced "unknown model -> UNKNOWN, never
 * OFF", which is the right rule. What no test here could catch is that the
 * quantity being compared did not answer the question at all: page 2A reports
 * the largest request the drive ACCEPTS, not what it delivers, and on CD-DA the
 * governor caps the rate regardless. A well-tested inference from the wrong
 * quantity still tests correctly, which is the whole lesson.
 *
 * What remains is source 1/2 polarity and the retirement of the value the
 * inference used to return.
 */

#include <stdio.h>
#include <string.h>

#include "internal.h"

static int fails;

static const char *name(accudisc_uncap_state s)
{
    switch (s) {
    case ACCUDISC_UNCAP_OFF:     return "OFF";
    case ACCUDISC_UNCAP_ON:      return "ON";
    case ACCUDISC_UNCAP_UNKNOWN: return "UNKNOWN";
    }
    return "?";
}

int main(void)
{
    /* 2 IS RETIRED (was ACCUDISC_UNCAP_LIKELY_ON) and must never be reused: a
     * consumer built before 0.8.0 reads a 2 as "likely on", so reassigning it
     * would misreport with nothing able to detect it. No enumerator may take
     * the value, and UNKNOWN must stay at 3 rather than sliding down to fill
     * the hole — sliding is exactly how the value gets reused by accident. */
    if (ACCUDISC_UNCAP_UNKNOWN != 3) {
        printf("FAIL %-46s UNKNOWN is %d, must stay 3\n",
               "2 stays retired (was LIKELY_ON)",
               (int)ACCUDISC_UNCAP_UNKNOWN);
        fails++;
    } else if (ACCUDISC_UNCAP_OFF == 2 || ACCUDISC_UNCAP_ON == 2) {
        printf("FAIL %-46s an enumerator took 2\n",
               "2 stays retired (was LIKELY_ON)");
        fails++;
    } else {
        printf("ok   %-46s OFF=0 ON=1 [2 retired] UNKNOWN=3\n",
               "2 stays retired (was LIKELY_ON)");
    }

    /* --- the state, which is now REPORTED rather than enforced ---------------
     *
     * Until 0.6.0 accudisc_read_cdda refused a subchannel read on
     * adsc_uncap_authoritative(dev) == ON, and these cases guarded that
     * polarity. The refusal is gone; the query is not, because callers still
     * reach it through accudisc_speed_uncap_probe/get and the CLI still reports
     * it. A polarity slip is therefore no longer a disabled guard but a wrong
     * answer to a direct question — still worth pinning, and the only thing
     * keeping ON and OFF apart now that no code path branches on them.
     *
     * The UNKNOWN row carries more weight since 0.8.0: with the inference gone
     * it is what an untouched handle without a driver ALWAYS returns, so this
     * is the case pinning that we report absence rather than guessing.
     *
     * Exercisable with no hardware: with drv == NULL, source 2 returns
     * ERR_UNSUPPORTED before touching the transport, so a zeroed handle reaches
     * only source 1. */
    struct {
        int set;
        accudisc_uncap_state want;
        const char *what;
    } auth[] = {
        { 0,  ACCUDISC_UNCAP_UNKNOWN, "handle untouched, no driver -> UNKNOWN" },
        { 1,  ACCUDISC_UNCAP_ON,      "we set it on -> ON (refusal fires)" },
        { -1, ACCUDISC_UNCAP_OFF,     "we set it off -> OFF (refusal silent)" },
    };

    for (size_t i = 0; i < sizeof(auth) / sizeof(auth[0]); i++) {
        struct accudisc_device d;
        accudisc_uncap_state got;

        memset(&d, 0, sizeof(d)); /* no driver, no transport touched */
        d.uncap_set = auth[i].set;
        got = adsc_uncap_authoritative(&d);

        if (got != auth[i].want) {
            printf("FAIL %-46s got %s, want %s\n", auth[i].what, name(got),
                   name(auth[i].want));
            fails++;
        } else {
            printf("ok   %-46s %s\n", auth[i].what, name(got));
        }
    }

    /* No handle state may produce anything outside the three live values. This
     * replaces a check that `LIKELY_ON` never leaked out of the authoritative
     * path; with that value retired, the general form is the useful one — it
     * also catches the retired 2 reappearing through this function. */
    {
        struct accudisc_device d;
        int bad = 0;
        for (int s = -1; s <= 1; s++) {
            accudisc_uncap_state got;
            memset(&d, 0, sizeof(d));
            d.uncap_set = s;
            got = adsc_uncap_authoritative(&d);
            if (got != ACCUDISC_UNCAP_OFF && got != ACCUDISC_UNCAP_ON &&
                got != ACCUDISC_UNCAP_UNKNOWN)
                bad = 1;
        }
        if (bad) {
            printf("FAIL %-46s returned a retired or unknown value\n",
                   "only OFF/ON/UNKNOWN are ever returned");
            fails++;
        } else {
            printf("ok   %-46s\n", "only OFF/ON/UNKNOWN are ever returned");
        }
    }

    /* --- scoped push/pop (§5.4) ---------------------------------------------
     *
     * The property under test is the safety one: a push that changed nothing
     * must leave `prior` at -1, and pop must then be a no-op. If that breaks,
     * `cmd_read`'s unconditional pop in the `out:` block starts writing a stale
     * stack value into persistent drive state on every error path — silently,
     * and only on hardware. No test above would notice.
     *
     * Reachable with a zeroed handle: with drv == NULL both get and set return
     * ERR_UNSUPPORTED before touching the transport. */
    {
        struct accudisc_device d;
        int prior = 12345; /* deliberate garbage: push must overwrite it */
        int rc;

        memset(&d, 0, sizeof(d));
        rc = accudisc_speed_uncap_push(&d, 1, &prior);

        if (rc != ACCUDISC_ERR_UNSUPPORTED) {
            printf("FAIL %-46s rc=%d, want ERR_UNSUPPORTED\n",
                   "push with no driver refuses", rc);
            fails++;
        } else {
            printf("ok   %-46s ERR_UNSUPPORTED\n", "push with no driver refuses");
        }

        if (prior != -1) {
            printf("FAIL %-46s prior=%d, want -1\n",
                   "failed push leaves nothing to restore", prior);
            fails++;
        } else {
            printf("ok   %-46s prior=-1\n",
                   "failed push leaves nothing to restore");
        }

        /* The pairing that matters: pop after a failed push must do nothing at
         * all, not attempt a write. */
        rc = accudisc_speed_uncap_pop(&d, prior);
        if (rc != ACCUDISC_OK) {
            printf("FAIL %-46s rc=%d, want OK\n",
                   "pop after failed push is a no-op", rc);
            fails++;
        } else {
            printf("ok   %-46s OK\n", "pop after failed push is a no-op");
        }

        /* A real prior does reach the drive layer — which has no driver here,
         * so it surfaces ERR_UNSUPPORTED rather than pretending to succeed. */
        rc = accudisc_speed_uncap_pop(&d, 0);
        if (rc != ACCUDISC_ERR_UNSUPPORTED) {
            printf("FAIL %-46s rc=%d, want ERR_UNSUPPORTED\n",
                   "pop with a real prior does attempt a set", rc);
            fails++;
        } else {
            printf("ok   %-46s ERR_UNSUPPORTED\n",
                   "pop with a real prior does attempt a set");
        }

        /* Argument validation, since cmd_read passes these straight through. */
        struct { const char *what; int rc; int want; } inval[] = {
            { "push(NULL) rejected",   accudisc_speed_uncap_push(NULL, 1, &prior),
              ACCUDISC_ERR_INVAL },
            { "push(prior=NULL) rejected", accudisc_speed_uncap_push(&d, 1, NULL),
              ACCUDISC_ERR_INVAL },
            { "pop(NULL) rejected",    accudisc_speed_uncap_pop(NULL, 0),
              ACCUDISC_ERR_INVAL },
        };
        for (size_t i = 0; i < sizeof(inval) / sizeof(inval[0]); i++) {
            if (inval[i].rc != inval[i].want) {
                printf("FAIL %-46s rc=%d\n", inval[i].what, inval[i].rc);
                fails++;
            } else {
                printf("ok   %-46s ERR_INVAL\n", inval[i].what);
            }
        }
    }

    /* -11 IS RETIRED AND MUST STAY RETIRED.
     *
     * This pair of checks used to assert that ERR_UNSAFE_COMBINATION had a
     * message and did not collide with another code. Both went with the guard
     * in 0.6.0. What replaces them protects the more important property: that
     * nothing ever takes the vacated number.
     *
     * A consumer compiled before 0.6.0 maps -11 to "unsafe combination". If a
     * future error is assigned -11, that consumer reports the wrong failure for
     * the right return value, with nothing on either side able to detect it —
     * the same well-formed-but-wrong-referent shape the size field exists to
     * prevent, arriving through the error space instead of the struct layout.
     *
     * accudisc_strerror's switch covers every ASSIGNED code and falls through
     * to "unknown error" otherwise, so asking it about -11 is a direct test of
     * whether -11 is assigned. Reusing the number makes this fail. */
    const char *retired = accudisc_strerror(-11);
    if (!retired || strcmp(retired, "unknown error") != 0) {
        printf("FAIL %-46s strerror gives \"%s\"\n",
               "-11 stays retired (was UNSAFE_COMBINATION)",
               retired ? retired : "(null)");
        fails++;
    } else {
        printf("ok   %-46s \"%s\"\n",
               "-11 stays retired (was UNSAFE_COMBINATION)", retired);
    }

    printf(fails ? "\n%d failure(s)\n" : "\nall passed\n", fails);
    return fails ? 1 : 0;
}
