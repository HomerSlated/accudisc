/* The write-health guards: a live-write budget, and a per-burn timing envelope.
 *
 * Both exist because of one incident. On 2026-09-03 a CD-RW was destroyed by 37
 * write operations in an hour, 22 of which fired AFTER the disc had already
 * failed, because the harness driving them checked nothing between iterations.
 * Damage to phase-change media accumulates per write PASS and is independent of
 * the interval between passes (ECMA-395 §13.1.4, US 6,091,698), so the guard
 * that binds is a bound on the number of passes — not a cool-down.
 *
 * THE NUMBERS BELOW ARE THE REAL ONES, from
 * private/research/incoming/2026-09-03-first-cd-rw-live-burns.md §9b. The burn
 * before the disc died ran settle=13320 ms / payload=30311 ms over 9600
 * sectors, where the same burn forty minutes earlier read settle=10000 ms /
 * payload=18836 ms. If this file ever stops flagging that pair, the guard has
 * stopped doing the one job it was built for.
 *
 * THE NEGATIVE CASES MATTER AS MUCH AS THE POSITIVE ONES. A guard that flags
 * everything is not a guard, and the specific way this one could be
 * type-correct but reference-wrong is by comparing raw elapsed times: the
 * 2400- and 9600-sector burns of that night differ 4x in payload BY DESIGN, so
 * an implementation that forgot to scale per sector would report every long
 * burn as a fault and every short one as an improvement, while passing any test
 * that only ever fed it one length. That case is pinned below.
 */

#include <stdio.h>
#include <string.h>

#include "internal.h"

static int fails;

static void check(int cond, const char *what)
{
    if (cond) {
        printf("ok   %s\n", what);
    } else {
        printf("FAIL %s\n", what);
        fails++;
    }
}

/* Fresh handle: zeroed, exactly as accudisc_open leaves the health fields. */
static void reset(struct accudisc_device *d)
{
    memset(d, 0, sizeof *d);
}

static uint32_t anomaly_of(struct accudisc_device *d)
{
    accudisc_write_health h = { .size = sizeof h };

    if (accudisc_write_health_get(d, &h) != ACCUDISC_OK)
        return 0xffffffffu; /* cannot be confused with a real mask */
    return h.anomaly;
}

int main(void)
{
    struct accudisc_device d;
    accudisc_write_health h;

    /* ---- budget: default, set, reset ------------------------------------ */
    reset(&d);
    h = (accudisc_write_health){ .size = sizeof h };
    check(accudisc_write_health_get(&d, &h) == ACCUDISC_OK &&
          h.budget == 0 && h.live_writes == 0,
          "a fresh handle is UNLIMITED (budget 0) — existing callers unaffected");

    check(accudisc_set_write_budget(&d, 5) == ACCUDISC_OK && d.wr_budget == 5,
          "set_write_budget takes");

    d.wr_live = 3;
    check(accudisc_set_write_budget(&d, 9) == ACCUDISC_OK && d.wr_live == 0,
          "setting a budget RESETS the count (a new bounded run starts at 0)");

    /* ---- ABI ------------------------------------------------------------- */
    reset(&d);
    h = (accudisc_write_health){ .size = 0 };
    check(accudisc_write_health_get(&d, &h) == ACCUDISC_ERR_ABI,
          "health_get with size 0 is ERR_ABI, not a silently short read");
    h = (accudisc_write_health){ .size = sizeof h + 8 };
    check(accudisc_write_health_get(&d, &h) == ACCUDISC_ERR_ABI,
          "health_get with an unknown larger size is ERR_ABI");
    check(accudisc_write_health_get(NULL, &h) == ACCUDISC_ERR_INVAL &&
          accudisc_set_write_budget(NULL, 1) == ACCUDISC_ERR_INVAL,
          "NULL device is ERR_INVAL on both entry points");

    /* ---- the budget actually REFUSES, and refuses before touching the drive.
     * accudisc_write checks it after the ABI import and before the .toc is
     * opened, so these paths never reach the (absent) hardware. The .toc path
     * is deliberately nonexistent: if the budget did NOT fire we would get a
     * parse/open error instead, and the assertion would fail rather than
     * passing for the wrong reason. */
    {
        accudisc_write_opts o = ACCUDISC_WRITE_OPTS_INIT;

        reset(&d);
        accudisc_set_write_budget(&d, 2);
        d.wr_live = 2;
        check(accudisc_write(&d, "/nonexistent/x.toc", "/nonexistent/x.bin",
                             &o, NULL, NULL) == ACCUDISC_ERR_WRITE_BUDGET,
              "a live write at the budget is REFUSED before anything is opened");

        o.simulate = 1;
        check(accudisc_write(&d, "/nonexistent/x.toc", "/nonexistent/x.bin",
                             &o, NULL, NULL) != ACCUDISC_ERR_WRITE_BUDGET,
              "SIMULATE is exempt — laser off, no OPC, costs the medium nothing");

        o.simulate = 0;
        d.wr_live = 1;
        check(accudisc_write(&d, "/nonexistent/x.toc", "/nonexistent/x.bin",
                             &o, NULL, NULL) != ACCUDISC_ERR_WRITE_BUDGET,
              "under the budget the write proceeds (fails later, on the path)");

        reset(&d);           /* budget 0 = unlimited */
        d.wr_live = 1000000;
        check(accudisc_write(&d, "/nonexistent/x.toc", "/nonexistent/x.bin",
                             &o, NULL, NULL) != ACCUDISC_ERR_WRITE_BUDGET,
              "budget 0 never refuses, however many writes have been made");
    }

    /* ---- timing envelope: the baseline ---------------------------------- */
    reset(&d);
    adsc_write_health_record(&d, 10000, 18836, 9600);
    h = (accudisc_write_health){ .size = sizeof h };
    accudisc_write_health_get(&d, &h);
    check(h.live_writes == 1 && h.base_settle_ms == 10000 &&
          h.base_payload_ms == 18836 && h.base_sectors == 9600,
          "the first live burn becomes the baseline");
    check(h.anomaly == 0,
          "with one burn there is NO baseline to deviate from — mask is 0, and "
          "that is not a clean bill of health");

    /* ---- THE REAL FAILURE. Both flags must fire. ------------------------- */
    adsc_write_health_record(&d, 13320, 30311, 9600);
    check((anomaly_of(&d) & ACCUDISC_WRITE_ANOMALY_PAYLOAD) != 0,
          "2026-09-03: payload 30311 vs 18836 over 9600 sectors is FLAGGED");
    check((anomaly_of(&d) & ACCUDISC_WRITE_ANOMALY_SETTLE) != 0,
          "2026-09-03: settle 13320 vs 10000 (cold value, warm drive) is FLAGGED");

    /* ---- the guard must be able NOT to fire ----------------------------- */
    reset(&d);
    adsc_write_health_record(&d, 10000, 18836, 9600);
    adsc_write_health_record(&d, 10000, 18836, 9600);
    check(anomaly_of(&d) == 0, "an identical repeat burn is NOT flagged");

    reset(&d);
    adsc_write_health_record(&d, 10000, 18836, 9600);
    adsc_write_health_record(&d, 10400, 20000, 9600);
    check(anomaly_of(&d) == 0,
          "ordinary variation inside tolerance is NOT flagged (no false alarm "
          "on every second burn)");

    /* ---- PER-SECTOR SCALING: the way this guard could be reference-wrong.
     * A 2400-sector burn at exactly the baseline's per-sector rate has a
     * payload a quarter as long. Raw comparison would call that a pass and the
     * reverse case a failure; only the scaled comparison is right. */
    reset(&d);
    adsc_write_health_record(&d, 10000, 18836, 9600);
    adsc_write_health_record(&d, 10000, 4709, 2400);   /* 18836/4 exactly */
    check(anomaly_of(&d) == 0,
          "a SHORTER burn at the same per-sector rate is not flagged");

    reset(&d);
    adsc_write_health_record(&d, 10000, 4709, 2400);
    adsc_write_health_record(&d, 10000, 18836, 9600);  /* 4x length, same rate */
    check(anomaly_of(&d) == 0,
          "a LONGER burn at the same per-sector rate is not flagged — the "
          "failure a raw elapsed-time comparison would produce");

    reset(&d);
    adsc_write_health_record(&d, 10000, 4709, 2400);
    adsc_write_health_record(&d, 10000, 28000, 9600);  /* 1.49x the rate */
    check((anomaly_of(&d) & ACCUDISC_WRITE_ANOMALY_PAYLOAD) != 0,
          "a longer burn that is genuinely SLOWER per sector IS flagged");

    /* ---- degenerate inputs must not fabricate a verdict ------------------ */
    reset(&d);
    adsc_write_health_record(&d, 0, 18836, 9600);
    adsc_write_health_record(&d, 9999, 18836, 9600);
    check((anomaly_of(&d) & ACCUDISC_WRITE_ANOMALY_SETTLE) == 0,
          "a zero baseline settle yields NO settle verdict, not a division or "
          "an infinite ratio");

    reset(&d);
    adsc_write_health_record(&d, 10000, 18836, 9600);
    adsc_write_health_record(&d, 10000, 18836, 0);
    check((anomaly_of(&d) & ACCUDISC_WRITE_ANOMALY_PAYLOAD) == 0,
          "a zero-sector burn yields NO payload verdict");

    /* ---- simulate never reaches the recorder at all ---------------------- */
    reset(&d);
    accudisc_set_write_budget(&d, 3);
    {
        accudisc_write_opts o = ACCUDISC_WRITE_OPTS_INIT;
        o.simulate = 1;
        (void)accudisc_write(&d, "/nonexistent/x.toc", "/nonexistent/x.bin",
                             &o, NULL, NULL);
    }
    h = (accudisc_write_health){ .size = sizeof h };
    accudisc_write_health_get(&d, &h);
    check(h.live_writes == 0,
          "a simulate run does not consume budget");

    printf(fails ? "\n%d failure(s)\n" : "\nall passed\n", fails);
    return fails ? 1 : 0;
}
