/* SPDX-License-Identifier: MIT */
/* The AVX2 syndrome sweep must equal the scalar one BIT FOR BIT.
 *
 * The scalar kernel is the definition of correct: it is the loop the eight-arm
 * A/B against real CTDB parity was passing before any of this existed. The
 * vector kernel is an acceleration, so the only thing worth asserting about it
 * is that it changed nothing. Both accumulator contents and the fused CRC are
 * compared, over geometries chosen to hit the places a vector kernel breaks:
 *
 *   - a stride that is NOT a multiple of 32, so the scalar tail runs and the
 *     split/recombine boundary is exercised rather than skipped;
 *   - npar = 1, so only the alpha^0 plane exists and the table path never runs;
 *   - npar = ADSC_RS16_MAX_NPAR, the largest table set;
 *   - values including 0 and 0xFFFF, because a log/antilog field goes wrong at
 *     zero and a nibble table goes wrong at the top of its range.
 *
 * THE GUARD THAT MAKES THIS TEST MEAN ANYTHING is that the two runs really did
 * use different kernels. Without it this file would pass by comparing the
 * scalar path against itself on any machine without AVX2 — the failure mode
 * this subsystem keeps producing. If the vector kernel is unavailable the test
 * SKIPS (77) and says so, rather than passing vacuously.
 */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "repair/ctdb_internal.h"
#include "repair/rs16.h"

static int failures;

static void expect(int cond, const char *fmt, ...)
{
    va_list ap;

    if (cond)
        return;
    failures++;
    fputs("FAIL: ", stdout);
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    putchar('\n');
}

/* xorshift32: the same stream on every platform, so a failure is reproducible
 * rather than dependent on the host's rand(). */
static uint32_t rng_state = 0x1234567u;

static uint16_t rng16(void)
{
    rng_state ^= rng_state << 13;
    rng_state ^= rng_state >> 17;
    rng_state ^= rng_state << 5;
    return (uint16_t)(rng_state >> 8);
}

static int run_case(unsigned S, unsigned sc, unsigned npar, const char *what)
{
    size_t words = (size_t)S * (sc + 2u);
    uint16_t *pcm = malloc(words * 2u);
    uint16_t *a_scalar = calloc((size_t)S * npar, 2u);
    uint16_t *a_vector = calloc((size_t)S * npar, 2u);
    uint32_t crc_scalar = 0, crc_vector = 0;
    const char *k1, *k2;
    int bad = 0;

    if (!pcm || !a_scalar || !a_vector) {
        fprintf(stderr, "allocation failed\n");
        exit(2);
    }
    for (size_t i = 0; i < words; i++)
        pcm[i] = rng16();
    /* Pin the two values a table-driven field is most likely to mishandle. */
    pcm[S + 0] = 0x0000u;
    pcm[S + 1] = 0xFFFFu;
    pcm[S + 2] = 0x0001u;

    setenv("ACCUDISC_REPAIR_KERNEL", "scalar", 1);
    k1 = adsc_ctdb_sweep_kernel();
    adsc_ctdb_sweep(pcm, S, S, sc, npar, a_scalar, &crc_scalar);

    unsetenv("ACCUDISC_REPAIR_KERNEL");
    k2 = adsc_ctdb_sweep_kernel();
    adsc_ctdb_sweep(pcm, S, S, sc, npar, a_vector, &crc_vector);

    if (strcmp(k1, "scalar") || !strcmp(k2, "scalar")) {
        free(pcm); free(a_scalar); free(a_vector);
        return 1; /* no second kernel to compare against */
    }

    expect(memcmp(a_scalar, a_vector, (size_t)S * npar * 2u) == 0,
           "%s: %s and %s disagree on the accumulator", what, k1, k2);
    expect(crc_scalar == crc_vector,
           "%s: crc %08x (%s) vs %08x (%s)", what, crc_scalar, k1,
           crc_vector, k2);

    /* Not a tautology: a kernel that wrote nothing would satisfy both checks
     * above. The sweep of random data must leave SOMETHING non-zero. */
    {
        int nonzero = 0;

        for (size_t i = 0; i < (size_t)S * npar; i++)
            if (a_vector[i]) { nonzero = 1; break; }
        expect(nonzero, "%s: the accumulator is entirely zero", what);
    }

    free(pcm); free(a_scalar); free(a_vector);
    return bad;
}

int main(void)
{
    int skipped;

    /* S is always even (it is 2 * wire_stride). 11776 is a multiple of 32;
     * 11760 — the real one — is not, and 34 is small enough that the vector
     * body runs once and the tail carries the rest. */
    skipped = run_case(11776u, 12u, 16u, "S multiple of 32, npar 16");
    skipped |= run_case(11760u, 12u, 16u, "real stride, npar 16");
    skipped |= run_case(34u, 8u, 8u, "short stride, mostly tail");
    skipped |= run_case(11760u, 4u, 1u, "npar 1, alpha^0 plane only");
    skipped |= run_case(11760u, 4u, ADSC_RS16_MAX_NPAR, "npar max");

    if (skipped) {
        printf("test_sweep: SKIP — no vector kernel on this machine, so the "
               "comparison would have been scalar against scalar\n");
        return 77;
    }
    printf("test_sweep: %d failures\n", failures);
    return failures ? 1 : 0;
}
