/* SPDX-License-Identifier: MIT */
/* CTDB parity repair — the grid, the CRC, and the public entry point.
 *
 * The Reed-Solomon arithmetic is in rs16.c; this file is everything around it:
 * where a column's symbols live, how the two domains relate, and what a caller
 * is told.
 *
 * THE GRID (verified against real CTDB parity, 2026-08-02, four arms; see
 * private/docs/rs16-spec.md §3a). With base = image_first_frame * 1176 words,
 * W = image_frames * 1176 words, and internal stride S = 2 * wire_stride:
 *
 *     stridecount = W / S - 2
 *     column c is the words  base + S + c + j*S,  j = 0 .. stridecount-1
 *
 * The two exclusions are the whole subtlety and neither is guessable. Row 0 is
 * NOT a codeword symbol — the window starts at S + c, not c — and the final
 * partial row is not one either, which is what the -2 buys. Getting this wrong
 * is close to undetectable: a wrong window reproduces `dirty_columns` exactly
 * (11760 against 11760, measured) while changing every correction, and the
 * r = 0 syndrome, being an XOR, matches under a window wrong by a whole row.
 * So do not "validate" this against S_0.
 *
 * Syndromes accumulate in the ROW direction with Horner, because a row is S
 * contiguous words: one sequential pass over the image instead of S strided
 * passes over it. On a 383 MB rip that is seconds against tens of minutes.
 */
#include <stdlib.h>
#include <string.h>

#include <accudisc/accudisc.h>

#include "gf16.h"
#include "rs16.h"

#define WORDS_PER_FRAME 1176u /* 2352 bytes / 2 */

/* CRC-32 (the reflected, 0xEDB88320 one; init and final xor 0xFFFFFFFF).
 * Computed over the codeword region only, NOT the whole image and NOT the
 * image window — measured against cdda2img's published crc_before/crc_after.
 *
 * It is a change detector, not a gate. CTDB publishes per-track CRCs, so there
 * is nothing here to compare this against; the caller's absolute check is a
 * different quantity computed over different bytes. */
static uint32_t crc32_words(const uint16_t *v, uint64_t n)
{
    static uint32_t table[256];
    static int built;
    uint32_t crc = 0xFFFFFFFFu;
    const uint8_t *p = (const uint8_t *)v;
    uint64_t nbytes = n * 2u;

    if (!built) {
        for (unsigned i = 0; i < 256; i++) {
            uint32_t c = i;

            for (unsigned k = 0; k < 8; k++)
                c = (c & 1u) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
            table[i] = c;
        }
        built = 1; /* idempotent and value-identical if raced; see gf16_init */
    }
    for (uint64_t i = 0; i < nbytes; i++)
        crc = table[(crc ^ p[i]) & 0xFFu] ^ (crc >> 8);
    return crc ^ 0xFFFFFFFFu;
}

/* One correction, held until every column has decoded. Nothing is written to
 * the caller's buffer before that, so an aliased out_pcm cannot be left half
 * repaired by a failure. */
struct fix {
    uint64_t word; /* index into the PCM, not the image */
    uint16_t value;
};

int accudisc_ctdb_repair(const accudisc_ctdb_req *req, uint8_t *out_pcm,
                         accudisc_ctdb_report *report)
{
    const uint16_t *pcm;
    const uint16_t *par;
    uint16_t *acc = NULL;
    struct fix *fixes = NULL;
    uint64_t base, W, pcm_words, nfix = 0, fix_cap;
    unsigned S, sc, npar;
    long delta;
    int rc = ACCUDISC_ERR_INVAL;
    uint32_t dirty = 0, repaired = 0, refused = 0, erasure_cols = 0;

    if (!req || !out_pcm)
        return ACCUDISC_ERR_INVAL;
    if (req->size != sizeof(*req))
        return ACCUDISC_ERR_ABI;
    if (report && report->size != sizeof(*report))
        return ACCUDISC_ERR_ABI;
    if (!req->pcm || !req->parity)
        return ACCUDISC_ERR_INVAL;
    if (req->npar == 0 || req->npar > ADSC_RS16_MAX_NPAR)
        return ACCUDISC_ERR_INVAL;
    if (req->wire_stride == 0 || req->wire_stride > 0x40000000u)
        return ACCUDISC_ERR_INVAL;
    if (req->image_frames == 0)
        return ACCUDISC_ERR_INVAL;

    npar = req->npar;
    S = req->wire_stride * 2u;
    base = (uint64_t)req->image_first_frame * WORDS_PER_FRAME;
    W = (uint64_t)req->image_frames * WORDS_PER_FRAME;
    pcm_words = req->pcm_bytes / 2u;
    delta = (long)req->offset_pairs * 2L;

    /* The blob must describe exactly S columns of npar symbols. This is the
     * one cheap cross-check that the entry and the geometry belong together —
     * a mismatched wire_stride fails here rather than decoding garbage. */
    if (req->parity_bytes != (uint64_t)S * npar * 2u)
        return ACCUDISC_ERR_INVAL;
    if (W / S < 3u)
        return ACCUDISC_ERR_INVAL; /* too short for a codeword at all */
    sc = (unsigned)(W / S) - 2u;

    if (base + W > pcm_words)
        return ACCUDISC_ERR_INVAL; /* image window is not inside the PCM */

    /* Every word the codeword region touches, at this offset, must exist. */
    {
        long first = (long)base + (long)S + delta;
        long last = first + (long)sc * (long)S + (long)S - 1;

        if (first < 0 || last < 0 || (uint64_t)last >= pcm_words)
            return ACCUDISC_ERR_INVAL;
    }
    if (req->pcm_erasures
        && req->pcm_erasures_bytes * 8u < pcm_words)
        return ACCUDISC_ERR_INVAL; /* bitmap does not cover the PCM */

    pcm = (const uint16_t *)(const void *)req->pcm;
    par = (const uint16_t *)(const void *)req->parity;

    acc = calloc((size_t)S * npar, sizeof(*acc));
    /* An all-erasure column returns npar errata, so npar per column is the
     * true worst case rather than npar/2. */
    fix_cap = (uint64_t)S * npar;
    fixes = calloc((size_t)fix_cap, sizeof(*fixes));
    if (!acc || !fixes) { rc = ACCUDISC_ERR_NOMEM; goto out; }

    adsc_gf16_init();

    /* Pass 1: syndromes, one sequential sweep. S_r <- v ^ S_r*alpha^r. */
    for (unsigned j = 0; j < sc; j++) {
        uint64_t row = (uint64_t)((long)base + (long)S + (long)j * (long)S + delta);

        for (unsigned c = 0; c < S; c++) {
            uint16_t v = pcm[row + c];
            uint16_t *a = &acc[(size_t)c * npar];

            a[0] ^= v; /* alpha^0 = 1 */
            for (unsigned r = 1; r < npar; r++)
                a[r] = (uint16_t)(adsc_gf16_mul_pow(a[r], r) ^ v);
        }
    }

    /* Pass 2: decode each column that disagrees. */
    for (unsigned c = 0; c < S; c++) {
        uint16_t E[ADSC_RS16_MAX_NPAR];
        unsigned erasures[ADSC_RS16_MAX_NPAR + 1];
        unsigned positions[ADSC_RS16_MAX_NPAR + 1];
        uint16_t values[ADSC_RS16_MAX_NPAR + 1];
        unsigned nera = 0;
        int clean = 1, n;

        for (unsigned r = 0; r < npar; r++) {
            E[r] = (uint16_t)(acc[(size_t)c * npar + r] ^ par[(size_t)r * S + c]);
            clean &= (E[r] == 0);
        }
        if (clean)
            continue;
        dirty++;

        if (req->pcm_erasures) {
            /* Stop one past npar: the decoder refuses any larger count, so a
             * truncated over-capacity list behaves identically to the true one
             * and the scan cannot run away on a badly aligned bitmap. */
            for (unsigned p = 0; p < sc && nera <= npar; p++) {
                uint64_t w = (uint64_t)((long)base + (long)S + (long)c
                                        + (long)p * (long)S + delta);

                if (req->pcm_erasures[w >> 3] & (uint8_t)(1u << (w & 7u)))
                    erasures[nera++] = p;
            }
            if (nera)
                erasure_cols++;
        }

        n = adsc_rs16_decode(npar, E, sc, nera ? erasures : NULL, nera,
                             positions, values, ADSC_RS16_MAX_NPAR + 1);
        /* Erasures are a hint, not a constraint. A bitmap that is wrong — C2
         * over-flagging, or a misaligned capture — can push a correctable
         * column over capacity, and error-only may still succeed on it. Retry
         * rather than lose audio to a bad hint. */
        if (n <= 0 && nera)
            n = adsc_rs16_decode(npar, E, sc, NULL, 0, positions, values,
                                 ADSC_RS16_MAX_NPAR + 1);
        if (n <= 0) {
            refused++;
            continue; /* keep counting: the report should describe the disc */
        }
        repaired++;
        for (int i = 0; i < n; i++) {
            uint64_t w = (uint64_t)((long)base + (long)S + (long)c
                                    + (long)positions[i] * (long)S + delta);

            if (nfix >= fix_cap) { rc = ACCUDISC_ERR_NOMEM; goto out; }
            fixes[nfix].word = w;
            fixes[nfix].value = (uint16_t)(pcm[w] ^ values[i]);
            nfix++;
        }
    }

    {
        uint64_t first = (uint64_t)((long)base + (long)S + delta);
        uint32_t before = crc32_words(pcm + first, (uint64_t)sc * S);
        uint32_t after = before;

        if (refused) {
            /* Nothing is applied. Reporting crc32_after == crc32_before here
             * is the truthful statement about out_pcm, which was not written. */
            rc = ACCUDISC_ERR_NOTFOUND;
        } else {
            uint16_t *dst = (uint16_t *)(void *)out_pcm;

            if (out_pcm != req->pcm)
                memcpy(out_pcm, req->pcm, (size_t)req->pcm_bytes);
            for (uint64_t i = 0; i < nfix; i++)
                dst[fixes[i].word] = fixes[i].value;
            after = crc32_words(dst + first, (uint64_t)sc * S);
            rc = ACCUDISC_OK;
        }
        if (report) {
            report->offset_pairs = req->offset_pairs;
            report->dirty_columns = dirty;
            report->repaired_columns = repaired;
            report->refused_columns = refused;
            report->erasure_columns = erasure_cols;
            report->corrections = (uint32_t)(refused ? 0 : nfix);
            report->crc32_before = before;
            report->crc32_after = after;
        }
    }

out:
    free(acc);
    free(fixes);
    return rc;
}
