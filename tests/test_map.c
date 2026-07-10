/* Status-map byte encoding tests. */

#include <assert.h>

#include <accudisc/accudisc.h>

#include "read/engine.h"

int main(void)
{
    /* States and severities round-trip through the accessor macros. */
    assert(ACCUDISC_MAP_STATE(ACCUDISC_MAP_OK) == ACCUDISC_MAP_OK);
    assert(ACCUDISC_MAP_SEVERITY(ACCUDISC_MAP_OK) == 0);

    /* C2 severity is ~log2(bits)+1, clamped to the nibble. */
    uint8_t b1 = adsc_map_c2_byte(1);
    assert(ACCUDISC_MAP_STATE(b1) == ACCUDISC_MAP_C2);
    assert(ACCUDISC_MAP_SEVERITY(b1) == 1);

    assert(ACCUDISC_MAP_SEVERITY(adsc_map_c2_byte(2)) == 2);
    assert(ACCUDISC_MAP_SEVERITY(adsc_map_c2_byte(3)) == 2);
    assert(ACCUDISC_MAP_SEVERITY(adsc_map_c2_byte(4)) == 3);
    assert(ACCUDISC_MAP_SEVERITY(adsc_map_c2_byte(255)) == 8);
    /* Worst case: all 2352 bits fired. */
    assert(ACCUDISC_MAP_SEVERITY(adsc_map_c2_byte(2352)) == 12);
    /* Clamp holds even for impossible counts. */
    assert(ACCUDISC_MAP_SEVERITY(adsc_map_c2_byte(0xffffffffu)) == 15);

    /* Severity never collides state into another nibble. */
    assert(ACCUDISC_MAP_STATE(adsc_map_c2_byte(0xffffffffu)) ==
           ACCUDISC_MAP_C2);

    /* Recovered: severity = attempts, clamped. */
    uint8_t r = adsc_map_recovered_byte(3);
    assert(ACCUDISC_MAP_STATE(r) == ACCUDISC_MAP_RECOVERED);
    assert(ACCUDISC_MAP_SEVERITY(r) == 3);
    assert(ACCUDISC_MAP_SEVERITY(adsc_map_recovered_byte(99)) == 15);

    /* Suspect: severity ~log2 of disagreeing bytes. */
    uint8_t s = adsc_map_suspect_byte(256);
    assert(ACCUDISC_MAP_STATE(s) == ACCUDISC_MAP_SUSPECT);
    assert(ACCUDISC_MAP_SEVERITY(s) == 9);
    assert(ACCUDISC_MAP_SEVERITY(adsc_map_suspect_byte(1)) == 1);

    /* Audio diff counts differing bytes over one sector. */
    uint8_t a[2352] = {0}, b[2352] = {0};
    assert(adsc_audio_diff(a, b) == 0);
    b[0] = 1;
    b[2351] = 0xff;
    assert(adsc_audio_diff(a, b) == 2);

    return 0;
}
