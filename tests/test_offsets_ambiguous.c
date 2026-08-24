/* The ERR_AMBIGUOUS branch of accudisc_offset_for_inquiry, against a fixture.
 *
 * WHY THIS FILE EXISTS. The shipped table has zero conflicting keys — the
 * retraction rule removed the last of them at 0.12.0 — so every drive in the
 * real corpus takes the n == 1 path. The multi-value branch (values[],
 * value_sources[], n_values, the AccurateRip figures being cleared) is live
 * public API with no way to reach it from shipped data, and it becomes
 * load-bearing again the moment the table is keyed on product alone.
 *
 * It COMPILES src/drive/offsets.c, it does not reimplement it. A second copy of
 * the matcher would assert that the copy works. The table is the only thing
 * substituted, through ADSC_OFFSETS_DB.
 */
#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "accudisc/accudisc.h"
#include "internal.h"

/* offsets.c reaches the device path through this. The test drives the INQUIRY
 * entry point directly for most assertions, but accudisc_offset_for_device and
 * accudisc_read_offset are public API whose documented ERR_AMBIGUOUS arms are
 * otherwise untested, so the stub reports a drive that IS ambiguous here. */
int adsc_dev_identify(struct accudisc_device *dev)
{
    snprintf(dev->id.vendor, sizeof(dev->id.vendor), "%s", "FIXTURE");
    snprintf(dev->id.product, sizeof(dev->id.product), "%s", "PAIR");
    snprintf(dev->id.revision, sizeof(dev->id.revision), "%s", "1.00");
    dev->id_valid = 1;
    return ACCUDISC_OK;
}

int main(void)
{
    accudisc_offset_info info;
    struct accudisc_device dev;
    int32_t samples;

    /* --- the fixture IS the table ----------------------------------------
     * First, because everything below is meaningless otherwise. A CMake wiring
     * mistake that compiled this against src/drive/offsets_db.inc would leave
     * the single-value control passing and only the ambiguous keys failing,
     * which reads as "the branch is broken" rather than "the test is". */
    info = (accudisc_offset_info)ACCUDISC_OFFSET_INFO_INIT;
    assert(accudisc_offset_for_inquiry("PLEXTOR", "DVDR PX-716A", &info)
           == ACCUDISC_ERR_NOTFOUND);

    /* --- the unambiguous path still works here ---------------------------- */
    info = (accudisc_offset_info)ACCUDISC_OFFSET_INFO_INIT;
    assert(accudisc_offset_for_inquiry("FIXTURE", "SOLO", &info) == ACCUDISC_OK);
    assert(info.read_offset == 100);
    assert(info.n_values == 1);
    assert(info.values[0] == 100);
    assert(info.ar_submissions == 55);
    assert(info.ar_agree_pct == 88);

    /* --- two values under one key ----------------------------------------- */
    info = (accudisc_offset_info)ACCUDISC_OFFSET_INFO_INIT;
    assert(accudisc_offset_for_inquiry("FIXTURE", "PAIR", &info)
           == ACCUDISC_ERR_AMBIGUOUS);

    /* read_offset must stay the sentinel. A caller that ignores the return code
     * has to find something unusable, not a plausible number: +10 is a
     * perfectly applicable correction and would be applied silently. */
    assert(info.read_offset == ACCUDISC_OFFSET_NONE);

    /* Both values, in table order, each with the source that holds it. */
    assert(info.n_values == 2);
    assert(info.values[0] == 10 && info.value_sources[0] == 1);
    assert(info.values[1] == 20 && info.value_sources[1] == 2);

    /* Accumulated across ALL matching rows, not taken from the first. The
     * fixture gives the rows DIFFERENT bits precisely so that "took the first"
     * (1) and "OR'd them" (3) are distinguishable. */
    assert(info.sources == 3);
    assert(info.flags == (ACCUDISC_OFFSET_F_CONFLICT | ACCUDISC_OFFSET_F_ADJUDICATED));

    /* CLEARED, not inherited. The first row carries 1234/99, so this can only
     * hold if the n > 1 branch actively zeroed them — which is the point:
     * AccurateRip's confidence describes the value AccurateRip holds, and on a
     * contested key there is no single such value to describe. */
    assert(info.ar_submissions == 0);
    assert(info.ar_agree_pct == 0);

    /* --- normalisation applies inside the multi-value path ----------------
     * The two rows are "fixture"/"SPACED  KEY" and "FIXTURE"/"SPACED KEY".
     * They are one key only if case folding and whitespace collapse run on the
     * TABLE as well as the query, so this is the ambiguity itself depending on
     * normalisation rather than a separate check of it. The query uses a third
     * spelling again, padded the way a real drive pads INQUIRY. */
    info = (accudisc_offset_info)ACCUDISC_OFFSET_INFO_INIT;
    assert(accudisc_offset_for_inquiry("  FiXtUrE ", "spaced   key ", &info)
           == ACCUDISC_ERR_AMBIGUOUS);
    assert(info.n_values == 2);
    assert(info.values[0] == 30 && info.values[1] == 40);
    assert(info.sources == 3);

    /* --- more values than values[] can hold -------------------------------
     * Five rows, ACCUDISC_OFFSET_MAX_VALUES is 4. n_values must CLAMP rather
     * than report the true count, since a caller that trusts n_values would
     * read values[4] out of bounds. */
    info = (accudisc_offset_info)ACCUDISC_OFFSET_INFO_INIT;
    assert(accudisc_offset_for_inquiry("FIXTURE", "FIVE", &info)
           == ACCUDISC_ERR_AMBIGUOUS);
    assert(info.n_values == ACCUDISC_OFFSET_MAX_VALUES);
    assert(info.values[0] == 1 && info.values[1] == 2);
    assert(info.values[2] == 3 && info.values[3] == 4);

    /* The dropped value appears NOWHERE. Asserting only n_values would pass if
     * the fifth row had overwritten values[3]. */
    for (unsigned i = 0; i < info.n_values; i++)
        assert(info.values[i] != 5);

    /* The fifth row is still ACCOUNTED FOR even though its value was dropped —
     * the scan keeps OR-ing sources and flags after the array fills. Bits 2 and
     * 4 exist only on that row, so this is the property under test and not a
     * restatement of the rows above. A caller must be able to tell "four values,
     * that is all there was" from "four values and something was discarded". */
    assert(info.sources == 3);
    assert(info.flags == ACCUDISC_OFFSET_F_ADJUDICATED);

    /* --- the device-keyed entry points report it too ----------------------- */
    memset(&dev, 0, sizeof(dev));
    info = (accudisc_offset_info)ACCUDISC_OFFSET_INFO_INIT;
    assert(accudisc_offset_for_device(&dev, &info) == ACCUDISC_ERR_AMBIGUOUS);
    assert(info.read_offset == ACCUDISC_OFFSET_NONE);
    assert(info.n_values == 2);

    /* The pre-0.10 contract, which is the one most callers are still on. It
     * used to hand back whichever row the table listed first; it must now
     * refuse, and must not touch the caller's variable while refusing. */
    memset(&dev, 0, sizeof(dev));
    samples = 424242;
    assert(accudisc_read_offset(&dev, &samples) == ACCUDISC_ERR_AMBIGUOUS);
    assert(samples == 424242);

    /* --- the ABI tripwire --------------------------------------------------
     * NOT a live defect: ACCUDISC_OFFSET_MAX_VALUES has been 4 since the struct
     * was introduced whole in 0.10.0, so no conforming caller has a smaller
     * values[] and none can be overrun today.
     *
     * It is a trap set for the next person. values[] and value_sources[] are the
     * LAST fields of accudisc_offset_info, and the write bound above is the
     * COMPILE-TIME macro while the ABI contract is the RUNTIME out->size.
     * Growing this macro would therefore write past the end of a caller
     * compiled against the old header — silently, since the memset that
     * precedes it is correctly bounded by size and would leave the struct
     * looking well-formed.
     *
     * A comment cannot fail. This can, at exactly the moment it matters:
     * derive the values[] write bound from out->size before changing this. */
    assert(ACCUDISC_OFFSET_MAX_VALUES == 4);

    printf("test_offsets_ambiguous: ok\n");
    return 0;
}
