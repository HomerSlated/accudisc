/* Transfer lengths handed to ATAPI must be even.
 *
 * ATAPI moves data 16 bits at a time, so an ODD allocation length is rejected
 * by the host adapter before the drive ever answers — Linux reports
 * host_status DID_ERROR (0x07) with no sense, surfacing as a bare
 * ACCUDISC_ERR_IO. Any two-step read that sizes its second transfer from a
 * returned data-length header must round it.
 *
 * Regression for the 2026-07-24 field report: `accudisc toc` reported
 * degrade=leadin_unreadable on disc after disc. A full TOC is
 * 4 + 11*ndesc bytes with ndesc = 3 pointers (A0/A1/A2) + ntracks, i.e.
 * 37 + 11*ntracks — ODD on every disc with an EVEN track count. So roughly
 * half of all discs failed, and the failure masqueraded as disc damage rather
 * than a transfer-length fault. The measured pair is asserted below: an
 * 11-track disc (even length) always worked; a 12-track disc (odd) never did.
 */

#include <assert.h>
#include <stdint.h>

#include <accudisc/accudisc.h>

#include "mmc/mmc.h"

/* Full TOC response length for a single-session disc with n audio tracks:
 * 4-byte header + 11 bytes per descriptor, descriptors = A0 + A1 + A2 + n. */
static uint32_t fulltoc_len(uint32_t ntracks)
{
    return 4u + 11u * (3u + ntracks);
}

int main(void)
{
    /* The rounding itself. */
    assert(adsc_alloc_even(0) == 0);
    assert(adsc_alloc_even(1) == 2);
    assert(adsc_alloc_even(2) == 2);
    assert(adsc_alloc_even(157) == 158);
    assert(adsc_alloc_even(158) == 158);
    assert(adsc_alloc_even(169) == 170);

    /* Rounding is idempotent — a second pass must not grow the length again. */
    for (uint32_t i = 0; i < 64; i++)
        assert(adsc_alloc_even(adsc_alloc_even(i)) == adsc_alloc_even(i));

    /* The measured field cases, both drives and discs real:
     *   11 tracks -> 158 bytes, even -> read fine all along;
     *   12 tracks -> 169 bytes, odd  -> DID_ERROR until rounded. */
    assert(fulltoc_len(11) == 158 && (fulltoc_len(11) & 1u) == 0);
    assert(fulltoc_len(12) == 169 && (fulltoc_len(12) & 1u) == 1);
    assert(adsc_alloc_even(fulltoc_len(12)) == 170);

    /* The general rule that made it look random: length parity follows
     * (1 + ntracks), so EVERY even-track disc produced an odd transfer. After
     * rounding, no track count can. */
    for (uint32_t n = 1; n <= 99; n++) {
        assert(((fulltoc_len(n) & 1u) == 1u) == ((n % 2u) == 0u));
        assert((adsc_alloc_even(fulltoc_len(n)) & 1u) == 0);
    }

    return 0;
}
