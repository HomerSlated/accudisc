/* SPDX-License-Identifier: MIT */
/* Reed-Solomon syndrome decoding over GF(2^16), for CTDB parity repair.
 *
 * DECODE ONLY. Nothing here encodes: the parity is fetched from CTDB and is
 * never generated locally, so there is no generator polynomial and no
 * systematic encoder.
 *
 * The quantity decoded is the ERROR SYNDROME
 *
 *     E = (syndromes of our audio) XOR (syndromes CTDB published)
 *
 * An all-zero E means the column is clean. `npar` is a runtime parameter,
 * never a compile-time constant: 8 and 16 both occur, on the same disc.
 */
#ifndef ACCUDISC_SRC_REPAIR_RS16_H
#define ACCUDISC_SRC_REPAIR_RS16_H

#include <stdint.h>

#include <accudisc/accudisc.h>

/* Headroom over the parity counts seen in the wild (8 and 16), chosen as
 * twice the larger. It is a bound on what this code will accept, not a
 * measurement of what CTDB emits. */
#define ADSC_RS16_MAX_NPAR 32

/* Syndromes of a word, oldest symbol first:
 *
 *     S_r = sum_j v[j] * alpha^(r * (n - 1 - j)),   r = 0 .. npar-1
 *
 * i.e. consecutive roots starting at alpha^0, with symbol positions counted
 * from the END of the word. Provided so that the caller forms E under
 * exactly the convention adsc_rs16_decode() inverts — two conventions that
 * differ by an exponent offset both produce well-formed syndromes and only
 * one of them decodes.
 *
 * ACCUDISC_OK, or ACCUDISC_ERR_INVAL on a bad argument.
 */
int adsc_rs16_syndromes(const uint16_t *v, unsigned n, unsigned npar,
                        uint16_t *syn);

/* Decode one column.
 *
 *   npar        parity symbol count, 1 .. ADSC_RS16_MAX_NPAR
 *   err_syn     E[0 .. npar-1], the error syndromes
 *   n_data      data symbols in the column, 1 .. ADSC_GF16_ORDER
 *   erasures    data indices known to be damaged (e.g. from C2 pointers),
 *               0-based, may be NULL when nerasures == 0. Must be in range
 *               and free of duplicates; a repeated index gives the errata
 *               locator a repeated root and makes Forney's denominator
 *               vanish, so it is rejected rather than tolerated.
 *   positions   out: data indices of the errata, 0-based, ascending
 *   values      out: XOR masks to apply at those positions
 *   out_cap     capacity of positions[] and values[], in symbols
 *
 * Returns the number of errata written (> 0), 0 if the column was already
 * clean (all syndromes zero), or a negative accudisc_err:
 *
 *   ACCUDISC_ERR_INVAL     a bad argument — out of range, duplicated
 *                          erasure, or out_cap too small for the answer
 *   ACCUDISC_ERR_NOTFOUND  the syndromes do not decode: beyond capacity, or
 *                          a locator whose roots do not account for its
 *                          degree, or errata that fail re-verification
 *
 * BUFFER CONTRACT — a real hazard. With no erasures at most npar/2 errata
 * can be returned. WITH erasures, up to npar can: an all-erasure column
 * returns npar. Sizing positions[]/values[] for npar/2 in both cases
 * overflows on the erasure path, so size for npar. out_cap is checked
 * before anything is written, and a short buffer is ACCUDISC_ERR_INVAL
 * rather than a silent truncation.
 *
 * POSITION BASIS. An erratum at data index j is taken to contribute
 * e * alpha^(r * (n_data - 1 - j)) to E[r] — that is, positions are counted
 * from the end of the DATA word, the same `n` the caller passed to
 * adsc_rs16_syndromes(). Passing the full codeword length to one and the
 * data length to the other shifts every returned position by npar while
 * leaving all of them in range and every syndrome well-formed. The two
 * spans must be the same quantity.
 *
 * Correction is possible when e + 2t <= npar, for e erasures and t errors
 * at unknown positions; each erasure costs half what an error costs.
 * Beyond that the decoder DECLINES. Declining is a normal outcome: a
 * syndrome set can be consistent with a low-weight error pattern that is
 * not the one that actually occurred, so a decoder that always produced an
 * answer would sometimes produce a confidently wrong one and corrupt audio
 * that was merely damaged.
 *
 * Positions flagged as erasures that turn out to be undamaged are dropped
 * from the result rather than returned with a zero mask, so the return
 * value counts real corrections. C2 pointers over-flag, so this is the
 * ordinary case, not the exotic one.
 */
int adsc_rs16_decode(unsigned npar, const uint16_t *err_syn, unsigned n_data,
                     const unsigned *erasures, unsigned nerasures,
                     unsigned *positions, uint16_t *values, unsigned out_cap);

#endif /* ACCUDISC_SRC_REPAIR_RS16_H */
