/* The speed-ladder probe's window layout.
 *
 * The point of this test is that the failure it guards against is SILENT.
 * Every window the probe times must be one the drive cache has never seen;
 * that is guaranteed by arithmetic alone (`adsc_speeds_layout`), and if the
 * arithmetic is wrong the windows overlap, the re-read is served from cache,
 * and the rung reports the same rate at every radius. A flat rung is also
 * exactly what a genuinely CLV-clamped rung looks like — so the bug would
 * present as the instrument's own diagnostic firing correctly, on hardware,
 * with no error anywhere. It has to be caught here or not at all.
 *
 * The case that matters most is a span that FITS at points=1 and does NOT
 * fit at points=3: the guard has to scale with the band count, and a guard
 * left at the old divisor would pass every points=1 test ever written.
 */

#include <stdio.h>

#include "internal.h"

static int fails;

static void check(int cond, const char *what)
{
    if (!cond) {
        printf("FAIL: %s\n", what);
        fails++;
    }
}

/* Rung i in band b, per the layout contract in internal.h. */
static uint32_t win(uint32_t slot, uint32_t band, uint8_t b, uint8_t i)
{
    return (uint32_t)b * band + (uint32_t)i * slot;
}

/* Every window disjoint from every other, given each is `len` sectors. */
static void check_disjoint(uint32_t count, uint8_t ncand, uint8_t points,
                           const char *what)
{
    uint32_t slot = 0, band = 0;
    char msg[160];

    if (adsc_speeds_layout(count, ncand, points, &slot, &band)
        != ACCUDISC_OK) {
        printf("FAIL: %s: layout refused a span that should fit\n", what);
        fails++;
        return;
    }

    for (uint8_t b = 0; b < points; b++)
        for (uint8_t i = 0; i < ncand; i++) {
            uint32_t a0 = win(slot, band, b, i);
            uint32_t a1 = a0 + slot; /* the whole slot is this rung's */

            snprintf(msg, sizeof msg,
                     "%s: window (band %u, rung %u) ends at %u, past the "
                     "span %u", what, b, i, a1, count);
            check(a1 <= count, msg);

            for (uint8_t c = 0; c < points; c++)
                for (uint8_t j = 0; j < ncand; j++) {
                    if (c == b && j == i)
                        continue;
                    uint32_t o0 = win(slot, band, c, j);

                    snprintf(msg, sizeof msg,
                             "%s: window (band %u, rung %u) at [%u,%u) "
                             "overlaps (band %u, rung %u) at %u — the "
                             "re-read would be cache-served",
                             what, b, i, a0, a1, c, j, o0);
                    check(o0 >= a1 || o0 + slot <= a0, msg);
                }
        }
}

int main(void)
{
    uint32_t slot, band;

    /* ---- points validation ------------------------------------------- */
    /* 3 is not a tunable. Anything else is a caller error, not a silent
     * round to the nearest supported value. */
    check(adsc_speeds_layout(400000, 8, 0, &slot, &band) == ACCUDISC_ERR_INVAL,
          "points=0 must be rejected by the layout (the probe maps it to 1 "
          "before calling, so the layout itself never sees a valid 0)");
    check(adsc_speeds_layout(400000, 8, 2, &slot, &band) == ACCUDISC_ERR_INVAL,
          "points=2 must be rejected");
    check(adsc_speeds_layout(400000, 8, 4, &slot, &band) == ACCUDISC_ERR_INVAL,
          "points=4 must be rejected");
    check(adsc_speeds_layout(400000, 8, 255, &slot, &band)
          == ACCUDISC_ERR_INVAL, "points=255 must be rejected");
    check(adsc_speeds_layout(400000, 0, 1, &slot, &band) == ACCUDISC_ERR_INVAL,
          "ncand=0 must be rejected");
    check(adsc_speeds_layout(400000, 8, 1, NULL, &band) == ACCUDISC_ERR_INVAL,
          "NULL slot out-param must be rejected");

    /* ---- THE regression: the guard must scale with the band count ----- */
    /* 177 = SPEEDS_MIN_SECTORS + SPEEDS_CHUNK, the smallest usable slot.
     * 8 rungs need 8*177 = 1416 sectors at one band and 24*177 = 4248 at
     * three. A span between those two fits one way and not the other, and
     * a guard still dividing by ncand alone would accept both. */
    check(adsc_speeds_layout(2000, 8, 1, &slot, &band) == ACCUDISC_OK,
          "2000 sectors fits 8 rungs at points=1");
    check(adsc_speeds_layout(2000, 8, 3, &slot, &band) == ACCUDISC_ERR_INVAL,
          "2000 sectors must NOT fit 8 rungs at points=3 — a guard that "
          "still divides by ncand alone overlaps the bands and reports "
          "every rung as flat");

    /* Both sides of the three-band boundary. */
    check(adsc_speeds_layout(4247, 8, 3, &slot, &band) == ACCUDISC_ERR_INVAL,
          "one sector below the three-band minimum must be refused");
    check(adsc_speeds_layout(4248, 8, 3, &slot, &band) == ACCUDISC_OK,
          "exactly the three-band minimum must be accepted");
    check(adsc_speeds_layout(1415, 8, 1, &slot, &band) == ACCUDISC_ERR_INVAL,
          "one sector below the one-band minimum must be refused");
    check(adsc_speeds_layout(1416, 8, 1, &slot, &band) == ACCUDISC_OK,
          "exactly the one-band minimum must be accepted");

    /* ---- the layout the guard is protecting --------------------------- */
    /* points=1 must lay out exactly as it always did: slot = count/ncand,
     * rung i at i*slot. This is the "existing behaviour is untouched"
     * assertion — the CLI's default path still runs through here. */
    check(adsc_speeds_layout(160000, 8, 1, &slot, &band) == ACCUDISC_OK
          && slot == 20000 && band == 160000,
          "points=1 layout is unchanged: slot = count/ncand");

    check(adsc_speeds_layout(360000, 8, 3, &slot, &band) == ACCUDISC_OK
          && slot == 15000 && band == 120000,
          "points=3 splits the span into three equal bands of ncand slots");

    /* The bands must be a fixed distance apart for EVERY rung — that is
     * what makes min_cx/max_cx a gradient rather than a comparison of
     * measurements taken at unrelated places. */
    check(adsc_speeds_layout(360000, 8, 3, &slot, &band) == ACCUDISC_OK
          && win(slot, band, 1, 3) - win(slot, band, 0, 3)
             == win(slot, band, 2, 5) - win(slot, band, 1, 5),
          "band spacing is the same for every rung");

    /* ---- disjointness, which is the actual guarantee ------------------ */
    check_disjoint(360000, 8, 3, "whole disc, 8 rungs, 3 bands");
    check_disjoint(160000, 8, 1, "middle half, 8 rungs, 1 band");
    check_disjoint(4248, 8, 3, "minimum three-band span");
    check_disjoint(400000, 16, 3, "16 rungs (the CLI's --ladder maximum)");
    check_disjoint(5000, 1, 3, "single rung");

    if (fails)
        printf("%d FAILED\n", fails);
    else
        printf("all speed-layout checks passed\n");
    return fails ? 1 : 0;
}
