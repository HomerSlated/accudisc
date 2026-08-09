/* The read-speed uncap classifier: does the per-model stock-ceiling table say
 * the right thing, including for models it has never heard of?
 *
 * The point of this test is the UNKNOWN cases. A table lookup that returns the
 * right answer for the one drive we own is easy; the failure this guards
 * against is the previous implementation's, which compared against a bare
 * `> 40` and therefore answered confidently about every drive in the world
 * from one drive's measurements. An unknown model must come back UNKNOWN and
 * never OFF — "we cannot tell" and "it is off" lead to opposite decisions.
 */

#include <stdio.h>
#include <string.h>

#include "internal.h"

static int fails;

static const char *name(accudisc_uncap_state s)
{
    switch (s) {
    case ACCUDISC_UNCAP_OFF:       return "OFF";
    case ACCUDISC_UNCAP_ON:        return "ON";
    case ACCUDISC_UNCAP_LIKELY_ON: return "LIKELY_ON";
    case ACCUDISC_UNCAP_UNKNOWN:   return "UNKNOWN";
    }
    return "?";
}

static void check(const char *what, const char *vendor, const char *product,
                  unsigned max_x, accudisc_uncap_state want)
{
    accudisc_uncap_state got = adsc_uncap_classify(vendor, product, max_x);

    if (got != want) {
        printf("FAIL %-46s got %s, want %s\n", what, name(got), name(want));
        fails++;
    } else {
        printf("ok   %-46s %s\n", what, name(got));
    }
}

int main(void)
{
    /* The one verified row. FEATURES.md feature 1: SpeedRead ON flips page-2A
     * max read 40x -> 48x on the PX-716A, SET OFF restores 40x — measured live
     * in both directions, which is the table's entry rule. */
    check("PX-716A at 48x -> uncapped", "PLEXTOR", "DVDR   PX-716A", 48,
          ACCUDISC_UNCAP_LIKELY_ON);
    check("PX-716A at 40x -> stock", "PLEXTOR", "DVDR   PX-716A", 40,
          ACCUDISC_UNCAP_OFF);
    check("PX-716A at 41x -> just over stock", "PLEXTOR", "DVDR   PX-716A", 41,
          ACCUDISC_UNCAP_LIKELY_ON);

    /* Below stock is still "not uncapped". A drive reporting under its own
     * ceiling is odd, but it is unambiguously not lifted. */
    check("PX-716A at 32x -> below stock", "PLEXTOR", "DVDR   PX-716A", 32,
          ACCUDISC_UNCAP_OFF);

    /* INQUIRY padding must not defeat the lookup: drives pad fixed-width
     * fields, so "DVDR   PX-716A" and "DVDR PX-716A" are the same drive. */
    check("PX-716A, single-spaced product", "PLEXTOR", "DVDR PX-716A", 48,
          ACCUDISC_UNCAP_LIKELY_ON);
    check("PX-716A, trailing pad", "PLEXTOR", "DVDR   PX-716A   ", 48,
          ACCUDISC_UNCAP_LIKELY_ON);

    /* THE CASES THAT MATTER. Each of these would have returned a confident
     * (and possibly wrong) answer under a bare `max_x > 40`. */
    check("another Plextor, 48x, not in table", "PLEXTOR", "DVDR   PX-760A",
          48, ACCUDISC_UNCAP_UNKNOWN);
    check("another Plextor, 40x, not in table", "PLEXTOR", "DVDR   PX-760A",
          40, ACCUDISC_UNCAP_UNKNOWN);
    check("non-Plextor at 48x", "HL-DT-ST", "DVDRAM GH24NSD1", 48,
          ACCUDISC_UNCAP_UNKNOWN);
    check("right product, wrong vendor", "PIONEER", "DVDR   PX-716A", 48,
          ACCUDISC_UNCAP_UNKNOWN);

    /* max_x == 0 means page 2A could not be read — nothing to judge. */
    check("speed unreadable (0x)", "PLEXTOR", "DVDR   PX-716A", 0,
          ACCUDISC_UNCAP_UNKNOWN);

    /* Defensive: the probe passes dev->id fields, which are always non-NULL,
     * but the classifier is public within the library. */
    check("NULL vendor", NULL, "DVDR   PX-716A", 48, ACCUDISC_UNCAP_UNKNOWN);
    check("NULL product", "PLEXTOR", NULL, 48, ACCUDISC_UNCAP_UNKNOWN);

    /* --- the authoritative state, which is now REPORTED rather than enforced -
     *
     * Until 0.6.0 accudisc_read_cdda refused a subchannel read on
     * adsc_uncap_authoritative(dev) == ON, and these cases guarded that
     * polarity. The refusal is gone; the classifier is not, because callers
     * still query it through accudisc_speed_uncap_probe/get and the CLI still
     * reports it. A polarity slip is therefore no longer a disabled guard but a
     * wrong answer to a direct question — still worth pinning, and the only
     * thing keeping ON and OFF apart now that no code path branches on them.
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

    /* The authoritative query must never invent the inferred value — that is
     * source 3's, and returning it here would make the read engine refuse on
     * an inference, which is precisely the design decision being avoided. */
    {
        struct accudisc_device d;
        int leaked = 0;
        for (int s = -1; s <= 1; s++) {
            memset(&d, 0, sizeof(d));
            d.uncap_set = s;
            if (adsc_uncap_authoritative(&d) == ACCUDISC_UNCAP_LIKELY_ON)
                leaked = 1;
        }
        if (leaked) {
            printf("FAIL %-46s returned LIKELY_ON\n",
                   "authoritative never yields LIKELY_ON");
            fails++;
        } else {
            printf("ok   %-46s\n", "authoritative never yields LIKELY_ON");
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
