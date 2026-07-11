/* SPDX-License-Identifier: MIT */
/* C2/audio lag probe — the pure correlation core, separated from the
 * device I/O so it can be unit-tested on synthetic read pairs. */
#ifndef ACCUDISC_SRC_DRIVE_C2LAG_H
#define ACCUDISC_SRC_DRIVE_C2LAG_H

#include <accudisc/accudisc.h>

/* Candidate shifts: +-8 sample pairs (+-32 bytes) comfortably brackets
 * every lag observed in the wild (PX-716A: 2 pairs). */
#define ADSC_C2LAG_MAX_SHIFT 8
#define ADSC_C2LAG_NSHIFT    (2 * ADSC_C2LAG_MAX_SHIFT + 1)

/* Evidence floors below which no verdict is honest. */
#define ADSC_C2LAG_MIN_FLAGS 64
#define ADSC_C2LAG_MIN_DIFFS 64
#define ADSC_C2LAG_MIN_TP    32
/* The reread diff is a PROXY oracle: wrong bytes that fail identically in
 * both paired reads are invisible to it, so absolute agreement runs well
 * below a true oracle's (measured: 249 permil where the oracle gave 993 on
 * the same drive/defect). Conclusiveness is therefore CONTRAST — the peak
 * must dominate every other shift — plus a modest absolute floor. */
#define ADSC_C2LAG_MIN_PEAK_MILLI 100
#define ADSC_C2LAG_MIN_CONTRAST   3 /* peak >= 3x runner-up */

struct adsc_c2lag_acc {
    uint64_t tp[ADSC_C2LAG_NSHIFT];    /* flags landing on unstable bytes */
    uint64_t flags[ADSC_C2LAG_NSHIFT]; /* flags with an in-range target */
    uint64_t diff_bytes;               /* unstable-byte observations */
};

/* Accumulate one sector read twice (independent, cache-defeated reads):
 * each read's fired flags are scored against the pair's audio diff at
 * every candidate shift. audio_* are ACCUDISC_BYTES_AUDIO bytes; c2_* are
 * ACCUDISC_BYTES_C2 bytes, one bit per audio byte, MSB-first. */
void adsc_c2lag_add(struct adsc_c2lag_acc *acc,
                    const uint8_t *audio_a, const uint8_t *c2_a,
                    const uint8_t *audio_b, const uint8_t *c2_b);

/* ACCUDISC_OK with out filled, or ACCUDISC_ERR_NOTFOUND when the evidence
 * is below the floors or no shift reaches the peak threshold. */
int adsc_c2lag_result(const struct adsc_c2lag_acc *acc, accudisc_c2_lag *out);

#endif
