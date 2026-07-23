/* Equivalence test for the memcmp fast-path in adsc_audio_diff (optimisation
 * #1, 2026-07-23). The fast path returns 0 for equal buffers without the byte
 * count; this asserts it is byte-identical to the plain scalar mismatch-count
 * for every input — a lone flipped byte still counts 1 (the fast path must not
 * swallow a mismatch), equal buffers count 0, all-differ counts the full
 * length, and random fuzzing agrees with a reference oracle. */

#include <assert.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <accudisc/accudisc.h> /* ACCUDISC_BYTES_AUDIO */

#include "read/engine.h"

/* The pre-optimisation implementation, kept as the oracle. */
static uint32_t ref_diff(const uint8_t *a, const uint8_t *b)
{
    uint32_t n = 0;

    for (uint32_t i = 0; i < ACCUDISC_BYTES_AUDIO; i++)
        n += a[i] != b[i];
    return n;
}

int main(void)
{
    uint8_t a[ACCUDISC_BYTES_AUDIO], b[ACCUDISC_BYTES_AUDIO];

    /* Equal buffers: the fast path returns 0. */
    for (uint32_t i = 0; i < ACCUDISC_BYTES_AUDIO; i++)
        a[i] = (uint8_t)(i * 7 + 1);
    memcpy(b, a, sizeof(b));
    assert(adsc_audio_diff(a, b) == 0);
    assert(adsc_audio_diff(a, b) == ref_diff(a, b));

    /* A single flipped byte at each boundary position must count as exactly 1 —
     * the fast path must not let a lone mismatch slip through as "equal". */
    const uint32_t probes[] = {0, 1, ACCUDISC_BYTES_AUDIO / 2,
                               ACCUDISC_BYTES_AUDIO - 2,
                               ACCUDISC_BYTES_AUDIO - 1};
    for (size_t p = 0; p < sizeof(probes) / sizeof(probes[0]); p++) {
        memcpy(b, a, sizeof(b));
        b[probes[p]] ^= 0xff;
        assert(adsc_audio_diff(a, b) == 1);
        assert(adsc_audio_diff(a, b) == ref_diff(a, b));
    }

    /* Every byte differs: count equals the full payload length. */
    for (uint32_t i = 0; i < ACCUDISC_BYTES_AUDIO; i++)
        b[i] = (uint8_t)(a[i] ^ 0xff);
    assert(adsc_audio_diff(a, b) == ACCUDISC_BYTES_AUDIO);
    assert(adsc_audio_diff(a, b) == ref_diff(a, b));

    /* Random fuzzing against the oracle: independent random bytes exercise the
     * count path, and 1-in-8 forced-equal buffers exercise the fast path. */
    srand(12345);
    for (int t = 0; t < 20000; t++) {
        for (uint32_t i = 0; i < ACCUDISC_BYTES_AUDIO; i++) {
            a[i] = (uint8_t)rand();
            b[i] = (uint8_t)rand();
        }
        if ((t & 7) == 0)
            memcpy(b, a, sizeof(b)); /* force equality -> fast path */
        assert(adsc_audio_diff(a, b) == ref_diff(a, b));
    }

    return 0;
}
