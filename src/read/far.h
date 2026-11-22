/* Where a cache-defeating far read goes (0.51.0). Pure, so it is testable
 * without a drive, and shared by the read engine and the c2lag and
 * accurate-stream probes, which each had their own copy of the old rule.
 *
 * The span being re-read is [a, a + n). The readable range is [lo, hi), with
 * hi == 0 meaning UNKNOWN (accudisc_read_req.readable_hi's sentinel).
 *
 *   1. a - dist, if that is in range;
 *   2. else (a + n - 1) + dist, if that is in range;
 *   3. else, range known only: the in-range sector furthest BEHIND the span
 *      (lo), if lo < a; failing that, the last in-range sector past it
 *      (hi - 1), if hi - 1 > a + n - 1. Behind first: the sector just past a
 *      span sits in the drive's read-ahead and is served from the buffer
 *      without a disc access (measured 2026-11-22, PX-716A: 0.7-0.9 ms, no
 *      flush), while one behind it is a real read and flushes;
 *   4. else no target.
 *
 * Every candidate is checked against BOTH ends of the range, so a span that
 * lies partly or wholly outside it (a forced read) cannot push a target past
 * hi or below lo, and no candidate falls inside the span itself.
 *
 * With hi == 0 the result is exactly 0.50.1's: back if a >= dist, else
 * forward from the span's last sector (unbounded, as before; only the uint32
 * limit can refuse it). */
#ifndef ADSC_READ_FAR_H
#define ADSC_READ_FAR_H

#include <stdint.h>

enum adsc_far {
    ADSC_FAR_NONE = 0, /* no target */
    ADSC_FAR_FULL = 1, /* dist away */
    ADSC_FAR_NEAR = 2  /* in range but closer than dist */
};

static inline int adsc_far_in(uint64_t t, uint64_t a, uint64_t n, uint64_t lo,
                              uint64_t hi)
{
    return t >= lo && t < hi && (t < a || t >= a + n);
}

static inline enum adsc_far adsc_far_target(uint32_t a, uint32_t n,
                                            uint32_t lo, uint32_t hi,
                                            uint32_t dist, uint32_t *out)
{
    uint64_t A = a, N = n ? n : 1, D = dist;
    uint64_t L = hi ? lo : 0, H = hi ? hi : (uint64_t)UINT32_MAX + 1;
    uint64_t end = A + N - 1;

    if (A >= D && adsc_far_in(A - D, A, N, L, H)) {
        *out = (uint32_t)(A - D);
        return ADSC_FAR_FULL;
    }
    if (adsc_far_in(end + D, A, N, L, H)) {
        *out = (uint32_t)(end + D);
        return ADSC_FAR_FULL;
    }
    if (!hi)
        return ADSC_FAR_NONE;
    if (adsc_far_in(L, A, N, L, H) && L < A) {
        *out = (uint32_t)L;
        return ADSC_FAR_NEAR;
    }
    if (H - 1 > end && adsc_far_in(H - 1, A, N, L, H)) {
        *out = (uint32_t)(H - 1);
        return ADSC_FAR_NEAR;
    }
    return ADSC_FAR_NONE;
}

#endif
