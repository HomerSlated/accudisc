/* ATIP manufacturer lookup: the (sec, frame-decade) key must resolve the
 * right maker, distinguish makers that share a `sec`, and return NULL for
 * codes not in the catalog rather than guessing. */

#include <assert.h>
#include <string.h>

#include <accudisc/accudisc.h>

static int is(const char *got, const char *want)
{
    return got && strcmp(got, want) == 0;
}

int main(void)
{
    /* Exact and frame-variant matches (real discs carry 97:24:01 for the
     * 97:24:00 catalog entry — decade 0 must still resolve Taiyo Yuden). */
    assert(is(accudisc_atip_manufacturer(97, 24, 0), "Taiyo Yuden"));
    assert(is(accudisc_atip_manufacturer(97, 24, 1), "Taiyo Yuden"));
    assert(is(accudisc_atip_manufacturer(97, 24, 9), "Taiyo Yuden"));

    /* Same `sec`, different decade => different manufacturer. This is why the
     * frame decade is required, not just min:sec. */
    assert(!is(accudisc_atip_manufacturer(97, 24, 20), "Taiyo Yuden"));
    assert(accudisc_atip_manufacturer(97, 24, 20) != NULL);

    /* Canonical codes validated against cdrecord/Nero. */
    assert(is(accudisc_atip_manufacturer(97, 26, 60), "CMC Magnetics Corporation"));
    assert(is(accudisc_atip_manufacturer(97, 34, 23), "Mitsubishi Chemical Corporation"));

    /* Cross-referenced (cdrecord-only) codes folded into the union. */
    assert(is(accudisc_atip_manufacturer(97, 32, 0), "TDK Corporation"));
    assert(is(accudisc_atip_manufacturer(97, 31, 0), "Ritek"));

    /* Unlisted codes return NULL — report the raw digits, never a wrong name. */
    assert(accudisc_atip_manufacturer(97, 59, 40) == NULL);
    assert(accudisc_atip_manufacturer(98, 24, 0) == NULL);  /* wrong min */

    return 0;
}
