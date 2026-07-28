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

/* ---- the admitted ladder ---------------------------------------------
 *
 * The vector below is a REAL run, not a construction: PX-716A rev 1.11,
 * Tracy (leadout 162892, clean), 2026-07-28, uncap ON, under flock. Using
 * recorded hardware output matters here — the rule's whole difficulty is
 * the cross-rung radius term, and invented numbers would let a rule that
 * ignores it pass.
 */
static accudisc_speed_rung tracy[] = {
    /* req  page2a  measured  min    max  */
    { 48, 48, 2301, 1715, 2767, 0, 0 },
    { 40, 40, 2373, 1808, 2828, 0, 0 },
    { 32, 32, 1955, 1514, 2309, 0, 0 },
    { 24, 24, 1498, 1175, 1757, 0, 0 },
    { 16,  8,  801,  801,  801, 0, 0 },
    {  8,  8,  801,  800,  801, 0, 0 },
    {  4,  4,  401,  401,  401, 0, 0 },
};
#define TRACY_N ((uint8_t)(sizeof tracy / sizeof tracy[0]))

static const char *vname(uint8_t v)
{
    switch (v) {
    case ACCUDISC_RUNG_ADMITTED:  return "admitted";
    case ACCUDISC_RUNG_DUPLICATE: return "duplicate";
    case ACCUDISC_RUNG_QUANTIZED: return "quantized";
    default:                      return "unknown";
    }
}

/* Render the admitted ladder as "40,32,24,8,4" for comparison. */
static void ladder_str(const accudisc_speed_rung *r, uint8_t n, char *out,
                       size_t cap)
{
    size_t len = 0;

    out[0] = '\0';
    for (uint8_t i = 0; i < n; i++)
        if (r[i].verdict == ACCUDISC_RUNG_ADMITTED)
            len += (size_t)snprintf(out + len, cap - len, "%s%u",
                                    len ? "," : "", r[i].requested_x);
}

static void admit_checks(void)
{
    accudisc_speed_rung r[TRACY_N];
    char got[64];
    char msg[256];

    /* --- the headline: the rule reproduces the hand-derived ladder --- */
    memcpy(r, tracy, sizeof tracy);
    adsc_speeds_admit(r, TRACY_N, 3, 3);
    ladder_str(r, TRACY_N, got, sizeof got);
    snprintf(msg, sizeof msg,
             "admitted ladder on the recorded PX-716A run: got \"%s\", "
             "wanted \"40,32,24,8,4\"", got);
    check(!strcmp(got, "40,32,24,8,4"), msg);

    /* Each verdict individually, so a wrong ladder says which rung moved. */
    static const struct { uint16_t req; uint8_t v; uint16_t eq; } want[] = {
        { 48, ACCUDISC_RUNG_DUPLICATE, 40 }, /* 1.0 radius-steps from 40 */
        { 40, ACCUDISC_RUNG_ADMITTED,   0 },
        { 32, ACCUDISC_RUNG_ADMITTED,   0 },
        { 24, ACCUDISC_RUNG_ADMITTED,   0 },
        { 16, ACCUDISC_RUNG_QUANTIZED,  8 }, /* the drive said so */
        {  8, ACCUDISC_RUNG_ADMITTED,   0 },
        {  4, ACCUDISC_RUNG_ADMITTED,   0 },
    };
    for (uint8_t i = 0; i < TRACY_N; i++) {
        snprintf(msg, sizeof msg, "req=%u: verdict %s (equiv %u), wanted %s "
                 "(equiv %u)", r[i].requested_x, vname(r[i].verdict),
                 r[i].equiv_x, vname(want[i].v), want[i].eq);
        check(r[i].verdict == want[i].v && r[i].equiv_x == want[i].eq, msg);
    }

    /* --- points == 1 must refuse, not guess -------------------------- */
    /* The same rungs with no interval. A rule willing to judge on
     * measured_cx alone would happily return a ladder here, and it would
     * be the confident wrong answer the sweep exists to prevent. */
    accudisc_speed_rung flat[TRACY_N];

    memcpy(flat, tracy, sizeof tracy);
    for (uint8_t i = 0; i < TRACY_N; i++)
        flat[i].min_cx = flat[i].max_cx = 0;
    adsc_speeds_admit(flat, TRACY_N, 1, 3);
    for (uint8_t i = 0; i < TRACY_N; i++) {
        snprintf(msg, sizeof msg,
                 "req=%u with points=1 must be UNKNOWN, got %s — a verdict "
                 "without an interval is a guess", flat[i].requested_x,
                 vname(flat[i].verdict));
        check(flat[i].verdict == ACCUDISC_RUNG_UNKNOWN, msg);
    }
    ladder_str(flat, TRACY_N, got, sizeof got);
    check(got[0] == '\0', "points=1 must admit no rungs at all");

    /* --- a rung that did not measure is UNKNOWN, never admitted ------ */
    accudisc_speed_rung dead[TRACY_N];

    memcpy(dead, tracy, sizeof tracy);
    dead[3].measured_cx = 0;  /* the 24 rung failed to time */
    adsc_speeds_admit(dead, TRACY_N, 3, 3);
    check(dead[3].verdict == ACCUDISC_RUNG_UNKNOWN,
          "a rung with no measurement must be UNKNOWN, not admitted");

    /* --- QUANTIZED needs no measurement and no comparison ------------ */
    /* page2a below the request is the drive's own statement. It must win
     * even when the measured rate would otherwise look admissible. */
    accudisc_speed_rung snap[2] = {
        {  8,  8,  801,  800,  801, 0, 0 },
        { 40,  8, 2373, 1808, 2828, 0, 0 }, /* absurd, but page2a says 8 */
    };
    adsc_speeds_admit(snap, 2, 3, 3);
    check(snap[1].verdict == ACCUDISC_RUNG_QUANTIZED && snap[1].equiv_x == 8,
          "page2a below the request must give QUANTIZED regardless of the "
          "measured rate — the drive's own report outranks our timing");

    /* --- THE STABILITY CHECK ----------------------------------------- */
    /* A rule tuned until it matches one disc is a census, not a rule. K is
     * a parameter solely so this can sweep it. K's real job is not to
     * create duplicates — req=48 is caught by measuring slower than req=40
     * and never consults the margin — but to avoid collapsing rungs that
     * are genuinely distinct. The binding constraint is the closest genuine
     * pair, 40-vs-32: a 4.18x gap against a 0.72x radius-step, so anything
     * below K=5.8 keeps it. */
    for (uint32_t k = 1; k <= 5; k++) {
        accudisc_speed_rung w[TRACY_N];

        memcpy(w, tracy, sizeof tracy);
        adsc_speeds_admit(w, TRACY_N, 3, k);
        ladder_str(w, TRACY_N, got, sizeof got);
        snprintf(msg, sizeof msg,
                 "ladder must not depend on the exact threshold: K=%u gave "
                 "\"%s\", wanted \"40,32,24,8,4\"", k, got);
        check(!strcmp(got, "40,32,24,8,4"), msg);
    }

    /* And the sweep must be capable of a DIFFERENT answer, or it proves
     * nothing about K — it would only show the rule ignores K. Past the
     * separation the ladder does move: at K=6 the margin (0.72 x 6 = 4.32x)
     * finally exceeds the 40-vs-32 gap of 4.18x and eats a real rung. */
    {
        accudisc_speed_rung w[TRACY_N];

        memcpy(w, tracy, sizeof tracy);
        adsc_speeds_admit(w, TRACY_N, 3, 6);
        ladder_str(w, TRACY_N, got, sizeof got);
        snprintf(msg, sizeof msg,
                 "K=6 must NOT give the stable ladder — if every K agrees, "
                 "the sweep above is vacuous (got \"%s\")", got);
        check(strcmp(got, "40,32,24,8,4") != 0, msg);
    }

    /* And the rule must still be ABLE to say duplicate — a check that can
     * only ever pass is worthless. Two rungs at genuinely the same rate. */
    accudisc_speed_rung same[2] = {
        { 32, 32, 1955, 1514, 2309, 0, 0 },
        { 40, 40, 1960, 1519, 2314, 0, 0 }, /* +0.05x: nothing */
    };
    adsc_speeds_admit(same, 2, 3, 3);
    check(same[1].verdict == ACCUDISC_RUNG_DUPLICATE && same[1].equiv_x == 32,
          "two rungs at the same rate must collapse — if this passes only "
          "because nothing is ever a duplicate, the rule is vacuous");
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

    admit_checks();

    if (fails)
        printf("%d FAILED\n", fails);
    else
        printf("all speed-layout and admission checks passed\n");
    return fails ? 1 : 0;
}
