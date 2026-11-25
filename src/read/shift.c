/* accudisc_shift_test (0.55.0): is y a displaced copy of x, damage allowed?
 *
 * THE DEFINITION IS cdda2img's `_shifted` / `_rates` in tools/shift_margin.py
 * at their commit 2e23787 (correspondence 275), and this is a transcription of
 * it, not a design. Our own first reference (AccuDisc 2026-11-25l) could never
 * fire: it measured a sector's self-difference out to |k| = 588 samples, where
 * a one-sector copy has no overlap left, so every sector failed its "has
 * signal" test. cdda2img found that by running it, corrected it, and measured
 * the corrected form on 5,938 real sectors with no false positive.
 *
 * FLOAT64 ON PURPOSE. Every rate is (differing bytes) / (overlap bytes) as a
 * double, and every test compares doubles exactly as numpy does (IEEE division,
 * and halving is exact). That makes this function agree with the reference bit
 * for bit, including on the boundary of each `<=`, which is what lets their
 * six-container table serve as our acceptance test. An exact-rational version
 * would be marginally "more correct" and would not be the definition.
 *
 * Cost: two passes of (2 * max_k + 1) overlaps, about 2.8 M byte compares for
 * one sector. A test function; the engine's use of it (0.56.0) runs only on
 * copies that already disagree. */

#include <string.h>

#include "../internal.h"

#define SHIFT_K_CAP 588 /* samples: half a sector */

/* Differing bytes between x[i] and y[i + b] over their overlap, b = 4k. */
static uint32_t diff_at(const uint8_t *x, const uint8_t *y, uint32_t n,
                        int32_t b, uint32_t *overlap)
{
    const uint8_t *a, *c;
    uint32_t len, d = 0;

    if (b >= 0) {
        a = x;
        c = y + b;
        len = n - (uint32_t)b;
    } else {
        a = x + (uint32_t)(-b);
        c = y;
        len = n - (uint32_t)(-b);
    }
    for (uint32_t i = 0; i < len; i++)
        d += a[i] != c[i];
    *overlap = len;
    return d;
}

int accudisc_shift_test(const uint8_t *x, const uint8_t *y, uint32_t len,
                        int32_t *shift)
{
    if (!x || !y || len < 8 || len % 4 != 0)
        return ACCUDISC_ERR_INVAL;

    uint32_t samples = len / 4;
    int32_t max_k = (int32_t)(samples / 2);
    if (max_k > SHIFT_K_CAP)
        max_k = SHIFT_K_CAP;

    /* SIGNAL is checked inside the loop below: s_k > 0 at every k. The
     * reference also asks "not all zero", but an all-zero x has s_k = 0 at
     * every k, so that test can never decide anything the loop does not; a
     * mutant deleting it was equivalent, so it is not transcribed. */
    uint32_t o0;
    double r0 = (double)diff_at(x, y, len, 0, &o0) / (double)o0;
    double best = 0.0;
    int found = 0;
    int32_t best_k = 0;

    for (int32_t k = -max_k; k <= max_k; k++) {
        if (k == 0)
            continue;
        uint32_t o, os;
        double self = (double)diff_at(x, x, len, 4 * k, &os) / (double)os;

        /* ... and unlike itself at every shift in range. */
        if (self == 0.0)
            return 0;
        double r = (double)diff_at(x, y, len, 4 * k, &o) / (double)o;

        /* `r <= 0.5` is IMPLIED by `r <= self / 2`, since self is a rate and
         * at most 1. Kept because it is in the definition; a mutant deleting
         * it is equivalent, and no input can tell them apart. */
        if (r <= r0 / 2 && r <= 0.5 && r <= self / 2 && (!found || r < best)) {
            best = r;
            best_k = k;
            found = 1; /* strict '<': the FIRST k wins a tie, as argmin does */
        }
    }
    if (!found)
        return 0;
    if (shift)
        *shift = best_k;
    return 1;
}
