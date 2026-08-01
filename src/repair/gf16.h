/* SPDX-License-Identifier: MIT */
/* GF(2^16) — the field the CTDB parity code is defined over. One field
 * symbol per 16-bit audio sample.
 *
 *   field polynomial  0x1100B  (x^16 + x^12 + x^3 + x + 1)
 *   generator         alpha = 2
 *
 * Neither constant is taken on trust: both are proved by construction in
 * the self-test. If 0x1100B were not primitive, or 2 not a generator, the
 * sequence alpha^0, alpha^1, ... would return to 1 in fewer than 65535
 * steps, so asserting that the antilog cycle visits every non-zero element
 * exactly once settles both at once.
 *
 * Layout follows src/cdda/rw.c's GF(2^6): a doubled antilog table so a sum
 * of two logarithms indexes it without a modulo.
 */
#ifndef ACCUDISC_SRC_REPAIR_GF16_H
#define ACCUDISC_SRC_REPAIR_GF16_H

#include <stdint.h>

#define ADSC_GF16_ORDER 65535u /* 2^16 - 1: alpha^65535 == 1 */

/* adsc_gf16_log() returns this for 0, whose logarithm does not exist. It is
 * negative on purpose: a table that answered with an in-range index would
 * multiply to a well-formed wrong answer, which is the way a log/antilog
 * field goes wrong silently. */
#define ADSC_GF16_LOG_UNDEFINED (-1)

/* Build the tables. Idempotent and cheap after the first call; every entry
 * point below calls it, so there is no init step for callers to forget and
 * no per-column state to reinitialise when npar changes. Not thread-safe on
 * first call — same contract as accudisc_rw_open()'s gf_init(). */
void adsc_gf16_init(void);

uint16_t adsc_gf16_mul(uint16_t a, uint16_t b);

/* b == 0 is undefined; both return 0 rather than reading out of range, and
 * callers are expected to have excluded it. */
uint16_t adsc_gf16_div(uint16_t a, uint16_t b);
uint16_t adsc_gf16_inv(uint16_t a);

/* alpha^e for any non-negative e. */
uint16_t adsc_gf16_pow(unsigned e);

/* a * alpha^e — the multiply for the common case where one operand's
 * logarithm is already in hand. e is reduced internally. */
uint16_t adsc_gf16_mul_pow(uint16_t a, unsigned e);

/* Discrete logarithm base alpha, or ADSC_GF16_LOG_UNDEFINED for a == 0. */
int adsc_gf16_log(uint16_t a);

#endif /* ACCUDISC_SRC_REPAIR_GF16_H */
