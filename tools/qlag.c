/* qlag — Q-subchannel positional lag measurement, device-free.
 *
 * Asks one question of a saved raw-subchannel capture: does the Q frame
 * arriving in transfer slot i describe sector base_lba + i, or some other
 * sector? For every CRC-good ADR=1 frame it computes
 *
 *     delta = (the frame's OWN absolute LBA) - (base_lba + slot index)
 *
 * and reports the distribution. A single delta of 0 across the capture means
 * the drive delivers Q in step with the audio, and an LBA-indexed subchannel
 * map is correct. A single nonzero delta is a constant lag: the map is right
 * everywhere once shifted. A spread is jitter, and a map cannot be trusted
 * without knowing why.
 *
 * WHY THIS MATTERS EVEN THOUGH Q IS SELF-LOCATING. A CRC-good position frame
 * carries its own address, so lag is invisible if you index by it. But
 * accudisc_q_parse leaves every position field zero on CRC failure (deliberate:
 * a bad frame decodes to out-of-range BCD), so a CRC-BAD frame can only be
 * placed by transfer slot — and CRC-bad frames are exactly what a subchannel
 * health map exists to show. Lag is irrelevant for the frames you can locate
 * and decisive for the frames you cannot.
 *
 * Public header only: this builds against an installed libaccudisc.
 *
 *     gcc -O2 -o build/qlag tools/qlag.c -laccudisc
 *     ./build/qlag capture.sub [BASE_LBA | --toc capture.fulltoc]
 *
 * BASE_LBA defaults to 0. --toc reads a full-TOC blob (accudisc_read_full_toc
 * format) and, if the capture length equals the lead-out LBA, confirms the
 * capture starts at 0 — which removes the "is my base right?" confound from
 * the answer. It is a cross-check, not a source: a capture that does not match
 * still measures, it just says so.
 *
 * Nothing here touches a drive.
 */
#include <accudisc/accudisc.h>

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Two different numbers, and confusing them misreads the output. SPREAD_MAX is
 * the VERDICT threshold: more distinct deltas than this and no single one is
 * the lag. MAX_DELTAS is only the histogram's capacity — reaching it also
 * forces SPREAD, but the verdict has already fired long before, so it is a
 * safety net rather than a threshold. */
#define SPREAD_MAX 8   /* > this many distinct deltas = SPREAD */
#define MAX_DELTAS 64  /* histogram capacity; overflow also forces SPREAD */

struct bucket {
    long delta;
    uint64_t count;
};

static int load(const char *path, uint8_t **buf, long *len)
{
    FILE *f = fopen(path, "rb");
    if (!f)
        return -1;
    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        return -1;
    }
    *len = ftell(f);
    rewind(f);
    *buf = malloc((size_t)*len);
    if (!*buf || fread(*buf, 1, (size_t)*len, f) != (size_t)*len) {
        free(*buf);
        fclose(f);
        return -1;
    }
    fclose(f);
    return 0;
}

/* Cross-check the capture's base against a full TOC. Returns the lead-out LBA,
 * or -1 if the blob did not parse. */
static int32_t leadout_of(const char *path)
{
    uint8_t *blob;
    long len;

    if (load(path, &blob, &len) != 0) {
        fprintf(stderr, "qlag: cannot read %s\n", path);
        return -1;
    }

    accudisc_fulltoc ft;
    int32_t out = -1;
    if (accudisc_fulltoc_parse(blob, (uint32_t)len, &ft) == ACCUDISC_OK) {
        for (uint32_t i = 0; i < ft.entry_count; i++) {
            const accudisc_fulltoc_entry *e = &ft.entries[i];
            if (e->point == 0xA2)
                out = accudisc_msf_to_lba(e->pmin, e->psec, e->pframe);
        }
    }
    free(blob);
    return out;
}

int main(int argc, char **argv)
{
    const char *subpath = NULL, *tocpath = NULL;
    long base = 0;

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--toc") && i + 1 < argc)
            tocpath = argv[++i];
        else if (!subpath)
            subpath = argv[i];
        else
            base = strtol(argv[i], NULL, 10);
    }
    if (!subpath) {
        fprintf(stderr,
                "usage: qlag CAPTURE.sub [BASE_LBA] [--toc FULLTOC.bin]\n");
        return 2;
    }

    uint8_t *raw;
    long sz;
    if (load(subpath, &raw, &sz) != 0) {
        fprintf(stderr, "qlag: cannot read %s\n", subpath);
        return 1;
    }
    if (sz % ACCUDISC_BYTES_SUB_RAW) {
        fprintf(stderr, "qlag: %s is %ld bytes, not a multiple of %d — this "
                        "wants RAW P-W (96 B/sector), not formatted Q\n",
                subpath, sz, ACCUDISC_BYTES_SUB_RAW);
        free(raw);
        return 1;
    }
    uint32_t nsec = (uint32_t)(sz / ACCUDISC_BYTES_SUB_RAW);

    if (tocpath) {
        int32_t leadout = leadout_of(tocpath);
        if (leadout < 0)
            printf("TOC: did not parse — base %ld taken as given\n", base);
        else if (base == 0 && (uint32_t)leadout == nsec)
            printf("TOC: lead-out %d == capture length — base 0 confirmed\n",
                   leadout);
        else
            printf("TOC: lead-out %d vs %u sectors from base %ld — capture is "
                   "NOT the whole disc; the base is an assumption\n",
                   leadout, nsec, base);
    }

    struct bucket b[MAX_DELTAS];
    int nb = 0, overflow = 0;
    uint64_t good = 0, bad = 0, nonpos = 0;

    for (uint32_t i = 0; i < nsec; i++) {
        uint8_t q[12];
        accudisc_q qd;

        accudisc_sub_extract_q(raw + (size_t)i * ACCUDISC_BYTES_SUB_RAW, q);
        accudisc_q_parse(q, &qd);

        if (!qd.crc_ok) {
            bad++;
            continue; /* no address: this frame CANNOT be placed by itself */
        }
        good++;
        if (qd.adr != ACCUDISC_Q_POSITION) {
            nonpos++; /* MCN / ISRC: legitimately interleaved, not damage */
            continue;
        }

        long own = accudisc_msf_to_lba(qd.abs_m, qd.abs_s, qd.abs_f);
        long delta = own - (base + (long)i);

        int j = 0;
        for (; j < nb; j++)
            if (b[j].delta == delta) {
                b[j].count++;
                break;
            }
        if (j == nb) {
            if (nb < MAX_DELTAS) {
                b[nb].delta = delta;
                b[nb].count = 1;
                nb++;
            } else {
                overflow++;
            }
        }
    }

    uint64_t pos = good - nonpos;
    printf("\n%s\n", subpath);
    printf("  sectors            %u  (base LBA %ld)\n", nsec, base);
    printf("  CRC-good           %llu  (%.2f%%)\n", (unsigned long long)good,
           nsec ? 100.0 * (double)good / nsec : 0.0);
    printf("  CRC-bad            %llu  (%.2f%%)  <- placeable only by slot\n",
           (unsigned long long)bad, nsec ? 100.0 * (double)bad / nsec : 0.0);
    /* TWO denominators, deliberately, because one of them misleads on its own.
     * "of all" is what a map cares about: what fraction of cells will be
     * NO_POSITION. "of CRC-good" is what the DISC is doing: the MCN/ISRC
     * interleave is a property of the pressing's subchannel stream, not of read
     * quality. Print only the first and a reader comparing captures across
     * speeds sees it halve when Q yield halves and concludes the interleave
     * thinned under load — cdda2img nearly did (their §150.4a; normalised, it
     * is flat to three digits across a 2x change in yield). */
    printf("  non-position       %llu  (%.2f%% of all, %.3f%% of CRC-good)"
           "  <- MCN/ISRC, healthy\n",
           (unsigned long long)nonpos,
           nsec ? 100.0 * (double)nonpos / nsec : 0.0,
           good ? 100.0 * (double)nonpos / (double)good : 0.0);
    printf("  position frames    %llu\n", (unsigned long long)pos);

    /* Say it, do not leave it to be inferred. Deriving one percentage from the
     * other DOES NOT WORK — they are printed at different precisions, so the
     * quotient of the displayed values is wrong by an amount that grows as
     * CRC-good falls. Measured on cdda2img's sweep (their §153/§154, ours
     * 2026-08-07f): at 38.73% CRC-good, 0.39/38.73 gives 1.007% against a true
     * 0.995% — a systematic error, not a slip, and one that is invisible on a
     * healthy capture because it only bites when the denominator shrinks. It
     * happened to a careful reader immediately below a correct table. Widening
     * the precision would make the derivation look MORE valid; the two figures
     * are both printed so that nobody needs it. */
    if (nonpos)
        printf("    (both percentages are printed because neither derives from "
               "the other: the\n     quotient of these rounded values drifts as "
               "CRC-good falls, by 1.2%% of itself\n     at 39%% yield. Read the "
               "column you want.)\n");

    if (!pos) {
        printf("\n  VERDICT: no CRC-good position frames — nothing measurable "
               "here.\n");
        free(raw);
        return 3;
    }

    /* Sort buckets by count, descending: the dominant delta first. */
    for (int i = 0; i < nb; i++)
        for (int j = i + 1; j < nb; j++)
            if (b[j].count > b[i].count) {
                struct bucket t = b[i];
                b[i] = b[j];
                b[j] = t;
            }

    printf("\n  delta (frame's own LBA - slot), %d distinct value(s)%s:\n", nb,
           overflow ? " [TRUNCATED — too many to track: this is a spread]" : "");
    for (int i = 0; i < nb; i++)
        printf("    %+8ld : %10llu  (%6.3f%%)\n", b[i].delta,
               (unsigned long long)b[i].count,
               100.0 * (double)b[i].count / (double)pos);

    double dom = 100.0 * (double)b[0].count / (double)pos;
    int spread = overflow || nb > SPREAD_MAX;
    printf("\n  VERDICT: ");
    if (spread)
        printf("SPREAD — %d+ distinct deltas. Q position is not a fixed "
               "function of the slot on this capture; a slot-indexed map is "
               "unsafe until this is explained.\n",
               nb);
    else if (b[0].delta == 0)
        printf("NO LAG — %.3f%% of position frames land in their own slot. "
               "A slot-indexed subchannel map is correct on this drive.\n",
               dom);
    else
        printf("LAG %+ld frame(s) — %.3f%% of position frames sit %ld slot(s) "
               "from their own address. Shift a slot-indexed map by this.\n",
               b[0].delta, dom, b[0].delta);

    /* Per-run detail only when there IS a dominant delta to be an exception
     * to. Under SPREAD every frame is its own run, and printing them is tens
     * of thousands of lines saying nothing the histogram did not. */
    if (nb > 1 && !spread) {
        printf("\n  Minority deltas are NOT lag. A frame can pass CRC-16 and "
               "still be positionally wrong;\n  the runs below are where. "
               "(Measured here 2026-08-07: 43 frames of 157,914, in six\n  runs, "
               "every delta an exact multiple of 512 sectors — a buffer number, "
               "not a disc\n  number. No mechanism claimed.)\n");

        long prev_slot = -2, prev_delta = 0;
        uint32_t runlen = 0;
        long run_start = 0;
        for (uint32_t i = 0; i < nsec; i++) {
            uint8_t q[12];
            accudisc_q qd;
            accudisc_sub_extract_q(raw + (size_t)i * ACCUDISC_BYTES_SUB_RAW, q);
            accudisc_q_parse(q, &qd);
            if (!qd.crc_ok || qd.adr != ACCUDISC_Q_POSITION)
                continue;
            long d = accudisc_msf_to_lba(qd.abs_m, qd.abs_s, qd.abs_f) -
                     (base + (long)i);
            if (d == b[0].delta)
                continue;
            if ((long)i == prev_slot + 1 && d == prev_delta) {
                runlen++;
            } else {
                if (runlen)
                    printf("    slot %ld..%ld (%u frame%s) delta %+ld\n",
                           run_start, prev_slot, runlen, runlen == 1 ? "" : "s",
                           prev_delta);
                run_start = i;
                runlen = 1;
            }
            prev_slot = i;
            prev_delta = d;
        }
        if (runlen)
            printf("    slot %ld..%ld (%u frame%s) delta %+ld\n", run_start,
                   prev_slot, runlen, runlen == 1 ? "" : "s", prev_delta);
    }

    free(raw);
    return 0;
}
