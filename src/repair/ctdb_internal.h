/* SPDX-License-Identifier: MIT */
/* CTDB repair internals shared between the library and its A/B harness.
 *
 * NOT INSTALLED and not part of the public interface. It exists for one
 * reason worth stating, because the reason is a test-integrity problem rather
 * than a layering one.
 *
 * accudisc_ctdb_repair() deliberately refuses to hand back a correction list
 * without its verdict — that refusal is the whole safety argument of the
 * public API. But the A/B harness needs the correction list element-wise: an
 * injected position-basis defect returns the right NUMBER of corrections at
 * the wrong bytes, and an injected grid window reproduces dirty_columns
 * exactly (11760 against 11760, measured) while changing every correction. So
 * the harness could not call the public entry point, and carried its own copy
 * of the sweep instead — which meant the eight-arm parity test certified a
 * COPY of the shipping code and would have stayed green through any rewrite
 * of the real one.
 *
 * The constraint is worth keeping and the duplication is not, so the routing
 * moves rather than the constraint: both sides call the sweep below. The
 * harness keeps its own decode loop, because its erasure_columns semantics
 * deliberately reproduce the reference tool's rather than ours.
 */
#ifndef ACCUDISC_SRC_REPAIR_CTDB_INTERNAL_H
#define ACCUDISC_SRC_REPAIR_CTDB_INTERNAL_H

#include <stdint.h>

/* Syndrome accumulator layout: PLANE-MAJOR, acc[r * S + c].
 *
 * This is the layout, not an implementation detail, because both callers index
 * it directly. It matches the parity blob (par[r * S + c]) and it is what makes
 * the sweep vectorisable: with r on the outside, the multiplier alpha^r is
 * CONSTANT across a whole plane, so a plane is a flat 2*S-byte run under one
 * multiplier. The obvious alternative, acc[c * npar + r], gives each lane in a
 * vector a different multiplier and cannot be done with a constant-multiply
 * kernel at all. It also costs nothing in locality: a plane is 2*S bytes
 * (23 KB at the real stride) and the image row it consumes is another 23 KB,
 * both resident while the plane is swept.
 */

/* Pass 1. Accumulates syndromes for all S columns over rows
 * first, first + S, ... first + (sc-1)*S, and — when crc_out is non-NULL —
 * the CRC-32 of that span in the SAME pass over memory.
 *
 * acc must be S*npar uint16_t, zeroed. adsc_gf16_init() must have been called.
 * The caller is responsible for having checked that [first, first + sc*S) lies
 * inside its buffer; this function does no bounds checking, by design, because
 * it is the innermost loop of the whole operation.
 *
 * Fusing the CRC is exact rather than approximate: the rows are contiguous, so
 * the span the sweep reads and the span crc32_before covers are the same bytes.
 */
void adsc_ctdb_sweep(const uint16_t *pcm, uint64_t first, unsigned S,
                     unsigned sc, unsigned npar, uint16_t *acc,
                     uint32_t *crc_out);

/* Standalone CRC-32 over n 16-bit words (reflected, 0xEDB88320, init and final
 * xor 0xFFFFFFFF). Needed on its own for crc32_after, which is computed over a
 * buffer the sweep does not re-read. */
uint32_t adsc_ctdb_crc32_words(const uint16_t *v, uint64_t n);

#endif /* ACCUDISC_SRC_REPAIR_CTDB_INTERNAL_H */
