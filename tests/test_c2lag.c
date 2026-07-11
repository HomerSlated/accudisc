/* C2/audio lag correlation core: synthetic read pairs with a known,
 * planted bitmap shift must yield that shift — and nothing else. */

#include <assert.h>
#include <string.h>

#include <accudisc/accudisc.h>

#include "drive/c2lag.h"

/* Set the C2 bit describing audio byte `byte` under lag L (in sample
 * pairs): the flag lands at bitmap position byte + 4L. */
static void set_flag(uint8_t *c2, int byte, int lag_pairs)
{
    int p = byte + 4 * lag_pairs;

    assert(p >= 0 && p < (int)ACCUDISC_BYTES_AUDIO);
    c2[p >> 3] |= (uint8_t)(1u << (7 - (p & 7)));
}

/* One synthetic sector pair: `nwrong` isolated unstable bytes, every one
 * flagged in read A at the planted lag. Stride keeps the wrong bytes
 * farther apart than any candidate shift, so only the true lag scores. */
static void make_pair(uint8_t *aud_a, uint8_t *c2_a,
                      uint8_t *aud_b, uint8_t *c2_b,
                      int nwrong, int lag_pairs, unsigned seed)
{
    for (int i = 0; i < (int)ACCUDISC_BYTES_AUDIO; i++)
        aud_a[i] = (uint8_t)(i * 13 + seed);
    memcpy(aud_b, aud_a, ACCUDISC_BYTES_AUDIO);
    memset(c2_a, 0, ACCUDISC_BYTES_C2);
    memset(c2_b, 0, ACCUDISC_BYTES_C2);

    int stride = 67; /* > 2*4*ADSC_C2LAG_MAX_SHIFT: shifts never collide */
    for (int w = 0; w < nwrong; w++) {
        int byte = 40 + w * stride;
        aud_b[byte] ^= 0x5a; /* unstable across the pair */
        set_flag(c2_a, byte, lag_pairs);
    }
}

int main(void)
{
    uint8_t aud_a[ACCUDISC_BYTES_AUDIO], aud_b[ACCUDISC_BYTES_AUDIO];
    uint8_t c2_a[ACCUDISC_BYTES_C2], c2_b[ACCUDISC_BYTES_C2];
    accudisc_c2_lag res;

    /* Positive lag +2 (the PX-716A magnitude), several sectors to clear
     * the evidence floors: 4 sectors x 30 flags = 120 observations. */
    struct adsc_c2lag_acc acc;
    memset(&acc, 0, sizeof(acc));
    for (unsigned sec = 0; sec < 4; sec++) {
        make_pair(aud_a, c2_a, aud_b, c2_b, 30, 2, sec * 7);
        adsc_c2lag_add(&acc, aud_a, c2_a, aud_b, c2_b);
    }
    assert(adsc_c2lag_result(&acc, &res) == ACCUDISC_OK);
    assert(res.lag_pairs == 2);
    assert(res.peak_milli >= 900);
    assert(res.runner_milli < res.peak_milli);
    assert(res.flags_used >= ADSC_C2LAG_MIN_FLAGS);
    assert(res.diff_bytes >= ADSC_C2LAG_MIN_DIFFS);

    /* Negative lag. */
    memset(&acc, 0, sizeof(acc));
    for (unsigned sec = 0; sec < 4; sec++) {
        make_pair(aud_a, c2_a, aud_b, c2_b, 30, -3, sec * 7);
        adsc_c2lag_add(&acc, aud_a, c2_a, aud_b, c2_b);
    }
    assert(adsc_c2lag_result(&acc, &res) == ACCUDISC_OK);
    assert(res.lag_pairs == -3);

    /* Zero lag (an honest drive) is a valid, reportable verdict. */
    memset(&acc, 0, sizeof(acc));
    for (unsigned sec = 0; sec < 4; sec++) {
        make_pair(aud_a, c2_a, aud_b, c2_b, 30, 0, sec * 7);
        adsc_c2lag_add(&acc, aud_a, c2_a, aud_b, c2_b);
    }
    assert(adsc_c2lag_result(&acc, &res) == ACCUDISC_OK);
    assert(res.lag_pairs == 0);

    /* No flags at all: inconclusive, never a made-up number. */
    memset(&acc, 0, sizeof(acc));
    make_pair(aud_a, c2_a, aud_b, c2_b, 0, 0, 1);
    adsc_c2lag_add(&acc, aud_a, c2_a, aud_b, c2_b);
    assert(adsc_c2lag_result(&acc, &res) == ACCUDISC_ERR_NOTFOUND);

    /* Two equal peaks (each wrong byte flagged at BOTH +1 and -1) is
     * ambiguous: contrast gate must refuse a verdict. */
    memset(&acc, 0, sizeof(acc));
    for (unsigned sec = 0; sec < 4; sec++) {
        make_pair(aud_a, c2_a, aud_b, c2_b, 30, 1, sec * 7);
        for (int w = 0; w < 30; w++)
            set_flag(c2_a, 40 + w * 67, -1); /* mirror peak at -1 */
        adsc_c2lag_add(&acc, aud_a, c2_a, aud_b, c2_b);
    }
    assert(adsc_c2lag_result(&acc, &res) == ACCUDISC_ERR_NOTFOUND);

    /* Flags incoherent with instability (planted far off every candidate
     * shift) must not fake a peak: below the peak floor -> inconclusive. */
    memset(&acc, 0, sizeof(acc));
    for (unsigned sec = 0; sec < 4; sec++) {
        make_pair(aud_a, c2_a, aud_b, c2_b, 30, 0, sec * 7);
        memset(c2_a, 0, ACCUDISC_BYTES_C2);
        for (int w = 0; w < 30; w++) /* 33 bytes off: outside +-32 range */
            set_flag(c2_a, 40 + w * 67 + 33, 0);
        adsc_c2lag_add(&acc, aud_a, c2_a, aud_b, c2_b);
    }
    assert(adsc_c2lag_result(&acc, &res) == ACCUDISC_ERR_NOTFOUND);

    return 0;
}
