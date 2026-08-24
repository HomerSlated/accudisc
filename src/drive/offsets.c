/* Drive offset lookup — the single portal for offset data.
 *
 * The table is compiled by tools/gen_offsets.py from the two live primary
 * sources (REDUMP via redumper, and AccurateRip), both factual user-submitted
 * measurement data — attributed in docs/reference/ATTRIBUTION.md.
 * Nothing here reaches a network: the merge happens on the development cycle
 * and its output is committed.
 *
 * Matching: INQUIRY vendor/product with whitespace runs collapsed, since drives
 * pad the fixed INQUIRY fields ("DVDR   PX-716A" vs "DVDR PX-716A"), and
 * CASE-FOLDED — the table is generated upper-cased and the query is upper-cased
 * to meet it. Vendors are not consistent with themselves ("AOpen"/"AOPEN"), so
 * a case-sensitive compare answers only for the casing the firmware happened to
 * use and silently misses the other. What the lookup does NOT do is alias:
 * HL-DT-ST and LG ELECTRONICS are one company and two INQUIRY strings, and the
 * generator emits a row for each rather than the lookup asserting the identity.
 *
 * The `sources` bitmask reports PRESENCE in each table, not corroboration by
 * independent parties. The tables are not merely correlated: REDUMP's offset
 * table IS AccurateRip's list, imported once in 2022 and frozen. The evidence
 * is on accudisc_offset_info.sources in the public header.
 *
 * A value REDUMP holds that AccurateRip has since changed or removed is that
 * one source's withdrawn draft, and the generator drops it rather than
 * reporting a conflict — each deletion is named in offsets_db.inc.
 */

#include <ctype.h>
#include <limits.h>
#include <stdint.h>
#include <string.h>

#include "../internal.h"

struct offset_entry {
    const char *vendor;
    const char *product;
    int32_t  read_offset;
    uint16_t ar_submissions;
    uint8_t  ar_agree_pct;
    uint8_t  sources;        /* ACCUDISC_OFFSET_SRC_* */
    uint8_t  flags;          /* ACCUDISC_OFFSET_F_* */
};

/* The table is a parameter so a test can substitute a small one. The generated
 * corpus currently has ZERO conflicting keys, which makes the n > 1 branch of
 * accudisc_offset_for_inquiry unreachable from shipped data and therefore
 * impossible to exercise against the real table — live code with no guard.
 * tests/test_offsets_ambiguous.c compiles THIS FILE against a fixture instead
 * of a copy of it, so what it asserts is the shipped implementation.
 *
 * Default only. Nothing but that test ever defines it, and the library builds
 * exactly as before. */
#ifndef ADSC_OFFSETS_DB
#  define ADSC_OFFSETS_DB "offsets_db.inc"
#endif

static const struct offset_entry offsets[] = {
#include ADSC_OFFSETS_DB
};

#define OFFSETS_N (sizeof(offsets) / sizeof(offsets[0]))

/* The INQUIRY matching rule, one implementation: collapse whitespace runs to
 * single spaces, trim ends, and UPPER-CASE.
 *
 * Case folding is deliberate and is the whole rule, not a convenience. Vendors
 * are not consistent about it — this corpus carries "AOpen" and "AOPEN",
 * "Plextor" and "PLEXTOR", "hp" and "HP" for the same companies — so a
 * case-sensitive compare turns one drive into two keys and answers only for
 * whichever casing the firmware happened to use. The generator emits the table
 * already upper-cased; folding here as well makes that a property the lookup
 * enforces rather than one it trusts the generator to have preserved.
 *
 * Verified lossless before the change: of 5888 table rows, ZERO pairs differ
 * only by case, so folding collides nothing that was previously distinct.
 *
 * UNDERSCORES ARE NOT FOLDED HERE, and that is a decision rather than an
 * omission. The GENERATOR folds underscore to space when it builds its join key,
 * so "DVDRAM_GHA2N" and "DVDRAM GHA2N" pool their submissions into one drive's
 * evidence — but it then emits BOTH spellings as their own rows, and this
 * lookup matches each literally. The asymmetry is the same one that governs
 * VENDOR_ALIAS: a build-time key may assert that two strings name one drive; a
 * runtime lookup may only answer for the string the firmware actually reported.
 * Folding here as well would answer for spellings no source ever recorded.
 *
 * ASCII-only by intent. INQUIRY fields are single-byte and this must not depend
 * on locale — toupper() with a negative char is undefined, hence the cast. */
void adsc_inquiry_normalize(const char *src, char *dst, size_t cap)
{
    size_t o = 0;
    int in_space = 1; /* swallows leading spaces */

    for (; *src && o + 1 < cap; src++) {
        if (*src == ' ' || *src == '\t') {
            in_space = 1;
            continue;
        }
        if (in_space && o > 0)
            dst[o++] = ' ';
        in_space = 0;
        dst[o++] = (char)toupper((unsigned char)*src);
    }
    dst[o] = '\0';
}

/* THE KEY IS THE PRODUCT IDENTIFIER; THE VENDOR ONLY NARROWS.
 *
 * Keith's rule: "For actual drive detection, we will use only the unique
 * product identifier, and only verify the vendor if present and not
 * conflicting." A vendor MISMATCH is therefore not a rejection — that is the
 * entire point. Firmware reports the vendor field inconsistently (empty, the
 * host adapter's "SATA", the OEM rather than the badge, run into the product),
 * so requiring it to match answers only for the spelling one submitter happened
 * to send. The product identifier is the part that identifies the drive.
 *
 *     product match -> candidate set
 *     -> some candidate's vendor matches?  narrow to those
 *     -> otherwise                          keep them all
 *     -> one DISTINCT offset: OK  |  more: ERR_AMBIGUOUS with values[]
 *
 * COUNT DISTINCT OFFSETS, NOT MATCHING ROWS, and this is the part a rewrite
 * gets wrong. The table deliberately carries a row per SPELLING — "HL-DT-ST"
 * and "LG ELECTRONICS", "DVDRAM GHA2N" and "DVDRAM_GHA2N", the same drive with
 * and without a vendor — so a product-keyed scan matches several rows as a
 * matter of course. Measured on the shipped table: 1242 products are matched by
 * more than one row and 1229 of those rows AGREE. Counting rows would report
 * ERR_AMBIGUOUS for a quarter of the corpus, every one of them a drive whose
 * offset is not in doubt.
 *
 * Measured before adopting it, on this table: 4562 distinct products, 13 with
 * more than one offset, and the vendor narrows ALL 13 to one. Every
 * (vendor, product) pair already in the table returns exactly what it returned
 * under vendor+product keying — verified across all 5882 — so the change is
 * purely additive: what it buys is the drive whose vendor string is not the one
 * a submitter sent.
 *
 * WHAT IT WOULD COST, and what is done about it. A product string naming a
 * CATEGORY rather than a model would answer for any vendor. Mostly such strings
 * protect themselves by colliding — "CD-ROM" is held at four offsets and comes
 * back ambiguous, which is a refusal to guess rather than a wrong guess — but a
 * generic name only one submitter ever sent has nothing to collide with. Two
 * rules, deliberately different in strength:
 *
 *   EMPTY product     never answers. It is not a weak identifier but the
 *                     absence of one, so no vendor can rescue it.
 *   GENERIC product   answers only when the VENDOR narrows to it. Six of them,
 *                     reviewed by hand (GENERIC_PRODUCTS in gen_offsets.py) and
 *                     marked ACCUDISC_OFFSET_F_GENERIC by the generator.
 *
 * The generic rule blocks the product-only PATH, not the row, because the rows
 * are real measurements — "BUFFALO OPTICAL DRIVE" carries 85 submissions — and
 * dropping them to fix a matching rule would be ignoring data to fix code.
 */
int accudisc_offset_for_inquiry(const char *vendor, const char *product,
                                accudisc_offset_info *out)
{
    char want_v[32], want_p[32], have_v[32], have_p[32];
    const struct offset_entry *best = NULL;
    int32_t vals[ACCUDISC_OFFSET_MAX_VALUES];
    uint8_t vsrc[ACCUDISC_OFFSET_MAX_VALUES];
    unsigned nvals = 0;
    int matched = 0, vendor_hit = 0, overflow = 0, caller_has_values;
    size_t i, k;

    if (!vendor || !product || !out)
        return ACCUDISC_ERR_INVAL;
    if (out->size == 0 || out->size > sizeof(*out))
        return ACCUDISC_ERR_ABI;

    /* values[] and value_sources[] are the LAST fields, so a caller whose
     * struct does not reach them must not have them written. Today only one
     * layout has ever shipped and this is always true; it is here so that the
     * scan below cannot become the thing that overruns a short struct. */
    caller_has_values = (out->size >= sizeof(*out));

    /* Zero everything the caller's struct covers, then set the sentinels. A
     * caller that ignores the return code must not find a usable-looking number
     * here: ACCUDISC_OFFSET_NONE is INT32_MIN precisely because 0 is a
     * legitimate offset for hundreds of real drives and would apply cleanly. */
    memset((char *)out + sizeof(out->size), 0, out->size - sizeof(out->size));
    out->read_offset = ACCUDISC_OFFSET_NONE;

    adsc_inquiry_normalize(vendor, want_v, sizeof(want_v));
    adsc_inquiry_normalize(product, want_p, sizeof(want_p));

    /* An empty product identifies nothing. Under vendor+product keying it was
     * harmless — it matched one row and only when the vendor matched too. Keyed
     * on the product alone it would match EVERY query that reports no product,
     * whatever drive sent it, and hand back one submitter's offset with the
     * confidence of an exact match. */
    if (want_p[0] == '\0')
        return ACCUDISC_ERR_NOTFOUND;

    /* Pass 1: does the vendor narrow anything? Asked separately because the
     * answer decides which rows pass 2 is allowed to look at, and a single pass
     * would have to commit before knowing. */
    for (i = 0; i < OFFSETS_N && !vendor_hit; i++) {
        adsc_inquiry_normalize(offsets[i].product, have_p, sizeof(have_p));
        if (strcmp(want_p, have_p) != 0)
            continue;
        adsc_inquiry_normalize(offsets[i].vendor, have_v, sizeof(have_v));
        if (strcmp(want_v, have_v) == 0)
            vendor_hit = 1;
    }

    for (i = 0; i < OFFSETS_N; i++) {
        adsc_inquiry_normalize(offsets[i].product, have_p, sizeof(have_p));
        if (strcmp(want_p, have_p) != 0)
            continue;
        if (vendor_hit) {
            adsc_inquiry_normalize(offsets[i].vendor, have_v, sizeof(have_v));
            if (strcmp(want_v, have_v) != 0)
                continue;
        } else if (offsets[i].flags & ACCUDISC_OFFSET_F_GENERIC) {
            /* A product naming a CATEGORY cannot answer on its own. Reached
             * only here — where the vendor narrowed nothing — so the row still
             * answers for the vendor it was submitted under, which is the
             * difference between declining to guess and discarding a
             * measurement: BUFFALO's "OPTICAL DRIVE" rests on 85 submissions.
             * See GENERIC_PRODUCTS in tools/gen_offsets.py for the six, and why
             * the generic names that COLLIDE are deliberately not among them. */
            continue;
        }

        matched = 1;
        out->sources |= offsets[i].sources;
        out->flags |= offsets[i].flags;

        /* AccurateRip's figures describe ONE of its entries, and several may
         * back the same offset under different vendor spellings ("" with 3
         * submissions beside "SHARK" with 1 — 232 products look like this).
         * Summing would triple-count the spelling variants of a single entry,
         * which the generator gives identical figures; taking whichever row the
         * scan reached first is arbitrary. The best-evidenced row is a real
         * measurement that never overstates, and its percentage travels with
         * its own count rather than being crossed with another row's. */
        if (!best || offsets[i].ar_submissions > best->ar_submissions)
            best = &offsets[i];

        for (k = 0; k < nvals; k++)
            if (vals[k] == offsets[i].read_offset)
                break;
        if (k < nvals)
            vsrc[k] |= offsets[i].sources;
        else if (nvals < ACCUDISC_OFFSET_MAX_VALUES) {
            vals[nvals] = offsets[i].read_offset;
            vsrc[nvals] = offsets[i].sources;
            nvals++;
        } else {
            /* More distinct offsets than values[] can carry. n_values alone
             * cannot say so — it would read as "four, and that was all" — and
             * silently narrowing what we report is the one thing this codebase
             * refuses to do. The shipped table's worst product holds exactly
             * four, so this is unreachable today and is here for the corpus
             * refresh that makes it five. */
            overflow = 1;
        }
    }

    if (!matched)
        return ACCUDISC_ERR_NOTFOUND;

    if (caller_has_values)
        for (k = 0; k < nvals; k++) {
            out->values[k] = vals[k];
            out->value_sources[k] = vsrc[k];
        }
    out->n_values = (uint8_t)nvals;
    if (overflow)
        out->flags |= ACCUDISC_OFFSET_F_TRUNCATED;

    if (nvals > 1) {
        /* The candidates disagree. read_offset stays ACCUDISC_OFFSET_NONE; the
         * caller prints values[], picks one, and passes it back through its own
         * configuration. AccuDisc does not pick, and never applies.
         *
         * The AccurateRip figures are CLEARED rather than taken from the
         * best-evidenced row: they describe the value AccurateRip holds, and on
         * a contested key there is no single such value for them to describe. */
        out->ar_submissions = 0;
        out->ar_agree_pct = 0;
        return ACCUDISC_ERR_AMBIGUOUS;
    }

    out->ar_submissions = best->ar_submissions;
    out->ar_agree_pct = best->ar_agree_pct;
    out->read_offset = vals[0];
    return ACCUDISC_OK;
}

int accudisc_offset_for_device(accudisc_device *dev, accudisc_offset_info *out)
{
    int rc;

    if (!dev || !out)
        return ACCUDISC_ERR_INVAL;
    rc = adsc_dev_identify(dev);
    if (rc != ACCUDISC_OK)
        return rc;
    return accudisc_offset_for_inquiry(dev->id.vendor, dev->id.product, out);
}

int accudisc_read_offset(accudisc_device *dev, int32_t *samples)
{
    accudisc_offset_info info = ACCUDISC_OFFSET_INFO_INIT;
    int rc;

    if (!dev || !samples)
        return ACCUDISC_ERR_INVAL;
    /* Pre-0.10 contract, kept verbatim for callers compiled against it: a
     * single number or nothing. A key whose sources disagree has no single
     * number, so it reports as ERR_AMBIGUOUS rather than silently handing back
     * whichever row the table happened to list first — which is exactly what
     * this function used to do. */
    rc = accudisc_offset_for_device(dev, &info);
    if (rc != ACCUDISC_OK)
        return rc;
    *samples = info.read_offset;
    return ACCUDISC_OK;
}
