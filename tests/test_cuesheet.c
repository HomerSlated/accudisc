/* SEND CUE SHEET builder: a known 2-track audio layout must produce the exact
 * entry sequence (MCN, lead-in, per-track pre-gap/index-1, lead-out) with the
 * right CTL/ADR, track numbers and absolute MSF (LBA + 150). */

#include <assert.h>
#include <string.h>

#include <accudisc/accudisc.h>

#include "write/write.h"

#define E(i) (buf + (i) * 8)

int main(void)
{
    struct adsc_write_toc toc;
    uint8_t buf[256];
    uint32_t len = 0;

    memset(&toc, 0, sizeof(toc));
    strcpy(toc.mcn, "0123456789012");
    toc.ntracks = 2;
    toc.leadout_lba = 20000;
    toc.track[0].audio = 1;
    toc.track[0].index1_lba = 0;       /* track 1 at LBA 0 */
    toc.track[1].audio = 1;
    toc.track[1].index1_lba = 10000;
    toc.track[1].pregap = 150;         /* 2s pre-gap before track 2 */

    assert(adsc_cuesheet_build(&toc, buf, sizeof(buf), &len) == ACCUDISC_OK);
    assert(len == 8 * 8);              /* MCN2 + leadin + t1(gap,idx1)
                                        * + t2(pregap,idx1) + leadout */

    /* MCN: two ADR=2 entries, digits 7 + 6. */
    assert(E(0)[0] == 0x02 && E(0)[1] == '0' && E(0)[7] == '6');
    assert(E(1)[0] == 0x02 && E(1)[1] == '7' && E(1)[6] == '2' && E(1)[7] == 0);

    /* Lead-in: CTL/ADR 0x01, TNO 0, MSF 0. */
    assert(E(2)[0] == 0x01 && E(2)[1] == 0 && E(2)[5] == 0 && E(2)[7] == 0);

    /* Track 1 gap (INDEX 0) anchors the session at 00:00:00. */
    assert(E(3)[0] == 0x01 && E(3)[1] == 1 && E(3)[2] == 0);
    assert(E(3)[5] == 0 && E(3)[6] == 0 && E(3)[7] == 0);

    /* Track 1 INDEX 1 at LBA 0 -> 00:02:00. */
    assert(E(4)[1] == 1 && E(4)[2] == 1);
    assert(E(4)[5] == 0 && E(4)[6] == 2 && E(4)[7] == 0);

    /* Track 2 pre-gap (INDEX 0) at LBA 9850 -> abs 10000 -> 02:13:25. */
    assert(E(5)[1] == 2 && E(5)[2] == 0);
    assert(E(5)[5] == 2 && E(5)[6] == 13 && E(5)[7] == 25);

    /* Track 2 INDEX 1 at LBA 10000 -> abs 10150 -> 02:15:25. */
    assert(E(6)[1] == 2 && E(6)[2] == 1);
    assert(E(6)[5] == 2 && E(6)[6] == 15 && E(6)[7] == 25);

    /* Lead-out: TNO 0xAA at LBA 20000 -> abs 20150 -> 04:28:50. */
    assert(E(7)[0] == 0x01 && E(7)[1] == 0xaa && E(7)[2] == 1);
    assert(E(7)[5] == 4 && E(7)[6] == 28 && E(7)[7] == 50);

    /* Pre-emphasis and copy set the CTL nibble. */
    memset(&toc, 0, sizeof(toc));
    toc.ntracks = 1;
    toc.leadout_lba = 100;
    toc.track[0].audio = 1;
    toc.track[0].preemphasis = 1;
    toc.track[0].copy = 1;
    assert(adsc_cuesheet_build(&toc, buf, sizeof(buf), &len) == ACCUDISC_OK);
    /* leadin(2) then track1 gap(3) + idx1(4)... index 1 entry is E(2). */
    assert((E(2)[0] & 0xf0) == 0x30);  /* CTL 0x20 copy | 0x10 pre-emph */

    /* Too-small buffer is a clean error, not a smash. */
    assert(adsc_cuesheet_build(&toc, buf, 8, &len) == ACCUDISC_ERR_SHORT);

    return 0;
}
