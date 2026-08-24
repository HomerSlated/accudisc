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

    /* And the caller is TOLD the list is short. n_values alone reads as "four,
     * and that was all"; F_TRUNCATED is the difference between that and "four
     * of the five we found". */
    assert(info.flags
           == (ACCUDISC_OFFSET_F_ADJUDICATED | ACCUDISC_OFFSET_F_TRUNCATED));
    assert(info.flags & ACCUDISC_OFFSET_F_TRUNCATED);

    /* --- the key is the PRODUCT; the vendor only narrows -------------------
     * Three rows, three vendor spellings, one offset. Under row-counting this
     * is "ambiguous, 3 values"; it is one value held three ways, and the
     * distinction is the whole of the product-only change. */
    info = (accudisc_offset_info)ACCUDISC_OFFSET_INFO_INIT;
    assert(accudisc_offset_for_inquiry("ZETA", "TWIN", &info) == ACCUDISC_OK);
    assert(info.read_offset == 70);
    assert(info.n_values == 1);
    assert(info.values[0] == 70);
    /* ZETA is in no row: the vendor narrows nothing, so all three contribute.
     * value_sources[0] is the OR across them, not the first row's. */
    assert(info.value_sources[0] == 3);
    assert(info.sources == 3);
    /* The best-evidenced row's figures, travelling as a PAIR. 3 would be the
     * first row scanned and 1 the last; 9/90 can only come from choosing the
     * row with the most submissions, and 9/100 could only come from crossing
     * one row's count with another's percentage. */
    assert(info.ar_submissions == 9);
    assert(info.ar_agree_pct == 90);

    /* Name a vendor that IS in the set and the answer narrows to that row
     * alone — different figures from the query above, on the same offset. */
    info = (accudisc_offset_info)ACCUDISC_OFFSET_INFO_INIT;
    assert(accudisc_offset_for_inquiry("ALPHA", "TWIN", &info) == ACCUDISC_OK);
    assert(info.read_offset == 70);
    assert(info.ar_submissions == 3);
    assert(info.value_sources[0] == 1);

    /* --- the vendor decides, or admits it cannot --------------------------- */
    info = (accudisc_offset_info)ACCUDISC_OFFSET_INFO_INIT;
    assert(accudisc_offset_for_inquiry("GAMMA", "SPLIT", &info) == ACCUDISC_OK);
    assert(info.read_offset == 80);
    assert(info.ar_submissions == 5);

    info = (accudisc_offset_info)ACCUDISC_OFFSET_INFO_INIT;
    assert(accudisc_offset_for_inquiry("DELTA", "SPLIT", &info) == ACCUDISC_OK);
    assert(info.read_offset == 90);

    /* A vendor in no row narrows nothing, and the caller gets both values
     * rather than whichever row came first — the failure mode this replaces. */
    info = (accudisc_offset_info)ACCUDISC_OFFSET_INFO_INIT;
    assert(accudisc_offset_for_inquiry("OMEGA", "SPLIT", &info)
           == ACCUDISC_ERR_AMBIGUOUS);
    assert(info.read_offset == ACCUDISC_OFFSET_NONE);
    assert(info.n_values == 2);
    assert(info.values[0] == 80 && info.values[1] == 90);
    assert(info.value_sources[0] == 1 && info.value_sources[1] == 2);
    assert(info.sources == 3);

    /* A vendor nobody reports still finds the drive. THIS IS THE POINT of the
     * change: firmware reports the vendor field inconsistently, and requiring it
     * to match answers only for the spelling one submitter happened to send. */
    info = (accudisc_offset_info)ACCUDISC_OFFSET_INFO_INIT;
    assert(accudisc_offset_for_inquiry("NO SUCH VENDOR", "SOLO", &info)
           == ACCUDISC_OK);
    assert(info.read_offset == 100);

    /* --- an empty product identifies nothing ------------------------------
     * Refused even for the exact vendor that submitted it. Keyed on the product
     * alone this row would otherwise answer for every drive reporting no
     * product string, with the confidence of an exact match. */
    info = (accudisc_offset_info)ACCUDISC_OFFSET_INFO_INIT;
    assert(accudisc_offset_for_inquiry("EMPTYP", "", &info)
           == ACCUDISC_ERR_NOTFOUND);
    assert(info.read_offset == ACCUDISC_OFFSET_NONE);
    info = (accudisc_offset_info)ACCUDISC_OFFSET_INFO_INIT;
    assert(accudisc_offset_for_inquiry("ANYONE", "   ", &info)
           == ACCUDISC_ERR_NOTFOUND);

    /* --- a generic product answers only when the vendor narrows -----------
     * The weaker of the two rules, and the difference matters: an empty product
     * can never answer, while this row is a real measurement that still answers
     * for the vendor that submitted it. Blocking the ROW would discard data to
     * fix a matching rule. */
    info = (accudisc_offset_info)ACCUDISC_OFFSET_INFO_INIT;
    assert(accudisc_offset_for_inquiry("KAPPA", "GENERICPROD", &info)
           == ACCUDISC_OK);
    assert(info.read_offset == 55);
    assert(info.ar_submissions == 4);
    /* Reported, so the caller knows the VENDOR earned this answer and the
     * product alone would not have. */
    assert(info.flags & ACCUDISC_OFFSET_F_GENERIC);

    /* Any other vendor, and the product cannot speak for itself. NOT ambiguous
     * — there is nothing to choose between — and not a plausible number. */
    info = (accudisc_offset_info)ACCUDISC_OFFSET_INFO_INIT;
    assert(accudisc_offset_for_inquiry("LAMBDA", "GENERICPROD", &info)
           == ACCUDISC_ERR_NOTFOUND);
    assert(info.read_offset == ACCUDISC_OFFSET_NONE);
    info = (accudisc_offset_info)ACCUDISC_OFFSET_INFO_INIT;
    assert(accudisc_offset_for_inquiry("", "GENERICPROD", &info)
           == ACCUDISC_ERR_NOTFOUND);

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
     * It is a trap set for the next person, and the trap is WORSE than the note
     * this replaces claimed. That note said to derive the values[] write bound
     * from out->size before growing the macro. THAT WOULD NOT BE ENOUGH:
     * value_sources[] follows values[], so growing the array MOVES it. Measured
     * on this platform — at 4, sizeof is 36 with value_sources at offset 32; at
     * 8, sizeof is 56 with value_sources at 48. An old caller's struct is
     * therefore NOT a prefix of the new one, and `size` cannot describe it: the
     * caller would read its value_sources from bytes the library used for
     * values. Bounding the write does not fix a field that has moved.
     *
     * So ACCUDISC_OFFSET_MAX_VALUES is frozen by the ABI. Reporting more values
     * needs a field APPENDED after value_sources[] (which `size` does handle),
     * or a new call — not a bigger array. ACCUDISC_OFFSET_F_TRUNCATED exists so
     * that until someone does one of those, a caller is at least TOLD the list
     * was short rather than silently handed a prefix.
     *
     * A comment cannot fail. This can, at exactly the moment it matters. */
    assert(ACCUDISC_OFFSET_MAX_VALUES == 4);

    printf("test_offsets_ambiguous: ok\n");
    return 0;
}
