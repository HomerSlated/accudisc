/* cdrdao .toc parser: a small audio .toc must yield the right track count,
 * MCN, ISRC/pre-emphasis flags, cumulative LBAs, pre-gaps and lead-out. */

#include <assert.h>
#include <string.h>

#include <accudisc/accudisc.h>

#include "write/write.h"

static const char *TOC =
    "CD_DA\n"
    "CATALOG \"1234567890123\"\n"
    "\n"
    "// Track 1\n"
    "TRACK AUDIO\n"
    "NO COPY\n"
    "NO PRE_EMPHASIS\n"
    "ISRC \"AAAAA1234567\"\n"
    "FILE \"x.bin\" 00:00:00 00:10:00\n"
    "\n"
    "// Track 2\n"
    "TRACK AUDIO\n"
    "NO COPY\n"
    "PRE_EMPHASIS\n"
    "ISRC \"BBBBB7654321\"\n"
    "FILE \"x.bin\" 00:10:00 00:20:00\n"
    "START 00:00:50\n";

int main(void)
{
    struct adsc_write_toc toc;

    assert(adsc_toc_parse_cue(TOC, &toc) == ACCUDISC_OK);
    assert(toc.ntracks == 2);
    assert(strcmp(toc.mcn, "1234567890123") == 0);

    /* Track 1: 750 sectors (00:10:00), no pre-gap, at LBA 0. */
    assert(toc.track[0].audio == 1);
    assert(toc.track[0].preemphasis == 0);
    assert(strcmp(toc.track[0].isrc, "AAAAA1234567") == 0);
    assert(toc.track[0].sectors == 750);
    assert(toc.track[0].index1_lba == 0);
    assert(toc.track[0].pregap == 0);
    assert(toc.track[0].file_offset == 0);

    /* Track 2: starts at LBA 750, 50-frame pre-gap => index1 at 800,
     * 1500 sectors, BIN offset 750*2352, pre-emphasis set. */
    assert(toc.track[1].preemphasis == 1);
    assert(toc.track[1].pregap == 50);
    assert(toc.track[1].index1_lba == 800);
    assert(toc.track[1].sectors == 1500);
    assert(toc.track[1].file_offset == (uint64_t)750 * 2352);

    /* Lead-out = sum of track lengths. */
    assert(toc.leadout_lba == 2250);

    /* Degenerate input rejected. */
    struct adsc_write_toc bad;
    assert(adsc_toc_parse_cue("CD_DA\n", &bad) == ACCUDISC_ERR_INVAL);

    return 0;
}
