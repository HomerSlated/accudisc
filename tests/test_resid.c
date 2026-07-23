/* Regression test for F-001: SG_IO short-transfer detection.
 *
 * SG_IO can complete with GOOD status yet transfer fewer bytes than requested
 * (partial DMA / drive under-run, seen on marginal USB bridges and near
 * end-of-disc). Before the fix, adsc_mmc_read_cd trusted such a read whole,
 * streaming the stale buffer tail to --pcm as valid audio and bypassing the
 * C2/consensus machinery. A fixed-length READ CD must instead be promoted to
 * ACCUDISC_ERR_SHORT.
 *
 * The ioctl plumbing cannot be exercised without a real drive, so the two pure
 * decision helpers that carry the fix are tested directly here; the transport
 * (sgio.c) and adsc_mmc_read_cd apply them verbatim, so a regression in the
 * rule breaks these assertions. */

#include <assert.h>

#include "transport/transport.h"

static void test_resid_clamp(void)
{
    /* Full transfer: no residual. */
    assert(adsc_resid_clamp(0, 4096) == 0);
    /* Partial transfer: the untransferred tail is reported verbatim. */
    assert(adsc_resid_clamp(2646, 4096) == 2646);
    /* Whole command undelivered (0 bytes moved): residual == buf_len. */
    assert(adsc_resid_clamp(4096, 4096) == 4096);
    /* Negative residual (nonsensical over-transfer) clamps to 0 — the exact
     * trap in the naive `io.resid > 0 ? io.resid : 0` sketch, made explicit. */
    assert(adsc_resid_clamp(-1, 4096) == 0);
    assert(adsc_resid_clamp(-100000, 4096) == 0);
    /* Residual larger than the buffer clamps to buf_len. */
    assert(adsc_resid_clamp(99999, 4096) == 4096);
    /* Zero-length command: nothing to short. */
    assert(adsc_resid_clamp(0, 0) == 0);
    assert(adsc_resid_clamp(5, 0) == 0);
}

static void test_exec_check_short(void)
{
    /* The bug itself: GOOD status with a residual must NOT stay OK. */
    assert(adsc_exec_check_short(ACCUDISC_OK, 2646) == ACCUDISC_ERR_SHORT);
    assert(adsc_exec_check_short(ACCUDISC_OK, 1) == ACCUDISC_ERR_SHORT);
    /* A clean, full transfer stays OK. */
    assert(adsc_exec_check_short(ACCUDISC_OK, 0) == ACCUDISC_OK);
    /* Non-OK results pass through unchanged, residual notwithstanding: the
     * drive's own diagnosis is never masked by the short-transfer promotion. */
    assert(adsc_exec_check_short(ACCUDISC_ERR_SENSE, 0) == ACCUDISC_ERR_SENSE);
    assert(adsc_exec_check_short(ACCUDISC_ERR_SENSE, 2646) == ACCUDISC_ERR_SENSE);
    assert(adsc_exec_check_short(ACCUDISC_ERR_IO, 0) == ACCUDISC_ERR_IO);
    assert(adsc_exec_check_short(ACCUDISC_ERR_IO, 4096) == ACCUDISC_ERR_IO);
}

int main(void)
{
    test_resid_clamp();
    test_exec_check_short();
    return 0;
}
