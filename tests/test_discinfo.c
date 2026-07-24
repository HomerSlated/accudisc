/* Phase 3 Step B4: the writable lead-in extent derived from the lead-in start
 * MSF (READ DISC INFORMATION bytes 17-19), per RECORDING_PLAN §11.6 and cdrdao
 * GenericMMC.cc:414-432.
 *
 * The extent is a property of the BLANK, not of how much CD-Text there is, so
 * getting it wrong mis-sizes every CD-Text lead-in write. The arithmetic is a
 * subtraction from a fixed origin, which is exactly the shape that underflows
 * on unexpected input — so the out-of-window cases are asserted, not assumed.
 */

#include <assert.h>
#include <stdint.h>

#include <accudisc/accudisc.h>

#include "write/write.h"

int main(void)
{
    /* A typical CD-R: ATIP lead-in start 97:24:01 (the Taiyo Yuden blank we
     * measure against). LBA 438301 -> 450000 - 438301. */
    assert(adsc_leadin_len_from_msf(97, 24, 1) == 11699);

    /* Window edges. 80:00:00 = LBA 360000 is the first accepted start. */
    assert(adsc_leadin_len_from_msf(80, 0, 0) == 450000u - 360000u); /* 90000 */
    assert(adsc_leadin_len_from_msf(99, 59, 74) == 450000u - 449999u); /* 1 */

    /* Just below the window falls back rather than producing a huge extent. */
    assert(adsc_leadin_len_from_msf(79, 59, 74) == 4500);

    /* At/above 100:00:00 the subtraction would yield 0 or underflow. Both must
     * fall back — a zero-length lead-in would write nothing, and an underflow
     * would ask the drive for ~4 billion sectors. */
    assert(adsc_leadin_len_from_msf(100, 0, 0) == 4500);
    assert(adsc_leadin_len_from_msf(120, 0, 0) == 4500);
    assert(adsc_leadin_len_from_msf(255, 255, 255) == 4500);

    /* A drive that reports zeros (or no ATIP) gets the 1-minute fallback. */
    assert(adsc_leadin_len_from_msf(0, 0, 0) == 4500);

    return 0;
}
