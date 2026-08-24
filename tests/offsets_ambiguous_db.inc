/* Synthetic offset table for tests/test_offsets_ambiguous.c — NOT generated,
 * NOT shipped, and deliberately NOT named offsets_db.inc: the quoted #include
 * form searches the including file's own directory first, so a fixture wearing
 * the real table's name would be silently overridden by src/drive/offsets_db.inc
 * and the test would pass against the wrong data.
 *
 * Columns match src/drive/offsets_db.inc exactly:
 *   vendor, product, read_offset, ar_submissions, ar_agree_pct, sources, flags
 *
 * EVERY ROW IS BUILT TO DISCRIMINATE. An assertion that a field is 0 proves
 * nothing if the field was 0 before the code ran, and an OR across rows is
 * indistinguishable from "took the first" unless the rows carry different bits.
 * So: the first matching row of each ambiguous key carries NONZERO AccurateRip
 * figures, and no two rows of a key share a sources/flags value.
 */

    /* The single-value control: the OK path must still work here, or a failure
     * below cannot be attributed to the multi-value branch. */
    { "FIXTURE", "SOLO", +100, 55, 88, 3, 0 },

    /* Two values, one key. sources 1 vs 2 so the OR must produce 3; flags 1 vs
     * 4 likewise; 1234/99 on the FIRST row so "cleared to 0" can fail. */
    { "FIXTURE", "PAIR", +10, 1234, 99, 1, 1 },
    { "FIXTURE", "PAIR", +20,    7, 50, 2, 4 },

    /* The same key spelled two ways. Normalisation (case fold, whitespace
     * collapse) has to happen on BOTH the query and the table for these to
     * collide, and the collision is what makes the key ambiguous at all. */
    { "fixture", "SPACED  KEY", +30, 3, 100, 1, 0 },
    { "FIXTURE", "SPACED KEY",  +40, 4, 100, 2, 0 },

    /* FIVE values against ACCUDISC_OFFSET_MAX_VALUES == 4. The fifth row is the
     * one that matters: it must NOT reach values[], and its sources/flags bits
     * must still reach the caller, because the loop keeps accumulating after the
     * value array is full. Bits 2 and 4 appear ONLY on that row. */
    { "FIXTURE", "FIVE", +1, 11, 100, 1, 0 },
    { "FIXTURE", "FIVE", +2, 12, 100, 1, 0 },
    { "FIXTURE", "FIVE", +3, 13, 100, 1, 0 },
    { "FIXTURE", "FIVE", +4, 14, 100, 1, 0 },
    { "FIXTURE", "FIVE", +5, 15, 100, 2, 4 },
