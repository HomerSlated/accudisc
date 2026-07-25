/* Golden test for `accudisc toc` stdout — the exact bytes cdda2img parses.
 *
 * Until cli/format.c was split out of main.c, observing this output required
 * the right physical disc in the tray, so the machine interface documented in
 * docs/reference/cli-machine-interface.md was asserted by nobody. This drives
 * the identical formatter from a captured lead-in blob (vec_fulltoc) through
 * the real acquisition-side decoders:
 *
 *     vec_fulltoc -> accudisc_fulltoc_parse -> adsc_toc_from_fulltoc
 *                 -> adsc_cli_fmt_toc -> compare
 *
 * Nothing here is a re-implementation: every step is the same function the CLI
 * calls. Run with --dump to print the actual output (for regenerating a golden
 * after a DELIBERATE, documented interface change).
 *
 * The three synthetic cases exist because their branches are otherwise
 * reachable only with the right physical disc — a pressing with a pregap, a
 * disc with an unreadable lead-in, a copy-protected one — which is precisely
 * the coverage the pure-function split buys.
 */

#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "../cli/format.h"
#include "toc/toc.h"
#include "vectors.h"

/* Render through the formatter into a heap buffer. */
static char *render(const accudisc_toc *toc, const accudisc_toc_info *info)
{
    static char buf[8192];
    FILE *fp = fmemopen(buf, sizeof(buf), "w");

    assert(fp);
    memset(buf, 0, sizeof(buf));
    adsc_cli_fmt_toc(fp, toc, info);
    fclose(fp); /* NUL-terminates within the buffer */
    return buf;
}

static void expect(const char *what, const char *got, const char *want,
                   int dump)
{
    if (dump) {
        printf("---- %s ----\n%s", what, got);
        return;
    }
    if (strcmp(got, want) != 0) {
        fprintf(stderr,
                "%s: output changed.\n--- want ---\n%s--- got ---\n%s"
                "\nIf this change is DELIBERATE, cli-machine-interface.md is a\n"
                "stable, additive-only contract: update it in the same commit,\n"
                "and add the change to API_PLAN.md's cdda2img ledger (§8).\n",
                what, want, got);
        assert(0 && "cli toc output changed");
    }
}

/* The real captured lead-in: 19 tracks, single session, all audio.
 *
 * PROVENANCE MATTERS HERE. vectors.h captured this CDEmu-mounted, i.e. from an
 * image built out of ripped tracks, so track 1's INDEX 01 sits at LBA 0. The
 * physical ABBA pressing reports track 1 at LBA 33 and lead-out 347208 — every
 * address exactly 33 higher, because the image carries no 33-sector pregap.
 * Track LENGTHS are byte-identical across both, which is what confirms it is
 * the same master rather than a decode fault. test_decode.c:110 independently
 * asserts this fixture's track 1 -> LBA 0 and A2 -> 347175.
 *
 * Consequence: this case does NOT exercise the `pregap` token (no pregap to
 * print). want_pregap below covers it.
 *
 * session_count is synthesized — it comes from READ DISC INFORMATION, a
 * different opcode, with no blob here. */
static const char want_fulltoc[] =
    "track 1 lba 0 sectors 17362 audio session 1\n"
    "track 2 lba 17362 sectors 18175 audio session 1\n"
    "track 3 lba 35537 sectors 18325 audio session 1\n"
    "track 4 lba 53862 sectors 16013 audio session 1\n"
    "track 5 lba 69875 sectors 20575 audio session 1\n"
    "track 6 lba 90450 sectors 19062 audio session 1\n"
    "track 7 lba 109512 sectors 21300 audio session 1\n"
    "track 8 lba 130812 sectors 22150 audio session 1\n"
    "track 9 lba 152962 sectors 14138 audio session 1\n"
    "track 10 lba 167100 sectors 15125 audio session 1\n"
    "track 11 lba 182225 sectors 24475 audio session 1\n"
    "track 12 lba 206700 sectors 18975 audio session 1\n"
    "track 13 lba 225675 sectors 19712 audio session 1\n"
    "track 14 lba 245387 sectors 21638 audio session 1\n"
    "track 15 lba 267025 sectors 14675 audio session 1\n"
    "track 16 lba 281700 sectors 17850 audio session 1\n"
    "track 17 lba 299550 sectors 18000 audio session 1\n"
    "track 18 lba 317550 sectors 17350 audio session 1\n"
    "track 19 lba 334900 sectors 12275 audio session 1\n"
    "session 1 tracks 1-19 audio 19 data 0 leadout 347175\n"
    "leadout lba 347175\n"
    "source=fulltoc degrade=none subq_indices=none sessions=1..1 "
    "disc_type=0x00 session_count=1\n";

/* The `pregap` token, which the fixture above cannot reach. These are the real
 * numbers the physical ABBA pressing emits for track 1 — the line that made
 * `pregap 33 ... pregaps=none` read as a contradiction and forced the
 * subq_indices rename on 2026-07-25. Both tokens appear here together, on
 * purpose: they answer different questions and this asserts they can coexist. */
static const char want_pregap[] =
    "track 1 lba 33 sectors 17362 audio session 1 pregap 33\n"
    "leadout lba 347208\n"
    "source=fulltoc degrade=none subq_indices=none sessions=1..1 "
    "disc_type=0x00 session_count=1\n";

/* A format-0 degrade: no session structure, so no `sessions=`/`disc_type=`
 * and no session lines. session_count survives because READ DISC INFORMATION
 * is a separate opcode — that asymmetry is the point of the field. */
static const char want_degraded[] =
    "track 1 lba 0 sectors 1000 audio\n"
    "leadout lba 1000\n"
    "source=toc degrade=leadin_unreadable subq_indices=none session_count=1\n";

/* Anomalies: comma-separated slugs, and the trust flag when the geometry bits
 * are implicated. Unreachable without a malformed/copy-protected lead-in. */
static const char want_anomalous[] =
    "track 1 lba 0 sectors 1000 audio\n"
    "leadout lba 1000\n"
    "source=fulltoc degrade=none subq_indices=none sessions=1..1 "
    "disc_type=0x00 session_count=1 anomalies=overlap,empty_track "
    "toc_trusted=0\n";

int main(int argc, char **argv)
{
    int dump = argc > 1 && !strcmp(argv[1], "--dump");
    accudisc_fulltoc ft;
    accudisc_toc toc;
    accudisc_toc_info info;

    /* 1. The real captured lead-in, whole chain. */
    memset(&info, 0, sizeof(info));
    assert(accudisc_fulltoc_parse(vec_fulltoc, sizeof(vec_fulltoc), &ft)
           == ACCUDISC_OK);
    assert(adsc_toc_from_fulltoc(&ft, &toc, &info) == ACCUDISC_OK);
    info.source = ACCUDISC_TOC_SRC_FULLTOC;
    info.degrade = ACCUDISC_TOC_DEGRADE_NONE;
    info.session_count = 1;
    expect("fulltoc", render(&toc, &info), want_fulltoc, dump);

    /* 2. The pregap token, as the physical pressing emits it. */
    memset(&toc, 0, sizeof(toc));
    toc.track_count = 1;
    toc.tracks[0].number = 1;
    toc.tracks[0].adr_ctrl = 0x10;
    toc.tracks[0].session = 1;
    toc.tracks[0].lba = 33;
    toc.tracks[0].sectors = 17362;
    toc.tracks[0].pregap = 33;
    toc.leadout_lba = 347208;
    expect("pregap", render(&toc, &info), want_pregap, dump);

    /* 3. Degraded acquisition — the format-0 fallback path. */
    memset(&toc, 0, sizeof(toc));
    memset(&info, 0, sizeof(info));
    toc.track_count = 1;
    toc.tracks[0].number = 1;
    toc.tracks[0].lba = 0;
    toc.tracks[0].sectors = 1000;
    toc.tracks[0].adr_ctrl = 0x10; /* ADR 1, CTRL 0 -> audio */
    toc.leadout_lba = 1000;
    info.source = ACCUDISC_TOC_SRC_TOC;
    info.degrade = ACCUDISC_TOC_DEGRADE_LEADIN_UNREADABLE;
    info.session_count = 1;
    expect("degraded", render(&toc, &info), want_degraded, dump);

    /* 4. Anomalous lead-in, including an UNTRUSTED_GEOMETRY bit. */
    info.source = ACCUDISC_TOC_SRC_FULLTOC;
    info.degrade = ACCUDISC_TOC_DEGRADE_NONE;
    info.first_session = 1;
    info.last_session = 1;
    info.disc_type = 0x00;
    toc.anomalies = ACCUDISC_TOC_ANOM_OVERLAP | ACCUDISC_TOC_ANOM_EMPTY_TRACK;
    assert(toc.anomalies & ACCUDISC_TOC_ANOM_UNTRUSTED_GEOMETRY);
    expect("anomalous", render(&toc, &info), want_anomalous, dump);

    if (!dump)
        printf("test_cli_toc_format: 4 cases OK\n");
    return 0;
}
