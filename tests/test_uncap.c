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

    /* --- the value the read-engine refusal actually keys on -----------------
     *
     * accudisc_read_cdda refuses on adsc_uncap_authoritative(dev) == ON, so a
     * polarity slip here would disable the guard (or refuse every subchannel
     * read) without any test above noticing. Exercisable with no hardware: with
     * drv == NULL, source 2 returns ERR_UNSUPPORTED before touching the
     * transport, so a zeroed handle reaches only source 1.
     *
     * What this still does NOT cover is the end-to-end refusal returning
     * ACCUDISC_ERR_UNSAFE_COMBINATION from a real read — that needs a drive and
     * is the outstanding hardware item. */
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

    /* The error the guard returns must have a real message: a caller printing
     * "unknown error" cannot act on it, which defeats a guard whose whole
     * purpose is to explain a refusal. */
    const char *msg = accudisc_strerror(ACCUDISC_ERR_UNSAFE_COMBINATION);
    if (!msg || strcmp(msg, "unknown error") == 0) {
        printf("FAIL %-46s strerror gives \"%s\"\n",
               "ERR_UNSAFE_COMBINATION has a message", msg ? msg : "(null)");
        fails++;
    } else {
        printf("ok   %-46s \"%s\"\n", "ERR_UNSAFE_COMBINATION has a message",
               msg);
    }

    /* And it must not collide with an existing code — a duplicate value would
     * silently alias two unrelated failures. */
    if (ACCUDISC_ERR_UNSAFE_COMBINATION == ACCUDISC_ERR_NOTFOUND ||
        ACCUDISC_ERR_UNSAFE_COMBINATION == ACCUDISC_ERR_CRC ||
        ACCUDISC_ERR_UNSAFE_COMBINATION == ACCUDISC_ERR_UNSUPPORTED) {
        printf("FAIL %-46s value collides\n", "ERR_UNSAFE_COMBINATION unique");
        fails++;
    } else {
        printf("ok   %-46s %d\n", "ERR_UNSAFE_COMBINATION unique",
               (int)ACCUDISC_ERR_UNSAFE_COMBINATION);
    }

    printf(fails ? "\n%d failure(s)\n" : "\nall passed\n", fails);
    return fails ? 1 : 0;
}
