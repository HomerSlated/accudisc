/* cdrdao .toc parser -> DAO layout model (recording engine, phase 1 slice 3b).
 *
 * A line-oriented scan sufficient for audio CD-DA .toc files as produced by
 * the AccuDisc/cdda2img read path and cdrdao: CD_DA, CATALOG, per-track
 * TRACK AUDIO / NO?PRE_EMPHASIS / NO?COPY / ISRC / FILE start length / START.
 * CD_TEXT and other blocks are ignored (their lines don't match our keywords).
 *
 * FILE gives the BIN read offset + this track's total length; START gives the
 * pre-gap. Tracks tile the disc contiguously from LBA 0; a second pass fixes
 * up start_lba/index1_lba and the lead-out. See the semantics validated on the
 * ABBA "Gold" image (leadout = sum of FILE lengths).
 */

#include <stdlib.h>
#include <string.h>

#include "../mmc/mmc.h"
#include "write.h"

#define SECTOR_BYTES 2352u

static const char *skipws(const char *p)
{
    while (*p == ' ' || *p == '\t')
        p++;
    return p;
}

/* Keyword at start of a (whitespace-trimmed) line, delimited by ws/EOL. */
static int kw(const char *p, const char *k)
{
    size_t n = strlen(k);
    return strncmp(p, k, n) == 0 &&
           (p[n] == 0 || p[n] == ' ' || p[n] == '\t' || p[n] == '\r' ||
            p[n] == '\n');
}

/* Parse "MM:SS:FF" -> frames; -1 on error, advances *pp. */
static long parse_msf(const char **pp)
{
    const char *p = skipws(*pp);
    char *e;
    long mm = strtol(p, &e, 10);
    if (e == p || *e != ':')
        return -1;
    p = e + 1;
    long ss = strtol(p, &e, 10);
    if (e == p || *e != ':')
        return -1;
    p = e + 1;
    long ff = strtol(p, &e, 10);
    if (e == p)
        return -1;
    *pp = e;
    if (mm < 0 || ss < 0 || ss > 59 || ff < 0 || ff > 74)
        return -1;
    return (mm * 60 + ss) * 75 + ff;
}

/* Copy a "..."-quoted string to dst; returns ptr past the close quote, or
 * NULL. dst may be NULL to just skip (name we don't need). */
static const char *parse_qstr(const char *p, char *dst, size_t cap)
{
    p = skipws(p);
    if (*p != '"')
        return NULL;
    p++;
    size_t i = 0;
    while (*p && *p != '"' && *p != '\n') {
        if (dst && i + 1 < cap)
            dst[i++] = *p;
        p++;
    }
    if (*p != '"')          /* unterminated (hit EOL/EOF before the close) */
        return NULL;
    if (dst)
        dst[i] = 0;
    return p + 1;
}

int adsc_toc_parse_cue(const char *text, struct adsc_write_toc *out)
{
    if (!text || !out)
        return ACCUDISC_ERR_INVAL;
    memset(out, 0, sizeof(*out));

    int cur = -1;                 /* current track index */
    int have_file[99] = {0};

    for (const char *p = text; *p;) {
        const char *ls = skipws(p);
        const char *le = ls;
        int inq = 0;
        while (*le && *le != '\n') {
            if (*le == '"')
                inq = !inq;
            le++;
        }
        /* A string literal may not span a line (matches cdrdao's flex lexer).
         * An unterminated quote here means a value carries an embedded newline,
         * whose tail would otherwise be scanned as its own directive line —
         * the TOC-injection vector. Refuse rather than reinterpret. */
        if (inq)
            return ACCUDISC_ERR_INVAL;

        if (kw(ls, "CATALOG")) {
            char m[32];
            if (parse_qstr(ls + 7, m, sizeof m)) {
                size_t j = 0;
                for (size_t i = 0; m[i] && j < 13; i++)
                    if (m[i] >= '0' && m[i] <= '9')
                        out->mcn[j++] = m[i];
                out->mcn[j] = 0;
                if (j != 13)
                    out->mcn[0] = 0; /* not a 13-digit MCN: drop it */
            }
        } else if (kw(ls, "TRACK")) {
            if (out->ntracks >= 99)
                return ACCUDISC_ERR_INVAL;
            cur = out->ntracks++;
            struct adsc_write_track *t = &out->track[cur];
            memset(t, 0, sizeof(*t));
            t->audio = (strstr(ls, "AUDIO") != NULL &&
                        (size_t)(le - ls) < 64);
        } else if (cur >= 0) {
            struct adsc_write_track *t = &out->track[cur];
            if (kw(ls, "ISRC")) {
                char s[32];
                if (parse_qstr(ls + 4, s, sizeof s) && strlen(s) == 12)
                    memcpy(t->isrc, s, 13);
            } else if (kw(ls, "NO")) {
                if (strstr(ls, "PRE_EMPHASIS"))
                    t->preemphasis = 0;
                else if (strstr(ls, "COPY"))
                    t->copy = 0;
            } else if (kw(ls, "PRE_EMPHASIS")) {
                t->preemphasis = 1;
            } else if (kw(ls, "COPY")) {
                t->copy = 1;
            } else if (kw(ls, "FILE") || kw(ls, "AUDIOFILE")) {
                const char *q = parse_qstr(ls + (ls[0] == 'A' ? 9 : 4),
                                           NULL, 0);
                if (!q)
                    return ACCUDISC_ERR_INVAL;
                long fstart = parse_msf(&q);
                long flen = parse_msf(&q);
                if (fstart < 0 || flen < 0)
                    return ACCUDISC_ERR_INVAL;
                /* stash raw FILE fields; fixed up in the second pass */
                t->file_offset = (uint64_t)fstart; /* frames for now */
                t->sectors = (uint32_t)flen;
                have_file[cur] = 1;
            } else if (kw(ls, "START")) {
                const char *q = ls + 5;
                long g = parse_msf(&q);
                t->pregap = (g > 0) ? (uint32_t)g : 0;
            }
        }
        p = (*le == '\n') ? le + 1 : le;
    }

    if (out->ntracks < 1)
        return ACCUDISC_ERR_INVAL;

    /* Second pass: contiguous disc layout. start_lba = running sum of lengths;
     * index1 sits `pregap` in; BIN offset = FILE-start frames * 2352. */
    uint32_t cum = 0;
    for (int i = 0; i < out->ntracks; i++) {
        struct adsc_write_track *t = &out->track[i];
        if (!t->audio || !have_file[i])
            return ACCUDISC_ERR_INVAL; /* audio-only, every track needs a FILE */
        if (t->pregap > t->sectors)
            return ACCUDISC_ERR_INVAL;
        uint32_t fstart_frames = (uint32_t)t->file_offset;
        t->file_offset = (uint64_t)fstart_frames * SECTOR_BYTES;
        t->index1_lba = cum + t->pregap;
        cum += t->sectors;
    }
    out->leadout_lba = cum;
    return ACCUDISC_OK;
}
