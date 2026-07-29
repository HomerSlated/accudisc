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

#include <stdio.h>
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

/* Parse a FILE *start offset* -> BYTES; -1 on error, advances *pp.
 *
 * cdrdao writes this field in two units and the grammar does not mark which:
 *
 *     start_offset : byte offset, or MM:SS:FF into the file
 *
 * so "0" is zero BYTES and "00:00:00" is zero FRAMES, and the two agree only
 * at zero. Everywhere else a bare count read as frames is off by 2352x — on
 * the burn path, silently. That is why this returns bytes rather than frames
 * and does the conversion itself: the unit is decided where the syntax is
 * known, not by a caller looking at a number that no longer says which it is.
 *
 * Rejecting the bare form outright would also have been defensible, but it is
 * the form cdrdao itself emits and the one our own annotated reference shows
 * four times (cdda2img docs/reference/reference.toc:362).
 */
static long long parse_file_start(const char **pp)
{
    const char *p = skipws(*pp);
    char *e;
    long long n = strtoll(p, &e, 10);
    if (e == p || n < 0)
        return -1;
    if (*e != ':') {           /* bare count: already bytes */
        *pp = e;
        return n;
    }
    long frames = parse_msf(pp); /* re-parse from the top as MM:SS:FF */
    if (frames < 0)
        return -1;
    return (long long)frames * SECTOR_BYTES;
}

/* Record why the parse failed and return the code, so the caller can say WHICH
 * LINE rather than "invalid argument" for the whole file. Requested by cdda2img
 * (§117.2) after losing several minutes to a rejected FILE line — and we lost
 * the same minutes to the same line independently the same day, which is about
 * as clear a signal as a diagnostic ever gets. err may be NULL. */
static int fail(char *err, size_t errcap, int lineno, const char *what)
{
    if (err && errcap)
        snprintf(err, errcap, "line %d: %s", lineno, what);
    return ACCUDISC_ERR_INVAL;
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

int adsc_toc_parse_cue(const char *text, struct adsc_write_toc *out,
                       char *err, size_t errcap)
{
    if (err && errcap)
        err[0] = 0;
    if (!text || !out)
        return ACCUDISC_ERR_INVAL;
    memset(out, 0, sizeof(*out));

    int cur = -1;                 /* current track index */
    int have_file[99] = {0};
    int lineno = 0;

    for (const char *p = text; *p;) {
        lineno++;
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
            return fail(err, errcap, lineno,
                        "unterminated quote (a value may not span a line)");

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
                return fail(err, errcap, lineno, "more than 99 tracks");
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
                    return fail(err, errcap, lineno,
                                "FILE needs a \"quoted filename\"");
                long long fstart = parse_file_start(&q);
                if (fstart < 0)
                    return fail(err, errcap, lineno,
                                "FILE start offset must be a byte count or "
                                "MM:SS:FF");
                long flen = parse_msf(&q);
                if (flen < 0)
                    return fail(err, errcap, lineno,
                                "FILE needs an explicit MM:SS:FF length; "
                                "cdrdao's \"length omitted = to end of file\" "
                                "is not supported because resolving it needs "
                                "the BIN's size, which this parser is not "
                                "given");
                /* file_offset is already BYTES (parse_file_start decided the
                 * unit); the second pass no longer converts it. */
                t->file_offset = (uint64_t)fstart;
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
        return fail(err, errcap, lineno, "no TRACK found");

    /* Second pass: contiguous disc layout. start_lba = running sum of lengths;
     * index1 sits `pregap` in; the BIN offset is already bytes. */
    uint32_t cum = 0;
    for (int i = 0; i < out->ntracks; i++) {
        struct adsc_write_track *t = &out->track[i];
        if (!t->audio)
            return fail(err, errcap, 0, "track is not TRACK AUDIO "
                                        "(this is an audio-only writer)");
        if (!have_file[i])
            return fail(err, errcap, 0, "track has no FILE line");
        if (t->pregap > t->sectors)
            return fail(err, errcap, 0, "START pregap exceeds the FILE length");
        /* file_offset is already in bytes -- see parse_file_start. */
        t->index1_lba = cum + t->pregap;
        cum += t->sectors;
    }
    out->leadout_lba = cum;
    return ACCUDISC_OK;
}
