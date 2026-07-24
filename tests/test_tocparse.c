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

    /* --- TOC injection via a newline inside a quoted value ------------------
     * A line-oriented scan that loses quote context lets a value carrying an
     * embedded '\n' spill its tail onto the next physical line, where a
     * column-0 keyword is matched as a real directive. Here a CD_TEXT TITLE
     * smuggles in a whole phantom track with a forged ISRC. The producer that
     * would emit this is a hostile or buggy one; the parser must refuse the
     * whole file rather than silently grow a track and change the lead-out.
     * A string literal may not span a line, so this is ACCUDISC_ERR_INVAL. */
    {
        static const char *INJECT =
            "CD_DA\n"
            "TRACK AUDIO\n"
            "CD_TEXT { LANGUAGE 0 {\n"
            "  TITLE \"innocent\n"
            "TRACK AUDIO\n"
            "ISRC \"ZZZZZ9999999\"\n"
            "FILE \"phantom.bin\" 00:00:00 05:00:00\n"
            "\"\n"
            "} }\n"
            "FILE \"real.bin\" 00:00:00 00:10:00\n";
        struct adsc_write_toc inj;
        assert(adsc_toc_parse_cue(INJECT, &inj) == ACCUDISC_ERR_INVAL);
    }

    /* A legitimately quoted single-line value with balanced quotes still
     * parses — the guard rejects unterminated quotes, not quotes as such. */
    {
        static const char *OK =
            "CD_DA\n"
            "TRACK AUDIO\n"
            "TITLE \"A perfectly ordinary title\"\n"
            "FILE \"real.bin\" 00:00:00 00:10:00\n";
        struct adsc_write_toc t2;
        assert(adsc_toc_parse_cue(OK, &t2) == ACCUDISC_OK);
        assert(t2.ntracks == 1);
    }

    return 0;
}
