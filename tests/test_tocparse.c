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

    assert(adsc_toc_parse_cue(TOC, &toc, NULL, 0) == ACCUDISC_OK);
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
    assert(adsc_toc_parse_cue("CD_DA\n", &bad, NULL, 0) == ACCUDISC_ERR_INVAL);

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
        assert(adsc_toc_parse_cue(INJECT, &inj, NULL, 0) == ACCUDISC_ERR_INVAL);
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
        assert(adsc_toc_parse_cue(OK, &t2, NULL, 0) == ACCUDISC_OK);
        assert(t2.ntracks == 1);
    }

    /* --- FILE start offset: BYTES or MM:SS:FF, and they are NOT the same ---
     * cdrdao's grammar is "start_offset : byte offset or MM:SS:FF into file",
     * with nothing in the token to say which. The two agree only at zero; a
     * bare count read as frames is off by 2352x, on the burn path, silently.
     * So the discriminating case is a NON-zero offset expressed both ways,
     * which is the pair below. A fixture using 0 — as ours and cdda2img's both
     * did — cannot tell the units apart at all. */
    {
        static const char *BYTES =
            "CD_DA\nTRACK AUDIO\nFILE \"x.bin\" 2352 00:10:00\n";
        static const char *FRAMES =
            "CD_DA\nTRACK AUDIO\nFILE \"x.bin\" 00:00:01 00:10:00\n";
        struct adsc_write_toc tb, tf;
        assert(adsc_toc_parse_cue(BYTES, &tb, NULL, 0) == ACCUDISC_OK);
        assert(adsc_toc_parse_cue(FRAMES, &tf, NULL, 0) == ACCUDISC_OK);
        /* 2352 bytes is ONE sector in; frame 1 is also one sector in. Same
         * place, reached through both syntaxes — that is the equivalence the
         * conversion has to preserve. */
        assert(tb.track[0].file_offset == 2352);
        assert(tf.track[0].file_offset == 2352);
        /* And the anti-test: had the bare form been read as frames it would
         * have become 2352*2352. Asserted explicitly so the regression is
         * named rather than merely absent. */
        assert(tb.track[0].file_offset != (uint64_t)2352 * 2352);
    }

    /* --- the parse error says WHICH line ------------------------------------
     * ACCUDISC_ERR_INVAL for a whole file names neither the line nor the field.
     * cdda2img lost minutes to a rejected FILE line (§117.2); so did we, the
     * same day, on the same line. */
    {
        static const char *NOLEN =
            "CD_DA\n"
            "\n"
            "TRACK AUDIO\n"
            "FILE \"x.bin\" 0\n";      /* cdrdao: length omitted = to EOF */
        struct adsc_write_toc t3;
        char err[256] = "not-touched";
        assert(adsc_toc_parse_cue(NOLEN, &t3, err, sizeof err) ==
               ACCUDISC_ERR_INVAL);
        assert(strncmp(err, "line 4:", 7) == 0);
        assert(strstr(err, "length") != NULL);

        /* err must be CLEARED on success, not left holding a stale message
         * from a previous call — a caller that logs it unconditionally would
         * otherwise report a failure that did not happen. */
        char err2[256] = "stale";
        struct adsc_write_toc t4;
        assert(adsc_toc_parse_cue(TOC, &t4, err2, sizeof err2) == ACCUDISC_OK);
        assert(err2[0] == 0);

        /* NULL err is still legal — every existing caller passes it. */
        assert(adsc_toc_parse_cue(NOLEN, &t3, NULL, 0) == ACCUDISC_ERR_INVAL);
    }

    return 0;
}
