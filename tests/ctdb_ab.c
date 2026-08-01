/* SPDX-License-Identifier: MIT */
/* Phase 3b A/B harness — decodes one (image, parity, npar, offset) against real
 * CTDB parity and prints the correction set as JSON, for diffing against the
 * output of cdda2img's pinned `ctanalyse-ab-baseline` binary.
 *
 * LICENCE HYGIENE: nothing here links, includes or invokes GPL code. The
 * reference outputs are already JSON files in the fixture, so the comparison is
 * between two files produced by two separate programs — interoperability
 * testing, not derivation.
 *
 * NOT the offset search and NOT the public API; both are Phase 4. The offset is
 * an INPUT here, which is what let us answer cdda2img's §144.2 (what the decoder
 * does when a wrong offset is imposed on it — their tool finds the offset
 * internally and cannot be asked).
 *
 * Syndromes accumulate in the ROW direction with Horner, so the image is read
 * once sequentially rather than once per column. Column c's codeword is
 * base + S + c + j*S (rs16-spec.md §3a), so a row is S CONTIGUOUS words; walked
 * this way the transpose is free, walked column-wise it is 11760 strided passes
 * over 383 MB.
 *
 * Driven by ctdb_ab.sh, which supplies the arms and skips when the fixtures are
 * absent. See private/docs/ctdb-wire-findings-2026-08-01.md.
 *
 * usage: ctdb_ab <pcm> <parity> <npar> <offset> <first_frame> <frames> [erasures.bin]
 */
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include "repair/gf16.h"
#include "repair/rs16.h"

#define SS 11760u /* internal stride, words */

int main(int argc, char **argv)
{
    const uint16_t *pcm;
    size_t file_words;
    uint16_t *par, *acc;
    const uint8_t *era = NULL;
    size_t par_u16;
    unsigned npar, sc;
    int fd, off;
    struct stat st;
    FILE *f;
    long delta;
    size_t base, W;

    if (argc < 7) {
        fprintf(stderr, "usage: %s <pcm> <parity> <npar> <offset> <first_frame>"
                        " <frames> [erasures.bin]\n", argv[0]);
        return 2;
    }
    npar = (unsigned)strtoul(argv[3], NULL, 10);
    off = (int)strtol(argv[4], NULL, 10);
    base = (size_t)strtoul(argv[5], NULL, 10) * 1176u;
    W = (size_t)strtoul(argv[6], NULL, 10) * 1176u;
    delta = (long)off * 2;

    fd = open(argv[1], O_RDONLY);
    if (fd < 0 || fstat(fd, &st)) { perror("pcm"); return 2; }
    pcm = mmap(NULL, (size_t)st.st_size, PROT_READ, MAP_SHARED, fd, 0);
    if (pcm == MAP_FAILED) { perror("mmap"); return 2; }
    file_words = (size_t)st.st_size / 2;

    f = fopen(argv[2], "rb");
    if (!f) { perror("parity"); return 2; }
    fseek(f, 0, SEEK_END); par_u16 = (size_t)ftell(f) / 2; rewind(f);
    par = malloc(par_u16 * 2);
    if (fread(par, 2, par_u16, f) != par_u16) { perror("parity read"); return 2; }
    fclose(f);

    if (argc > 7) {
        int efd = open(argv[7], O_RDONLY);
        struct stat es;

        if (efd < 0 || fstat(efd, &es)) { perror("erasures"); return 2; }
        era = mmap(NULL, (size_t)es.st_size, PROT_READ, MAP_SHARED, efd, 0);
        if (era == MAP_FAILED) { perror("erasure mmap"); return 2; }
    }

    adsc_gf16_init();
    sc = (unsigned)(W / SS) - 2;
    if (par_u16 / npar != SS) {
        fprintf(stderr, "parity has %zu columns, expected %u\n",
                par_u16 / npar, SS);
        return 2;
    }

    acc = calloc((size_t)SS * npar, sizeof(*acc));

    /* One sequential pass. Horner, oldest symbol first: S_r <- v ^ S_r*alpha^r. */
    for (unsigned j = 0; j < sc; j++) {
        long row = (long)base + (long)SS + (long)j * (long)SS + delta;

        if (row < 0 || (size_t)row + SS > file_words) {
            fprintf(stderr, "row %u out of range at offset %d\n", j, off);
            return 2;
        }
        for (unsigned c = 0; c < SS; c++) {
            uint16_t v = pcm[(size_t)row + c];
            uint16_t *a = &acc[(size_t)c * npar];

            a[0] ^= v; /* alpha^0 = 1 */
            for (unsigned r = 1; r < npar; r++)
                a[r] = (uint16_t)(adsc_gf16_mul_pow(a[r], r) ^ v);
        }
    }

    /* Decode every dirty column. */
    unsigned dirty = 0, decoded = 0, refused = 0, ncorr = 0, erasure_columns = 0;
    unsigned positions[64];
    uint16_t values[64];
    unsigned erasures[64];
    unsigned *sect = calloc(1u << 20, sizeof(unsigned));
    unsigned nsect = 0;

    printf("{\n  \"offset\": %d,\n  \"npar\": %u,\n  \"image_first_frame\": %zu,\n"
           "  \"image_frames\": %zu,\n  \"stridecount\": %u,\n  \"corrections\": [",
           off, npar, base / 1176u, W / 1176u, sc);

    for (unsigned c = 0; c < SS; c++) {
        uint16_t E[ADSC_RS16_MAX_NPAR];
        unsigned nera = 0;
        int clean = 1, rc;

        for (unsigned r = 0; r < npar; r++) {
            E[r] = (uint16_t)(acc[(size_t)c * npar + r] ^ par[(size_t)r * SS + c]);
            clean &= (E[r] == 0);
        }
        if (clean)
            continue;
        dirty++;

        if (era) {
            /* Stop at npar+1: the decoder refuses any count above npar, so a
             * truncated over-capacity list behaves identically to the true one
             * and the scan stays bounded. */
            for (unsigned p = 0; p < sc && nera <= npar; p++) {
                size_t w = (size_t)((long)base + (long)SS + (long)c
                                    + (long)p * (long)SS + delta);
                if (era[w >> 3] & (uint8_t)(1u << (w & 7u)))
                    erasures[nera++] = p;
            }
        }

        rc = adsc_rs16_decode(npar, E, sc, nera ? erasures : NULL, nera,
                              positions, values, 64);

        /* cdda2img's cdrepair.c:262-266 tries erasures first and falls back to
         * error-only WITHOUT counting the column, which is why their
         * erasure_columns means "erasures were used AND helped" rather than
         * "this column had erasures". Reproduced here so the A/B compares the
         * same quantity; §144.5 records that we consider the semantics
         * lossy and will define ours differently in the public API. */
        if (nera && rc > 0) {
            erasure_columns++;
        } else if (nera && rc <= 0) {
            rc = adsc_rs16_decode(npar, E, sc, NULL, 0, positions, values, 64);
        }
        if (rc <= 0) { refused++; continue; }
        decoded++;
        for (int i = 0; i < rc; i++) {
            size_t w = (size_t)((long)base + (long)SS + (long)c
                                + (long)positions[i] * (long)SS + delta);
            uint16_t old = pcm[w];

            printf("%s\n    {\"byte\": %zu, \"old\": %u, \"new\": %u}",
                   ncorr ? "," : "", w * 2, old, (unsigned)(old ^ values[i]));
            ncorr++;
            unsigned s = (unsigned)(w * 2 / 2352);
            unsigned seen = 0;
            for (unsigned k = 0; k < nsect; k++) if (sect[k] == s) { seen = 1; break; }
            if (!seen && nsect < (1u << 20)) sect[nsect++] = s;
        }
    }
    printf("\n  ],\n  \"corrected_errors\": %u,\n  \"dirty_columns\": %u,\n"
           "  \"decoded_columns\": %u,\n  \"refused_columns\": %u,\n"
           "  \"erasure_columns\": %u,\n  \"affected_sector_count\": %u\n}\n",
           ncorr, dirty, decoded, refused, erasure_columns, nsect);
    return 0;
}
