/* Rotation classifier over synthetic GET PERFORMANCE curves. Pure function, no
 * hardware — one case per shape, plus the real PX-716A curve. */

#include <assert.h>
#include <stddef.h>

#include <accudisc/accudisc.h>

/* kB/s helpers: 1x CD = 176.4, so Nx ~= N*176. */
#define KX(n) ((uint32_t)((n) * 176))

int main(void)
{
    /* No descriptors: the drive rejected GET PERFORMANCE (e.g. CDEmu). Never
     * inferred as any real strategy. */
    assert(accudisc_classify_rotation(NULL, 0) == ACCUDISC_ROTATION_UNKNOWN);

    /* One rising segment = CAV. This is the measured PX-716A CD curve
     * (lba 0..359999, 2999..7056 kB/s = 17x..40x). */
    accudisc_perf_desc cav[] = {{ 0, 2999, 359999, 7056 }};
    assert(accudisc_classify_rotation(cav, 1) == ACCUDISC_ROTATION_CAV);

    /* One flat segment = CLV. */
    accudisc_perf_desc clv[] = {{ 0, KX(4), 359999, KX(4) }};
    assert(accudisc_classify_rotation(clv, 1) == ACCUDISC_ROTATION_CLV);

    /* Rises then caps flat at the rim = partial CAV. */
    accudisc_perf_desc pcav[] = {
        { 0,      KX(17), 200000, KX(40) },  /* CAV region */
        { 200000, KX(40), 359999, KX(40) },  /* capped, flat */
    };
    assert(accudisc_classify_rotation(pcav, 2) == ACCUDISC_ROTATION_PCAV);

    /* Stepped flat zones = zoned CLV. */
    accudisc_perf_desc zclv[] = {
        { 0,      KX(16), 120000, KX(16) },
        { 120000, KX(24), 240000, KX(24) },
        { 240000, KX(32), 359999, KX(32) },
    };
    assert(accudisc_classify_rotation(zclv, 3) == ACCUDISC_ROTATION_ZCLV);

    /* Multiple flat segments at the SAME level = still plain CLV, not zoned. */
    accudisc_perf_desc clv2[] = {
        { 0,      KX(8), 180000, KX(8) },
        { 180000, KX(8), 359999, KX(8) },
    };
    assert(accudisc_classify_rotation(clv2, 2) == ACCUDISC_ROTATION_CLV);

    return 0;
}
